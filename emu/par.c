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
 *              c_par_scan(X, R)    R = prefix sums of X  (list)
 *              c_par_scale(X, S, R) R = elementwise X * S (list)
 *              c_par_fib_fast(N, R) R = [fib(1), ..., fib(N)] mod 2^64 (list)
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
/* worst-case heap words per numeric list element: 2 (list cell) +
   ~9 (bigint) + slack */
#define PAR_WORDS_PER_ELEM 12
/* worst-case heap words per numeric list element: 2 (list cell) +
   ~9 (bigint) + slack */


typedef enum { PAR_SUM, PAR_PROD, PAR_MIN, PAR_MAX, PAR_MAP, PAR_SCAN, PAR_FIB } par_op_t;

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

/*
 * Build a term for a signed 64-bit value. 1-word ints (inline range
 * +/- (2^56-1)) are a distinct term type from bigints and are what
 * integer literals in that range are, so return a 1-word int whenever
 * possible and only fall back to a signed bigint beyond it.
 * (writef %d prints 1-word ints truncated to 32 bits -- use %w.)
 */
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

/* Build a fresh list of terms from an array of numbers.
   Each term must be fully built BEFORE its list cell is reserved,
   because term construction allocates on the same heap. */
static BPLONG u64array_to_list(const uint64_t *a, BPLONG len, int signed_val)
{
    BPLONG i, lst0;
    BPLONG_PTR ptr;

    LOCAL_OVERFLOW_CHECK_WITH_MARGIN("par", PAR_WORDS_PER_ELEM * len + 8);
    if (len == 0) return nil_sym;
    {
        BPLONG t0 = signed_val ? i64_to_term((BPLONG)a[0]) : u64_to_term(a[0]);
        lst0 = ADDTAG(heap_top, LST);
        FOLLOW(heap_top++) = t0;
        ptr = heap_top++;
    }
    for (i = 1; i < len; i++) {
        BPLONG t = signed_val ? i64_to_term((BPLONG)a[i]) : u64_to_term(a[i]);
        FOLLOW(ptr) = ADDTAG(heap_top, LST);
        FOLLOW(heap_top++) = t;
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
    uint64_t *out;
    size_t lo, hi;
    par_op_t op;
    uint64_t scale;
    uint64_t acc;
} par_chunk_t;

/* fib(n) mod 2^64 by fast doubling: f = fib(n), f1 = fib(n+1). */
static void fib_u64(uint64_t n, uint64_t *f, uint64_t *f1)
{
    uint64_t a = 0, b = 1;
    int i;
    for (i = 63; i >= 0; i--) {
        uint64_t c = a * ((b << 1) - a); /* 2ab - a^2 = fib(2k)   */
        uint64_t d = a * a + b * b;      /* a^2 + b^2 = fib(2k+1) */
        if ((n >> i) & 1) { a = d; b = c + d; }
        else               { a = c; b = d; }
    }
    *f = a;
    *f1 = b;
}

static void *par_chunk_run(void *arg)
{
    par_chunk_t *c = (par_chunk_t *)arg;
    size_t i;
    uint64_t acc;
    int64_t m;

    switch (c->op) {
    case PAR_SUM:
    case PAR_SCAN:
        acc = 0;
        for (i = c->lo; i < c->hi; i++) {
            acc += c->d[i];
            if (c->op == PAR_SCAN) c->out[i] = acc;
        }
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
    case PAR_MAP:
        for (i = c->lo; i < c->hi; i++) c->out[i] = c->d[i] * c->scale;
        break;
    case PAR_FIB: {
        uint64_t f, f1;
        for (i = c->lo; i < c->hi; i++) {
            fib_u64(i + 1, &f, &f1);
            c->out[i] = f;
        }
        break;
    }
    }
    return 0;
}

/* Number of worker threads for n elements. */
static int par_nthreads(size_t n)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int nt = (cpus > 0) ? (int)cpus : 1;
    if (nt > PAR_NTHREADS_MAX) nt = PAR_NTHREADS_MAX;
    if (n < PAR_SEQUENTIAL_BELOW) return 1;
    if (nt > (int)(n / 1024)) nt = (int)(n / 1024);
    return nt < 1 ? 1 : nt;
}

