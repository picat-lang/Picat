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
 *   Mode 1 delegation protocol (see pvm_fork_frame): a delegated
 *   frame's AR_CPF is patched to &pvm_deleg_fail_word in both copies;
 *   when its disjunction fails, lab_pvm_deleg_fail (emu_inst.h)
 *   blocks the delegator in pvm_deleg_wait() until the worker exits,
 *   and if the worker's main succeeded it re-runs the original
 *   re-entry locally to re-materialize the result (deterministic
 *   re-derivation); otherwise the disjunction fails to its caller.
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
   missed delegation (the parent then runs the remaining clauses
   itself), never break correctness.
   re   = original re-entry word (to restore in a child whose lineage
          is not the delegated one)
   pid  = worker forked for this frame (meaningful in the delegator's
          copy only; 0 in everyone else's)
   mine = this process forked the worker for this frame (inherited
          entries are COW-reset to 0 in every child) */
#define PVM_FRAME_LOG 1024
static BPLONG pvm_forked_ar[PVM_FRAME_LOG];
static BPLONG pvm_forked_re[PVM_FRAME_LOG];
static BPLONG pvm_forked_pid[PVM_FRAME_LOG];
static char pvm_forked_mine[PVM_FRAME_LOG];

/* Frames whose remaining clauses this process is exploring: this
   process's own fork frame plus the fork frames of its forking
   ancestors. A visible patch on a frame in this chain belongs to this
   lineage (kept); an inherited patch on any other frame belongs to
   some ancestor's kept first clause, so the remaining clauses are
   this process's own search space and the original re-entry must be
   restored here. */
#define PVM_DELEG_CHAIN 16
static BPLONG pvm_deleg_chain[PVM_DELEG_CHAIN];
static int pvm_deleg_chain_n = 0;

/* Original re-entry word of the frame this process was forked for:
   PVM_FORK_MAYBE dispatches the child there (its own frame cell is
   patched before the macro dispatches). */
BPLONG pvm_child_reentry = 0;

static int pvm_frame_slot(BPLONG_PTR ar)
{
    return (int)(((BPULONG)ar >> 4) % PVM_FRAME_LOG);
}

/* Called from the FORK / SET_FORK macros in toam.h, i.e. exactly when
   a choice point records AR_CPF = re-entry word of the disjunction's
   remaining clauses. Mode 1 only. If a fork is made: the child
   continues with the remaining clauses (the caller dispatches it at
   pvm_child_reentry); the parent keeps the first clause. Both copies
   patch the frame cell so that a failure of the disjunction (first
   clause in the parent, exhausted remaining clauses in the child)
   fails the whole disjunction to its caller (lab_pvm_deleg_fail,
   which waits for the worker if the current process is the
   delegator) instead of re-running the delegated clauses. Returns 1
   only in the child. */
int pvm_fork_frame(BPLONG_PTR ar)
{
    BPLONG re;
    pid_t pid;
    int s, k, i;

    if (pvm_shm == NULL || !pvm.armed || pvm.mode != 1) return 0;

    if (pvm_shm->found) {
        if (pvm_is_fork_child) {
            fflush(NULL);     /* _exit skips stdio flushing */
            _exit(0);         /* worker: the tree is solved, drop out */
        }
        return 0;             /* root: finish its own territory, then
                                  collect (early abort is a v2 item) */
    }

    if (pvm_shm->live >= pvm.nt) return 0;

    re = AR_CPF(ar);
    if (re < 0x10000LL) return 0;               /* no re-entry word */
    if (*(BPLONG *)(re) < 0x10000LL) return 0;  /* not a jmp entry */

    /* Delegate only when the engine state is exactly the frame's
       entry state: the frame was just created by the running
       instruction (a fresh choice frame) and nothing has executed
       since. Then a re-run of the re-entry from the forked state is
       serial-equivalent to a normal backtrack re-entry. For a
       SET_FORK mid-body on an already-executed call frame the state
       has diverged (trail/heap/locals), so skip. */
    if (heap_top != (BPLONG_PTR)AR_H(ar) ||
        trail_top != (BPLONG_PTR)AR_T(ar) ||
        sfreg != (BPLONG_PTR)AR_SF(ar) ||
        local_top != (BPLONG_PTR)AR_TOP(ar))
        return 0;

    s = pvm_frame_slot(ar);
    if (pvm_forked_ar[s] == (BPLONG)ar) return 0;  /* already delegated */
    pvm_forked_ar[s] = (BPLONG)ar;
    pvm_forked_re[s] = re;

    pid = fork();
    if (pid == (pid_t)-1) return 0;  /* fork failed: continue serially */

    if (pid == 0) {
        /* child: take the remaining clauses of this disjunction. The
           engine state is exactly what a frame failure would restore
           (the fork instruction allocates nothing, so heap/trail are
           at the frame entry values). */
        pvm_is_fork_child = 1;
        pvm_nchildren = 0;
        atexit(pvm_worker_exit_atexit);
        __sync_fetch_and_add(&pvm_shm->live, 1);

        /* Inherited delegation records: forget ownership, and restore
           the original re-entry for frames this process is NOT on the
           delegated side of (an ancestor's kept-first-clause
           territory: their remaining clauses are this process's own
           search space). Frames in the delegation chain (fork frames
           of this process or its forking ancestors) stay patched
           here: exhausting them must fail the disjunction, not
           re-run the remaining clauses. */
        for (k = 0; k < PVM_FRAME_LOG; k++) {
            BPLONG_PTR fr = (BPLONG_PTR)pvm_forked_ar[k];
            int in_chain;
            if (fr == (BPLONG_PTR)NULL || fr == ar) continue;
            pvm_forked_mine[k] = 0;
            in_chain = 0;
            for (i = 0; i < pvm_deleg_chain_n; i++)
                if (pvm_deleg_chain[i] == (BPLONG)fr) {
                    in_chain = 1;
                    break;
                }
            if (!in_chain)
                AR_CPF(fr) = pvm_forked_re[k];
        }

        if (pvm_deleg_chain_n < PVM_DELEG_CHAIN)
            pvm_deleg_chain[pvm_deleg_chain_n++] = (BPLONG)ar;

        /* patch my copy of my frame too (exhaustion must fail the
           disjunction, not re-run its remaining clauses); the caller
           dispatches at the saved original re-entry. */
        AR_CPF(ar) = (BPLONG)&pvm_deleg_fail_word;
        pvm_forked_pid[s] = 0;
        pvm_forked_mine[s] = 1;
        pvm_child_reentry = re;
        return 1;
    }

    /* parent: keep the first clause; patch this frame copy (COW: the
       child got the pre-patch image). */
    pvm_forked_pid[s] = (BPLONG)pid;
    pvm_forked_mine[s] = 1;
    AR_CPF(ar) = (BPLONG)&pvm_deleg_fail_word;
    if (pvm_nchildren < PARVM_MAX_WORKERS)
        pvm_my_children[pvm_nchildren++] = pid;
    return 0;
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
    int s = pvm_frame_slot(f);
    int st = -1;

    if (pvm_forked_mine[s] && pvm_forked_pid[s] > 0) {
        int wst;
        if (waitpid((pid_t)pvm_forked_pid[s], &wst, 0) > 0)
            st = WIFEXITED(wst) ? WEXITSTATUS(wst) : 1;
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
    int s = pvm_frame_slot(f);

    if (pvm_forked_ar[s] != (BPLONG)0 && pvm_forked_ar[s] == (BPLONG)f)
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
    if (mode != 1 && mode != 2) {
        bp_exception = illegal_arguments;
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
    pvm.aval = aval;
    pvm.w_lo = 1;
    pvm.w_hi = aval;
    pvm.armed = 1;

    if (mode == 1) {
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
    if (pvm.mode == 1) {
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
    if (pvm_is_fork_child && pvm.mode == 1) {
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
    if (pvm.mode == 1 && pvm_shm->found)
        for (i = 0; i < pvm_nchildren; i++)
            kill(pvm_my_children[i], SIGKILL);
    pvm_reap_my_children();
    result = (pvm.mode == 1) ? (long)pvm_shm->found : (long)pvm_shm->count;
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

int pvm_fork_frame(BPLONG_PTR ar)
{
    (void)ar;
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

    if (nt < 1 || mode != 1 && mode != 2) {
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
    pvm.aval = aval;
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
    if (pvm.mode == 1) pvm_shm->found = 1;
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
    ASSIGN_f_atom(r, MAKEINT(pvm.mode == 1 ? pvm_shm->found : pvm_shm->count));
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
