/********************************************************************
 *   File   : par.c
 *   Purpose: data-parallel builtins for Picat 3.9.
 *
 *            Fork-join data parallelism on top of pthreads. The VM
 *            is single-threaded, so worker threads only do pure C
 *            arithmetic on a private snapshot of the input data
 *            (malloc'd, no VM heap access), and the results are
 *            combined on the calling (VM) thread. This follows the
 *            same discipline as the timer threads in event.c.
 *
 *            Semantics: all arithmetic is 64-bit two's-complement,
 *            wrapping mod 2^64 (same convention as Futhark i64).
 *            Elements may be inline integers or bignints in the
 *            signed 64-bit range; input that is not a numeric
 *            list/array makes the call fail (as sort does).
 *
 *            cpred API (called via bp.<name> from Picat modules):
 *              c_par_sum(X, S)     S = sum of X      (empty -> 0)
 *              c_par_prod(X, P)    P = product of X  (empty -> 1)
 *              c_par_min(X, M)     M = minimum of X  (empty -> fail)
 *              c_par_max(X, M)     M = maximum of X  (empty -> fail)
 *              c_wall_ms(T)        T = wall-clock ms since epoch
 ********************************************************************/
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>

#include "term.h"
#include "basic.h"
#include "bapi.h"
#include "extern_decl.h"

#define PAR_NTHREADS_MAX 512
#define PAR_SEQUENTIAL_BELOW 262144

typedef enum { PAR_SUM, PAR_PROD, PAR_MIN, PAR_MAX } par_op_t;

/* ------------------------------------------------------------------
 * number <-> term conversion
 * ---------------------------------------------------------------- */

/* Read a term as a signed 64-bit value. Returns 1 on success.
   Accepts inline integers and bignints within the i64 range. */
static int term_to_i64(BPLONG t, BPLONG *s)
{
    uint64_t v;
    BPLONG sign, size;
    UBIGINT DLst;
    int i;

    DEREF(t);
    if (ISINT(t)) { *s = INTVAL(t); return 1; }
    if (!IS_BIGINT(t)) return 0;
    {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(t);
        size = INTVAL(FOLLOW(ptr + 1));
        sign = (size < 0) ? -1 : 1;
        if (size < 0) size = -size;
        /* walk the digit list, least significant digit first (base 2^28) */
        v = 0;
        {
            uint64_t pow = 1;
            BPLONG lst = FOLLOW(ptr + 2);
            for (i = 0; i < size; i++) {
                BPLONG_PTR c = (BPLONG_PTR)UNTAGGED_ADDR(lst);
                BPLONG d = INTVAL(FOLLOW(c));
                if ((uint64_t)d > (UINT64_MAX - v) / pow) return 0;
                v += (uint64_t)d * pow;
                pow *= 268435456ULL;
                lst = FOLLOW(c + 1);
            }
        }
    }
    if (sign == 1) {
        if (v > (uint64_t)INT64_MAX) return 0;
        *s = (BPLONG)v;
    } else {
        if (v > (uint64_t)INT64_MAX + 1) return 0;
        if (v == (uint64_t)INT64_MAX + 1) *s = (BPLONG)0x8000000000000000ULL;
        else *s = (BPLONG)(-(int64_t)v);
    }
    return 1;
}

/* Build a term for a signed 64-bit value (inline int if representable). */
static BPLONG i64_to_term(BPLONG s)
{
    if (BP_IN_1W_INT_RANGE(s)) return MAKEINT(s);
    if ((uint64_t)s == (uint64_t)0x8000000000000000ULL) return bp_int64_min_to_bigint();
    return bp_int_to_bigint(s);
}

/* Build a term for an unsigned 64-bit value (0 .. 2^64-1). */
static BPLONG u64_to_term(uint64_t v)
{
    if (v <= (uint64_t)BP_MAXINT_1W) return MAKEINT((BPLONG)v);
    return bp_uint64_to_bigint(v);
}

