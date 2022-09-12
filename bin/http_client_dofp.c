/* Copyright (c) 2017 - 2021 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * http_client.c -- A simple HTTP/QUIC client
 */

#ifndef WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#else
#include <Windows.h>
#include <WinSock2.h>
#include <io.h>
#include <stdlib.h>
#include <getopt.h>
#define STDOUT_FILENO 1
#define random rand
#pragma warning(disable:4996) //POSIX name deprecated
#endif
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <event2/event.h>
#include <math.h>
#include <float.h>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "lsquic.h"
#include "test_common.h"
#include "prog.h"

#include "abr_max_j.h"
#include "abr_max_min_j.h"
#include "abr_max_min_j_norm.h"
#include "abr_quality_instability.h"

#include "../src/liblsquic/lsquic_logger.h"
#include "../src/liblsquic/lsquic_int_types.h"
#include "../src/liblsquic/lsquic_util.h"
/* include directly for reset_stream testing */
#include "../src/liblsquic/lsquic_varint.h"
#include "../src/liblsquic/lsquic_hq.h"
#include "../src/liblsquic/lsquic_sfcw.h"
#include "../src/liblsquic/lsquic_hash.h"
#include "../src/liblsquic/lsquic_stream.h"
/* include directly for retire_cid testing */
#include "../src/liblsquic/lsquic_conn.h"
#include "lsxpack_header.h"

#define N_REP 11 /* Number of available media representations (quality levels) Apple: 11, Ghent: 6 */
#define K_MAX 10 /* Quality  values for average quality computation */
#define N_MAX_SEG 184 /* Max number of segments to be downloaded */
#define AVG_COUNT 5 /* Moving average count */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* This is used to exercise generating and sending of priority frames */
static int randomly_reprioritize_streams;

static int s_display_cert_chain;

/* If this file descriptor is open, the client will accept server push and
 * dump the contents here.  See -u flag.
 */
static int promise_fd = -1;

/* Set to true value to use header bypass.  This means that the use code
 * creates header set via callbacks and then fetches it by calling
 * lsquic_stream_get_hset() when the first "on_read" event is called.
 */
static int g_header_bypass;

static int s_discard_response;

/* If set to a non-zero value, abandon reading from stream early: read at
 * most `s_abandon_early' bytes and then close the stream.
 */
static long s_abandon_early;

struct sample_stats
{
    unsigned        n;
    unsigned long   min, max;
    unsigned long   sum;        /* To calculate mean */
    unsigned long   sum_X2;     /* To calculate stddev */
};

static struct sample_stats  s_stat_to_conn,     /* Time to connect */
                            s_stat_ttfb,
                            s_stat_req;     /* From TTFB to EOS */
static unsigned s_stat_conns_ok, s_stat_conns_failed;
static unsigned long s_stat_downloaded_bytes;

struct throughput_stats
{
    long double         throughput;
    long double         tot_throughput;
    long double         s_throughput;
    long double         e_temp_throughput;
    long double         e_throughput[N_MAX_SEG];
    long double         comp_throughput[N_MAX_SEG];
    double              b_level[N_MAX_SEG];
};

struct h2br_stats
{
    long double         T_e;
};

struct wlb_stats
{
    unsigned            re_count;
    unsigned            re_unused_count;
    long double         re_data; // [kB]
    long double         re_unused_data; // [kB]
};

// Minh - Add BOLA ABR - ADD - S
struct bola_m_stats
{
    double        Sm [N_REP];    // segment size
    double        Vm [N_REP];    // utilities
    double        value [N_REP]; // the values of optimization problem
    double        max_value;     // the value of equation (9) in the paper
    double        V;             // the control parameter for overflow case
    double        gma;           // control parameter for rebuffering case
    double        SM;            // minimum segment size
    unsigned      m_star;        // selected quality
};
// Minh - Add BOLA ABR - ADD - E

// Minh - Add BBA ABR - ADD - S
/* BBA selects the bitrate based on a function f = a*buffer_level + b
* 
*/

struct bba_stats
{
    double        rS;
    double        cuS;
    double        a;
    double        b;
    unsigned      selected_bitrate;
};
// Minh - Add BBA ABR - ADD - E

struct sara_stats
{
    double        I;
    double        B_alpha;
    double        B_beta;
    double        B_max;
    long double   W [N_REP][N_MAX_SEG];    // KB
    long double   weights [N_MAX_SEG];     // In KB
    long double   down_rate [N_MAX_SEG];   // In KB/s
    double        H;                       // Weighted Harmonic mean of first n segments
    double        delta;
};

static struct           throughput_stats t_stats;
static struct           h2br_stats h_stats;
// Minh - Add BOLA ABR - ADD - S
static struct           bola_m_stats b_m_stats;
// Minh - Add BOLA ABR - ADD - E
static struct           bba_stats b_b_stats;
static struct           sara_stats s_stats;
static struct           wlb_stats w_stats;
static double const     buffer_size = 20.0; // Maximum buffer size
static double           buffer_level = 0.0; /* Updated buffer level of the downloaded segment */
static unsigned         seg_length = 4U; /* Lenght of the segments to be downloaded */
static unsigned         playout = 0U; /* States whether the playout is running [1] or is paused (stall or buffering) [0] */
static unsigned         min_init_bs = 4U; /* Minimum initial[/stall] buffer size (sec) to [re]start playout */
static lsquic_time_t    start_t; /* Start time */
static lsquic_time_t    playout_t; /* Start playout time */
static double           stalls_t[N_MAX_SEG] = {0.0}; /* Stalls media time */
static double           stalls_d[N_MAX_SEG] = {0.0}; /* Stalls duration */
static unsigned         stall_ind = 0U; // First stall is the initial buffering
static lsquic_time_t    stall_t; /* Start stall time */
static const char       FP_PATH[] = "apple/"; // "ghent", "apple"
static const char       SP_PATH[] = "/segment_";
static const char       EXT[] = ".m4s";
static unsigned         seg_ind = 1U;

/* APPle*/
static char             *seg_paths[N_REP] = {"apple/145/segment_1.m4s", 
                                             "apple/300/segment_1.m4s", 
                                             "apple/600/segment_1.m4s", 
                                             "apple/900/segment_1.m4s", 
                                             "apple/1600/segment_1.m4s", 
                                             "apple/2400/segment_1.m4s", 
                                             "apple/3400/segment_1.m4s", 
                                             "apple/4500/segment_1.m4s", 
                                             "apple/5800/segment_1.m4s", 
                                             "apple/8100/segment_1.m4s", 
                                             "apple/11600/segment_1.m4s"}; /* To be actually parsed from an MPD file but done without loss of generality (Not considering the initialization segment) */
static const int        seg_bitrates[N_REP] = {145, 300, 600, 900, 1600, 2400, 3400, 4500, 5800, 8100, 11600}; /* [kbps] To be actually parsed from an MPD file but done without loss of generality */
static char             *seg_res[N_REP] = {"640x360", "768x432", "960x540", "960x540", "960x540", "1280x720", "1280x720", "1920x1080", "1920x1080", "2560x1440", "3840x2160"};

/* Ghent */
// static char             *seg_paths[N_REP] = {"ghent/150/segment_1.m4s", 
//                                              "ghent/150/segment_1.m4s",
//                                              "ghent/150/segment_1.m4s",
//                                              "ghent/150/segment_1.m4s",
//                                              "ghent/150/segment_1.m4s",
//                                              "ghent/150/segment_1.m4s"}; /* To be actually parsed from an MPD file but done without loss of generality (Not considering the initialization segment) */
// static const int        seg_bitrates[N_REP] = {150, 500, 1150, 2600, 5450, 10700}; /* [kbps] To be actually parsed from an MPD file but done without loss of generality */
// static char             *seg_res[N_REP] = {"640x360", "854x480", "1280x720", "1920x1080", "2560x1440", "3840x2160"};


static int              seg_chosen_q[N_MAX_SEG]; /* Chosen representation quality for each downloaded segment. If -1, the segment has not been yet downloaded */
// static int              re_seg_chosen_q[N_MAX_SEG]; /* Chosen representation quality for each re-transmitted segment. If -1, the segment has not been re-transmitted */
static int              qualities_ind = -1; /* Index for values substitution */
static const double     alpha = .5, beta = .5;
static unsigned         rep_seg_ind = 1U;
static double           rep_seg_time = 4.0; /* Left time for the segment to be fully reproduced (4s -> ... -> 0s) */
static const char       WEIGHTS_FILENAME[] = "bin/weights_apple_tos.txt";
static char             METRICS_FILENAME[] = "DoFP_extensions/apple_tos/metrics_abr_00.csv";
static char             METRICS_OUT_FILENAME[] = "DoFP_extensions/apple_tos/metrics_abr_00_out.csv";
static char             JSON_FILENAME[] = "DoFP_extensions/apple_tos/itu-p1203_abr_00.json";
static char             JSON_OUT_FILENAME[] = "DoFP_extensions/apple_tos/itu-p1203_abr_00_out.txt";
static const float      FPS = 24.0;
static const char       DEVICE[] = "pc";
static const char       DISPLAYSIZE[] = "3840x2160";
static const unsigned   VIEWINGDISTANCE = 150U;
static bool             isMultiStream = false; // Set to true automatically if multiplexing feature is enabled (-w parameter > 1)
static bool             request_cancellation = true; // True if request cancellation, or stream termination or stream cancellation, is enabled

/* Update the buffer size when required */
static void update_buff(bool sr){
    if(playout){ /* If player is not paused */
        double elapsed_time = (double) (lsquic_time_now() - playout_t) / 1000000;
        playout_t = lsquic_time_now();
        if (elapsed_time > buffer_level) {
            rep_seg_ind = seg_ind - 1;
            rep_seg_time = 0.0;
            playout = 0;
            stalls_t[stall_ind] = (rep_seg_ind + 1) * seg_length;
            stall_t = lsquic_time_now() - ((lsquic_time_t) elapsed_time - buffer_level) * 1000000;
            buffer_level = 0.0;
        } else {
            buffer_level -= elapsed_time;
            if (elapsed_time > rep_seg_time) {
                // printf("Elapsed time: %.3f \n", elapsed_time);
                // printf("Check rep_seg_ind update: += %.1f\n", floor(1 + (elapsed_time - rep_seg_time) / seg_length));
                rep_seg_ind += floor(1 + (elapsed_time - rep_seg_time) / seg_length);
                rep_seg_time = seg_length - (elapsed_time - rep_seg_time - floor((elapsed_time - rep_seg_time) / seg_length) * seg_length);
            } else
                rep_seg_time -= elapsed_time;
        }
    }
    if (sr){ /* Segment received */
        buffer_level += seg_length;
        if (!playout){ /* If player is paused */
            if (buffer_level > min_init_bs) {
                playout = 1;
                playout_t = lsquic_time_now();
                stalls_d[stall_ind] = (double) (playout_t - stall_t) / 1000000;
                printf("Stall n. %u: Media start stall time -> %.3f, Time to stall -> %.3f s\n", stall_ind + 1, stalls_t[stall_ind], stalls_d[stall_ind]);
                ++stall_ind;
                if(rep_seg_time <= 0){
                    if (rep_seg_ind < seg_ind){
                        ++rep_seg_ind;
                        rep_seg_time = seg_length;
                    }
                }
                return;
            }
        }
    }
}

static int
getWords(char *base, char target[N_MAX_SEG][40])
{
    int n=0,i,j=0;
    
    while(base[i]!='\0')
    {
        if(base[i]!=' '){
            target[n][j++]=base[i];
        }
        else{
            target[n][j++]='\0';//insert NULL
            n++;
            j=0;
        }
        i++;
    }
    return n;
    
}

/*
function : getBitrateFromPath
argument list:
1) source - path source string
Return type - int : bitrate on success, 0 on failure
*/

static int
getBitrateFromPath(const char* source)
{
    int length, i, j, bitrate;
    int target_length = 5;
    char target[target_length]; // 4 ciphers + '/0'
    // get string length
    for (length = 0; source[length] != '\0'; length++)
        ;
    
    // printf("\nLength is %i!\n", length);
    
    // reach end of bitrate string (name of directory of the segments)
    for (i = length - 2; source[i + 1] != '/'; i--)
        ;
    
    // printf("\n/ is at position %i!\n", i);
    
    target[target_length - 1] = '\0'; // assign null at last
    j = target_length - 2;
    
    while (source[i] != '/') {
        // printf("%c", source[i]);
        target[j] = source[i];
        j--;
        i--;
    }
    j++;
    
    // check if the bitrate ciphers are less than target_length - 1
    if (j > 0)
        for (int d = j; d < target_length; d++)
            target[d - j] = target[d]; // shift the ciphers by j
        
    // printf("\n");
    for (j = 0; j < target_length; j++)
        ;
        // printf("%c", target[j]); // shift the ciphers by j
        
    bitrate = atoi(target);
    
    // printf("\nBitrate gathered -> %i!\n", bitrate);
    
    return bitrate;
}

/*
function : getSegIndFromPath
argument list:
1) source - path source string
Return type - unsigned : segment index on success, 0 on failure
*/

static unsigned
getSegIndFromPath(const char* source)
{
	// Minh - MOD - S
	#if 0
    int length, i, j;
    int target_length = 3;
    unsigned seg_ind;
    char target[target_length]; // 2 ciphers + '/0'
    // get string length
    for (length = 0; source[length] != '\0'; length++)
        ;
    
    // reach end of segment index string (e.g. 'segment_75.m4s')
    for (i = length - 2; source[i + 1] != '.'; i--)
        ;
    
    target[target_length - 1] = '\0'; // assign null at last
    j = target_length - 2;
    
    while (source[i] != '_') {
        target[j] = source[i];
        j--;
        i--;
    }
    j++;
    
    // check if the segment index ciphers are less than target_length - 1
    if (j > 0)
        for (int d = j; d < target_length; d++)
            target[d - j] = target[d]; // shift the ciphers by j
        
    seg_ind = (unsigned) atoi(target);
    #else
    // example of source: ghent/5450/segment_100.m4s
    unsigned seg_ind = 0;

	char str[strlen(source) + 1]; // do not forget +1 for the null character!
	strcpy(str, source);    

    char *pChr = strtok (str, "_.");
    int i = 0;
    while (pChr != NULL) {
        if (i == 1) {	// the seg_id is in the 2nd element
        	// printf("----------- seg_id found: %s\n", pChr);
        	seg_ind = (unsigned) atoi(pChr);
        }
        pChr = strtok (NULL, "_.");
        i++;
    }

    #endif
    // Minh - MOD - E
    // printf("============> Minh --- seg_ind: %u\n", seg_ind);
    return seg_ind;
}


static void
weights_init (long double W[N_REP][N_MAX_SEG], char const* filename) {
    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;
    unsigned r_ind = 0; // representation index
    int n = 0;
    char target[N_MAX_SEG][40];

    fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("NO WEIGHT FILE FOUND at %s!\n", filename);
        return;
    }
    
    while ((read = getline(&line, &len, fp)) != -1) {
        //printf("Retrieved line of length %zu:\n", read);
        //printf("%s", line);
        n = getWords(line, target);
        // printf("N = %d\n", n);
        if (n != N_MAX_SEG) {
            printf("ERROR: Quality Idx %d: number of returned words from getWords() is %i instead of %i\n", r_ind, n, N_MAX_SEG);
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            W[r_ind][i] = strtold(target[i], NULL)*8/1000.0;
            // printf("W[%d][%ld] = %Lf ", r_ind, i, W[r_ind][i]);
        }
        printf("\n");
        r_ind++;
    }

    fclose(fp);
    if (line)
        free(line);
    printf("=========== 2 ==================\n");
}

static void
update_sample_stats (struct sample_stats *stats, unsigned long val)
{
    LSQ_DEBUG("%s: %p: %lu", __func__, stats, val);
    if (stats->n)
    {
        if (val < stats->min)
            stats->min = val;
        else if (val > stats->max)
            stats->max = val;
    }
    else
    {
        stats->min = val;
        stats->max = val;
    }
    stats->sum += val;
    stats->sum_X2 += val * val;
    ++stats->n;
}


static void
calc_sample_stats (const struct sample_stats *stats,
        long double *mean_p, long double *stddev_p)
{
    unsigned long mean, tmp;

    if (stats->n)
    {
        mean = stats->sum / stats->n;
        *mean_p = (long double) mean;
        if (stats->n > 1)
        {
            tmp = stats->sum_X2 - stats->n * mean * mean;
            tmp /= stats->n - 1;
            *stddev_p = sqrtl((long double) tmp);
        }
        else
            *stddev_p = 0;
    }
    else
    {
        *mean_p = 0;
        *stddev_p = 0;
    }
}


