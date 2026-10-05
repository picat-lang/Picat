/* fdn.c: Picat side of picat-fdn (see fdaccel/README.md).

   - fdn_init(): at start-up, loads fdn_hook.pi and points labeling/2 (called
     by cp's solve/1,2) at fdn_hook's fdn_labeling/2; the original code stays
     reachable as '$fdn_orig_labeling'/2.
   - c_fdn_start(VS, US, Vars, Path, H): extracts the constraint network
     reachable from Vars out of the live constraint store (FD variables, their
     suspension lists and the neq attribute) and starts the native search.
     VS / US are the variable and value strategies as Picat's
     labeling_var_strategy/2 and labeling_val_strategy/2 return them, and
     Vars is already reordered by labeling_reorder_vars/3 (fdn_hook.pi does
     both). It fails, and Picat labels as usual, when anything is outside
     the supported vocabulary: an unknown suspended propagator, an attribute
     other than the neq one, a strategy other than leftmost / ff / min /
     max / ff_min / ff_max and up / down / updown / split / reverse_split,
     or a value range wider than 65536.
   - c_fdn_next(H, Vals): next solution in Picat's order, or fails;
     c_fdn_nextp(H, Vars, Vals, Path) also returns its decision path (runs
     started with Path = true), replayed by fdn_hook.pi before Picat's rest
     step.
   - count_all/2 is repointed the same way at fdn_hook's fdn_count_all/2
     (original: '$fdn_orig_count_all'/2); for a bare cp solve goal,
     c_fdn_count(VS, US, Vars, Count) counts the solutions natively.
   - branch and bound (solve with min/max, fdn_hook.pi fdn_bb):
     c_fdn_bb(VS, US, Vars, Obj, Path, H) extracts the network once;
     c_fdn_round(H, Ub, R) starts one round, the search with Obj <= Ub,
     whose solutions c_fdn_next(R, Vals) returns; c_fdn_close(H).

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
      '$fd_abs_diff_eq'/3                abs(X-Y) #= C (domain + bounds)
      clpfd_multiply_fast/3, clpfd_multiply_slow/3
                                         X*Y #= Z (rule replay)
      '$fd_idiv_check_int'/3, '$fd_idiv_check_arc'/3,
      '$fd_floored_div_slow'/3           X div Y #= Z (Y fixed, X >= 0)
      '$fd_mod_check_fast'/3, '$fd_mod_check_slow'/3
                                         X mod Y #= Z (Y fixed)
      '$constr_min'/n, '$constr_max'/n   R #= min([X..]) / max([X..])
      reify_veqc_constr/3, reify_vneqc_constr/3,
      reify_eq_constr_fast/3, reify_neq_constr_fast/3,
      reify_ge_constr/3                  B <=> X #= Y / X #!= Y / X #>= Y
      clp_interp_b_entail_veqc_fast/3, ..._vneqc_fast/3,
      ..._veqv_fast/3, ..._vneqv_fast/3  B => X #= Y / X #!= Y
      '$element_delay'/3, '$element_ins_I'/3
                                         element(I, Tuple, V): I fixed
      '$element_ins_V'/4                 element: V fixed (range) + FC
      e$$cp$$watch_lex_lt/4, ..._le/4    lex chains
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
enum { F_NEQ, F_OUTOF, F_ADOUT, F_PD, F_DP, F_MAPCV, F_MAPVC, F_IGN, F_BCV, F_BVC, F_LIN_EQ, F_LIN_GE, F_LIN_ARC,
       F_ABS, F_MUL, F_DIV, F_MOD, F_MIN, F_MAX,
       F_VEQC, F_VNEQC, F_REIF_EQ, F_REIF_NEQ, F_REIF_GE,
       F_ENT_VEQC, F_ENT_VNEQC, F_ENT_VEQV, F_ENT_VNEQV,
       F_ELDELAY, F_ELFAST, F_ELFC, F_LEX_LT, F_LEX_LE };
typedef struct {
    vmap ids; pvec dvs;           /* FD variables, in discovery order */
    vmap fseen; pvec frames; ivec fkind;
    long glo, ghi;
} ext;

