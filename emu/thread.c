/********************************************************************
 *   File   : thread.c
 *   Purpose: the thread module for Picat 3.9 (real OS threads).
 *
 *            The VM is single-threaded: only the thread that owns
 *            the toam loop may execute Picat code. Following the
 *            house pattern of the timer threads (event.c), a
 *            "thread" here is a pthread that runs a *registered C
 *            worker task* on plain C data -- never the VM -- and a
 *            result mailbox that the VM thread reads after
 *            join(). This is a safe concurrency layer: workers
 *            never touch the heap, so no GC interlocks are needed.
 *
 *            Syntax (module thread, lib2/thread.pi):
 *              T = new_thread(Task, [A1, ..., An])   % Task is an atom
 *              T.start()                             % spawns the worker
 *              join(T)                               % blocks until done
 *              R = result(T)                         % result after join
 *              this_thread() = T                     % main thread (id 0)
 *              M = new_mutex(); acquire_mutex(M); release_mutex(M)
 *              S = new_sem(N); p_sem(S); v_sem(S)
 *              C = new_counter(); V = counter_get(C)
 *
 *            Worker tasks and their arguments (all integers):
 *              sum_range(Lo, Hi)     -> sum of Lo..Hi       (mod 2^64)
 *              prod_range(Lo, Hi)    -> product Lo..Hi      (mod 2^64)
 *              bump(C, K, M)         -> C += 1, K times,
 *                                       under mutex M if M >= 0
 *              sleep_ms(Ms)          -> sleep
 *
 *            A worker that finishes updates its mailbox and signals
 *            the joiner's condition variable. join() on the main
 *            thread (id 0) or result() on a not-yet-joined thread
 *            fail.
 ********************************************************************/
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "term.h"
#include "basic.h"
#include "bapi.h"
#include "extern_decl.h"

#define MAX_THREADS 256
#define MAX_COUNTERS 64
#define MUSEMS_MAX 64

typedef struct {
    pthread_t pthread;
    int id;
    int status;     /* 0 = created, 1 = running, 2 = done */
    uint64_t result;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    /* worker context */
    const char *task;
    long args[8];
    int nargs;
} picat_thread_t;

typedef struct {
    int used;
    int id;
    pthread_mutex_t m;
} picat_mutex_t;

typedef struct {
    int used;
    int id;
    sem_t s;
} picat_sem_t;

static picat_thread_t threads[MAX_THREADS];
static picat_mutex_t mutexes[MUSEMS_MAX];
static picat_sem_t semaphores[MUSEMS_MAX];
static unsigned long long counters[MAX_COUNTERS];
static int counters_used[MAX_COUNTERS];
static int next_mutex = 0, next_sem = 0, next_thread = 1;

/* ---------------- worker tasks ---------------- */

static void task_sum_range(picat_thread_t *t)
{
    uint64_t s = 0, i;
    for (i = (uint64_t)t->args[0]; i <= (uint64_t)t->args[1]; i++) s += i;
    t->result = s;
}

static void task_prod_range(picat_thread_t *t)
{
    uint64_t p = 1, i;
    for (i = (uint64_t)t->args[0]; i <= (uint64_t)t->args[1]; i++) p *= i;
    t->result = p;
}

static void task_bump(picat_thread_t *t)
{
    int c = t->args[0], k = t->args[1], m = t->args[2];
    picat_mutex_t *mx = 0;
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (mutexes[i].used && mutexes[i].id == m) { mx = &mutexes[i]; break; }
    for (int i = 0; i < k; i++) {
        if (mx) pthread_mutex_lock(&mx->m);
        counters[c] += 1;
        if (mx) pthread_mutex_unlock(&mx->m);
    }
    t->result = counters[c];
}

static void task_sleep_ms(picat_thread_t *t)
{
    usleep((useconds_t)(t->args[0] * 1000));
    t->result = 0;
}

typedef void (*task_fn)(picat_thread_t *);
typedef struct { const char *name; task_fn fn; int narg; } task_def_t;

static task_def_t tasks[] = {
    { "sum_range", task_sum_range, 2 },
    { "prod_range", task_prod_range, 2 },
    { "bump", task_bump, 3 },
    { "sleep_ms", task_sleep_ms, 1 },
    { 0, 0, 0 }
};

static void *thread_worker(void *arg)
{
    picat_thread_t *t = (picat_thread_t *)arg;
    task_def_t *d;
    for (d = tasks; d->name; d++)
        if (strcmp(d->name, t->task) == 0) break;
    if (d->name) d->fn(t);
    pthread_mutex_lock(&t->lock);
    t->status = 2;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->lock);
    return 0;
}