/* When more than `nread' bytes are read from stream `stream_id', apply
 * priority in `ehp'.
 */
struct priority_spec
{
    enum {
        PRIORITY_SPEC_ACTIVE    = 1 << 0,
    }                                       flags;
    lsquic_stream_id_t                      stream_id;
    size_t                                  nread;
    struct lsquic_ext_http_prio             ehp;
};
static struct priority_spec *s_priority_specs;
static unsigned s_n_prio_specs;

static void
maybe_perform_priority_actions (struct lsquic_stream *, lsquic_stream_ctx_t *);

struct lsquic_conn_ctx;

struct path_elem {
    TAILQ_ENTRY(path_elem)      next_pe;
    const char                 *path;
    unsigned                    seg_ind;
    unsigned                    seg_q;
};

struct http_client_ctx {
    const char                  *hostname;
    const char                  *method;
    const char                  *payload;
    char                         payload_size[20];

    /* hcc_path_elems holds a list of paths which are to be requested from
     * the server.  Each new request gets the next path from the list (the
     * iterator is stored in hcc_cur_pe); when the end is reached, the
     * iterator wraps around.
     */
    TAILQ_HEAD(, path_elem)      hcc_path_elems;
    struct path_elem            *hcc_cur_pe;
    unsigned                     hcc_still_segments;
    
    TAILQ_HEAD(, path_elem)      hcc_ret_path_elems; // Re-transmission queue
    struct path_elem            *hcc_ret_pe;
    unsigned                     hcc_open_ret_streams;
    unsigned                     hcc_open_streams;
    unsigned                     hcc_still_ret_segments; // Segments missing to be re-transmitted from current re-transmission window
    
    unsigned                     chosen_abr; // {0 -> Normalized_Quality_Instability; 1 -> MaxJ; 2 -> MaxMinJ (MaxJ*); 3 -> MaxMinJ_Buff_Norm; 4 -> MaxR select; 5 -> BOLA; 6 -> SARA; 7 -> BBA}
    bool                         h2br; // If H2BR module is implemented
    unsigned                     hcc_total_n_reqs;
    unsigned                     hcc_reqs_per_conn;
    unsigned                     hcc_concurrency;
    unsigned                     hcc_cc_reqs_per_conn;
    unsigned                     hcc_n_open_conns;
    unsigned                     hcc_reset_after_nbytes;
    unsigned                     hcc_retire_cid_after_nbytes;
    const char                  *hcc_download_dir;
    
    char                        *hcc_sess_resume_file_name;

    enum {
        HCC_SKIP_SESS_RESUME    = (1 << 0),
        HCC_SEEN_FIN            = (1 << 1),
        HCC_ABORT_ON_INCOMPLETE = (1 << 2),
    }                            hcc_flags;
    struct prog                 *prog;
    const char                  *qif_file;
    FILE                        *qif_fh;
};

struct lsquic_conn_ctx {
    TAILQ_ENTRY(lsquic_conn_ctx) next_ch;
    lsquic_conn_t       *conn;
    struct http_client_ctx   *client_ctx;
    lsquic_time_t        ch_created;
    unsigned             ch_n_reqs;    /* This number gets decremented as streams are closed and
                                        * incremented as push promises are accepted.
                                        */
    unsigned             ch_n_cc_streams;   /* This number is incremented as streams are opened
                                             * and decremented as streams are closed. It should
                                             * never exceed hcc_cc_reqs_per_conn in client_ctx.
                                             */
    enum {
        CH_SESSION_RESUME_SAVED   = 1 << 0,
    }                    ch_flags;
};


struct hset_elem
{
    STAILQ_ENTRY(hset_elem)     next;
    size_t                      nalloc;
    struct lsxpack_header       xhdr;
};


STAILQ_HEAD(hset, hset_elem);

static void
hset_dump (const struct hset *, FILE *);
static void
hset_destroy (void *hset);
static void
display_cert_chain (lsquic_conn_t *);


static void
create_connections (struct http_client_ctx *client_ctx)
{
    size_t len;
    FILE *file;
    unsigned char sess_resume[0x2000];

    if (0 == (client_ctx->hcc_flags & HCC_SKIP_SESS_RESUME)
                                    && client_ctx->hcc_sess_resume_file_name)
    {
        file = fopen(client_ctx->hcc_sess_resume_file_name, "rb");
        if (!file)
        {
            LSQ_DEBUG("cannot open %s for reading: %s",
                        client_ctx->hcc_sess_resume_file_name, strerror(errno));
            goto no_file;
        }
        len = fread(sess_resume, 1, sizeof(sess_resume), file);
        if (0 == len && !feof(file))
            LSQ_WARN("error reading %s: %s",
                        client_ctx->hcc_sess_resume_file_name, strerror(errno));
        fclose(file);
        LSQ_INFO("create connection sess_resume %zu bytes", len);
    }
    else no_file:
        len = 0;

    while (client_ctx->hcc_n_open_conns < client_ctx->hcc_concurrency &&
           client_ctx->hcc_total_n_reqs > 0)
        if (0 != prog_connect(client_ctx->prog, len ? sess_resume : NULL, len))
        {
            LSQ_ERROR("connection failed");
            exit(EXIT_FAILURE);
        }
}


static void
create_streams (struct http_client_ctx *client_ctx, lsquic_conn_ctx_t *conn_h)
{
    t_stats.tot_throughput /= (client_ctx->hcc_still_ret_segments + client_ctx->hcc_still_segments); // Throughput subdivision for number of streams to be opened
    while (conn_h->ch_n_reqs - conn_h->ch_n_cc_streams &&
            conn_h->ch_n_cc_streams < client_ctx->hcc_cc_reqs_per_conn && client_ctx->hcc_open_streams < (client_ctx->hcc_still_ret_segments + client_ctx->hcc_still_segments))
    {
        lsquic_conn_make_stream(conn_h->conn);
        conn_h->ch_n_cc_streams++;
        ++client_ctx->hcc_open_streams;
    }
}


static lsquic_conn_ctx_t *
http_client_on_new_conn (void *stream_if_ctx, lsquic_conn_t *conn)
{
    struct http_client_ctx *client_ctx = stream_if_ctx;
    lsquic_conn_ctx_t *conn_h = calloc(1, sizeof(*conn_h));
    conn_h->conn = conn;
    conn_h->client_ctx = client_ctx;
    conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                client_ctx->hcc_reqs_per_conn);
    client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
    ++conn_h->client_ctx->hcc_n_open_conns;
    if (!TAILQ_EMPTY(&client_ctx->hcc_path_elems))
        create_streams(client_ctx, conn_h);
    conn_h->ch_created = lsquic_time_now();
    return conn_h;
}


struct create_another_conn_or_stop_ctx
{
    struct event            *event;
    struct http_client_ctx  *client_ctx;
};


static void
create_another_conn_or_stop (evutil_socket_t sock, short events, void *ctx)
{
    struct create_another_conn_or_stop_ctx *const cacos = ctx;
    struct http_client_ctx *const client_ctx = cacos->client_ctx;

    event_del(cacos->event);
    event_free(cacos->event);
    free(cacos);

    create_connections(client_ctx);
    if (0 == client_ctx->hcc_n_open_conns)
    {
        LSQ_INFO("All connections are closed: stop engine");
        prog_stop(client_ctx->prog);
    }
}


static void
http_client_on_conn_closed (lsquic_conn_t *conn)
{
    lsquic_conn_ctx_t *conn_h = lsquic_conn_get_ctx(conn);
    struct create_another_conn_or_stop_ctx *cacos;
    enum LSQUIC_CONN_STATUS status;
    struct event_base *eb;
    char errmsg[80];

    status = lsquic_conn_status(conn, errmsg, sizeof(errmsg));
    LSQ_INFO("Connection closed.  Status: %d.  Message: %s", status,
        errmsg[0] ? errmsg : "<not set>");
    if (conn_h->client_ctx->hcc_flags & HCC_ABORT_ON_INCOMPLETE)
    {
        if (!(conn_h->client_ctx->hcc_flags & HCC_SEEN_FIN))
            abort();
    }
    --conn_h->client_ctx->hcc_n_open_conns;

    cacos = calloc(1, sizeof(*cacos));
    if (!cacos)
    {
        LSQ_ERROR("cannot allocate cacos");
        exit(1);
    }
    eb = prog_eb(conn_h->client_ctx->prog);
    cacos->client_ctx = conn_h->client_ctx;
    cacos->event = event_new(eb, -1, 0, create_another_conn_or_stop, cacos);
    if (!cacos->event)
    {
        LSQ_ERROR("cannot allocate event");
        exit(1);
    }
    if (0 != event_add(cacos->event, NULL))
    {
        LSQ_ERROR("cannot add cacos event");
        exit(1);
    }
    event_active(cacos->event, 0, 0);

    free(conn_h);
}


static int
hsk_status_ok (enum lsquic_hsk_status status)
{
    return status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK;
}


static void
http_client_on_hsk_done (lsquic_conn_t *conn, enum lsquic_hsk_status status)
{
    lsquic_conn_ctx_t *conn_h = lsquic_conn_get_ctx(conn);
    struct http_client_ctx *client_ctx = conn_h->client_ctx;

    if (hsk_status_ok(status))
        LSQ_INFO("handshake success %s",
                    status == LSQ_HSK_RESUMED_OK ? "(session resumed)" : "");
    else if (status == LSQ_HSK_FAIL)
        LSQ_INFO("handshake failed");
    else if (status == LSQ_HSK_RESUMED_FAIL)
    {
        LSQ_INFO("handshake failed because of session resumption, will retry "
                                                                "without it");
        client_ctx->hcc_flags |= HCC_SKIP_SESS_RESUME;
        ++client_ctx->hcc_concurrency;
        ++client_ctx->hcc_total_n_reqs;
    }
    else
        assert(0);

    if (hsk_status_ok(status) && s_display_cert_chain)
        display_cert_chain(conn);

    if (hsk_status_ok(status))
    {
        conn_h = lsquic_conn_get_ctx(conn);
        ++s_stat_conns_ok;
        update_sample_stats(&s_stat_to_conn,
                                    lsquic_time_now() - conn_h->ch_created);
        if (TAILQ_EMPTY(&client_ctx->hcc_path_elems))
        {
            LSQ_INFO("no paths mode: close connection");
            lsquic_conn_close(conn_h->conn);
        }
    }
    else
        ++s_stat_conns_failed;
}


/* Now only used for gQUIC and will be going away after that */
static void
http_client_on_sess_resume_info (lsquic_conn_t *conn, const unsigned char *buf,
                                                                size_t bufsz)
{
    lsquic_conn_ctx_t *const conn_h = lsquic_conn_get_ctx(conn);
    struct http_client_ctx *const client_ctx = conn_h->client_ctx;
    FILE *file;
    size_t nw;

    assert(client_ctx->hcc_sess_resume_file_name);

    /* Our client is rather limited: only one file and only one ticket per
     * connection can be saved.
     */
    if (conn_h->ch_flags & CH_SESSION_RESUME_SAVED)
    {
        LSQ_DEBUG("session resumption information already saved for this "
                                                                "connection");
        return;
    }

    file = fopen(client_ctx->hcc_sess_resume_file_name, "wb");
    if (!file)
    {
        LSQ_WARN("cannot open %s for writing: %s",
            client_ctx->hcc_sess_resume_file_name, strerror(errno));
        return;
    }

    nw = fwrite(buf, 1, bufsz, file);
    if (nw == bufsz)
    {
        LSQ_DEBUG("wrote %zd bytes of session resumption information to %s",
            nw, client_ctx->hcc_sess_resume_file_name);
        conn_h->ch_flags |= CH_SESSION_RESUME_SAVED;
    }
    else
        LSQ_WARN("error: fwrite(%s) returns %zd instead of %zd: %s",
            client_ctx->hcc_sess_resume_file_name, nw, bufsz, strerror(errno));

    fclose(file);
}


struct lsquic_stream_ctx {
    lsquic_stream_t     *stream;
    struct http_client_ctx   *client_ctx;
    const char          *path;
    enum {
        HEADERS_SENT    = (1 << 0),
        PROCESSED_HEADERS = 1 << 1,
        ABANDON = 1 << 2,   /* Abandon reading from stream after sh_stop bytes
                             * have been read.
                             */
    }                    sh_flags;
    lsquic_time_t        sh_created;
    lsquic_time_t        sh_ttfb;
    size_t               sh_stop;   /* Stop after reading this many bytes if ABANDON is set */
    size_t               sh_nread;  /* Number of bytes read from stream using one of
                                     * lsquic_stream_read* functions.
                                     */
    bool                 isTerminated;
    long double          sh_throughput;
    bool                 isRet;
    unsigned             seg_ind;
    unsigned             seg_q;
    unsigned             count;
    FILE                *download_fh;
    struct lsquic_reader reader;
};

static bool
isRetSegAcceptable(struct lsquic_stream_ctx *st_h)
{
    //printf("SEG_IND: %i, SEG_REP: %i", client_ctx->hcc_ret_pe->seg_ind, rep_seg_ind);
    // Check if re-transmitted index (real array index [starting from 0]) + 1 > rep_seg_ind (path index starting from 1 [not 0])
    if (st_h->seg_ind > rep_seg_ind)
        return true;
    else
        return false;
}


