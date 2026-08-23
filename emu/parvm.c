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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define PARVM_MAX_ENGINES 256

/* M2 cpreds (defined below, in both PAR_THREADS sections) */
int c_pvm_fork(void);
int c_pvm_delegate(void);
int c_pvm_worker_id(void);
int c_pvm_chunk(void);
int c_pvm_report(void);
int c_pvm_collect(void);
int c_pvm_solution(void);

extern int toam(BPLONG_PTR, BPLONG_PTR, BPLONG_PTR);

/* env-gated protocol trace switch (defined further down) */
static int pvm_dbg_on(void);

/* mode 1/3: the reported solution, copied out of the shared block by
   the root's c_pvm_collect (before the unmap) and served by
   c_pvm_solution. -1 = nothing reported yet. */
static BPLONG pvm_sol_cache[PVM_SOL_CAP];
static long pvm_sol_len = -1;

/* Serialize t (a list or array of ground integers) into the session's
   shared solution region, then mark it complete (sol_len). Returns
   BP_ERROR (with bp_exception set) if t is not such a term or the
   solution is longer than PVM_SOL_CAP. Called only after the process
   has won the report (CAS on pvm_shm->found), so the write is racing
   against no other writer. */
static int pvm_serialize_solution(BPLONG t)
{
    BPLONG sym, v;
    long n = 0;

    if (b_IS_ARRAY_c(t)) {
        BPLONG_PTR ptr;
        BPLONG i;

        if (!ISATOM(t)) {       /* an empty array is the atom "{}()" */
            ptr = (BPLONG_PTR)UNTAGGED_ADDR(t);
            sym = FOLLOW(ptr);
            n = GET_ARITY((SYM_REC_PTR)sym);
            if (n > PVM_SOL_CAP) {
                bp_exception = out_of_range;
                return BP_ERROR;
            }
            for (i = 1; i <= n; i++) {
                v = FOLLOW(ptr + i);
                DEREF(v);
                if (!ISINT(v)) {
                    bp_exception = c_type_error(et_INTEGER, t);
                    return BP_ERROR;
                }
                pvm_shm->sol[i - 1] = v;
            }
        }
    } else if (ISLIST(t)) {
        BPLONG_PTR ptr;

        while (ISLIST(t)) {
            if (n == PVM_SOL_CAP) {
                bp_exception = out_of_range;
                return BP_ERROR;
            }
            ptr = (BPLONG_PTR)UNTAGGED_ADDR(t);
            v = FOLLOW(ptr);
            DEREF(v);
            if (!ISINT(v)) {
                bp_exception = c_type_error(et_INTEGER, t);
                return BP_ERROR;
            }
            pvm_shm->sol[n++] = v;
            t = FOLLOW(ptr + 1);
            DEREF(t);
        }
    } else {
        bp_exception = c_type_error(et_LIST, t);
        return BP_ERROR;
    }
    __sync_synchronize();
    pvm_shm->sol_len = n;      /* completion marker (data written above) */
    return BP_TRUE;
}

/* Root side: materialize the reported solution as a fresh integer
   array of length sol_len. Errors if no solution was reported. */
int c_pvm_solution()
{
    BPLONG s = ARG(1, 1);
    BPLONG a, i;
    BPLONG_PTR ap;

    DEREF(s);
    if (pvm_sol_len < 0 || pvm_sol_len > PVM_SOL_CAP) {
        bp_exception = illegal_arguments;  /* nothing reported */
        return BP_ERROR;
    }
    a = picat_build_array(pvm_sol_len);
    ap = (BPLONG_PTR)UNTAGGED_ADDR(a);
    for (i = 0; i < pvm_sol_len; i++)
        ap[i + 1] = pvm_sol_cache[i];
    ASSIGN_f_atom(s, a);
    return BP_TRUE;
}

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
    insert_cpred("pvm_delegate", 1, c_pvm_delegate);
    insert_cpred("pvm_fork", 3, c_pvm_fork);
    insert_cpred("pvm_worker_id", 1, c_pvm_worker_id);
    insert_cpred("pvm_chunk", 2, c_pvm_chunk);
    insert_cpred("pvm_report", 1, c_pvm_report);
    insert_cpred("pvm_collect", 1, c_pvm_collect);
    insert_cpred("pvm_solution", 1, c_pvm_solution);
}

/* ------------------------------------------------------------------
 * M2: OR-parallel CP search over forked search processes.
 *
 * Top-level primitives (called from the user's main):
 *
 *   bp.pvm_fork(NT, Mode, A)   start a parallel session with NT search
 *       processes. Mode 1 = first solution, 2 = count all. A = the
 *       number of values of the partition variable (mode 2).
 *   bp.pvm_worker_id(I)        this process's worker id (0 = root).
 *   bp.pvm_chunk(Lo,Hi)        this process's value slice for the
 *       partition variable (mode 2; the whole [1,A] for mode 1/root).
 *   bp.pvm_report(S)           mode 1/3: report the solution BY VALUE
 *       (S = a list or array of ground integers; the first reporter
 *       wins, others are no-ops); mode 2: report N solutions for the
 *       local slice.
 *   bp.pvm_solution(S)         root side, after pvm_collect: bind S to
 *       a fresh array holding the reported solution (mode 1/3, R=1).
 *   bp.pvm_collect(R)          root only: wait for the worker processes
 *       and return R (1/0 in mode 1/3, the total count in mode 2).
 *
 *   The user's main forks the session and then runs ONE body, with the
 *   report at the model's success point:
 *       ( model, pvm_report(Sol) ; true ), pvm_collect(R),
 *       ( R = 1, pvm_solution(S), print ; ... ).
 *   In mode 1 the root is worker 0 and searches its own territory as
 *   usual; on every choice point the FORK/SET_FORK hook
 *   (PVM_FORK_MAYBE, toam.h) may fork a child that takes the
 *   disjunction's remaining clauses while the caller (parent) keeps
 *   the first, growing the pool up to NT (disjoint OR subtrees, so a
 *   solution is examined exactly once). The first process to a
 *   solution reports its value (integers to the shared block, then
 *   found 0->1 by CAS) and finishes; other processes notice found at
 *   their next choicepoint and leave; the root unwinds its session to
 *   the user's ( model ; true ) cut (its own model failure there is
 *   expected when the solution was in a worker's territory), collects,
 *   and materializes the reported value.
 *
 *   Mode 1/3 value-chunking protocol (see pvm_fork_frame): a
 *   disjunction whose re-entry re-executes its FORK site (the CP
 *   value disjunctions) is split into contiguous value chunks of C
 *   values (mode 1: C = 1; mode 3: C = bp.pvm_fork third arg). The
 *   forking process (the owner) keeps values 1..C and walks them
 *   normally (its frame cell is NOT patched, so value failures
 *   re-enter through the original re-entry). A worker is dispatched
 *   at the original re-entry, which advances from value 1 to value
 *   2; its pending skip (from-2) makes it re-dispatch the re-entry
 *   once per skipped value (hook return 3) until it lands on its
 *   first value.
 *
 *   Pool (mode 1/3): process slots flow to the deepest active
 *   frontier. A fork (first child of a fresh frame, t=0 tail fork,
 *   or a boundary re-fork) is allowed only when a process slot is
 *   free AND the forking frame is at least as deep as every other
 *   live process's frontier frame (per-seat table in the shm;
 *   deeper = lower frame address). A worker that exhausts its chunk
 *   WITH a child covering the rest hands off (exit status 77, the
 *   successor's pid in the shared done registry) and releases its
 *   slot; the (a) waiter (root, or a worker parked on a deep
 *   delegation) follows the 77 chain in the registry -- a successor
 *   is often a grandchild, invisible to waitpid. A worker that
 *   exhausts its chunk WITHOUT a child (slot starved) becomes the
 *   tail walker: it walks further values itself and retries the
 *   re-fork at every value boundary, so a freed slot immediately
 *   spawns the next chunk link and the tail walker hands off and
 *   exits (the serial tail walk exists only as long as the pool is
 *   genuinely starved). A worker that backtracks ABOVE its root
 *   delegation frame has exhausted its region and exits 0.
 *   (a) waiting is a futex park (about 20 ms ticks) on the shared
 *   wake word; a per-session SIGCHLD handler reaps this process's
 *   direct children early (zombie hygiene + crash flags + slot
 *   release for children that could not self-release). A worker's
 *   own exit records its outcome in the done registry before
 *   _exit; the atexit path (crash/error exits) records a crash
 *   marker and quietly kills its orphan children.
 *
 *   Delegation is attempted only when the engine state equals the
 *   frame's entry state (fresh choice frame), and a forked child
 *   restores the original re-entries of inherited frames outside its
 *   delegation lineage before searching.
 *
 *   A process forked mid-search inherits the C stack inside the toam
 *   loop and the COW engine state; healthy endings _exit (recorded
 *   in the done registry), atexit covers the rest.
 * ------------------------------------------------------------------ */

#define PARVM_MAX_WORKERS 256

PAR_TLS pvm_t pvm;
pvm_shm_t *pvm_shm = NULL;

static char pvm_shm_name[64];
static int pvm_is_fork_child = 0;

/* Delegation window (process-local, COW'd with the rest of the engine
   state): bp.pvm_delegate(1) before solve(), bp.pvm_delegate(0) after.
   Only a frame whose first qualifying firing happens with the window
   open may be delegated. This keeps OUT of delegation the model-setup
   loop frames and -- critically -- the (model, report ; true) fallback
   wrapper: a worker's trivial success in the true clause is
   indistinguishable from exhaustion in the done registry, and the
   deleg-fail stub would then fail the wrapper at the root even though
   the worker clause succeeded (UNSAT -> spurious failure; a found
   solution -> lost). Each COW'd worker inherits the window open (it is
   forked inside the solve) and closes it itself when its search leaves
   the solve; a worker is never delegated at a frame above its root
   frame (scope-exit exits first). */
static long pvm_delegate_depth = 0;

/* bp.pvm_delegate(1) opens the window, bp.pvm_delegate(0) closes it
   (counted, so nested solves need not interleave). Outside a running
   pvm_fork session it is a silent no-op. Process-local: workers keep
   their own copy (see the pvm_delegate_depth comment). */
int c_pvm_delegate(void)
{
    BPLONG i = ARG(1, 1);
    long on;

    DEREF(i);
    if (!ISINT(i)) {
        bp_exception = c_type_error(et_INTEGER, i);
        return BP_ERROR;
    }
    on = INTVAL(i);
    if (on > 0)
        pvm_delegate_depth++;
    else if (pvm_delegate_depth > 0)
        pvm_delegate_depth--;
    if (pvm_dbg_on())
        fprintf(stderr, "PVMDEL pid=%d on=%ld depth->%ld\n",
                (int)getpid(), on, (long)pvm_delegate_depth);
    return BP_TRUE;
}

/* Debug force: PVM_FORCE_AR=<hex> delegates that exact frame at its
   first CLEAN firing (the state gate still applies), deliberately
   bypassing the tombstone first-firing rule, to prove the worker
   chunk-walk on real value disjunctions. Only the root (a non-child)
   may force. */
static BPLONG pvm_force_ar = 0;

/* The arena base is ASLR-shifted between runs, but frame offsets are
   stable: match on the low 20 bits (= a 1 MB window, far more than
   these engines' local stacks use). */
#define PVM_FORCE_MATCH(a) \
    (pvm_force_ar != 0 && \
     ((BPLONG)(a) & 0xFFFFFULL) == (pvm_force_ar & 0xFFFFFULL))

static pid_t pvm_my_children[PARVM_MAX_WORKERS];
static int pvm_nchildren = 0;
/* Set (this process) when a direct child died abnormally -- signalled
   or non-zero exit (other than the transfer status); a healthy
   worker always exits 0 or PVM_ST_TRANSFER. Also ORed into the
   shared pvm_shm->bad so the root sees a death anywhere in the tree.
   pvm_collect rejects a result if bad is set and the pool was not
   intentionally SIGKILLed (i.e. it is a real crash, which in count
   mode means the total is a silent undercount). */
