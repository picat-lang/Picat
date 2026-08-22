/********************************************************************
 *   File   : parvm.c
 *   Purpose: multiple TOAM engines in one process (parsearch, Path B).
 *
 *            The TOAM register/region globals are PAR_TLS (thread-local)
 *            on native multithreaded builds, so each worker thread can
 *            own an independent engine (heap, local stack, trail,
 *            choicepoints) while sharing the read-only program area
 *            (parea), the symbol table, and the code.
 *
 *            Worker arenas are carved from ONE BP_MALLOC block: BP_MALLOC
 *            requires every arena to share the same TOP_BIT, which
 *            per-worker mallocs cannot guarantee on 57-bit VA kernels.
 *
 *            v1 scope: engine bootstrap + isolation test only. No
 *            search splitting yet (that is M3/M4).
 ********************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "term.h"
#include "basic.h"
#include "bapi.h"
#include "extern_decl.h"
#include "frame.h"

#if PAR_THREADS
#include <pthread.h>
#endif

#define PARVM_MAX_ENGINES 256

#if PAR_THREADS

static BPLONG_PTR parvm_reserve_block;  /* one region; all engine arenas */
static BPLONG parvm_reserved = 0;
static BPLONG parvm_stack_words;
static BPLONG parvm_trail_words;

/* Carve n engines out of a single BP_MALLOC block. Must be called on the
   main thread before any worker starts; idempotent for identical sizes. */
int parvm_reserve(BPLONG n, BPLONG stack_words, BPLONG trail_words)
{
    BPLONG_PTR p;
    int tries;

    if (n < 1 || n > PARVM_MAX_ENGINES) return BP_FALSE;
    if (stack_words < 100000 || trail_words < 10000) return BP_FALSE;
    if (parvm_reserved != 0) {
        if (parvm_reserved != n || parvm_stack_words != stack_words
             || parvm_trail_words != trail_words) return BP_FALSE;
        return BP_TRUE;
    }
    p = NULL;
    for (tries = 0; tries < 16 && p == NULL; tries++) {
        BP_MALLOC(p, n * (stack_words + trail_words));
    }
    if (p == NULL) return BP_FALSE;
    parvm_reserve_block = p;
    parvm_reserved = n;
    parvm_stack_words = stack_words;
    parvm_trail_words = trail_words;
    return BP_TRUE;
}

/* Bootstrap this thread's TLS engine state onto carved engine idx.
   Must be the first VM-related action of a worker thread. Fails if this
   thread's TLS is already dirty (engine partially/fully used). */
int parvm_engine_init(BPLONG idx)
{
    BPLONG_PTR base;

    if (idx < 0 || idx >= parvm_reserved) return BP_FALSE;
    if (parvm_reserve_block == NULL) return BP_FALSE;
    if (heap_top || breg || arreg || sfreg || local_top) return BP_FALSE;

    base = parvm_reserve_block + idx * (parvm_stack_words + parvm_trail_words);
    stack_low_addr = base;
    stack_up_addr = base + parvm_stack_words - 1;
    stack_size = parvm_stack_words;
    trail_low_addr = base + parvm_stack_words;
    trail_up_addr = trail_low_addr + parvm_trail_words - 1;
    trail_size = parvm_trail_words;
    trail_water_mark = trail_low_addr + LARGE_MARGIN;
    trail_water_mark0 = trail_low_addr + 2;
    heap_top = stack_low_addr;
    trail_top = trail_up_addr;
    local_top = stack_up_addr;
    local_top0 = local_top;
    cpreg = NULL;
    gc_b = NULL;
    gcQueueConstruct();  /* per-thread GC worklist starts NULL */
    init_stack(0);  /* root SUSP/NONDET/FLAT frames in this arena */
    return BP_TRUE;
}

/* ------------------------------------------------------------------
 * isolation self-test (c_par_vm_test)
 * ------------------------------------------------------------------ */