static lsquic_stream_ctx_t *
http_client_on_new_stream (void *stream_if_ctx, lsquic_stream_t *stream)
{
    
    /* Buffer update */
    update_buff(false); // [false] is "normal Time Update"
    // printf("Buffer size: %.3f sec\n", buffer_level);
    
    const int pushed = lsquic_stream_is_pushed(stream);

    if (pushed)
    {
        LSQ_INFO("not accepting server push");
        lsquic_stream_refuse_push(stream);
        return NULL;
    }

    lsquic_stream_ctx_t *st_h = calloc(1, sizeof(*st_h));
    st_h->stream = stream;
    st_h->client_ctx = stream_if_ctx;
    st_h->sh_created = lsquic_time_now();
    st_h->isTerminated = false;
    
    struct path_elem *temp_pe;
    temp_pe = calloc(1, sizeof(*temp_pe));
    
    transmission:
        printf("\nTransmission:\n");
        if (!st_h->client_ctx->hcc_cur_pe) {
            printf("====> INIT QUEUE !\n");
            st_h->client_ctx->hcc_cur_pe = TAILQ_FIRST(
                                                &st_h->client_ctx->hcc_path_elems);
        } else if (st_h->client_ctx->hcc_still_segments) {
            // If we don't have space in the buffer, wait sometime before sending request for new segment
            if (buffer_level > buffer_size){
                printf("==> MAIN2 FULL BUFFER! Sleep for %d s\n", (unsigned int) seg_length);
                sleep((unsigned int) seg_length); // Sleep for x seconds until the buffer level allow new segments download
                st_h->sh_created = lsquic_time_now();
            }
            temp_pe = TAILQ_NEXT(st_h->client_ctx->hcc_cur_pe, next_pe);
            if (!temp_pe){ // If there are no more available new segments to be downloaded throw an error
                if (st_h->client_ctx->hcc_still_ret_segments) { // If we still have segments to be retransmitted let's try that
                    goto retransmission;
                } else { // Otherwise close the stream
                    LSQ_ERROR("NO SEGMENTS IN ANY QUEUE.");
                    lsquic_stream_close(stream);
                    return st_h;
                }
            } else
                st_h->client_ctx->hcc_cur_pe = temp_pe;
        }
        else if (st_h->client_ctx->hcc_still_ret_segments) {
            goto retransmission;
        }
        st_h->path = st_h->client_ctx->hcc_cur_pe->path;
        printf("=================MINH 913 path: %s\n", st_h->path);
        st_h->isRet = false;
        st_h->seg_ind = st_h->client_ctx->hcc_cur_pe->seg_ind;
        st_h->seg_q = st_h->client_ctx->hcc_cur_pe->seg_q;
        goto process_path;
    
    retransmission:
        printf("\nRe-Transmission:\n");
        // temp_pe = calloc(1, sizeof(*temp_pe));
        //if(!TAILQ_EMPTY(&st_h->client_ctx->hcc_ret_path_elems) && (st_h->client_ctx->hcc_open_ret_streams < 1)) {
        if(!TAILQ_EMPTY(&st_h->client_ctx->hcc_ret_path_elems) && st_h->client_ctx->hcc_still_ret_segments) {
            if (st_h->client_ctx->hcc_cc_reqs_per_conn > 1) {
                if (st_h->client_ctx->hcc_ret_pe){
                    temp_pe = TAILQ_NEXT(st_h->client_ctx->hcc_ret_pe, next_pe);
                    if (!temp_pe) { // If it's the last element of the queue
                        if (st_h->client_ctx->hcc_still_segments) { // If we still have segments to be retransmitted let's try that
                            goto transmission;
                        } else {
                            LSQ_ERROR("NO SEGMENTS IN ANY QUEUE.");
                            lsquic_stream_close(stream);
                            return st_h;
                        }
                    } else
                        st_h->client_ctx->hcc_ret_pe = temp_pe;
                }
            }
            if (!st_h->client_ctx->hcc_ret_pe)
                st_h->client_ctx->hcc_ret_pe = TAILQ_FIRST(&st_h->client_ctx->hcc_ret_path_elems);
            /* Path has been set. Check whether the throughput is enough to re-download the segment in time before the playout of its low-quality version */
            double available_time = 0.0;
            if (st_h->client_ctx->hcc_ret_pe->seg_ind > rep_seg_ind)
                available_time = rep_seg_time + (st_h->client_ctx->hcc_ret_pe->seg_ind - rep_seg_ind - 1) * seg_length; // .. (st_h->client_ctx->hcc_ret_pe->seg_ind (+1) - rep_seg_ind (-1)) * ..
            // printf("Available time: %.3f\n", available_time);
            // printf("Bitrate: %d\n", seg_bitrates[st_h->client_ctx->hcc_ret_pe->seg_q]);
            // printf("Estimated throughput: %.3Lf\n", t_stats.e_temp_throughput);
            // printf("Re-transmission time: %.3Lf\n", seg_bitrates[st_h->client_ctx->hcc_ret_pe->seg_q] * seg_length / t_stats.e_temp_throughput);
            /* Until the available playout time before the to-be-re-transmitted segment is lower than the time required to re-transmit the segment check the next to-be-re-transmitted element */
            // while(available_time < (seg_bitrates[st_h->client_ctx->hcc_ret_pe->seg_q] * seg_length / t_stats.e_temp_throughput)){
                // if ((temp_pe = TAILQ_NEXT(st_h->client_ctx->hcc_ret_pe, next_pe))){
                    // st_h->client_ctx->hcc_ret_pe = temp_pe;
                    // available_time = rep_seg_time + (st_h->client_ctx->hcc_ret_pe->seg_ind - rep_seg_ind) * seg_length;
                    // --st_h->client_ctx->hcc_still_ret_segments;
                // } else { // No more re-transmission element in the queue to be checked -> Try to download new segments
                    // if (st_h->client_ctx->hcc_still_segments) { // If we still have segments to be retransmitted let's try that
                        // goto transmission;
                    // } else {
                        // st_h->client_ctx->hcc_still_ret_segments = 0;
                        // LSQ_ERROR("NO SEGMENTS IN ANY QUEUE.");
                        // lsquic_stream_close(stream);
                        // return st_h;
                    // }
                // }
            // }
            ///////////////////// MINH - Temporal fix - S
            char *m_temp = st_h->client_ctx->hcc_ret_pe->path;
            int m_temp_length = strlen(m_temp);
            printf("=================MINH 966-1 path: %s. last character: %c, length: %ld\n",
                m_temp, m_temp[m_temp_length-1], m_temp_length);

            if (m_temp[m_temp_length-1] != 's') {
                strcat(m_temp,"s");
                printf("\t***** fixed path: %s. length: %ld\n", m_temp, strlen(m_temp));
            }           
            #if 0
            st_h->path = st_h->client_ctx->hcc_ret_pe->path; // Set the segment path
            #endif
            st_h->path = m_temp;
            printf("=================MINH 966-2 path: %s\n", st_h->path);
            ///////////////////// MINH - Temporal fix - E
            st_h->isRet = true; // It's a re-transmission
            st_h->seg_ind = st_h->client_ctx->hcc_ret_pe->seg_ind;
            st_h->seg_q = st_h->client_ctx->hcc_ret_pe->seg_q;
            //++st_h->client_ctx->hcc_open_ret_streams; // This stream belongs to the re-transmission ones
            goto process_path; // Process the request
        } else {
            LSQ_ERROR("NO SEGMENTS IN RETRANSMISSION QUEUE.");
            lsquic_stream_close(stream);
            return st_h;
        }
        
    process_path:
        printf("\nProcess-path:\n");
        // Try concurrent priority 50%,50%
        lsquic_stream_set_http_prio(stream, &(struct lsquic_ext_http_prio){
                        .urgency = LSQUIC_DEF_HTTP_URGENCY,
                        .incremental = 1,
                    });
    
        if (st_h->client_ctx->payload)
        {
        	printf("Process-path-1:\n");
            st_h->reader.lsqr_read = test_reader_read;
            st_h->reader.lsqr_size = test_reader_size;
            st_h->reader.lsqr_ctx = create_lsquic_reader_ctx(st_h->client_ctx->payload);
            if (!st_h->reader.lsqr_ctx)
                exit(1);
        }
        else
            st_h->reader.lsqr_ctx = NULL;
        LSQ_INFO("created new stream, path: %s", st_h->path);
        lsquic_stream_wantwrite(stream, 1);
        printf("Process-path-2:\n");
        if (randomly_reprioritize_streams)
        {
            if ((1 << lsquic_conn_quic_version(lsquic_stream_conn(stream)))
                                                        & LSQUIC_IETF_VERSIONS)
                lsquic_stream_set_http_prio(stream,
                    &(struct lsquic_ext_http_prio){
                        .urgency = random() & 7,
                        .incremental = random() & 1,
                    }
                );
            else
                lsquic_stream_set_priority(stream, 1 + (random() & 0xFF));
        }
        if (s_priority_specs)
            maybe_perform_priority_actions(stream, st_h);
        if (s_abandon_early)
        {
            st_h->sh_stop = random() % (s_abandon_early + 1);
            st_h->sh_flags |= ABANDON;
        }

        printf("Process-path-3:\n");
        if (st_h->client_ctx->hcc_download_dir)
        {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s",
                                st_h->client_ctx->hcc_download_dir, st_h->path);
            st_h->download_fh = fopen(path, "wb");
            if (st_h->download_fh)
                LSQ_NOTICE("downloading %s to %s", st_h->path, path);
            else
            {
                LSQ_ERROR("cannot open %s for writing: %s", path, strerror(errno));
                lsquic_stream_close(stream);
            }
        }
        else
            st_h->download_fh = NULL;

        return st_h;
}


static void
send_headers (lsquic_stream_ctx_t *st_h)
{
    const char *hostname = st_h->client_ctx->hostname;
    struct header_buf hbuf;
    unsigned h_idx = 0;
    if (!hostname)
        hostname = st_h->client_ctx->prog->prog_hostname;
    hbuf.off = 0;
    struct lsxpack_header headers_arr[10];
#define V(v) (v), strlen(v)
    header_set_ptr(&headers_arr[h_idx++], &hbuf, V(":method"), V(st_h->client_ctx->method));
    header_set_ptr(&headers_arr[h_idx++], &hbuf, V(":scheme"), V("https"));
    header_set_ptr(&headers_arr[h_idx++], &hbuf, V(":path"), V(st_h->path));
    header_set_ptr(&headers_arr[h_idx++], &hbuf, V(":authority"), V(hostname));
    header_set_ptr(&headers_arr[h_idx++], &hbuf, V("user-agent"), V(st_h->client_ctx->prog->prog_settings.es_ua));
    //header_set_ptr(&headers_arr[h_idx++], &hbuf, V("expect"), V("100-continue"));
    if (randomly_reprioritize_streams)
    {
        char pfv[10];
        sprintf(pfv, "u=%ld", random() & 7);
        header_set_ptr(&headers_arr[h_idx++], &hbuf, V("priority"), V(pfv));
        if (random() & 1)
            sprintf(pfv, "i");
        else
            sprintf(pfv, "i=?0");
        header_set_ptr(&headers_arr[h_idx++], &hbuf, V("priority"), V(pfv));
    }
    if (st_h->client_ctx->payload)
    {
        header_set_ptr(&headers_arr[h_idx++], &hbuf, V("content-type"), V("application/octet-stream"));
        header_set_ptr(&headers_arr[h_idx++], &hbuf, V("content-length"), V( st_h->client_ctx->payload_size));
    }
    lsquic_http_headers_t headers = {
        .count = h_idx,
        .headers = headers_arr,
    };
    if (0 != lsquic_stream_send_headers(st_h->stream, &headers,
                                    st_h->client_ctx->payload == NULL))
    {
        LSQ_ERROR("cannot send headers: %s", strerror(errno));
        exit(1);
    }
}


/* This is here to exercise lsquic_conn_get_server_cert_chain() API */
static void
display_cert_chain (lsquic_conn_t *conn)
{
    STACK_OF(X509) *chain;
    X509_NAME *name;
    X509 *cert;
    unsigned i;
    char buf[100];

    chain = lsquic_conn_get_server_cert_chain(conn);
    if (!chain)
    {
        LSQ_WARN("could not get server certificate chain");
        return;
    }

    for (i = 0; i < sk_X509_num(chain); ++i)
    {
        cert = sk_X509_value(chain, i);
        name = X509_get_subject_name(cert);
        LSQ_INFO("cert #%u: name: %s", i,
                            X509_NAME_oneline(name, buf, sizeof(buf)));
        X509_free(cert);
    }

    sk_X509_free(chain);
}


static void
http_client_on_write (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    ssize_t nw;

    if (st_h->sh_flags & HEADERS_SENT)
    {
        if (st_h->client_ctx->payload && test_reader_size(st_h->reader.lsqr_ctx) > 0)
        {
            nw = lsquic_stream_writef(stream, &st_h->reader);
            if (nw < 0)
            {
                LSQ_ERROR("write error: %s", strerror(errno));
                exit(1);
            }
            if (test_reader_size(st_h->reader.lsqr_ctx) > 0)
            {
                lsquic_stream_wantwrite(stream, 1);
            }
            else
            {
                lsquic_stream_shutdown(stream, 1);
                lsquic_stream_wantread(stream, 1);
            }
        }
        else
        {
            lsquic_stream_shutdown(stream, 1);
            lsquic_stream_wantread(stream, 1);
        }
    }
    else
    {
        st_h->sh_flags |= HEADERS_SENT;
        send_headers(st_h);
    }
}


static size_t
discard (void *ctx, const unsigned char *buf, size_t sz, int fin)
{
    lsquic_stream_ctx_t *st_h = ctx;

    if (st_h->sh_flags & ABANDON)
    {
        if (sz > st_h->sh_stop - st_h->sh_nread)
            sz = st_h->sh_stop - st_h->sh_nread;
    }

    return sz;
}


static void
maybe_perform_priority_actions (struct lsquic_stream *stream,
                                                lsquic_stream_ctx_t *st_h)
{
    const lsquic_stream_id_t stream_id = lsquic_stream_id(stream);
    struct priority_spec *spec;
    unsigned n_active;
    int s;

    n_active = 0;
    for (spec = s_priority_specs; spec < s_priority_specs + s_n_prio_specs;
                                                                        ++spec)
    {
        if ((spec->flags & PRIORITY_SPEC_ACTIVE)
            && spec->stream_id == stream_id
            && st_h->sh_nread >= spec->nread)
        {
            s = lsquic_stream_set_http_prio(stream, &spec->ehp);
            if (s != 0)
            {
                LSQ_ERROR("could not apply priorities to stream %"PRIu64,
                                                                    stream_id);
                exit(1);
            }
            spec->flags &= ~PRIORITY_SPEC_ACTIVE;
        }
        n_active += !!(spec->flags & PRIORITY_SPEC_ACTIVE);
    }

    if (n_active == 0)
        s_priority_specs = NULL;
}


static void
http_client_on_read (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    struct http_client_ctx *const client_ctx = st_h->client_ctx;
    struct hset *hset;
    ssize_t nread;
    unsigned old_prio, new_prio;
    unsigned char buf[0x200];
    unsigned nreads = 0;
    /* Throughput computation */
    // lsquic_time_t init;
    lsquic_time_t end;
    // Segment bitrate and index
    int bitrate = 0;
    unsigned seg_ind = 0;
    // Gather bitrate from path
    // printf("Gather bitrate...\n");
    bitrate = getBitrateFromPath(st_h->path);
    seg_ind = getSegIndFromPath(st_h->path);
    
    // init = lsquic_time_now();
    
    if (bitrate == 0 || seg_ind == 0) {
        printf("==== UNABLE TO GATHER BITRATE OR SEG IND FROM PATH -> CLOSING STREAM ====\n");
        printf("========== bitrate: %d\n", bitrate);
        printf("========== seg_ind: %u\n", seg_ind);
        lsquic_stream_close(stream);
        return;
    } // else
        // printf("==== Gathered Bitrate (%i) and Segment Index (%u) from path: Throughput/Deadline check will start soon ====\n", bitrate, seg_ind);
    
#ifdef WIN32
    srand(GetTickCount());
#endif

    do
    {
        /* Throughput computation */
        if (g_header_bypass && !(st_h->sh_flags & PROCESSED_HEADERS))
        {
            hset = lsquic_stream_get_hset(stream);
            if (!hset)
            {
                LSQ_ERROR("could not get header set from stream");
                exit(2);
            }
            st_h->sh_ttfb = lsquic_time_now();
            update_sample_stats(&s_stat_ttfb, st_h->sh_ttfb - st_h->sh_created);
            if (s_discard_response)
                LSQ_DEBUG("discard response: do not dump headers");
            else
                hset_dump(hset, stdout);
            hset_destroy(hset);
            st_h->sh_flags |= PROCESSED_HEADERS;
        }
        else if (nread = (s_discard_response
                            ? lsquic_stream_readf(stream, discard, st_h)
                            : lsquic_stream_read(stream, buf,
                                    st_h->sh_flags & ABANDON
                                  ? MIN(sizeof(buf), st_h->sh_nread - st_h->sh_stop)
                                  : sizeof(buf))),
                    nread > 0)
        {
            st_h->sh_nread += (size_t) nread;
            s_stat_downloaded_bytes += nread;
            end = lsquic_time_now();
            // printf("Init: %ld; End: %ld\n", st_h->sh_created, end);
            // printf("Difference: %ld\n", end - st_h->sh_created);
            update_buff(false);
            if (st_h->seg_q > 0 && (client_ctx->chosen_abr == 0 || client_ctx->chosen_abr == 2 || client_ctx->chosen_abr == 3)              && st_h->isRet) {
                /* ADD Throughput computation and CANCEL FEATURE */
                st_h->sh_throughput = (long double) st_h->sh_nread * 8 / ((long double) 1000 * (end - st_h->sh_created) / 1000000); // [kbps]
                // printf("Passed seconds: %Lf\n", (long double) (end - st_h->sh_created) / 1000000);
                /* Compute deadline and download time for segment */
                double available_time = 0.0;
                if (st_h->isRet) {
                    if (seg_ind > rep_seg_ind)
                        available_time = rep_seg_time + (seg_ind - rep_seg_ind - 1) * seg_length;
                } else
                    available_time = rep_seg_time + (seg_ind - rep_seg_ind - 1) * seg_length;
                /* CANCEL_PUSH if available_time is not enough */
                if (request_cancellation) {
                    printf("\n==! TRANSMISSION STATUS CHECK !==\n");
                    printf("Number of read bytes from stream: %zu\n", st_h->sh_nread);
                    printf("Available time: %.3f\n", available_time);
                    printf("Estimated throughput: %.3Lf\n", st_h->sh_throughput);
                    printf("Re-transmission time: %.3Lf\n", (bitrate * seg_length - (st_h->sh_nread * 8 / 1000)) / st_h->sh_throughput);
                    // if (available_time < 0.9 * ((bitrate * seg_length - (st_h->sh_nread * 8 / 1000)) / st_h->sh_throughput)) {
                    if (available_time < 0.1) {
                        printf("\n==! TRANSMISSION STATUS CHECK !==\n");
                        printf("Number of read bytes from stream: %zu\n", st_h->sh_nread);
                        printf("Available time: %.3f\n", available_time);
                        printf("Estimated throughput: %.3Lf\n", st_h->sh_throughput);
                        printf("Re-transmission time: %.3Lf\n", (bitrate * seg_length - (st_h->sh_nread * 8 / 1000)) / st_h->sh_throughput);
                        printf("==== NOT ENOUGH THROUGHPUT FOR FULLFILLING REQUEST -> CLOSING STREAM for segment %u ====\n", seg_ind);
                        /* Send CANCEL_PUSH -> If next segment, need to re-download it with different quality */
                        st_h->isTerminated = true;
                        lsquic_stream_close(stream);
                        break;
                    }
                }
            }
            /* test stream_reset after some number of read bytes */
            if (client_ctx->hcc_reset_after_nbytes &&
                s_stat_downloaded_bytes > client_ctx->hcc_reset_after_nbytes)
            {
                lsquic_stream_maybe_reset(stream, 0x1, 1);
                break;
            }
            /* test retire_cid after some number of read bytes */
            if (client_ctx->hcc_retire_cid_after_nbytes &&
                s_stat_downloaded_bytes > client_ctx->hcc_retire_cid_after_nbytes)
            {
                lsquic_conn_retire_cid(lsquic_stream_conn(stream));
                client_ctx->hcc_retire_cid_after_nbytes = 0;
                break;
            }
            if (!g_header_bypass && !(st_h->sh_flags & PROCESSED_HEADERS))
            {
                /* First read is assumed to be the first byte */
                st_h->sh_ttfb = lsquic_time_now();
                update_sample_stats(&s_stat_ttfb,
                                    st_h->sh_ttfb - st_h->sh_created);
                st_h->sh_flags |= PROCESSED_HEADERS;
            }
            if (!s_discard_response)
                fwrite(buf, 1, nread, st_h->download_fh
                                    ? st_h->download_fh : stdout);
            if (randomly_reprioritize_streams && (st_h->count++ & 0x3F) == 0)
            {
                if ((1 << lsquic_conn_quic_version(lsquic_stream_conn(stream)))
                                                        & LSQUIC_IETF_VERSIONS)
                {
                    struct lsquic_ext_http_prio ehp;
                    if (0 == lsquic_stream_get_http_prio(stream, &ehp))
                    {
                        ehp.urgency = 7 & (ehp.urgency + 1);
                        ehp.incremental = !ehp.incremental;
#ifndef NDEBUG
                        const int s =
#endif
                        lsquic_stream_set_http_prio(stream, &ehp);
                        assert(s == 0);
                    }
                }
                else
                {
                    old_prio = lsquic_stream_priority(stream);
                    new_prio = 1 + (random() & 0xFF);
#ifndef NDEBUG
                    const int s =
#endif
                    lsquic_stream_set_priority(stream, new_prio);
                    assert(s == 0);
                    LSQ_DEBUG("changed stream %"PRIu64" priority from %u to %u",
                                lsquic_stream_id(stream), old_prio, new_prio);
                }
            }
            if (s_priority_specs)
                maybe_perform_priority_actions(stream, st_h);
            if ((st_h->sh_flags & ABANDON) && st_h->sh_nread >= st_h->sh_stop)
            {
                LSQ_DEBUG("closing stream early having read %zd bytes",
                                                            st_h->sh_nread);
                lsquic_stream_close(stream);
                break;
            }
        }
        else if (0 == nread)
        {
            update_sample_stats(&s_stat_req, lsquic_time_now() - st_h->sh_ttfb);
            client_ctx->hcc_flags |= HCC_SEEN_FIN;
            lsquic_stream_shutdown(stream, 0);
            break;
        }
        else if (client_ctx->prog->prog_settings.es_rw_once && EWOULDBLOCK == errno)
        {
            LSQ_NOTICE("emptied the buffer in 'once' mode");
            break;
        }
        else if (lsquic_stream_is_rejected(stream))
        {
            LSQ_NOTICE("stream was rejected");
            lsquic_stream_close(stream);
            break;
        }
        else
        {
            LSQ_ERROR("could not read: %s", strerror(errno));
            exit(2);
        }
    }
    while (client_ctx->prog->prog_settings.es_rw_once
            && nreads++ < 3 /* Emulate just a few reads */);
}


