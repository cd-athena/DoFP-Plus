DoFP+ ABR Algorithm README
=============================================

Description
-----------

This is a fork of [LSQUIC](https://github.com/litespeedtech/lsquic) where we implemented DoFP+ ABR algorithm in QUIC
and HTTP/3.


Requirements
------------

To build LSQUIC, you need CMake, zlib, Gurobi and BoringSSL.  The example program
uses libevent to provide the event loop.

Building BoringSSL
------------------

BoringSSL is not packaged; you have to build it yourself.  The process is
straightforward.  You will need `go` installed.

1. Clone BoringSSL by issuing the following command:

```
git clone https://boringssl.googlesource.com/boringssl
cd boringssl
```

You may need to install pre-requisites like zlib and libevent.

2. Use specific BoringSSL version

```
git checkout 31bad2514d21f6207f3925ba56754611c462a873
```

3. Compile the library

```
cmake . &&  make
```

Remember where BoringSSL sources are:
```
BORINGSSL=$PWD
```

If you want to turn on optimizations, do

```
cmake -DCMAKE_BUILD_TYPE=Release . && make
```

If you want to build as a library, (necessary to build lsquic itself
as as shared library) do:

```
cmake -DBUILD_SHARED_LIBS=1 . && make
```

Building LSQUIC Library
-----------------------

LSQUIC's `http_client`, `http_server`, and the tests link BoringSSL
libraries statically.  Following previous section, you can build LSQUIC
as follows:

1. Get the source code

```
git clone https://github.com/cd-athena/DoFP-Plus.git
cd lsquic
```

2. Compile the library

Statically:


```
# $BORINGSSL is the top-level BoringSSL directory from the previous step
cmake -DBORINGSSL_DIR=$BORINGSSL .
make
```

3. Create SSL certificate at the server side

```
openssl req -newkey rsa:2048 -new -nodes -x509 -days 3650 -keyout key.pem -out cert.pem
```

4. Install ITU-T P.1203 codec extension

Follow the instruction in IT-T P.1203's [extension](https://github.com/Telecommunication-Telemedia-Assessment/itu-p1203-codecextension)


Modify the following line in `./bin/http_client_dofp_python.c` with the path of ITU-T P.1203's extension

```
snprintf(command, max_command_length, "python3 /home/minh/Documents/itu-p1203-codecextension/calculate.py -m 0 %s > %s\n", JSON_FILENAME, JSON_OUT_FILENAME);
```

Modify the parameters related to ITU-T P.1203's extension
```
static const float      FPS = 24.0;
static const char       DEVICE[] = "pc";
static const char       DISPLAYSIZE[] = "3840x2160";
static const unsigned   VIEWINGDISTANCE = 150U;
```

5. Modify video's information

The current version of DoFP+ does not support reading the MPD file. Therefore, you need to update some video's information in
```
#define N_REP 7 /* Number of available media representations (quality levels) */
#define N_MAX_SEG 75 /* Max number of segments to be downloaded */

static char             *seg_paths[N_REP] = {"tos1_h264/107/segment_1.m4s", 
                                              "tos1_h264/240/segment_1.m4s", 
                                              "tos1_h264/346/segment_1.m4s", 
                                              "tos1_h264/715/segment_1.m4s", 
                                              "tos1_h264/1347/segment_1.m4s", 
                                              "tos1_h264/2426/segment_1.m4s", 
                                              "tos1_h264/4121/segment_1.m4s"};
static const int        seg_bitrates[N_REP] = {107, 240, 346, 715, 1347, 2426, 4121}; // [kbps]
static char             *seg_res[N_REP] = {"256x114", "426x190", "640x286", "854x382", "1280x572", "1920x858", "2560x1142"};
static const char       FP_PATH[] = "tos1_h264/";
static const char       SP_PATH[] = "/segment_";
static const char       EXT[] = ".m4s";
static const char       WEIGHTS_FILENAME[] = "tos1_h264/weights.txt";
```

Run experiments
---------------------

1. Server

```
./bin/http_server_dofp -c www.optimized-abr.com,cert.pem,key.pem -s <server_ip>:<port>
```

2. Client

```
./bin/http_client_dofp_python -H www.optimized-abr.com -s <server_ip>:<port> -p tos1_h264/107/segment_1.m4s -M GET -K -r 250 -w 1 -J 0
```

where:

```
  -M  HTTP method (e.g., GET)
  -K  Discard server response
  -r  Total number of requests to send
  -w  Number of concurrent requests per single connection
  -J  ABR algorithms (0: DoFP+, 4: Throughput-based, 5: BOLA, 6: SARA, 7: BBA-0)
```

3. Results
The results are reported at the client machine. There are four files as follows

- metrics_abr_<-J value>.csv        --> This file comprises the log of download every segments.
- metrics_abr_<-J value>_out.csv    --> This file provides some metrics of the streaming session.
- itu-p1203_abr_<-J value>.json     --> This file comprises the information of every downloaded segment. It is used for calculating QoE by ITU-T P.1203's extension.
- itu-p1203_abr_<-J value>_out.json --> This files is the output of ITU-T P.1203's extension. The predicted overall QoE has the key `"O46"`.

## Contributors
* Daniele Lorenzi - Christian Doppler Laboratory ATHENA, Alpen-Adria-Universitaet Klagenfurt - daniele.lorenzi@aau.at
* Minh Nguyen - Christian Doppler Laboratory ATHENA, Alpen-Adria-Universitaet Klagenfurt - minhnguyenkstn@gmail.com


## Citation
```
@article{nguyen2022dofp+,
  title={DoFP+: An HTTP/3-Based Adaptive Bitrate Approach Using Retransmission Techniques},
  author={Nguyen, Minh and Lorenzi, Daniele and Tashtarian, Farzad and Hellwagner, Hermann and Timmerer, Christian},
  journal={IEEE Access},
  volume={10},
  pages={109565--109579},
  year={2022},
  publisher={IEEE}
}

```