static int pvm_child_bad = 0;
/* 1 while reaping children we just SIGKILLed on purpose (found set,
   mode 1/3): their signalled exits are expected, not failures. */
static int pvm_reap_quiet = 0;

/* Pool (mode 1/3) exit protocol:
   0                 = my disjunction region is exhausted (or found
                       was reported on the way out)
   PVM_ST_TRANSFER   = my chunk is exhausted and my child covers the
                       rest of the disjunction (its pid is the succ
                       of my done entry); I exit to release my
                       process slot, and the (a) waiter follows the
                       chain in the done registry */
#define PVM_ST_TRANSFER 77
#define PVM_ST_CRASH    (-1)
/* Seat-depth sentinel: "no frontier observed yet" (shallowest
   possible frame address). */
#define PVM_DEPTH_SHALLOW 0x7fffffffffffffLL

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

/* My seat index in the shared frontier table (-1 = unresolved).
   COW per process like everything else in this section. */
static int pvm_my_seat = -1;
/* This process's root delegation frame (the frame it was forked for;
   0 = none, i.e. the root). A worker's region is the subtree of that
   disjunction; "I left my region" is detected ON-SIDE, not by frame
   address (the local-stack frame allocator is non-monotone in depth:
   frames nested BELOW the root frame sit at higher addresses, and GC
   trail compaction / local-stack expansion move saved pointers):
   pvm_root_onside flips to 0 at the root frame's own lab_fail (its
   value failed) and back to 1 when the disjunction re-fires (value
   advance); a FORK firing while NOT onside is therefore a frame
   pushed in the CALLER's context -- the disjunction exhausted. A
   missed hook anywhere degrades to over-search through the driver
   tail (still a correct exit), never to a premature region-done.
   The frame's identity across stack expansion is by OFFSET from the
   fixed local-stack anchor stack_up_addr (the engine itself keeps
   frame offsets, see the C_PRED restore in emu_inst.h). */
static BPLONG pvm_my_rootframe = 0;
static long pvm_rootframe_off = 0;
static int pvm_root_onside = 0;

/* PVM_DBG=1: one-line protocol trace on stderr. */
static int pvm_dbg_on(void)
{
    static int on = -1;

    if (on < 0) on = (getenv("PVM_DBG") != NULL);
    return on;
}

static void pvm_dbg(const char *tag, BPLONG_PTR ar, long extra)
{
    if (pvm_dbg_on())
        fprintf(stderr, "%s pid=%d ar=%p x=%ld\n", tag, (int)getpid(),
                (void *)ar, extra);
}

/* Bump the shared waker word and wake parked (a) waiters. */
static void pvm_pool_wake(void)
{
    if (pvm_shm == NULL) return;
    __sync_fetch_and_add(&pvm_shm->wake, 1);
    syscall(SYS_futex, (void *)&pvm_shm->wake, FUTEX_WAKE, 64, NULL, NULL, 0);
}

static int pvm_resolve_my_seat(void)
{
    long i, me = (long)getpid();

    if (pvm_my_seat >= 0) return pvm_my_seat;
    if (pvm_shm == NULL) return -1;
    for (i = 0; i < PVM_POOL_SEATS; i++)
        if (pvm_shm->pvm_seat[i].pid == me)
            return (pvm_my_seat = (int)i);
    return -1;
}

/* Claim (root, at arm time) or resolve (a fork child, whose seat the
   parent marks at spawn) my frontier seat, and set its depth to my
   current frontier frame. A fork child NEVER claims a free seat
   (that would race the parent's spawn-time assignment into a
   duplicate seat and a double slot release): if its seat is not
   marked yet, the update is skipped -- the parent marks it with the
   right depth microseconds later, and the claim is retried at the
   next hook. A stale depth only over-blocks the gate, never
   under-blocks it. */
static void pvm_seat_claim(BPLONG depth)
{
    long i;

    if (pvm_shm == NULL) return;
    if (pvm_my_seat < 0) {
        pvm_my_seat = pvm_resolve_my_seat();
        if (pvm_my_seat < 0 && !pvm_is_fork_child)
            for (i = 0; i < PVM_POOL_SEATS; i++)
                if (__sync_bool_compare_and_swap(
                        (long *)&pvm_shm->pvm_seat[i].pid, (long)0,
                        (long)getpid())) {
                    pvm_my_seat = (int)i;
                    break;
                }
    }
    if (pvm_my_seat >= 0) {
        pvm_shm->pvm_seat[pvm_my_seat].wait = 0;   /* active again */
        pvm_shm->pvm_seat[pvm_my_seat].depth = depth;
    }
}

/* Release my seat on the way out, returning my process slot to the
   pool IF I held it (an active process; an (a)-waiter gave its slot
   to the pool at deleg_wait entry and keeps it out while waiting).
   The CAS keeps the self-release / SIGCHLD-handler race exactly once
   (the handler does the same wait-flag check for children it reaps). */
static void pvm_seat_release(void)
{
    long me = (long)getpid();
    int active;

    if (pvm_my_seat < 0 || pvm_shm == NULL) return;
    active = (pvm_shm->pvm_seat[pvm_my_seat].wait == 0);
    pvm_shm->pvm_seat[pvm_my_seat].wait = 0;
    if (__sync_bool_compare_and_swap(
            (long *)&pvm_shm->pvm_seat[pvm_my_seat].pid, me, (long)0) &&
        active) {
        __sync_fetch_and_add(&pvm_shm->pool_free, 1);
        pvm_pool_wake();
    }
    pvm_my_seat = -1;
}

/* Deepest-frontier fork gate: no ACTIVE process may hold a frame
   shallower than ar (shallower = higher address; frames are pushed
   down). My own seat is updated to ar by the caller (pvm_pool_take)
   beforehand. (a)-waiters are NOT active frontiers -- their work
   continues in the successor chain, whose own seats (deeper) do the
   gating; counted here they would starve every fork below the wait
   frame (the q479 shape: a root waiting at Q4 blocks the Q5/Q6
   frontier it is waiting for). */
static int pvm_gate_ok(BPLONG_PTR ar)
{
    BPLONG a = (BPLONG)ar;
    long i;

    for (i = 0; i < PVM_POOL_SEATS; i++) {
        long sp = pvm_shm->pvm_seat[i].pid;
        BPLONG sd;

        if (sp == 0 || sp == (long)getpid() || pvm_shm->pvm_seat[i].wait)
            continue;
        sd = pvm_shm->pvm_seat[i].depth;
        if (sd < a) return 0;
    }
    return 1;
}

/* Consume one process slot for a child about to be forked at frame
   ar: the budget must be free and my frame must be the deepest
   active frontier. The caller refunds (pool_free++) if the ensuing
   fork or spawn then fails. */
static int pvm_pool_take(BPLONG_PTR ar)
{
    static int dbg = -1;
    static int notake = -1;
    static BPLONG block_ar = 0;

    if (dbg < 0) dbg = (getenv("PVM_DBG") != NULL);
    if (notake < 0) notake = (getenv("PVM_NOTAKE") != NULL);
    if (block_ar == 0 && getenv("PVM_BLOCK_AR") != NULL)
        block_ar = (BPLONG)strtoull(getenv("PVM_BLOCK_AR"), NULL, 16);
    if (block_ar != 0 && ((BPLONG)ar & 0xFFFFFULL) == (block_ar & 0xFFFFFULL))
        return 0;    /* bisection: never delegate this exact frame */
    if (notake)
        return 0;     /* bisection: window armed, slot logic live,
                        no COW child is ever forked */
    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3) || pvm_shm->found)
        return 0;
    if (pvm_shm->pool_free < 1) {
        if (dbg)
            fprintf(stderr, "PVT %d ar=%p free=%ld [budget]\n",
                    (int)getpid(), (void *)ar, pvm_shm->pool_free);
        return 0;
    }
    pvm_seat_claim((BPLONG)ar);
    if (!pvm_gate_ok(ar)) {
        long i;
        if (dbg)
            for (i = 0; i < PVM_POOL_SEATS; i++)
                if (pvm_shm->pvm_seat[i].pid &&
                    pvm_shm->pvm_seat[i].pid != (long)getpid() &&
                    !pvm_shm->pvm_seat[i].wait &&
                    pvm_shm->pvm_seat[i].depth < (BPLONG)ar)
                    fprintf(stderr, "PVT %d ar=%p [gate] blocked by pid=%ld depth=%p\n",
                            (int)getpid(), (void *)ar,
                            pvm_shm->pvm_seat[i].pid,
                            (void *)pvm_shm->pvm_seat[i].depth);
        return 0;
    }
    if (dbg)
        fprintf(stderr, "PVT %d ar=%p [take] free=%ld->%ld\n",
                (int)getpid(), (void *)ar, pvm_shm->pool_free,
                pvm_shm->pool_free - 1);
    return __sync_fetch_and_add(&pvm_shm->pool_free, -1) >= 0;
}

/* Completion/handoff registry (pvm_shm->pvm_done; fresh per session,
   so a recycled pid cannot collide with a stale entry). Two-phase
   publish: a slot is claimed by a CAS on pid (0 -> -pid, a sending
   marker no reader can match), the fields are written, and the
   positive pid then publishes (readers match on pid, then a full
   barrier pairs with the writer's publish and sees complete
   st/succ). Waiters invalidate their entry after consuming it
   (pvm_done_invalidate), so the number of occupied slots is bounded
   by the number of dead non-root workers (<= nt-1 < PVM_DONE_SLOTS):
   a free slot always exists and the claim race is lossless. */
static void pvm_done_write(long pid, long st, long succ)
{
    long i;

    if (pvm_shm == NULL) return;
    /* Claim with a SENDING pid (-pid, never a legal positive value)
       so a reader that matches on pid can only ever see a slot whose
       st/succ are complete: the fields are written first, then the
       positive pid publishes (release) and the reader's barrier
       (pvm_done_lookup) pairs with it. A writer that dies between
       claim and publish leaves a stale -pid slot; that writer's
       atexit CRASH entry (or the fallback overwrite below, which
       resets pid unconditionally) resolves it, and a crashed
       session is rejected anyway. */
    for (i = 0; i < PVM_DONE_SLOTS; i++)
        if (__sync_bool_compare_and_swap(
                (long *)&pvm_shm->pvm_done[i].pid, (long)0, -pid))
            break;
    if (i < PVM_DONE_SLOTS) {
        pvm_shm->pvm_done[i].succ = succ;
        pvm_shm->pvm_done[i].st = st;
        __sync_synchronize();
        pvm_shm->pvm_done[i].pid = pid;
        pvm_shm->pvm_done[i].tick =
            __sync_fetch_and_add(&pvm_shm->pvm_tick, 1);
        pvm_pool_wake();
        return;
    }
    /* Unreachable (see above): a burst of fully unconsumed entries.
       Best effort: overwrite the oldest slot; a concurrent writer
       may race here, and a lost race costs a waiter one 20 ms tick
       before it observes the entry -- but never a wrong one, since
       the fields are rewritten on every attempt and the tick only
       moves forward. */
    {
        long best = 0;

        for (i = 1; i < PVM_DONE_SLOTS; i++)
            if (pvm_shm->pvm_done[i].tick < pvm_shm->pvm_done[best].tick)
                best = i;
        pvm_shm->pvm_done[best].succ = succ;
        pvm_shm->pvm_done[best].st = st;
        pvm_shm->pvm_done[best].pid = pid;
        pvm_shm->pvm_done[best].tick =
            __sync_fetch_and_add(&pvm_shm->pvm_tick, 1);
        pvm_pool_wake();
    }
}

/* Free a consumed entry (the waiter has finished with it). */
static void pvm_done_invalidate(long pid)
{
    long i;

    if (pvm_shm == NULL) return;
    for (i = 0; i < PVM_DONE_SLOTS; i++)
        if (__sync_bool_compare_and_swap(
                (long *)&pvm_shm->pvm_done[i].pid, pid, (long)0))
            return;
}