static void
http_client_on_close (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    const int pushed = lsquic_stream_is_pushed(stream);
    if (pushed)
    {
        assert(NULL == st_h);
        return;
    }
    
    struct http_client_ctx *const client_ctx = st_h->client_ctx;
    lsquic_conn_t *const conn = lsquic_stream_conn(stream);
    lsquic_conn_ctx_t *const conn_h = lsquic_conn_get_ctx(conn);
    --conn_h->ch_n_reqs;
    --conn_h->ch_n_cc_streams;
    
    printf("SRS: %i, SS: %i", client_ctx->hcc_still_ret_segments, client_ctx->hcc_still_segments);
    
    if (client_ctx->hcc_still_ret_segments || client_ctx->hcc_still_segments) {
    
        printf("Read bytes: %.0ld, time_now(): %.0ld, st_h->sh_created: %.0ld, download time: %.0lds\n", st_h->sh_nread, lsquic_time_now(), st_h->sh_created, (lsquic_time_now() - st_h->sh_created) / 1000000);
        
        long double new_throughput = (long double) st_h->sh_nread * 8 / ((long double) 1000 * (lsquic_time_now() - st_h->sh_created) / 1000000); // [kbps]
        /* Smoothed throughput computation */

        if (t_stats.s_throughput == 0)
            t_stats.s_throughput = new_throughput;
        else
            t_stats.s_throughput = .875 * t_stats.s_throughput + .125 * new_throughput; // (1-1/8) and 1/8 as described in the WISH paper
        /* Throughput computation */
		
		// Minh - Fix 0kbps throughput - S - reason: sh_nread = 0.
		#if 0          
        t_stats.throughput = new_throughput;
        #else
        t_stats.throughput = (new_throughput == 0) ? t_stats.s_throughput : new_throughput;
        #endif
        // Minh - Fix 0kbps throughput - E

        // Estimated Throughput is the MIN(Smoothed and Normal)
        printf("==> Throughput: %.3Lf kbps\n", t_stats.throughput);
        printf("==> Smoothed throughput: %Lf kbps\n", t_stats.s_throughput);
        //t_stats.e_temp_throughput = MIN(t_stats.throughput, t_stats.s_throughput);
        t_stats.e_temp_throughput = 0.9 * t_stats.throughput;
        t_stats.tot_throughput += t_stats.e_temp_throughput; // Useful for computing the total throughput in multistreams scenarios
        printf("==> Estimated throughput: %Lf kbps\n", t_stats.e_temp_throughput);
        printf("==> Total throughput: %Lf kbps\n", t_stats.tot_throughput);
        LSQ_INFO("%s called", __func__);
        // struct http_client_ctx *const client_ctx = st_h->client_ctx;
        // lsquic_conn_t *const conn = lsquic_stream_conn(stream);
        // lsquic_conn_ctx_t *const conn_h = lsquic_conn_get_ctx(conn);
        // --conn_h->ch_n_reqs;
        // --conn_h->ch_n_cc_streams;
        double download_time = (double) (lsquic_time_now() - st_h->sh_created) / 1000000;
        printf("Stream init: %.3f; Stream close: %.3f; Download time: %.3f.\n", (double) st_h->sh_created / 1000000, (double) lsquic_time_now() / 1000000, download_time);
        if (!st_h->isRet){
            if (!st_h->isTerminated) {
                seg_chosen_q[++qualities_ind] = st_h->seg_q;
                t_stats.e_throughput[qualities_ind] = t_stats.e_temp_throughput;
                t_stats.comp_throughput[qualities_ind] = t_stats.throughput;
                // SARA parameters computation
                if (st_h->client_ctx->chosen_abr == 6) {
                    s_stats.weights[seg_ind - 1] = s_stats.W[seg_chosen_q[qualities_ind]][seg_ind - 1];
                    s_stats.down_rate[seg_ind - 1] = t_stats.e_throughput[qualities_ind];
                    long double num = 0.0;
                    long double den = 0.0;
                    unsigned start_ind = 0;
                    if (seg_ind > AVG_COUNT)
                        start_ind = seg_ind - AVG_COUNT;
                    for (size_t i = start_ind; i < seg_ind; i++) {
                        printf("i: %zu, count: %u, seg_ind: %u\n", i, AVG_COUNT, seg_ind);
                        num += s_stats.weights[i];
                        den += s_stats.weights[i]*1.0 / s_stats.down_rate[i];
                        printf("i: %zu -> s_stats.weights: %.3Lf, s_stats.down_rate: %.3Lf\n", i, s_stats.weights[i], s_stats.down_rate[i]);
                    }           
                    s_stats.H = (double) num / den;
                }
                printf("Transmitted segment from path: %s\n", st_h->path);
                /* Buffer update */
                update_buff(true); // [true] is "Segment Received"
                t_stats.b_level[qualities_ind] = buffer_level;
                printf("Buffer size: %.3f sec\n", buffer_level);
                printf("Segment reproduced: %u\n", rep_seg_ind);
                printf("Time left for reproduced segment: %.3f sec\n", rep_seg_time);
                --client_ctx->hcc_still_segments;
                --client_ctx->hcc_open_streams;
                /* QUALITY PRINT */
                printf("Chosen segments qualities [");
                for (unsigned i = 0; i < sizeof(seg_chosen_q) / sizeof(seg_chosen_q[0]); ++i){
                    printf(" %i ", seg_chosen_q[i]);
                }
                printf("]\n");
                /* Update seg_paths */
                ++seg_ind;
                // Check seg_chosen_q size
                // if(seg_ind > (sizeof(seg_chosen_q)/sizeof(seg_chosen_q[0]))){ // If quality array is full and needs to be expanded
                    // printf("Try to realloc seg_chosen_q!\n");
                    // int *seg_chosen_p = seg_chosen_q;
                    // seg_chosen_p = realloc(seg_chosen_p, 2 * seg_ind * sizeof(int)); // Reallocate memory
                // }
                if (seg_ind <= N_MAX_SEG) {
                    for (size_t i = 0; i < N_REP; i++){
                        // Update paths
                        int up_len = strlen(FP_PATH) + 6 + strlen(SP_PATH) + strlen(EXT) + 6; // 6 is the max. number of bitrate ciphers, 6 is the max. segment index (1,..,999999)"
                        char* temp_pp = (char*)malloc((up_len+1)*sizeof(char));
                        snprintf(temp_pp, (up_len+1)*sizeof(char), "%s%d%s%d%s", FP_PATH, seg_bitrates[i], SP_PATH, seg_ind, EXT);
                        printf("Updated path %d: %s\n", (int) i, temp_pp);
                        seg_paths[i] = temp_pp;
                        temp_pp = NULL;
                    }
                } else {
                    update_buff(false);
                    for (int k = 0; k < 10; k++){
                        printf("\a\n");
                        sleep(0.5);
                    }
                    printf("===! END OF SEGMENTS !===\n");
                    printf("!! Closing connection !!\n");
                    client_ctx->hcc_total_n_reqs = 0;
                    lsquic_conn_close(conn_h->conn);
                    return;
                }
            } else {
                --client_ctx->hcc_still_segments;
                --client_ctx->hcc_open_streams;
            }
        } else {
            /* Buffer update */
            update_buff(false); // [false] is "Normal Update" (the re-transmission of a segment doesn't add seconds to the buffer time)
            printf("Buffer size: %.3f sec\n", buffer_level);
            printf("Segment reproduced: %u\n", rep_seg_ind);
            printf("Time left for reproduced segment: %.3f sec\n", rep_seg_time);
            //--client_ctx->hcc_open_ret_streams;
            --client_ctx->hcc_open_streams;
            --client_ctx->hcc_still_ret_segments;
            w_stats.re_count++;
            w_stats.re_data += st_h->sh_nread / 1000;
            if (!st_h->isTerminated) {
                printf("Re-Transmitted segment from path: %s\n", st_h->path);
                if(isRetSegAcceptable(st_h)){
                    printf("Segment acceptable!\n");
                    printf("Quality changed from q = %i", seg_chosen_q[st_h->seg_ind - 1]);
                    seg_chosen_q[st_h->seg_ind - 1] = (int) st_h->seg_q;
                    printf(" to q = %i\n", seg_chosen_q[st_h->seg_ind - 1]);
                } else { // Segment not acceptable
                    w_stats.re_unused_count++;
                    w_stats.re_unused_data += st_h->sh_nread / 1000;
                    printf("Segment NOT acceptable!\n");
                }
            } else { // Stream terminated
                w_stats.re_unused_count++;
                w_stats.re_unused_data += st_h->sh_nread / 1000;
            }
            /* QUALITY PRINT */
            printf("Chosen segments qualities [");
            for (unsigned i = 0; i < sizeof(seg_chosen_q) / sizeof(seg_chosen_q[0]); ++i){
                printf(" %i ", seg_chosen_q[i]);
            }

        }
        
    } else {
        update_buff(false);
        printf("No segment has been trasmitted through this stream!!\n");
        printf("Closing connection!!\n");
        client_ctx->hcc_total_n_reqs = 0;
        lsquic_conn_close(conn_h->conn);
        return;
    }
    
    abr:
    /* ABR Algorithm */
    if (client_ctx->hcc_still_ret_segments == 0 && client_ctx->hcc_still_segments == 0) { // if new segment and re-transmitted segments are received
        printf("Total throughput: %.3Lf kbps\n", t_stats.tot_throughput);
        if (rep_seg_ind < seg_ind) { // If there is still playout of reproduction
            if (buffer_level >= min_init_bs && playout){
                unsigned next_quality = 0;
                if (client_ctx->chosen_abr < 4) {
                    printf("ABR starting... \n"); 
                    
                    unsigned start_seg_ind = rep_seg_ind;
                    // if (rep_seg_time <= available_time_off)
                        // start_seg_ind++; // Start from the second segment after the one being played out (the deadline is too short for the first one after it to be re-downloaded)
                    
                    if (buffer_level < 0.5 * buffer_size && seg_ind < N_MAX_SEG)
                        start_seg_ind = seg_ind - 1; // Only download the next segment
                    
                    unsigned group_n = seg_ind - start_seg_ind;  // Number of segments in the group to be checked for [re-]transmission -> |T|
                    if (seg_ind > N_MAX_SEG)
                        group_n--;
                    
                    unsigned min_q[group_n];
                    for (size_t i = 0; i < group_n; i++){
                        min_q[i] = seg_chosen_q[i + start_seg_ind];
                        if (i < group_n - 1)
                            printf("min_q[%zu]: %d \n", i, min_q[i]);
                    }
                    if (seg_ind > N_MAX_SEG)
                        min_q[group_n - 1] = seg_chosen_q[start_seg_ind + group_n - 1];
                    else
                        min_q[group_n - 1] = 0;
                    printf("min_q[%u]: %d \n", group_n - 1, min_q[group_n - 1]);
                    
                    unsigned n_par = group_n; // Throughputs 
                    for (size_t i = 0; i < group_n; i++){
                        n_par += N_REP - min_q[i];
                    }
                    if (client_ctx->chosen_abr != 1)
                        n_par++; // j*
                    
                    double available_times[group_n];
                    
                    if (buffer_level < 0.5 * buffer_size){
                        available_times[0] = seg_length * 0.9;
                        printf("available_times[%zu]: %.3f \n", 0, available_times[0]);
                    }
                    else {
                        double buffer_threshold = (double) min_init_bs;
                        if (buffer_level >= 0.75 * buffer_size)
                            buffer_threshold = 0.5 * buffer_size;
                        for (size_t i = 0; i < group_n; i++){
                            if (i < group_n - 1)
                                available_times[i] = (rep_seg_time + (i + start_seg_ind - rep_seg_ind) * seg_length) * 0.9;
                                // available_times[i] = (rep_seg_time + (i + start_seg_ind - rep_seg_ind) * seg_length);
                            else // If last segment (next segment to be downloaded)
                                // available_times[i] = (rep_seg_time + (i + start_seg_ind - rep_seg_ind) * seg_length) * 0.9;
                                if (seg_ind < N_MAX_SEG)
                                    available_times[i] = seg_length * 0.9; // EPIQ paper: rep_seg_time - buffer_threshold + (i + start_seg_ind - rep_seg_ind) * seg_length;
                                else
                                    available_times[i] = seg_length * 0.9;; // EPIQ paper: (rep_seg_time + (i + start_seg_ind - rep_seg_ind) * seg_length) * 0.9;
                            printf("available_times[%zu]: %.3f \n", i, available_times[i]);
                        }
                    }
                    
                    
                    double sol[n_par];
                    
                    int error;
                    
                    lsquic_time_t init_opt_model = lsquic_time_now();
                    
                    if (client_ctx->hcc_cc_reqs_per_conn > 1)
                        isMultiStream = true;
                    
                    // ABR SELECTION
                    if (client_ctx->chosen_abr == 0)
                        error = getMaxQINormCoefficients(N_REP, group_n, n_par, seg_length, (double) t_stats.tot_throughput, seg_bitrates, available_times, min_q, sol, isMultiStream, alpha, beta);
                    else if (client_ctx->chosen_abr == 1)
                        error = getMaxJCoefficients(N_REP, group_n, n_par, seg_length, (double) t_stats.tot_throughput, seg_bitrates, available_times, min_q, sol);
                    else if (client_ctx->chosen_abr == 2)
                        error = getMaxMinJCoefficients(N_REP, group_n, n_par, seg_length, (double) t_stats.tot_throughput, seg_bitrates, available_times, min_q, sol, isMultiStream);
                    else if (client_ctx->chosen_abr == 3)
                        error = getMaxMinJNormCoefficients(N_REP, group_n, n_par, seg_length, (double) t_stats.tot_throughput, seg_bitrates, available_times, min_q, sol, isMultiStream, alpha, beta, buffer_level, buffer_size);
                    else {
                        printf("ERROR: CHOSEN_ABR HAS NO VALID VALUE!\n");
                        return;
                    }
                    
                    printf("Optimization model running time: %.3Lf\n", (long double) (lsquic_time_now() - init_opt_model) / 1000000);
                    
                    if (error == -1){
                        printf("ERROR: OPTIMIZATION ALGORITHM RETURNED -1!\n");
                        return;
                    }
                    
                    unsigned chosen_q[group_n];
                    for (size_t i = 0; i < group_n; i++)
                        chosen_q[i] = min_q[i];
                    
                    double chosen_T[group_n];
                    
                    printf("\nSOLUTIONS:\n");
                    unsigned temp_ind = 0;
                    for (size_t i = 0; i < group_n; i++){
                        for (size_t j = min_q[i]; j < N_REP; j++){
                            if ((unsigned) (sol[temp_ind]) == 1)
                                chosen_q[i] = (int) j;
                            temp_ind++;
                        }
                        if (client_ctx->chosen_abr != 1)
                            chosen_T[i] = sol[n_par - group_n - 1 + i];
                        else
                            chosen_T[i] = sol[n_par - group_n + i];
                    //printf("\n\n");
                    }
                    
                    for (size_t i = 0; i < group_n; i++)
                        printf("Segment %lu: Quality -> %i; Throughput -> %.0f\n", i + start_seg_ind + 1, chosen_q[i], chosen_T[i]);
                    // Print J* [and Q*]
                    if (client_ctx->chosen_abr != 1)
                        printf("J* -> %.0f\n", sol[n_par - 1]);
                    
                    if (seg_ind <= N_MAX_SEG) { // NEW SEGMENTS TO DOWNLOAD
                        // Only send the next segment, don't retransmit
                        ++client_ctx->hcc_still_segments;
                        struct path_elem *pe;
                        pe = calloc(1, sizeof(*pe));
                        next_quality = chosen_q[sizeof(chosen_q)/sizeof(chosen_q[0]) - 1]; // Gather segment chosen quality
                        pe->path = seg_paths[next_quality]; /* Path of the next requested segment */
                        pe->seg_ind = seg_ind;
                        pe->seg_q = next_quality;
                        printf("Downloading seg. %d, rep. %d, path: '%s'\n", seg_ind, next_quality, pe->path);
                        TAILQ_INSERT_TAIL(&client_ctx->hcc_path_elems, pe, next_pe);
                        conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                                client_ctx->hcc_reqs_per_conn);
                        client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
                    }
                    
                    /* Quality check for the chosen re-trans. segments */
                    // if (group_n > 1) {
                        // for (unsigned i = group_n - 2; i > 0; i--) {
                            // if (chosen_q[i] != min_q[i]) {
                                // if (chosen_q[i] > chosen_q[i + 1]) {
                                    // chosen_q[i] = chosen_q[i + 1];
                                // }
                            // }
                        // }
                    // }
                    
                    /* Output of the quality check */
                    // for (size_t i = 0; i < group_n; i++)
                        // printf("Segment %lu: Quality -> %i; Throughput -> %.0f\n", i + start_seg_ind + 1, chosen_q[i], chosen_T[i]);
                    
                    // Segments re-transmission for the buffered segments
                    
                    for (unsigned i = 0; i < group_n - 1; ++i) {
                        if (chosen_q[i] != min_q[i]) { // If the chosen quality is higher than the buffered one (&& at least equal to the subsequent one)
                            //Retransmit
                            ++client_ctx->hcc_still_ret_segments;
                            /* Create path for to-be-re-transmitted segment */
                            int up_len = strlen(FP_PATH) + 4 + strlen(SP_PATH) + strlen(EXT) + 3; // ciphers as index (1,..,999)"
                            char* temp_pp = (char*)malloc((up_len+1)*sizeof(char));
                            snprintf(temp_pp, (up_len+1)*sizeof(char), "%s%d%s%d%s", FP_PATH, seg_bitrates[chosen_q[i]], SP_PATH, i + start_seg_ind + 1, EXT);
                            /* Insert path element in TAILQ */
                            struct path_elem *pe;
                            pe = calloc(1, sizeof(*pe));
                            pe->path = temp_pp; /* Path of the next requested segment */
                            pe->seg_ind = i + start_seg_ind + 1;
                            pe->seg_q = chosen_q[i];
                            printf("Added to the queue: segment index %u, representation %u, segment path '%s'\n", i + start_seg_ind + 1, chosen_q[i], pe->path);
                            TAILQ_INSERT_TAIL(&client_ctx->hcc_ret_path_elems, pe, next_pe);
                            conn_h->ch_n_reqs += MIN(client_ctx->hcc_total_n_reqs,
                                                            client_ctx->hcc_reqs_per_conn);
                            client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
                        }
                    }
                } else if (client_ctx->chosen_abr == 4) { // MaxR select
                    unsigned chosen_q = 0;
                    
                    for (ssize_t i = N_REP - 1; i >= 0; i--) {
                        if ((1 - 0.1) * (double) t_stats.tot_throughput > seg_bitrates[i]) { // 0.1 parameter
                            chosen_q = i;
                            break;
                        }
                    }
                    
                    // If we don't have space in the buffer, wait sometime before sending request for new segment
                    // if (buffer_level > buffer_size){
                        // printf("==> FULL BUFFER! Sleep for %d s\n", (unsigned int) ceil((double) seg_length - (buffer_size - buffer_level)));
                        // sleep((unsigned int) seg_length); // Sleep for x seconds until the buffer level allow new segments download
                    // }
                    
                    ++client_ctx->hcc_still_segments;
                    struct path_elem *pe;
                    pe = calloc(1, sizeof(*pe));
                    next_quality = chosen_q; // Gather segment chosen quality
                    pe->path = seg_paths[next_quality]; /* Path of the next requested segment */
                    pe->seg_ind = seg_ind;
                    pe->seg_q = next_quality;
                    printf("Downloading seg. %d, rep. %d, path: '%s'\n", seg_ind, next_quality, pe->path);
                    TAILQ_INSERT_TAIL(&client_ctx->hcc_path_elems, pe, next_pe);
                    conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                            client_ctx->hcc_reqs_per_conn);
                    client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
                } else if (client_ctx->chosen_abr == 5) { // BOLA - Minh
                    
                    b_m_stats.max_value = 0;
                    b_m_stats.gma = 5.0/seg_length;
                    b_m_stats.SM = DBL_MAX;

                    // get segment size
                    for (int i = 0; i < N_REP; i++) {
                        b_m_stats.Sm[i] = s_stats.W[i][seg_ind];
                    }
                    #if 0
                    b_m_stats.SM = b_m_stats.Sm[N_REP-1]; // get the min segment size    /* DANIELE - Why min segment size is N_REP - 1? */
                    #else
                    b_m_stats.SM = b_m_stats.Sm[0];
                    #endif

                    b_m_stats.V = ((buffer_size*1.0/seg_length)-1.0)/(b_m_stats.Vm[N_REP-1]+(b_m_stats.gma*seg_length));

                    // equation 9
                    for (int i = 0; i < N_REP; i++) {
                        b_m_stats.value[i] = (b_m_stats.V*(b_m_stats.Vm[i] + b_m_stats.gma*seg_length) - (buffer_level*1.0)/seg_length)/b_m_stats.Sm[i];
                    }

                    //choose next_quality_idx that maximize vaWe still need a minimum buffer size 3p for the aWe still need a minimum buffer size 3p for the algorithm to work effectivelylgorithm to work effectivelylue
                    for(int i = 0; i < N_REP; i++) {
                        // skip representations whose objective < 0
                        if (b_m_stats.value[i] < 0) {
                            continue;
                        }

                        if (b_m_stats.value[i] > b_m_stats.max_value)
                        {
                            b_m_stats.max_value = b_m_stats.value[i];
                            b_m_stats.m_star = i;
                        }
                    }
                    // pause for max[p · (Q − Q_d_max + 1), 0]
                    // If we don't have space in the buffer, wait sometime before sending request for new segment
                    // if (buffer_level > buffer_size){
                        // printf("==> FULL BUFFER! Sleep for %d s\n", (unsigned int) ceil((double) seg_length - (buffer_size - buffer_level)));
                        // sleep((unsigned int) seg_length); // Sleep for x seconds until the buffer level allow new segments download
                    // }
                    
                    ++client_ctx->hcc_still_segments;
                    struct path_elem *pe;
                    pe = calloc(1, sizeof(*pe));
                    next_quality = b_m_stats.m_star; // Gather segment chosen quality
                    pe->path = seg_paths[next_quality]; /* Path of the next requested segment */
                    pe->seg_ind = seg_ind;
                    pe->seg_q = next_quality;
                    printf("Downloading seg. %d, rep. %d, path: '%s'\n", seg_ind, next_quality, pe->path);
                    TAILQ_INSERT_TAIL(&client_ctx->hcc_path_elems, pe, next_pe);
                    conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                            client_ctx->hcc_reqs_per_conn);
                    client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
    // Minh - Add BOLA ABR - ADD - E                
                } else if (client_ctx->chosen_abr == 6) { // SARA select
                    
                    unsigned l = 0;
                    s_stats.delta = 0.0;

                    // Minh - ADD - S
                    printf("MInh: H = %.2f\t Buffer level = %.2f\n", s_stats.H, buffer_level);
                    // Minh - ADD - E

                    if (buffer_level > s_stats.I){
                        if (s_stats.W[seg_chosen_q[qualities_ind]][seg_ind] / s_stats.H > buffer_level - s_stats.I) {

                            printf("MInh: W/H = %.2Lf\n", s_stats.W[seg_chosen_q[qualities_ind]][seg_ind] / s_stats.H);
                            
                            for (int i = seg_chosen_q[qualities_ind]; i >= 0; i--) {
                                if (s_stats.W[i][seg_ind] / s_stats.H <= buffer_level - s_stats.I) {
                                    l = (unsigned) i;
                                    break;
                                }
                            }
                        } else if (buffer_level <= s_stats.B_alpha) { // Additive increase
                            printf("Additive Increase!\n");
                            // Minh - MOD - S
                            unsigned qualities_ind_increased = seg_chosen_q[qualities_ind] + 1;
                            
                            if (s_stats.W[qualities_ind_increased][seg_ind] / s_stats.H < buffer_level - s_stats.I)
                                l = qualities_ind_increased;
                            // Minh - MOD - E
                            else
                                l = seg_chosen_q[qualities_ind];
                        } else if (buffer_level <= s_stats.B_beta) { // Aggressive switching
                            printf("Aggressive Switching!\n");
                            l = seg_chosen_q[qualities_ind];
                            for (int i = N_REP - 1; i >= (int) seg_chosen_q[qualities_ind]; i--) {
                                if (s_stats.W[i][seg_ind] / s_stats.H <= buffer_level - s_stats.I) {
                                    printf("W[][] -> %.1Lf, H -> %.3f, W/H -> %.3Lf, buff.lev - I -> %.3f\n", s_stats.W[i][seg_ind], s_stats.H, s_stats.W[i][seg_ind] / s_stats.H, buffer_level - s_stats.I);
                                    l = (unsigned) i;
                                    break;
                                }
                            }
                        } else if (buffer_level > s_stats.B_beta) { // Delayed Download
                            printf("Delayed Download!\n");
                            // Minh - MOD - S
                            l = seg_chosen_q[qualities_ind];
                            // Minh - MOD - E
                            for (int i = N_REP - 1; i >= (int) seg_chosen_q[qualities_ind]; i--) {
                                if (s_stats.W[i][seg_ind] / s_stats.H <= buffer_level - s_stats.B_alpha) {
                                    l = (unsigned) i;
                                    break;
                                }
                            }
                            s_stats.delta = buffer_level - s_stats.B_beta;
                        } else
                            l = seg_chosen_q[qualities_ind];
                    }
                    else {
                        l = 0;
                    }
                    
                    if (l >= N_REP)
                        l = N_REP - 1;
                    
                    printf("l is %u\n", l);
                    
                    printf("MAIN Sleep for %d s\n", (unsigned int) s_stats.delta);
                    sleep((unsigned int) s_stats.delta); // Sleep for x seconds until the buffer level allow new segments download
                    
                    ++client_ctx->hcc_still_segments;
                    struct path_elem *pe;
                    pe = calloc(1, sizeof(*pe));
                    next_quality = l; // Gather segment chosen quality
                    pe->path = seg_paths[next_quality]; /* Path of the next requested segment */
                    pe->seg_ind = seg_ind;
                    pe->seg_q = next_quality;
                    printf("Downloading seg. %d, rep. %d, path: '%s'\n", seg_ind, next_quality, pe->path);
                    TAILQ_INSERT_TAIL(&client_ctx->hcc_path_elems, pe, next_pe);
                    conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                            client_ctx->hcc_reqs_per_conn);
                    client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
                }
                else if (client_ctx->chosen_abr == 7) { // BBA-0 ABR
                    b_b_stats.rS = 0.2 * buffer_size;
                    b_b_stats.cuS = 0.7 * buffer_size;
                    b_b_stats.a = 1.0 * (seg_bitrates[N_REP-1] - seg_bitrates[0])/b_b_stats.cuS;
                    b_b_stats.b = seg_bitrates[0] - b_b_stats.rS * b_b_stats.a;

                    double       f_buff_value = b_b_stats.a * buffer_level + b_b_stats.b; // (kbps)  
                    unsigned     m_selectedQualityIndex = 0;
                    unsigned     m_quality_plus = 0;
                    unsigned     m_quality_subtract = 0;

                    // determine plus quality
                    if (seg_chosen_q[qualities_ind] == N_REP-1)   // if the last selected segment's quality == the highest one
                      m_quality_plus = N_REP-1;
                    else
                      m_quality_plus = seg_chosen_q[qualities_ind]+1;   // the plus quality is the next higher level
                    
                    // determine subtract quality
                    if (seg_chosen_q[qualities_ind] == 0)
                      m_quality_subtract = 0;
                    else
                      m_quality_subtract = seg_chosen_q[qualities_ind]-1; // the subtract quality is the previous lower level
                    
                    // determine the next segment's bittrate
                    if (buffer_level <= b_b_stats.rS){
                      m_selectedQualityIndex = 0;
                    }
                    else if (buffer_level >= (b_b_stats.rS + b_b_stats.cuS)){
                      m_selectedQualityIndex = N_REP-1;
                    }
                    else if (f_buff_value >= seg_bitrates[m_quality_plus]){
                      for (int i = N_REP-1; i >= 0; i--){
                        if (seg_bitrates[i] < f_buff_value){
                          m_selectedQualityIndex = i;
                          break;
                        }
                      }
                    }
                    else if (f_buff_value <= seg_bitrates[m_quality_subtract]){
                      for (int i = 0; i < N_REP; i++){
                        if (seg_bitrates[i] > f_buff_value){
                          m_selectedQualityIndex = i;
                          break;
                        }
                      }
                    }
                    else {
                      m_selectedQualityIndex = seg_chosen_q[qualities_ind];
                    }

                    printf("===================== BBA-0 ==================");
                    printf(" Selected bitrate: %u\n", m_selectedQualityIndex);
                    
                    // printf("Sleep for %d s\n", (unsigned int) s_stats.delta);
                    // sleep((unsigned int) s_stats.delta); // Sleep for x seconds until the buffer level allow new segments download
                    
                    ++client_ctx->hcc_still_segments;
                    struct path_elem *pe;
                    pe = calloc(1, sizeof(*pe));
                    next_quality = m_selectedQualityIndex; // Gather segment chosen quality
                    pe->path = seg_paths[next_quality]; /* Path of the next requested segment */
                    pe->seg_ind = seg_ind;
                    pe->seg_q = next_quality;
                    printf("Downloading seg. %d, rep. %d, path: '%s'\n", seg_ind, next_quality, pe->path);
                    TAILQ_INSERT_TAIL(&client_ctx->hcc_path_elems, pe, next_pe);
                    conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                            client_ctx->hcc_reqs_per_conn);
                    client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
                }
                // H2BR RETRANSMISSION
                if (client_ctx->h2br) {
                    // Throughput T^e from PAPER
                    // ...
                    h_stats.T_e = t_stats.tot_throughput;
                    // seg_ind is the next segment index
                    // seg_bitrates[next_quality] is the chosen bitrate for next segment
                    //if (buffer_level >= min_init_bs) has already been checked
                    if (h_stats.T_e > seg_bitrates[next_quality]) {
                        printf("===================== H2BR STRATEGY ==================");
                        double buffer_estim = 0.0;
                        int start_group = -1;
                        int end_group = -1;
                        bool retransmit = true;
                        unsigned first_quality = 0U;
                        unsigned second_quality = 0U;
                        unsigned ret_segments = 0U;
                        unsigned group_quality = 0U;
                        unsigned quality_levels[seg_ind - rep_seg_ind - 1];
                        printf("\nQuality level: [ ");
                        for (unsigned i = 0; i < seg_ind - rep_seg_ind - 1; i++) {
                            quality_levels[i] = seg_chosen_q[rep_seg_ind + i];
                            printf("%i ", quality_levels[i]);
                        }
                        printf("]\n");
                        // Check beginning quality value
                        first_quality = seg_chosen_q[rep_seg_ind - 1];
                        if (quality_levels[0] < seg_chosen_q[rep_seg_ind - 1]) {
                            start_group = 0;
                        }
                        // } else if (quality_levels[0] == seg_chosen_q[rep_seg_ind - 1] && seg_chosen_q[rep_seg_ind - 1] + 1 < N_REP) {
                            // first_quality = seg_chosen_q[rep_seg_ind - 1] + 1;
                            // start_group = 0;
                        // }
                        
                        unsigned start_search = 0U;
                        find_groups:
                        // Check groups and conditions
                        for (unsigned i = start_search; i < seg_ind - rep_seg_ind - 2; i++) {
                            printf("\n q[i]: %u - q[i+1]: %u", quality_levels[i], quality_levels[i+1]);
                            if (start_group != -1 && end_group != -1)
                                break;
                            if (quality_levels[i] > quality_levels[i + 1]) {
                                // if (start_group == 0) {
                                    // end_group = i;
                                    // second_quality = quality_levels[i] + 1;
                                // } else if (start_group == -1) {
                                    // start_group = i + 1;
                                    // first_quality = quality_levels[i];
                                // } else {
                                    // end_group = i;
                                    // second_quality = quality_levels[i] + 1;
                                // }
                                start_group = i + 1;
                                first_quality = quality_levels[i];
                            } else if (quality_levels[i] < quality_levels[i + 1]) {
                                if (start_group != -1) {
                                    end_group = i;
                                    second_quality = quality_levels[i + 1];
                                }
                            }
                        }
                        // Check the group indexes
                        if (start_group == -1) {
                            retransmit = false;
                        } else if (end_group == -1) {
                            if (quality_levels[seg_ind - rep_seg_ind - 2] < next_quality) {
                                end_group = seg_ind - rep_seg_ind - 2;
                                second_quality = next_quality;
                            } else
                                retransmit = false;
                        }
                        // } else if (start_group > end_group) {
                            // end_group = start_group;
                        // }
                        
                        // Indexes are set
                        if (retransmit) {
                            // If the right extrem quality value is lower than the group quality we set it to the group quality + 1
                            if (second_quality <= quality_levels[start_group]) {
                                second_quality = quality_levels[start_group] + 1;
                            }
                            printf("\nStart group: %i, End group: %i, retransmit: %d, first quality: %u, second quality: %u!\n", start_group, end_group, retransmit, first_quality, second_quality);
                            //printf("\nTEST SEG. FALT 1\n");
                            unsigned n_segments = end_group - start_group + 1;
                            // Try first with minimum of adjacent quality values
                            group_quality = MIN(first_quality, second_quality);
                            double available_times[n_segments];
                            for (size_t i = 0; i < n_segments; i++)
                                available_times[i] = (rep_seg_time + (i + start_group) * seg_length);
                            //printf("\nTEST SEG. FALT 2\n");
                            long double T_r[n_segments];
                            bool break_loop = false;
                            //printf("\nTEST SEG. FALT 3\n");
                            for (unsigned k = MIN(first_quality, second_quality); k <= MAX(first_quality, second_quality); k++) {
                                for (unsigned i = 0; i < n_segments; i++) {
                                    long double split_throughput = h_stats.T_e/(i + 1 + 1); // i + 1 ret. segments + 1 next segment
                                    buffer_estim = buffer_level + seg_length - (seg_length * seg_bitrates[next_quality] + (i + 1) * seg_length * seg_bitrates[k])/h_stats.T_e;
                                    if (buffer_estim < min_init_bs){
                                        break_loop = true;
                                    }
                                    for (unsigned j = 0; j < i + 1; j++) {
                                        T_r[j] = seg_bitrates[k] * seg_length / available_times[j];
                                        if (T_r[j] > split_throughput) { // Not enough throughput for retransmission
                                            break_loop = true;
                                        }
                                    }
                                    if (break_loop) {
                                        break_loop = false;
                                        break;
                                    }
                                    else {
                                        if (ret_segments <= i + 1) { // If actual ret_segments is lower or equal than the new number of segments to be pushed at quality k, go for it
                                            ret_segments = i + 1;
                                            group_quality = k;
                                            printf("Conditions satisfied for %u ret_segments -> buffer_estim: %.3f, split_throughput: %.3Lf", ret_segments, buffer_estim, split_throughput);
                                            for (unsigned j = 0; j < i + 1; j++)
                                                printf(", T_r[%u]: %.3Lf", j, T_r[j]);
                                            printf("\n");
                                        }
                                    }
                                }
                            }
                            
                            // Check next group if ret_segments is 0 and other groups are available
                            if (ret_segments == 0 && end_group < seg_ind - rep_seg_ind - 2) {
                                first_quality = quality_levels[end_group];
                                start_search = end_group + 1;
                                start_group = -1;
                                end_group = -1;
                                goto find_groups;
                            }
                            //printf("\nTEST SEG. FALT 4\n");
                            
                            if (ret_segments == 0 && group_quality == 0)
                                printf("!!Problems in assessing the group quality and number of segments to be pushed!!\n");
                            // Add push segments to the queue
                            // Retransmit
                            for (unsigned int s = 0; s < ret_segments; s++) {
                                ++client_ctx->hcc_still_ret_segments;
                                /* Create path for to-be-re-transmitted segment */
                                int up_len = strlen(FP_PATH) + 4 + strlen(SP_PATH) + strlen(EXT) + 3; // ciphers as index (1,..,999)"
                                char* temp_pp = (char*)malloc((up_len+1)*sizeof(char));
                                snprintf(temp_pp, (up_len+1)*sizeof(char), "%s%d%s%d%s", FP_PATH, seg_bitrates[group_quality], SP_PATH, rep_seg_ind + start_group + s + 1, EXT);
                                /* Insert path element in TAILQ */
                                struct path_elem *pe;
                                pe = calloc(1, sizeof(*pe));
                                pe->path = temp_pp; /* Path of the next requested segment */
                                pe->seg_ind = rep_seg_ind + start_group + s + 1;
                                pe->seg_q = group_quality;
                                printf("Added to the queue: segment index %u, representation %u, segment path '%s'\n", rep_seg_ind + start_group + s + 1, group_quality, pe->path);
                                TAILQ_INSERT_TAIL(&client_ctx->hcc_ret_path_elems, pe, next_pe);
                                conn_h->ch_n_reqs += MIN(client_ctx->hcc_total_n_reqs,
                                                                client_ctx->hcc_reqs_per_conn);
                                client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
                            }
                        }
                    }
                }
            } else { // Transmit only new segment with lowest resolution
                ++client_ctx->hcc_still_segments;
                struct path_elem *pe;
                pe = calloc(1, sizeof(*pe));
                pe->path = seg_paths[0]; /* Path of the next requested segment */
                pe->seg_ind = seg_ind;
                pe->seg_q = 0;
                printf("Lowest representation, segment path: '%s'\n", pe->path);
                TAILQ_INSERT_TAIL(&client_ctx->hcc_path_elems, pe, next_pe);
                conn_h->ch_n_reqs = MIN(client_ctx->hcc_total_n_reqs,
                                                        client_ctx->hcc_reqs_per_conn);
                client_ctx->hcc_total_n_reqs -= conn_h->ch_n_reqs;
            }
        } else { /* CLOSE CONN IF THERE IS NO PLACE FOR IMPROVEMENT */
            printf("Closing connection!\n");
            client_ctx->hcc_total_n_reqs = 0;
            lsquic_conn_close(conn_h->conn);
            printf("After closing connection!\n");
            return;
        }
        t_stats.tot_throughput = 0.0; // re-initialization total throughput;
    }
    
    /* End new request for segment */
    if (0 == conn_h->ch_n_reqs)
    {
        // printf("all requests completed, closing connection");
        LSQ_INFO("all requests completed, closing connection");
        lsquic_conn_close(conn_h->conn);
    }
    else
    {
        LSQ_INFO("%u active stream, %u request remain, creating %u new stream",
            conn_h->ch_n_cc_streams,
            conn_h->ch_n_reqs - conn_h->ch_n_cc_streams,
            MIN((conn_h->ch_n_reqs - conn_h->ch_n_cc_streams),
                (client_ctx->hcc_cc_reqs_per_conn - conn_h->ch_n_cc_streams)));
        /* CHECK ON TRANSMISSION AND RE-TRANSMISSION QUEUES */
        printf("SRS: %u, SS: %u\n", client_ctx->hcc_still_ret_segments, client_ctx->hcc_still_segments);
        if (client_ctx->hcc_cc_reqs_per_conn > 1) {
            if (!client_ctx->hcc_open_streams) {
                printf("\n==> Transmission Time <==\n");
                create_streams(client_ctx, conn_h); // Open the transmission stream
            }
        } else {
            if (client_ctx->hcc_still_segments) {           // Check if next stream is re-transmission and - if so - whether it is possible or not
                printf("Transmission\n");
                create_streams(client_ctx, conn_h); // Open the transmission stream
            } else if (client_ctx->hcc_still_ret_segments) {
                printf("\nRe-transmission path update\n");
                struct path_elem *temp_pe;
                temp_pe = calloc(1, sizeof(*temp_pe));
                if (client_ctx->hcc_ret_pe)
                    temp_pe = TAILQ_NEXT(client_ctx->hcc_ret_pe, next_pe);
                else
                    temp_pe = TAILQ_FIRST(&client_ctx->hcc_ret_path_elems);
                if (!temp_pe) { // If it's the last element of the queue
                    printf("\nGoing through ABR again\n");
                    goto abr; // Re-execute ABR strategy
                }
                client_ctx->hcc_ret_pe = temp_pe;
                double available_time = 0.0;
                if (client_ctx->hcc_ret_pe->seg_ind > rep_seg_ind)
                    available_time = rep_seg_time + (client_ctx->hcc_ret_pe->seg_ind - rep_seg_ind - 1) * seg_length;
                while(available_time < (seg_bitrates[client_ctx->hcc_ret_pe->seg_q] * seg_length / t_stats.tot_throughput)){
                    --client_ctx->hcc_still_ret_segments;
                    if ((temp_pe = TAILQ_NEXT(client_ctx->hcc_ret_pe, next_pe))){
                        client_ctx->hcc_ret_pe = temp_pe;
                        available_time = rep_seg_time + (client_ctx->hcc_ret_pe->seg_ind - rep_seg_ind) * seg_length;
                    } else {
                        printf("\nGoing through ABR again\n");
                        goto abr;
                    }
                }
                create_streams(client_ctx, conn_h); // Open the re-transmission stream
            }
        }
        /* END CHECK */
    }
    if (st_h->reader.lsqr_ctx)
        destroy_lsquic_reader_ctx(st_h->reader.lsqr_ctx);
    if (st_h->download_fh)
        fclose(st_h->download_fh);
    free(st_h);
}


