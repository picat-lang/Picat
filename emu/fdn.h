/* fdn: native multicore FD solver for Picat (see fdaccel/README.md).
   Interface between the Picat bridge (fdn.c, knows Picat terms) and the
   solver (fdn_solver.cpp, knows nothing about Picat). */
#ifndef FDN_H
#define FDN_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fdn_net fdn_net;
typedef struct fdn_run fdn_run;

/* network construction; variables are 0..n-1, values in [glo, ghi] */
fdn_net *fdn_net_new(int glo, int ghi);
int  fdn_var(fdn_net *, int nvals, const int *vals);      /* returns id */
void fdn_edge(fdn_net *, int owner, int target, int d);   /* owner = v -> target != v+d */
void fdn_hall(fdn_net *, int n, const int *xs);           /* all_distinct Hall group */
int  fdn_tuple(fdn_net *, int n, const int *xs);          /* channel tuple, returns id */
void fdn_chan(fdn_net *, int x, int tuple, int off, int a);
void fdn_map(fdn_net *, int x, int t, int s, int k);       /* w removed from x -> t != s*w+k */
/* c + sum a_i x_i  (op 0: = 0, op 1: >= 0, op 2: = 0 with arc consistency
   once at most 2 variables are unbound, as '$linear_constr_eq_ARC_aux') */
void fdn_linear(fdn_net *, int op, long c, int n, const long *a, const int *xs);
/* heur 0 leftmost, 1 ff; xs in labeling order (ffc pre-sorted by caller) */
void fdn_label(fdn_net *, int heur, int n, const int *xs);
void fdn_net_free(fdn_net *);

/* search; the run takes ownership of the net */
fdn_run *fdn_start(fdn_net *);
/* next solution: values of the label variables, in label order.
   Returns 1 (solution), 0 (no more). *bt receives Picat-equivalent
   backtracks since the previous call. */
int  fdn_next(fdn_run *, int *vals, long *bt);
int  fdn_nlabel(fdn_run *);
const int *fdn_label_ids(fdn_run *);   /* variable ids in solver label order */
void fdn_free(fdn_run *);
const char *fdn_stats(fdn_run *);
#ifdef __cplusplus
}
#endif
#endif