static int pvm_done_lookup(long pid, long *st, long *succ)
{
    long i;

    if (pvm_shm == NULL) return 0;
    for (i = 0; i < PVM_DONE_SLOTS; i++)
        if (pvm_shm->pvm_done[i].pid == pid) {
            __sync_synchronize();  /* pairs with pvm_done_write's
                                      publish: st/succ are complete */
            *succ = pvm_shm->pvm_done[i].succ;
            *st = pvm_shm->pvm_done[i].st;
            return 1;
        }
    return 0;
}

/* Record my outcome, release my seat + process slot, drop the live
   counter, and go. _exit skips atexit. */
static void pvm_worker_exit(long st, long succ)
{
    pvm_dbg("WORKER-EXIT", NULL, st);
    if (pvm_shm != NULL) {
        pvm_done_write((long)getpid(), st, succ);
        pvm_seat_release();
        __sync_fetch_and_add(&pvm_shm->live, -1);
    }
    fflush(NULL);       /* _exit skips stdio flushing */
    _exit((int)st);
}

/* A worker that has seen the tree solved: quietly kill and reap my
   still-running children (they are worthless now), then exit 0 as
   any other healthy worker. */
static void pvm_worker_die_found(void)
{
    long i;

    pvm_dbg("DIE-FOUND", NULL, 0);
    if (pvm_shm != NULL) {
        pvm_reap_quiet = 1;
        for (i = 0; i < pvm_nchildren; i++)
            kill((pid_t)pvm_my_children[i], SIGKILL);
        pvm_reap_my_children();
        pvm_reap_quiet = 0;
    }
    pvm_worker_exit(0, 0);
}

/* atexit for a forked worker (crash / picat-error exit path; the
   healthy endings _exit and skip it): flag my outcome unless I
   already registered it, quietly kill and reap my children (the
   session is tainted; the orphans are worthless), and release my
   seat + slot. */
static void pvm_worker_exit_atexit(void)
{
    long i;
    long dst, dsucc;

    if (!pvm_is_fork_child) return;
    pvm_dbg("ATEXIT", NULL, 0);
    if (pvm_dbg_on() && pvm_shm != NULL) {
        /* crash-time forensics: breg + top of the live frame stack +
           protocol state (rootframe identity, pool, seat) */
        BPLONG_PTR fr = (BPLONG_PTR)AR_B(breg);
        int k;

        fprintf(stderr,
                "CRASH-DUMP pid=%d breg=%p htop=%p ttop=%p ltop=%p sf=%p sup=%p\n",
                (int)getpid(), (void *)breg, (void *)heap_top,
                (void *)trail_top, (void *)local_top, (void *)sfreg,
                (void *)stack_up_addr);
        fprintf(stderr,
                "  root=%p off=%ld onside=%d pool_free=%d live=%d seat=%d wait=%d bad=%d mybad=%d\n",
                (void *)pvm_my_rootframe, (long)pvm_rootframe_off,
                pvm_root_onside, (int)pvm_shm->pool_free,
                (int)pvm_shm->live, pvm_my_seat,
                (pvm_my_seat >= 0) ?
                    (int)pvm_shm->pvm_seat[pvm_my_seat].wait : -1,
                (int)pvm_shm->bad, pvm_child_bad);
        for (k = 0; k < 12 && fr != NULL; k++) {
            fprintf(stderr, "  F%d ar=%p P=%p T=%p\n", k, (void *)fr,
                    (void *)(BPLONG_PTR)AR_CPF(fr), (void *)AR_T(fr));
            fr = (BPLONG_PTR)AR_B(fr);
        }
    }
    if (pvm_shm != NULL && !pvm_done_lookup((long)getpid(), &dst, &dsucc))
        pvm_done_write((long)getpid(), PVM_ST_CRASH, 0);
    if (pvm_shm != NULL)
        pvm_reap_quiet = 1;
    for (i = 0; i < pvm_nchildren; i++)
        kill((pid_t)pvm_my_children[i], SIGKILL);
    pvm_reap_my_children();
    pvm_reap_quiet = 0;
    if (pvm_shm != NULL) {
        pvm_seat_release();
        __sync_fetch_and_add(&pvm_shm->live, -1);
    }
}

/* SIGCHLD (installed per session; inherited by workers): reap my
   dead children early (zombie hygiene), flag abnormal exits, and
   release the process slot of a child that could not self-release
   (SIGKILLed / died before its exit path ran) -- exactly once, by
   the seat CAS. The reaped child's outcome is in the shared done
   registry (its exit path wrote it); the (a) waiters read it there.
   Async-signal-safe: waitpid, atomics, raw futex syscall only. */
static void pvm_sigchld(int sig)
{
    int st;
    pid_t p;
    long i;

    (void)sig;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        for (i = 0; i < pvm_nchildren; i++) {
            if (pvm_my_children[i] == (long)p) {
                int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
                int abnormal = pvm_reap_quiet
                     ? (WIFEXITED(st) && code != 0)
                     : (!WIFEXITED(st) ||
                        (code != 0 && code != PVM_ST_TRANSFER));

                if (abnormal) {
                    pvm_child_bad = 1;
                    if (pvm_shm != NULL) pvm_shm->bad = 1;
                }
                pvm_my_children[i] = pvm_my_children[--pvm_nchildren];
                break;
            }
        }
        if (pvm_shm != NULL) {
            for (i = 0; i < PVM_POOL_SEATS; i++) {
                if (__sync_bool_compare_and_swap(
                        (long *)&pvm_shm->pvm_seat[i].pid, (long)p,
                        (long)0)) {
                    /* the seat's wait flag says whether this child
                       held its slot (active: return it) or had given
                       it to the pool while (a)-waiting */
                    if (pvm_shm->pvm_seat[i].wait == 0) {
                        __sync_fetch_and_add(&pvm_shm->pool_free, 1);
                        pvm_pool_wake();
                    }
                    break;
                }
            }
        }
    }
}

/* Sleep about 5 ms, waking early on a pool waker (shared futex
   word, bumped on slot release and done writes) or a signal (my
   SIGCHLD handler). The timeout is the backstop for a missed wake
   (a waker that fires between my load of the word and my wait start)
   -- kept short so a miss (more likely in -O0 builds, where this
   window is wider) costs little. The caller re-checks its condition
   on return. */
static void pvm_park_sleep(void)
{
    struct timespec ts;
    long v;

    v = pvm_shm->wake;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 5000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    syscall(SYS_futex, (void *)&pvm_shm->wake, FUTEX_WAIT, v, &ts, NULL, 0);
}

static int pvm_open_shm(void)
{
    int fd, tries;
    pid_t me = getpid();

    for (tries = 0; tries < 4; tries++) {
        snprintf(pvm_shm_name, sizeof(pvm_shm_name), "/pvm_%d_%d",
                 (int)me, (int)(time(NULL) & 0xffffffL) + tries);
        fd = shm_open(pvm_shm_name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) return BP_FALSE;
        if (ftruncate(fd, sizeof(pvm_shm_t)) != 0) { close(fd); return BP_FALSE; }
        pvm_shm = (pvm_shm_t *)mmap(NULL, sizeof(pvm_shm_t),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (pvm_shm == (pvm_shm_t *)MAP_FAILED) return BP_FALSE;
        pvm_shm->live = 0;
        pvm_shm->count = 0;
        pvm_shm->found = 0;
        pvm_shm->bad = 0;
        pvm_shm->sol_len = -1;
        pvm_shm->pool_free = 0;
        pvm_shm->wake = 0;
        pvm_shm->pvm_tick = 0;
        return BP_TRUE;
    }
    return BP_FALSE;
}

/* Reap this process's direct children (blocking; retry on EINTR). */
void pvm_reap_my_children(void)
{
    for (;;) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p > 0) {
            int i;
            for (i = 0; i < pvm_nchildren; i++)
                if (pvm_my_children[i] == p) {
                    /* quiet (post-found SIGKILL sweep): only a plain
                       non-zero EXIT is still a failure -- signalled
                       exits are ours; non-quiet: any abnormal exit
                       (a 77 transfer handoff is a healthy exit, as in
                       pvm_sigchld -- blocking reaps in collect can
                       race the sigchld handler and see it first). */
                    int abnormal = pvm_reap_quiet
                         ? (WIFEXITED(st) && WEXITSTATUS(st) != 0)
                         : (!WIFEXITED(st) ||
                            (WEXITSTATUS(st) != 0 &&
                             WEXITSTATUS(st) != PVM_ST_TRANSFER));
                    if (abnormal) {
                        pvm_child_bad = 1;
                        if (pvm_shm != NULL) pvm_shm->bad = 1;
                    }
                    pvm_my_children[i] = pvm_my_children[--pvm_nchildren];
                }
            if (pvm_nchildren == 0) return;
        } else if (p < 0) {
            if (errno == EINTR) continue;
            return;  /* ECHILD */
        }
    }
}

/* Per-frame delegation record (ring). A stale slot can only COST a
   missed delegation (or a redundant overlap fork), never break
   correctness.
   re   = original re-entry word (to restore in a child whose lineage
           is not the delegated one; the value walk's "next value")
   e1H/T/SF/TOP = the engine state at fork time (the frame's entry,
           value 1 already assigned); the owner's boundary restore
           and the re-run re-materialization go back to it
   from = first value index (1-based) this process owns in the
           disjunction's value sequence; the owner's chunk is
           [from, from+C-1], a tail worker's is [from, last]
    pid  = worker forked for this frame (meaningful in the delegator's
            copy only; 0 in everyone else's; 0 here = never forked or
            already reaped; in the pool era it also tracks the (a)
            wait's current successor)
    st   = saved exit status of a reaped worker (-1 = no result yet)
    mine = this process forked the worker for this frame (inherited
            entries are COW-reset to 0 in every child)
    root = this process was FORKED FOR this frame (its root delegation
            frame): at its boundary with a child it may hand off and
            exit (PVM_ST_TRANSFER)
    tail = this process is walking past its chunk without a child
            (the tail walker; retries the re-fork at every value
            boundary instead of parking) */
#define PVM_FRAME_LOG 1024
static BPLONG pvm_forked_ar[PVM_FRAME_LOG];
static BPLONG pvm_forked_re[PVM_FRAME_LOG];
static BPLONG pvm_forked_e1H[PVM_FRAME_LOG];
static BPLONG pvm_forked_e1T[PVM_FRAME_LOG];
static BPLONG pvm_forked_e1SF[PVM_FRAME_LOG];
static BPLONG pvm_forked_e1TOP[PVM_FRAME_LOG];
static BPLONG pvm_forked_pid[PVM_FRAME_LOG];
static int pvm_forked_st[PVM_FRAME_LOG];
static long pvm_forked_tried[PVM_FRAME_LOG];
static long pvm_forked_from[PVM_FRAME_LOG];
static char pvm_forked_mine[PVM_FRAME_LOG];
static char pvm_forked_root[PVM_FRAME_LOG];
static char pvm_forked_tail[PVM_FRAME_LOG];
/* pending: the first qualifying firing was recorded but NOT taken --
   delegation is deferred to the frame's second qualifying firing
   (same address, same re word), which proves the disjunction is a
   value loop (a COW worker re-dispatching the re-entry of a
   cascade/structural frame walks the wrong region; measured on
   ramsey K=3 N=5: workers over-walked above the model constraints
   into free E/D -> free_var_not_allowed in solve). */
static char pvm_forked_pending[PVM_FRAME_LOG];

/* Re-entry forensics (PVM_DBG): the first 8 hook firings and the
   first 8 labfail parks of a COW child, with the state deltas vs its
   root slot's entry record. */
static int pvm_rent_nfires = 99;
static int pvm_rent_nfails = 99;

/* Value-skip state (COW per process): this process is walking the
   values of frame pvm_skip_frame and must re-dispatch its re-entry
   pvm_skip_count more times before searching. */
long pvm_skip_count = 0;
int pvm_skip_armed = 0;
BPLONG pvm_skip_frame = 0;

/* E_1 copy-out for the toam.h macro (pvm_fork_frame return 2/3):
   the armed frame's fork-time entry state and re-entry word. */
BPLONG pvm_e1_H = 0, pvm_e1_T = 0, pvm_e1_SF = 0, pvm_e1_TOP = 0;
BPLONG pvm_e1_re = 0;