static struct lsquic_stream_if http_client_if = {
    .on_new_conn            = http_client_on_new_conn,
    .on_conn_closed         = http_client_on_conn_closed,
    .on_new_stream          = http_client_on_new_stream,
    .on_read                = http_client_on_read,
    .on_write               = http_client_on_write,
    .on_close               = http_client_on_close,
    .on_hsk_done            = http_client_on_hsk_done,
};


/* XXX This function assumes we can send the request in one shot.  This is
 * not a realistic assumption to make in general, but will work for our
 * limited use case (QUIC Interop Runner).
 */
static void
hq_client_on_write (struct lsquic_stream *stream, lsquic_stream_ctx_t *st_h)
{
    if (st_h->client_ctx->payload)
    {
        LSQ_ERROR("payload is not supported in HQ client");
        lsquic_stream_close(stream);
        return;
    }

    lsquic_stream_write(stream, "GET ", 4);
    lsquic_stream_write(stream, st_h->path, strlen(st_h->path));
    lsquic_stream_write(stream, "\r\n", 2);
    lsquic_stream_shutdown(stream, 1);
    lsquic_stream_wantread(stream, 1);
}


static size_t
hq_client_print_to_file (void *user_data, const unsigned char *buf,
                                                size_t buf_len, int fin_unused)
{
    fwrite(buf, 1, buf_len, user_data);
    return buf_len;
}