/* ---------------- number <-> term helpers ---------------- */

static int term_to_long(BPLONG t, BPLONG *s)
{
    DEREF(t);
    if (ISINT(t)) { *s = INTVAL(t); return 1; }
    if (IS_BIGINT(t)) {
        BPLONG v = bp_bigint_to_native_long(t);
        *s = v;
        return 1;  /* values outside the i64 range wrap; tasks are mod 2^64 */
    }
    return 0;
}

/*
 * tasks produce unsigned mod 2^64 results; return the unsigned value.
 * 1-word ints (<= 2^56-1) are the same term type as literals in that
 * range, so prefer them; beyond, an unsigned bigint.
 */
static BPLONG u64_to_term2(uint64_t v)
{
    if (v <= (uint64_t)BP_MAXINT_1W) return MAKEINT((BPLONG)v);
    return bp_uint64_to_bigint(v);
}

/* ---------------- cpreds ---------------- */

int c_thread_new()
{
    BPLONG task = ARG(1, 3), al = ARG(2, 3), r = ARG(3, 3);
    task_def_t *d;
    picat_thread_t *t;
    BPLONG lst;
    long vals[8];
    int nargs = 0;
    int id;

    DEREF(task);
    if (!ISATOM(task)) return BP_FALSE;
    {
        const char *tname = GET_NAME(GET_ATM_SYM_REC(task));
        for (d = tasks; d->name; d++)
            if (strcmp(d->name, tname) == 0) break;
    }
    if (!d->name) return BP_FALSE;

    lst = al;
    DEREF(lst);
    while (ISLIST(lst) && nargs < d->narg) {
        BPLONG_PTR c = (BPLONG_PTR)UNTAGGED_ADDR(lst);
        BPLONG v;
        if (!term_to_long(FOLLOW(c), &v)) return BP_FALSE;
        vals[nargs++] = (long)v;
        lst = FOLLOW(c + 1);
        DEREF(lst);
    }
    if (nargs != d->narg) return BP_FALSE;

    id = next_thread++;
    if (id > MAX_THREADS) return BP_FALSE;
    t = &threads[id - 1];
    t->id = id;
    t->status = 0;
    t->nargs = nargs;
    t->task = d->name;
    for (int i = 0; i < nargs; i++) t->args[i] = vals[i];
    if (pthread_mutex_init(&t->lock, NULL) != 0) return BP_ERROR;
    pthread_cond_init(&t->cv, NULL);
    return unify(r, MAKEINT(id));
}

int c_thread_start()
{
    BPLONG tid = ARG(1, 1);
    DEREF(tid);
    if (!ISINT(tid)) return BP_FALSE;
    int id = INTVAL(tid);
    if (id < 1 || id >= next_thread) return BP_FALSE;
    picat_thread_t *t = &threads[id - 1];
    if (t->status != 0) return BP_FALSE;
    t->status = 1;
    if (pthread_create(&t->pthread, NULL, thread_worker, t) != 0) {
        t->status = 0;
        return BP_ERROR;
    }
    pthread_detach(t->pthread);  /* auto-reap; mailbox lives in a static array */
    return BP_TRUE;
}

int c_thread_join()
{
    BPLONG tid = ARG(1, 1);
    DEREF(tid);
    if (!ISINT(tid)) return BP_FALSE;
    int id = INTVAL(tid);
    if (id < 1 || id >= next_thread) return BP_FALSE;
    picat_thread_t *t = &threads[id - 1];
    if (t->status == 0) return BP_FALSE;  /* not started */
    pthread_mutex_lock(&t->lock);
    while (t->status != 2)
        pthread_cond_wait(&t->cv, &t->lock);
    pthread_mutex_unlock(&t->lock);
    return BP_TRUE;
}

int c_thread_this_thread()
{
    return unify(ARG(1, 1), MAKEINT(0));
}

int c_thread_result()
{
    BPLONG tid = ARG(1, 2), r = ARG(2, 2);
    DEREF(tid);
    if (!ISINT(tid)) return BP_FALSE;
    int id = INTVAL(tid);
    if (id < 1 || id >= next_thread) return BP_FALSE;
    picat_thread_t *t = &threads[id - 1];
    if (t->status != 2) return BP_FALSE;  /* not joined/finished */
    return unify(r, u64_to_term2(t->result));
}

/* ---------------- sync primitives ---------------- */

