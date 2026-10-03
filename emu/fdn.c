/* fdn.c: Picat side of picat-fdn (see fdaccel/README.md).

   - fdn_init(): at start-up, loads fdn_hook.pi and points labeling/2 (called
     by cp's solve/1,2) at fdn_hook's fdn_labeling/2; the original code stays
     reachable as '$fdn_orig_labeling'/2.
   - c_fdn_start(Opts, Vars, H): extracts the constraint network reachable
     from Vars out of the live constraint store (FD variables, their
     suspension lists and the neq attribute) and starts the native search.
     It fails, and Picat labels as usual, when anything is outside the
     supported vocabulary: an unknown suspended propagator, an attribute
     other than the neq one, a labeling option other than ff / ffc /
     leftmost / up, or a value range wider than 65536.
   - c_fdn_next(H, Vals): next solution in Picat's order, or fails.

   Supported propagators (live frames, by their delay symbol):
     '$combined_neq'/2 + attribute _$attr_neq = combined_propagators(Vs, VCs)
                                        X #!= Y, X #!= Y + C (incl. abs)
     outof/3                            all_different
     '$alldistinct_outof'/4             all_distinct (FC + Hall check)
     '$alldistinct_primal_dual_var_eq'/4, ..._neq/4,
     '$alldistinct_dual_primal_var_eq'/3, ..._neq/3
                                        all_distinct on permutations
     '$linear_constr_eq_INT_aux'/2n+2, '$linear_constr_ge_aux'/2n+2,
     '$linear_constr_eq_ARC_aux'/2n+2   linear (in)equalities
     v_in_cv_dom/3, v_in_vc_dom/3 (+ '$v_in_cv_int'/3, '$v_in_vc_int'/3)
                                        X #= C - Y, X #= Y + C (arc consistent)
       args: (Type, C, A1..An, X1..Xn) meaning C + sum Ai*Xi = 0 / >= 0
       (emu/clpfd_libs.c nary_interval_consistent_eq/ge)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include "bprolog.h"
#include "frame.h"
#include "event.h"
#include "fdn.h"

extern PAR_TLS BPLONG_PTR stack_up_addr;
extern BPLONG n_backtracks;
extern int dm_true(BPLONG_PTR dv_ptr, BPLONG elm);
extern int count_cs_list(BPLONG list);

/* ---------------------------------------------------------------- var map */
typedef struct { BPLONG_PTR *keys; int *vals; int cap, n; } vmap;
static void vm_init(vmap *m) { m->cap = 1024; m->n = 0; m->keys = calloc(m->cap, sizeof *m->keys); m->vals = malloc(m->cap * sizeof(int)); }
static void vm_free(vmap *m) { free(m->keys); free(m->vals); }
static int vm_get(vmap *m, BPLONG_PTR k) {
    size_t h = ((size_t)k >> 3) * 0x9E3779B97F4A7C15ULL;
    for (int i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->keys[i]) return -1;
        if (m->keys[i] == k) return m->vals[i];
    }
}
static void vm_put(vmap *m, BPLONG_PTR k, int v) {
    if (2 * (m->n + 1) > m->cap) {
        vmap o = *m; m->cap *= 2; m->n = 0;
        m->keys = calloc(m->cap, sizeof *m->keys); m->vals = malloc(m->cap * sizeof(int));
        for (int i = 0; i < o.cap; i++) if (o.keys[i]) vm_put(m, o.keys[i], o.vals[i]);
        vm_free(&o);
    }
    size_t h = ((size_t)k >> 3) * 0x9E3779B97F4A7C15ULL;
    int i = h & (m->cap - 1);
    while (m->keys[i]) i = (i + 1) & (m->cap - 1);
    m->keys[i] = k; m->vals[i] = v; m->n++;
}