static void
hq_client_on_read (struct lsquic_stream *stream, lsquic_stream_ctx_t *st_h)
{
    FILE *out = st_h->download_fh ? st_h->download_fh : stdout;
    ssize_t nread;

    nread = lsquic_stream_readf(stream, hq_client_print_to_file, out);
    if (nread <= 0)
    {
        if (nread < 0)
            LSQ_WARN("error reading response for %s: %s", st_h->path,
                                                        strerror(errno));
        lsquic_stream_close(stream);
    }
}


/* The "hq" set of callbacks differs only in the read and write routines */
static struct lsquic_stream_if hq_client_if = {
    .on_new_conn            = http_client_on_new_conn,
    .on_conn_closed         = http_client_on_conn_closed,
    .on_new_stream          = http_client_on_new_stream,
    .on_read                = hq_client_on_read,
    .on_write               = hq_client_on_write,
    .on_close               = http_client_on_close,
};


static void
usage (const char *prog)
{
    const char *const slash = strrchr(prog, '/');
    if (slash)
        prog = slash + 1;
    printf(
"Usage: %s [opts]\n"
"\n"
"Options:\n"
"   -p PATH     Path to request.  May be specified more than once.  If no\n"
"                 path is specified, the connection is closed as soon as\n"
"                 handshake succeeds.\n"
"   -n CONNS    Number of concurrent connections.  Defaults to 1.\n"
"   -r NREQS    Total number of requests to send.  Defaults to 1.\n"
"   -R MAXREQS  Maximum number of requests per single connection.  Some\n"
"                 connections will have fewer requests than this.\n"
"   -w CONCUR   Number of concurrent requests per single connection.\n"
"                 Defaults to 1.\n"
"   -M METHOD   Method.  Defaults to GET.\n"
"   -P PAYLOAD  Name of the file that contains payload to be used in the\n"
"                 request.  This adds two more headers to the request:\n"
"                 content-type: application/octet-stream and\n"
"                 content-length\n"
"   -K          Discard server response\n"
"   -I          Abort on incomplete reponse from server\n"
"   -4          Prefer IPv4 when resolving hostname\n"
"   -6          Prefer IPv6 when resolving hostname\n"
#ifndef WIN32
"   -C DIR      Certificate store.  If specified, server certificate will\n"
"                 be verified.\n"
#endif
"   -a          Display server certificate chain after successful handshake.\n"
"   -b N_BYTES  Send RESET_STREAM frame after the client has read n bytes.\n"
"   -t          Print stats to stdout.\n"
"   -T FILE     Print stats to FILE.  If FILE is -, print stats to stdout.\n"
"   -q FILE     QIF mode: issue requests from the QIF file and validate\n"
"                 server responses.\n"
"   -e TOKEN    Hexadecimal string representing resume token.\n"
"   -3 MAX      Close stream after reading at most MAX bytes.  The actual\n"
"                 number of bytes read is randominzed.\n"
"   -9 SPEC     Priority specification.  May be specified several times.\n"
"                 SPEC takes the form stream_id:nread:UI, where U is\n"
"                 urgency and I is incremental.  Matched \\d+:\\d+:[0-7][01]\n"
"   -7 DIR      Save fetched resources into this directory.\n"
"   -Q ALPN     Use hq ALPN.  Specify, for example, \"h3-29\".\n"
            , prog);
}


#ifndef WIN32
static X509_STORE *store;

/* Windows does not have regex... */
static int
ends_in_pem (const char *s)
{
    int len;

    len = strlen(s);

    return len >= 4
        && 0 == strcasecmp(s + len - 4, ".pem");
}


static X509 *
file2cert (const char *path)
{
    X509 *cert = NULL;
    BIO *in;

    in = BIO_new(BIO_s_file());
    if (!in)
        goto end;

    if (BIO_read_filename(in, path) <= 0)
        goto end;

    cert = PEM_read_bio_X509_AUX(in, NULL, NULL, NULL);

  end:
    BIO_free(in);
    return cert;
}