int c_thread_new_mutex()
{
    BPLONG r = ARG(1, 1);
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (!mutexes[i].used) {
            if (pthread_mutex_init(&mutexes[i].m, NULL) != 0) return BP_ERROR;
            mutexes[i].used = 1;
            mutexes[i].id = ++next_mutex;
            return unify(r, MAKEINT(next_mutex));
        }
    return BP_FALSE;
}

int c_thread_acquire_mutex()
{
    BPLONG mid = ARG(1, 1);
    DEREF(mid);
    if (!ISINT(mid)) return BP_FALSE;
    int id = INTVAL(mid);
    if (id < 1 || id > next_mutex) return BP_FALSE;
    int found = -1;
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (mutexes[i].used && mutexes[i].id == id) { found = i; break; }
    if (found < 0) return BP_FALSE;
    pthread_mutex_lock(&mutexes[found].m);
    return BP_TRUE;
}

int c_thread_release_mutex()
{
    BPLONG mid = ARG(1, 1);
    DEREF(mid);
    if (!ISINT(mid)) return BP_FALSE;
    int id = INTVAL(mid);
    int found = -1;
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (mutexes[i].used && mutexes[i].id == id) { found = i; break; }
    if (found < 0) return BP_FALSE;
    pthread_mutex_unlock(&mutexes[found].m);
    return BP_TRUE;
}

int c_thread_new_sem()
{
    BPLONG narg = ARG(1, 2), r = ARG(2, 2);
    BPLONG n;
    if (!term_to_long(narg, &n) || n < 0) return BP_FALSE;
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (!semaphores[i].used) {
            if (sem_init(&semaphores[i].s, 0, (unsigned)n) != 0) return BP_ERROR;
            semaphores[i].used = 1;
            semaphores[i].id = ++next_sem;
            return unify(r, MAKEINT(next_sem));
        }
    return BP_FALSE;
}

int c_thread_p_sem()
{
    BPLONG sid = ARG(1, 1);
    DEREF(sid);
    if (!ISINT(sid)) return BP_FALSE;
    int id = INTVAL(sid);
    int found = -1;
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (semaphores[i].used && semaphores[i].id == id) { found = i; break; }
    if (found < 0) return BP_FALSE;
    sem_wait(&semaphores[found].s);
    return BP_TRUE;
}

int c_thread_v_sem()
{
    BPLONG sid = ARG(1, 1);
    DEREF(sid);
    if (!ISINT(sid)) return BP_FALSE;
    int id = INTVAL(sid);
    int found = -1;
    for (int i = 0; i < MUSEMS_MAX; i++)
        if (semaphores[i].used && semaphores[i].id == id) { found = i; break; }
    if (found < 0) return BP_FALSE;
    sem_post(&semaphores[found].s);
    return BP_TRUE;
}

/* ---------------- shared counters (race demos) ---------------- */

int c_thread_new_counter()
{
    BPLONG r = ARG(1, 1);
    for (int i = 0; i < MAX_COUNTERS; i++)
        if (!counters_used[i]) {
            counters[i] = 0;
            counters_used[i] = 1;
            return unify(r, MAKEINT(i));
        }
    return BP_FALSE;
}

int c_thread_counter_get()
{
    BPLONG cid = ARG(1, 2), r = ARG(2, 2);
    DEREF(cid);
    if (!ISINT(cid)) return BP_FALSE;
    int id = INTVAL(cid);
    if (id < 0 || id >= MAX_COUNTERS || !counters_used[id]) return BP_FALSE;
    return unify(r, u64_to_term2(counters[id]));
}

void Cboot_thread()
{
    insert_cpred("thread_new", 3, c_thread_new);
    insert_cpred("thread_start", 1, c_thread_start);
    insert_cpred("thread_join", 1, c_thread_join);
    insert_cpred("thread_this_thread", 1, c_thread_this_thread);
    insert_cpred("thread_result", 2, c_thread_result);
    insert_cpred("thread_new_mutex", 1, c_thread_new_mutex);
    insert_cpred("thread_acquire_mutex", 1, c_thread_acquire_mutex);
    insert_cpred("thread_release_mutex", 1, c_thread_release_mutex);
    insert_cpred("thread_new_sem", 2, c_thread_new_sem);
    insert_cpred("thread_p_sem", 1, c_thread_p_sem);
    insert_cpred("thread_v_sem", 1, c_thread_v_sem);
    insert_cpred("thread_new_counter", 1, c_thread_new_counter);
    insert_cpred("thread_counter_get", 2, c_thread_counter_get);
}