/* ---------------------------------------------------------------- probe */
static vmap PV; static BPLONG_PTR *PQ; static int PQn, PQcap;
static int pvar(BPLONG_PTR dv) {
    int id = vm_get(&PV, dv);
    if (id < 0) {
        id = PV.n; vm_put(&PV, dv, id);
        if (PQn == PQcap) { PQcap = PQcap ? 2 * PQcap : 256; PQ = realloc(PQ, PQcap * sizeof *PQ); }
        PQ[PQn++] = dv;
    }
    return id;
}
static void pterm(BPLONG t, int depth) {
    BPLONG_PTR top;
    DEREF(t);
    if (IS_SUSP_VAR(t)) { printf("v%d", pvar((BPLONG_PTR)UNTAGGED_TOPON_ADDR(t))); return; }
    if (ISINT(t)) { printf("%ld", (long)INTVAL(t)); return; }
    if (ISREF(t)) { printf("_"); return; }
    if (ISATOM(t)) { printf("%s", GET_NAME(GET_ATM_SYM_REC(t))); return; }
    if (depth > 6) { printf("..."); return; }
    if (ISLIST(t)) {
        int k = 0; printf("[");
        while (ISLIST(t)) {
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t);
            if (k++) printf(","); if (k > 40) { printf("..."); break; }
            pterm(FOLLOW(p), depth + 1); t = FOLLOW(p + 1); DEREF(t);
        }
        printf("]"); return;
    }
    if (ISSTRUCT(t)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); SYM_REC_PTR s = (SYM_REC_PTR)FOLLOW(p);
        int n = GET_ARITY(s); printf("%s(", GET_NAME(s));
        for (int i = 1; i <= n; i++) { if (i > 1) printf(","); if (i > 24) { printf("..."); break; } pterm(FOLLOW(p + i), depth + 1); }
        printf(")"); return;
    }
    printf("?");
}
static void pcs(const char *nm, BPLONG cs) {
    BPLONG_PTR top;
    DEREF(cs);
    while (ISLIST(cs)) {
        BPLONG_PTR sreg = (BPLONG_PTR)UNTAGGED_ADDR(cs);
        BPLONG_PTR f = (BPLONG_PTR)((BPULONG)stack_up_addr - (BPULONG)UNTAGGED_CONT(FOLLOW(sreg)));
        BPLONG_PTR reep = (BPLONG_PTR)AR_REEP(f);
        SYM_REC_PTR s = (SYM_REC_PTR)FOLLOW(reep + 2);
        BPLONG_PTR btm = (BPLONG_PTR)UNTAGGED_ADDR(AR_BTM(f));
        printf("   %s %s/%d%s (", nm, GET_NAME(s), GET_ARITY(s), FRAME_IS_DEAD(f) ? " DEAD" : "");
        for (BPLONG_PTR a = btm; a > f; a--) { pterm(FOLLOW(a), 0); if (a > f + 1) printf(","); }
        printf(")\n");
        cs = LIST_NEXT(sreg); DEREF(cs);
    }
}
int c_fdn_probe(void) {
    BPLONG L = ARG(1, 1); BPLONG_PTR top;
    vm_init(&PV); PQn = 0;
    DEREF(L);
    while (ISLIST(L)) { BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(L); BPLONG x = FOLLOW(p); DEREF(x);
        if (IS_SUSP_VAR(x)) pvar((BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)); L = FOLLOW(p + 1); DEREF(L); }
    for (int q = 0; q < PQn && q < 200; q++) {
        BPLONG_PTR dv = PQ[q];
        printf("v%d: size=%ld first=%ld last=%ld type=%s attached=", q, (long)DV_size(dv), (long)DV_first(dv), (long)DV_last(dv),
               IS_IT_DOMAIN(dv) ? "it" : IS_UN_DOMAIN(dv) ? "un" : "bv");
        pterm(DV_attached(dv), 0); printf("\n");
        pcs("ins", DV_ins_cs(dv)); pcs("minmax", DV_minmax_cs(dv)); pcs("dom", DV_dom_cs(dv)); pcs("outer", DV_outer_dom_cs(dv));
    }
    printf("total vars reached: %d\n", PQn);
    vm_free(&PV);
    return BP_TRUE;
}


/* ---------------------------------------------------------------- dyn arrays */
typedef struct { int *a; int n, cap; } ivec;
static void iv_push(ivec *v, int x) { if (v->n == v->cap) { v->cap = v->cap ? 2 * v->cap : 64; v->a = realloc(v->a, v->cap * sizeof(int)); } v->a[v->n++] = x; }
typedef struct { BPLONG_PTR *a; int n, cap; } pvec;
static void pv_push(pvec *v, BPLONG_PTR x) { if (v->n == v->cap) { v->cap = v->cap ? 2 * v->cap : 64; v->a = realloc(v->a, v->cap * sizeof(BPLONG_PTR)); } v->a[v->n++] = x; }