/* Build a fresh list of terms from an array of numbers. */
static BPLONG u64array_to_list(const uint64_t *a, BPLONG len, int signed_val)
{
    BPLONG i, lst0;
    BPLONG_PTR ptr;

    LOCAL_OVERFLOW_CHECK_WITH_MARGIN("par", 2 * len + 2);
    if (len == 0) return nil_sym;
    lst0 = ADDTAG(heap_top, LST);
    FOLLOW(heap_top++) = signed_val ? i64_to_term((BPLONG)a[0]) : u64_to_term(a[0]);
    ptr = heap_top++;
    for (i = 1; i < len; i++) {
        FOLLOW(ptr) = ADDTAG(heap_top, LST);
        FOLLOW(heap_top++) = signed_val ? i64_to_term((BPLONG)a[i]) : u64_to_term(a[i]);
        ptr = heap_top++;
    }
    FOLLOW(ptr) = nil_sym;
    return lst0;
}

/* ------------------------------------------------------------------
 * input collection: list or array of numbers -> growable C buffer
 * ---------------------------------------------------------------- */

static int collect_input(BPLONG x, uint64_t **buf, BPLONG *n)
{
    uint64_t *b = (uint64_t *)malloc(sizeof(uint64_t) * 1024);
    BPLONG cnt = 0, cap = 1024;
    BPLONG s;

    if (b == NULL) return 0;
    DEREF(x);
    if (b_IS_ARRAY_c(x)) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(x);
        BPLONG i = GET_ARITY((SYM_REC_PTR)FOLLOW(ptr));
        for (; i > 0; i--) {
            BPLONG e = FOLLOW(ptr + i);
            if (!term_to_i64(e, &s)) { free(b); return 0; }
            if (cnt == cap) { cap *= 2; b = (uint64_t *)realloc(b, cap * sizeof(uint64_t)); if (!b) return 0; }
            b[cnt++] = (uint64_t)s;
        }
        *buf = b; *n = cnt;
        return 1;
    }
    DEREF(x);
    while (ISLIST(x)) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(x);
        BPLONG e = FOLLOW(ptr);
        if (!term_to_i64(e, &s)) { free(b); return 0; }
        if (cnt == cap) { cap *= 2; b = (uint64_t *)realloc(b, cap * sizeof(uint64_t)); if (!b) return 0; }
        b[cnt++] = (uint64_t)s;
        x = FOLLOW(ptr + 1);
        DEREF(x);
    }
    *buf = b; *n = cnt;
    return 1;
}

/* ------------------------------------------------------------------
 * fork-join engine
 * ---------------------------------------------------------------- */

typedef struct {
    const uint64_t *d;
    size_t lo, hi;
    par_op_t op;
    uint64_t acc;
} par_chunk_t;

static void *par_chunk_run(void *arg)
{
    par_chunk_t *c = (par_chunk_t *)arg;
    size_t i;
    uint64_t acc;
    int64_t m;

    switch (c->op) {
    case PAR_SUM:
        acc = 0;
        for (i = c->lo; i < c->hi; i++) acc += c->d[i];
        c->acc = acc;
        break;
    case PAR_PROD:
        acc = 1;
        for (i = c->lo; i < c->hi; i++) acc *= c->d[i];
        c->acc = acc;
        break;
    case PAR_MIN:
        m = INT64_MAX;
        for (i = c->lo; i < c->hi; i++) { int64_t v = (int64_t)c->d[i]; if (v < m) m = v; }
        c->acc = (uint64_t)m;
        break;
    case PAR_MAX:
        m = INT64_MIN;
        for (i = c->lo; i < c->hi; i++) { int64_t v = (int64_t)c->d[i]; if (v > m) m = v; }
        c->acc = (uint64_t)m;
        break;
    }
    return 0;
}

