/********************************************************************
 *   File   : clpfd.c
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2026
 *   Purpose: Primitives on domain variables and constraints

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/
#include "bprolog.h"
#include <stdlib.h>
#include "event.h"
#include "clpfd.h"
#include "frame.h"
#define CALL_DOMAIN_PREV(dv_ptr, elm, prev) {           \
        if (IS_IT_DOMAIN(dv_ptr))                       \
            prev = elm-1;                               \
        else                                            \
            prev = domain_prev_bv(dv_ptr, elm-1);       \
    }

#define CALL_DOMAIN_NEXT(dv_ptr, elm, next) {           \
        if (IS_IT_DOMAIN(dv_ptr))                       \
            next = elm+1;                               \
        else                                            \
            next = domain_next_bv(dv_ptr, elm+1);       \
    }

#define UNIFY_DVAR_VAL(B, val) {                                \
        if (IS_SUSP_VAR(B)) {                                   \
            dv_ptr_b = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(B);      \
            ASSIGN_DVAR(dv_ptr_b, val);                         \
            return BP_TRUE;                                     \
        } else {                                                \
            return B == val;                                    \
        }                                                       \
    }

int dvar_bv(BPLONG op)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(op);
    if (IS_SUSP_VAR(op)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_ADDR(op);
        if (IS_BV_DOMAIN(dv_ptr)) return 1;
    }
    return 0;
}

int dvar_excludable_or_int(BPLONG op)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(op);
    if (ISINT(op)) return 1;
    if (IS_SUSP_VAR(op)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(op);
        if (IS_UN_DOMAIN(dv_ptr) ||
            (IS_IT_DOMAIN(dv_ptr) && !IS_SMALL_DOMAIN(dv_ptr))) return 0;
        return 1;
    }
    return 0;
}

int b_EXCLUDABLE_LIST_c(BPLONG list)
{
    BPLONG_PTR ptr, top;
    BPLONG op;
    DEREF(list);
    while (ISLIST(list)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(list);
        op = FOLLOW(ptr);
        if (!dvar_excludable_or_int(op)) return BP_FALSE;
        list = FOLLOW(ptr+1);
        DEREF(list);
    }
    if (ISNIL(list)) return BP_TRUE;
    bp_exception = illegal_arguments;
    return BP_ERROR;
}

int nondvar(BPLONG op)
{
    return (dvar(op) ? 0 : 1);
}

int dvar(BPLONG op)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(op);
    if (IS_SUSP_VAR(op)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(op);
        if (!IS_UN_DOMAIN(dv_ptr)) return 1;
    }
    return 0;
}

int dvar_or_int(BPLONG op)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(op);
    if (IS_SUSP_VAR(op)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(op);
        if (!IS_UN_DOMAIN(dv_ptr)) return 1;
    } else if (ISINT(op))
        return 1;
    return 0;
}

/* check while counting. Return -1 immediately if the count exceeds limit. */
int n_vars_gt(BPLONG count0, BPLONG op, BPLONG limit)
{
    BPLONG i, arity;
    BPLONG_PTR top;

    DEREF(op);
    SWITCH_OP(op, n_n_vars_gt,
              {count0++; if (count0 > limit) return -1;},

              {},

              {UNTAG_ADDR(op);
                  if ((count0 = n_vars_gt(count0, FOLLOW(op), limit)) == -1) return -1;
                  if ((count0 = n_vars_gt(count0, FOLLOW((BPLONG_PTR)op+1), limit)) == -1) return -1;},

              {UNTAG_ADDR(op);
                  arity = GET_ARITY((SYM_REC_PTR)FOLLOW(op));
                  for (i = 1; i <= arity; i++)
                      if ((count0 = n_vars_gt(count0, FOLLOW(((BPLONG_PTR)op+i)), limit)) == -1) return -1;},

              {count0++; if (count0 > limit) return -1;});
    return count0;
}

int trigger_vars_ins(BPLONG op)
{
    BPLONG i, arity;
    BPLONG_PTR top, dv_ptr;

    SWITCH_OP(op, n_trigger_vars,
              {CREATE_SUSP_VAR_ins(op, arreg);},

              {},

              {UNTAG_ADDR(op);
                  trigger_vars_ins(FOLLOW(op));
                  trigger_vars_ins(FOLLOW((BPLONG_PTR)op+1));},

              {UNTAG_ADDR(op);
                  arity = GET_ARITY((SYM_REC_PTR)FOLLOW(op));
                  for (i = 1; i <= arity; i++)
                      trigger_vars_ins(FOLLOW(((BPLONG_PTR)op+i)));},

              {INSERT_SUSP_CALL(op, A_DV_ins_cs(dv_ptr), arreg);});
    return 1;
}

int trigger_vars_minmax(BPLONG op)
{
    BPLONG i, arity;
    BPLONG_PTR top, dv_ptr;

    SWITCH_OP(op, n_trigger_vars,
              {CREATE_SUSP_VAR_minmax(op, arreg);},

              {},

              {UNTAG_ADDR(op);
                  trigger_vars_minmax(FOLLOW(op));
                  trigger_vars_minmax(FOLLOW((BPLONG_PTR)op+1));},

              {UNTAG_ADDR(op);
                  arity = GET_ARITY((SYM_REC_PTR)FOLLOW(op));
                  for (i = 1; i <= arity; i++)
                      trigger_vars_minmax(FOLLOW(((BPLONG_PTR)op+i)));},

              {INSERT_SUSP_CALL(op, A_DV_minmax_cs(dv_ptr), arreg);});
    return 1;
}

int trigger_vars_dom(BPLONG op)
{
    BPLONG i, arity;
    BPLONG_PTR top, dv_ptr;

    SWITCH_OP(op, n_trigger_vars,
              {CREATE_SUSP_VAR_dom(op, arreg);},

              {},

              {UNTAG_ADDR(op);
                  trigger_vars_dom(FOLLOW(op));
                  trigger_vars_dom(FOLLOW((BPLONG_PTR)op+1));},

              {UNTAG_ADDR(op);
                  arity = GET_ARITY((SYM_REC_PTR)FOLLOW(op));
                  for (i = 1; i <= arity; i++)
                      trigger_vars_dom(FOLLOW(((BPLONG_PTR)op+i)));},

              {INSERT_SUSP_CALL(op, A_DV_dom_cs(dv_ptr), arreg);});
    return 1;
}

int trigger_vars_any_dom(BPLONG op)
{
    BPLONG i, arity;
    BPLONG_PTR top, dv_ptr;

    SWITCH_OP(op, n_trigger_vars,
              {CREATE_SUSP_VAR_any_dom(op, arreg);},

              {},

              {UNTAG_ADDR(op);
                  trigger_vars_any_dom(FOLLOW(op));
                  trigger_vars_any_dom(FOLLOW((BPLONG_PTR)op+1));},

              {UNTAG_ADDR(op);
                  arity = GET_ARITY((SYM_REC_PTR)FOLLOW(op));
                  for (i = 1; i <= arity; i++)
                      trigger_vars_any_dom(FOLLOW(((BPLONG_PTR)op+i)));},

              {INSERT_SUSP_CALL(op, A_DV_dom_cs(dv_ptr), arreg);
                  INSERT_SUSP_CALL(op, A_DV_outer_dom_cs(dv_ptr), arreg);});
    return 1;
}

int exclude_elm_dvars() {
    BPLONG P_elm, P_list1, P_list2;

    P_elm = ARG(1, 3);
    P_list1 = ARG(2, 3);
    P_list2 = ARG(3, 3);

    return b_EXCLUDE_ELM_DVARS(P_elm, P_list1, P_list2);
}


/*
  Dense cache for the exclude_elm_* constraint lists
  (on by default; set CPDENSE=0 to disable).

  The constraint engine builds each exclusion list once and passes
  the same term lists to these builtins thousands of times during
  search (measured on queens-200: 192M cons-cell walks over 78K
  distinct entries; the lists are fully reused).  On first sight of
  a list we snapshot per-entry access data into dense C arrays;
  later calls walk the arrays instead of chasing scattered cons
  cells and pair terms.

  For every entry we store:
    - the heap slot that holds the variable ("vslot"); the value is
      read fresh from that slot on every call, so a variable that
      gets instantiated (susp var -> int) between calls is handled
      exactly as in the plain walk;
    - for VCS, the constant C of the (V,C) pair (an int, stable);
    - the list cell address, so the cached structure can be
      re-verified against the live term list.

  The cache is keyed by the head cell addresses of the (two) lists
  and guarded by:
    - an O(1) fingerprint on every call (last cell cdr, and the
      last pair for VCS), and
    - a full O(n) structural re-walk every CPDEN_VERIFY_EVERY hits.
  On any mismatch the arrays are freed, the key is blacklisted
  after a second failure, and the call falls back to the plain
  term walk, so any in-place mutation of the lists self-heals.
*/
static int cpdense_enabled(void)
{
    static int e = -1;
    if (e < 0) {
        const char *v = getenv("CPDENSE");
        e = !(v && v[0] == '0');
    }
    return e;
}

#define CPDEN_HT_BITS 14
#define CPDEN_HT_SIZE (1 << CPDEN_HT_BITS)
#define CPDEN_VERIFY_EVERY 64
#define CPDEN_MAX_ENTRIES (1L << 24)
#define CPDEN_BLACK_MAX 256

typedef struct cpden_rec {
    BPLONG_PTR *vslot;   /* slot holding the variable (VCS: pair arg1) */
    int *c;              /* per-entry constant (non-NULL only for VCS) */
    BPLONG *cell;        /* list cell untagged address per entry (both lists) */
    int n;               /* total entries */
    int n1;              /* entries from list1 (rest: list2) */
    unsigned long key[2];
    int hits;
    int bad;
} cpden_rec;

typedef struct {
    unsigned long key[2];
    int rec;
} cpden_slot;