/* Original re-entry word of the frame this process was forked for:
   PVM_FORK_MAYBE dispatches the child there (its own frame cell is
   patched before the macro dispatches). */
BPLONG pvm_child_reentry = 0;

static int pvm_frame_slot(BPLONG_PTR ar)
{
    return (int)(((BPULONG)ar >> 4) % PVM_FRAME_LOG);
}

/* Linear probing over the frame->slot table: two live frames may hash
   to the same slot (frames 16KB apart in address), so every
   frame-to-record access must probe. lookup returns the recorded
   slot of ar or -1; alloc returns a free probed slot or -1 if full. */
static int pvm_slot_lookup(BPLONG_PTR ar)
{
    int s = pvm_frame_slot(ar), i;
    for (i = 0; i < PVM_FRAME_LOG; i++) {
        if (pvm_forked_ar[s] == (BPLONG)ar) return s;
        if (pvm_forked_ar[s] == (BPLONG)0) return -1;
        s = (s + 1) % PVM_FRAME_LOG;
    }
    return -1;
}

static int pvm_slot_alloc(BPLONG_PTR ar)
{
    int s = pvm_frame_slot(ar), i;
    for (i = 0; i < PVM_FRAME_LOG; i++) {
        if (pvm_forked_ar[s] == (BPLONG)0) return s;
        s = (s + 1) % PVM_FRAME_LOG;
    }
    return -1;
}

/* Seen-but-undelegated marker: a frame that has fired the hook is
   eligible for delegation only on its FIRST qualifying firing (frame
   creation, engine state fresh). Re-fired frames (value re-entries,
   loop iterations, structural-disjunction clause entries) walk the
   re-dispatch with a context that a COW'd worker cannot reconstruct
   (loop iterators, clause state live above the frame), so taking a
   slot on a re-fire would delegate "values" that are not values and
   park the owner mid-search (the q479 n10 R=0 corruption). Mark such
   frames so every later firing of the same address walks serially.
   Conservative by design: a recycled local-stack address that later
   hosts a fresh frame only COSTS a missed delegation. */
static void pvm_slot_tombstone(BPLONG_PTR ar)
{
    int s = pvm_slot_alloc(ar);

    if (s < 0) return;
    pvm_forked_ar[s] = (BPLONG)ar;
    pvm_forked_re[s] = 0;
    pvm_forked_e1H[s] = 0;
    pvm_forked_e1T[s] = 0;
    pvm_forked_e1SF[s] = 0;
    pvm_forked_e1TOP[s] = 0;
    pvm_forked_pid[s] = 0;
    pvm_forked_st[s] = -1;
    pvm_forked_tried[s] = 0;
    pvm_forked_from[s] = 0;
    pvm_forked_mine[s] = 0;
    pvm_forked_root[s] = 0;
    pvm_forked_tail[s] = 0;
    pvm_forked_pending[s] = 0;
}

/* Set by pvm_labfail_park: the boundary owner's frame re-entry
   (P = AR_CPF(AR) at the parking lab_fail). Kept for the park's
   diagnostic state; the deleg-fail stub no longer re-dispatches it
   (the solution is reported by value, no re-derivation). */
BPLONG pvm_rerun_site = 0;

/* Shared by pvm_fork_frame and pvm_fork_frame_tail: in a newly
   forked child, forget ownership of the inherited delegation
   records (slots other than the just-forked frame's). The engine
   frame cells themselves must NOT be touched here: an inherited
   slot may reference a frame that has since been popped (its local
   stack cells reused by live frames) and a write through it
   corrupts the live frame. Non-owner descendants handle a
   delegated frame's failure through the lab_pvm_deleg_fail stub
   (pvm_deleg_wait returns -1 -> pvm_deleg_reentry(B)), which reads
   the slot records only. */
static void pvm_child_inherit_reset(BPLONG_PTR ar)
{
    int k;

    for (k = 0; k < PVM_FRAME_LOG; k++) {
        BPLONG_PTR fr = (BPLONG_PTR)pvm_forked_ar[k];
        if (fr == (BPLONG_PTR)NULL || fr == ar) continue;
        pvm_forked_mine[k] = 0;
    }
}

/* Fork a value-chunk worker for an armed frame (slot s): the child
   takes values from + 0 .. from + C - 1 (and, as the chain's tail,
   walks on until it hands off or exhausts the disjunction). The
   caller must have secured the process slot (pvm_pool_take) and
   must be at the frame's entry state. Returns 1 in the new child
   (all worker state set; the caller must return rc 1 from the
   hook), 0 in the parent (pid[s] and the child's seat recorded),
   -1 on failure (the caller refunds the slot). */
static int pvm_spawn_chunk(BPLONG_PTR ar, int s, long from)
{
    BPLONG re;
    long C, skip;
    pid_t pid;
    long i;

    C = (pvm.mode == 1) ? 1 : pvm.aval;
    if (C < 1) C = 1;
    re = pvm_forked_re[s];
    if (re < 0x10000LL) {
        pvm_dbg("SPAWN-BADRE", ar, from);
        return -1;
    }
    if (*(BPLONG *)(re) < 0x10000LL) {
        pvm_dbg("SPAWN-BADRE2", ar, from);
        return -1;
    }
    /* Same invariant as the hook's state gate, minus the local stack
       (COW'd whole, re-entry re-initializes what it uses): the
       re-execute-from-`re` start state must match the frame entry for
       trail/heap/sf. */
    if (heap_top != (BPLONG_PTR)AR_H(ar) ||
        trail_top != (BPLONG_PTR)AR_T(ar) ||
        sfreg != (BPLONG_PTR)AR_SF(ar)) {
        pvm_dbg("SPAWN-BADSTATE", ar, from);
        return -1;
    }
    skip = from - 2;   /* the dispatch lands on value 2 */
    if (skip < 0) skip = 0;

    pid = fork();
    if (pid == (pid_t)-1) {
        pvm_dbg("SPAWN-FORKERR", ar, from);
        return -1;
    }
    pvm_dbg("SPAWN", ar, from);
    if (pid == 0) {
        if (!pvm_is_fork_child) {
            pvm_is_fork_child = 1;      /* the parent was the root */
            atexit(pvm_worker_exit_atexit);
        }
        pvm_nchildren = 0;
        __sync_fetch_and_add(&pvm_shm->live, 1);
        pvm_my_rootframe = (BPLONG)ar;
        pvm_rootframe_off =
            (long)((BPULONG)stack_up_addr - (BPULONG)ar);
        pvm_root_onside = 1;     /* the root frame is on the stack
                                     right after the fork */
        pvm_child_inherit_reset(ar);

        pvm_forked_ar[s] = (BPLONG)ar;
        pvm_forked_re[s] = re;
        pvm_forked_e1H[s] = (BPLONG)heap_top;
        pvm_forked_e1T[s] = (BPLONG)trail_top;
        pvm_forked_e1SF[s] = (BPLONG)sfreg;
        pvm_forked_e1TOP[s] = (BPLONG)local_top;
        pvm_forked_pid[s] = 0;
        pvm_forked_st[s] = -1;
        pvm_forked_mine[s] = 1;
        pvm_forked_root[s] = 1;
        pvm_forked_tail[s] = 1;
        pvm_forked_pending[s] = 0;
        pvm_forked_tried[s] = 0;
        pvm_forked_from[s] = from;
        pvm_rent_nfires = 0;
        pvm_rent_nfails = 0;
        AR_CPF(ar) = (BPLONG)&pvm_deleg_fail_word;
        pvm_skip_count = skip;
        pvm_skip_frame = (BPLONG)ar;
        pvm_skip_armed = (skip > 0);
        pvm_child_reentry = re;
        return 1;
    }

    /* parent: record the child */
    pvm_forked_pid[s] = (BPLONG)pid;
    pvm_forked_st[s] = -1;
    if (pvm_nchildren < PARVM_MAX_WORKERS)
        pvm_my_children[pvm_nchildren++] = (long)pid;
    for (i = 0; i < PVM_POOL_SEATS; i++) {
        if (__sync_bool_compare_and_swap(
                (long *)&pvm_shm->pvm_seat[i].pid, (long)0, (long)pid)) {
            pvm_shm->pvm_seat[i].depth = (BPLONG)ar;
            break;
        }
    }
    return 0;
}

/* TEMP diagnostic: one-shot per-process code/state dump at a named
   hook site. P = where the engine continues; re = the frame's
   re-entry word; heap diff vs e1H says whether any code (e.g. clause
   1's body) executed since the frame's entry. */
static void pvm_dump_once(const char *tag, BPLONG_PTR ar, int s,
                          BPLONG_PTR Pp)
{
    static int n = 0;
    BPLONG re = pvm_forked_re[s];
    BPLONG_PTR r = (BPLONG_PTR)re;

    if (!pvm_dbg_on()) return;

    if (n > 6) return;
    n++;
    fprintf(stderr, "%s pid=%d ar=%p P=%p P-1=%p re=%p\n", tag,
            (int)getpid(), (void *)ar, (void *)Pp, (void *)(Pp - 1),
            (void *)re);
    fprintf(stderr, "  *P-2=%lx *P-1=%lx *P=%lx *P+1=%lx *P+2=%lx\n",
            (unsigned long)*(Pp - 2), (unsigned long)*(Pp - 1),
            (unsigned long)*Pp, (unsigned long)*(Pp + 1),
            (unsigned long)*(Pp + 2));
    fprintf(stderr, "  *re=%lx *(re+1)=%lx *(re+2)=%lx AR_CPF=%lx\n",
            (unsigned long)*(BPLONG *)r, (unsigned long)*(BPLONG *)(r + 1),
            (unsigned long)*(BPLONG *)(r + 2),
            (unsigned long)AR_CPF(ar));
    fprintf(stderr,
            "  htop=%p e1H=%lx dH=%ld ttop=%p e1T=%lx sf=%p e1SF=%lx ltop=%p e1TOP=%lx\n",
            (void *)heap_top, (unsigned long)pvm_forked_e1H[s],
            (long)(heap_top - (BPLONG_PTR)pvm_forked_e1H[s]),
            (void *)trail_top, (unsigned long)pvm_forked_e1T[s],
            (void *)sfreg, (unsigned long)pvm_forked_e1SF[s],
            (void *)local_top, (unsigned long)pvm_forked_e1TOP[s]);
}

/* Called from the FORK / SET_FORK macros in toam.h, i.e. exactly
   when a choice point (re)records AR_CPF = re-entry word of the
   disjunction's remaining alternatives. Modes 1/3 only. Return:
     0 = keep searching this value (caller continues the dispatch)
     1 = in the forked worker: dispatch at pvm_child_reentry
     4 = boundary delegated: the caller must go to
         lab_pvm_deleg_fail via &pvm_deleg_fail_word (a worker with
         the root flag hands off and exits; the root / a deep owner
         (a) waits for the successor chain)
     3 = value skip: the caller must re-dispatch the re-entry
         (pvm_e1_re) without searching (advance one more value)
   Only value disjunctions (whose re-entry re-executes the FORK
   site) re-fire the hook; structural nondets see the hook once, at
   frame creation (M2 behavior is a C=1 special case of the same
   protocol). */
