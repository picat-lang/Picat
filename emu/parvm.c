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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define PARVM_MAX_ENGINES 256

/* M2 cpreds (defined below, in both PAR_THREADS sections) */
int c_pvm_fork(void);
int c_pvm_worker_id(void);
int c_pvm_chunk(void);
int c_pvm_report(void);
int c_pvm_collect(void);

extern int toam(BPLONG_PTR, BPLONG_PTR, BPLONG_PTR);

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
    insert_cpred("pvm_fork", 3, c_pvm_fork);
    insert_cpred("pvm_worker_id", 1, c_pvm_worker_id);
    insert_cpred("pvm_chunk", 2, c_pvm_chunk);
    insert_cpred("pvm_report", 1, c_pvm_report);
    insert_cpred("pvm_collect", 1, c_pvm_collect);
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
 *   bp.pvm_report(N)           signal a solution (mode 1) or report N
 *       solutions for the local slice (mode 2).
 *   bp.pvm_collect(R)          root only: wait for the worker processes
 *       and return R (1/0 in mode 1, the total count in mode 2).
 *
 *   The user's main forks the session and then branches on
 *   pvm_worker_id: workers run the model (restricting the partition
 *   variable to their chunk in mode 2) and report; the root collects
 *   and prints. In mode 1 the root is worker 0 and searches its own
 *   territory as usual; on every choice point the FORK/SET_FORK hook
 *   (PVM_FORK_MAYBE, toam.h) may fork a child that takes the
 *   disjunction's remaining clauses while the caller (parent) keeps
 *   the first, growing the pool up to NT (disjoint OR subtrees, so a
 *   solution is examined exactly once). The first process to a
 *   solution sets shm->found; processes notice it at their next
 *   choicepoint and leave; the root unwinds its session to the
 *   user's ( model ; true ) cut, collects and prints.
 *
 *   Mode 1/3 value-chunking protocol (see pvm_fork_frame): a
 *   disjunction whose re-entry re-executes its FORK site (the CP
 *   value disjunctions) is split into contiguous value chunks of C
 *   values (mode 1: C = 1; mode 3: C = bp.pvm_fork third arg). The
 *   forking process (the owner) keeps values 1..C and walks them
 *   normally (its frame cell is NOT patched, so value failures
 *   re-enter through the original re-entry). When the owner's walk
 *   reaches value C+1 (re-entry re-fire with C values tried), it
 *   restores the frame to its fork-time entry state and goes to
 *   lab_pvm_deleg_fail: pvm_deleg_wait() blocks until the worker
 *   exits; if the worker's main succeeded the original re-entry is
 *   re-run locally from the fork-time entry (deterministic
 *   re-derivation from value 2); otherwise the disjunction fails to
 *   its caller. The worker starts the dispatch at the original
 *   re-entry, which advances from value 1 to value 2; its pending
 *   skip (from-2, from = C+1) makes it re-dispatch the re-entry
 *   once per skipped value (hook return 3) until it lands on value
 *   C+1. While still at the fork-time entry (value 1) the worker
 *   may tail-fork another worker for values 2C+1.. (from = 2C+1,
 *   skip = 2C-2) and so on, fanning the chunk chain out to NT
 *   processes at t=0; the last worker (no budget) walks its values
 *   to exhaustion. A worker's boundary (its own C+1th value)
 *   delegates upward the same way, so a whole chunk chain can park
 *   in delegated waits while the next process owns the walk.
 *   Delegation is attempted only when the engine state equals the
 *   frame's entry state (fresh choice frame), and a forked child
 *   restores the original re-entries of inherited frames outside its
 *   delegation lineage before searching.
 *
 *   A process forked mid-search inherits the C stack inside the toam
 *   loop and the COW engine state; its atexit handler reaps its own
 *   children and decrements the shared live counter.
 * ------------------------------------------------------------------ */

#define PARVM_MAX_WORKERS 256

PAR_TLS pvm_t pvm;
pvm_shm_t *pvm_shm = NULL;

static char pvm_shm_name[64];
static int pvm_is_fork_child = 0;

static pid_t pvm_my_children[PARVM_MAX_WORKERS];
static int pvm_nchildren = 0;

/* atexit for a forked worker: let its own (branch-forked) children
   finish and reap them, then drop the shared live counter. */
static void pvm_worker_exit_atexit(void)
{
    if (!pvm_is_fork_child) return;
    pvm_reap_my_children();
    if (pvm_shm != NULL)
        __sync_fetch_and_add(&pvm_shm->live, -1);
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
                if (pvm_my_children[i] == p)
                    pvm_my_children[i] = pvm_my_children[--pvm_nchildren];
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
           already reaped)
   st   = saved exit status of a reaped worker (-1 = no result yet)
   mine = this process forked the worker for this frame (inherited
           entries are COW-reset to 0 in every child)
   tail = this process is the tail owner (walks to exhaustion, never
           parks at a boundary) */
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
static char pvm_forked_tail[PVM_FRAME_LOG];

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