static cpden_slot cpden_tab[CPDEN_HT_SIZE];
static cpden_rec *cpden_recs = (cpden_rec *)0;
static int cpden_nrecs = 0, cpden_cap = 0;
static long cpden_entries = 0;
static unsigned long cpden_black[2 * CPDEN_BLACK_MAX];
static int cpden_nblack = 0;

static int cpden_black_listed(const unsigned long k[2])
{
    int i;
    for (i = 0; i < cpden_nblack; i++)
        if (cpden_black[2*i] == k[0] && cpden_black[2*i+1] == k[1])
            return 1;
    return 0;
}

static void cpden_blacken(const unsigned long k[2])
{
    if (cpden_nblack < CPDEN_BLACK_MAX) {
        cpden_black[2*cpden_nblack] = k[0];
        cpden_black[2*cpden_nblack+1] = k[1];
        cpden_nblack++;
    }
}

static void cpden_keyhash(const unsigned long k[2], unsigned *h)
{
    *h = ((unsigned)(k[0] >> 4) ^ (unsigned)(k[1] >> 4)) & (CPDEN_HT_SIZE - 1);
}

static int cpden_find(const unsigned long k[2])
{
    unsigned h;
    cpden_keyhash(k, &h);
    for (;;) {
        if (cpden_tab[h].key[0] == 0 && cpden_tab[h].key[1] == 0)
            return -1;
        if (cpden_tab[h].key[0] == k[0] && cpden_tab[h].key[1] == k[1])
            return cpden_tab[h].rec;
        h = (h + 1) & (CPDEN_HT_SIZE - 1);
    }
}

static int cpden_new_rec(void)
{
    if (cpden_nrecs == cpden_cap) {
        int ncap = cpden_cap ? 2 * cpden_cap : 256;
        cpden_rec *nr = (cpden_rec *)realloc(cpden_recs, (size_t)ncap * sizeof(cpden_rec));
        if (!nr) return -1;
        cpden_recs = nr;
        cpden_cap = ncap;
    }
    return cpden_nrecs++;
}

static void cpden_clear_slot(int ri)
{
    unsigned long k[2] = { cpden_recs[ri].key[0], cpden_recs[ri].key[1] };
    unsigned h;
    cpden_keyhash(k, &h);
    for (;;) {
        if (cpden_tab[h].rec == ri) {
            cpden_tab[h].key[0] = cpden_tab[h].key[1] = 0;
            cpden_tab[h].rec = -1;
            break;
        }
        h = (h + 1) & (CPDEN_HT_SIZE - 1);
    }
}

static void cpden_drop(int ri)
{
    cpden_rec *r = &cpden_recs[ri];
    free(r->vslot); free(r->c); free(r->cell);
    r->vslot = (BPLONG_PTR *)0;
    r->c = (int *)0;
    r->cell = (BPLONG *)0;
    cpden_entries -= r->n;
    r->n = 0;
    cpden_clear_slot(ri);
}

/* O(1) fingerprint over the recorded tail cell. */
static int cpden_fp_ok(const cpden_rec *r)
{
    BPLONG_PTR p;
    if (r->n <= 0) return 1;
    p = (BPLONG_PTR)UNTAGGED_ADDR(r->cell[r->n - 1]);
    if (FOLLOW(p + 1) != nil_sym) return 0;
    if (r->c) { /* VCS: last car must still own the last vslot */
        BPLONG pair = FOLLOW(p);
        DEREF_NONVAR(pair);
        if ((BPLONG_PTR)UNTAGGED_ADDR(pair) + 1 != r->vslot[r->n - 1])
            return 0;
    }
    return 1;
}

/* full structural re-walk: the cdr chain must run through exactly
   the recorded cells and re-expose the recorded vslots. */
/* one segment must be a cons chain running through exactly the
   recorded cells and ending in nil */
static int cpden_verify_segment(const cpden_rec *r, int a, int b)
{
    BPLONG l;
    int i;

    if (a >= b) return 1; /* empty segment (e.g. list1 == nil) */
    l = r->cell[a];
    for (i = a; i < b; i++) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(l);
        if (l != r->cell[i]) return 0;
        if (r->c) { /* VCS: the car's pair must own this vslot */
            BPLONG pair = FOLLOW(p);
            DEREF_NONVAR(pair);
            if ((BPLONG_PTR)UNTAGGED_ADDR(pair) + 1 != r->vslot[i])
                return 0;
        } else if (p != r->vslot[i]) {
            return 0;
        }
        l = FOLLOW(p + 1);
        if (i + 1 < b && l != r->cell[i + 1]) return 0;
    }
    return l == nil_sym;
}

static int cpden_verify(const cpden_rec *r)
{
    if (!cpden_verify_segment(r, 0, r->n1)) return 0;
    return cpden_verify_segment(r, r->n1, r->n);
}

/* take ownership of the scratch buffers; caller guarantees n > 0 */
static void cpden_register(const unsigned long key[2], BPLONG_PTR *vslot,
                           int *c, BPLONG *cell, int n, int n1)
{
    int ri, h, fresh = 0;
    cpden_rec *r;

    if (cpden_entries + n > CPDEN_MAX_ENTRIES) {
        free(vslot); free(c); free(cell);
        return;
    }
    ri = cpden_find(key);
    if (ri >= 0) {
        r = &cpden_recs[ri];
        cpden_entries -= r->n;
        free(r->vslot); free(r->c); free(r->cell);
    } else {
        ri = cpden_new_rec();
        if (ri < 0) { free(vslot); free(c); free(cell); return; }
        fresh = 1;
        r = &cpden_recs[ri];
        h = 0;
        cpden_keyhash(key, &h);
        for (;;) {
            if (cpden_tab[h].key[0] == 0 && cpden_tab[h].key[1] == 0) break;
            h = (h + 1) & (CPDEN_HT_SIZE - 1);
        }
        cpden_tab[h].key[0] = key[0];
        cpden_tab[h].key[1] = key[1];
        cpden_tab[h].rec = ri;
    }
    /* shrink to the exact size (scratch grew in doublings) */
    {
        BPLONG_PTR *nv = (BPLONG_PTR *)realloc(vslot, (size_t)n * sizeof(BPLONG_PTR));
        int *nc = c ? (int *)realloc(c, (size_t)n * sizeof(int)) : (int *)0;
        BPLONG *ncell = (BPLONG *)realloc(cell, (size_t)n * sizeof(BPLONG));
        if (!nv || (c && !nc) || !ncell) {
            free(nv); if (nc) free(nc); free(ncell);
            r->vslot = (BPLONG_PTR *)0;
            r->c = (int *)0;
            r->cell = (BPLONG *)0;
            r->n = 0;
            r->n1 = 0;
            cpden_clear_slot(ri);
            if (fresh) cpden_nrecs = ri;
            return;
        }
        r->vslot = nv;
        r->c = nc;
        r->cell = ncell;
    }
    r->key[0] = key[0];
    r->key[1] = key[1];
    r->hits = 0;
    r->bad = 0;
    r->n = n;
    r->n1 = n1;
    cpden_entries += n;
}

/* dense walk for a cached record; returns 0 on conflict, 1 otherwise */
static int cpden_apply_dvar(const cpden_rec *r, BPLONG elm)
{
    BPLONG_PTR dv_ptr;
    int i;
    for (i = 0; i < r->n; i++) {
        BPLONG P_v = FOLLOW(r->vslot[i]);
        DEREF_NONVAR(P_v);
        if (IS_SUSP_VAR(P_v)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(P_v);
            domain_set_false_noint(dv_ptr, elm);
        } else if (P_v == MAKEINT(elm)) return 0;
    }
    return 1;
}

static int cpden_apply_vcs(const cpden_rec *r, BPLONG elm)
{
    BPLONG_PTR dv_ptr;
    int i;
    for (i = 0; i < r->n; i++) {
        BPLONG P_v = FOLLOW(r->vslot[i]);
        BPLONG xc = elm - r->c[i];
        DEREF_NONVAR(P_v);
        if (IS_SUSP_VAR(P_v)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(P_v);
            domain_set_false_noint(dv_ptr, xc);
        } else if (INTVAL(P_v) == xc) return 0;
    }
    return 1;
}

/* growable scratch buffers for first-sight walks */
typedef struct {
    BPLONG_PTR *vslot;
    int *c;        /* non-NULL only for VCS */
    BPLONG *cell;
    int n, cap;
    int n1;
    int want_c;
} cpden_scratch;

#define CPDEN_SCR_INIT(s) do { \
    (s)->vslot = (BPLONG_PTR *)0; (s)->c = (int *)0; (s)->cell = (BPLONG *)0; \
    (s)->n = (s)->cap = 0; (s)->n1 = 0; \
} while (0)

static int cpden_scratch_add(cpden_scratch *s, BPLONG_PTR vs, int c, BPLONG cell)
{
    if (s->n == s->cap) {
        int nc = s->cap ? 2 * s->cap : 64;
        s->vslot = (BPLONG_PTR *)realloc(s->vslot, (size_t)nc * sizeof(BPLONG_PTR));
        if (s->want_c) s->c = (int *)realloc(s->c, (size_t)nc * sizeof(int));
        s->cell = (BPLONG *)realloc(s->cell, (size_t)nc * sizeof(BPLONG));
        if (!s->vslot || (s->want_c && !s->c) || !s->cell) return 0;
        s->cap = nc;
    }
    s->vslot[s->n] = vs;
    if (s->want_c) s->c[s->n] = c;
    s->cell[s->n] = cell;
    s->n++;
    return 1;
}

static void cpden_scratch_free(cpden_scratch *s)
{
    free(s->vslot); free(s->c); free(s->cell);
    s->vslot = (BPLONG_PTR *)0;
    s->c = (int *)0;
    s->cell = (BPLONG *)0;
    s->n = s->cap = 0;
}