static int verbose = -1;
static char why[256];
static int unsupported(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(why, sizeof why, fmt, ap); va_end(ap); return 0;
}

/* ---------------------------------------------------------------- extraction */
enum { F_NEQ, F_OUTOF, F_ADOUT, F_PD, F_DP, F_MAPCV, F_MAPVC, F_IGN, F_LIN_EQ, F_LIN_GE, F_LIN_ARC };
typedef struct {
    vmap ids; pvec dvs;           /* FD variables, in discovery order */
    vmap fseen; pvec frames; ivec fkind;
    long glo, ghi;
} ext;

static SYM_REC_PTR S_neq, S_outof, S_adout, S_pdeq, S_pdneq, S_dpeq, S_dpneq, S_cvdom, S_vcdom, S_cvint, S_vcint;
static void init_syms(void) {
    if (S_neq) return;
    S_neq = BP_NEW_SYM("$combined_neq", 2); S_outof = BP_NEW_SYM("outof", 3);
    S_adout = BP_NEW_SYM("$alldistinct_outof", 4);
    S_pdeq = BP_NEW_SYM("$alldistinct_primal_dual_var_eq", 4); S_pdneq = BP_NEW_SYM("$alldistinct_primal_dual_var_neq", 4);
    S_dpeq = BP_NEW_SYM("$alldistinct_dual_primal_var_eq", 3); S_dpneq = BP_NEW_SYM("$alldistinct_dual_primal_var_neq", 3);
    S_cvdom = BP_NEW_SYM("v_in_cv_dom", 3); S_vcdom = BP_NEW_SYM("v_in_vc_dom", 3);
    S_cvint = BP_NEW_SYM("$v_in_cv_int", 3); S_vcint = BP_NEW_SYM("$v_in_vc_int", 3);
}
static int frame_kind(SYM_REC_PTR s) {
    if (s == S_neq) return F_NEQ;
    if (s == S_outof) return F_OUTOF;
    if (s == S_adout) return F_ADOUT;
    if (s == S_pdeq || s == S_pdneq) return F_PD;
    if (s == S_dpeq || s == S_dpneq) return F_DP;
    if (s == S_cvdom) return F_MAPCV;
    if (s == S_vcdom) return F_MAPVC;
    if (s == S_cvint || s == S_vcint) return F_IGN;   /* bounds part, subsumed by the value map */
    const char *n = GET_NAME(s); int len = GET_LENGTH(s), ar = GET_ARITY(s);
    if (ar >= 4 && ar % 2 == 0) {
        if (len == 25 && !strncmp(n, "$linear_constr_eq_INT_aux", 25)) return F_LIN_EQ;
        if (len == 21 && !strncmp(n, "$linear_constr_ge_aux", 21)) return F_LIN_GE;
        if (len == 25 && !strncmp(n, "$linear_constr_eq_ARC_aux", 25)) return F_LIN_ARC;
    }
    return -1;
}
static int ext_var(ext *e, BPLONG_PTR dv) {
    int id = vm_get(&e->ids, dv);
    if (id < 0) {
        id = e->dvs.n; vm_put(&e->ids, dv, id); pv_push(&e->dvs, dv);
        if (DV_first(dv) < e->glo) e->glo = DV_first(dv);
        if (DV_last(dv) > e->ghi) e->ghi = DV_last(dv);
    }
    return id;
}
static void ext_term(ext *e, BPLONG t) {          /* enqueue all FD variables in t */
    BPLONG_PTR top;
    for (;;) {
        DEREF(t);
        if (IS_SUSP_VAR(t)) { ext_var(e, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(t)); return; }
        if (ISINT(t)) return;
        if (ISLIST(t)) { BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); ext_term(e, FOLLOW(p)); t = FOLLOW(p + 1); continue; }
        if (ISSTRUCT(t)) {
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); int n = GET_ARITY((SYM_REC_PTR)FOLLOW(p));
            for (int i = 1; i < n; i++) ext_term(e, FOLLOW(p + i));
            t = FOLLOW(p + n); continue;
        }
        return;
    }
}
static BPLONG frame_arg(BPLONG_PTR f, int i) {    /* 1-based */
    BPLONG_PTR btm = (BPLONG_PTR)UNTAGGED_ADDR(AR_BTM(f));
    return FOLLOW(btm - (i - 1));
}
static int frame_arity(BPLONG_PTR f) { return (int)((BPLONG_PTR)UNTAGGED_ADDR(AR_BTM(f)) - f); }
static BPLONG neq_attr(BPLONG_PTR dv, int *ok) {  /* combined_propagators(..) or 0 */
    BPLONG_PTR top; BPLONG a = DV_attached(dv), res = 0;
    *ok = 1; DEREF(a);
    if (a == nil_sym || ISREF(a)) return 0;
    if (!ISSTRUCT(a)) { *ok = unsupported("attribute term"); return 0; }
    BPLONG lst = FOLLOW((BPLONG_PTR)UNTAGGED_ADDR(a) + 1); DEREF(lst);
    while (ISLIST(lst)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(lst);
        BPLONG pair = FOLLOW(p); DEREF(pair);
        BPLONG_PTR pp = (BPLONG_PTR)UNTAGGED_ADDR(pair);
        BPLONG nm = FOLLOW(pp + 1); DEREF(nm);
        if (nm == attr_neq_atm) { res = FOLLOW(pp + 2); DEREF(res); }
        else { *ok = unsupported("attribute %s", ISATOM(nm) ? GET_NAME(GET_ATM_SYM_REC(nm)) : "?"); return 0; }
        lst = FOLLOW(p + 1); DEREF(lst);
    }
    return res;
}
static int ext_cs(ext *e, BPLONG cs) {
    BPLONG_PTR top;
    DEREF(cs);
    while (ISLIST(cs)) {
        BPLONG_PTR sreg = (BPLONG_PTR)UNTAGGED_ADDR(cs);
        BPLONG_PTR f = (BPLONG_PTR)((BPULONG)stack_up_addr - (BPULONG)UNTAGGED_CONT(FOLLOW(sreg)));
        cs = LIST_NEXT(sreg); DEREF(cs);
        if (FRAME_IS_DEAD(f) || vm_get(&e->fseen, f) >= 0) continue;
        SYM_REC_PTR s = (SYM_REC_PTR)FOLLOW((BPLONG_PTR)AR_REEP(f) + 2);
        int k = frame_kind(s);
        if (k < 0) return unsupported("propagator %.*s/%d", GET_LENGTH(s), GET_NAME(s), (int)GET_ARITY(s));
        if (k >= F_LIN_EQ && frame_arity(f) != (int)GET_ARITY(s)) return unsupported("frame arity");
        vm_put(&e->fseen, f, e->frames.n); pv_push(&e->frames, f); iv_push(&e->fkind, k);
        for (int i = 1, n = frame_arity(f); i <= n; i++) ext_term(e, frame_arg(f, i));
    }
    return 1;
}
static int ext_collect(ext *e) {
    for (int q = 0; q < e->dvs.n; q++) {
        BPLONG_PTR dv = e->dvs.a[q]; int ok;
        if (IS_UN_DOMAIN(dv)) return unsupported("variable without finite domain");
        BPLONG at = neq_attr(dv, &ok);
        if (!ok) return 0;
        if (at) ext_term(e, at);
        if (!ext_cs(e, DV_ins_cs(dv)) || !ext_cs(e, DV_minmax_cs(dv)) || !ext_cs(e, DV_dom_cs(dv)) || !ext_cs(e, DV_outer_dom_cs(dv)))
            return 0;
        if (e->ghi - e->glo > 65535) return unsupported("value range %ld..%ld", e->glo, e->ghi);
    }
    return 1;
}