/* Set by pvm_labfail_park: the boundary owner's frame re-entry
   (P = AR_CPF(AR) at the parking lab_fail). The stub's success
   re-derivation re-dispatches it: standard backtrack state, and it
   advances exactly into the delegated value. */
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

/* Called from the FORK / SET_FORK macros in toam.h, i.e. exactly
   when a choice point (re)records AR_CPF = re-entry word of the
   disjunction's remaining alternatives. Modes 1/3 only. Return:
     0 = keep searching this value (caller continues the dispatch)
     1 = in the forked worker: dispatch at pvm_child_reentry
     2 = boundary: the caller has restored the frame to its
         fork-time entry (pvm_e1_*) and must go to
         lab_pvm_deleg_fail via &pvm_deleg_fail_word
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
    pid_t pid;
    int s;

    (void)p;
    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3))
        return 0;
    C = (pvm.mode == 1) ? 1 : pvm.aval;
    if (C < 1) C = 1;

    if (pvm_shm->found) {
        if (pvm_is_fork_child) {
            fflush(NULL);     /* _exit skips stdio flushing */
            _exit(0);         /* worker: the tree is solved, drop out */
        }
        return 0;             /* root: finish its own territory, then
                                  collect (early abort is a v3 item) */
    }

    re = AR_CPF(ar);
    if (re < 0x10000LL) return 0;                    /* no re-entry word */
    if (*(BPLONG *)(re) < 0x10000LL) return 0;       /* not a jmp entry */

    /* Delegate only when the engine state is exactly the frame's
       entry state: the frame was just created by the running
       instruction (a fresh choice frame) and nothing has executed
       since (on a value-disjunction re-entry the frame re-records
       the current value's state as its entry at the re-executed
       FORK, so the gate passes there too). For a SET_FORK mid-body
       on an already-executed call frame the state has diverged
       (trail/heap/locals), so skip. */
    if (heap_top != (BPLONG_PTR)AR_H(ar) ||
        trail_top != (BPLONG_PTR)AR_T(ar) ||
        sfreg != (BPLONG_PTR)AR_SF(ar) ||
        local_top != (BPLONG_PTR)AR_TOP(ar))
        return 0;

    s = pvm_slot_lookup(ar);
    if (s >= 0) {
        /* armed frame: this process owns this disjunction's value
           walk (as owner, tail worker, or re-running a re-run) */
        if (!pvm_forked_mine[s]) return 0;      /* inherited: not ours */
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
            return 0;   /* search this value ourselves */
        }
        /* tried >= C: boundary (chunk exhausted) or tail owner */
        if (pvm_forked_pid[s] > 0) {
            int wst;
            pid_t r = waitpid((pid_t)pvm_forked_pid[s], &wst, WNOHANG);
            if (r > 0) {
                pvm_forked_st[s] = WIFEXITED(wst) ? WEXITSTATUS(wst) : 1;
                pvm_forked_pid[s] = 0;
            } else if (r < 0 && errno == ECHILD) {
                pvm_forked_pid[s] = 0;  /* reaped elsewhere */
            }
            /* r == 0: worker still running */
        }
        /* Boundary re-firing reached without the lab_fail park having
           intercepted (should be unreachable: pvm_labfail_park parks
           one step earlier, before this value is advanced into).
            Degrade safely: search past the boundary ourselves; the
            worker runs on in parallel (first-solution is unaffected). */
        return 0;
    }

    /* unrecorded: the (re-)tried disjunction's fork site */
    if (pvm_shm->live >= pvm.nt)
        return 0;
    s = pvm_slot_alloc(ar);
    if (s < 0) return 0;   /* slot table full: search serially */

    pid = fork();
    if (pid == (pid_t)-1) return 0;  /* fork failed: continue serially */

    if (pid == 0) {
        /* worker: owns values C+1.., reached by skipping C-1 values
           from the dispatch point (value 2). The engine state is
           exactly the frame's entry (value 1 assigned); the frame
           cell is patched so that an exhaustion failure (should one
           happen before the first re-FORK re-records it) fails the
           disjunction rather than re-run the delegated values. */
        pvm_is_fork_child = 1;
        pvm_nchildren = 0;
        atexit(pvm_worker_exit_atexit);
        __sync_fetch_and_add(&pvm_shm->live, 1);
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
        pvm_forked_tail[s] = 1;
        pvm_forked_tried[s] = 0;
        pvm_forked_from[s] = C + 1;
        AR_CPF(ar) = (BPLONG)&pvm_deleg_fail_word;
        pvm_skip_count = C - 1;
        pvm_skip_frame = (BPLONG)ar;
        pvm_skip_armed = (pvm_skip_count > 0);
        pvm_child_reentry = re;
        return 1;
    }

    /* parent: owner of values 1..C, walked with the unpatched frame
       (value failures re-enter through the original re-entry). */
    pvm_forked_ar[s] = (BPLONG)ar;
    pvm_forked_re[s] = re;
    pvm_forked_e1H[s] = (BPLONG)heap_top;
    pvm_forked_e1T[s] = (BPLONG)trail_top;
    pvm_forked_e1SF[s] = (BPLONG)sfreg;
    pvm_forked_e1TOP[s] = (BPLONG)local_top;
    pvm_forked_pid[s] = (BPLONG)pid;
    pvm_forked_st[s] = -1;
    pvm_forked_mine[s] = 1;
    pvm_forked_tail[s] = 0;
    pvm_forked_tried[s] = 1;   /* value 1 will be searched */
    pvm_forked_from[s] = 1;
    if (pvm_nchildren < PARVM_MAX_WORKERS)
        pvm_my_children[pvm_nchildren++] = pid;
    return 0;
}