static int
init_x509_cert_store (const char *path)
{
    struct dirent *ent;
    X509 *cert;
    DIR *dir;
    char file_path[NAME_MAX];
    int ret;

    dir = opendir(path);
    if (!dir)
    {
        LSQ_WARN("Cannot open directory `%s': %s", path, strerror(errno));
        return -1;
    }

    store = X509_STORE_new();

    while ((ent = readdir(dir)))
    {
        if (ends_in_pem(ent->d_name))
        {
            ret = snprintf(file_path, sizeof(file_path), "%s/%s",
                                                            path, ent->d_name);
            if (ret < 0)
            {
                LSQ_WARN("file_path formatting error %s", strerror(errno));
                continue;
            }
            else if ((unsigned)ret >= sizeof(file_path))
            {
                LSQ_WARN("file_path was truncated %s", strerror(errno));
                continue;
            }
            cert = file2cert(file_path);
            if (cert)
            {
                if (1 != X509_STORE_add_cert(store, cert))
                    LSQ_WARN("could not add cert from %s", file_path);
            }
            else
                LSQ_WARN("could not read cert from %s", file_path);
        }
    }
    (void) closedir(dir);
    return 0;
}


static int
verify_server_cert (void *ctx, STACK_OF(X509) *chain)
{
    X509_STORE_CTX *store_ctx = NULL;
    X509 *cert;
    int ver;

    if (!store)
    {
        if (0 != init_x509_cert_store(ctx))
            return -1;
    }

    cert = sk_X509_shift(chain);
    X509_STORE_CTX_init(&store_ctx, store, cert, chain);

    ver = X509_verify_cert(&store_ctx);

    X509_STORE_CTX_cleanup(&store_ctx);

    if (ver != 1)
        LSQ_WARN("could not verify server certificate");

    return ver == 1 ? 0 : -1;
}
#endif


static void *
hset_create (void *hsi_ctx, lsquic_stream_t *stream, int is_push_promise)
{
    struct hset *hset;

    if ((hset = malloc(sizeof(*hset))))
    {
        STAILQ_INIT(hset);
        return hset;
    }
    else
        return NULL;
}


static struct lsxpack_header *
hset_prepare_decode (void *hset_p, struct lsxpack_header *xhdr,
                                                        size_t req_space)
{
    struct hset *const hset = hset_p;
    struct hset_elem *el;
    char *buf;

    if (0 == req_space)
        req_space = 0x100;

    if (req_space > LSXPACK_MAX_STRLEN)
    {
        LSQ_WARN("requested space for header is too large: %zd bytes",
                                                                    req_space);
        return NULL;
    }

    if (!xhdr)
    {
        buf = malloc(req_space);
        if (!buf)
        {
            LSQ_WARN("cannot allocate buf of %zd bytes", req_space);
            return NULL;
        }
        el = malloc(sizeof(*el));
        if (!el)
        {
            LSQ_WARN("cannot allocate hset_elem");
            free(buf);
            return NULL;
        }
        STAILQ_INSERT_TAIL(hset, el, next);
        lsxpack_header_prepare_decode(&el->xhdr, buf, 0, req_space);
        el->nalloc = req_space;
    }
    else
    {
        el = (struct hset_elem *) ((char *) xhdr
                                        - offsetof(struct hset_elem, xhdr));
        if (req_space <= el->nalloc)
        {
            LSQ_ERROR("requested space is smaller than already allocated");
            return NULL;
        }
        if (req_space < el->nalloc * 2)
            req_space = el->nalloc * 2;
        buf = realloc(el->xhdr.buf, req_space);
        if (!buf)
        {
            LSQ_WARN("cannot reallocate hset buf");
            return NULL;
        }
        el->xhdr.buf = buf;
        el->xhdr.val_len = req_space;
        el->nalloc = req_space;
    }

    return &el->xhdr;
}


static int
hset_add_header (void *hset_p, struct lsxpack_header *xhdr)
{
    unsigned name_len, value_len;
    /* Not much to do: the header value are in xhdr */

    if (xhdr)
    {
        name_len = xhdr->name_len;
        value_len = xhdr->val_len;
        s_stat_downloaded_bytes += name_len + value_len + 4;    /* ": \r\n" */
    }
    else
        s_stat_downloaded_bytes += 2;   /* \r\n "*/

    return 0;
}


static void
hset_destroy (void *hset_p)
{
    struct hset *hset = hset_p;
    struct hset_elem *el, *next;

    for (el = STAILQ_FIRST(hset); el; el = next)
    {
        next = STAILQ_NEXT(el, next);
        free(el->xhdr.buf);
        free(el);
    }
    free(hset);
}


static void
hset_dump (const struct hset *hset, FILE *out)
{
    const struct hset_elem *el;

    STAILQ_FOREACH(el, hset, next)
        if (el->xhdr.flags & (LSXPACK_HPACK_VAL_MATCHED|LSXPACK_QPACK_IDX))
            fprintf(out, "%.*s (%s static table idx %u): %.*s\n",
                (int) el->xhdr.name_len, lsxpack_header_get_name(&el->xhdr),
                el->xhdr.flags & LSXPACK_HPACK_VAL_MATCHED ? "hpack" : "qpack",
                el->xhdr.flags & LSXPACK_HPACK_VAL_MATCHED ? el->xhdr.hpack_index
                                                    : el->xhdr.qpack_index,
                (int) el->xhdr.val_len, lsxpack_header_get_value(&el->xhdr));
        else
            fprintf(out, "%.*s: %.*s\n",
                (int) el->xhdr.name_len, lsxpack_header_get_name(&el->xhdr),
                (int) el->xhdr.val_len, lsxpack_header_get_value(&el->xhdr));

    fprintf(out, "\n");
    fflush(out);
}


/* These are basic and for illustration purposes only.  You will want to
 * do your own verification by doing something similar to what is done
 * in src/liblsquic/lsquic_http1x_if.c
 */
static const struct lsquic_hset_if header_bypass_api =
{
    .hsi_create_header_set  = hset_create,
    .hsi_prepare_decode     = hset_prepare_decode,
    .hsi_process_header     = hset_add_header,
    .hsi_discard_header_set = hset_destroy,
};


static void
display_stat (FILE *out, const struct sample_stats *stats, const char *name)
{
    long double mean, stddev;

    calc_sample_stats(stats, &mean, &stddev);
    fprintf(out, "%s: n: %u; min: %.2Lf ms; max: %.2Lf ms; mean: %.2Lf ms; "
        "sd: %.2Lf ms\n", name, stats->n, (long double) stats->min / 1000,
        (long double) stats->max / 1000, mean / 1000, stddev / 1000);
}


static lsquic_conn_ctx_t *
qif_client_on_new_conn (void *stream_if_ctx, lsquic_conn_t *conn)
{
    lsquic_conn_make_stream(conn);
    return stream_if_ctx;
}


static void
qif_client_on_conn_closed (lsquic_conn_t *conn)
{
    struct http_client_ctx *client_ctx = (void *) lsquic_conn_get_ctx(conn);
    LSQ_INFO("connection is closed: stop engine");
    prog_stop(client_ctx->prog);
}


struct qif_stream_ctx
{
    int                         reqno;
    struct lsquic_http_headers  headers;
    char                       *qif_str;
    size_t                      qif_sz;
    size_t                      qif_off;
    char                       *resp_str;   /* qif_sz allocated */
    size_t                      resp_off;   /* Read so far */
    enum {
        QSC_HEADERS_SENT = 1 << 0,
        QSC_GOT_HEADERS  = 1 << 1,
    }                           flags;
};

#define MAX(a, b) ((a) > (b) ? (a) : (b))

lsquic_stream_ctx_t *
qif_client_on_new_stream (void *stream_if_ctx, lsquic_stream_t *stream)
{
    struct http_client_ctx *const client_ctx = stream_if_ctx;
    FILE *const fh = client_ctx->qif_fh;
    struct qif_stream_ctx *ctx;
    struct lsxpack_header *header;
    static int reqno;
    size_t nalloc;
    char *end, *tab, *line;
    char line_buf[0x1000];

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        perror("calloc");
        exit(1);
    }
    ctx->reqno = reqno++;

    nalloc = 0;
    while ((line = fgets(line_buf, sizeof(line_buf), fh)))
    {
        end = strchr(line, '\n');
        if (!end)
        {
            fprintf(stderr, "no newline\n");
            exit(1);
        }

        if (end == line)
            break;

        if (*line == '#')
            continue;

        tab = strchr(line, '\t');
        if (!tab)
        {
            fprintf(stderr, "no TAB\n");
            exit(1);
        }

        if (nalloc + (end + 1 - line) > ctx->qif_sz)
        {
            if (nalloc)
                nalloc = MAX(nalloc * 2, nalloc + (end + 1 - line));
            else
                nalloc = end + 1 - line;
            ctx->qif_str = realloc(ctx->qif_str, nalloc);
            if (!ctx->qif_str)
            {
                perror("realloc");
                exit(1);
            }
        }
        memcpy(ctx->qif_str + ctx->qif_sz, line, end + 1 - line);

        ctx->headers.headers = realloc(ctx->headers.headers,
                sizeof(ctx->headers.headers[0]) * (ctx->headers.count + 1));
        if (!ctx->headers.headers)
        {
            perror("realloc");
            exit(1);
        }
        header = &ctx->headers.headers[ctx->headers.count++];
        lsxpack_header_set_offset2(header, ctx->qif_str + ctx->qif_sz, 0,
                                    tab - line, tab - line + 1, end - tab - 1);
        ctx->qif_sz += end + 1 - line;
    }

    lsquic_stream_wantwrite(stream, 1);

    if (!line)
    {
        LSQ_DEBUG("Input QIF file ends; close file handle");
        fclose(client_ctx->qif_fh);
        client_ctx->qif_fh = NULL;
    }

    return (void *) ctx;
}


static void
qif_client_on_write (struct lsquic_stream *stream, lsquic_stream_ctx_t *h)
{
    struct qif_stream_ctx *const ctx = (void *) h;
    size_t towrite;
    ssize_t nw;

    if (ctx->flags & QSC_HEADERS_SENT)
    {
        towrite = ctx->qif_sz - ctx->qif_off;
        nw = lsquic_stream_write(stream, ctx->qif_str + ctx->qif_off, towrite);
        if (nw >= 0)
        {
            LSQ_DEBUG("wrote %zd bytes to stream", nw);
            ctx->qif_off += nw;
            if (ctx->qif_off == (size_t) nw)
            {
                lsquic_stream_shutdown(stream, 1);
                lsquic_stream_wantread(stream, 1);
                LSQ_DEBUG("finished writing request %d", ctx->reqno);
            }
        }
        else
        {
            LSQ_ERROR("cannot write to stream: %s", strerror(errno));
            lsquic_stream_wantwrite(stream, 0);
            lsquic_conn_abort(lsquic_stream_conn(stream));
        }
    }
    else
    {
        if (0 == lsquic_stream_send_headers(stream, &ctx->headers, 0))
        {
            ctx->flags |= QSC_HEADERS_SENT;
            LSQ_DEBUG("sent headers");
        }
        else
        {
            LSQ_ERROR("cannot send headers: %s", strerror(errno));
            lsquic_stream_wantwrite(stream, 0);
            lsquic_conn_abort(lsquic_stream_conn(stream));
        }
    }
}


static void
qif_client_on_read (struct lsquic_stream *stream, lsquic_stream_ctx_t *h)
{
    struct qif_stream_ctx *const ctx = (void *) h;
    struct hset *hset;
    ssize_t nr;
    unsigned char buf[1];

    LSQ_DEBUG("reading response to request %d", ctx->reqno);

    if (!(ctx->flags & QSC_GOT_HEADERS))
    {
        hset = lsquic_stream_get_hset(stream);
        if (!hset)
        {
            LSQ_ERROR("could not get header set from stream");
            exit(2);
        }
        LSQ_DEBUG("got header set for response %d", ctx->reqno);
        hset_dump(hset, stdout);
        hset_destroy(hset);
        ctx->flags |= QSC_GOT_HEADERS;
    }
    else
    {
        if (!ctx->resp_str)
        {
            ctx->resp_str = malloc(ctx->qif_sz);
            if (!ctx->resp_str)
            {
                perror("malloc");
                exit(1);
            }
        }
        if (ctx->resp_off < ctx->qif_sz)
        {
            nr = lsquic_stream_read(stream, ctx->resp_str + ctx->resp_off,
                                        ctx->qif_sz - ctx->resp_off);
            if (nr > 0)
            {
                ctx->resp_off += nr;
                LSQ_DEBUG("read %zd bytes of reponse %d", nr, ctx->reqno);
            }
            else if (nr == 0)
            {
                LSQ_INFO("response %d too short", ctx->reqno);
                LSQ_WARN("response %d FAIL", ctx->reqno);
                lsquic_stream_shutdown(stream, 0);
            }
            else
            {
                LSQ_ERROR("error reading from stream");
                lsquic_stream_wantread(stream, 0);
                lsquic_conn_abort(lsquic_stream_conn(stream));
            }
        }
        else
        {
            /* Collect EOF */
            nr = lsquic_stream_read(stream, buf, sizeof(buf));
            if (nr == 0)
            {
                if (0 == memcmp(ctx->qif_str, ctx->resp_str, ctx->qif_sz))
                    LSQ_INFO("response %d OK", ctx->reqno);
                else
                    LSQ_WARN("response %d FAIL", ctx->reqno);
                lsquic_stream_shutdown(stream, 0);
            }
            else if (nr > 0)
            {
                LSQ_INFO("response %d too long", ctx->reqno);
                LSQ_WARN("response %d FAIL", ctx->reqno);
                lsquic_stream_shutdown(stream, 0);
            }
            else
            {
                LSQ_ERROR("error reading from stream");
                lsquic_stream_shutdown(stream, 0);
                lsquic_conn_abort(lsquic_stream_conn(stream));
            }
        }
    }
}


static void
qif_client_on_close (struct lsquic_stream *stream, lsquic_stream_ctx_t *h)
{
    struct lsquic_conn *conn = lsquic_stream_conn(stream);
    struct http_client_ctx *client_ctx = (void *) lsquic_conn_get_ctx(conn);
    struct qif_stream_ctx *const ctx = (void *) h;
    free(ctx->qif_str);
    free(ctx->resp_str);
    free(ctx->headers.headers);
    free(ctx);
    if (client_ctx->qif_fh)
        lsquic_conn_make_stream(conn);
    else
        lsquic_conn_close(conn);
}


const struct lsquic_stream_if qif_client_if = {
    .on_new_conn            = qif_client_on_new_conn,
    .on_conn_closed         = qif_client_on_conn_closed,
    .on_new_stream          = qif_client_on_new_stream,
    .on_read                = qif_client_on_read,
    .on_write               = qif_client_on_write,
    .on_close               = qif_client_on_close,
};