/* build-phase helpers */
typedef struct { ext *e; fdn_net *net; vmap consts; ivec cid; ivec cval; } bld;
static int b_id(bld *b, BPLONG t) {               /* variable id of an FD var or an integer (constant var) */
    BPLONG_PTR top; DEREF(t);
    if (IS_SUSP_VAR(t)) return vm_get(&b->e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(t));
    if (ISINT(t)) {
        int v = INTVAL(t);
        if (v < b->e->glo || v > b->e->ghi) return -1;   /* constant outside the value range */
        for (int i = 0; i < b->cval.n; i++) if (b->cval.a[i] == v) return b->cid.a[i];
        int id = fdn_var(b->net, 1, &v); iv_push(&b->cval, v); iv_push(&b->cid, id); return id;
    }
    return -1;
}
static int b_list(bld *b, BPLONG t, ivec *out) {  /* FD variables of a list (integers skipped) */
    BPLONG_PTR top; DEREF(t);
    while (ISLIST(t)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
        if (IS_SUSP_VAR(x)) iv_push(out, vm_get(&b->e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)));
        else if (!ISINT(x)) return 0;
        t = FOLLOW(p + 1); DEREF(t);
    }
    return t == nil_sym;
}
static int b_tuple(bld *b, BPLONG t) {
    BPLONG_PTR top; DEREF(t);
    if (!ISSTRUCT(t)) return -1;
    BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); int n = GET_ARITY((SYM_REC_PTR)FOLLOW(p));
    int *xs = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) if ((xs[i] = b_id(b, FOLLOW(p + 1 + i))) < 0) { free(xs); return -1; }
    int id = fdn_tuple(b->net, n, xs); free(xs); return id;
}
static int b_int(BPLONG t, long *v) { BPLONG_PTR top; DEREF(t); if (!ISINT(t)) return 0; *v = INTVAL(t); return 1; }