/* the list grew by tail-append: the recorded chain must still be an
   intact prefix, and the old tail cdr must now be a list cell. */
static int cpden_prefix_ok(const cpden_rec *r)
{
    BPLONG l;
    int i;

    if (!cpden_verify_segment(r, 0, r->n1)) return 0;
    if (r->n1 == r->n) return 1; /* only list1 exists and is intact */
    l = r->cell[r->n1];
    for (i = r->n1; i < r->n; i++) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(l);
        if (l != r->cell[i]) return 0;
        if (r->c) {
            BPLONG pair = FOLLOW(p);
            DEREF_NONVAR(pair);
            if ((BPLONG_PTR)UNTAGGED_ADDR(pair) + 1 != r->vslot[i])
                return 0;
        } else if (p != r->vslot[i]) {
            return 0;
        }
        l = FOLLOW(p + 1);
        if (i + 1 < r->n && l != r->cell[i + 1]) return 0;
    }
    return l != nil_sym && ISLIST(l) && !ISVAR(l);
}

/* extend the record with the appended cells (prefix known intact).
   Returns 0 on allocation failure (arrays left as they were). */
static int cpden_extend_tail(cpden_rec *r)
{
    BPLONG l;
    int n0, cap;
    BPLONG_PTR *nv;
    int *nc;
    BPLONG *ncell;

    l = FOLLOW((BPLONG_PTR)UNTAGGED_ADDR(r->cell[r->n - 1]) + 1);
    DEREF_NONVAR(l);
    n0 = r->n;
    cap = 2 * n0;
    nv = (BPLONG_PTR *)realloc(r->vslot, (size_t)cap * sizeof(BPLONG_PTR));
    if (r->c) nc = (int *)realloc(r->c, (size_t)cap * sizeof(int));
    else nc = (int *)0;
    ncell = (BPLONG *)realloc(r->cell, (size_t)cap * sizeof(BPLONG));
    if (!nv || (r->c && !nc) || !ncell) {
        free(nv); if (nc) free(nc); free(ncell);
        return 0;
    }
    r->vslot = nv; r->c = nc; r->cell = ncell;

    while (ISLIST(l)) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(l);
        if (r->n >= cap) {
            cap = 2 * cap;
            nv = (BPLONG_PTR *)realloc(r->vslot, (size_t)cap * sizeof(BPLONG_PTR));
            if (r->c) nc = (int *)realloc(r->c, (size_t)cap * sizeof(int));
            else nc = (int *)0;
            ncell = (BPLONG *)realloc(r->cell, (size_t)cap * sizeof(BPLONG));
            if (!nv || (r->c && !nc) || !ncell)
                return 0; /* inconsistent; caller drops */
            r->vslot = nv; r->c = nc; r->cell = ncell;
        }
        if (r->c) {
            BPLONG pair = FOLLOW(p);
            BPLONG Pc;
            DEREF_NONVAR(pair);
            Pc = FOLLOW(((BPLONG_PTR)UNTAGGED_ADDR(pair)) + 2);
            DEREF_NONVAR(Pc);
            r->vslot[r->n] = (BPLONG_PTR)UNTAGGED_ADDR(pair) + 1;
            r->c[r->n] = (int)INTVAL(Pc);
        } else {
            r->vslot[r->n] = p;
        }
        r->cell[r->n] = l;
        r->n++;
        l = FOLLOW(p + 1);
        DEREF_NONVAR(l);
    }
    cpden_entries += r->n - n0;
    return 1;
}

int b_EXCLUDE_ELM_DVARS(BPLONG P_elm, BPLONG P_list1, BPLONG P_list2)
{
    BPLONG elm, P_v, processing_part, P_list;
    BPLONG_PTR dv_ptr, ptr;
    int use_cache = cpdense_enabled();
    unsigned long key[2] = { 0, 0 };
    cpden_scratch s;
    int building = 0;

    DEREF_NONVAR(P_elm);
    elm = INTVAL(P_elm);

    if (use_cache) {
        BPLONG t1 = P_list1, t2 = P_list2;
        DEREF_NONVAR(t1);
        DEREF_NONVAR(t2);
        key[0] = (t1 == nil_sym) ? 0 : (unsigned long)(t1 & ~3);
        key[1] = (t2 == nil_sym) ? 0 : (unsigned long)(t2 & ~3);
        if ((key[0] | key[1]) && !cpden_black_listed(key)) {
            int ri = cpden_find(key);
            if (ri >= 0) {
                cpden_rec *rr = &cpden_recs[ri];
                if (cpden_fp_ok(rr)) {
                    rr->hits++;
                    if ((rr->hits % CPDEN_VERIFY_EVERY) != 1 || cpden_verify(rr))
                        return cpden_apply_dvar(rr, elm);
                } else if (rr->n > 0 && cpden_prefix_ok(rr)) {
                    if (cpden_extend_tail(rr)) return cpden_apply_dvar(rr, elm);
                }
                rr->bad++;
                if (rr->bad >= 2) cpden_blacken(rr->key);
                cpden_drop(ri);
            }
        }
        if ((key[0] | key[1]) && !cpden_black_listed(key)) {
            CPDEN_SCR_INIT(&s);
            building = 1;
        } else {
            CPDEN_SCR_INIT(&s);
        }
    }

    processing_part = 1;
    P_list = P_list1;

start:
    DEREF_NONVAR(P_list);
    while (P_list != nil_sym) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(P_list);
        P_v = FOLLOW(ptr);

        DEREF_NONVAR(P_v);
        if (IS_SUSP_VAR(P_v)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(P_v);
            domain_set_false_noint(dv_ptr, elm);
        } else if (P_v == P_elm) {
            if (building) cpden_scratch_free(&s);
            return 0;
        }

        if (building) {
            cpden_scratch_add(&s, ptr, 0, P_list);
            if (processing_part == 1) s.n1++;
        }

        P_list = FOLLOW(ptr+1);
        DEREF_NONVAR(P_list);
    }
    if (processing_part == 1) {
        P_list = P_list2;
        processing_part = 2;
        goto start;
    }
    if (building && s.n > 0) {
        cpden_register(key, s.vslot, s.c, s.cell, s.n, s.n1);
    } else if (building) {
        cpden_scratch_free(&s);
    }
    return 1;
}

/*
  exclude_elm_vcs(X,VCs)
  VCs=[(Y1,C1),...,(Yn,Cn)]
  ensure that Yi \= X-Ci
*/
int exclude_elm_vcs() {
    BPLONG elm, P_list;

    elm = ARG(1, 2);
    P_list = ARG(2, 2);
    return b_EXCLUDE_ELM_VCS(elm, P_list);
}

int b_EXCLUDE_ELM_VCS(BPLONG elm, BPLONG P_list)
{
    BPLONG xc;
    BPLONG_PTR dv_ptr, ptr;
    BPLONG P_pair, P_v, P_c;
    int use_cache = cpdense_enabled();
    unsigned long key[2] = { 0, 0 };
    cpden_scratch s;
    int building = 0;

    DEREF_NONVAR(elm);
    elm = INTVAL(elm);

    if (use_cache) {
        BPLONG t = P_list;
        DEREF_NONVAR(t);
        key[0] = (t == nil_sym) ? 0 : (unsigned long)(t & ~3);
        if (key[0] && !cpden_black_listed(key)) {
            int ri = cpden_find(key);
            if (ri >= 0) {
                cpden_rec *rr = &cpden_recs[ri];
                if (cpden_fp_ok(rr)) {
                    rr->hits++;
                    if ((rr->hits % CPDEN_VERIFY_EVERY) != 1 || cpden_verify(rr))
                        return cpden_apply_vcs(rr, elm);
                } else if (rr->n > 0 && cpden_prefix_ok(rr)) {
                    if (cpden_extend_tail(rr)) return cpden_apply_vcs(rr, elm);
                }
                rr->bad++;
                if (rr->bad >= 2) cpden_blacken(rr->key);
                cpden_drop(ri);
            }
        }
        if (key[0] && !cpden_black_listed(key)) {
            CPDEN_SCR_INIT(&s);
            s.want_c = 1;
            building = 1;
        } else {
            CPDEN_SCR_INIT(&s);
        }
    }

    DEREF_NONVAR(P_list);
    while (P_list != nil_sym) {
        BPLONG_PTR lp;
        BPLONG lraw;
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(P_list);
        lp = ptr;
        lraw = P_list;
        P_list = FOLLOW(ptr+1);

        P_pair = FOLLOW(ptr);
        DEREF_NONVAR(P_pair);
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(P_pair);

        P_v = FOLLOW(ptr+1);  /* (V,C) */
        DEREF_NONVAR(P_v);
        P_c = FOLLOW(ptr+2);  /* (V,C) */
        DEREF_NONVAR(P_c);

        xc = elm-INTVAL(P_c);
        if (IS_SUSP_VAR(P_v)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(P_v);
            domain_set_false_noint(dv_ptr, xc);
        } else if (INTVAL(P_v) == xc) {
            if (building) cpden_scratch_free(&s);
            return 0;
        }

        if (building)
            cpden_scratch_add(&s, ptr + 1, (int)INTVAL(P_c), lraw);

        DEREF_NONVAR(P_list);
    }
    if (building && s.n > 0) {
        cpden_register(key, s.vslot, s.c, s.cell, s.n, s.n);
    } else if (building) {
        cpden_scratch_free(&s);
    }
    return 1;
}

