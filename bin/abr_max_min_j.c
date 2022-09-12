/* ABR Maximum J optimization algorithm (min_q starts from old_j)
 * 
 * Author: Daniele Lorenzi, Minh Nguyen, Farzad
 * Email:  lorenzi.daniele97@gmail.com
 * Year:   2021
 */

/* This example formulates and solves the following simple MIP model:

    maximize    j* + sum_(j) (a_{ij} * j) -> [j* = Min(j_{i})], for every segment i
    subject to  sum_(j) (a_{ij}) = 1 for every segment i
                seg_dur x bitrates[i] - available_times[i] * Ti <= 0 for every segment i
                sum_(i) (Ti) <= throughput
                j* <= sum_(j) (a_{ij}*j) for every segment i
*/

#include "abr_max_min_j.h"

int
getMaxMinJCoefficients(unsigned n_rep, unsigned n_seg, unsigned n_par, unsigned seg_dur, double throughput, 
                    const int* bitrates, double* available_times, unsigned* min_q, double* sol, bool isMultiStream) {
    GRBenv * env = NULL; // Environment, and
    GRBmodel * model = NULL; // Model structure
    // Error code output by the GUROBI functions
    int error = 0;
    // Number of values to be added as constraints
    unsigned n_val = 0;
    // Temporary index the location of the parameters
    unsigned temp_ind = 0;
    // Array to index the location of the parameter
    int ind[n_par];
    // Array to specify the coefficient of the indexed parameter
    double val[n_par];
    // Array to specify the coefficient of the objective function parameters
    double obj[n_par];
    // Array to store the types of the parameters
    char vtype[n_par];
    int qrow[1]; // Array to index the parameters
    int qcol[1]; // used in quadratic constraints
    // Array to index the coefficient of the quadratic constraint parameters
    double qval[1];
    // Optimization status information
    int optimstatus;
    // Final value of the optimized objective function
    double objval;

    /* Create environment */
    error = GRBemptyenv( & env);
    if (error) goto QUIT;

    error = GRBstartenv(env);
    if (error) goto QUIT;

    /* Create an empty model */
    error = GRBnewmodel(env, & model, "maxminJ", 0, NULL, NULL, NULL, NULL, NULL);
    if (error) goto QUIT;

    /* Add variables */
    for (size_t i = 0; i < n_seg; i++) {
        for (size_t j = min_q[i]; j < n_rep; j++) {
            obj[temp_ind] = (double)(j % n_rep); // NREP;
            vtype[temp_ind] = GRB_BINARY; // a_{ij}
            temp_ind++;
        }
    }
    while (temp_ind < n_par - 1) {
        obj[temp_ind] = 0.0;
        vtype[temp_ind] = GRB_INTEGER; // Throughputs
        temp_ind++;
    }
    obj[n_par - 1] = 1.0;
    vtype[n_par - 1] = GRB_INTEGER; // j*

    error = GRBaddvars(model, n_par, 0, NULL, NULL, NULL, obj, NULL, NULL, vtype,
        NULL);
    if (error) goto QUIT;

    /* Change objective sense to maximization */
    error = GRBsetintattr(model, GRB_INT_ATTR_MODELSENSE, GRB_MAXIMIZE);
    if (error) goto QUIT;

    temp_ind = 0;
    /* First constraints group: for every i -> sum(a_{ij}) == 1 */
    for (size_t i = 0; i < n_seg; i++) {
        n_val = 0;
        for (size_t j = min_q[i]; j < n_rep; j++) {
            ind[n_val] = temp_ind;
            val[n_val] = 1.0;
            n_val++;
            temp_ind++;
        }
        if (n_val > 0){
            error = GRBaddconstr(model, n_val, ind, val, GRB_EQUAL, 1.0, NULL);
            if (error) goto QUIT;
        }
    }

    temp_ind = 0;
    /* Second constraints group: for every i -> SD x ri - di * Ti <= 0 */
    for (size_t i = 0; i < n_seg; i++) {
        n_val = 0;
        for (size_t j = min_q[i]; j < n_rep; j++) {
            ind[n_val] = temp_ind;
            val[n_val] = seg_dur * bitrates[j];
            n_val++;
            temp_ind++;
        }
        ind[n_val] = n_par - n_seg - 1 + i; // this index value is the throughput Ti
        val[n_val] = - available_times[i];
        if (n_val > 0){
            error = GRBaddgenconstrIndicator(model, NULL, temp_ind - (n_rep - min_q[i]), 0, n_val + 1, ind, val, GRB_LESS_EQUAL, 0.0); // If j_new != j_old add the constraint on that segment
            if (error) goto QUIT;
        }
    }

    /* Third constraint: sum(Ti) <= Throughput */
    for (size_t i = 0; i < n_seg; i++) {
        ind[i] = n_par - n_seg - 1 + i;
        val[i] = 1.0;
    }
    error = GRBaddconstr(model, n_seg, ind, val, GRB_LESS_EQUAL, throughput, NULL);
    if (error) goto QUIT;
    
    temp_ind = 0;
    /* Indicator constraints: for every i -> if a_{ij} == 1 with j = j_old (for every i except the last one -> Quality 0 can be selected and needs to be downloaded) -> Ti = 0.0 */
    for (size_t i = 0; i < n_seg - 1; i++) {
        ind[0] = n_par - n_seg - 1 + i;
        val[0] = 1.0;
        error = GRBaddgenconstrIndicator(model, NULL, temp_ind, 1, 1, ind, val, GRB_EQUAL, 0.0);
        if (error) goto QUIT;
        temp_ind += n_rep - min_q[i];
    }
    
    /* Throughput constraints if multiStream is selected -> every T (if T != 0) has to be equal because of the throughput splitting across streams */
    if (isMultiStream){
        temp_ind = 0;
        /* Indicator constraints: for every i -> if a_{ij} == 0 with j = j_old -> (T{i} - T{i+1} >= 0.0 for every i except last one) */
        size_t i = 0;
        for (i = 0; i < n_seg - 1; i++) {
            for (size_t j = 0; j < n_seg; j++) {
                if (i == j)
                    continue;
                ind[0] = n_par - n_seg - 1 + i;
                val[0] = 1.0;
                ind[1] = n_par - n_seg - 1 + j;
                val[1] = -1.0;
                error = GRBaddgenconstrIndicator(model, NULL, temp_ind, 0, 2, ind, val, GRB_GREATER_EQUAL, 0.0);
                if (error) goto QUIT;
            }
            temp_ind += n_rep - min_q[i];
        }
        for (size_t j = 0; j < n_seg - 1; j++) {
                // if (i == j)
                    // continue;
                ind[0] = n_par - n_seg - 1 + i;
                val[0] = 1.0;
                ind[1] = n_par - n_seg - 1 + j;
                val[1] = -1.0;
                error = GRBaddconstr(model, 2, ind, val, GRB_GREATER_EQUAL, 0.0, NULL); // This condition has to be valid always -> Not constrained to any value of a{ij}
                if (error) goto QUIT;
            }
        
    }

    temp_ind = 0;
    /* Fourth constraints group: for every i and possible j -> (a_{ij}*j* - a_{ij}*j) <= 0 */
    for (size_t i = 0; i < n_seg; i++) {
        for (size_t j = min_q[i]; j < n_rep; j++) {
            qrow[0] = temp_ind; // a_{ij} index
            qcol[0] = n_par - 1; // last index value is j*
            qval[0] = 1.0;
            ind[0] = temp_ind;
            val[0] = - (double) j;
            error = GRBaddqconstr(model, 1, ind, val, 1, qrow, qcol, qval, GRB_LESS_EQUAL, 0.0, NULL);
            if (error) goto QUIT;
            temp_ind++;
        }
    }

    /* Optimize model */
    error = GRBoptimize(model);
    if (error) goto QUIT;

    /* Capture solution information */
    error = GRBgetintattr(model, GRB_INT_ATTR_STATUS, & optimstatus);
    if (error) goto QUIT;

    error = GRBgetdblattr(model, GRB_DBL_ATTR_OBJVAL, & objval);
    if (error) goto QUIT;

    error = GRBgetdblattrarray(model, GRB_DBL_ATTR_X, 0, n_par, sol);
    if (error) goto QUIT;

    QUIT:

        /* Error reporting */
        if (error) {
            printf("ERROR: %s\n", GRBgeterrormsg(env));
            return -1;
        }

    /* Free model */
    GRBfreemodel(model);

    /* Free environment */
    GRBfreeenv(env);

    return 0;
}