static int build(ext *e, fdn_net *net) {
    bld b = {e, net}; vm_init(&b.consts); int ok = 1;
    ivec L = {0};
    for (int q = 0; q < e->dvs.n && ok; q++) {    /* neq attributes */
        BPLONG at = neq_attr(e->dvs.a[q], &ok), top0;
        BPLONG_PTR top;
        if (!at) continue;
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(at);
        L.n = 0; ok = b_list(&b, FOLLOW(p + 1), &L);
        for (int i = 0; ok && i < L.n; i++) fdn_edge(net, q, L.a[i], 0);
        BPLONG vcs = FOLLOW(p + 2); DEREF(vcs);   /* [(Y,C)]: Y != X - C */
        while (ok && ISLIST(vcs)) {
            BPLONG_PTR c = (BPLONG_PTR)UNTAGGED_ADDR(vcs);
            BPLONG pr = FOLLOW(c); DEREF(pr);
            BPLONG_PTR pp = (BPLONG_PTR)UNTAGGED_ADDR(pr);
            BPLONG y = FOLLOW(pp + 1); long cc; DEREF(y);
            if (!b_int(FOLLOW(pp + 2), &cc)) { ok = unsupported("neq offset"); break; }
            if (IS_SUSP_VAR(y)) fdn_edge(net, q, vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)), (int)-cc);
            vcs = FOLLOW(c + 1); DEREF(vcs);
        }
        (void)top0;
    }
    for (int k = 0; k < e->frames.n && ok; k++) {
        BPLONG_PTR f = e->frames.a[k]; int kind = e->fkind.a[k];
        BPLONG x0 = frame_arg(f, 1); BPLONG_PTR top; DEREF(x0);
        int ox = IS_SUSP_VAR(x0) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x0)) : -1;
        long iv, off;
        switch (kind) {
        case F_NEQ: case F_IGN: break;
        case F_MAPCV: case F_MAPVC: {             /* v_in_cv_dom(X,C,Y): X = C-Y; v_in_vc_dom(X,Y,C): X = Y+C */
            BPLONG y = frame_arg(f, kind == F_MAPCV ? 3 : 2); long cc; DEREF(y);
            if (ox < 0 || !IS_SUSP_VAR(y) || !b_int(frame_arg(f, kind == F_MAPCV ? 2 : 3), &cc)) { ok = unsupported("binary eq frame"); break; }
            fdn_map(net, vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)), ox, kind == F_MAPCV ? -1 : 1, (int)cc);
            break;
        }
        case F_OUTOF: case F_ADOUT:
            if (ox < 0) { ok = unsupported("outof on integer"); break; }
            L.n = 0;
            ok = b_list(&b, frame_arg(f, kind == F_OUTOF ? 2 : 3), &L) && b_list(&b, frame_arg(f, kind == F_OUTOF ? 3 : 4), &L);
            for (int i = 0; ok && i < L.n; i++) fdn_edge(net, ox, L.a[i], 0);
            if (ok && kind == F_ADOUT) { iv_push(&L, ox); fdn_hall(net, L.n, L.a); }
            break;
        case F_PD: case F_DP: {
            int t = b_tuple(&b, frame_arg(f, kind == F_PD ? 4 : 3));
            if (ox < 0 || t < 0 || !b_int(frame_arg(f, 2), &iv)) { ok = unsupported("channel frame"); break; }
            if (kind == F_PD) { if (!b_int(frame_arg(f, 3), &off)) { ok = unsupported("channel offset"); break; } }
            else off = 0;
            fdn_chan(net, ox, t, (int)off, (int)iv);
            break;
        }
        default: {                                /* linear */
            int n = (frame_arity(f) - 2) / 2; long c;
            if (!b_int(frame_arg(f, 2), &c)) { ok = unsupported("linear constant"); break; }
            long *a = malloc(n * sizeof(long)); int *xs = malloc(n * sizeof(int)); int m = 0;
            for (int i = 0; ok && i < n; i++) {
                long ai, xv; BPLONG x = frame_arg(f, 3 + n + i); DEREF(x);
                if (!b_int(frame_arg(f, 3 + i), &ai)) { ok = unsupported("linear coefficient"); break; }
                if (b_int(x, &xv)) { c += ai * xv; continue; }
                if (!IS_SUSP_VAR(x)) { ok = unsupported("linear term"); break; }
                int id = vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x));
                a[m] = ai; xs[m] = id; m++;
            }
            if (ok) fdn_linear(net, kind == F_LIN_GE ? 1 : kind == F_LIN_ARC ? 2 : 0, c, m, a, xs);
            free(a); free(xs);
        }
        }
    }
    free(L.a); free(b.cid.a); free(b.cval.a); vm_free(&b.consts);
    return ok;
}