int pvm_fork_frame(BPLONG_PTR ar, BPLONG_PTR p)
{
    BPLONG re;
    long C;
    int s;
    int rc;
    {
        static int fl_on = -1, fl_n = 0;
        static int sd_on = -1, sd_n = 0;
        static int ch_on = -1, ch_n1 = 0, ch_n2 = 0;
        if (fl_on < 0) fl_on = (getenv("PVM_FL") != NULL);
        if (sd_on < 0) sd_on = (getenv("PVM_SD") != NULL);
        if (ch_on < 0) ch_on = (getenv("PVM_CHAIN") != NULL);
        if (ch_on && !pvm_is_fork_child) {
            static long ch_lo = -1, ch_hi = -1;
            char *cr = getenv("PVM_CHAIN_RANGE");
            if (cr && ch_lo < 0) {
                ch_lo = strtol(cr, NULL, 10);
                ch_hi = cr && strchr(cr, ',') ?
                    strtol(strchr(cr, ',') + 1, NULL, 10) : ch_lo;
            }
            ch_n1++;
            if (ch_n1 >= ch_lo && ch_n1 <= ch_hi) {
                BPLONG_PTR f = ar;
                int k;
                long aro = (long)((BPULONG)ar - (BPULONG)stack_up_addr);
                fprintf(stderr, "CHAIN pid=%d ev=%d ar=%lx\n",
                        (int)getpid(), fl_n, (unsigned long)aro);
                for (k = 0; k < 16 && f != NULL; k++) {
                    fprintf(stderr,
                            "  [%d] off=%lx T=%lx H=%lx CPF=%lx\n",
                            k,
                            (unsigned long)((BPULONG)f -
                                             (BPULONG)stack_up_addr),
                            (unsigned long)(BPULONG)AR_T(f),
                            (unsigned long)(BPULONG)AR_H(f),
                            (unsigned long)(BPULONG)AR_CPF(f));
                    f = (BPLONG_PTR)AR_B(f);
                }
                fflush(stderr);
            }
        }
        if (sd_on && sd_n < 20000) {
            sd_n++;
            fprintf(stderr,
                    "SD pid=%d B=%lx AR=%lx H=%lx T=%lx SF=%lx LT=%lx\n",
                    (int)getpid(),
                    (unsigned long)((BPULONG)breg - (BPULONG)stack_up_addr),
                    (unsigned long)((BPULONG)arreg - (BPULONG)stack_up_addr),
                    (unsigned long)(BPULONG)heap_top,
                    (unsigned long)(BPULONG)trail_top,
                    (unsigned long)(BPULONG)sfreg,
                    (unsigned long)((BPULONG)local_top - (BPULONG)stack_up_addr));
            fflush(stderr);
        }
        if (fl_on && fl_n < 20000) {
            fl_n++;
            fprintf(stderr, "FL pid=%d ar=%lx re=%lx\n", (int)getpid(),
                    (unsigned long)((BPULONG)ar & 0xFFFFFL),
                    (unsigned long)((BPULONG)AR_CPF(ar) & 0xFFFFFL));
            if (p != NULL)
                fprintf(stderr,
                        "   P p0=%lx p1=%lx p2=%lx p3=%lx\n",
                        (unsigned long)*(p),
                        (unsigned long)*(p + 1),
                        (unsigned long)*(p + 2),
                        (unsigned long)*(p + 3));
            fflush(stderr);
        }
    }

    (void)p;
    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3))
        return 0;
    C = (pvm.mode == 1) ? 1 : pvm.aval;
    if (C < 1) C = 1;

    if (pvm_shm->found) {
        if (pvm_is_fork_child)
            pvm_worker_die_found();      /* noreturn */
        return 0;             /* root: finish its own territory, then
                                  collect (early abort is a later item) */
    }

    /* A worker backtracked above its root delegation frame: its
       delegated disjunction (and everything of its context below it)
       is exhausted -> the region is done. Onside bookkeeping (see
       pvm_my_rootframe): this firing IS the root disjunction
       (re-)entering (value advance: it was 0 since the failed
       value's lab_fail, the re-fire re-arms it); otherwise, a firing
       while NOT onside is a frame pushed in the caller's context --
       the disjunction has failed to its caller. Frame ADDRESS
       comparison is unusable (non-monotone frame placement, GC);
       the offset from stack_up_addr is expansion-stable. If the
       worker left a live successor (a tail chain it spawned), the
       region is NOT done: hand off (77 -> successor) so the (a)
       waiter follows the full chain instead of resolving early on
       my 0. noreturn. */
    if (pvm_is_fork_child && pvm_my_rootframe != 0) {
        if ((BPLONG)ar ==
            (BPLONG)((BPULONG)stack_up_addr - (BPULONG)pvm_rootframe_off))
            pvm_root_onside = 1;
        else if (!pvm_root_onside) {
        int rs;
        long succ = 0;

        rs = pvm_slot_lookup((BPLONG_PTR)pvm_my_rootframe);
        if (rs >= 0 && pvm_forked_mine[rs] && pvm_forked_pid[rs] > 0)
            succ = pvm_forked_pid[rs];
        pvm_dbg("SCOPE-EXIT", ar, (long)pvm_my_rootframe);
        if (succ)
            pvm_worker_exit(PVM_ST_TRANSFER, succ);
        else
            pvm_worker_exit(0, 0);
        }
    }

    if (pvm_is_fork_child && pvm_rent_nfires < 8) {
        int s0 = (pvm_my_rootframe != 0) ?
            pvm_slot_lookup((BPLONG_PTR)pvm_my_rootframe) : -1;

        pvm_rent_nfires++;
        if (pvm_dbg_on())
            fprintf(stderr,
                    "RENT pid=%d ar=%p root=%s dH=%ld dT=%ld dSF=%ld\n",
                    (int)getpid(), (void *)ar,
                    ((BPLONG)ar ==
                     (BPLONG)((BPULONG)stack_up_addr -
                               (BPULONG)pvm_rootframe_off)) ? "y" : "n",
                    (s0 >= 0) ?
                        (long)(heap_top -
                               (BPLONG_PTR)pvm_forked_e1H[s0]) : -99,
                    (s0 >= 0) ?
                        (long)(trail_top -
                               (BPLONG_PTR)pvm_forked_e1T[s0]) : -99,
                    (s0 >= 0) ?
                        (long)(sfreg -
                               (BPLONG_PTR)pvm_forked_e1SF[s0]) : -99);
    }

    re = AR_CPF(ar);
    if (re < 0x10000LL) {
        pvm_dbg("SKIP-NORE", ar, re);
        return 0;                    /* no re-entry word (self-guarding:
                                         the check fails on every firing) */
    }
    if (*(BPLONG *)(re) < 0x10000LL) {
        pvm_dbg("SKIP-NORE2", ar, *(BPLONG *)(re));
        return 0;                    /* not a jmp entry (self-guarding) */
    }

    if (PVM_FORCE_MATCH(ar) &&
        !pvm_is_fork_child) {
        /* debug force: arm + delegate this frame on its first firing
           after the re-words check, deliberately BEFORE the state
           gate (the experiment IS about whether the gate's premise
           holds for this frame shape). */
        int s2 = pvm_slot_lookup(ar);

        if (s2 >= 0 && pvm_forked_mine[s2])
            return 0;    /* already armed at an earlier firing */
        pvm_dbg("FORCE", ar, (long)s2);
        fprintf(stderr,
                "FDUMP pid=%d ar=%p P=%p re=%p\n"
                "  htop=%p AR_H=%p dH=%ld ttop=%p AR_T=%p dT=%ld\n"
                "  sf=%p AR_SF=%p ltop=%p AR_TOP=%p\n"
                "  *P-1=%lx *P=%lx *re=%lx *(re+1)=%lx\n",
                (int)getpid(), (void *)ar, (void *)p, (void *)re,
                (void *)heap_top, (void *)AR_H(ar),
                (long)(heap_top - (BPLONG_PTR)AR_H(ar)),
                (void *)trail_top, (void *)AR_T(ar),
                (long)(trail_top - (BPLONG_PTR)AR_T(ar)),
                (void *)sfreg, (void *)AR_SF(ar),
                (void *)local_top, (void *)AR_TOP(ar),
                (unsigned long)*(BPLONG_PTR)(p - 1), (unsigned long)*p,
                (unsigned long)*(BPLONG_PTR)re,
                (unsigned long)*(BPLONG_PTR)(re + 1));
        if (!pvm_pool_take(ar)) {
            pvm_dbg("FORCE-NOPICK", ar, 0);
            return 0;
        }
        if (s2 < 0)
            s2 = pvm_slot_alloc(ar);
        if (s2 < 0) {
            __sync_fetch_and_add(&pvm_shm->pool_free, 1);
            pvm_dbg("FORCE-NOSLOT", ar, 0);
            return 0;
        }
        pvm_forked_ar[s2] = (BPLONG)ar;
        pvm_forked_re[s2] = re;
        pvm_forked_e1H[s2] = (BPLONG)heap_top;
        pvm_forked_e1T[s2] = (BPLONG)trail_top;
        pvm_forked_e1SF[s2] = (BPLONG)sfreg;
        pvm_forked_e1TOP[s2] = (BPLONG)local_top;
        pvm_forked_st[s2] = -1;
        pvm_forked_mine[s2] = 1;
        rc = pvm_spawn_chunk(ar, s2, C + 1);
        if (rc == 1) {
            pvm_dbg("FORCE-CHILD", ar, 0);
            return 1;
        }
        if (rc < 0) {
            __sync_fetch_and_add(&pvm_shm->pool_free, 1);
            pvm_dbg("FORCE-SPAWNFAIL", ar, 0);
            return 0;
        }
        pvm_forked_root[s2] = 0;
        pvm_forked_tail[s2] = 0;
        pvm_forked_tried[s2] = 1;   /* the owner walks this value */
        pvm_forked_from[s2] = 1;
        pvm_dbg("FORCE-PARENT", ar, (long)pvm_forked_pid[s2]);
        return 0;
    }

    /* Delegate only when the engine state that a COW'd worker would
       re-execute is exactly the frame's recorded entry state for the
       TRAIL, HEAP and SEARCH-FAIL registers: a trail/heap divergence
       means the re-executed re-entry would double-record events that
       the COW already carries. The LOCAL STACK is deliberately
       excluded from the check: the PICAT compiler always allocates
       call locals between the frame push and the FORK (and re-pushed
       / re-entered frames record their TOP before more code runs),
       so a local check rejects every frame in this engine (measured:
       all 638 FORK firings of queens n10, dH=dT=dSF=0, dTOP!=0).
       Locals are COW'd whole and the re-entry re-initializes what it
       uses, which the forced runs validate end-to-end (worker chunk
       walks, found-by-value, sound UNSAT exhaustion). */
    if (heap_top != (BPLONG_PTR)AR_H(ar) ||
        trail_top != (BPLONG_PTR)AR_T(ar) ||
        sfreg != (BPLONG_PTR)AR_SF(ar)) {
        pvm_dbg("SKIP-STATE", ar,
                (long)(heap_top != (BPLONG_PTR)AR_H(ar)));
        if (getenv("PVM_DBG") != NULL)
            fprintf(stderr, "SDUMP pid=%d ar=%p dH=%ld dT=%ld dSF=%ld dTOP=%ld\n",
                    (int)getpid(), (void *)ar,
                    (long)(heap_top - (BPLONG_PTR)AR_H(ar)),
                    (long)(trail_top - (BPLONG_PTR)AR_T(ar)),
                    (long)(sfreg - (BPLONG_PTR)AR_SF(ar)),
                    (long)(local_top - (BPLONG_PTR)AR_TOP(ar)));
        pvm_slot_tombstone(ar);  /* a re-fire, not a fresh frame: never
                                     delegate this frame (it re-fires with
                                     loop/clause context a COW'd worker
                                     cannot reconstruct) */
        return 0;
    }

    s = pvm_slot_lookup(ar);
    if (s >= 0) {
        /* armed frame: this process owns this disjunction's value
           walk (as owner, tail worker, re-running a re-run, or the
           frame is tombstoned (seen, undelegated: walk serially) */
        if (!pvm_forked_mine[s]) {
            pvm_dbg("SKIP-SEEN", ar, 0);
            return 0;      /* tombstone: first qualifying firing passed */
        }
        if (pvm_forked_pending[s]) {
            /* second qualifying firing of a recorded frame. Same
               re word: the disjunction re-entered itself -- a proven
               value loop -- delegate now (identical COW state and
               re-entry as a first-firing take: re recorded at the
               FIRST firing, state = the frame entry). Different re
               word: the address was recycled by another disjunction
               -- refresh the record and keep walking. */
            if (re != pvm_forked_re[s]) {
                pvm_dbg("PEND-RESET", ar, 0);
                pvm_forked_re[s] = re;
                pvm_forked_e1H[s] = (BPLONG)heap_top;
                pvm_forked_e1T[s] = (BPLONG)trail_top;
                pvm_forked_e1SF[s] = (BPLONG)sfreg;
                pvm_forked_e1TOP[s] = (BPLONG)local_top;
                pvm_forked_tried[s] = 0;
                pvm_forked_from[s] = 1;
                return 0;
            }
            pvm_dbg("CONFIRM", ar, 0);
            if (!pvm_pool_take(ar))
                return 0;    /* stays pending: retry at a later firing */
            rc = pvm_spawn_chunk(ar, s, C + 1);
            if (rc == 1) {
                pvm_dbg("CONF-CHILD", ar, 0);
                return 1;
            }
            if (rc < 0) {
                __sync_fetch_and_add(&pvm_shm->pool_free, 1);
                return 0;    /* stays pending: retry at a later firing */
            }
            pvm_forked_pending[s] = 0;
            pvm_forked_root[s] = 0;
            pvm_forked_tail[s] = 0;
            pvm_forked_tried[s] = 1;   /* value 1 was walked serially */
            pvm_forked_from[s] = C + 1;
            pvm_dbg("CONF-PARENT", ar, (long)pvm_forked_pid[s]);
            if (C < 2)
                return 4;    /* owner's share (value 1) is done: park */
            return 0;        /* owner still owes values 2..C */
        }
        if (pvm_skip_armed && pvm_skip_frame == (BPLONG)ar &&
            pvm_skip_count > 0) {
            pvm_skip_count--;
            if (pvm_skip_count <= 0) {
                pvm_skip_count = 0;
                pvm_skip_armed = 0;
            }
            pvm_e1_re = pvm_forked_re[s];
            pvm_rerun_site = 0;
            return 3;
        }
        if (pvm_forked_tried[s] < C) {
            pvm_forked_tried[s]++;
            pvm_dbg("WALK", ar, pvm_forked_tried[s]);
            pvm_dump_once("WALK-DUMP", ar, s, p);
            return 0;   /* search this value ourselves */
        }
        /* tried >= C: boundary */
        if (pvm_forked_pid[s] > 0) {
            pvm_dbg("BOUND-RC4", ar, (long)pvm_forked_pid[s]);
            pvm_dump_once("BOUND-DUMP", ar, s, p);
            return 4;   /* the rest is delegated: the deleg-fail stub
                            decides (worker-with-root handoff, else
                            (a) wait) */
        }
        if (pvm_forked_tail[s]) {
            /* tail walker at a value boundary: fork the next chunk if
               a slot is free right now, else walk the next C values
               (and retry at the next boundary) */
            if (pvm_pool_take(ar)) {
                rc = pvm_spawn_chunk(ar, s, pvm_forked_from[s] + C);
                if (rc == 1) return 1;    /* I am the new child */
                if (rc >= 0) {
                    pvm_dbg("TAIL-RC4", ar, (long)pvm_forked_pid[s]);
                    return 4;    /* it covers the rest: park */
                }
                __sync_fetch_and_add(&pvm_shm->pool_free, 1);
            }
            pvm_dbg("TAIL-WALK", ar, pvm_forked_from[s]);
            pvm_forked_from[s] += C;
            pvm_forked_tried[s] = 0;
            return 0;
        }
        /* fresh boundary (no child yet): fork the next chunk if a
           slot is free right now, else become the tail walker */
        if (pvm_pool_take(ar)) {
            rc = pvm_spawn_chunk(ar, s, pvm_forked_from[s] + C);
            if (rc == 1) return 1;
            if (rc >= 0) {
                pvm_dbg("BOUND-RC4", ar, (long)pvm_forked_pid[s]);
                return 4;
            }
            __sync_fetch_and_add(&pvm_shm->pool_free, 1);
        }
        pvm_dbg("TAIL-MARK", ar, pvm_forked_from[s]);
        pvm_forked_tail[s] = 1;
        pvm_forked_from[s] += C;
        pvm_forked_tried[s] = 0;
        return 0;
    }

    if (pvm_delegate_depth <= 0) {
        pvm_dbg("SKIP-NOFLAG", ar, pvm_delegate_depth);
        return 0;   /* outside the bp.pvm_delegate window (loop setup
                       frames, the (model; true) wrapper, ...): walk
                       serially. No tombstone: the frame is otherwise
                       eligible if a windowed firing ever reaches it
                       (recycled address). */
    }

    /* unrecorded: first qualifying firing of a fresh frame site.
       Record it PENDING and walk value 1 serially; the take is
       deferred to the SECOND qualifying firing (same address, same
       re word -- the disjunction re-entered itself, i.e. a proven
       value loop; a COW worker re-dispatching the re-entry of a
       cascade/structural frame walks the wrong region, measured on
       ramsey K=3 N=5: over-walk above the model constraints into
       free E/D, free_var_not_allowed in solve). No pool seat is
       consumed until the confirmation take. pvm_spawn_chunk reads
       pvm_forked_re[s] (the child dispatches and records from it),
       so the record must exist before any confirm-time spawn. */
    s = pvm_slot_alloc(ar);
    if (s < 0) {
        pvm_dbg("PEND-NOSLOT", ar, 0);
        return 0;   /* slot table full: walk serially (no tombstone:
                       the frame may still confirm later via a
                       freed slot? no -- without a slot it can never
                       be recorded: this site walks serially forever;
                       a recycled address re-records on a free slot) */
    }
    pvm_forked_ar[s] = (BPLONG)ar;
    pvm_forked_re[s] = re;
    pvm_forked_e1H[s] = (BPLONG)heap_top;
    pvm_forked_e1T[s] = (BPLONG)trail_top;
    pvm_forked_e1SF[s] = (BPLONG)sfreg;
    pvm_forked_e1TOP[s] = (BPLONG)local_top;
    pvm_forked_st[s] = -1;
    pvm_forked_pid[s] = 0;
    pvm_forked_tried[s] = 0;
    pvm_forked_from[s] = 1;
    pvm_forked_mine[s] = 1;
    pvm_forked_root[s] = 0;
    pvm_forked_tail[s] = 0;
    pvm_forked_pending[s] = 1;
    pvm_dbg("PEND", ar, 0);
    return 0;
}