static SYM_REC_PTR S_neq, S_outof, S_adout, S_pdeq, S_pdneq, S_dpeq, S_dpneq, S_cvdom, S_vcdom, S_cvint, S_vcint;
static SYM_REC_PTR S_abs, S_mul, S_div, S_mod;
static SYM_REC_PTR S_veqc, S_vneqc, S_req, S_rneq, S_rge, S_entc1, S_entc2, S_entv1, S_entv2;
static SYM_REC_PTR S_eldelay, S_eli, S_elv, S_eliv, S_elvi;
static void init_syms(void) {
    if (S_neq) return;
    S_neq = BP_NEW_SYM("$combined_neq", 2); S_outof = BP_NEW_SYM("outof", 3);
    S_adout = BP_NEW_SYM("$alldistinct_outof", 4);
    S_pdeq = BP_NEW_SYM("$alldistinct_primal_dual_var_eq", 4); S_pdneq = BP_NEW_SYM("$alldistinct_primal_dual_var_neq", 4);
    S_dpeq = BP_NEW_SYM("$alldistinct_dual_primal_var_eq", 3); S_dpneq = BP_NEW_SYM("$alldistinct_dual_primal_var_neq", 3);
    S_cvdom = BP_NEW_SYM("v_in_cv_dom", 3); S_vcdom = BP_NEW_SYM("v_in_vc_dom", 3);
    S_cvint = BP_NEW_SYM("$v_in_cv_int", 3); S_vcint = BP_NEW_SYM("$v_in_vc_int", 3);
    S_abs = BP_NEW_SYM("$fd_abs_diff_eq", 3);
    S_mul = BP_NEW_SYM("clpfd_multiply_fast", 3);
    S_div = BP_NEW_SYM("$fd_idiv_check_int", 3);
    S_mod = BP_NEW_SYM("$fd_mod_check_fast", 3);
    S_veqc = BP_NEW_SYM("reify_veqc_constr", 3); S_vneqc = BP_NEW_SYM("reify_vneqc_constr", 3);
    S_req = BP_NEW_SYM("reify_eq_constr_fast", 3); S_rneq = BP_NEW_SYM("reify_neq_constr_fast", 3);
    S_rge = BP_NEW_SYM("reify_ge_constr", 3);
    S_entc1 = BP_NEW_SYM("clp_interp_b_entail_veqc_fast", 3); S_entc2 = BP_NEW_SYM("clp_interp_b_entail_vneqc_fast", 3);
    S_entv1 = BP_NEW_SYM("clp_interp_b_entail_veqv_fast", 3); S_entv2 = BP_NEW_SYM("clp_interp_b_entail_vneqv_fast", 3);
    S_eldelay = BP_NEW_SYM("$element_delay", 3); S_eli = BP_NEW_SYM("$element_ins_I", 3);
    S_elv = BP_NEW_SYM("$element_ins_V", 4);
    S_eliv = BP_NEW_SYM("$element_I_to_V", 4); S_elvi = BP_NEW_SYM("$element_V_to_I", 3);
}
static int frame_kind(SYM_REC_PTR s) {
    if (s == S_neq) return F_NEQ;
    if (s == S_outof) return F_OUTOF;
    if (s == S_adout) return F_ADOUT;
    if (s == S_pdeq || s == S_pdneq) return F_PD;
    if (s == S_dpeq || s == S_dpneq) return F_DP;
    if (s == S_cvdom) return F_MAPCV;
    if (s == S_vcdom) return F_MAPVC;
    if (s == S_cvint) return F_BCV;   /* bounds part of X = C-Y: not subsumed by the value map */
    if (s == S_vcint) return F_BVC;   /* once a unification left the domains inconsistent */
    if (s == S_abs) return F_ABS;
    if (s == S_mul) return F_MUL;
    if (s == S_div) return F_DIV;
    if (s == S_mod) return F_MOD;
    if (s == S_veqc) return F_VEQC;
    if (s == S_vneqc) return F_VNEQC;
    if (s == S_req) return F_REIF_EQ;
    if (s == S_rneq) return F_REIF_NEQ;
    if (s == S_rge) return F_REIF_GE;
    if (s == S_entc1) return F_ENT_VEQC;
    if (s == S_entc2) return F_ENT_VNEQC;
    if (s == S_entv1) return F_ENT_VEQV;
    if (s == S_entv2) return F_ENT_VNEQV;
    if (s == S_eldelay || s == S_eli) return F_ELDELAY;
    if (s == S_elv) return F_ELFAST;
    if (s == S_eliv || s == S_elvi) return F_ELFC;
    const char *n = GET_NAME(s); int len = GET_LENGTH(s), ar = GET_ARITY(s);
    if (ar >= 4 && ar % 2 == 0) {
        if (len == 25 && !strncmp(n, "$linear_constr_eq_INT_aux", 25)) return F_LIN_EQ;
        if (len == 21 && !strncmp(n, "$linear_constr_ge_aux", 21)) return F_LIN_GE;
        if (len == 25 && !strncmp(n, "$linear_constr_eq_ARC_aux", 25)) return F_LIN_ARC;
    }
    if (ar >= 3) {
        if (len >= 15 && !strncmp(n, "clpfd_multiply_", 15)) return F_MUL;
        if (len >= 15 && (!strncmp(n, "$fd_idiv_check_", 15) || !strncmp(n, "$fd_floored_div", 15))) return F_DIV;
        if (len >= 14 && !strncmp(n, "$fd_mod_check_", 14)) return F_MOD;
        if (len == 11 && !strncmp(n, "$constr_min", 11)) return F_MIN;
        if (len == 11 && !strncmp(n, "$constr_max", 11)) return F_MAX;
    }
    if (ar == 4) {
        if (len >= 13 && strstr(n, "watch_lex_lt")) return F_LEX_LT;
        if (len >= 13 && strstr(n, "watch_lex_le")) return F_LEX_LE;
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
    for (int k = 0; k < e->frames.n && ok; k++) {
        BPLONG_PTR f = e->frames.a[k]; int kind = e->fkind.a[k];
        BPLONG x0 = frame_arg(f, 1); BPLONG_PTR top; DEREF(x0);
        int ox = IS_SUSP_VAR(x0) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x0)) : -1;
        long iv, off;
        if (getenv("FDN_DUMP")) {
            int ar = frame_arity(f);
            fprintf(stderr, "frame %d kind=%d ar=%d:", k, kind, ar);
            for (int ai = 1; ai <= ar; ai++) {
                BPLONG a = frame_arg(f, ai); DEREF(a);
                if (IS_SUSP_VAR(a)) fprintf(stderr, " v%d", vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(a)));
                else if (ISINT(a)) fprintf(stderr, " %d", (int)INTVAL(a));
                else fprintf(stderr, " t%p", (void *)a);
            }
            fprintf(stderr, "\n");
        }
        switch (kind) {
        case F_IGN: break;
        case F_BCV: case F_BVC: {                 /* '$v_in_cv_int'(X,C,Y): X = C-Y; '$v_in_vc_int'(X,Y,C): X = Y+C,
                                                      on bound/ins events of Y (emu_inst.h lab_v_in_cv_int) */
            BPLONG y = frame_arg(f, kind == F_BCV ? 3 : 2); long cc; DEREF(y);
            int tx = b_id(&b, frame_arg(f, 1));
            if (tx < 0 || !IS_SUSP_VAR(y) || !b_int(frame_arg(f, kind == F_BCV ? 2 : 3), &cc)) { ok = unsupported("binary eq bounds frame"); break; }
            fdn_bmap(net, vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)), tx, kind == F_BCV ? -1 : 1, (int)cc);
            break;
        }
        case F_NEQ: {   /* '$combined_neq'(X, combined_propagators(Ys, [(Y,C)])): X != Y, Y != X - C.
                           Taken from the frame, which is what Picat runs: after a
                           unification of two FD variables, the surviving variable
                           can carry the frame without the _$attr_neq attribute, or
                           two such frames */
            BPLONG cp = frame_arg(f, 2); DEREF(cp);
            if (ox < 0 || !ISSTRUCT(cp)) { ok = unsupported("neq frame"); break; }
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(cp);
            L.n = 0; ok = b_list(&b, FOLLOW(p + 1), &L);
            for (int i = 0; ok && i < L.n; i++) fdn_edge(net, ox, L.a[i], 0);
            BPLONG vcs = FOLLOW(p + 2); DEREF(vcs);
            while (ok && ISLIST(vcs)) {
                BPLONG_PTR c = (BPLONG_PTR)UNTAGGED_ADDR(vcs);
                BPLONG pr = FOLLOW(c); DEREF(pr);
                BPLONG_PTR pp = (BPLONG_PTR)UNTAGGED_ADDR(pr);
                BPLONG y = FOLLOW(pp + 1); long cc; DEREF(y);
                if (!b_int(FOLLOW(pp + 2), &cc)) { ok = unsupported("neq offset"); break; }
                if (IS_SUSP_VAR(y)) fdn_edge(net, ox, vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)), (int)-cc);
                vcs = FOLLOW(c + 1); DEREF(vcs);
            }
            break;
        }
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
            for (int i = 0; ok && i < L.n; i++)      /* after X = Y between two of its variables */
                for (int j = 0; ok && j <= i; j++)
                    if (L.a[i] == (j < i ? L.a[j] : ox)) ok = unsupported("variable repeated in all_different/all_distinct");
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
        case F_ABS: {                             /* abs(X-Y) #= N */
            long nn;
            BPLONG y = frame_arg(f, 2); DEREF(y);
            int oy = IS_SUSP_VAR(y) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)) : -1;
            if (ox < 0 || oy < 0 || !b_int(frame_arg(f, 3), &nn)) { ok = unsupported("abs frame"); break; }
            fdn_abs(net, ox, oy, (int)nn);
            break;
        }
        case F_MUL: {                             /* X*Y #= Z */
            /* the 2-var ARC rule is value-by-value on the operands' domains:
               superlinear in the width (measured 0.55 s at width 3721 vs
               0.02 s without the multiply), so every multiply falls back to
               Picat's own bounds propagation */
            ok = unsupported("multiply constraint");
            break;
        }
        case F_DIV: case F_MOD: {                 /* X div Y #= Z / X mod Y #= Z, Y fixed */
            BPLONG y = frame_arg(f, 2), z = frame_arg(f, 3); DEREF(y); DEREF(z);
            long yy;
            int oz = IS_SUSP_VAR(z) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(z)) : -1;
            if (ox < 0 || oz < 0 || !b_int(y, &yy) || yy == 0) { ok = unsupported("div frame"); break; }
            if (kind == F_DIV) fdn_div(net, ox, (int)yy, oz); else fdn_mod(net, ox, (int)yy, oz);
            break;
        }
        case F_MIN: case F_MAX: {                 /* R #= min([X..]) / max([X..]) */
            int n = frame_arity(f);
            int *ids = malloc((n - 1) * sizeof(int)); long *vals = malloc((n - 1) * sizeof(long));
            int m2 = 0;
            for (int i = 2; i <= n; i++) {
                BPLONG x = frame_arg(f, i); DEREF(x);
                int id = IS_SUSP_VAR(x) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)) : -1;
                long v = 0;
                if (id < 0 && !b_int(x, &v)) { ok = unsupported("min/max frame"); break; }
                ids[m2] = id; vals[m2] = v; m2++;
            }
            if (ok && ox >= 0) fdn_mm(net, kind == F_MIN, ox, m2, ids, vals);
            else if (ok) ok = unsupported("min/max result");
            free(ids); free(vals);
            break;
        }
        case F_VEQC: case F_VNEQC: {              /* B <=> X #= C / B <=> X #!= C */
            BPLONG b = frame_arg(f, 1); DEREF(b);
            int ob = IS_SUSP_VAR(b) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(b)) : -1;
            long cv; BPLONG x = frame_arg(f, 2); DEREF(x);
            int oxx = IS_SUSP_VAR(x) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)) : -1;
            if (ob < 0 || oxx < 0 || !b_int(frame_arg(f, 3), &cv)) { ok = unsupported("reify frame"); break; }
            fdn_reif(net, kind == F_VEQC ? 0 : 1, ob, oxx, (int)cv);
            break;
        }
        case F_REIF_EQ: case F_REIF_NEQ: case F_REIF_GE: {   /* B <=> X #= Y / #!= / #>= */
            BPLONG b = frame_arg(f, 1), x = frame_arg(f, 2), y = frame_arg(f, 3);
            DEREF(b); DEREF(x); DEREF(y);
            int ob = IS_SUSP_VAR(b) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(b)) : -1;
            int oxx = IS_SUSP_VAR(x) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)) : -1;
            int oy = IS_SUSP_VAR(y) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)) : -1;
            if (kind == F_REIF_GE) {
                if (ob < 0 || oxx < 0 || oy < 0) { ok = unsupported("reify ge frame"); break; }
                fdn_reif(net, 4, ob, oxx, oy);
                break;
            }
            if (ob < 0 || oxx < 0) { ok = unsupported("reify frame"); break; }
            if (oy >= 0) fdn_reif(net, kind == F_REIF_EQ ? 2 : 3, ob, oxx, oy);
            else {
                long cv;
                if (!b_int(y, &cv)) { ok = unsupported("reify frame"); break; }
                fdn_reif(net, kind == F_REIF_EQ ? 0 : 1, ob, oxx, (int)cv);
            }
            break;
        }
        case F_ENT_VEQC: case F_ENT_VNEQC: {      /* B => X #= C / B => X #!= C */
            BPLONG b = frame_arg(f, 1), x = frame_arg(f, 2); DEREF(b); DEREF(x);
            int ob = IS_SUSP_VAR(b) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(b)) : -1;
            int oxx = IS_SUSP_VAR(x) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)) : -1;
            long cv;
            if (ob < 0 || oxx < 0 || !b_int(frame_arg(f, 3), &cv)) { ok = unsupported("entail frame"); break; }
            fdn_entail(net, kind == F_ENT_VEQC ? 0 : 1, ob, oxx, (int)cv);
            break;
        }
        case F_ENT_VEQV: case F_ENT_VNEQV: {      /* B => X #= Y / B => X #!= Y */
            BPLONG b = frame_arg(f, 1), x = frame_arg(f, 2), y = frame_arg(f, 3);
            DEREF(b); DEREF(x); DEREF(y);
            int ob = IS_SUSP_VAR(b) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(b)) : -1;
            int oxx = IS_SUSP_VAR(x) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)) : -1;
            int oy = IS_SUSP_VAR(y) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y)) : -1;
            if (ob < 0 || oxx < 0 || oy < 0) { ok = unsupported("entail frame"); break; }
            fdn_entail(net, kind == F_ENT_VEQV ? 2 : 3, ob, oxx, oy);
            break;
        }
        case F_ELDELAY: {                         /* element(I, Tuple, V): I fixed -> V = Tuple[I] */
            BPLONG tup = frame_arg(f, 3);
            if (getenv("FDN_DUMP")) {
                BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(tup); int n = GET_ARITY((SYM_REC_PTR)FOLLOW(p));
                BPLONG iv = frame_arg(f, 1), vv2 = frame_arg(f, 2); DEREF(iv); DEREF(vv2);
                fprintf(stderr, "eldraw: I=");
                if (IS_SUSP_VAR(iv)) fprintf(stderr, "v%d", vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(iv))); else fprintf(stderr, "?");
                fprintf(stderr, " V=");
                if (IS_SUSP_VAR(vv2)) fprintf(stderr, "v%d", vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(vv2))); else fprintf(stderr, "?");
                fprintf(stderr, " ar=%d tup:", n);
                for (int i = 0; i < n; i++) { long vv; if (b_int(FOLLOW(p + 1 + i), &vv)) fprintf(stderr, " %ld", vv); else fprintf(stderr, " ?"); }
                fprintf(stderr, "\n");
            }
            int t = b_tuple(&b, tup);
            if (ox < 0 || t < 0) { ok = unsupported("element frame"); break; }
            BPLONG v = frame_arg(f, 2); DEREF(v);
            int ov = IS_SUSP_VAR(v) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(v)) : -1;
            if (ov < 0) { ok = unsupported("element frame"); break; }
            fdn_eldelay(net, ox, ov, t);
            break;
        }
        case F_ELFAST: {                          /* element: V fixed -> I in range; FC on removals */
            BPLONG v = frame_arg(f, 2); DEREF(v);
            int ov = IS_SUSP_VAR(v) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(v)) : -1;
            if (ox < 0 || ov < 0) { ok = unsupported("element frame"); break; }
            /* the shared tuple: from the element_delay / ins_I frame on the same index AND the same value variable
               (several elements can share the index variable; matching only the index would pair the wrong tuple) */
            int tf = -1;
            for (int k = 0; k < e->frames.n && tf < 0; k++) {
                if (e->fkind.a[k] != F_ELDELAY) continue;
                BPLONG i2 = frame_arg(e->frames.a[k], 1); DEREF(i2);
                if (!IS_SUSP_VAR(i2) || (BPLONG_PTR)UNTAGGED_TOPON_ADDR(i2) != e->dvs.a[ox]) continue;
                BPLONG u2 = frame_arg(e->frames.a[k], 2); DEREF(u2);
                if (u2 == v) tf = k;
            }
            if (tf < 0) { ok = unsupported("element tuple"); break; }
            /* the range table built from the tuple itself (the ground truth): value w -> I in
               [min,max] over the positions where tuple = w. The stock's hashtable walk is unreliable: it misses entries when the values land in different buckets. */
            BPLONG tup = frame_arg(e->frames.a[tf], 3); DEREF(tup);
            if (!ISSTRUCT(tup)) { ok = unsupported("element tuple"); break; }
            BPLONG_PTR tp = (BPLONG_PTR)UNTAGGED_ADDR(tup); int nt = GET_ARITY((SYM_REC_PTR)FOLLOW(tp));
            if (nt > 1024) { ok = unsupported("element tuple too long"); break; }
            long kk[1024], lo[1024], hi[1024]; int nsup = 0;
            for (int q = 0; q < nt; q++) {
                long tv; BPLONG te = FOLLOW(tp + 1 + q); DEREF(te);
                if (!b_int(te, &tv)) { ok = unsupported("element tuple value"); break; }
                int fnd = -1;
                for (int k = 0; k < nsup && fnd < 0; k++) if (kk[k] == tv) fnd = k;
                if (fnd < 0) { kk[nsup] = tv; lo[nsup] = q + 1; hi[nsup] = q + 1; nsup++; }
                else { if (q + 1 < lo[fnd]) lo[fnd] = q + 1; if (q + 1 > hi[fnd]) hi[fnd] = q + 1; }
            }
            if (!ok) break;
            const int tr_ = getenv("FDN_TRACE") != NULL;
            if (tr_) { fprintf(stderr, "elfast: nsup=%d:", nsup); for (int k = 0; k < nsup; k++) fprintf(stderr, " val%ld:[%ld,%ld]", kk[k], lo[k], hi[k]); fprintf(stderr, "\n"); }
            if (nsup == 0) { ok = unsupported("element tuple empty"); break; }
            int t = b_tuple(&b, tup);
            if (t < 0) { ok = unsupported("element tuple"); break; }
            fdn_elfast(net, ox, ov, t, nsup, kk, lo, hi);
            break;
        }
        case F_ELFC: {                            /* element FC on removals (I_to_V / V_to_I) */
            int four = frame_arity(f) == 4;
            BPLONG a1 = frame_arg(f, 1), a2 = frame_arg(f, 2); DEREF(a1); DEREF(a2);
            int oi = four ? (IS_SUSP_VAR(a1) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(a1)) : -1)
                          : (IS_SUSP_VAR(a2) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(a2)) : -1);
            int ov = four ? (IS_SUSP_VAR(a2) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(a2)) : -1)
                          : (IS_SUSP_VAR(a1) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(a1)) : -1);
            if (oi < 0 || ov < 0) { ok = unsupported("element frame"); break; }
            BPLONG vt = four ? a2 : a1;           /* the value variable term */
            int t = -1;
            for (int k = 0; k < e->frames.n && t < 0; k++) {
                if (e->fkind.a[k] != F_ELDELAY) continue;
                BPLONG i2 = frame_arg(e->frames.a[k], 1); DEREF(i2);
                if (!IS_SUSP_VAR(i2) || (BPLONG_PTR)UNTAGGED_TOPON_ADDR(i2) != e->dvs.a[oi]) continue;
                BPLONG u2 = frame_arg(e->frames.a[k], 2); DEREF(u2);
                if (u2 == vt) t = b_tuple(&b, frame_arg(e->frames.a[k], 3));
            }
            if (t < 0) { ok = unsupported("element tuple"); break; }
            fdn_elfc(net, oi, ov, t);
            break;
        }
        case F_LEX_LT: case F_LEX_LE: {           /* lex chain */
            ivec Xs = {0}, Ys = {0};
            if (!b_list(&b, frame_arg(f, 3), &Xs) || !b_list(&b, frame_arg(f, 4), &Ys) || Xs.n != Ys.n) {
                free(Xs.a); free(Ys.a); ok = unsupported("lex frame"); break;
            }
            int np = 1 + Xs.n;
            int *pr = malloc(2 * np * sizeof(int));
            BPLONG x0 = frame_arg(f, 1), y0 = frame_arg(f, 2); DEREF(x0); DEREF(y0);
            pr[0] = IS_SUSP_VAR(x0) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x0)) : -1;
            pr[1] = IS_SUSP_VAR(y0) ? vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(y0)) : -1;
            for (int i = 0; i < Xs.n; i++) { pr[2 + 2 * i] = Xs.a[i]; pr[3 + 2 * i] = Ys.a[i]; }
            free(Xs.a); free(Ys.a);
            for (int i = 0; i < 2 * np; i++) if (pr[i] < 0) { ok = unsupported("lex frame"); break; }
            if (ok) fdn_lex(net, kind == F_LEX_LE, np, pr);
            free(pr);
            break;
        }
        default: {                                /* linear */
            int n = (frame_arity(f) - 2) / 2; long c;
            if (!b_int(frame_arg(f, 2), &c)) { ok = unsupported("linear constant"); break; }
            long *a = malloc(n * sizeof(long)), *kv = malloc(n * sizeof(long)); int *xs = malloc(n * sizeof(int)); int m = 0;
            for (int i = 0; ok && i < n; i++) {   /* terms in frame order, constants in place (Picat's pass order) */
                long ai, xv; BPLONG x = frame_arg(f, 3 + n + i); DEREF(x);
                if (!b_int(frame_arg(f, 3 + i), &ai)) { ok = unsupported("linear coefficient"); break; }
                if (b_int(x, &xv)) { a[m] = ai; xs[m] = -1; kv[m] = xv; m++; continue; }
                if (!IS_SUSP_VAR(x)) { ok = unsupported("linear term"); break; }
                int id = vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x));
                for (int j = 0; j < m; j++)   /* after X = Y between two of its variables; Picat
                                                 treats the occurrences separately */
                    if (xs[j] == id) { ok = unsupported("variable repeated in a linear constraint"); break; }
                a[m] = ai; xs[m] = id; m++;
            }
            if (ok) fdn_linear(net, kind == F_LIN_GE ? 1 : kind == F_LIN_ARC ? 2 : 0, c, m, a, xs, kv);
            free(a); free(xs); free(kv);
        }
        }
    }
    free(L.a); free(b.cid.a); free(b.cval.a); vm_free(&b.consts);
    return ok;
}