/*
  Select a variable based on the first-fail principle.
  No dereference needed because the list was just copied.
*/
int b_select_ff(BPLONG Vars, BPLONG BestVar)
{
    BPLONG Var;
    BPLONG_PTR dv_ptr, dv_ptr0, ptr;
    BPLONG size, size0;

    DEREF_NONVAR(Vars);
    while (ISLIST(Vars)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
        Var = FOLLOW(ptr); DEREF_NONVAR(Var);
        Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
        if (IS_SUSP_VAR(Var)) {
            dv_ptr0 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
            size0 = DV_size(dv_ptr0);  /* first dvar */
            while (ISLIST(Vars)) {
                if (size0 == 2) break;  /* no size can be smaller than 2 */
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
                Var = FOLLOW(ptr); DEREF_NONVAR(Var);
                Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
                if (IS_SUSP_VAR(Var)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
                    size = DV_size(dv_ptr);
                    if (size < size0) {
                        dv_ptr0 = dv_ptr; size0 = size;
                    }
                }
            }
            ASSIGN_v_heap_term(BestVar, (BPLONG)dv_ptr0);
            return 1;
        }
    }
    return 0;
}

/*
  Select a variable based on the first-fail principle,
  breaking ties by selecting a variable with the smallest lower bound.
*/
int b_SELECT_FF_MIN_cf(BPLONG Vars, BPLONG BestVar)
{
    BPLONG Var, size0, size;
    BPLONG_PTR dv_ptr, dv_ptr0, ptr;


    DEREF_NONVAR(Vars);
    while (ISLIST(Vars)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
        Var = FOLLOW(ptr); DEREF_NONVAR(Var);
        Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
        if (IS_SUSP_VAR(Var)) {
            dv_ptr0 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
            size0 = DV_size(dv_ptr0);
            while (ISLIST(Vars)) {
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
                Var = FOLLOW(ptr); DEREF_NONVAR(Var);
                Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
                if (IS_SUSP_VAR(Var)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
                    size = DV_size(dv_ptr);
                    if (size < size0 || (size == size0 && DV_first(dv_ptr) < DV_first(dv_ptr0))) {
                        dv_ptr0 = dv_ptr; size0 = size;
                    }
                }
            }
            ASSIGN_v_heap_term(BestVar, (BPLONG)dv_ptr0);
            return 1;
        }
    }
    return 0;
}

/*
  Select a variable based on the first-fail principle,
  breaking ties by selecting a variable with the largest uppper bound.
*/
int b_SELECT_FF_MAX_cf(BPLONG Vars, BPLONG BestVar)
{
    BPLONG Var, size, size0;
    BPLONG_PTR dv_ptr, dv_ptr0, ptr;

    DEREF_NONVAR(Vars);
    while (ISLIST(Vars)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
        Var = FOLLOW(ptr); DEREF_NONVAR(Var);
        Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
        if (IS_SUSP_VAR(Var)) {
            dv_ptr0 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
            size0 = DV_size(dv_ptr0);
            while (ISLIST(Vars)) {
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
                Var = FOLLOW(ptr); DEREF_NONVAR(Var);
                Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
                if (IS_SUSP_VAR(Var)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
                    size = DV_size(dv_ptr);
                    if (size < size0 || (size == size0 && DV_last(dv_ptr) > DV_last(dv_ptr0))) {
                        dv_ptr0 = dv_ptr; size0 = size;
                    }
                }
            }
            ASSIGN_v_heap_term(BestVar, (BPLONG)dv_ptr0);
            return 1;
        }
    }
    return 0;
}

/*
  Select a variable with the smallest lower bound, breaking ties by selecting 
  the left-most one with the smallest domain.
*/
int b_SELECT_MIN_cf(BPLONG Vars, BPLONG BestVar)
{
    BPLONG Var, min, min0;
    BPLONG_PTR dv_ptr, dv_ptr0, ptr;

    DEREF_NONVAR(Vars);
    while (ISLIST(Vars)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
        Var = FOLLOW(ptr); DEREF_NONVAR(Var);
        Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
        if (IS_SUSP_VAR(Var)) {
            dv_ptr0 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
            min0 = DV_first(dv_ptr0);
            while (ISLIST(Vars)) {
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
                Var = FOLLOW(ptr); DEREF_NONVAR(Var);
                Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
                if (IS_SUSP_VAR(Var)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
                    min = DV_first(dv_ptr);
                    if (min < min0 || (min == min0 && DV_size(dv_ptr) < DV_size(dv_ptr0))) {
                        dv_ptr0 = dv_ptr; min0 = min;
                    }
                }
            }
            ASSIGN_v_heap_term(BestVar, (BPLONG)dv_ptr0);
            return 1;
        }
    }
    return 0;
}

/*
  Select a variable with the smallest lower bound, breaking ties by selecting 
  the left-most one with the smallest domain.
*/
int b_SELECT_MAX_cf(BPLONG Vars, BPLONG BestVar)
{
    BPLONG Var, max0, max;
    BPLONG_PTR dv_ptr, dv_ptr0, ptr;

    DEREF_NONVAR(Vars);
    while (ISLIST(Vars)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
        Var = FOLLOW(ptr); DEREF_NONVAR(Var);
        Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
        if (IS_SUSP_VAR(Var)) {
            dv_ptr0 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
            max0 = DV_last(dv_ptr0);
            while (ISLIST(Vars)) {
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
                Var = FOLLOW(ptr); DEREF_NONVAR(Var);
                Vars = FOLLOW(ptr+1); DEREF_NONVAR(Vars);
                if (IS_SUSP_VAR(Var)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
                    max = DV_last(dv_ptr);
                    if (max > max0 || (max == max0 && DV_size(dv_ptr) < DV_size(dv_ptr0))) {
                        dv_ptr0 = dv_ptr; max0 = max;
                    }
                }
            }
            ASSIGN_v_heap_term(BestVar, (BPLONG)dv_ptr0);
            return 1;
        }
    }
    return 0;
}

int b_CONSTRAINTS_NUMBER_cf(BPLONG Var, BPLONG N)
{
    BPLONG_PTR ptr;
    BPLONG lst, count, attrs;
    BPLONG_PTR top;

    DEREF_NONVAR(Var);
    ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
    count = count_cs_list(DV_ins_cs(ptr));

    attrs = DV_attached(ptr);
    DEREF(attrs);
    if (ISSTRUCT(attrs)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(attrs);
        lst = FOLLOW(ptr+1);  /* $attrs(AttrValueList) */
        DEREF(lst);
        while (ISLIST(lst)) {
            BPLONG pair, attr_name, value;
            BPLONG_PTR pair_ptr;
            ptr = (BPLONG_PTR)UNTAGGED_ADDR(lst);
            pair = FOLLOW(ptr); DEREF(pair);
            lst = FOLLOW(ptr+1); DEREF(lst);
            pair_ptr = (BPLONG_PTR)UNTAGGED_ADDR(pair);
            attr_name = FOLLOW(pair_ptr+1);
            DEREF_NONVAR(attr_name);  /* (name,value) */
            if (attr_name == attr_neq_atm) {
                value = FOLLOW(pair_ptr+2); DEREF_NONVAR(value);  /* combined_propagators(NeqVs,NeqVCs) */
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(value);
                count += count_cs_list(FOLLOW(ptr+1))+count_cs_list(FOLLOW(ptr+2));
            } else if (attr_name == attr_cfd_atm) {
                value = FOLLOW(pair_ptr+2); DEREF_NONVAR(value);  /* cs(L) */
                ptr = (BPLONG_PTR)UNTAGGED_ADDR(value);
                count += count_cs_list(FOLLOW(ptr+1));
            }
        }
    }

    ASSIGN_f_atom(N, MAKEINT(count));
    return BP_TRUE;
}

int count_cs_list(BPLONG list)
{
    int i = 0;
    BPLONG_PTR ptr;

    while (ISLIST(list)) {
        i ++;
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(list);
        list = FOLLOW(ptr+1);
    }
    return i;
}