static size_t par_chunk_len(size_t n, int k, int nt)
{
    size_t len = n / nt;
    if ((size_t)k < n % nt) len++;
    return len;
}

/* n > 0. Returns 0 on success. */
static int par_reduce(uint64_t *d, size_t n, par_op_t op, uint64_t *res)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int nt = (cpus > 0) ? (int)cpus : 1;
    static pthread_t tids[PAR_NTHREADS_MAX];
    static par_chunk_t chunks[PAR_NTHREADS_MAX];

    if (nt > PAR_NTHREADS_MAX) nt = PAR_NTHREADS_MAX;
    if (n < PAR_SEQUENTIAL_BELOW || nt > (int)(n / 1024)) nt = 1;
    if (nt < 1) nt = 1;

    if (nt == 1) {
        par_chunk_t c = { d, 0, n, op, 0 };
        par_chunk_run(&c);
        *res = c.acc;
        return 0;
    }

    for (int k = 0; k < nt; k++) {
        size_t lo = 0;
        for (int j = 0; j < k; j++) lo += par_chunk_len(n, j, nt);
        chunks[k].d = d;
        chunks[k].lo = lo;
        chunks[k].hi = lo + par_chunk_len(n, k, nt);
        chunks[k].op = op;
        chunks[k].acc = 0;
        if (pthread_create(&tids[k], NULL, par_chunk_run, &chunks[k]) != 0) {
            for (int j = 0; j < k; j++) pthread_join(tids[j], NULL);
            return -1;
        }
    }
    for (int k = 0; k < nt; k++) pthread_join(tids[k], NULL);

    switch (op) {
    case PAR_SUM:  { uint64_t a = 0; for (int k = 0; k < nt; k++) a += chunks[k].acc; *res = a; } break;
    case PAR_PROD: { uint64_t a = 1; for (int k = 0; k < nt; k++) a *= chunks[k].acc; *res = a; } break;
    case PAR_MIN:  { int64_t a = INT64_MAX; for (int k = 0; k < nt; k++) { int64_t v = (int64_t)chunks[k].acc; if (v < a) a = v; } *res = (uint64_t)a; } break;
    case PAR_MAX:  { int64_t a = INT64_MIN; for (int k = 0; k < nt; k++) { int64_t v = (int64_t)chunks[k].acc; if (v > a) a = v; } *res = (uint64_t)a; } break;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * cpreds
 * ---------------------------------------------------------------- */

static int c_par_aggregate(par_op_t op, int identity_ok)
{
    BPLONG x = ARG(1, 2), r = ARG(2, 2);
    uint64_t *d = 0;
    BPLONG n;
    uint64_t res;

    if (!collect_input(x, &d, &n)) { free(d); return BP_FALSE; }
    if (n == 0) {
        free(d);
        if (!identity_ok) return BP_FALSE;
        res = (op == PAR_SUM) ? 0 : 1;
        return unify(r, i64_to_term((BPLONG)res));
    }
    if (par_reduce(d, n, op, &res) != 0) { free(d); return BP_ERROR; }
    free(d);
    return unify(r, i64_to_term((BPLONG)res));
}

int c_par_sum()   { return c_par_aggregate(PAR_SUM, 1); }
int c_par_prod()  { return c_par_aggregate(PAR_PROD, 1); }
int c_par_min()   { return c_par_aggregate(PAR_MIN, 0); }
int c_par_max()   { return c_par_aggregate(PAR_MAX, 0); }

int c_wall_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return unify(ARG(1, 1), i64_to_term((BPLONG)(tv.tv_sec * 1000LL + tv.tv_usec / 1000)));
}

void Cboot_par()
{
    insert_cpred("c_par_sum", 2, c_par_sum);
    insert_cpred("c_par_prod", 2, c_par_prod);
    insert_cpred("c_par_min", 2, c_par_min);
    insert_cpred("c_par_max", 2, c_par_max);
    insert_cpred("c_wall_ms", 1, c_wall_ms);
}