/* ---------------------------------------------------------------- handles */
/* a search (r), or a branch and bound (net, obj: objective variable id or
   -1; round: the current round's search handle, whose parent it is) */
/* path: solutions carry their decision path (c_fdn_nextp); lidx: variable id ->
   label index, lpos: label index -> first position in Vars (both for paths) */
typedef struct handle { fdn_run *r; fdn_net *net; int n; BPLONG *fixed; int *vals; int obj;
                        struct handle *parent, *round; int slot; int path, nid, nl; int *lidx, *lpos; } handle;
static handle **HT; static int HTn;

static int atom_is(BPLONG t, const char *a) {
    BPLONG_PTR top; DEREF(t);
    if (!ISATOM(t)) return 0;
    SYM_REC_PTR s = GET_ATM_SYM_REC(t); int len = GET_LENGTH(s);
    return len == (int)strlen(a) && !strncmp(GET_NAME(s), a, len);
}
/* VS, US: the strategy atoms computed by labeling_var_strategy/2 and
   labeling_val_strategy/2 (fdn_hook.pi) */
static int parse_strat(BPLONG VS, BPLONG US, int *vs, int *us) {
    static const char *vn[] = {"leftmost", "ff", "min", "max", "ff_min", "ff_max"};
    static const char *un[] = {"up", "down", "updown", "split", "reverse_split"};
    *vs = *us = -1;
    for (int i = 0; i < 6; i++) if (atom_is(VS, vn[i])) *vs = i;
    for (int i = 0; i < 5; i++) if (atom_is(US, un[i])) *us = i;
    if (*vs < 0) return unsupported("variable strategy");
    if (*us < 0) return unsupported("value strategy");
    return 1;
}