/* Called from the toam.h macro immediately after a forked worker is
   dispatched (t = 0: the worker is still at its fork-time entry,
   state = the frame's entry, nothing executed since the fork).
   Favors the chunk chain: if budget remains, fork a grandchild that
   owns the next C values (from + C), and this worker becomes the
   boundary owner of its own chunk. Return 1 only in the grandchild. */
int pvm_fork_frame_tail(BPLONG_PTR ar, BPLONG_PTR p)
{
    BPLONG re;
    long C, from, skip;
    pid_t pid;
    int s;

    (void)p;
    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3))
        return 0;
    if (pvm_shm->found || pvm_shm->live >= pvm.nt) return 0;
    C = (pvm.mode == 1) ? 1 : pvm.aval;
    if (C < 1) C = 1;
    s = pvm_slot_lookup(ar);
    if (s < 0 || !pvm_forked_mine[s] || pvm_forked_pid[s] != 0)
        return 0;
    re = pvm_forked_re[s];
    if (re < 0x10000LL) return 0;
    if (*(BPLONG *)(re) < 0x10000LL) return 0;
    if (heap_top != (BPLONG_PTR)AR_H(ar) ||
        trail_top != (BPLONG_PTR)AR_T(ar) ||
        sfreg != (BPLONG_PTR)AR_SF(ar) ||
        local_top != (BPLONG_PTR)AR_TOP(ar))
        return 0;

    from = pvm_forked_from[s] + C;
    skip = from - 2;   /* the dispatch lands on value 2 */
    if (skip < 0) skip = 0;

    pid = fork();
    if (pid == (pid_t)-1) return 0;
    if (pid == 0) {
        /* grandchild: same worker protocol, next chunk. (atexit and
           the live counter were already set up by the parent fork.) */
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
        pvm_forked_tail[s] = 1;
        pvm_forked_tried[s] = 0;
        pvm_forked_from[s] = from;
        AR_CPF(ar) = (BPLONG)&pvm_deleg_fail_word;
        pvm_skip_count = skip;
        pvm_skip_frame = (BPLONG)ar;
        pvm_skip_armed = (skip > 0);
        pvm_child_reentry = re;
        return 1;
    }

    /* this worker becomes the boundary owner of its own chunk */
    pvm_forked_pid[s] = (BPLONG)pid;
    pvm_forked_st[s] = -1;
    if (pvm_nchildren < PARVM_MAX_WORKERS)
        pvm_my_children[pvm_nchildren++] = pid;
    return 0;
}

/* Called from lab_fail just after P = AR_CPF(AR), i.e. right before
   the failed value's disjunction re-dispatches its re-entry (which
   advances to the next value). Parks the boundary owner HERE, one
   step before the boundary value is advanced into, so the re-derivation
   site is the re-entry itself (a valid dispatch label) and the engine
   state is exactly the frame's entry after a standard backtrack:
   re-dispatching the re-entry then searches the delegated value exactly
   as a serial search would. Returns 1 when the caller must park
   (P -> deleg-fail word, CONTCASE); 0 otherwise. A worker that sees
   the global solution drops out here (earlier than the next fork hook). */