/* ---------------------------------------------------------------- handles */
typedef struct { fdn_run *r; int n; BPLONG *fixed; int *vals; } handle;
static handle **HT; static int HTn;

static int parse_opts(BPLONG opts, int *heur, int *ffc) {
    BPLONG_PTR top; DEREF(opts);
    *heur = 0; *ffc = 0;
    while (ISLIST(opts)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(opts); BPLONG o = FOLLOW(p); DEREF(o);
        const char *s = ISATOM(o) ? GET_NAME(GET_ATM_SYM_REC(o)) : NULL;
        if (!s) return unsupported("labeling option (non-atom)");
        int len = GET_LENGTH(GET_ATM_SYM_REC(o));
        if (len == 2 && !strncmp(s, "ff", 2)) { *heur = 1; *ffc = 0; }
        else if (len == 3 && !strncmp(s, "ffc", 3)) { *heur = 1; *ffc = 1; }
        else if (len == 8 && !strncmp(s, "leftmost", 8)) { *heur = 0; *ffc = 0; }
        else if (len == 2 && !strncmp(s, "up", 2)) ;
        else return unsupported("labeling option %.*s", len, s);
        opts = FOLLOW(p + 1); DEREF(opts);
    }
    return opts == nil_sym ? 1 : unsupported("labeling options not a list");
}

static int degree(BPLONG_PTR dv) {                /* b_CONSTRAINTS_NUMBER_cf */
    int ok, n = count_cs_list(DV_ins_cs(dv));
    BPLONG at = neq_attr(dv, &ok);
    if (at) { BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(at); n += count_cs_list(FOLLOW(p + 1)) + count_cs_list(FOLLOW(p + 2)); }
    return n;
}