static void ext_free(ext *e) {
    vm_free(&e->ids); vm_free(&e->fseen); free(e->dvs.a); free(e->frames.a); free(e->fkind.a);
}
static ext *ext_new(ext *e) {
    memset(e, 0, sizeof *e); vm_init(&e->ids); vm_init(&e->fseen); e->glo = LONG_MAX; e->ghi = LONG_MIN;
    return e;
}
/* the network for labeling Vars with strategies VS / US, or NULL with the
   reason in why; *nout = length of Vars */
static fdn_net *extract(BPLONG VS, BPLONG US, BPLONG Vars, ext *e, int *nout) {
    BPLONG_PTR top; int vs, us;
    if (verbose < 0) verbose = getenv("FDN_VERBOSE") != NULL;
    init_syms(); why[0] = 0;
    if (!parse_strat(VS, US, &vs, &us)) return NULL;
    /* label list: FD variables and integers */
    int n = 0; BPLONG t = Vars; DEREF(t);
    while (ISLIST(t)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
        if (IS_SUSP_VAR(x)) ext_var(e, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x));
        else if (!ISINT(x)) { unsupported("non-FD term in the label list"); return NULL; }
        n++; t = FOLLOW(p + 1); DEREF(t);
    }
    if (t != nil_sym) { unsupported("label list"); return NULL; }
    int nlab = e->dvs.n;
    if (nlab == 0) { unsupported("nothing to label"); return NULL; }
    /* indomain_split with min+max < 0 and odd: Mid = (min+max)/2 rounds up to
       max, the lower "half" is the whole domain and Picat splits forever */
    if (us == US_SPLIT || us == US_REVERSE_SPLIT)
        for (int q = 0; q < nlab; q++) if (DV_first(e->dvs.a[q]) < 0) { unsupported("split on negative values"); return NULL; }
    if (!ext_collect(e)) return NULL;
    if (e->frames.n > 5000 || e->dvs.n > 5000) { unsupported("network too large"); return NULL; }
    fdn_net *net = fdn_net_new((int)e->glo, (int)e->ghi);
    {
        int *vals = malloc((e->ghi - e->glo + 1) * sizeof(int));
        for (int q = 0; q < e->dvs.n; q++) {
            BPLONG_PTR dv = e->dvs.a[q]; int k = 0;
            for (BPLONG v = DV_first(dv); v <= DV_last(dv); v++) if (dm_true(dv, v)) vals[k++] = (int)v;
            fdn_var(net, k, vals);
        }
        free(vals);
    }
    if (!build(e, net)) { fdn_net_free(net); return NULL; }
    /* label order: the label list (first occurrence of each variable; the
       selection rules pick the first of equal candidates, so duplicates
       never matter). Reorderings (ffc, constr, degree, ffd, backward,
       inout) were done by Picat's own labeling_reorder_vars/3 */
    {
        int *lab = malloc(n * sizeof(int)), m = 0;
        char *seen = calloc(nlab, 1);
        t = Vars; DEREF(t);
        while (ISLIST(t)) {
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
            if (IS_SUSP_VAR(x)) { int id = vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)); if (!seen[id]) { seen[id] = 1; lab[m++] = id; } }
            t = FOLLOW(p + 1); DEREF(t);
        }
        fdn_label(net, vs, us, m, lab);
        free(lab); free(seen);
    }
    *nout = n;
    return net;
}