/* Called from the toam.h macro immediately after a forked worker is
   dispatched (t = 0: the worker is still at its fork-time entry,
   state = the frame's entry, nothing executed since the fork).
   Starts the chunk chain under the pool gate: if a process slot is
   free AND this frame is the deepest active frontier, fork a
   grandchild that owns the next C values (from + C), and this
   worker becomes the boundary owner of its own chunk. (At t = 0 the
   forking worker is usually the SHALLOW end of the pool -- the
   parent has already descended -- so the gate blocks most of these
   and the slots stay for the deep frontier; that is intended.)
   Return 1 only in the grandchild. */
int pvm_fork_frame_tail(BPLONG_PTR ar, BPLONG_PTR p)
{
    int s;
    int rc;

    (void)p;
    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3))
        return 0;
    if (pvm_shm->found) return 0;
    s = pvm_slot_lookup(ar);
    if (s < 0 || !pvm_forked_mine[s] || pvm_forked_pid[s] != 0)
        return 0;
    if (!pvm_pool_take(ar)) return 0;
    rc = pvm_spawn_chunk(ar, s, pvm_forked_from[s] +
                         ((pvm.mode == 1) ? 1 : pvm.aval));
    if (rc == 1) return 1;
    if (rc < 0) {
        __sync_fetch_and_add(&pvm_shm->pool_free, 1);
        return 0;
    }
    pvm_forked_tail[s] = 0;   /* I am the boundary owner of my chunk */
    return 0;
}

/* Called from lab_fail just after P = AR_CPF(AR), i.e. right before
   the failed value's disjunction re-dispatches its re-entry (which
   advances to the next value). Parks the boundary owner HERE, one
   step before the boundary value is advanced into, so the engine
   state is exactly the frame's entry after a standard backtrack and
   the deleg-fail stub can fail the disjunction to the caller without
   touching any engine state. Returns 1 when the caller must park
   (P -> deleg-fail word, CONTCASE); 0 otherwise. A worker that sees
   the global solution drops out here (earlier than the next fork hook). */
int pvm_labfail_park(BPLONG_PTR ar, BPLONG_PTR p)
{
    long C;
    int s;
    {
        static int lf_on = -1, lf_n = 0;
        if (lf_on < 0) lf_on = (getenv("PVM_FL") != NULL);
        if (lf_on && lf_n < 20000) {
            lf_n++;
            fprintf(stderr, "LF pid=%d ar=%lx\n", (int)getpid(),
                    (unsigned long)((BPULONG)ar & 0xFFFFFL));
            fflush(stderr);
        }
    }

    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3))
        return 0;
    if (pvm_is_fork_child && pvm_my_rootframe != 0 &&
        (BPLONG)ar ==
        (BPLONG)((BPULONG)stack_up_addr - (BPULONG)pvm_rootframe_off))
        pvm_root_onside = 0;
        /* a root-frame value just failed: the disjunction either
           re-fires below (the hook re-sets onside) or fails to its
           caller (the next firing above it takes scope-exit) */
    if (pvm_is_fork_child && pvm_rent_nfails < 8) {
        pvm_rent_nfails++;
        if (pvm_dbg_on())
            fprintf(stderr, "RENT-FAIL pid=%d ar=%p root=%s\n",
                    (int)getpid(), (void *)ar,
                    (pvm_my_rootframe != 0 && (BPLONG)ar ==
                     (BPLONG)((BPULONG)stack_up_addr -
                               (BPULONG)pvm_rootframe_off)) ? "y" : "n");
    }
    if (pvm_shm->found) {
        if (pvm_is_fork_child)
            pvm_worker_die_found();   /* noreturn */
        return 0;             /* root: finish its own territory */
    }
    C = (pvm.mode == 1) ? 1 : pvm.aval;
    if (C < 1) C = 1;
    s = pvm_slot_lookup(ar);
    if (s >= 0 && pvm_forked_mine[s] &&
        pvm_forked_tried[s] >= C &&
        pvm_forked_pid[s] > 0) {
        /* boundary owner with a live delegation: park in the
           deleg-fail stub (it 77-hands-off for root-flagged
           workers, (a) waits otherwise). */
        pvm_dbg("PARK", ar, (long)pvm_forked_pid[s]);
        pvm_rerun_site = (BPLONG)p;
        return 1;
    }
    return 0;
}

/* Clear a frame's delegation record (called by lab_pvm_deleg_fail
   after the outcome is consumed): a re-tried disjunction can fork
   afresh at its next fork site. */
void pvm_scope_lost(BPLONG_PTR b)
{
    /* Called from the deleg-fail stub when it consumes a parked
       boundary and pops that frame's block: if the popped frame is
       THIS process's root delegation frame, the region is over (the
       next firing above it takes scope-exit). Deeper parked frames
       leave onside untouched. */
    if (pvm_is_fork_child && pvm_my_rootframe != 0 &&
        (BPLONG)b ==
        (BPLONG)((BPULONG)stack_up_addr - (BPULONG)pvm_rootframe_off))
        pvm_root_onside = 0;
}

void pvm_slot_rearm(BPLONG_PTR ar)
{
    int s = pvm_slot_lookup(ar);

    if (s < 0) return;
    pvm_forked_ar[s] = 0;
    pvm_forked_re[s] = 0;
    pvm_forked_e1H[s] = 0;
    pvm_forked_e1T[s] = 0;
    pvm_forked_e1SF[s] = 0;
    pvm_forked_e1TOP[s] = 0;
    pvm_forked_pid[s] = 0;
    pvm_forked_st[s] = -1;
    pvm_forked_tried[s] = 0;
    pvm_forked_from[s] = 0;
    pvm_forked_mine[s] = 0;
    pvm_forked_root[s] = 0;
    pvm_forked_tail[s] = 0;
}

/* Called from lab_pvm_deleg_fail (failure of a delegated
   disjunction). Pool era: three outcomes.
   1. A WORKER at its root delegation frame, chunk exhausted, with a
      child covering the rest: it HANDS OFF -- records
      (PVM_ST_TRANSFER, successor pid) in the done registry and
      exits (noreturn), releasing its process slot to the pool.
   2. An (a) WAIT: the root (or a worker parked on a deep delegation)
      waits on the successor chain in the shared done registry --
      the successor is often a grandchild, invisible to waitpid.
      Parked in ~20 ms futex ticks, woken by the shared wake word
      (slot releases, done writes) or by my SIGCHLD handler.
   3. Nobody delegated (defensive): -1 -> the stub continues the
      walk at the next value.
   Returns 0 (the delegated region is exhausted -- or holds the
   found solution, already reported by value), or a nonzero status
   (crash; the bad flag is set). */
