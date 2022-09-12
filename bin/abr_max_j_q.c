/* ABR Maximum J optimization algorithm (min_q starts from old_j)
 * 
 * Author: Daniele Lorenzi, Minh Nguyen, Farzad
 * Email:  lorenzi.daniele97@gmail.com
 * Year:   2021
 */

/* This example formulates and solves the following simple MIP model:

    maximize    /alpha j* + /betha j^ -> j* = Min(j_{i}); j^ = Max(j_{i});
    subject to  sum_(j) (a_{ij}) = 1 for every segment i
	            seg_dur x bitrates[i] - available_times[i] * Ti <= 0 for every segment i
				sum_(i) (Ti) <= bandwidth
				j* <= sum_(j) (a_{ij}*j) for every segment i
				j^ >= sum_(j) (a_{ij}*j) for every segment i
*/

#include "abr_max_j_q.h"

int
getMaxJQCoefficients(unsigned n_rep, unsigned n_seg, unsigned n_par, unsigned seg_dur, double bandwidth, double alpha,
					double betha, const int* bitrates, double* available_times, unsigned* min_q, double* sol) {
    GRBenv * env = NULL;
    GRBmodel * model = NULL;
    int error = 0;
	unsigned n_val = 0; // Number of values to be added as constraints
	unsigned temp_ind = 0; // Index to store the location of the parameter
    int ind[n_par];
    double val[n_par];
    double obj[n_par];
    char vtype[n_par];
	int qrow[1];
	int qcol[1];
	double qval[1];
    int optimstatus;
    double objval;
	
	/* Print values */
	printf("#rep: %u, #seg: %u, #par: %u, seg.dur.: %u, bandwidth: %.0f, alpha: %.2f, betha: %.2f\n", n_rep, n_seg, n_par, seg_dur, bandwidth, alpha, betha);

    /* Create environment */
    error = GRBemptyenv( & env);
    if (error) goto QUIT;

    error = GRBstartenv(env);
    if (error) goto QUIT;

    /* Create an empty model */
    error = GRBnewmodel(env, & model, "maxminJQ", 0, NULL, NULL, NULL, NULL, NULL);
    if (error) goto QUIT;

	/* Add variables */
    for (size_t i = 0; i < n_seg; i++) {
        for (size_t j = min_q[i]; j < n_rep; j++) {
            obj[temp_ind] = 0.0; // NREP;
			vtype[temp_ind] = GRB_BINARY; // a_{ij}
			temp_ind++;
        }
    }
    while (temp_ind < n_par - 2) {
        obj[temp_ind] = 0.0;
        vtype[temp_ind] = GRB_INTEGER; // Throughputs
		temp_ind++;
    }
	obj[n_par - 2] = alpha;
	vtype[n_par - 2] = GRB_INTEGER; // j*
	obj[n_par - 1] = betha;
	vtype[n_par - 1] = GRB_INTEGER; // j^

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
        ind[n_val] = n_par - n_seg - 2 + i; // this index value is the throughput Ti
        val[n_val] = - available_times[i];
		if (n_val > 0){
			error = GRBaddgenconstrIndicator(model, NULL, temp_ind - (n_rep - min_q[i]), 0, n_val + 1, ind, val, GRB_LESS_EQUAL, 0.0); // If j_new != j_old add the constraint on that segment download time
			if (error) goto QUIT;
		}
    }

    /* Third constraint: sum(Throughput) <= Bandwidth */
    for (size_t i = 0; i < n_seg; i++) {
        ind[i] = n_par - n_seg - 2 + i;
        val[i] = 1.0;
    }
    error = GRBaddconstr(model, n_seg, ind, val, GRB_LESS_EQUAL, bandwidth, NULL);
    if (error) goto QUIT;
	
	temp_ind = 0;
	/* Indicator constraints: for every i -> if a_{ij} == 1 with j = j_old -> Ti = 0.0 */
    for (size_t i = 0; i < n_seg; i++) {
		ind[0] = n_par - n_seg - 2 + i;
		val[0] = 1.0;
        error = GRBaddgenconstrIndicator(model, NULL, temp_ind, 1, 1, ind, val, GRB_EQUAL, 0.0);
		if (error) goto QUIT;
		temp_ind += n_rep - min_q[i];
    }

	temp_ind = 0;
	/* Fourth constraints group: for every i and possible j -> (a_{ij} * j* - a_{ij} * j) <= 0 */
    for (size_t i = 0; i < n_seg; i++) {
        for (size_t j = min_q[i]; j < n_rep; j++) {
            qrow[0] = temp_ind; // a_{ij} index
            qcol[0] = n_par - 2; // this index value is j*
			qval[0] = 1.0;
			ind[0] = temp_ind;
			val[0] = - (double) j;
			error = GRBaddqconstr(model, 1, ind, val, 1, qrow, qcol, qval, GRB_LESS_EQUAL, 0.0, NULL);
			if (error) goto QUIT;
			temp_ind++;
        }
    }
	
	temp_ind = 0;
	/* Fifth constraints group: for every i and possible j -> (a_{ij} * j^ - a_{ij} * j) >= 0 */
    for (size_t i = 0; i < n_seg; i++) {
        for (size_t j = min_q[i]; j < n_rep; j++) {
            qrow[0] = temp_ind; // a_{ij} index
            qcol[0] = n_par - 1; // this index value is j^
			qval[0] = 1.0;
			ind[0] = temp_ind;
			val[0] = - (double) j;
			error = GRBaddqconstr(model, 1, ind, val, 1, qrow, qcol, qval, GRB_GREATER_EQUAL, 0.0, NULL);
			if (error) goto QUIT;
			temp_ind++;
        }
    }
	
	// j^ <= N_REP - 1
	ind[0] = n_par - 1;
	val[0] = 1.0;
	error = GRBaddconstr(model, 1, ind, val, GRB_LESS_EQUAL, n_rep - 1, NULL);
	if (error) goto QUIT;

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
    /*
      printf("\nOptimization complete\n");
      if (optimstatus == GRB_OPTIMAL) {
        printf("Optimal objective: %.f\n", objval);

    	for (size_t i = 0; i < n_seg * n_rep + n_seg; i++)
    		printf("- var[%zu]=%.3f -", i, sol[i]);
      } else if (optimstatus == GRB_INF_OR_UNBD) {
        printf("Model is infeasible or unbounded\n");
      } else {
        printf("Optimization was stopped early\n");
      }
    */

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