/* handle positions: Vars entry -> index in the label order, or a fixed
   integer */
static handle *new_handle(BPLONG Vars, int n, ext *e, fdn_net *net) {
    BPLONG_PTR top;
    handle *h = calloc(1, sizeof *h);
    h->n = n; h->fixed = malloc(n * sizeof(BPLONG)); h->vals = malloc(n * sizeof(int)); h->obj = -1;
    BPLONG t = Vars; DEREF(t); int i = 0;
    while (ISLIST(t)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
        if (IS_SUSP_VAR(x)) { h->fixed[i] = 0; h->vals[i] = vm_get(&e->ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x)); }
        else { h->fixed[i] = x; h->vals[i] = -1; }
        i++;
        t = FOLLOW(p + 1); DEREF(t);
    }
    int nl; const int *ids = fdn_net_label(net, &nl);
    int *pos = malloc(e->dvs.n * sizeof(int));
    for (int q = 0; q < e->dvs.n; q++) pos[q] = -1;
    for (int j = 0; j < nl; j++) pos[ids[j]] = j;
    for (int j = 0; j < n; j++) if (h->vals[j] >= 0) h->vals[j] = pos[h->vals[j]];
    h->lidx = pos; h->nid = e->dvs.n; h->nl = nl;
    h->lpos = malloc((nl ? nl : 1) * sizeof(int));
    for (int j = 0; j < nl; j++) h->lpos[j] = -1;
    for (int j = n - 1; j >= 0; j--) if (h->vals[j] >= 0) h->lpos[h->vals[j]] = j;
    return h;
}
static int put_handle(handle *h) {
    int slot = 0; while (slot < HTn && HT[slot]) slot++;
    if (slot == HTn) { HT = realloc(HT, (HTn + 16) * sizeof *HT); memset(HT + HTn, 0, 16 * sizeof *HT); HTn += 16; }
    HT[slot] = h; h->slot = slot; return slot;
}
static handle *get_handle(BPLONG H) {
    BPLONG_PTR top; DEREF(H);
    if (!ISINT(H)) return NULL;
    int slot = INTVAL(H);
    return slot < 0 || slot >= HTn ? NULL : HT[slot];
}
static void free_handle(handle *h) {
    if (h->r) fdn_free(h->r);
    if (h->parent) h->parent->round = NULL;
    HT[h->slot] = NULL; free(h->fixed); free(h->vals); free(h->lidx); free(h->lpos); free(h);
}
/* the solution list for the handle's positions (values in label order) */
static BPLONG sol_list(handle *h, const int *buf) {
    LOCAL_OVERFLOW_CHECK_WITH_MARGIN("fdn", 2 * h->n + 64);
    BPLONG lst = nil_sym;
    for (int i = h->n - 1; i >= 0; i--) {
        BPLONG v = h->vals[i] >= 0 ? MAKEINT(buf[h->vals[i]]) : h->fixed[i];
        FOLLOW(heap_top) = v; FOLLOW(heap_top + 1) = lst;
        lst = ADDTAG(heap_top, LST); heap_top += 2;
    }
    return lst;
}