/* Last pvm_deleg_wait() result, readable from the toam stub (which
   lives in another translation unit and cannot take a C return into a
   computed-goto branch cleanly). -1 = not the delegator. */
int pvm_last_deleg_status = -1;

/* TEMPORARY DEBUG (PVM_SEGV=1): SIGSEGV crash hook. Dumps the engine
   TLS state, the choicepoint frame chain and the code stream at the
   fault, then re-raises. Offline core-dump substitute for forked
   processes (no root for core_pattern here). */
#include <ucontext.h>
#include <signal.h>
static void pvm_segv_dump(int sig, siginfo_t *si, void *uap)
{
    char fname[80];
    snprintf(fname, sizeof(fname), "/tmp/wpqi/segvdump_%d.log",
             (int)getpid());
    {
        FILE *fp = fopen(fname, "w");
        ucontext_t *uc = (ucontext_t *)uap;
        BPLONG_PTR f;
        int k;
        if (fp) {
            fprintf(fp,
                    "SEGV pid=%d si_addr=%p rip=%llx rsp=%llx rbp=%llx "
                    "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx "
                    "rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx "
                    "r12=%llx r13=%llx r14=%llx r15=%llx\n",
                    (int)getpid(), (void *)si->si_addr,
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RBP],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R8],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R9],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R10],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R11],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R12],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R13],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R15]);
            fprintf(fp,
                    "ENG B=%p AR=%p H=%p T=%p SF=%p LG=%p HB=%p "
                    "STU=%p\n",
                    (void *)breg, (void *)arreg,
                    (void *)heap_top, (void *)trail_top, (void *)sfreg,
                    (void *)local_top, (void *)hbreg, (void *)stack_up_addr);
            fprintf(fp, "PVM child=%d armed=%d lastst=%d mode=%d aval=%d\n",
                    (int)pvm_is_fork_child, pvm.armed,
                    pvm_last_deleg_status, pvm.mode, (int)pvm.aval);
            {
                FILE *mf = fopen("/proc/self/maps", "r");
                char ml[512];
                unsigned long long targets[3];
                int t;
                targets[0] =
                     (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
                targets[1] = (unsigned long long)breg;
                targets[2] =
                     (unsigned long long)(*(volatile BPLONG *)
                                          (uc->uc_mcontext
                                           .gregs[REG_RBP] - 0xa28));
                if (mf) {
                    for (t = 0; t < 3; t++)
                        fprintf(fp, "TGT%d=%p\n", t, (void *)targets[t]);
                    while (fgets(ml, sizeof(ml), mf)) {
                        unsigned long long lo, hi;
                        if (sscanf(ml, "%llx-%llx", &lo, &hi) == 2)
                            for (t = 0; t < 3; t++)
                                if (targets[t] >= lo &&
                                    targets[t] < hi) {
                                    fputs(ml, fp);
                                    fprintf(fp, "  <- TGT%d\n", t);
                                }
                    }
                    fclose(mf);
                }
            }
            /* -O0 toam() parameter spills (live P/AR/LOCAL_TOP when
               the fault frame is toam's) */
            if ((unsigned long long)uc->uc_mcontext.gregs[REG_RBP] > 0x10000) {
                unsigned long long rbp =
                     (unsigned long long)uc->uc_mcontext.gregs[REG_RBP];
                fprintf(fp, "TOAM P=%p AR=%p LT=%p (%p)\n",
                        (void *)(*(volatile BPLONG *)(rbp - 0xa28)),
                        (void *)(*(volatile BPLONG *)(rbp - 0xa30)),
                        (void *)(*(volatile BPLONG *)(rbp - 0xa38)),
                        (void *)rbp);
            }
            f = (BPLONG_PTR)breg;
            for (k = 0; k < 12 && f != NULL; k++) {
                fprintf(fp,
                        " F[%d]=%p CPS=%p TOP=%p BTM=%p B=%p CPF=%p "
                        "H=%p T=%p SF=%p\n",
                        k, (void *)f, (void *)AR_CPS(f),
                        (void *)AR_TOP(f), (void *)AR_BTM(f),
                        (void *)AR_B(f), (void *)AR_CPF(f),
                        (void *)AR_H(f), (void *)AR_T(f),
                        (void *)AR_SF(f));
                f = (BPLONG_PTR)AR_B(f);
            }
            /* re-entry code streams: raw words of the re cells of the
               B frame and the live-AR frame; w0 = first re-entry
               label, w1.. = frame-relative operands. */
            if ((unsigned long long)uc->uc_mcontext.gregs[REG_RBP] >
                0x10000) {
                unsigned long long rbp =
                     (unsigned long long)uc->uc_mcontext.gregs[REG_RBP];
                BPLONG_PTR ar_sp =
                     (BPLONG_PTR)(*(volatile BPLONG *)(rbp - 0xa30));
                BPLONG_PTR frs[2];
                int n = 0;
                frs[n++] = (BPLONG_PTR)breg;
                if (ar_sp != frs[0])
                    frs[n++] = ar_sp;
                for (k = 0; k < n; k++) {
                    BPLONG_PTR fr = frs[k];
                    BPLONG_PTR re;
                    if (fr == (BPLONG_PTR)NULL)
                        continue;
                    re = (BPLONG_PTR)AR_CPF(fr);
                    fprintf(fp,
                            "RECELL fr=%p re=%p w0=%llx w1=%llx "
                            "w2=%llx w3=%llx w4=%llx w5=%llx\n",
                            (void *)fr, (void *)re,
                            (long long)(*(volatile BPLONG *)(re + 0)),
                            (long long)(*(volatile BPLONG *)(re + 8)),
                            (long long)(*(volatile BPLONG *)(re + 16)),
                            (long long)(*(volatile BPLONG *)(re + 24)),
                            (long long)(*(volatile BPLONG *)(re + 32)),
                            (long long)(*(volatile BPLONG *)(re + 40)));
                }
            }
            fclose(fp);
        }
    }
    signal(sig, SIG_DFL);
    {
        /* probe the fault page (below si_addr, same page: safe if the
           object is real and only the offset is wrong) */
        if (si->si_addr && ((BPULONG)si->si_addr & 7) == 0 &&
            (BPULONG)si->si_addr > 4096) {
            long off = (long)((BPULONG)si->si_addr & 4095L);
            volatile BPLONG *a = (BPLONG *)si->si_addr;
            int k, k0 = (int)(off % 8) == 0 ? -1 : (int)(-((off / 8) % 8));
            for (k = k0; k < 0; k--)
                fprintf(stderr, " M[%+d]=0x%lx\n", k,
                        (unsigned long)a[k]);
        }
    }
    raise(sig);
}

int pvm_deleg_wait(BPLONG_PTR f)
{
    int s = pvm_slot_lookup(f);
    long C = (pvm.mode == 1) ? 1 : pvm.aval;
    long p, dst, dsucc;

    if (C < 1) C = 1;
    if (s < 0 || !pvm_forked_mine[s]) {
        pvm_last_deleg_status = -1;
        return -1;
    }
    if (pvm_shm->found && pvm_is_fork_child)
        pvm_worker_die_found();       /* noreturn */

    /* (1) root-flagged worker, chunk done, child covers the rest */
    if (pvm_is_fork_child && pvm_forked_root[s] &&
        pvm_forked_tried[s] >= C && pvm_forked_pid[s] > 0) {
        pvm_dbg("H77", f, (long)pvm_forked_pid[s]);
        pvm_worker_exit(PVM_ST_TRANSFER, (long)pvm_forked_pid[s]);
    }

    if (pvm_forked_pid[s] <= 0) {
        /* no live delegation: tail walker continues at the next
           value (or return an already-recorded outcome) */
        pvm_last_deleg_status = (pvm_forked_st[s] != -1)
             ? (int)pvm_forked_st[s] : -1;
        return pvm_last_deleg_status;
    }

    /* (2) (a) wait on the successor chain. My frontier seat moves to
       the wait frame and goes INACTIVE (gate-skipped): while I wait,
       I am not a frontier -- my work continues in the successor
       chain. My process slot goes to the pool too (released here,
       taken back again on the way out): (a)-waiters are exactly the
       processes that do NOT need a fork slot, and keeping them would
       starve the frontier of every budget. While I wait, every other
       process still holds at most its slot, so the pool can never
       drop below one free slot and the take-back below cannot
       underflow. Cleared again on the way out (the seat then
       over-blocks only, as a stale depth must). */
    pvm_seat_claim((BPLONG)f);
    if (pvm_my_seat >= 0)
        pvm_shm->pvm_seat[pvm_my_seat].wait = 1;
    __sync_fetch_and_add(&pvm_shm->pool_free, 1);
    pvm_dbg("A-WAIT", f, (long)p);
    p = (long)pvm_forked_pid[s];
    for (;;) {
        if (pvm_done_lookup(p, &dst, &dsucc)) {
            long consumed = p;

            __sync_fetch_and_add(&pvm_shm->pool_free, -1);  /* my slot back */
            if (pvm_my_seat >= 0)
                pvm_shm->pvm_seat[pvm_my_seat].wait = 0;
            if (dst == 0) {
                pvm_done_invalidate(consumed);
                pvm_forked_pid[s] = 0;
                pvm_forked_st[s] = 0;
                pvm_last_deleg_status = 0;
                return 0;
            }
            if (dst == PVM_ST_TRANSFER) {
                /* keep waiting on the successor; my slot stays OUT
                   of the pool for the whole chain (released once at
                   entry, taken back once at exit -- any +1 here
                   leaks the pool, which was the free<0 underflow) */
                pvm_done_invalidate(consumed);
                p = dsucc;
                pvm_forked_pid[s] = (BPLONG)p;
                if (pvm_my_seat >= 0)
                    pvm_shm->pvm_seat[pvm_my_seat].wait = 1;
                continue;
            }
            /* the successor crashed (its atexit recorded it) */
            pvm_done_invalidate(consumed);
            pvm_child_bad = 1;
            pvm_shm->bad = 1;
            pvm_forked_pid[s] = 0;
            pvm_forked_st[s] = 1;
            pvm_last_deleg_status = 1;
            return 1;
        }
        if (pvm_shm->bad) {
            /* a crash was flagged elsewhere (e.g. an orphan no
               parent handler could flag): give up; the root's
               collect rejects the session. */
            __sync_fetch_and_add(&pvm_shm->pool_free, -1);
            pvm_child_bad = 1;
            pvm_forked_pid[s] = 0;
            pvm_forked_st[s] = 1;
            pvm_last_deleg_status = 1;
            return 1;
        }
        if (pvm_shm->found && pvm_is_fork_child) {
            /* I am not holding a slot (released above); the exit
               path's seat release must not return one. */
            pvm_worker_die_found();    /* noreturn */
        }
        if (pvm_dbg_on()) {
            static BPLONG last_lt = (BPLONG)-1;
            if ((BPLONG)local_top != last_lt) {
                fprintf(stderr, "WTCH pid=%d LT-chg %p -> %p\n",
                        (int)getpid(), (void *)last_lt,
                        (void *)local_top);
                last_lt = (BPLONG)local_top;
            }
        }
        pvm_park_sleep();
    }
}

/* Original re-entry word of a delegated frame (0 if not recorded). */
BPLONG pvm_deleg_reentry(BPLONG_PTR f)
{
    int s = pvm_slot_lookup(f);

    if (s >= 0)
        return pvm_forked_re[s];
    return 0;
}

