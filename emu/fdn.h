/* fdn: native multicore FD solver for Picat (see fdaccel/README.md).
   Interface between the Picat bridge (fdn.c, knows Picat terms) and the
   solver (fdn_solver.cpp, knows nothing about Picat). */
#ifndef FDN_H
#define FDN_H
#include <stddef.h>
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
void fdn_bmap(fdn_net *, int y, int t, int s, int k);      /* bounds of y -> bounds of t (X = Y+C, C-Y) */
/* c + sum a_i x_i  (op 0: = 0, op 1: >= 0, op 2: = 0 with arc consistency
   once at most 2 variables are unbound, as '$linear_constr_eq_ARC_aux');
   constants in place: xs[i] < 0 means the constant kv[i] (Picat's single
   propagation pass walks the terms in this order) */
void fdn_linear(fdn_net *, int op, long c, int n, const long *a, const int *xs, const long *kv);
/* abs(X-Y) = n; X*Y = Z; X div y = Z / X mod y = Z (y a fixed divisor);
   R = min/max of (ids, vals) where id < 0 means the constant vals[i] */
void fdn_abs(fdn_net *, int x, int y, int n);
void fdn_mul(fdn_net *, int x, int y, int z);
void fdn_div(fdn_net *, int x, int y, int z);
void fdn_mod(fdn_net *, int x, int y, int z);
void fdn_mm(fdn_net *, int ismin, int r, int n, const int *ids, const long *vals);
/* reified B <=> (mode 0: X = c, 1: X != c, 2: X = Y, 3: X != Y, 4: X >= Y);
   entailed B => (0: X = c, 1: X != c, 2: X = Y, 3: X != Y) */
void fdn_reif(fdn_net *, int mode, int b, int x, int y);
void fdn_entail(fdn_net *, int mode, int b, int x, int y);
/* element(I, tuple, V): I fixed -> V = tuple[I-1]; fast: V fixed -> I in
   [lo[w], hi[w]] over the value->range table, plus FC on removals (t = the
   tuple id shared with fdn_eldelay) */
void fdn_eldelay(fdn_net *, int i, int v, int tuple);
void fdn_elfast(fdn_net *, int i, int v, int tuple, int nsup, const long *ks, const long *lo, const long *hi);
/* element FC on removals only (the I_to_V / V_to_I frames) */
void fdn_elfc(fdn_net *, int i, int v, int tuple);
/* lexicographic chain of np pairs (x0,y0,x1,y1,...): watch bounds on the
   first np-1 pairs, arc consistency on the last (le: <=, else <) */
void fdn_lex(fdn_net *, int le, int np, const int *pr);
/* the net as a JSON model for the external CP-SAT server (fdn_cpsat.py,
   FDN_EXTSOLVER=cp-sat); obj: the objective variable id or -1; lab/nlab: the
   solution-dedup projection (the original label list's variable ids, or NULL
   for the net's own label order). Returns a malloc'd string, *len = its
   length. */
const char *fdn_export_json(const fdn_net *, int obj, const int *lab, int nlab, size_t *len);
/* labeling strategies (labeling_var_strategy/2, labeling_val_strategy/2 in
   cpeval/decompiled/fd_labeling.pi) */
enum { VS_LEFTMOST, VS_FF, VS_MIN, VS_MAX, VS_FF_MIN, VS_FF_MAX };
enum { US_UP, US_DOWN, US_UPDOWN, US_SPLIT, US_REVERSE_SPLIT };
/* xs: label order (any reordering already done by the caller) */
void fdn_label(fdn_net *, int varsel, int valsel, int n, const int *xs);
const int *fdn_net_label(fdn_net *, int *n);   /* the label order set by fdn_label */
void fdn_net_free(fdn_net *);

/* search; the run takes ownership of the net. path: every solution carries
   its decision path (fdn_last_path) */
fdn_run *fdn_start(fdn_net *, int path);
/* the effective thread count (explicit FDN_THREADS, else min(cores,64)) */
int  fdn_nthreads(void);
/* count mode (count_all): solutions are counted natively; *bt receives the
   exact backtrack total, the return value is the solution count */
fdn_run *fdn_start_count(fdn_net *);
long fdn_count(fdn_run *, long *bt);
/* next solution: values of the label variables, in label order.
   Returns 1 (solution), 0 (no more). *bt receives Picat-equivalent
   backtracks since the previous call. */
int  fdn_next(fdn_run *, int *vals, long *bt);
/* one branch-and-bound round (Picat's minof_aux): a run like fdn_start, read
   with fdn_next, whose search starts with variable obj narrowed to <= ub
   (obj < 0: no bound). The run does not own the net, which serves the next
   round; free it with fdn_net_free after the last run is freed. */
fdn_run *fdn_start_round(fdn_net *, int obj, long ub, int path);
/* the decisions from the root to the solution fdn_next returned last (runs
   started with path set): n triples (variable id, lo, hi), lo == hi meaning
   "x = lo", else "domain_region(x, lo, hi)" (split) */
const int *fdn_last_path(fdn_run *, int *n);
int  fdn_nlabel(fdn_run *);
const int *fdn_label_ids(fdn_run *);   /* variable ids in solver label order */
void fdn_free(fdn_run *);
const char *fdn_stats(fdn_run *);
#ifdef __cplusplus
}
#endif
#endif