int c_fdn_start(void) {
    BPLONG Opts = ARG(1, 3), Vars = ARG(2, 3), H = ARG(3, 3);
    BPLONG_PTR top; int heur, ffc;
    if (verbose < 0) verbose = getenv("FDN_VERBOSE") != NULL;
    init_syms(); why[0] = 0;
    if (!parse_opts(Opts, &heur, &ffc)) goto fallback;
    ext e; memset(&e, 0, sizeof e); vm_init(&e.ids); vm_init(&e.fseen); e.glo = LONG_MAX; e.ghi = LONG_MIN;
    /* label list: FD variables and integers */
    int n = 0; BPLONG t = Vars; DEREF(t);
    while (ISLIST(t)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
        if (IS_SUSP_VAR(x)) ext_var(&e, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x));
        else if (!ISINT(x)) { unsupported("non-FD term in the label list"); goto free_e; }
        n++; t = FOLLOW(p + 1); DEREF(t);
    }
    if (t != nil_sym) { unsupported("label list"); goto free_e; }
    int nlab = e.dvs.n;
    if (nlab == 0) { unsupported("nothing to label"); goto free_e; }
    if (!ext_collect(&e)) goto free_e;
    fdn_net *net = fdn_net_new((int)e.glo, (int)e.ghi);
    {
        int *vals = malloc((e.ghi - e.glo + 1) * sizeof(int));
        for (int q = 0; q < e.dvs.n; q++) {
            BPLONG_PTR dv = e.dvs.a[q]; int k = 0;
            for (BPLONG v = DV_first(dv); v <= DV_last(dv); v++) if (dm_true(dv, v)) vals[k++] = (int)v;
            fdn_var(net, k, vals);
        }
        free(vals);
    }
    if (!build(&e, net)) { fdn_net_free(net); goto free_e; }
    /* label order: the label list (first occurrence of each variable) */
    {
        int *lab = malloc(n * sizeof(int)), *deg = malloc(n * sizeof(int)), m = 0;
        char *seen = calloc(nlab, 1);
        t = Vars; DEREF(t);
        while (ISLIST(t)) {
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
            if (IS_SUSP_VAR(x)) { int id = vm_get(&e.ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)); if (!seen[id]) { seen[id] = 1; lab[m++] = id; } }
            t = FOLLOW(p + 1); DEREF(t);
        }
        if (ffc) {   /* sort('>=') on (Count, Var): count descending, then variable address descending */
            for (int i = 0; i < m; i++) deg[i] = degree(e.dvs.a[lab[i]]);
            for (int i = 1; i < m; i++) {
                int li = lab[i], di = deg[i], j = i - 1;
                while (j >= 0 && (deg[j] < di || (deg[j] == di && e.dvs.a[lab[j]] < e.dvs.a[li]))) {
                    lab[j + 1] = lab[j]; deg[j + 1] = deg[j]; j--;
                }
                lab[j + 1] = li; deg[j + 1] = di;
            }
        }
        fdn_label(net, heur, m, lab);
        free(lab); free(deg); free(seen);
    }
    {
        handle *h = calloc(1, sizeof *h);
        h->n = n; h->fixed = malloc(n * sizeof(BPLONG)); h->vals = malloc(n * sizeof(int));
        t = Vars; DEREF(t); int i = 0;
        while (ISLIST(t)) {                       /* positions: label index or fixed integer */
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
            if (IS_SUSP_VAR(x)) { h->fixed[i] = 0; h->vals[i] = vm_get(&e.ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)); }
            else { h->fixed[i] = x; h->vals[i] = -1; }
            i++;
            t = FOLLOW(p + 1); DEREF(t);
        }
        /* map variable id -> position in the solver's label order */
        h->r = fdn_start(net); int nl = fdn_nlabel(h->r);
        int *pos = malloc(e.dvs.n * sizeof(int));
        const int *ids = fdn_label_ids(h->r);
        for (int j = 0; j < nl; j++) pos[ids[j]] = j;
        for (int j = 0; j < n; j++) if (h->vals[j] >= 0) h->vals[j] = pos[h->vals[j]];
        free(pos);
        int slot = 0; while (slot < HTn && HT[slot]) slot++;
        if (slot == HTn) { HT = realloc(HT, (HTn + 16) * sizeof *HT); memset(HT + HTn, 0, 16 * sizeof *HT); HTn += 16; }
        HT[slot] = h;
        if (verbose) fprintf(stderr, "fdn: native search (%d vars, %d frames, %d label, values %ld..%ld)\n",
                             e.dvs.n, e.frames.n, nl, e.glo, e.ghi);
        vm_free(&e.ids); vm_free(&e.fseen); free(e.dvs.a); free(e.frames.a); free(e.fkind.a);
        return unify(H, MAKEINT(slot));
    }