int c_fd_degree() {
    BPLONG Var, N, cs, count;
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    Var = ARG(1, 2);
    N = ARG(2, 2);

    DEREF(Var);
    if (!IS_SUSP_VAR(Var)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
    cs = DV_ins_cs(dv_ptr);
    if (ISLIST(cs)) {
        DV_ins_cs(dv_ptr) = ADDTAG(UNTAGGED_ADDR(cs), STR);  /* mark it so it won't be counted twice */
        count = dvar_degree_count_connected_vars_cs(cs);
        DV_ins_cs(dv_ptr) = cs;
        dvar_degree_reset_cs_tags_cs(cs);
    } else {
        count = 0;
    }
    unify(N, MAKEINT(count));
    return BP_TRUE;
}

int dvar_degree_count_connected_vars_cs(BPLONG cs)
{
    BPLONG_PTR ptr, sf;
    BPLONG constr, count = 0;

    while (ISLIST(cs)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(cs);  /* untag LST */
        constr = FOLLOW(ptr);  /* car */
        sf = (BPLONG_PTR)((BPULONG)stack_up_addr-(BPULONG)UNTAGGED_CONT(constr));
        count += dvar_degree_count_connected_vars_frame(sf);
        cs = FOLLOW(ptr+1);  /* cdr */
    }
    return count;
}

int dvar_degree_count_connected_vars_frame(BPLONG_PTR f)
{
    BPLONG_PTR sp;
    BPLONG count = 0;

    sp = (BPLONG_PTR)UNTAGGED_ADDR(AR_BTM(f));
    while (sp > f) {
        count += dvar_degree_count_connected_vars_term(FOLLOW(sp));
        sp--;
    }
    return count;
}

int dvar_degree_count_connected_vars_term(BPLONG term)
{
    BPLONG_PTR dv_ptr, ptr, top;
    BPLONG cs, count;

    count = 0;
start:
    DEREF(term);
    if (IS_SUSP_VAR(term)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(term);
        cs = DV_ins_cs(dv_ptr);
        if (ISLIST(cs)) {
            DV_ins_cs(dv_ptr) = ADDTAG(UNTAGGED_ADDR(cs), STR);  /* mark it so it won't be counted twice */
            count = 1;
        }
    } else if (ISLIST(term)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(term);
        count = dvar_degree_count_connected_vars_term(FOLLOW(ptr));
        term = FOLLOW(ptr+1);
        goto start;
    } else if (ISSTRUCT(term)) {
        BPLONG i, arity;
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(term);
        arity = GET_ARITY((SYM_REC_PTR)FOLLOW(ptr));
        count = 0;
        for (i = 1; i < arity; i++) {
            count += dvar_degree_count_connected_vars_term(FOLLOW(ptr+i));
        }
    }
    return count;
}

void dvar_degree_reset_cs_tags_cs(BPLONG cs)
{
    BPLONG_PTR ptr, sf;
    BPLONG constr;

    while (ISLIST(cs)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(cs);  /* untag LST */
        constr = FOLLOW(ptr);  /* car */
        sf = (BPLONG_PTR)((BPULONG)stack_up_addr-(BPULONG)UNTAGGED_CONT(constr));
        dvar_degree_reset_cs_tags_frame(sf);
        cs = FOLLOW(ptr+1);  /* cdr */
    }
}

void dvar_degree_reset_cs_tags_frame(BPLONG_PTR f)
{
    BPLONG_PTR sp;

    sp = (BPLONG_PTR)UNTAGGED_ADDR(AR_BTM(f));
    while (sp > f) {
        dvar_degree_reset_cs_tags_term(FOLLOW(sp));
        sp--;
    }
}

void dvar_degree_reset_cs_tags_term(BPLONG term)
{
    BPLONG_PTR dv_ptr, ptr, top;
    BPLONG cs;

start:
    DEREF(term);
    if (IS_SUSP_VAR(term)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(term);
        cs = DV_ins_cs(dv_ptr);
        if (ISNIL(cs) || ISLIST(cs)) {
            return;
        } else {  /* restore the tag */
            cs = ADDTAG(UNTAGGED_ADDR(cs), LST);
            DV_ins_cs(dv_ptr) = cs;
            //      dvar_degree_reset_cs_tags_cs(cs);
        }
    } else if (ISLIST(term)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(term);
        dvar_degree_reset_cs_tags_term(FOLLOW(ptr));
        term = FOLLOW(ptr+1);
        goto start;
    } else if (ISSTRUCT(term)) {
        BPLONG i, arity;
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(term);
        arity = GET_ARITY((SYM_REC_PTR)FOLLOW(ptr));
        for (i = 1; i < arity; i++) {
            dvar_degree_reset_cs_tags_term(FOLLOW(ptr+i));
        }
    }
}

void display_constraint(BPLONG_PTR frame)
{
    SYM_REC_PTR sym_ptr;
    BPLONG arity, i;

    sym_ptr = (SYM_REC_PTR)FOLLOW(((BPLONG_PTR)AR_REEP(frame)+2));
    arity = GET_ARITY(sym_ptr);
    bp_write_pname(GET_NAME(sym_ptr));
    fprintf(curr_out, "(");
    for (i = arity; i > 0; i--) {
        write_term1(*(frame+i), curr_out);
        if (i != 1) fprintf(curr_out, ",");
    }
    fprintf(curr_out, ")\n");
}

int display_constraints() {

    BPLONG_PTR frame;

    frame = sfreg;
    while (AR_PREV(frame) != (BPLONG)frame) {
        display_constraint(frame);
        frame = (BPLONG_PTR)AR_PREV(frame);
    }
    return 1;
}

int c_VV_EQ_C_CON() {
    BPLONG X, Y, C;

    X = ARG(1, 3);
    Y = ARG(2, 3);
    C = ARG(3, 3);
    return b_VV_EQ_C_CON_ccc(X, Y, C);
}

int b_VV_EQ_C_CON_ccc(BPLONG X, BPLONG Y, BPLONG C)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y;

    DEREF_NONVAR(X); if (!IS_SUSP_VAR(X)) return BP_TRUE;
    DEREF_NONVAR(Y); if (!IS_SUSP_VAR(Y)) return BP_TRUE;
    C = INTVAL(C);  /* C is dereferenced already */

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);

    if (IS_BV_DOMAIN(dv_ptr_y))
        c_VV_EQ_C_CON_aux(dv_ptr_x, dv_ptr_y, C);

    if (!IS_SUSP_VAR(FOLLOW(dv_ptr_x))) return BP_TRUE;

    if (IS_BV_DOMAIN(dv_ptr_x))
        c_VV_EQ_C_CON_aux(dv_ptr_y, dv_ptr_x, C);

    return BP_TRUE;
}

int c_VV_EQ_C_CON_aux(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y, BPLONG C)
{
    BPLONG currX, currY, maxX, minY, maxY;


    /* for each x in X, there is an y in Y such that x+y=C */
    currX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);
    /* write_term(dv_ptr_x);printf("+"); write_term(dv_ptr_y); printf("_eq_%d",C);printf("\n"); */
    for (; ; ) {
        currY = C-currX;
        if (!dm_true(dv_ptr_y, currY)) {
            domain_set_false_aux(dv_ptr_x, currX);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
        if (currX >= maxX) return BP_TRUE;
        if (IS_IT_DOMAIN(dv_ptr_x)) {
            currX++;
        } else {
            currX = domain_next_bv(dv_ptr_x, currX+1);
        }
    }
    return BP_TRUE;
}

/* X=Y+C */
int c_V_EQ_VC_CON() {
    BPLONG X, Y, C;

    X = ARG(1, 3);
    Y = ARG(2, 3);
    C = ARG(3, 3);
    return b_V_EQ_VC_CON_ccc(X, Y, C);
}

int b_V_EQ_VC_CON_ccc(BPLONG X, BPLONG Y, BPLONG C)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y;

    DEREF_NONVAR(X); if (!IS_SUSP_VAR(X)) return BP_TRUE;
    DEREF_NONVAR(Y); if (!IS_SUSP_VAR(Y)) return BP_TRUE;
    C = INTVAL(C);  /* no dereference is necessary */

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);

    if (IS_BV_DOMAIN(dv_ptr_y))
        c_V_EQ_VC_CON_aux(dv_ptr_x, dv_ptr_y, C);

    if (!IS_SUSP_VAR(FOLLOW(dv_ptr_x))) return BP_TRUE;

    if (IS_BV_DOMAIN(dv_ptr_x))
        c_V_EQ_VC_CON_aux(dv_ptr_y, dv_ptr_x, -C);
    return BP_TRUE;
}

int c_V_EQ_VC_CON_aux(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y, BPLONG C)
{
    BPLONG currX, currY, maxX, minY, maxY;


    /* for each x in X, there is an y in Y such that x=y+C */
    currX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);
    /* write_term(dv_ptr_x);printf("_eq_"); write_term(dv_ptr_y); printf("+%d",C);printf("\n"); */
    for (; ; ) {
        currY = currX-C;
        if (!dm_true(dv_ptr_y, currY)) {
            /* printf("exclude(%d,",currX);write_term(dv_ptr_x);printf(")\n");  */
            domain_set_false_aux(dv_ptr_x, currX);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
        if (currX >= maxX) return BP_TRUE;
        if (IS_IT_DOMAIN(dv_ptr_x)) {
            currX++;
        } else {
            currX = domain_next_bv(dv_ptr_x, currX+1);
        }
    }
    return BP_TRUE;
}

/* for each x in X, there is an y in Y such that A*x+B*y=C */
int c_UU_EQ_C_CON() {
    BPLONG A, X, B, Y, C;
    BPLONG_PTR dv_ptr_x, dv_ptr_y;

    A = ARG(1, 5); A = INTVAL(A);
    X = ARG(2, 5); DEREF_NONVAR(X); if (!IS_SUSP_VAR(X)) return BP_TRUE;
    B = ARG(3, 5); B = INTVAL(B);
    Y = ARG(4, 5); DEREF_NONVAR(Y); if (!IS_SUSP_VAR(Y)) return BP_TRUE;
    C = ARG(5, 5); C = INTVAL(C);

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);

    if (IS_BV_DOMAIN(dv_ptr_y) || B != 1)
        c_UU_EQ_C_CON_aux(A, dv_ptr_x, B, dv_ptr_y, C);

    if (!IS_SUSP_VAR(FOLLOW(dv_ptr_x))) return BP_TRUE;

    if (IS_BV_DOMAIN(dv_ptr_x) || A != 1)
        c_UU_EQ_C_CON_aux(B, dv_ptr_y, A, dv_ptr_x, C);

    return BP_TRUE;
}

/* for each x in X, there is an y in Y such that A*x+B*y=C */
int c_UU_EQ_C_CON_aux(BPLONG A, BPLONG_PTR dv_ptr_x, BPLONG B, BPLONG_PTR dv_ptr_y, BPLONG C)
{
    BPLONG tmp, currX, currY, maxX, minY, maxY;

    currX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);
    for (; ; ) {
        tmp = C-A*currX;
        currY = tmp/B;
        if (B*currY != tmp || !dm_true(dv_ptr_y, currY)) {
            domain_set_false_aux(dv_ptr_x, currX);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
        if (currX >= maxX) {return BP_TRUE;}
        if (IS_IT_DOMAIN(dv_ptr_x)) {
            currX++;
        } else {
            currX = domain_next_bv(dv_ptr_x, currX+1);
        }
    }
    return BP_TRUE;
}

void print_event_queue() {
    int i;
    printf("trigger_no=" BPLONG_FMT_STR "\n", trigger_no);
    for (i = 1; i <= trigger_no; i++) {
        printf("FLAG(%d) queue(" BPULONG_FMT_STR ")\n", event_flag[i], (BPULONG)triggeredCs[i]);
    }
    if (trigger_no >= 1) printf("\n");
}

