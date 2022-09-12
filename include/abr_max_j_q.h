#include <stdlib.h>
#include <stdio.h>
#include "gurobi_c.h"

int
getMaxJQCoefficients(unsigned n_rep, unsigned n_seg, unsigned n_par, unsigned seg_dur, double bandwidth, double alpha, double betha, const int* bitrates, double* available_times, unsigned* min_q, double* sol);