int pvm_labfail_park(BPLONG_PTR ar, BPLONG_PTR p)
{
    long C;
    int s;

    if (pvm_shm == NULL || !pvm.armed ||
        (pvm.mode != 1 && pvm.mode != 3))
        return 0;
    if (pvm_shm->found) {
        if (pvm_is_fork_child) {
            fflush(NULL);     /* _exit skips stdio flushing */
            pvm_worker_exit_atexit();
            _exit(0);
        }
        return 0;             /* root: finish its own territory */
    }
    C = (pvm.mode == 1) ? 1 : pvm.aval;
    if (C < 1) C = 1;
    s = pvm_slot_lookup(ar);
    if (s >= 0 && pvm_forked_mine[s] &&
        pvm_forked_tried[s] >= C &&
        (pvm_forked_pid[s] > 0 || pvm_forked_st[s] != -1)) {
        pvm_rerun_site = (BPLONG)p;
        return 1;
    }
    return 0;
}

/* Clear a frame's delegation record (called by lab_pvm_deleg_fail
   after the outcome is consumed): a re-tried disjunction can fork
   afresh at its next fork site. */
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
    pvm_forked_tail[s] = 0;
}

/* Called from lab_pvm_deleg_fail (failure of a delegated disjunction).
   If this process is the delegator, the forked worker has just
   exhausted the delegated remaining clauses (it is still draining or
   already exited): wait for it. Returns the worker's main exit status
   (picat exits 0 iff its main succeeded), or -1 if the current
   process is not the delegator for f. A worker that notices the tree
   is solved drops out here. */
/* Last pvm_deleg_wait() result, readable from the toam stub (which
   lives in another translation unit and cannot take a C return into a
   computed-goto branch cleanly). -1 = not the delegator. */
int pvm_last_deleg_status = -1;

int pvm_deleg_wait(BPLONG_PTR f)
{
    int s = pvm_slot_lookup(f);
    int st = -1;

    if (s >= 0 && pvm_forked_mine[s]) {
        if (pvm_forked_pid[s] > 0) {
            int wst;
            if (waitpid((pid_t)pvm_forked_pid[s], &wst, 0) > 0) {
                st = WIFEXITED(wst) ? WEXITSTATUS(wst) : 1;
                pvm_forked_st[s] = st;
                pvm_forked_pid[s] = 0;
            }
        } else {
            st = pvm_forked_st[s];  /* reaped earlier in the hook */
        }
    }
    if (pvm_shm != NULL && pvm_shm->found && pvm_is_fork_child) {
        pvm_worker_exit_atexit();
        fflush(NULL);
        _exit(0);
    }
    pvm_last_deleg_status = st;
    return st;
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
        /* root is worker 0; the pool grows through branch forks */
        __sync_fetch_and_add(&pvm_shm->live, 1);
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
    BPLONG c = INTVAL(ARG(1, 1));

    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (pvm.mode == 1 || pvm.mode == 3) {
        pvm_shm->found = 1;
    } else {
        __sync_fetch_and_add(&pvm_shm->count, c);
    }
    return BP_TRUE;
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
           finish the subtree and drop out of the process. */
        if (pvm_shm->found)
            for (i = 0; i < pvm_nchildren; i++)
                kill(pvm_my_children[i], SIGKILL);
        pvm_reap_my_children();
        if (pvm_shm != NULL)
            __sync_fetch_and_add(&pvm_shm->live, -1);  /* _exit skips atexit */
        fflush(NULL);       /* _exit skips stdio flushing */
        _exit(0);
    }
    if ((pvm.mode == 1 || pvm.mode == 3) && pvm_shm->found)
        for (i = 0; i < pvm_nchildren; i++)
            kill(pvm_my_children[i], SIGKILL);
    pvm_reap_my_children();
    result = (pvm.mode == 1 || pvm.mode == 3)
         ? (long)pvm_shm->found : (long)pvm_shm->count;
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
    pvm_shm = &pvm_serial_shm;
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
    BPLONG c = INTVAL(ARG(1, 1));
    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (pvm.mode == 1 || pvm.mode == 3) pvm_shm->found = 1;
    else __sync_fetch_and_add(&pvm_shm->count, c);
    return BP_TRUE;
}

int c_pvm_collect()
{
    BPLONG r = ARG(1, 1);
    DEREF(r);
    if (pvm_shm == NULL || !pvm.armed) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
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
    insert_cpred("pvm_fork", 3, c_pvm_fork);
    insert_cpred("pvm_worker_id", 1, c_pvm_worker_id);
    insert_cpred("pvm_chunk", 2, c_pvm_chunk);
    insert_cpred("pvm_report", 1, c_pvm_report);
    insert_cpred("pvm_collect", 1, c_pvm_collect);
}

#endif  /* PAR_THREADS */