int c_pvm_fork()
{
    BPLONG nt = INTVAL(ARG(1, 3));
    BPLONG mode = INTVAL(ARG(2, 3));
    BPLONG aval = INTVAL(ARG(3, 3));
    long i;

    pvm_sol_len = -1;  /* cleared at entry: a failed arm must not
                           leave a previous session's solution live */
    if (getenv("PVM_SEGV")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = pvm_segv_dump;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
    }
    if (nt < 1 || nt > PARVM_MAX_WORKERS) {
        bp_exception = out_of_range;
        return BP_ERROR;
    }
    if (mode != 1 && mode != 2 && mode != 3) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (mode == 3 && (aval < 1 || aval > 1000000)) {
        bp_exception = out_of_range;  /* mode 3: C = chunk size */
        return BP_ERROR;
    }
    if (pvm.armed || pvm_shm != NULL) {
        bp_exception = illegal_arguments;  /* nested sessions: unsupported */
        return BP_ERROR;
    }
    if (pvm_open_shm() != BP_TRUE) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }

    pvm.nt = nt;
    pvm.mode = mode;
    pvm.wid = 0;
    pvm.aval = (mode == 3) ? aval : 0;
    pvm.w_lo = 1;
    pvm.w_hi = aval;
    pvm.armed = 1;

    if (mode == 1 || mode == 3) {
        const char *fe;

        pvm_force_ar = 0;
        fe = getenv("PVM_FORCE_AR");
        if (fe && *fe)
            pvm_force_ar = (BPLONG)strtoull(fe, NULL, 16);
        /* root is worker 0; the pool grows through branch forks, and
           freed slots flow to the deepest active frontier */
        __sync_fetch_and_add(&pvm_shm->live, 1);
        pvm_shm->pool_free = nt - 1;
        pvm_my_seat = -1;
        pvm_my_rootframe = 0;
        pvm_rootframe_off = 0;
        pvm_root_onside = 0;
        pvm_seat_claim(PVM_DEPTH_SHALLOW);
        {
            struct sigaction sa;
            sa.sa_handler = pvm_sigchld;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
            sigaction(SIGCHLD, &sa, NULL);
        }
        return BP_TRUE;
    }

    /* mode 2: NT worker children; the root only collects. The number
       of non-empty chunks can be smaller than NT (e.g. 12 values,
       NT=8 -> 6 chunks of 2); fork exactly that many. */
    {
        BPLONG C = (aval + nt - 1) / nt;
        BPLONG nt_eff = (aval + C - 1) / C;
        for (i = 0; i < nt_eff; i++) {
            pid_t pid;
            pvm.wid = (BPLONG)i + 1;
            pvm.w_lo = (BPLONG)i * C + 1;
            pvm.w_hi = ((BPLONG)i * C + C > aval) ? aval : (BPLONG)i * C + C;
            pid = fork();
            if (pid == (pid_t)-1) {
                bp_exception = illegal_arguments;
                return BP_ERROR;
            }
            if (pid == 0) {
                pvm_is_fork_child = 1;
                pvm_nchildren = 0;   /* drop the root's inherited list:
                                         the atexit would quietly SIGKILL
                                         its (now sibling) processes */
                atexit(pvm_worker_exit_atexit);
                __sync_fetch_and_add(&pvm_shm->live, 1);
                return BP_TRUE;  /* continue the user's picat main here */
            }
            if (pvm_nchildren < PARVM_MAX_WORKERS)
                pvm_my_children[pvm_nchildren++] = pid;
        }
        pvm.wid = 0;
        pvm.w_lo = 1;
        pvm.w_hi = aval;
    }
    return BP_TRUE;
}

int c_pvm_worker_id()
{
    BPLONG i = ARG(1, 1);

    DEREF(i);
    if (!pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    ASSIGN_f_atom(i, MAKEINT(pvm.wid));
    return BP_TRUE;
}

int c_pvm_chunk()
{
    BPLONG lo = ARG(1, 2);
    BPLONG hi = ARG(2, 2);

    DEREF(lo);
    DEREF(hi);
    if (!pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    ASSIGN_f_atom(lo, MAKEINT(pvm.w_lo));
    ASSIGN_f_atom(hi, MAKEINT(pvm.w_hi));
    return BP_TRUE;
}

int c_pvm_report()
{
    BPLONG t = ARG(1, 1);

    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (pvm.mode == 2) {
        DEREF(t);
        if (!ISINT(t)) {
            bp_exception = c_type_error(et_INTEGER, t);
            return BP_ERROR;
        }
        __sync_fetch_and_add(&pvm_shm->count, INTVAL(t));
        return BP_TRUE;
    }
    /* mode 1/3: report the solution BY VALUE (a list or array of
       ground integers). The first reporter wins (CAS on found); a
       second reporter is a no-op (the first-found solution stands).
       Winner: serialize, then mark sol_len (the reader-side
       completion marker). A serialization error leaves found=1 with
       sol_len=-1; this process then exits non-zero, the bad flag is
       set, and pvm_collect rejects the session. */
    if (!__sync_bool_compare_and_swap(&pvm_shm->found, 0, 1))
        return BP_TRUE;
    DEREF(t);
    return pvm_serialize_solution(t);
}

int c_pvm_collect()
{
    BPLONG r = ARG(1, 1);
    long i;
    long result;

    DEREF(r);
    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (pvm_is_fork_child && (pvm.mode == 1 || pvm.mode == 3)) {
        /* a delegated branch whose (inherited) session has ended:
           finish the subtree and drop out of the process. A crashed
           grandchild was already flagged in the shared state; the
           root's collect rejects the session on it. Records the
           region-done outcome (st 0) for the (a) waiters. */
        if (pvm_shm->found) {
            pvm_reap_quiet = 1;
            for (i = 0; i < pvm_nchildren; i++)
                kill((pid_t)pvm_my_children[i], SIGKILL);
        }
        pvm_reap_my_children();
        pvm_reap_quiet = 0;
        pvm_worker_exit(0, 0);    /* noreturn */
    }
    if (!pvm_is_fork_child && (pvm.mode == 1 || pvm.mode == 3))
        pvm_seat_release();   /* the root is out of the frontier */
    if ((pvm.mode == 1 || pvm.mode == 3) && pvm_shm->found) {
        pvm_reap_quiet = 1;
        for (i = 0; i < pvm_nchildren; i++)
            kill(pvm_my_children[i], SIGKILL);
        pvm_reap_my_children();
        pvm_reap_quiet = 0;
    } else
        pvm_reap_my_children();
    if ((pvm_child_bad || pvm_shm->bad) &&
        !((pvm.mode == 1 || pvm.mode == 3) && pvm_shm->found &&
          pvm_shm->sol_len >= 0)) {
        /* a worker died inside the search (arena overflow, OOM,
           ...): in count mode the total would be a silent undercount,
           in an unsuccessful first-solution run the "no solution"
           verdict is untrustworthy, and a found whose report was
           never completed (sol_len < 0) is a crashed finder --
           refuse the result. */
        bp_exception = run_time_error;
        pvm.armed = 0;
        shm_unlink(pvm_shm_name);
        munmap(pvm_shm, sizeof(pvm_shm_t));
        pvm_shm = NULL;
        return BP_ERROR;
    }
    result = (pvm.mode == 1 || pvm.mode == 3)
         ? (long)pvm_shm->found : (long)pvm_shm->count;
    if ((pvm.mode == 1 || pvm.mode == 3) && result &&
        pvm_shm->sol_len >= 0) {
        /* copy the reported solution out of the shared block before
           the unmap; c_pvm_solution serves it from the local copy.
           The blocking reaps above finished the finder's process, so
           its write (integers, barrier, sol_len) is complete: seeing
           sol_len >= 0 here orders the data. */
        if (pvm_shm->sol_len <= PVM_SOL_CAP) {
            __sync_synchronize();
            memcpy(pvm_sol_cache, pvm_shm->sol,
                   (size_t)pvm_shm->sol_len * sizeof(BPLONG));
            pvm_sol_len = pvm_shm->sol_len;
        }
    }
    pvm.armed = 0;
    shm_unlink(pvm_shm_name);
    munmap(pvm_shm, sizeof(pvm_shm_t));
    pvm_shm = NULL;
    ASSIGN_f_atom(r, MAKEINT(result));
    return BP_TRUE;
}

#else /* !PAR_THREADS: single-threaded targets (wasm, non-Linux) */

PAR_TLS pvm_t pvm;
pvm_shm_t *pvm_shm = NULL;

static pvm_shm_t pvm_serial_shm;

int pvm_fork_frame(BPLONG_PTR ar, BPLONG_PTR p)
{
    (void)ar;
    (void)p;
    return 0;
}

void pvm_reap_my_children(void)
{
}

int c_pvm_delegate()
{
    BPLONG i = ARG(1, 1);
    DEREF(i);
    return BP_TRUE;   /* no-op: single-engine targets have no delegation */
}

int c_pvm_fork()
{
    BPLONG nt = INTVAL(ARG(1, 3));
    BPLONG mode = INTVAL(ARG(2, 3));
    BPLONG aval = INTVAL(ARG(3, 3));

    if (nt < 1 || (mode != 1 && mode != 2 && mode != 3)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    pvm_serial_shm.live = 0;
    pvm_serial_shm.count = 0;
    pvm_serial_shm.found = 0;
    pvm_serial_shm.sol_len = -1;
    pvm_shm = &pvm_serial_shm;
    pvm_sol_len = -1;
    pvm.nt = nt;
    pvm.mode = mode;
    pvm.wid = 0;
    pvm.aval = (mode == 3) ? aval : 0;
    pvm.w_lo = 1;
    pvm.w_hi = aval;
    pvm.armed = 1;
    return BP_TRUE;
}

int c_pvm_worker_id()
{
    BPLONG i = ARG(1, 1);
    DEREF(i);
    ASSIGN_f_atom(i, MAKEINT(0));
    return BP_TRUE;
}

int c_pvm_chunk()
{
    BPLONG lo = ARG(1, 2);
    BPLONG hi = ARG(2, 2);
    DEREF(lo);
    DEREF(hi);
    ASSIGN_f_atom(lo, MAKEINT(1));
    ASSIGN_f_atom(hi, MAKEINT(pvm.aval));
    return BP_TRUE;
}

int c_pvm_report()
{
    BPLONG t = ARG(1, 1);

    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (pvm.mode == 2) {
        DEREF(t);
        if (!ISINT(t)) {
            bp_exception = c_type_error(et_INTEGER, t);
            return BP_ERROR;
        }
        __sync_fetch_and_add(&pvm_shm->count, INTVAL(t));
        return BP_TRUE;
    }
    if (pvm_shm->found)
        return BP_TRUE;  /* a solution was already reported */
    pvm_shm->found = 1;
    DEREF(t);
    return pvm_serialize_solution(t);
}

int c_pvm_collect()
{
    BPLONG r = ARG(1, 1);
    DEREF(r);
    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if ((pvm.mode == 1 || pvm.mode == 3) && pvm_shm->found &&
        pvm_shm->sol_len >= 0 && pvm_shm->sol_len <= PVM_SOL_CAP) {
        __sync_synchronize();
        memcpy(pvm_sol_cache, pvm_shm->sol,
               (size_t)pvm_shm->sol_len * sizeof(BPLONG));
        pvm_sol_len = pvm_shm->sol_len;
    }
    ASSIGN_f_atom(r, MAKEINT((pvm.mode == 1 || pvm.mode == 3)
                             ? pvm_shm->found : pvm_shm->count));
    pvm.armed = 0;
    pvm_shm = NULL;
    return BP_TRUE;
}

int c_par_vm_test()
{
    BPLONG r = ARG(1, 1);
    return unify(r, BP_FALSE);
}

void Cboot_parvm()
{
    insert_cpred("c_par_vm_test", 1, c_par_vm_test);
    insert_cpred("pvm_delegate", 1, c_pvm_delegate);
    insert_cpred("pvm_fork", 3, c_pvm_fork);
    insert_cpred("pvm_worker_id", 1, c_pvm_worker_id);
    insert_cpred("pvm_chunk", 2, c_pvm_chunk);
    insert_cpred("pvm_report", 1, c_pvm_report);
    insert_cpred("pvm_collect", 1, c_pvm_collect);
    insert_cpred("pvm_solution", 1, c_pvm_solution);
}

#endif  /* PAR_THREADS */
