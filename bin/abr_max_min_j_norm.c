/* ABR Normalized Maximum J optimization algorithm (min_q starts from old_j)
 * 
 * Author: Daniele Lorenzi, Minh Nguyen, Farzad
 * Email:  lorenzi.daniele97@gmail.com
 * Year:   2021
 */

/* This example formulates and solves the following simple MIP model:

    maximize    alpha * (j* / N) + beta * (sum_(j) (a_{ij} * j * N^i) / (N * N^i)) -> [j* = Min(j_{i})], for every segment i
    subject to  sum_(j) (a_{ij}) = 1 for every segment i
	            seg_dur x bitrates[i] - available_times[i] * Ti <= 0 for every segment i
				sum_(i) (Ti) <= bandwidth
				j* <= sum_(j) (a_{ij}*j) for every segment i
*/

#include "abr_max_min_j_norm.h"

int
getMaxMinJNormCoefficients(unsigned n_rep, unsigned n_seg, unsigned n_par, unsigned seg_dur, double bandwidth, 
					const int* bitrates, double* available_times, unsigned* min_q, double* sol, bool isMultiStream, double alpha, double beta, double buffer_level, double buffer_size) {
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

    /* Create environment */
    error = GRBemptyenv( & env);
    if (error) goto QUIT;

    error = GRBstartenv(env);
    if (error) goto QUIT;

    /* Create an empty model */
    error = GRBnewmodel(env, & model, "maxminJNorm", 0, NULL, NULL, NULL, NULL, NULL);
    if (error) goto QUIT;
	
	double normal_factor = 0.0;
	for (size_t i = 0; i < n_seg; i++) {
		normal_factor += pow((double) n_rep-1, (double) i + 2);
	}	
	
	/* Add variables */
    for (size_t i = 0; i < n_seg; i++) {
        for (size_t j = min_q[i]; j < n_rep; j++) {
            obj[temp_ind] = alpha * (j % n_rep) * pow((double) n_rep-1, (double) i + 1) / normal_factor;
			vtype[temp_ind] = GRB_BINARY; // a_{ij}
			temp_ind++;
        }
    }
    while (temp_ind < n_par - 1) {
        obj[temp_ind] = 0.0;
        vtype[temp_ind] = GRB_INTEGER; // Throughputs
		temp_ind++;
    }
	obj[n_par - 1] = beta / (n_rep-1);
	vtype[n_par - 1] = GRB_INTEGER; // j*

    error = GRBaddvars(model, n_par, 0, NULL, NULL, NULL, obj, NULL, NULL, vtype,
        NULL);
    if (error) goto QUIT;
	
	// error = GRBsetdblattr(model, GRB_DBL_ATTR_OBJCON, beta * (buffer_level + seg_dur) / buffer_size);

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
			error = GRBaddgenconstrIndicator(model, NULL, temp_ind - (n_rep - min_q[i]), 0, n_val + 1, ind, val, GRB_LESS_EQUAL, 0.0); // If j_new != j_old add the constraint on that segment download time
			if (error) goto QUIT;
		}
    }

    /* Third constraint: sum(Throughput) <= Bandwidth */
    for (size_t i = 0; i < n_seg; i++) {
        ind[i] = n_par - n_seg - 1 + i;
        val[i] = 1.0;
    }
    error = GRBaddconstr(model, n_seg, ind, val, GRB_LESS_EQUAL, bandwidth, NULL);
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
	
	/* Throughput constraints if multiStream is selected -> every T (if T != 0) has to be equal because of the bandiwdth splitting across streams */
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
	
	double objcon;
	error = GRBgetdblattr(model, GRB_DBL_ATTR_OBJCON, &objcon);
	printf("\nObjCon: %.3f\n", objcon);

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