/* Fill the lo/hi of chunk k (0-based) for n elements in nt chunks. */
static void par_chunk_bounds(size_t n, int nt, int k, size_t *lo, size_t *hi)
{
    size_t len = n / nt;
    size_t start = 0;
    int j;
    if ((size_t)k < n % nt) len++;
    for (j = 0; j < k; j++) {
        size_t lj = n / nt;
        if ((size_t)j < n % nt) lj++;
        start += lj;
    }
    *lo = start;
    *hi = start + len;
}

/* Fork par_nthreads(n) workers running par_chunk_run on chunks[0..nt).
   The caller must have pre-filled d, out, op, scale of each chunk that
   may run; par_chunk_bounds assigns lo/hi. Returns 0 on success. */
static int par_fork_join(size_t n, par_chunk_t *chunks)
{
    int nt = par_nthreads(n);
    static pthread_t tids[PAR_NTHREADS_MAX];

    for (int k = 0; k < nt; k++) par_chunk_bounds(n, nt, k, &chunks[k].lo, &chunks[k].hi);
    if (nt == 1) {
        par_chunk_run(&chunks[0]);
        return 0;
    }
    for (int k = 0; k < nt; k++) {
        if (pthread_create(&tids[k], NULL, par_chunk_run, &chunks[k]) != 0) {
            for (int j = 0; j < k; j++) pthread_join(tids[j], NULL);
            return -1;
        }
    }
    for (int k = 0; k < nt; k++) pthread_join(tids[k], NULL);
    return 0;
}

/* n > 0. Returns 0 on success. */
static int par_reduce(uint64_t *d, size_t n, par_op_t op, uint64_t *res)
{
    int nt = par_nthreads(n);
    static par_chunk_t chunks[PAR_NTHREADS_MAX];
    int k;

    for (k = 0; k < nt; k++) {
        chunks[k].d = d;
        chunks[k].op = op;
    }
    if (par_fork_join(n, chunks) != 0) return -1;

    switch (op) {
    case PAR_SUM:  { uint64_t a = 0; for (k = 0; k < nt; k++) a += chunks[k].acc; *res = a; } break;
    case PAR_PROD: { uint64_t a = 1; for (k = 0; k < nt; k++) a *= chunks[k].acc; *res = a; } break;
    case PAR_MIN:  { int64_t a = INT64_MAX; for (k = 0; k < nt; k++) { int64_t v = (int64_t)chunks[k].acc; if (v < a) a = v; } *res = (uint64_t)a; } break;
    case PAR_MAX:  { int64_t a = INT64_MIN; for (k = 0; k < nt; k++) { int64_t v = (int64_t)chunks[k].acc; if (v > a) a = v; } *res = (uint64_t)a; } break;
    default: *res = 0; break;
    }
    return 0;
}

/* Elementwise out[i] = in[i] * scale (mod 2^64). n > 0. */
static int par_map(const uint64_t *in, uint64_t *out, size_t n, uint64_t scale)
{
    int nt = par_nthreads(n);
    static par_chunk_t chunks[PAR_NTHREADS_MAX];

    for (int k = 0; k < nt; k++) {
        chunks[k].d = in;
        chunks[k].out = out;
        chunks[k].op = PAR_MAP;
        chunks[k].scale = scale;
    }
    return par_fork_join(n, chunks);
}

/* out[i] = sum of in[0..i] (inclusive prefix sum, mod 2^64). n > 0. */
static int par_prefix(const uint64_t *in, uint64_t *out, size_t n)
{
    int nt = par_nthreads(n);
    static par_chunk_t chunks[PAR_NTHREADS_MAX];

    for (int k = 0; k < nt; k++) {
        chunks[k].d = in;
        chunks[k].out = out;
        chunks[k].op = PAR_SCAN;
        chunks[k].acc = 0;
    }
    if (par_fork_join(n, chunks) != 0) return -1;

    /* offset each chunk by the sum of all previous chunks */
    {
        uint64_t off = 0;
        for (int k = 0; k < nt; k++) {
            for (size_t i = chunks[k].lo; i < chunks[k].hi; i++) out[i] += off;
            off += chunks[k].acc;
        }
    }
    return 0;
}