free_e:
    vm_free(&e.ids); vm_free(&e.fseen); free(e.dvs.a); free(e.frames.a); free(e.fkind.a);
fallback:
    if (verbose) fprintf(stderr, "fdn: fallback to Picat labeling: %s\n", why);
    return BP_FALSE;
}

int c_fdn_next(void) {
    BPLONG H = ARG(1, 2), Vals = ARG(2, 2), top0; BPLONG_PTR top;
    DEREF(H); int slot = INTVAL(H); (void)top0;
    if (slot < 0 || slot >= HTn || !HT[slot]) return BP_FALSE;
    handle *h = HT[slot];
    static int *buf; static int bufn;
    int nl = fdn_nlabel(h->r);
    if (nl > bufn) { bufn = nl; buf = realloc(buf, bufn * sizeof(int)); }
    long bt;
    int got = fdn_next(h->r, buf, &bt);
    n_backtracks += bt;
    if (!got) {
        if (verbose) fprintf(stderr, "fdn: search done (%s)\n", fdn_stats(h->r));
        fdn_free(h->r); free(h->fixed); free(h->vals); free(h); HT[slot] = NULL;
        return BP_FALSE;
    }
    LOCAL_OVERFLOW_CHECK_WITH_MARGIN("fdn", 2 * h->n + 64);
    BPLONG lst = nil_sym;
    for (int i = h->n - 1; i >= 0; i--) {
        BPLONG v = h->vals[i] >= 0 ? MAKEINT(buf[h->vals[i]]) : h->fixed[i];
        FOLLOW(heap_top) = v; FOLLOW(heap_top + 1) = lst;
        lst = ADDTAG(heap_top, LST); heap_top += 2;
    }
    return unify(Vals, lst);
}

static void fdn_dump_syms(const char *sub) {
    for (long b = 0; b < BUCKET_CHAIN; b++)
        for (SYM_REC_PTR s = sym_hash_table[b]; s; s = GET_NEXT(s))
            if (strstr(GET_NAME(s), sub)) printf("sym %.*s/%d etype=%d\n", GET_LENGTH(s), GET_NAME(s), (int)GET_ARITY(s), GET_ETYPE(s));
}

void fdn_boot(void) {
    insert_cpred("c_fdn_probe", 1, c_fdn_probe);
    insert_cpred("c_fdn_start", 3, c_fdn_start);
    insert_cpred("c_fdn_next", 2, c_fdn_next);
}

void fdn_init(void) {
    char path[4096]; const char *e = getenv("FDN");
    if (e && !strcmp(e, "0")) return;
    if ((e = getenv("FDN_HOOK"))) snprintf(path, sizeof path, "%s", e);
    else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 32);
        if (n < 0) return;
        path[n] = 0; char *sl = strrchr(path, '/'); strcpy(sl ? sl + 1 : path, "fdn_hook.pi");
    }
    if (access(path, R_OK) != 0) { fprintf(stderr, "fdn: %s not found, native solver disabled\n", path); return; }
    bp_call_term(ADDTAG(insert_sym("initialize_bp", 13, 0), ATM));
    BPLONG_PTR h = heap_top;
    FOLLOW(h) = (BPLONG)insert_sym("picat_load", 10, 1);
    FOLLOW(h + 1) = ADDTAG(insert_sym(path, strlen(path), 0), ATM);
    heap_top += 2;
    bp_call_term(ADDTAG(h, STR));
    SYM_REC_PTR lab = insert_sym("labeling", 8, 2), orig = insert_sym("$fdn_orig_labeling", 18, 2),
        hook = insert_sym("e$$fdn_hook$$fdn_labeling", 25, 2);
    if (getenv("FDN_DEBUG")) fdn_dump_syms("fdn_");
    if (GET_ETYPE(hook) != T_PRED || GET_ETYPE(lab) != T_PRED) {
        fprintf(stderr, "fdn: hook not installed (labeling etype %d, hook etype %d)\n", GET_ETYPE(lab), GET_ETYPE(hook));
        return;
    }
    GET_ETYPE(orig) = GET_ETYPE(lab); GET_EP(orig) = GET_EP(lab);
    GET_EP(lab) = GET_EP(hook);
}