/* for each x in X, there is a y in Y such that A*x=B*y+C */
int c_U_EQ_UC_CON() {
    BPLONG A, X, B, Y, C;
    BPLONG_PTR dv_ptr_x, dv_ptr_y;

    A = ARG(1, 5); A = INTVAL(A);
    X = ARG(2, 5); DEREF_NONVAR(X); if (!IS_SUSP_VAR(X)) return BP_TRUE;
    B = ARG(3, 5); B = INTVAL(B);
    Y = ARG(4, 5); DEREF_NONVAR(Y); if (!IS_SUSP_VAR(Y)) return BP_TRUE;
    C = ARG(5, 5); C = INTVAL(C);

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);

    if (IS_BV_DOMAIN(dv_ptr_y) || B != 1)
        c_U_EQ_UC_CON_aux(A, dv_ptr_x, B, dv_ptr_y, C);

    if (!IS_SUSP_VAR(FOLLOW(dv_ptr_x))) return BP_TRUE;

    if (IS_BV_DOMAIN(dv_ptr_x) || A != 1)
        c_U_EQ_UC_CON_aux(B, dv_ptr_y, A, dv_ptr_x, -C);
    return BP_TRUE;
}

/* for each x in X, there is a y in Y such that A*x=B*y+C */
int c_U_EQ_UC_CON_aux(BPLONG A, BPLONG_PTR dv_ptr_x, BPLONG B, BPLONG_PTR dv_ptr_y, BPLONG C)
{
    BPLONG tmp, currX, currY, maxX, minY, maxY;

    currX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);

    for (; ; ) {
        tmp = A*currX-C;
        currY = tmp/B;
        if (tmp != B*currY || !dm_true(dv_ptr_y, currY)) {
            domain_set_false_aux(dv_ptr_x, currX);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
        if (currX >= maxX) return BP_TRUE;
        if (IS_IT_DOMAIN(dv_ptr_x)) {
            currX++;
        } else {
            currX = domain_next_bv(dv_ptr_x, currX+1);
        }
    }
    return BP_TRUE;
}

/*
  $set(SP,SA,Card,Ref,Univ,UnivSize,Tag,Notation)
*/

#define CLPSETTERM_PTR_GET_SP(ptr) FOLLOW(ptr+1)
#define CLPSETTERM_PTR_GET_SA(ptr) FOLLOW(ptr+2)
#define CLPSETTERM_PTR_GET_CARD(ptr) FOLLOW(ptr+3)
#define CLPSETTERM_PTR_GET_REF(ptr) FOLLOW(ptr+4)
#define CLPSETTERM_PTR_GET_USIZE(ptr) FOLLOW(ptr+6)
#define CLPSETTERM_PTR_GET_TAG(ptr) FOLLOW(ptr+7)
/** CLP(Set) **/
/*
  $clpset_check_when_card_bound_dvar(Card,S,SetTerm)=>true.
  fd_size(SP,SPSize),
  UpSize is SPSize-2, % not count dummies
  fd_size(SA,NotAddedSize),
  AddedSize is UnivSize-NotAddedSize+2,
  (Card=:=UpSize ->
  $clpset_indomain_pickup_all_possible(Ref,SP,SA,Notation,Tag)
  ;   
  Card=:=AddedSize -> 
  $clpsetterm_indomain_pickup_only_in(Ref,SP,SA,Card,Notation,Tag)
  ;
  true).
*/
int b_CLPSET_CARD_BOUND_c(BPLONG SetTerm)
{
    BPLONG_PTR top, ptr, sa_dv_ptr, sp_dv_ptr;
    BPLONG SP, SA, Card, Ref, USize, Tag;
    BPLONG SPSize, SASize;
    BPLONG card_low, card_up;

    DEREF(SetTerm);
    ptr = (BPLONG_PTR)UNTAGGED_ADDR(SetTerm);

    SP = CLPSETTERM_PTR_GET_SP(ptr); DEREF(SP);
    sp_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SP);
    SPSize = DV_size(sp_dv_ptr);

    SA = CLPSETTERM_PTR_GET_SA(ptr); DEREF(SA);
    sa_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SA);
    SASize = DV_size(sa_dv_ptr);

    USize = CLPSETTERM_PTR_GET_USIZE(ptr); DEREF(USize); USize = INTVAL(USize);

    Card = CLPSETTERM_PTR_GET_CARD(ptr); DEREF(Card); Card = INTVAL(Card);

    Ref = CLPSETTERM_PTR_GET_REF(ptr);

    Tag = CLPSETTERM_PTR_GET_TAG(ptr);

    card_low = USize-SASize+2; card_up = SPSize-2;
    if (Card > card_up || Card < card_low) return 0;
    if (Card == card_up ) {
        unify(Tag, BP_ONE);
        unify(Ref, clpset_pickup_all_possible(sp_dv_ptr, sa_dv_ptr));
    } else if (Card == card_low) {
        unify(Tag, BP_ONE);
        unify(Ref, clpset_pickup_only_in(sp_dv_ptr, sa_dv_ptr, Card));
    }
    return 1;
}

/*
  $clpset_check_when_low_updated_dvar(SA,S,SetTerm) => true.
  b_DM_COUNT_cf(SA,NotAddedSize),
  AddedSize is UnivSize-NotAddedSize+2,
  domain_region_min(Card,AddedSize),    % Card #>= AddedSize,
  ((integer(Card),Card==AddedSize)->
  $clpsetterm_indomain_pickup_only_in(Ref,SP,SA,Card,Notation,Tag)
  ;
  true
  ).
*/
int b_CLPSET_LOW_UPDATED_c(BPLONG SetTerm)
{
    BPLONG_PTR top, ptr, dv_ptr, sp_dv_ptr, sa_dv_ptr;
    BPLONG SP, SA, Card, Ref, USize, SASize;
    BPLONG new_card_low;

    DEREF(SetTerm);
    ptr = (BPLONG_PTR)UNTAGGED_ADDR(SetTerm);

    SA = CLPSETTERM_PTR_GET_SA(ptr); DEREF(SA);
    sa_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SA);
    SASize = DV_size(sa_dv_ptr);

    USize = CLPSETTERM_PTR_GET_USIZE(ptr); DEREF(USize); USize = INTVAL(USize);

    new_card_low = USize-SASize+2;

    Card = CLPSETTERM_PTR_GET_CARD(ptr); DEREF(Card);

    if (!ISINT(Card)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Card);
        if (domain_region_noint(dv_ptr, new_card_low, BP_MAXINT_1W) == 0) return 0;
        Card = DV_var(dv_ptr);
    }
    if (ISINT(Card) && INTVAL(Card) == new_card_low) {
        SP = CLPSETTERM_PTR_GET_SP(ptr); DEREF(SP);
        sp_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SP);
        Ref = CLPSETTERM_PTR_GET_REF(ptr);
        unify(Ref, clpset_pickup_only_in(sp_dv_ptr, sa_dv_ptr, INTVAL(Card)));
    }
    return 1;
}

/*
  $clpset_check_when_up_updated_dvar(SP,S,SetTerm) => 
  b_DM_COUNT_cf(SP,UpSize),
  UpBoundSize is UpSize-2, % not count dummies
  domain_region_max(Card,UpBoundSize),
  ((integer(Card),Card==UpBoundSize) -> 
  $clpset_indomain_pickup_all_possible(Ref,SP,SA,Notation,Tag)
  ;  
  true).
*/
int b_CLPSET_UP_UPDATED_c(BPLONG SetTerm)
{
    BPLONG_PTR top, ptr, dv_ptr, sa_dv_ptr, sp_dv_ptr;
    BPLONG SP, SA, Card, Ref, SPSize;
    BPLONG new_card_up;

    DEREF(SetTerm);
    ptr = (BPLONG_PTR)UNTAGGED_ADDR(SetTerm);

    SP = CLPSETTERM_PTR_GET_SP(ptr); DEREF(SP);
    sp_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SP);
    SPSize = DV_size(sp_dv_ptr);

    new_card_up = SPSize-2;

    Card = CLPSETTERM_PTR_GET_CARD(ptr); DEREF(Card);
    if (!ISINT(Card)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Card);
        if (domain_region_noint(dv_ptr, BP_MININT_1W, new_card_up) == 0) return 0;
        Card = DV_var(dv_ptr);
    }

    if (ISINT(Card) && INTVAL(Card) == new_card_up) {
        SA = CLPSETTERM_PTR_GET_SA(ptr); DEREF(SA);
        sa_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SA);
        Ref = CLPSETTERM_PTR_GET_REF(ptr);
        unify(Ref, clpset_pickup_all_possible(sp_dv_ptr, sa_dv_ptr));
    }
    return 1;
}