/* Path: true = solutions carry their decision path, read with c_fdn_nextp
   (fdn_hook.pi, when Picat's rest step runs after each solution) */
static int is_true(BPLONG t) { return atom_is(t, "true"); }
int c_fdn_start(void) {
    BPLONG VS = ARG(1, 5), US = ARG(2, 5), Vars = ARG(3, 5), Path = ARG(4, 5), H = ARG(5, 5);
    ext e; int n;
    fdn_net *net = extract(VS, US, Vars, ext_new(&e), &n);
    if (!net) {
        ext_free(&e);
        if (verbose) fprintf(stderr, "fdn: fallback to Picat labeling: %s\n", why);
        return BP_FALSE;
    }
    handle *h = new_handle(Vars, n, &e, net);
    h->path = is_true(Path);
    h->r = fdn_start(net, h->path); int nl = fdn_nlabel(h->r);
    int slot = put_handle(h);
    if (verbose) fprintf(stderr, "fdn: native search (%d vars, %d frames, %d label, values %ld..%ld)\n",
                         e.dvs.n, e.frames.n, nl, e.glo, e.ghi);
    if (verbose) fprintf(stderr, "fdn: threads=%d\n", fdn_nthreads());
    ext_free(&e);
    return unify(H, MAKEINT(slot));
}

int c_fdn_next(void) {
    BPLONG H = ARG(1, 2), Vals = ARG(2, 2);
    handle *h = get_handle(H);
    if (!h || !h->r) return BP_FALSE;
    static int *buf; static int bufn;
    int nl = fdn_nlabel(h->r);
    if (nl > bufn) { bufn = nl; buf = realloc(buf, bufn * sizeof(int)); }
    long bt;
    int got = fdn_next(h->r, buf, &bt);
    n_backtracks += bt;
    if (!got) {
        if (verbose) fprintf(stderr, "fdn: search done (%s)\n", fdn_stats(h->r));
        free_handle(h);
        return BP_FALSE;
    }
    return unify(Vals, sol_list(h, buf));
}

/* c_fdn_nextp(H, Vars, Vals, Path): c_fdn_next, plus the solution's decision
   path in search order: eq(X, V) = X was assigned V, rg(X, L, U) = X was
   narrowed to L..U (split), X taken from Vars. Replaying it one call per
   decision (fdn_hook.pi fdn_replay) reproduces Picat's propagation events
   decision by decision. A single Vars = Vals unification runs the
   propagators once, after all bindings, which can leave other frames dead
   or alive than Picat's labeling does, and that changes what the rest step
   labels. */