/* out[i] = fib(i+1) mod 2^64. n > 0. */
static int par_fib_run(const uint64_t *in, uint64_t *out, size_t n)
{
    int nt = par_nthreads(n);
    static par_chunk_t chunks[PAR_NTHREADS_MAX];

    for (int k = 0; k < nt; k++) {
        chunks[k].d = in;
        chunks[k].out = out;
        chunks[k].op = PAR_FIB;
    }
    return par_fork_join(n, chunks);
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

/* ------------------------------------------------------------------
 * heap room: building a large result list needs many contiguous heap
 * words. Two costs per element: the list cell (2 words) and, for
 * values outside the one-word integer range, a bigint (~9 words).
 * The VM only expands the arena at instruction boundaries, so a
 * cpred that out-grows the free space must do it itself:
 * garbage_collector() frees unreachable terms in place, and
 * expand_local_global_stacks() relocates the arena -- so the result
 * slot (like every other term address) must be re-fetched after.
 * ---------------------------------------------------------------- */


static int ensure_heap_room(BPLONG need, BPLONG *r, BPLONG arity, BPLONG pos)
{
    int tries = 0;
    BPLONG total;
    garbage_collector();
    while (local_top - heap_top <= need && tries < 16) {
        if (toam_signal_vec != 0 || in_critical_region != 0) return -1;
        /* one-size-fits-all jump instead of repeated doublings */
        total = (heap_top - stack_low_addr) + (stack_up_addr - local_top)
                + need + LARGE_MARGIN;
        if (expand_local_global_stacks(total) == BP_ERROR) {
            if (expand_local_global_stacks(0) == BP_ERROR) return -1;
        }
        *r = ARG(pos, arity);  /* arena moved: re-fetch the result slot */
        tries++;
    }
    return (local_top - heap_top > need) ? 0 : -1;
}

int c_par_scan()
{
    BPLONG x = ARG(1, 2), r = ARG(2, 2);
    uint64_t *in = 0, *out;
    BPLONG n;

    if (!collect_input(x, &in, &n)) { free(in); return BP_FALSE; }
    if (n == 0) { free(in); return unify(r, nil_sym); }
    out = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (out == NULL || par_prefix(in, out, n) != 0) {
        free(out); free(in);
        return BP_ERROR;
    }
    if (ensure_heap_room(PAR_WORDS_PER_ELEM * n + 16, &r, 2, 2) != 0) {
        free(out); free(in);
        return BP_ERROR;
    }
    {
        BPLONG L = u64array_to_list(out, n, 1);
        free(out); free(in);
        return unify(r, L);
    }
}

int c_par_scale()
{
    BPLONG x = ARG(1, 3), sarg = ARG(2, 3), r = ARG(3, 3);
    BPLONG s, n;
    uint64_t *in = 0, *out;

    if (!term_to_i64(sarg, &s)) return BP_FALSE;
    if (!collect_input(x, &in, &n)) { free(in); return BP_FALSE; }
    if (n == 0) { free(in); return unify(r, nil_sym); }
    out = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (out == NULL || par_map(in, out, n, (uint64_t)s) != 0) {
        free(out); free(in);
        return BP_ERROR;
    }
    if (ensure_heap_room(PAR_WORDS_PER_ELEM * n + 16, &r, 3, 3) != 0) {
        free(out); free(in);
        return BP_ERROR;
    }
    {
        BPLONG L = u64array_to_list(out, n, 1);
        free(out); free(in);
        return unify(r, L);
    }
}

int c_par_fib_fast()
{
    BPLONG narg = ARG(1, 2), r = ARG(2, 2);
    BPLONG n;
    uint64_t *out;

    if (!term_to_i64(narg, &n) || n < 0) return BP_FALSE;
    if (n == 0) return unify(r, nil_sym);
    out = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (out == NULL || par_fib_run(0, out, n) != 0) {
        free(out);
        return BP_ERROR;
    }
    if (ensure_heap_room(PAR_WORDS_PER_ELEM * n + 16, &r, 2, 2) != 0) {
        free(out);
        return BP_ERROR;
    }
    {
        BPLONG L = u64array_to_list(out, n, 0);  /* unsigned representatives */
        free(out);
        return unify(r, L);
    }
}

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
    insert_cpred("c_par_scan", 2, c_par_scan);
    insert_cpred("c_par_scale", 3, c_par_scale);
    insert_cpred("c_par_fib_fast", 2, c_par_fib_fast);
    insert_cpred("c_wall_ms", 1, c_wall_ms);
}