/* SP and SA are derefered already */
/*
  $clpset_indomain_pickup_all_possible(S,SP,SA,{},Tag):-var(S),dvar(SP) :  %set notation
  domain_min_max(SP,First,Last),
  domain_next_inst(SP,First,RealFirst),
  (RealFirst==Last->Tag=1,S={};
  b_DM_PREV_ccf(SP,Last,RealLast),
  $clpset_dvar_indomain_pickup_all_possible(SP,SA,RealFirst,RealLast,Set),
  Tag=1,
  S={Set}).

  %%
  $clpset_dvar_indomain_pickup_all_possible(SP,SA,Cur,Last,Set):-Cur==Last :
  domain_set_false(SA,Cur),
  Set=Cur.
  $clpset_dvar_indomain_pickup_all_possible(SP,SA,Cur,Last,Set):-true :
  domain_next_inst(SP,Cur,Next),
  domain_set_false(SA,Cur),
  Set=(Cur,Set1),
  $clpset_dvar_indomain_pickup_all_possible(SP,SA,Next,Last,Set1).
*/
BPLONG clpset_pickup_all_possible(BPLONG_PTR sp_dv_ptr, BPLONG_PTR sa_dv_ptr)
{
    BPLONG set0, return_val;
    BPLONG_PTR set_tail_ptr;
    BPLONG last, cur;

    cur = DV_first(sp_dv_ptr);
    last = DV_last(sp_dv_ptr);
    CALL_DOMAIN_NEXT(sp_dv_ptr, cur, cur);  /* real first */
    if (cur == last) {
        return empty_set;
    }
    CALL_DOMAIN_PREV(sp_dv_ptr, last, last);  /* real last */
    set_tail_ptr = &set0;
    /**/
    while (cur != last) {
        FOLLOW(set_tail_ptr) = ADDTAG(heap_top, STR);
        FOLLOW(heap_top++) = (BPLONG)comma_psc;
        FOLLOW(heap_top++) = MAKEINT(cur);
        set_tail_ptr = heap_top++;
        domain_set_false_noint(sa_dv_ptr, cur);
        CALL_DOMAIN_NEXT(sp_dv_ptr, cur, cur);
    }
    /* last elm */
    FOLLOW(set_tail_ptr) = MAKEINT(cur);
    domain_set_false_noint(sa_dv_ptr, cur);
    /**/
    return_val = ADDTAG(heap_top, STR);
    FOLLOW(heap_top++) = (BPLONG)set_constructor_psc;
    FOLLOW(heap_top++) = set0;
    /*  write_term(return_val);printf("\n"); */
    return return_val;
}

/*
  $clpsetterm_indomain_pickup_only_in(S,SP,SA,Card,{},Tag):-var(S),dvar(SP) :
  domain_min_max(SP,First,Last),
  domain_next_inst(SP,First,RealFirst),
  $clpset_dvar_indomain_pickup_only_in(SP,SA,RealFirst,Last,Set,Card),
  Tag=1,
  (Card=:=0->S={};S={Set}).

  %%
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Cur,Last,Set,Card):-Cur==Last : true.
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Cur,Last,Set,Card):-
  b_DM_TRUE_cc(SA,Cur) :
  domain_next_inst(SP,Cur,Next),
  domain_set_false(SP,Cur),
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Next,Last,Set,Card).
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Cur,Last,Set,Card):-Card==1 :
  Set=Cur,
  domain_next_inst(SP,Cur,Next),
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Next,Last,Set1,0).
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Cur,Last,Set,Card):-true :
  Set=(Cur,Set1),
  Card1 is Card-1,
  domain_next_inst(SP,Cur,Next),
  $clpset_dvar_indomain_pickup_only_in(SP,SA,Next,Last,Set1,Card1).
*/
BPLONG clpset_pickup_only_in(BPLONG_PTR sp_dv_ptr, BPLONG_PTR sa_dv_ptr, BPLONG card)
{
    BPLONG last, cur;

    BPLONG set0, return_val;
    BPLONG_PTR set_tail_ptr;

    cur = DV_first(sp_dv_ptr);
    last = DV_last(sp_dv_ptr);
    CALL_DOMAIN_NEXT(sp_dv_ptr, cur, cur);  /* real first */
    if (card == 0) {clpset_exclude_all_possible(sp_dv_ptr, cur, last); return empty_set;}
    set_tail_ptr = &set0;
    /**/
    for (; ; ) {
        /* if (cur==last){printf("STRANGE %d %d %d\n",cur,last,card);exit(1);} */
        if (!dm_true(sa_dv_ptr, cur)) {  /* in low bound */
            if (card == 1) {
                FOLLOW(set_tail_ptr) = MAKEINT(cur);
                CALL_DOMAIN_NEXT(sp_dv_ptr, cur, cur);
                clpset_exclude_all_possible(sp_dv_ptr, cur, last);
                return_val = ADDTAG(heap_top, STR);
                FOLLOW(heap_top++) = (BPLONG)set_constructor_psc;
                FOLLOW(heap_top++) = set0;
                /*      write_term(return_val);printf("\n"); */
                return return_val;
            }
            FOLLOW(set_tail_ptr) = ADDTAG(heap_top, STR);
            FOLLOW(heap_top++) = (BPLONG)comma_psc;
            FOLLOW(heap_top++) = MAKEINT(cur);
            set_tail_ptr = heap_top++;
            card--;
        } else {
            domain_set_false_noint(sp_dv_ptr, cur);
        }
        CALL_DOMAIN_NEXT(sp_dv_ptr, cur, cur);
    }
}

void clpset_exclude_all_possible(BPLONG_PTR sp_dv_ptr, BPLONG cur, BPLONG last)
{
    while (cur != last) {
        domain_set_false_noint(sp_dv_ptr, cur);
        CALL_DOMAIN_NEXT(sp_dv_ptr, cur, cur);
    }
}

/*
  $determinate_pred(reify_eq_constr_consistency,3):-true : true.
  reify_eq_constr_consistency(B,X,Y):-
  dvar(X),dvar(Y) :
  domain_min_max(X,MinX,MaxX),
  domain_min_max(Y,MinY,MaxY),
  (MinX>MaxY->B=0;
  MinY>MaxX->B=0;
  X==Y -> B=1;
  true).
  reify_eq_constr_consistency(B,X,Y):-
  dvar(X),integer(Y) :
  (b_DM_TRUE_cc(X,Y)->true;B=0).
  reify_eq_constr_consistency(B,X,Y):-
  integer(X),dvar(Y) :
  (b_DM_TRUE_cc(Y,X)->true;B=0).
  reify_eq_constr_consistency(B,X,Y):-true : true.
*/
int b_REIFY_EQ_CONSTR_ACTION(BPLONG B, BPLONG X, BPLONG Y)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y, dv_ptr_b;

    DEREF_NONVAR(X); DEREF_NONVAR(Y);
    DEREF_NONVAR(B);
    if (X == Y) {
        UNIFY_DVAR_VAL(B, BP_ONE);
    }

    if (ISINT(X)) {
        if (ISINT(Y)) {
            UNIFY_DVAR_VAL(B, BP_ZERO);  /* X \= Y */
        }
        dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
        if (!dm_true(dv_ptr_y, INTVAL(X))) {
            UNIFY_DVAR_VAL(B, BP_ZERO);
        }
    } else {
        dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        if (ISINT(Y)) {
            if (!dm_true(dv_ptr_x, INTVAL(Y))) {
                UNIFY_DVAR_VAL(B, BP_ZERO);
            }
        } else {  /* X and Y are domain vars */
            dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
            if (dm_disjoint(dv_ptr_x, dv_ptr_y)) {
                UNIFY_DVAR_VAL(B, BP_ZERO);
            }
        }
    }
    return BP_TRUE;
}

/*
  $determinate_pred(reify_ge_constr_consistency,3):-true : true.
  %reify_ge_constr_consistency(B,X,Y):-write(reify_ge_constr_consistency(B,X,Y)),nl,fail.
  reify_ge_constr_consistency(B,X,Y):-
  domain_min_max(X,MinX,MaxX),
  domain_min_max(Y,MinY,MaxY),
  (MinX>=MaxY->B=1;
  MinY>MaxX->B=0;
  true).
  %    write('<=reify_ge_constr_consistency'(B,X,Y)),nl.
*/
int b_REIFY_GE_CONSTR_ACTION(BPLONG B, BPLONG X, BPLONG Y)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y, dv_ptr_b;
    BPLONG min_x, max_x, min_y, max_y;

    DEREF_NONVAR(X); DEREF_NONVAR(Y);
    if (ISINT(X)) {
        min_x = max_x = INTVAL(X);
    } else {
        dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        min_x = DV_first(dv_ptr_x);
        max_x = DV_last(dv_ptr_x);
    }
    if (ISINT(Y)) {
        min_y = max_y = INTVAL(Y);
    } else {
        dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
        min_y = DV_first(dv_ptr_y);
        max_y = DV_last(dv_ptr_y);
    }
    if (min_x >= max_y) {
        DEREF_NONVAR(B);
        UNIFY_DVAR_VAL(B, BP_ONE);
    }
    if (max_x < min_y) {
        DEREF_NONVAR(B);
        UNIFY_DVAR_VAL(B, BP_ZERO);
    }
    return BP_TRUE;
}

/*
  $determinate_pred(reify_neq_constr_consistency,3):-true : true.
  reify_neq_constr_consistency(B,X,Y):-
  dvar(X),dvar(Y) :
  domain_min_max(X,MinX,MaxX),
  domain_min_max(Y,MinY,MaxY),
  (MinX>MaxY->B=1;
  MinY>MaxX->B=1;
  X==Y -> B=0;
  true).
  reify_neq_constr_consistency(B,X,Y):-
  dvar(X),integer(Y) :
  (b_DM_TRUE_cc(X,Y)->true;B=1).
  reify_neq_constr_consistency(B,X,Y):-
  integer(X),dvar(Y) :
  (b_DM_TRUE_cc(Y,X)->true;B=1).
  reify_neq_constr_consistency(B,X,Y):-true : true.
*/
int b_REIFY_NEQ_CONSTR_ACTION(BPLONG B, BPLONG X, BPLONG Y)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y, dv_ptr_b;
    /*
      B = FOLLOW(arreg+3);DEREF_NONVAR(B); 
      X = FOLLOW(arreg+2);DEREF_NONVAR(X); 
      Y = FOLLOW(arreg+1);DEREF_NONVAR(Y); 
    */
    DEREF_NONVAR(B);
    DEREF_NONVAR(X);
    DEREF_NONVAR(Y);

    if (X == Y) {
        UNIFY_DVAR_VAL(B, BP_ZERO);
    }
    if (ISINT(X)) {
        if (ISINT(Y)) {
            UNIFY_DVAR_VAL(B, BP_ONE);  /* X \= Y */
        }
        dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
        if (!dm_true(dv_ptr_y, INTVAL(X))) {
            UNIFY_DVAR_VAL(B, BP_ONE);
        }
    } else {
        dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        if (ISINT(Y)) {
            if (!dm_true(dv_ptr_x, INTVAL(Y))) {
                UNIFY_DVAR_VAL(B, BP_ONE);
            }
        } else {  /* X and Y are domain vars */
            dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
            if (dm_disjoint(dv_ptr_x, dv_ptr_y)) {
                UNIFY_DVAR_VAL(B, BP_ONE);
            }
        }
    }
    return BP_TRUE;
}