static BPLONG parvm_make_int_list(BPLONG n)
{
    BPLONG i, lst0;
    BPLONG_PTR ptr;

    if (n == 0) return nil_sym;
    if (local_top - heap_top < 2 * n + 8) return (BPLONG)NULL;
    {
        BPLONG t0 = MAKEINT(n - 1);
        lst0 = ADDTAG(heap_top, LST);
        FOLLOW(heap_top++) = t0;
        ptr = heap_top++;
    }
    for (i = 1; i < n; i++) {
        BPLONG t = MAKEINT(i);
        FOLLOW(ptr) = ADDTAG(heap_top, LST);
        FOLLOW(heap_top++) = t;
        ptr = heap_top++;
    }
    FOLLOW(ptr) = nil_sym;
    return lst0;
}

typedef struct {
    BPLONG idx;
    long ok;
    long heap_after_build;
    long heap_after_gc;
} parvm_worker_result_t;

static void *parvm_worker(void *arg)
{
    parvm_worker_result_t *r = (parvm_worker_result_t *)arg;
    BPLONG l1, l2;

    r->ok = 0;
    r->heap_after_build = 0;
    r->heap_after_gc = 0;
    if (parvm_engine_init(r->idx) == BP_FALSE) return (void *)0;
    if (heap_top != stack_low_addr || trail_top != trail_up_addr) return (void *)0;

    l1 = parvm_make_int_list(1000);
    if (l1 == (BPLONG)NULL) return (void *)0;
    if (heap_top - stack_low_addr != 2000) return (void *)0;
    if (garbage_collector() == BP_ERROR) return (void *)0;
    if (heap_top - stack_low_addr >= 2000) return (void *)0;  /* l1 not rooted: must be reclaimed */

    l2 = parvm_make_int_list(8000);
    if (l2 == (BPLONG)NULL) return (void *)0;
    r->heap_after_build = (long)(heap_top - stack_low_addr);
    if (garbage_collector() == BP_ERROR) return (void *)0;
    r->heap_after_gc = (long)(heap_top - stack_low_addr);
    if (r->heap_after_gc >= 16000) return (void *)0;

    r->ok = 1;
    return (void *)0;
}

int c_par_vm_test()
{
    BPLONG r = ARG(1, 1);
    BPLONG i, ok = BP_TRUE;

    BPLONG_PTR snap_heap = heap_top;
    BPLONG_PTR snap_trail = trail_top;
    BPLONG_PTR snap_breg = breg;
    BPLONG_PTR snap_local = local_top;
    BPLONG_PTR snap_sf = sfreg;
    BPLONG_PTR snap_sla = stack_low_addr;
    BPLONG_PTR snap_sua = stack_up_addr;

    pthread_t tid;
    parvm_worker_result_t res;

    if (parvm_reserve(2, 2000000, 1000000) != BP_TRUE) { ok = BP_FALSE; goto done; }
    for (i = 0; i < 2 && ok == BP_TRUE; i++) {
        res.idx = i;
        res.ok = 0;
        if (pthread_create(&tid, NULL, parvm_worker, &res) != 0) { ok = BP_FALSE; break; }
        pthread_join(tid, NULL);
        if (!res.ok) ok = BP_FALSE;
    }
    /* main thread state must be untouched */
    if (heap_top != snap_heap || trail_top != snap_trail || breg != snap_breg
         || local_top != snap_local || sfreg != snap_sf
         || stack_low_addr != snap_sla || stack_up_addr != snap_sua) {
        ok = BP_FALSE;
    }
done:
    return unify(r, MAKEINT(ok));
}

void Cboot_parvm()
{
    insert_cpred("c_par_vm_test", 1, c_par_vm_test);
}

#else /* !PAR_THREADS: single-threaded targets (wasm, non-Linux) */

int c_par_vm_test()
{
    BPLONG r = ARG(1, 1);
    return unify(r, BP_FALSE);
}

void Cboot_parvm()
{
    insert_cpred("c_par_vm_test", 1, c_par_vm_test);
}

#endif  /* PAR_THREADS */