int c_fdn_nextp(void) {
    BPLONG H = ARG(1, 4), Vars = ARG(2, 4), Vals = ARG(3, 4), Path = ARG(4, 4); BPLONG_PTR top;
    handle *h = get_handle(H);
    if (!h || !h->r) return BP_FALSE;
    static int *buf; static int bufn; static BPLONG *vt; static int vtn;
    int nl = fdn_nlabel(h->r);
    if (nl > bufn) { bufn = nl; buf = realloc(buf, bufn * sizeof(int)); }
    long bt;
    int got = fdn_next(h->r, buf, &bt);
    n_backtracks += bt;
    if (!got) {
        if (verbose) fprintf(stderr, "fdn: search done (%s)\n", fdn_stats(h->r));
        free_handle(h);
        return BP_FALSE;
    }
    int k; const int *d = fdn_last_path(h->r, &k);
    LOCAL_OVERFLOW_CHECK_WITH_MARGIN("fdn", 6 * k + 2 * h->n + 64);
    Vars = ARG(2, 4); Vals = ARG(3, 4); Path = ARG(4, 4);   /* after a possible collection */
    if (h->n > vtn) { vtn = h->n; vt = realloc(vt, vtn * sizeof(BPLONG)); }
    BPLONG t = Vars; DEREF(t); int i = 0;
    while (ISLIST(t) && i < h->n) { BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); vt[i++] = FOLLOW(p); t = FOLLOW(p + 1); DEREF(t); }
    if (i != h->n) return BP_FALSE;
    BPLONG lst = nil_sym;
    static SYM_REC_PTR S_eq, S_rg;
    if (!S_eq) { S_eq = BP_NEW_SYM("eq", 2); S_rg = BP_NEW_SYM("rg", 3); }
    for (int j = k - 1; j >= 0; j--) {
        int id = d[3 * j], lo = d[3 * j + 1], hi = d[3 * j + 2];
        int li = id >= 0 && id < h->nid ? h->lidx[id] : -1, pos = li >= 0 ? h->lpos[li] : -1;
        if (pos < 0) return BP_FALSE;                   /* cannot happen: decisions are on label variables */
        BPLONG_PTR f = heap_top;
        if (lo == hi) { FOLLOW(f) = (BPLONG)S_eq; FOLLOW(f + 1) = vt[pos]; FOLLOW(f + 2) = MAKEINT(lo); heap_top += 3; }
        else { FOLLOW(f) = (BPLONG)S_rg; FOLLOW(f + 1) = vt[pos]; FOLLOW(f + 2) = MAKEINT(lo); FOLLOW(f + 3) = MAKEINT(hi); heap_top += 4; }
        FOLLOW(heap_top) = ADDTAG(f, STR); FOLLOW(heap_top + 1) = lst;
        lst = ADDTAG(heap_top, LST); heap_top += 2;
    }
    BPLONG vl = nil_sym;                             /* as sol_list, within the same overflow check */
    for (int j = h->n - 1; j >= 0; j--) {
        FOLLOW(heap_top) = h->vals[j] >= 0 ? MAKEINT(buf[h->vals[j]]) : h->fixed[j]; FOLLOW(heap_top + 1) = vl;
        vl = ADDTAG(heap_top, LST); heap_top += 2;
    }
    return unify(Path, lst) && unify(Vals, vl);
}

/* c_fdn_bb(VS, US, Vars, Obj, Path, H): the network for a branch and bound with
   objective variable Obj (an FD variable, reached through the constraints
   on Vars or not, or an integer); no search yet. */
int c_fdn_bb(void) {
    BPLONG VS = ARG(1, 6), US = ARG(2, 6), Vars = ARG(3, 6), Obj = ARG(4, 6), Path = ARG(5, 6), H = ARG(6, 6);
    BPLONG_PTR top; ext e; int n;
    fdn_net *net = extract(VS, US, Vars, ext_new(&e), &n);
    if (!net) {
        ext_free(&e);
        if (verbose) fprintf(stderr, "fdn: fallback to Picat branch and bound: %s\n", why);
        return BP_FALSE;
    }
    handle *h = new_handle(Vars, n, &e, net);
    h->net = net; h->path = is_true(Path); DEREF(Obj);
    if (IS_SUSP_VAR(Obj)) h->obj = vm_get(&e.ids, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Obj));
    if (verbose) fprintf(stderr, "fdn: native branch and bound (%d vars, %d frames, values %ld..%ld)\n",
                         e.dvs.n, e.frames.n, e.glo, e.ghi);
    ext_free(&e);
    return unify(H, MAKEINT(put_handle(h)));
}
/* c_fdn_round(H, Ub, R): a new round of branch and bound H: the search with
   the objective variable narrowed to <= Ub, read with c_fdn_next(R, Vals).
   Picat leaves a round after its first accepted solution, so the previous
   round's search, if still open, is freed here (and by c_fdn_close). */
int c_fdn_round(void) {
    BPLONG H = ARG(1, 3), Ub = ARG(2, 3), R = ARG(3, 3); BPLONG_PTR top;
    handle *h = get_handle(H);
    if (!h || !h->net) return BP_FALSE;
    if (h->round) free_handle(h->round);
    DEREF(Ub);
    long ub = ISINT(Ub) ? INTVAL(Ub) : LONG_MAX / 4;   /* a big integer: no bound */
    handle *g = calloc(1, sizeof *g);
    g->n = h->n; g->obj = -1;
    g->fixed = malloc(h->n * sizeof(BPLONG)); memcpy(g->fixed, h->fixed, h->n * sizeof(BPLONG));
    g->vals = malloc(h->n * sizeof(int)); memcpy(g->vals, h->vals, h->n * sizeof(int));
    g->nid = h->nid; g->nl = h->nl; g->path = h->path;
    g->lidx = malloc(h->nid * sizeof(int)); memcpy(g->lidx, h->lidx, h->nid * sizeof(int));
    g->lpos = malloc((h->nl ? h->nl : 1) * sizeof(int)); memcpy(g->lpos, h->lpos, (h->nl ? h->nl : 1) * sizeof(int));
    g->r = fdn_start_round(h->net, h->obj, ub, h->path);
    g->parent = h; h->round = g;
    if (verbose) fprintf(stderr, "fdn: round (ub=%ld)\n", ub);
    return unify(R, MAKEINT(put_handle(g)));
}
int c_fdn_close(void) {
    BPLONG H = ARG(1, 1);
    handle *h = get_handle(H);
    if (h && h->net) {
        if (h->round) free_handle(h->round);
        fdn_net_free(h->net); free_handle(h);
    }
    return BP_TRUE;
}