/*
  for each x in X, |x| is in Y;
  for each y in Y, either y or -y is in X
*/
int b_ABS_CON_cc(BPLONG X, BPLONG Y)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y;
    BPLONG elm, melm, minX, maxX, minY, maxY;

    DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;
    DEREF_NONVAR(Y);
    if (!IS_SUSP_VAR(Y)) return BP_TRUE;

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
    if (IS_IT_DOMAIN(dv_ptr_x) && IS_IT_DOMAIN(dv_ptr_y)) return BP_TRUE;

    minX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);

    for (elm = minY; elm <= maxY; elm++) {
        melm = -elm;
        if (!dm_true(dv_ptr_y, elm)) {
            domain_set_false_aux(dv_ptr_x, elm);
            domain_set_false_aux(dv_ptr_x, melm);
        } else if (!dm_true(dv_ptr_x, elm) &&
                   !dm_true(dv_ptr_x, melm)) {
            domain_set_false_aux(dv_ptr_y, elm);
        }
    }
    return BP_TRUE;
}

/*
  fd_abs_x_to_y(X,Y):-
  domain_min_max(X,MinX,MaxX),
  (MinX >= 0 -> domain_region(Y,MinX,MaxX);
  MaxX =< 0 -> Lower is -MaxX, Upper is -MinX, domain_region(Y,Lower,Upper);
  AbsMinX is -MinX,
  (AbsMinX>MaxX->Up is AbsMinX;Up is MaxX),
  domain_region_max(Y,Up)).
*/
/* when X updated */
int b_FD_ABS_X_TO_Y(BPLONG X, BPLONG Y)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y;
    BPLONG minX, maxX, up;

    DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) {
        minX = maxX = INTVAL(X);
    } else {
        dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        minX = DV_first(dv_ptr_x);
        maxX = DV_last(dv_ptr_x);
    }
    DEREF_NONVAR(Y);
    if (ISINT(Y)) {
        Y = INTVAL(Y);
        if (minX > -Y) {
            return unify(X, MAKEINT(Y));
        } else if (maxX < Y) {
            return unify(X, MAKEINT(-Y));
        }
        return BP_TRUE;
    }
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
    if (minX >= 0) {
        return domain_region_noint(dv_ptr_y, minX, maxX);
    } else if (maxX <= 0) {
        return domain_region_noint(dv_ptr_y, -maxX, -minX);
    } else {
        up = -minX;
        if (maxX > up) up = maxX;
        return domain_region_noint(dv_ptr_y, 0, up);
    }
}

/* X mod Y = Z (precondition integer(Y),Y>0,min(X)>=0)
   for each x in X if (x mod Y) is not in Z, then exclude x from X
*/
int b_MOD_CON_ccc(BPLONG X, BPLONG Y, BPLONG Z)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_z;
    BPLONG currX, maxX;

    DEREF_NONVAR(Y); Y = INTVAL(Y);
    DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;
    DEREF_NONVAR(Z);
    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    currX = DV_first(dv_ptr_x);
    maxX = DV_last(dv_ptr_x);
    if (maxX >= 3000)
        return BP_TRUE;
    if (ISINT(Z)) {
        Z = INTVAL(Z);
        while (currX <= maxX) {
            if (currX%Y != Z) {
                if (domain_set_false_aux(dv_ptr_x, currX) == BP_FALSE) return BP_FALSE;
            }
            if (IS_IT_DOMAIN(dv_ptr_x)) {
                currX++;
            } else {
                currX = domain_next_bv(dv_ptr_x, currX+1);
            }
        }
    } else {
        dv_ptr_z = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Z);
        while (currX <= maxX) {
            if (!dm_true(dv_ptr_z, currX % Y)) {
                if (domain_set_false_aux(dv_ptr_x, currX) == BP_FALSE) return BP_FALSE;
            }
            if (IS_IT_DOMAIN(dv_ptr_x)) {
                currX++;
            } else {
                currX = domain_next_bv(dv_ptr_x, currX+1);
            }
        }
    }
    return BP_TRUE;
}


/* X // Y = Z (precondition integer(Y), Y>0 min(X)>=0)
   Z in min(X)//Y..max(X)//Y
   X in min(Z)*Y..(max(Z)+1)*Y-1
*/
int b_IDIV_CON_ccc(BPLONG X, BPLONG Y, BPLONG Z)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_z;
    BPLONG low, up;

    /*  printf("=>check_idiv"); write_term(X); printf(" ");write_term(Y); printf(" ");write_term(Z); printf("\n"); */

    DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;
    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    DEREF_NONVAR(Y); Y = INTVAL(Y);
    low = DV_first(dv_ptr_x);
    up = DV_last(dv_ptr_x);
    if (low > BP_MININT_1W) low = low/Y;
    if (up < BP_MAXINT_1W) up = up/Y;

    DEREF_NONVAR(Z);
    if (ISINT(Z)) {
        Z = INTVAL(Z);
        if (Z < low || Z > up) return BP_FALSE;
        if (domain_region_noint(dv_ptr_x, Y*Z, Y*(Z+1)-1) == BP_FALSE) return BP_FALSE;
    } else {
        dv_ptr_z = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Z);
        if (domain_region_noint(dv_ptr_z, low, up) == BP_FALSE) return BP_FALSE;
        low = DV_first(dv_ptr_z);
        up = DV_last(dv_ptr_z);
        if (low > BP_MININT_1W && up < BP_MAXINT_1W) {
            if (domain_region_noint(dv_ptr_x, Y*low, Y*(up+1)-1) == BP_FALSE) return BP_FALSE;
        }
    }
    return BP_TRUE;
}

/*
  for each x in X, either x-N or x+N must be in Y.
  for each y in Y, either y-N or y+N must be in X.
*/
int b_ABS_DIFF_CON_ccc(BPLONG X, BPLONG Y, BPLONG N)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y;
    BPLONG elm, min, max;

    DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;
    DEREF_NONVAR(Y);
    if (!IS_SUSP_VAR(Y)) return BP_TRUE;
    DEREF_NONVAR(N);
    N = INTVAL(N);

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
    min = DV_first(dv_ptr_x); max = DV_last(dv_ptr_x);
    for (elm = min; elm <= max; elm++) {
        if (dm_true(dv_ptr_y, elm-N) || dm_true(dv_ptr_y, elm+N));
        else if (domain_set_false_aux(dv_ptr_x, elm) == BP_FALSE) return BP_FALSE;
    }

    if (!IS_SUSP_VAR(FOLLOW(dv_ptr_x))) return BP_TRUE;
    min = DV_first(dv_ptr_y); max = DV_last(dv_ptr_y);
    for (elm = min; elm <= max; elm++) {
        if (dm_true(dv_ptr_x, elm-N) || dm_true(dv_ptr_x, elm+N));
        else if (domain_set_false_aux(dv_ptr_y, elm) == BP_FALSE) return BP_FALSE;
    }

    return BP_TRUE;
}

/* abs(X-Y) = N
   triggered after Ex is excluded from X
*/
int b_ABS_DIFF_X_TO_Y(BPLONG Ex)
{
    BPLONG X, Y, N, Ey;
    BPLONG_PTR dv_ptr_x, dv_ptr_y;

    X = FOLLOW(arreg+3); DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;

    Y = FOLLOW(arreg+2); DEREF_NONVAR(Y);
    if (!IS_SUSP_VAR(Y)) return BP_TRUE;

    N = FOLLOW(arreg+1); DEREF_NONVAR(N); N = INTVAL(N);
    DEREF_NONVAR(Ex); Ex = INTVAL(Ex);

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);

    Ey = Ex-N;
    if (!dm_true(dv_ptr_x, Ey-N)) {
        if (domain_set_false_aux(dv_ptr_y, Ey) == BP_FALSE) return BP_FALSE;
    }
    Ey = Ex+N;
    if (!dm_true(dv_ptr_x, Ey+N)) {
        if (domain_set_false_aux(dv_ptr_y, Ey) == BP_FALSE) return BP_FALSE;
    }
    return BP_TRUE;
}

/* abs(abs(X)-abs(Y)) \= N: either X or Y is bound */
int b_ABS_ABS_DIFF_NEQ()
{
    BPLONG X, Y, N, t;
    BPLONG_PTR dv_ptr;

    X = FOLLOW(arreg+3); DEREF_NONVAR(X);
    Y = FOLLOW(arreg+2); DEREF_NONVAR(Y);
    N = FOLLOW(arreg+1); DEREF_NONVAR(N); N = INTVAL(N);

    if (!IS_SUSP_VAR(X)) {
        X = INTVAL(X);
        if (!IS_SUSP_VAR(Y)) {
            Y = INTVAL(Y);
            if (X < 0) X = -X;
            if (Y < 0) Y = -Y;
            if (X-Y == N || Y-X == N) return BP_FALSE; else return BP_TRUE;
        } else {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
        }
    } else {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        X = INTVAL(Y);  /* Y must be an integer */
    }

    if (X < 0) X = -X;
    t = X+N;
    domain_set_false_noint(dv_ptr, t);
    if (domain_set_false_aux(dv_ptr, -t) == BP_FALSE) return BP_FALSE;
    t = X-N;
    if (t >= 0) {
        if (domain_set_false_aux(dv_ptr, t) == BP_FALSE) return BP_FALSE;
        if (domain_set_false_aux(dv_ptr, -t) == BP_FALSE) return BP_FALSE;
    }
    return BP_TRUE;
}