int
main (int argc, char **argv)
{
    int opt, s, was_empty;
    lsquic_time_t start_time;
    FILE *stats_fh = NULL;
    long double elapsed;
    struct http_client_ctx client_ctx;
    struct stat st;
    struct path_elem *pe;
    struct sport_head sports;
    struct prog prog;
    const char *token = NULL;
    struct priority_spec *priority_specs = NULL;
    stall_t = lsquic_time_now();
    // seg_chosen_q[0] = 0; // The quality of the first downloaded segment
    // SARA stats
    s_stats.I = seg_length;
    s_stats.B_alpha = 2 * seg_length;
    s_stats.B_beta = 5 * seg_length;
    // BOLA stats
    b_m_stats.gma = 5.0/seg_length;
    b_m_stats.SM = DBL_MAX;
    b_m_stats.max_value = 0;
    w_stats.re_data = 0.0;
    w_stats.re_count = 0U;

    int experiment_id = 0;
    
    // Initialization of segments qualities
    for (unsigned i = 0; i < sizeof(seg_chosen_q) / sizeof(seg_chosen_q[0]); ++i){
        seg_chosen_q[i] = -1;
    }
    // Initialization of re-transmitted segments qualities
    // for (unsigned i = 1; i < sizeof(re_seg_chosen_q) / sizeof(re_seg_chosen_q[0]); ++i){
        // re_seg_chosen_q[i] = -1;
    // }

// Minh - Add BOLA ABR - ADD - S
    // Initialization of v_m values for BOLA
    printf("====> Get BOLA v_m\n");
    for (unsigned i = 0; i < N_REP; ++i){
        b_m_stats.Vm[i] = log((double) seg_bitrates[i] / seg_bitrates[0]);
    }
// Minh - Add BOLA ABR - ADD - E    
    // Initialization of W values for SARA
    weights_init(s_stats.W, WEIGHTS_FILENAME);
    
    // for (ssize_t i = 0; i < N_REP; i++) {
        // for (ssize_t j = 0; j < N_MAX_SEG; j++) {
            // printf("%Lf ", s_stats.W[i][j]);
        // }
        // printf("\n");
    // }

    TAILQ_INIT(&sports);
    memset(&client_ctx, 0, sizeof(client_ctx));
    TAILQ_INIT(&client_ctx.hcc_path_elems);
    TAILQ_INIT(&client_ctx.hcc_ret_path_elems);
    client_ctx.method = "GET";
    client_ctx.hcc_concurrency = 1;
    client_ctx.hcc_cc_reqs_per_conn = 1;
    client_ctx.hcc_reqs_per_conn = 1;
    client_ctx.hcc_total_n_reqs = 1;
    client_ctx.hcc_reset_after_nbytes = 0;
    client_ctx.hcc_retire_cid_after_nbytes = 0;
    client_ctx.hcc_still_segments = 1U;
    client_ctx.hcc_open_ret_streams = 0U;
    client_ctx.hcc_still_ret_segments = 0U;
    client_ctx.prog = &prog;
    client_ctx.h2br = false;
    client_ctx.chosen_abr = 1;

    printf("====> 1\n");

    prog_init(&prog, LSENG_HTTP, &sports, &http_client_if, &client_ctx);

    while (-1 != (opt = getopt(argc, argv, PROG_OPTS
                                    ":J:Z:46Br:R:IKu:EP:M:n:w:H:p:0:q:e:hatT:b:d"
                            "3:"    /* 3 is 133+ for "e" ("e" for "early") */
                            "9:"    /* 9 sort of looks like P... */
                            "7:"    /* Download directory */
                            "Q:"    /* ALPN, e.g. h3-29 */
#ifndef WIN32
                                                                      "C:"
#endif
                                                                            )))
    {
        switch (opt) {
        case 'a':
            ++s_display_cert_chain;
            break;
        case 'J':
            client_ctx.chosen_abr = atoi(optarg);
            break;
        case 'Z':
                if (atoi(optarg) == 1){
                	client_ctx.h2br = true;
                	printf("===== H2BR is enable ====\n");
                }
            break;
        case '4':
        case '6':
            prog.prog_ipver = opt - '0';
            break;
        case 'B':
            g_header_bypass = 1;
            prog.prog_api.ea_hsi_if = &header_bypass_api;
            prog.prog_api.ea_hsi_ctx = NULL;
            break;
        case 'I':
            client_ctx.hcc_flags |= HCC_ABORT_ON_INCOMPLETE;
            break;
        case 'K':
            ++s_discard_response;
            break;
        case 'u':   /* Accept p<U>sh promise */
            promise_fd = open(optarg, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (promise_fd < 0)
            {
                perror("open");
                exit(1);
            }
            prog.prog_settings.es_support_push = 1;     /* Pokes into prog */
            break;
        case 'E':   /* E: randomly reprioritize str<E>ams.  Now, that's
                     * pretty random. :)
                     */
            srand((uintptr_t) argv);
            randomly_reprioritize_streams = 1;
            break;
        case 'n':
            client_ctx.hcc_concurrency = atoi(optarg);
            break;
        case 'w':
            client_ctx.hcc_cc_reqs_per_conn = atoi(optarg);
            break;
        case 'P':
            client_ctx.payload = optarg;
            if (0 != stat(optarg, &st))
            {
                perror("stat");
                exit(2);
            }
            sprintf(client_ctx.payload_size, "%jd", (intmax_t) st.st_size);
            break;
        case 'M':
            client_ctx.method = optarg;
            break;
        case 'r':
            client_ctx.hcc_total_n_reqs = atoi(optarg);
            break;
        case 'R':
            client_ctx.hcc_reqs_per_conn = atoi(optarg);
            break;
        case 'H':
            client_ctx.hostname = optarg;
            prog.prog_hostname = optarg;            /* Pokes into prog */
            break;
        case 'p':
            pe = calloc(1, sizeof(*pe));
            pe->path = optarg;
            TAILQ_INSERT_TAIL(&client_ctx.hcc_path_elems, pe, next_pe);
            break;
        case 'h':
            usage(argv[0]);
            prog_print_common_options(&prog, stdout);
            exit(0);
        case 'q':
            client_ctx.qif_file = optarg;
            break;
        case 'e':
            if (TAILQ_EMPTY(&sports))
                token = optarg;
            else
                sport_set_token(TAILQ_LAST(&sports, sport_head), optarg);
            break;
#ifndef WIN32
        case 'C':
            prog.prog_api.ea_verify_cert = verify_server_cert;
            prog.prog_api.ea_verify_ctx = optarg;
            break;
#endif
        case 't':
            stats_fh = stdout;
            break;
        case 'T':
            if (0 == strcmp(optarg, "-"))
                stats_fh = stdout;
            else
            {
                stats_fh = fopen(optarg, "w");
                if (!stats_fh)
                {
                    perror("fopen");
                    exit(1);
                }
            }
            break;
        case 'b':
            client_ctx.hcc_reset_after_nbytes = atoi(optarg);
            break;
        case 'd':
            client_ctx.hcc_retire_cid_after_nbytes = atoi(optarg);
            break;
        // Minh - add
        case 'D':
            experiment_id = atoi(optarg);
            printf("**** experiment_id: %d\n", experiment_id);
            break;
        case '0':
            http_client_if.on_sess_resume_info = http_client_on_sess_resume_info;
            client_ctx.hcc_sess_resume_file_name = optarg;
            goto common_opts;
        case '3':
            s_abandon_early = strtol(optarg, NULL, 10);
            break;
        case '9':
        {
            /* Parse priority spec and tack it onto the end of the array */
            lsquic_stream_id_t stream_id;
            size_t nread;
            struct lsquic_ext_http_prio ehp;
            struct priority_spec *new_specs;
            stream_id = strtoull(optarg, &optarg, 10);
            if (*optarg != ':')
                exit(1);
            ++optarg;
            nread = strtoull(optarg, &optarg, 10);
            if (*optarg != ':')
                exit(1);
            ++optarg;
            if (!(*optarg >= '0' && *optarg <= '7'))
                exit(1);
            ehp.urgency = *optarg++ - '0';
            if (!(*optarg >= '0' && *optarg <= '1'))
                exit(1);
            ehp.incremental = *optarg++ - '0';
            ++s_n_prio_specs;
            new_specs = realloc(priority_specs,
                                sizeof(priority_specs[0]) * s_n_prio_specs);
            if (!new_specs)
            {
                perror("malloc");
                exit(1);
            }
            priority_specs = new_specs;
            priority_specs[s_n_prio_specs - 1] = (struct priority_spec) {
                .flags      = PRIORITY_SPEC_ACTIVE,
                .stream_id  = stream_id,
                .nread      = nread,
                .ehp        = ehp,
            };
            s_priority_specs = priority_specs;
            break;
        }
        case '7':
            client_ctx.hcc_download_dir = optarg;
            break;
        case 'Q':
            /* XXX A bit hacky, as `prog' has already been initialized... */
            prog.prog_engine_flags &= ~LSENG_HTTP;
            prog.prog_api.ea_alpn      = optarg;
            prog.prog_api.ea_stream_if = &hq_client_if;
            break;
        common_opts:
        default:
            if (0 != prog_set_opt(&prog, opt, optarg))
                exit(1);
        }
    }

    printf("====> 2\n");
    // Set file path for metrics
    snprintf(METRICS_FILENAME, sizeof(METRICS_FILENAME)*2, "%s%i%i%s", "DoFP_extensions/apple_tos/metrics_abr_", client_ctx.chosen_abr, experiment_id, ".csv");
    
    // Set file path for output metrics
    snprintf(METRICS_OUT_FILENAME, sizeof(METRICS_OUT_FILENAME)*2, "%s%i%i%s", "DoFP_extensions/apple_tos/metrics_abr_", client_ctx.chosen_abr, experiment_id, "_out.csv");
    
    // Set file path for json metrics
    snprintf(JSON_FILENAME, sizeof(JSON_FILENAME)*2, "%s%i%i%s", "DoFP_extensions/apple_tos/itu-p1203_abr_", client_ctx.chosen_abr, experiment_id, ".json");
    
    // Set file path for json qoe metrics
    snprintf(JSON_OUT_FILENAME, sizeof(JSON_OUT_FILENAME)*2, "%s%i%i%s", "DoFP_extensions/apple_tos/itu-p1203_abr_", client_ctx.chosen_abr, experiment_id, "_out.txt");
    
    if (client_ctx.chosen_abr == 6) // SARA
        min_init_bs = s_stats.I;

#if LSQUIC_CONN_STATS
    prog.prog_api.ea_stats_fh = stats_fh;
#endif
    prog.prog_settings.es_ua = LITESPEED_ID;

    if (client_ctx.qif_file)
    {
        client_ctx.qif_fh = fopen(client_ctx.qif_file, "r");
        if (!client_ctx.qif_fh)
        {
            fprintf(stderr, "Cannot open %s for reading: %s\n",
                                    client_ctx.qif_file, strerror(errno));
            exit(1);
        }
        LSQ_NOTICE("opened QIF file %s for reading\n", client_ctx.qif_file);
        prog.prog_api.ea_stream_if = &qif_client_if;
        g_header_bypass = 1;
        prog.prog_api.ea_hsi_if = &header_bypass_api;
        prog.prog_api.ea_hsi_ctx = NULL;
    }
    else if (TAILQ_EMPTY(&client_ctx.hcc_path_elems))
    {
        fprintf(stderr, "Specify at least one path using -p option\n");
        exit(1);
    }

    start_time = lsquic_time_now();
    start_t = lsquic_time_now();
    was_empty = TAILQ_EMPTY(&sports);
    if (0 != prog_prep(&prog))
    {
        LSQ_ERROR("could not prep");
        exit(EXIT_FAILURE);
    }
    if (!(client_ctx.hostname || prog.prog_hostname))
    {
        fprintf(stderr, "Specify hostname (used for SNI and :authority) via "
            "-H option\n");
        exit(EXIT_FAILURE);
    }
    if (was_empty && token)
        sport_set_token(TAILQ_LAST(&sports, sport_head), token);

    if (client_ctx.qif_file)
    {
        if (0 != prog_connect(&prog, NULL, 0))
        {
            LSQ_ERROR("connection failed");
            exit(EXIT_FAILURE);
        }
    }
    else {
        create_connections(&client_ctx);
    }
    
    // RUN SYSTEM BASH "NETWORK TRACES" SCRIPT
    // int status = system("sudo ./bin/A_2018_01_26_11_26_26_good_4M.sh &");

    LSQ_DEBUG("entering event loop");
    
    s = prog_run(&prog);

    if (stats_fh)
    {
        elapsed = (long double) (lsquic_time_now() - start_time) / 1000000;
        fprintf(stats_fh, "overall statistics as calculated by %s:\n", argv[0]);
        display_stat(stats_fh, &s_stat_to_conn, "time for connect");
        display_stat(stats_fh, &s_stat_req, "time for request");
        display_stat(stats_fh, &s_stat_ttfb, "time to 1st byte");
        fprintf(stats_fh, "downloaded %lu application bytes in %.3Lf seconds\n",
            s_stat_downloaded_bytes, elapsed);
        fprintf(stats_fh, "%.2Lf reqs/sec; %.0Lf bytes/sec\n",
            (long double) s_stat_req.n / elapsed,
            (long double) s_stat_downloaded_bytes / elapsed);
        fprintf(stats_fh, "read handler count %lu\n", prog.prog_read_count);
    }
    
    /* CREATE OUTPUT CSV FILE */
    FILE *fp = fopen(METRICS_FILENAME,"wa");
    if (!fp)
        printf("Error opening JSON file %s!\n", METRICS_FILENAME);
    fprintf(fp, "THROUGHPUT,BITRATE,BUFFER,QUALITY,STALLT,STALLD");
    // Minh - Add metrics when H2BR is enable - MOD - S
    #if 0
    if (client_ctx.chosen_abr < 4)
    #else
    if (client_ctx.chosen_abr < 4 || client_ctx.h2br)
    #endif
    // Minh - Add metrics when H2BR is enable - MOD - E
        fprintf(fp, ",REDATA,RECOUNT,REUNUSED,REUNUSEDCOUNT");
    fprintf(fp, "\n");
    for (size_t i = 0; i < N_MAX_SEG; i++) {
        fprintf(fp, "%.2Lf,%i,%.3f,%i,%.3f,%.3f", t_stats.e_throughput[i], seg_bitrates[seg_chosen_q[i]], t_stats.b_level[i], seg_chosen_q[i], stalls_t[i], stalls_d[i]);
        // Minh - Add metrics when H2BR is enable - MOD - S
        #if 0
        if (i == 0 && client_ctx.chosen_abr < 4)
        #else
        if (i == 0 && (client_ctx.chosen_abr < 4 || client_ctx.h2br))
        #endif
        // Minh - Add metrics when H2BR is enable - MOD - E
            fprintf(fp, ",%.3Lf,%u,%.3Lf,%u", w_stats.re_data, w_stats.re_count, w_stats.re_unused_data, w_stats.re_unused_count);
        fprintf(fp, "\n");
    }
    fclose(fp);
    
    /* CREATE INPUT JSON FILE */
    FILE *jfp = fopen(JSON_FILENAME,"wa");
    if (!jfp)
        printf("Error opening JSON file %s!\n", JSON_FILENAME);
    fprintf(jfp, "{\n\t\"I11\":{\n\t\t\"segments\":[],\n\t\t\"streamId\":42},\n\t\"I13\":{\n\t\t\"segments\":[");
    unsigned int start = 0U;
    fprintf(jfp, "\n\t\t{\n\t\t\t\"bitrate\":%i,\n\t\t\t\"codec\":\"hevc\",\n\t\t\t\"duration\":%u,\n\t\t\t\"fps\":%.1f,\n\t\t\t\"resolution\":\"%s\",\n\t\t\t\"start\":%u\n\t\t\t}", 
            seg_bitrates[seg_chosen_q[0]], seg_length, FPS, seg_res[seg_chosen_q[0]], start);
    start += seg_length;
    for (size_t i = 1; i < N_MAX_SEG; i++) {
        fprintf(jfp, ",\n\t\t{\n\t\t\t\"bitrate\": %i,\n\t\t\t\"codec\":\"hevc\",\n\t\t\t\"duration\":%u,\n\t\t\t\"fps\":%.1f,\n\t\t\t\"resolution\":\"%s\",\n\t\t\t\"start\":%u\n\t\t\t}", 
            seg_bitrates[seg_chosen_q[i]], seg_length, FPS, seg_res[seg_chosen_q[i]], start);
    start += seg_length;
    }
    fprintf(jfp, "],\n\t\t\"streamId\":42\n},\n\t\"I23\":{\n\t\"stalling\":[");
    fprintf(jfp, "[%.3f,%.3f]", stalls_t[0], stalls_d[0]);
    for (size_t j = 1; j < stall_ind; j++)
        fprintf(jfp, ",[%.3f,%.3f]", stalls_t[j], stalls_d[j]);
    fprintf(jfp, "],\n\t\t\"streamId\": 42},\n\t\"IGen\":{\n\t\t\"device\":\"%s\",\n\t\t\"displaySize\":\"%s\",\n\t\t\"viewingDistance\":\"%u%s\"}}\n", DEVICE, DISPLAYSIZE, VIEWINGDISTANCE, "cm");
    fclose(jfp);
    
    /* CREATE OUTPUT JSON FILE */
    int max_command_length = 250;
    char command[max_command_length];
    snprintf(command, max_command_length, "python3 /home/minh/qoe_model_source/itu-p1203-codecextension/calculate.py -m 0 %s > %s\n", JSON_FILENAME, JSON_OUT_FILENAME);
    int error = system(command);
    if (error == -1)
        printf("Error executing the command \'%s\'", command);
    
    /* CREATE OUTPUT METRICS FILE */
    snprintf(command, max_command_length, "python3 compute-metrics.py %s %s > %s\n", METRICS_FILENAME, JSON_OUT_FILENAME, METRICS_OUT_FILENAME);
    error = system(command);
    if (error == -1)
        printf("Error executing the command \'%s\'", command);
    
    prog_cleanup(&prog);
    if (promise_fd >= 0)
        (void) close(promise_fd);

    while ((pe = TAILQ_FIRST(&client_ctx.hcc_path_elems)))
    {
        TAILQ_REMOVE(&client_ctx.hcc_path_elems, pe, next_pe);
        free(pe);
    }

    if (client_ctx.qif_fh)
        (void) fclose(client_ctx.qif_fh);

    free(priority_specs);
    exit(0 == s ? EXIT_SUCCESS : EXIT_FAILURE);
}