/* c_fdn_alone(Vars): no live suspension frame refers to an FD variable
   outside the list Vars. Stock labeling, after each solution of Vars, labels
   the constrained variables left in the whole store once
   (retrieve_frozen_dvars: the variables of all live suspended goals,
   fd_labeling.pi); if this succeeds, that step has nothing to do. Walks the
   frame chain as c_frozen_f (emu/delay.c) does, without building the goals,
   and stops at the first outside variable. */
static int alone_term(vmap *m, BPLONG t, int depth) {
    BPLONG_PTR top;
    for (;;) {
        DEREF(t);
        if (IS_SUSP_VAR(t)) return vm_get(m, (BPLONG_PTR)UNTAGGED_TOPON_ADDR(t)) >= 0;
        if (depth > 10000) return 0;                 /* give up: treat as not alone */
        if (ISLIST(t)) { BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); if (!alone_term(m, FOLLOW(p), depth + 1)) return 0; t = FOLLOW(p + 1); depth++; continue; }
        if (ISSTRUCT(t)) {
            BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); int n = GET_ARITY((SYM_REC_PTR)FOLLOW(p));
            for (int i = 1; i < n; i++) if (!alone_term(m, FOLLOW(p + i), depth + 1)) return 0;
            t = FOLLOW(p + n); depth++; continue;
        }
        return 1;
    }
}
int c_fdn_alone(void) {
    BPLONG Vars = ARG(1, 1); BPLONG_PTR top;
    if (verbose < 0) verbose = getenv("FDN_VERBOSE") != NULL;
    vmap m; vm_init(&m); int ok = 1;
    BPLONG t = Vars; DEREF(t);
    while (ISLIST(t)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t); BPLONG x = FOLLOW(p); DEREF(x);
        if (IS_SUSP_VAR(x)) { BPLONG_PTR dv = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x); if (vm_get(&m, dv) < 0) vm_put(&m, dv, 0); }
        t = FOLLOW(p + 1); DEREF(t);
    }
    for (BPLONG_PTR f = sfreg; ok && AR_PREV(f) != (BPLONG)f; f = (BPLONG_PTR)AR_PREV(f)) {
        if (FRAME_IS_DEAD(f)) continue;
        for (int i = 1, n = frame_arity(f); ok && i <= n; i++) ok = alone_term(&m, frame_arg(f, i), 0);
    }
    vm_free(&m);
    if (!ok && verbose) fprintf(stderr, "fdn: other constrained variables in the store: labeled by Picat after each solution\n");
    return ok ? BP_TRUE : BP_FALSE;
}

/* c_fdn_count(VS, US, Vars, Count): the number of solutions of labeling
   Vars with strategies VS / US, counted natively (count_all(solve(..)) in
   fdn_hook.pi); adds Picat's backtracks for the whole search. Fails, before
   any search, when the network is unsupported. */
int c_fdn_count(void) {
    BPLONG VS = ARG(1, 4), US = ARG(2, 4), Vars = ARG(3, 4), C = ARG(4, 4);
    ext e; int n;
    fdn_net *net = extract(VS, US, Vars, ext_new(&e), &n);
    if (!net) {
        ext_free(&e);
        if (verbose) fprintf(stderr, "fdn: fallback to Picat count_all: %s\n", why);
        return BP_FALSE;
    }
    if (verbose) fprintf(stderr, "fdn: native count_all (%d vars, %d frames, values %ld..%ld)\n",
                         e.dvs.n, e.frames.n, e.glo, e.ghi);
    if (verbose) fprintf(stderr, "fdn: threads=%d\n", fdn_nthreads());
    ext_free(&e);
    fdn_run *r = fdn_start_count(net);
    long bt, cnt = fdn_count(r, &bt);
    n_backtracks += bt;
    if (verbose) fprintf(stderr, "fdn: count done (count=%ld, %s)\n", cnt, fdn_stats(r));
    fdn_free(r);
    return unify(C, MAKEINT(cnt));
}

static void fdn_dump_syms(const char *sub) {
    for (long b = 0; b < BUCKET_CHAIN; b++)
        for (SYM_REC_PTR s = sym_hash_table[b]; s; s = GET_NEXT(s))
            if (strstr(GET_NAME(s), sub)) printf("sym %.*s/%d etype=%d\n", GET_LENGTH(s), GET_NAME(s), (int)GET_ARITY(s), GET_ETYPE(s));
}

void fdn_boot(void) {
    insert_cpred("c_fdn_probe", 1, c_fdn_probe);
    insert_cpred("c_fdn_start", 5, c_fdn_start);
    insert_cpred("c_fdn_nextp", 4, c_fdn_nextp);
    insert_cpred("c_fdn_next", 2, c_fdn_next);
    insert_cpred("c_fdn_count", 4, c_fdn_count);
    insert_cpred("c_fdn_bb", 6, c_fdn_bb);
    insert_cpred("c_fdn_round", 3, c_fdn_round);
    insert_cpred("c_fdn_close", 1, c_fdn_close);
    insert_cpred("c_fdn_alone", 1, c_fdn_alone);
}

/* point name/2 at hook/2; the original code stays reachable as orig/2 */
static void repoint(const char *name, const char *orig, const char *hook) {
    SYM_REC_PTR p = insert_sym((char *)name, strlen(name), 2), o = insert_sym((char *)orig, strlen(orig), 2),
        h = insert_sym((char *)hook, strlen(hook), 2);
    if (GET_ETYPE(h) != T_PRED || GET_ETYPE(p) != T_PRED) {
        fprintf(stderr, "fdn: %s hook not installed (%s etype %d, hook etype %d)\n", name, name, GET_ETYPE(p), GET_ETYPE(h));
        return;
    }
    GET_ETYPE(o) = GET_ETYPE(p); GET_EP(o) = GET_EP(p);
    GET_EP(p) = GET_EP(h);
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
    if (getenv("FDN_DEBUG")) fdn_dump_syms("fdn_");
    repoint("labeling", "$fdn_orig_labeling", "e$$fdn_hook$$fdn_labeling");
    if (!((e = getenv("FDN_COUNT")) && !strcmp(e, "0")))
        repoint("count_all", "$fdn_orig_count_all", "e$$fdn_hook$$fdn_count_all");
}
