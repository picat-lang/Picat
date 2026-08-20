/********************************************************************
 *   File   : satext.c
 *   Purpose: exchangeable external SAT solver for Picat 3.9.
 *
 *            Runs an external CNF SAT solver on a formula given in
 *            Picat and returns status and model as Picat terms.
 *            Two solver input protocols are supported:
 *
 *              IPASIR 2.0  -- "s <nvars> <nclauses>" + clauses + "solve"
 *                              (cadical family)
 *              DIMACS      -- "p cnf ..." + clauses
 *                              (kissat, minisat, picosat, glucose,
 *                              cryptominisat, maplechrono, lingeling)
 *
 *            Protocol is picked from the solver name; unknown names
 *            are probed once and the result cached.
 *
 *            Input transfer (solvers that read CNF from stdin):
 *              small formulas -- text written directly to a pipe
 *              large formulas -- binary CNF staged in an anonymous
 *                                memfd; the fd is handed to the
 *                                satshim helper (next to the picat
 *                                binary) via SCM_RIGHTS, and the
 *                                shim re-formats the text and feeds
 *                                the solver.
 *            Spec lists containing the token "@file" get a DIMACS
 *            file generated in a temp dir; the token is replaced by
 *            the file path in the solver argv.
 *
 *            Exchangeable solver for the standard `import sat` flow:
 *            the clauses fed to the embedded solver (b_SAT_ADD_CL_c)
 *            are mirrored into a CNF buffer; when a solver is selected
 *            (bp.c_satext_set_solver(Spec) or SATEXT_SOLVER), the
 *            c_sat_start hook runs that solver on the mirrored CNF and
 *            maps the model back onto the dvars, so solve/solve_all/
 *            findall behave as with the embedded solver. The embedded
 *            solver is still fed, so a failed external run falls back
 *            to it.
 *
 *            Low-level cpred API (called via bp.<name> from Picat):
 *              c_satext_set_solver(Spec)
 *                 Spec: atom/string (name, PATH-resolved) or a list
 *                       of atoms/strings forming the solver argv
 *                       ("@file" token supported); nil/false clears.
 *                       A list of argv lists is a first-wins
 *                       portfolio: the solvers are raced on the same
 *                       CNF, the first decisive answer wins, the
 *                       rest are killed (max 8 solvers; see the
 *                       SATEXT_PRT_* variables below).
 *              c_satext_solve(Spec, Clauses, Status, Model)
 *                 Spec    : list of atoms or char-list strings, the
 *                           solver argv (first element = executable,
 *                           resolved via PATH unless it contains '/')
 *                 Clauses : list of clauses (each a list of integer
 *                           literals) OR flat list of literals with
 *                           0 clause separators
 *                 Status  : sat / unsat / unknown
 *                 Model   : list of 0/1 for vars 1..maxvar (nil if
 *                           none available)
 *              c_satext_cnf_info(Clauses, Nvar, Ncl, Nlits)
 *              c_satext_write_dimacs(Clauses, Path)
 *              c_satext_last_status(St)  how the most recent
 *                              solve(Vars) call resolved: 1 = SAT,
 *                              2 = UNSAT, 0 = unknown/abandoned
 *                              (external no-decisive-answer with
 *                              SATEXT_NO_FALLBACK set; the solve
 *                              failed WITHOUT a verdict - this is
 *                              not an UNSAT result)
 *
 *            Environment:
 *              SATEXT_SOLVER   solver selection for the `import sat`
 *                              flow. One solver: whitespace-separated
 *                              argv (first = executable, name or
 *                              path; the rest = extra arguments; a
 *                              "@file" token is replaced by a
 *                              generated CNF file). A portfolio:
 *                              '|' separates several such argv
 *                              strings; the solvers are raced and the
 *                              first decisive answer wins (max 8).
 *              SATEXT_PRT_BUDGET_MS
 *                              portfolio wall budget in ms per solve
 *                              (default 60000, 0 = no budget); on
 *                              expiry the race is killed and the
 *                              built-in solver answers.
 *              SATEXT_NO_FALLBACK
 *                              non-empty: when the external solver(s)
 *                              return unknown (e.g. the wall budget
 *                              elapsed) the built-in solver is NOT
 *                              run and the solve fails instead of
 *                              answering from it; default: the
 *                              built-in solver answers.
 *              SATEXT_PRT_MIN  estimated CNF size in bytes below which
 *                              a portfolio collapses to its first
 *                              solver (default 64 KiB).
 *              SATEXT_PRT_STATS  non-empty: print a per-solve line
 *                              with each racer's wall time and the
 *                              winner to stderr.
 *              SATEXT_SHIM     path of the satshim helper
 *              SATEXT_TMPDIR   directory for generated CNF files
 *                              (default: /dev/shm, else /tmp)
 *              SATEXT_SHIM_MIN estimate in bytes above which the
 *                              shim path is used (default 4 MiB)
 ********************************************************************/
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include "term.h"
#include "basic.h"
#include "bapi.h"

#define SATEXT_UNSAT_EXIT 20
#define SATEXT_SHIM_FD    3    /* fd carrying the memfd in the shim child */
#define SATEXT_OUT_FD     7    /* fd the solver's stdout is dup'd onto */
#define SATEXT_SHIM_MIN_BYTES (4L << 20)
/* portfolio (first-wins race of several external solvers) */
#define SATEXT_PRT_MAX         8      /* max solvers raced per solve */
#define SATEXT_PRT_BUDGET_MS   60000  /* default wall budget per solve */
#define SATEXT_PRT_MIN_BYTES   (64L << 10) /* race only above this size */

/* Every child of this process (and of the shim, which is itself such
   a child) must die when its direct parent dies, however it dies
   (Ctrl-C on picat, kill -9, crash): otherwise a killed picat leaves
   its racing solvers orphaned, spinning on full CPUs. PR_SET_PDEATHSIG
   makes the kernel deliver SIGKILL at the parent's death, and the
   flag survives exec, so the chain picat -> runner -> [shim ->]
   solver is covered hop by hop. */
static void child_dies_with_parent(void)
{
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
}

/* ------------------------------------------------------------------
 * number <-> term (same conventions as par.c)
 * ---------------------------------------------------------------- */

/* Read a term as a signed 64-bit value; 1 on success. */
static int term_to_i64_se(BPLONG t, int64_t *s)
{
    uint64_t v;
    BPLONG sign, size;
    int i;

    DEREF(t);
    if (ISINT(t)) { *s = INTVAL(t); return 1; }
    if (!IS_BIGINT(t)) return 0;
    {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(t);
        size = INTVAL(FOLLOW(ptr + 1));
        sign = (size < 0) ? -1 : 1;
        if (size < 0) size = -size;
        v = 0;
        {
            uint64_t pow = 1;
            BPLONG lst = FOLLOW(ptr + 2);
            for (i = 0; i < size; i++) {
                BPLONG_PTR cptr = (BPLONG_PTR)UNTAGGED_ADDR(lst);
                BPLONG d = INTVAL(FOLLOW(cptr));
                if ((uint64_t)d > (UINT64_MAX - v) / pow) return 0;
                v += (uint64_t)d * pow;
                pow *= 268435456ULL;
                lst = FOLLOW(cptr + 1);
            }
        }
    }
    if (sign == 1) {
        if (v > (uint64_t)INT64_MAX) return 0;
        *s = (int64_t)v;
    } else {
        if (v > (uint64_t)INT64_MAX + 1) return 0;
        if (v == (uint64_t)INT64_MAX + 1) *s = (int64_t)0x8000000000000000LL;
        else *s = -(int64_t)v;
    }
    return 1;
}

/* ------------------------------------------------------------------
 * string handling: Picat string (list of one-char ints) or atom
 * ---------------------------------------------------------------- */

static int term_to_cstr_char(BPLONG t, int *c)
{
    int64_t v;

    DEREF(t);
    if (ISINT(t)) {
        *c = (int)INTVAL(t);
        return (*c >= 0 && *c <= 127);
    }
    if (TAG(t) == ATM) {
        SYM_REC_PTR p = (SYM_REC_PTR)UNTAGGED_ADDR(t);
        if (GET_LENGTH(p) == 1) {
            *c = (int)GET_NAME(p)[0];
            return 1;
        }
    }
    if (term_to_i64_se(t, &v) && v >= 0 && v <= 127) {
        *c = (int)v;
        return 1;
    }
    return 0;
}

/* Copy a Picat string/atom term into a malloc'd C string. */
static int term_to_cstr(BPLONG t, char **out)
{
    size_t cap = 32, len = 0;
    char *s;

    DEREF(t);
    if (TAG(t) == ATM) {
        SYM_REC_PTR p = (SYM_REC_PTR)UNTAGGED_ADDR(t);
        size_t nl = (size_t)GET_LENGTH(p);
        s = (char *)malloc(nl + 1);
        if (s == NULL) return 0;
        memcpy(s, GET_NAME(p), nl);
        s[nl] = 0;
        *out = s;
        return 1;
    }
    if (!ISLIST(t)) return 0;
    s = (char *)malloc(cap);
    if (s == NULL) return 0;
    while (ISLIST(t)) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(t);
        BPLONG e = FOLLOW(ptr);
        int ch;
        DEREF(e);
        if (!term_to_cstr_char(e, &ch)) { free(s); return 0; }
        if (len + 2 > cap) {
            cap *= 2;
            s = (char *)realloc(s, cap);
            if (s == NULL) return 0;
        }
        s[len++] = (char)ch;
        t = FOLLOW(ptr + 1);
    }
    s[len] = 0;
    *out = s;
    return 1;
}

static char *str_dup(const char *s)
{
    char *d = (char *)malloc(strlen(s) + 1);
    if (d != NULL) strcpy(d, s);
    return d;
}

/* ------------------------------------------------------------------
 * CNF collection
 * ---------------------------------------------------------------- */

typedef struct {
    int32_t *lits;      /* literals, 0-terminated clauses */
    uint64_t cap;
    uint64_t nlits;
    uint64_t nclauses;
    uint64_t maxvar;
} cnf_t;

static void cnf_init(cnf_t *c)
{
    c->lits = (int32_t *)malloc(sizeof(int32_t) * 1024);
    c->cap = 1024;
    c->nlits = 0;
    c->nclauses = 0;
    c->maxvar = 0;
}

/* ------------------------------------------------------------------
 * persistent CNF buffer for the picat `import sat` flow:
 * b_SAT_ADD_CL_c mirrors every clause here; c_sat_init resets it;
 * c_sat_start hands it to the selected external solver.
 * ---------------------------------------------------------------- */

static cnf_t g_cnf;      /* g_cnf.lits == NULL until first push */
static int g_mirroring = 0;

static void ext_res_free(void);

void ext_cnf_reset(void)
{
    ext_res_free();
    if (g_cnf.lits != NULL) {
        free(g_cnf.lits);
        g_cnf.lits = NULL;
        g_cnf.cap = 0;
    }
    g_cnf.nlits = 0;
    g_cnf.nclauses = 0;
    g_cnf.maxvar = 0;
    g_mirroring = 0;
}

void ext_cnf_set_mirroring(int on)
{
    g_mirroring = on;
}

int satext_ext_mirroring(void)
{
    return g_mirroring;
}

/* one literal of the current clause (called in the order the clauses
   are built); var numbers must be > 0 */
void ext_cnf_push_lit(int32_t v)
{
    if (!g_mirroring) return;
    if (g_cnf.nlits == g_cnf.cap) {
        uint64_t nc = g_cnf.cap ? g_cnf.cap : 1024;
        nc *= 2;
        g_cnf.lits = (int32_t *)realloc(g_cnf.lits, nc * sizeof(int32_t));
        if (g_cnf.lits == NULL) return;
        g_cnf.cap = nc;
    }
    g_cnf.lits[g_cnf.nlits++] = v;
    if (v != 0) {
        int32_t a = v < 0 ? -v : v;
        if (a > 0 && (uint64_t)a > g_cnf.maxvar) g_cnf.maxvar = a;
    }
}

/* end of current clause */
void ext_cnf_end_clause(void)
{
    if (!g_mirroring) return;
    ext_cnf_push_lit(0);
    g_cnf.nclauses++;
}

const cnf_t *ext_cnf_get(void)
{
    return &g_cnf;
}

static int cnf_push(cnf_t *c, int32_t v, int terminator)
{
    if (c->nlits == c->cap) {
        c->cap *= 2;
        c->lits = (int32_t *)realloc(c->lits, c->cap * sizeof(int32_t));
        if (c->lits == NULL) return 0;
    }
    c->lits[c->nlits++] = v;
    if (terminator) c->nclauses++;
    return 1;
}

static int lit_range_ok(int64_t v)
{
    int64_t a = v < 0 ? -v : v;
    return a == 0 || (a >= 0 && a <= (int64_t)0x7fffffffLL);
}

/* One clause (list or array of integer literals). A clause that does
   not end in an explicit 0 gets one appended. */
static int cnf_collect_clause(BPLONG cl, cnf_t *c)
{
    int64_t v;
    int had0 = 0;
    int ok = 1;

    DEREF(cl);
    if (b_IS_ARRAY_c(cl)) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(cl);
        BPLONG i = GET_ARITY((SYM_REC_PTR)FOLLOW(ptr));
        for (; i > 0 && ok; i--) {
            BPLONG e = FOLLOW(ptr + i);
            DEREF(e);
            if (!term_to_i64_se(e, &v) || !lit_range_ok(v)) { ok = 0; break; }
            if (!cnf_push(c, (int32_t)v, v == 0)) { ok = 0; break; }
            if (v == 0) had0 = 1;
            else {
                uint64_t a = (uint64_t)(v < 0 ? -v : v);
                if (a > c->maxvar) c->maxvar = a;
            }
        }
        if (ok && !had0) ok = cnf_push(c, 0, 1);
        return ok;
    }
    while (ISLIST(cl) && ok) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(cl);
        BPLONG e = FOLLOW(ptr);
        DEREF(e);
        if (!term_to_i64_se(e, &v) || !lit_range_ok(v)) { ok = 0; break; }
        if (!cnf_push(c, (int32_t)v, v == 0)) { ok = 0; break; }
        if (v == 0) had0 = 1;
        else {
            uint64_t a = (uint64_t)(v < 0 ? -v : v);
            if (a > c->maxvar) c->maxvar = a;
        }
        cl = FOLLOW(ptr + 1);
    }
    if (ok && !had0) ok = cnf_push(c, 0, 1);
    return ok;
}

/* Parse a Picat term as CNF. Accepts
     [C1, C2, ...]                    (each Ci literals)
     [l1, l2, ..., 0, l3, ..., 0]     (flat, 0 separators)
   Also the corresponding arrays. Returns 1 on success. */
static int collect_cnf(BPLONG x, cnf_t *c)
{
    cnf_init(c);
    DEREF(x);
    if (x == nil_sym) return 1;            /* no clauses */
    if (!ISLIST(x) && !b_IS_ARRAY_c(x)) return 0;
    {
        int flat;
        if (b_IS_ARRAY_c(x)) {
            BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(x);
            BPLONG i = GET_ARITY((SYM_REC_PTR)FOLLOW(ptr));
            BPLONG first = FOLLOW(ptr + i);
            DEREF(first);
            flat = !ISLIST(first) && !b_IS_ARRAY_c(first);
            if (flat) {
                for (; i > 0; i--) {
                    BPLONG e = FOLLOW(ptr + i);
                    int64_t v;
                    DEREF(e);
                    if (!term_to_i64_se(e, &v)) return 0;
                    if (!cnf_push(c, (int32_t)v, v == 0)) return 0;
                    if (v != 0) {
                        uint64_t a = (uint64_t)(v < 0 ? -v : v);
                        if (a > c->maxvar) c->maxvar = a;
                    }
                }
                if (c->nlits == 0 || c->lits[c->nlits - 1] != 0)
                    if (!cnf_push(c, 0, 1)) return 0;
            } else {
                for (; i > 0; i--)
                    if (!cnf_collect_clause(FOLLOW(ptr + i), c)) return 0;
            }
        } else {
            BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(x);
            BPLONG first = FOLLOW(ptr);
            DEREF(first);
            flat = !ISLIST(first) && !b_IS_ARRAY_c(first);
            if (flat) {
                BPLONG t = x;
                while (ISLIST(t)) {
                    BPLONG_PTR tp = (BPLONG_PTR)UNTAGGED_ADDR(t);
                    BPLONG e = FOLLOW(tp);
                    int64_t v;
                    DEREF(e);
                    if (!term_to_i64_se(e, &v)) return 0;
                    if (!cnf_push(c, (int32_t)v, v == 0)) return 0;
                    if (v != 0) {
                        uint64_t a = (uint64_t)(v < 0 ? -v : v);
                        if (a > c->maxvar) c->maxvar = a;
                    }
                    t = FOLLOW(tp + 1);
                }
                if (c->nlits == 0 || c->lits[c->nlits - 1] != 0)
                    if (!cnf_push(c, 0, 1)) return 0;
            } else {
                BPLONG t = x;
                while (ISLIST(t)) {
                    BPLONG_PTR tp = (BPLONG_PTR)UNTAGGED_ADDR(t);
                    if (!cnf_collect_clause(FOLLOW(tp), c)) return 0;
                    t = FOLLOW(tp + 1);
                }
            }
        }
    }
    return 1;
}

static void cnf_fin(cnf_t *c)
{
    free(c->lits);
    c->lits = NULL;
}

/* ------------------------------------------------------------------
 * protocol registry
 * ---------------------------------------------------------------- */

enum { PROTO_NONE = -1, PROTO_DIMACS = 0, PROTO_IPASIR = 1 };

static int proto_by_name(const char *base)
{
    if (strncmp(base, "kissat", 6) == 0 ||
        strncmp(base, "minisat", 7) == 0 ||
        strncmp(base, "picosat", 7) == 0 ||
        strncmp(base, "glucose", 7) == 0 ||
        strncmp(base, "cryptominisat", 13) == 0 ||
        strncmp(base, "maplechrono", 11) == 0 ||
        strncmp(base, "lingeling", 9) == 0)
        return PROTO_DIMACS;
    /* CaDiCaL: the 1.x/2.1-era binaries read DIMACS only; modern
       CaDiCaL auto-detects the protocol from the "p" header, so
       DIMACS is the safe choice for the whole family */
    if (strncmp(base, "cadical", 7) == 0) return PROTO_DIMACS;
    return PROTO_NONE;
}

typedef struct { char *name; int proto; } proto_cache_t;
static proto_cache_t proto_cache[32];
static int proto_cache_n = 0;

static const char *base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Run `exe` on a tiny formula using the guessed protocol; report the
   protocol iff it produced a clean SAT/UNSAT status. */
static int probe_one(const char *exe, int guess)
{
    int p[2], q[2];
    pid_t pid;
    static const char probe_ipasir[] =
        "p ipasir-2 3 3\nc 1 2 0\nc -1 3 0\nc -1 -2 0\nsolve\n";
    static const char probe_dimacs[] =
        "p cnf 3 3\n1 2 0\n-1 3 0\n-1 -2 0\n";
    const char *text = (guess == PROTO_IPASIR) ? probe_ipasir : probe_dimacs;
    char *out = (char *)malloc(65536);
    size_t outlen = 0;
    int st = 0, ok = 0;

    if (out == NULL) return PROTO_NONE;
    if (pipe(p) != 0 || pipe(q) != 0) { free(out); return PROTO_NONE; }
    pid = fork();
    if (pid < 0) { free(out); return PROTO_NONE; }
    if (pid == 0) {
        child_dies_with_parent();
        char *prog = str_dup(exe);
        int n = open("/dev/null", 0);
        dup2(p[0], 0);
        dup2(q[1], 1);
        if (n >= 0) dup2(n, 2);      /* probe noise stays invisible */
        signal(SIGPIPE, SIG_IGN);
        {
            int i;
            for (i = 3; i < 1024; i++) close(i);
        }
        execvp(prog, (char *[]){ prog, NULL });
        _exit(127);
    }
    close(p[0]); close(q[1]);
    {
        const char *cc = text;
        while (*cc) {
            ssize_t r = write(p[1], cc, strlen(cc));
            if (r <= 0) break;
            cc += r;
        }
    }
    close(p[1]);
    for (;;) {
        ssize_t r = read(q[0], out + outlen, 65536 - outlen - 1);
        if (r <= 0) break;
        outlen += (size_t)r;
        if (outlen + 1 >= 65536) break;
    }
    close(q[0]);
    out[outlen] = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) &&
        (WEXITSTATUS(st) == 10 || WEXITSTATUS(st) == 20)) {
        if (strstr(out, "SATISFIABLE") != NULL ||
            strstr(out, "UNSATISFIABLE") != NULL)
            ok = 1;
    }
    free(out);
    return ok ? guess : PROTO_NONE;
}

static int detect_protocol(const char *exe, const char *base)
{
    int i, pr;

    pr = proto_by_name(base);
    if (pr != PROTO_NONE) return pr;
    for (i = 0; i < proto_cache_n; i++)
        if (strcmp(proto_cache[i].name, base) == 0)
            return proto_cache[i].proto;
    pr = probe_one(exe, PROTO_IPASIR);
    if (pr == PROTO_NONE) pr = probe_one(exe, PROTO_DIMACS);
    if (proto_cache_n < 32) {
        proto_cache[proto_cache_n].name = str_dup(base);
        proto_cache[proto_cache_n].proto = pr;
        proto_cache_n++;
    }
    return pr;
}

/* ------------------------------------------------------------------
 * solver output parsing
 * ---------------------------------------------------------------- */

typedef struct {
    int status;          /* 0 unknown, 1 sat, 2 unsat */
    int32_t *model;      /* 0/1 per var 1..nvar */
    uint64_t model_len;
    uint64_t model_idx;  /* next var for IPASIR value-format models */
    int have_model;
} solve_out_t;

static void parse_out_line(char *line, uint64_t nvar, int proto,
                           solve_out_t *o)
{
    char *p;

    if (line[0] == 'c' || line[0] == 't' || line[0] == 0) return;
    if (line[0] == 's') {
        p = line + 1;
        while (*p == ' ') p++;
        if (strncmp(p, "UNSATISFIABLE", 13) == 0) o->status = 2;
        else if (strncmp(p, "SATISFIABLE", 11) == 0) o->status = 1;
        else if (strncmp(p, "UNKNOWN", 7) == 0) o->status = 0;
        return;
    }
    if (line[0] == 'v') {
        char *q = line + 1;
        uint64_t idx;
        int32_t *m;
        while (*q == ' ') q++;
        if (*q == '0' && (q[1] == 0 || q[1] == '\n')) return;  /* empty model */
        if (!o->have_model) {
            m = (int32_t *)calloc((size_t)nvar, sizeof(int32_t));
            if (m == NULL) return;
            o->model = m;
            o->model_len = nvar;
            o->model_idx = 0;
            o->have_model = 1;
        }
        m = o->model;
        idx = o->model_idx;
        while (*q && *q != '\n') {
            char *end;
            long v;
            while (*q == ' ') q++;
            if (*q == 0) break;
            v = strtol(q, &end, 10);
            if (end == q) break;
            q = end;
            if (proto == PROTO_IPASIR) {
                if (idx < nvar && (v == 0 || v == 1)) m[idx] = (int32_t)v;
                idx++;
            } else if (v != 0) {
                uint64_t a = (uint64_t)(v < 0 ? -v : v);
                if (a >= 1 && a <= nvar) m[a - 1] = (v > 0) ? 1 : 0;
            }
        }
        o->model_idx = idx;
    }
}

/* Read the solver's stdout pipe to EOF, parsing status/model.
   Grows the buffer as needed (models can be MBs wide for big models). */
static void drain_output(int fd, uint64_t nvar, int proto, solve_out_t *o)
{
    size_t cap = 256 * 1024, len = 0, off;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) { close(fd); return; }
    for (;;) {
        ssize_t r;
        if (len + 8192 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) break;
        }
        r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        len += (size_t)r;
        off = 0;
        while (off < len) {
            char *nl = memchr(buf + off, '\n', len - off);
            size_t ll = (nl != NULL) ? (size_t)(nl - (buf + off)) : len - off;
            if (nl == NULL) break;
            *nl = 0;
            parse_out_line(buf + off, nvar, proto, o);
            off += ll + 1;
        }
        if (off > 0) {
            memmove(buf, buf + off, len - off);
            len -= off;
        }
    }
    if (len > 0) {
        buf[len] = 0;
        parse_out_line(buf, nvar, proto, o);
    }
    close(fd);
    free(buf);
}

/* ------------------------------------------------------------------
 * heap room (same pattern as par.c)
 * ---------------------------------------------------------------- */

static int ensure_heap_room(BPLONG need, BPLONG *r, BPLONG arity, BPLONG pos)
{
    int tries = 0;
    BPLONG total;
    garbage_collector();
    while (local_top - heap_top <= need && tries < 16) {
        if (toam_signal_vec != 0 || in_critical_region != 0) return -1;
        total = (heap_top - stack_low_addr) + (stack_up_addr - local_top)
                + need + (1 << 20);
        if (expand_local_global_stacks(total) == BP_ERROR) {
            if (expand_local_global_stacks(0) == BP_ERROR) return -1;
        }
        *r = ARG(pos, arity);     /* arena moved: re-fetch the slot */
        tries++;
    }
    return (local_top - heap_top > need) ? 0 : -1;
}

static BPLONG bools_to_list(const int32_t *m, BPLONG n)
{
    BPLONG i, lst0;
    BPLONG_PTR ptr;

    if (n == 0) return nil_sym;
    LOCAL_OVERFLOW_CHECK_WITH_MARGIN("satext", 2L * n + 8);
    lst0 = ADDTAG(heap_top, LST);
    FOLLOW(heap_top++) = MAKEINT(m[0]);
    ptr = heap_top++;
    for (i = 1; i < n; i++) {
        FOLLOW(ptr) = ADDTAG(heap_top, LST);
        FOLLOW(heap_top++) = MAKEINT(m[i]);
        ptr = heap_top++;
    }
    FOLLOW(ptr) = nil_sym;
    return lst0;
}

/* ------------------------------------------------------------------
 * spec handling
 * ---------------------------------------------------------------- */

typedef struct {
    char **argv;          /* NULL-terminated, malloc'd */
    int argc;
    int atfile;           /* index of "@file" in argv, or -1 */
    char *exe;            /* argv[0] */
    char *base;           /* basename(exe) */
} spec_t;

static void spec_free(spec_t *s)
{
    int i;
    for (i = 0; i < s->argc; i++) free(s->argv[i]);
    free(s->argv);
    free(s->base);
    s->argv = NULL;
    s->argc = 0;
    s->exe = NULL;
    s->base = NULL;
}

/* An ordered selection of one or more solver specs. n == 1 is a plain
   single-solver selection; n > 1 is a first-wins portfolio (the
   solvers are raced and the first decisive answer wins). */
typedef struct {
    int n;
    spec_t specs[SATEXT_PRT_MAX];
    int proto[SATEXT_PRT_MAX];
} spec_list_t;

static void spec_list_fin(spec_list_t *sl)
{
    int i;
    for (i = 0; i < sl->n; i++) spec_free(&sl->specs[i]);
    sl->n = 0;
}

static int spec_list_empty(spec_list_t *sl)
{
    sl->n = 0;
    return 1;
}

/* Parse one whitespace-separated argv string into s. */
static int spec_from_cstr(const char *str, spec_t *s)
{
    const char *p = str;
    size_t cap = 8;

    s->argv = (char **)malloc(cap * sizeof(char *));
    if (s->argv == NULL) return 0;
    s->argc = 0;
    s->atfile = -1;
    s->exe = NULL;
    s->base = NULL;

    while (*p != 0) {
        const char *q;
        char *tok;
        size_t len;
        while (isspace((unsigned char)*p)) p++;
        if (*p == 0) break;
        q = p;
        while (*q != 0 && !isspace((unsigned char)*q)) q++;
        len = (size_t)(q - p);
        tok = (char *)malloc(len + 1);
        if (tok == NULL) { spec_free(s); return 0; }
        memcpy(tok, p, len);
        tok[len] = 0;
        p = q;
        if (s->argc == (int)cap) {
            cap *= 2;
            s->argv = (char **)realloc(s->argv, cap * sizeof(char *));
            if (s->argv == NULL) { free(tok); spec_free(s); return 0; }
        }
        s->argv[s->argc] = tok;
        if (s->argc == 0) {
            s->base = str_dup(base_name(tok));
            if (s->base == NULL) { spec_free(s); return 0; }
        } else if (strcmp(tok, "@file") == 0) {
            s->atfile = s->argc;
        }
        s->argc++;
    }
    if (s->argc == 0) { spec_free(s); return 0; }
    s->argv[s->argc] = NULL;
    s->exe = s->argv[0];  /* alias: freed with argv */
    return 1;
}

/* Parse a selection string (the SATEXT_SOLVER value): several
   whitespace-separated argv strings joined by '|'. One part without
   '|' is a single-solver selection; more parts form a portfolio. */
static int spec_list_from_cstr(const char *str, spec_list_t *sl)
{
    const char *p = str;

    spec_list_empty(sl);
    for (;;) {
        const char *bar = strchr(p, '|');
        char part[1024];
        size_t len = (bar != NULL) ? (size_t)(bar - p) : strlen(p);

        if (len >= sizeof(part)) break;    /* absurd; treat as invalid */
        memcpy(part, p, len);
        part[len] = 0;
        if (len > 0 && sl->n < SATEXT_PRT_MAX) {
            int pr;
            if (!spec_from_cstr(part, &sl->specs[sl->n])) break;
            pr = detect_protocol(sl->specs[sl->n].exe,
                                 sl->specs[sl->n].base);
            if (pr == PROTO_NONE) {
                fprintf(stderr,
                        "satext: cannot detect the protocol of \"%s\"; "
                        "ignoring it\n", sl->specs[sl->n].exe);
                spec_free(&sl->specs[sl->n]);
            } else {
                sl->proto[sl->n] = pr;
                sl->n++;
            }
        }
        if (bar == NULL) break;
        p = bar + 1;
        if (sl->n >= SATEXT_PRT_MAX) {
            fprintf(stderr,
                    "satext: at most %d portfolio solvers are supported; "
                    "ignoring the rest\n", SATEXT_PRT_MAX);
            break;
        }
    }
    return (sl->n >= 1);
}

static int parse_spec(BPLONG x, spec_t *s)
{
    BPLONG t;
    size_t cap = 16;

    s->argv = (char **)malloc(cap * sizeof(char *));
    s->argc = 0;
    s->atfile = -1;
    s->exe = NULL;
    s->base = NULL;
    if (s->argv == NULL) return 0;

    DEREF(x);
    if (x == nil_sym) return 0;
    if (!ISLIST(x)) return 0;
    t = x;
    while (ISLIST(t)) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(t);
        BPLONG e = FOLLOW(ptr);
        char *cs;
        DEREF(e);
        if (!term_to_cstr(e, &cs)) { spec_free(s); return 0; }
        if (s->argc == (int)cap) {
            cap *= 2;
            s->argv = (char **)realloc(s->argv, cap * sizeof(char *));
            if (s->argv == NULL) { free(cs); spec_free(s); return 0; }
        }
        s->argv[s->argc] = cs;
        if (s->argc == 0) {
            if (cs[0] == 0) { spec_free(s); return 0; }
            s->base = str_dup(base_name(cs));
        } else if (strcmp(cs, "@file") == 0) {
            s->atfile = s->argc;
        }
        s->argc++;
        t = FOLLOW(ptr + 1);
    }
    s->exe = s->argv[0];   /* alias: freed together with argv */
    s->argv[s->argc] = NULL;
    return 1;
}

/* Parse a Picat term as a spec: a single atom/string (solver name or
   path) or the full argv list understood by parse_spec. */
static int parse_spec_term(BPLONG x, spec_t *s)
{
    DEREF(x);
    if (TAG(x) == ATM) {
        SYM_REC_PTR p = (SYM_REC_PTR)UNTAGGED_ADDR(x);
        char *cs = str_dup(GET_NAME(p));
        if (cs == NULL) return 0;
        s->argv = (char **)malloc(2 * sizeof(char *));
        if (s->argv == NULL) { free(cs); return 0; }
        s->argv[0] = cs;
        s->argv[1] = NULL;
        s->argc = 1;
        s->atfile = -1;
        s->exe = cs;                    /* alias, freed with argv */
        s->base = str_dup(base_name(cs));
        return (s->base != NULL);
    }
    return parse_spec(x, s);
}

/* 1 if x is a Picat string: [] or a list of one-character atoms
   (strings and one-arg argv lists are term-identical; the selection
   grammar uses this to tell them apart) */
static int is_string(BPLONG x)
{
    BPLONG t = x;

    DEREF(t);
    if (t == nil_sym) return 1;
    if (!ISLIST(t)) return 0;
    for (;;) {
        BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t);
        BPLONG e = FOLLOW(p);
        DEREF(e);
        if (TAG(e) != ATM) return 0;
        if (strlen(GET_NAME((SYM_REC_PTR)UNTAGGED_ADDR(e))) != 1) return 0;
        {
            BPLONG tl = FOLLOW(p + 1);
            DEREF(tl);
            if (tl == nil_sym) return 1;
            if (!ISLIST(tl)) return 0;
            t = tl;
        }
    }
}

/* Parse a Picat term as a solver selection:
       atom / string             -> one solver (name or path)
       [A1, ..., An]             -> one solver, full argv
       [[A1, ...], [B1, ...]]    -> portfolio: several solvers, each
                                     with its own argv (first-wins);
                                     recognized only when every element
                                     is a list that is not a string
   Protocols are detected; a solver whose protocol cannot be detected
   is skipped (with a warning) if it has companions, or the call fails
   if it is the only one. */
static int parse_spec_list(BPLONG x, spec_list_t *sl)
{
    spec_list_empty(sl);
    DEREF(x);

    if (ISLIST(x)) {
        BPLONG_PTR p0 = (BPLONG_PTR)UNTAGGED_ADDR(x);
        BPLONG e0 = FOLLOW(p0);
        DEREF(e0);
        if (ISLIST(e0) && !is_string(e0)) {   /* a list of argv lists */
            BPLONG t2 = x;
            while (ISLIST(t2)) {
                BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(t2);
                BPLONG e = FOLLOW(p);
                BPLONG pe;
                DEREF(e);
                if (!ISLIST(e) || is_string(e)) {
                    spec_list_fin(sl);
                    return 0;
                }
                if (sl->n >= SATEXT_PRT_MAX) { spec_list_fin(sl); return 0; }
                if (!parse_spec(e, &sl->specs[sl->n])) {
                    spec_list_fin(sl);
                    return 0;
                }
                {
                    int pr = detect_protocol(sl->specs[sl->n].exe,
                                              sl->specs[sl->n].base);
                    if (pr == PROTO_NONE) {
                        fprintf(stderr,
                                "satext: cannot detect the protocol of "
                                "\"%s\"; ignoring it\n",
                                sl->specs[sl->n].exe);
                        spec_free(&sl->specs[sl->n]);
                    } else {
                        sl->proto[sl->n] = pr;
                        sl->n++;
                    }
                }
                pe = FOLLOW(p + 1);
                DEREF(pe);
                t2 = pe;
            }
            return (sl->n >= 1);
        }
    }
    {
        spec_t tmp;
        int pr;
        if (!parse_spec_term(x, &tmp)) return 0;
        pr = detect_protocol(tmp.exe, tmp.base);
        if (pr == PROTO_NONE) {
            fprintf(stderr,
                    "satext: cannot detect the protocol of \"%s\"; "
                    "ignoring it\n", tmp.exe);
            spec_free(&tmp);
            return 0;
        }
        sl->specs[0] = tmp;
        sl->proto[0] = pr;
        sl->n = 1;
        return 1;
    }
}

/* ------------------------------------------------------------------
 * CNF text rendering
 * ---------------------------------------------------------------- */

typedef struct { char *b; size_t len, cap; } sb_t;

static int sb_put(sb_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64 * 1024;
        while (b->len + n + 1 > nc) nc *= 2;
        b->b = (char *)realloc(b->b, nc);
        if (b->b == NULL) return 0;
        b->cap = nc;
    }
    memcpy(b->b + b->len, s, n);
    b->len += n;
    b->b[b->len] = 0;
    return 1;
}

static int sb_int(sb_t *b, int64_t v)
{
    char t[24], tmp[24];
    size_t k = 0, j = 0;
    uint64_t u;

    if (v < 0) { t[k++] = '-'; u = (uint64_t)-v; }
    else u = (uint64_t)v;
    do { tmp[j++] = (char)('0' + (char)(u % 10u)); u /= 10u; } while (u > 0);
    while (j > 0) t[k++] = tmp[--j];
    return sb_put(b, t, k);
}

/* pfx: "" for DIMACS, "c" for IPASIR-2 clause lines
   ("c <lits> 0"), emitted before the first literal of each clause */
static int render_clauses(const cnf_t *c, sb_t *b, const char *pfx)
{
    uint64_t off;
    for (off = 0; off < c->nlits; off++) {
        if ((off == 0 || c->lits[off - 1] == 0) &&
            !sb_put(b, pfx, strlen(pfx)))
            return 0;
        if (!sb_int(b, (int64_t)c->lits[off])) return 0;
        if (!sb_put(b, " ", 1)) return 0;
        if (c->lits[off] == 0 && !sb_put(b, "\n", 1)) return 0;
    }
    return 1;
}

static int render_dimacs(const cnf_t *c, sb_t *b)
{
    char h[64];
    int hl = snprintf(h, sizeof(h), "p cnf %llu %llu\n",
                      (unsigned long long)c->maxvar,
                      (unsigned long long)c->nclauses);
    if (hl < 0) return 0;
    if (!sb_put(b, h, (size_t)hl)) return 0;
    return render_clauses(c, b, "");
}

static int render_ipasir(const cnf_t *c, sb_t *b)
{
    char h[64];
    int hl = snprintf(h, sizeof(h), "p ipasir-2 %llu %llu\n",
                      (unsigned long long)c->maxvar,
                      (unsigned long long)c->nclauses);
    if (hl < 0) return 0;
    if (!sb_put(b, h, (size_t)hl)) return 0;
    if (!render_clauses(c, b, "c ")) return 0;
    return sb_put(b, "solve\n", 6);
}

/* ------------------------------------------------------------------
 * solver execution
 * ---------------------------------------------------------------- */

static int write_all(int fd, const void *s, size_t n)
{
    const char *p = (const char *)s;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return -1;      /* EPIPE: solver gone */
    }
    return 0;
}

/* Path of the satshim helper: $SATEXT_SHIM, else next to the picat
   binary (via /proc/self/exe), else bare "satshim" from PATH. */
static const char *shim_path(void)
{
    static const char *p = NULL;
    const char *e;
    char exepath[4096];
    ssize_t n;

    if (p != NULL) return p;
    e = getenv("SATEXT_SHIM");
    if (e && *e) { p = e; return p; }
    n = readlink("/proc/self/exe", exepath, sizeof(exepath) - 1);
    if (n > 0) {
        char *sl;
        exepath[n] = 0;
        sl = strrchr(exepath, '/');
        if (sl != NULL) {
            char *cand;
            *(sl + 1) = 0;
            cand = (char *)malloc(strlen(exepath) + 8 + 1);
            if (cand != NULL) {
                sprintf(cand, "%ssatshim", exepath);
                if (access(cand, X_OK) == 0) { p = cand; return p; }
                free(cand);
            }
        }
    }
    p = "satshim";     /* last resort: search PATH */
    return p;
}

/* Generate a DIMACS file under the tmp dir; returns malloc'd path. */
static char *write_cnf_file(const cnf_t *c)
{
    static const char *dir = NULL;
    const char *e;
    char *path;
    size_t plen;
    int fd;
    FILE *f;
    sb_t b;
    const char *dirp;

    if (dir == NULL) {
        e = getenv("SATEXT_TMPDIR");
        if (e && *e) dir = e;
        else {
            struct stat st;
            if (stat("/dev/shm", &st) == 0) dir = "/dev/shm";
            else dir = "/tmp";
        }
    }
    dirp = dir;
    plen = strlen(dirp) + 40;
    path = (char *)malloc(plen);
    if (path == NULL) return NULL;
    snprintf(path, plen, "%s/satext-cnf-%u-%u.cnf", dirp,
             (unsigned)getpid(), (unsigned)(getpid() ^ 0x9E3779B9u));
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { free(path); return NULL; }
    b.b = NULL; b.len = 0; b.cap = 0;
    if (!render_dimacs(c, &b)) { close(fd); (void)unlink(path);
                                  free(b.b); free(path); return NULL; }
    {
        const char *s = b.b;
        size_t n = b.len;
        int ok = 1;
        while (n > 0) {
            ssize_t r = write(fd, s, n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { ok = 0; break; }
            s += r;
            n -= (size_t)r;
        }
        if (close(fd) != 0) ok = 0;
        if (!ok) { (void)unlink(path); free(b.b); free(path); return NULL; }
    }
    free(b.b);
    return path;
}

/* Run the solver and drain its output into o.
   mode: 0 = direct (VM writes CNF text to the stdin pipe)
         1 = shim   (binary CNF in memfd over SCM_RIGHTS to satshim)
   Returns the solver exit status, or -1 on spawn failure. */
static int run_solver(spec_t *s, const cnf_t *c, int proto, int mode,
                      solve_out_t *o)
{
    int sv[2] = { -1, -1 };
    int outp[2] = { -1, -1 };
    int stdinp[2] = { -1, -1 };
    int memfd = -1;
    char *cnf_file = NULL;
    pid_t pid = -1;
    int st = 0;
    int rc = -1;
    int open_max;

    o->status = 0;
    o->model = NULL;
    o->model_len = 0;
    o->have_model = 0;

    open_max = (int)sysconf(_SC_OPEN_MAX);
    if (open_max <= 0) open_max = 1024;
    if (open_max > 65536) open_max = 65536;

    if (mode == 1) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) goto done;
        if (pipe(outp) != 0) goto done;
        memfd = memfd_create("satext-cnf", 0);
        if (memfd < 0) goto done;
        {
            char hdr[28];
            int32_t magic = 0x46415049;
            int64_t ncl = (int64_t)c->nclauses;
            int64_t nv = (int64_t)c->maxvar;
            int64_t nl = (int64_t)c->nlits;
            memcpy(hdr + 0, &magic, 4);
            memcpy(hdr + 4, &ncl, 8);
            memcpy(hdr + 12, &nv, 8);
            memcpy(hdr + 20, &nl, 8);
            if (write_all(memfd, hdr, sizeof(hdr)) != 0) goto done;
            if (write_all(memfd, c->lits, c->nlits * sizeof(int32_t))
                != 0)
                goto done;
            if (lseek(memfd, 0, SEEK_SET) < 0) goto done;
        }
    } else {
        if (s->atfile >= 0) {
            cnf_file = write_cnf_file(c);
            if (cnf_file == NULL) goto done;
        }
        if (pipe(stdinp) != 0) goto done;
        if (pipe(outp) != 0) goto done;
    }

    pid = fork();
    if (pid < 0) goto done;
    if (pid == 0) {
        child_dies_with_parent();
        int i;
        int ndev = -1;
        if (mode == 1) {
            if (sv[1] != SATEXT_SHIM_FD) {
                if (dup2(sv[1], SATEXT_SHIM_FD) < 0) _exit(125);
                close(sv[1]);
            }
            if (outp[1] != SATEXT_OUT_FD) {
                if (dup2(outp[1], SATEXT_OUT_FD) < 0) _exit(125);
                close(outp[1]);
            }
        } else {
            if (s->atfile >= 0) {
                ndev = open("/dev/null", O_RDONLY);
                if (ndev < 0) _exit(125);
                dup2(ndev, 0);
                close(ndev);
            } else {
                if (dup2(stdinp[0], 0) < 0) _exit(125);
            }
            if (dup2(outp[1], 1) < 0) _exit(125);
        }
        for (i = 3; i < open_max; i++) {
            if (mode == 1 && i == SATEXT_SHIM_FD) continue;
            if (mode == 1 && i == SATEXT_OUT_FD) continue;
            close(i);
        }
        signal(SIGPIPE, SIG_IGN);
        {
            char **av;
            int a, na;
            const char *sh;
            if (mode == 1) {
                /* [shim, bin, <ipasir>, <fd>, solver, args...] */
                na = s->argc + 4;
                av = (char **)malloc(sizeof(char *) * (size_t)(na + 1));
                if (av == NULL) _exit(125);
                sh = shim_path();
                av[0] = (sh != NULL) ? (char *)sh : (char *)"satshim";
                av[1] = "bin";
                av[2] = (proto == PROTO_IPASIR) ? (char *)"1" : (char *)"0";
                av[3] = "7";
                for (a = 0; a < s->argc; a++)
                    av[4 + a] = s->argv[a];
                av[na] = NULL;
                execvp(av[0], av);
            } else {
                na = s->argc;
                av = (char **)malloc(sizeof(char *) * (size_t)(na + 1));
                if (av == NULL) _exit(125);
                for (a = 0; a < s->argc; a++)
                    av[a] = (a == s->atfile) ? cnf_file : s->argv[a];
                av[na] = NULL;
                execvp(av[0], av);
            }
            _exit(127);
        }
    }

    /* parent */
    if (mode == 1) {
        struct msghdr m;
        struct iovec io;
        char one = 0;
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr *cm;
        ssize_t sr;

        close(sv[1]);
        close(outp[1]);
        memset(&m, 0, sizeof(m));
        memset(&io, 0, sizeof(io));
        memset(cbuf, 0, sizeof(cbuf));
        io.iov_base = &one;
        io.iov_len = 1;
        m.msg_iov = &io;
        m.msg_iovlen = 1;
        m.msg_control = cbuf;
        m.msg_controllen = sizeof(cbuf);
        cm = (struct cmsghdr *)cbuf;
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        *(int *)CMSG_DATA(cm) = memfd;
        sr = sendmsg(sv[0], &m, 0);
        close(sv[0]);
        close(memfd);
        memfd = -1;
        if (sr < 0) {
            /* shim cannot start properly; let the wait below report */
        }
    } else {
        sb_t text;
        int ok;

        close(stdinp[0]);
        close(outp[1]);
        text.b = NULL; text.len = 0; text.cap = 0;
        if (s->atfile < 0) {
            ok = (proto == PROTO_IPASIR) ? render_ipasir(c, &text)
                                         : render_dimacs(c, &text);
            if (ok) ok = (write_all(stdinp[1], text.b, text.len) == 0);
        }
        if (text.b) free(text.b);
        close(stdinp[1]);
        stdinp[1] = -1;
    }

    drain_output(outp[0], (uint64_t)c->maxvar, proto, o);
    outp[0] = -1;
    if (waitpid(pid, &st, 0) < 0) { rc = -1; goto done; }
    if (WIFEXITED(st)) {
        rc = WEXITSTATUS(st);
        if (o->status == 0) {
            if (WEXITSTATUS(st) == 10) o->status = 1;
            else if (WEXITSTATUS(st) == SATEXT_UNSAT_EXIT) o->status = 2;
        }
    }
done:
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    if (stdinp[0] >= 0) close(stdinp[0]);
    if (stdinp[1] >= 0) close(stdinp[1]);
    if (outp[0] >= 0) close(outp[0]);
    if (outp[1] >= 0) close(outp[1]);
    if (memfd >= 0) close(memfd);
    if (cnf_file != NULL) { (void)unlink(cnf_file); free(cnf_file); }
    return rc;
}

/* ------------------------------------------------------------------
 * selected external solver for the picat `import sat` flow
 * (set via $solver(Spec) in solve(Options,Vars) or SATEXT_SOLVER)
 * ---------------------------------------------------------------- */

static spec_list_t g_sl;
static int g_sl_valid = 0;

/* env-var selection, cached per value; the in-program selection (g_sl)
   takes precedence over it */
static char *env_key = NULL;
static spec_list_t env_sl;
static int env_sl_valid = 0;

void satext_solver_clear(void)
{
    if (g_sl_valid) spec_list_fin(&g_sl);
    g_sl_valid = 0;
}

static int satext_active_list(spec_list_t **slp)
{
    if (g_sl_valid) {
        *slp = &g_sl;
        return 1;
    }
    {
        const char *e = getenv("SATEXT_SOLVER");
        if (e && *e) {
            if (!env_sl_valid || env_key == NULL || strcmp(env_key, e) != 0) {
                spec_list_fin(&env_sl);
                env_sl_valid = 0;
                if (spec_list_from_cstr(e, &env_sl)) {
                    if (env_key != NULL) free(env_key);
                    env_key = str_dup(e);
                    env_sl_valid = (env_key != NULL) ? 1 : 0;
                }
            }
            if (env_sl_valid) {
                *slp = &env_sl;
                return 1;
            }
        }
    }
    return 0;
}

/* 1 if an external solver is selected and usable */
int satext_ext_prepare(void)
{
    spec_list_t *sl;

    if (!satext_active_list(&sl)) return 0;
    return (sl->n >= 1) ? 1 : 0;
}

static int pick_mode(spec_t *s, const cnf_t *c, int proto)
{
    long long minb = SATEXT_SHIM_MIN_BYTES;
    const char *e;
    unsigned long long est;

    (void)proto;
    e = getenv("SATEXT_SHIM_MIN");
    if (e && *e) minb = (long long)strtoll(e, NULL, 10);
    est = (unsigned long long)c->nlits * 8ULL + 64ULL;
    if (s->atfile < 0 && est > (unsigned long long)minb &&
        shim_path() != NULL)
        return 1;
    return 0;
}

static int ext_res_status = 0;
static int32_t *ext_res_model = NULL;
static int64_t ext_res_model_len = 0;

static void ext_res_free(void)
{
    if (ext_res_model != NULL) {
        free(ext_res_model);
        ext_res_model = NULL;
    }
    ext_res_model_len = 0;
    ext_res_status = 0;
}

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static long long prt_budget_ms(void)
{
    const char *e = getenv("SATEXT_PRT_BUDGET_MS");
    long long v = SATEXT_PRT_BUDGET_MS;
    if (e && *e) {
        v = strtoll(e, NULL, 10);
        if (v < 0) v = 0;
    }
    return v;
}

static unsigned long long prt_min_bytes(void)
{
    const char *e = getenv("SATEXT_PRT_MIN");
    unsigned long long v = SATEXT_PRT_MIN_BYTES;
    if (e && *e) {
        long long t = strtoll(e, NULL, 10);
        if (t >= 0) v = (unsigned long long)t;
    }
    return v;
}

static int prt_stats(void)
{
    const char *e = getenv("SATEXT_PRT_STATS");
    return (e != NULL && *e != 0) ? 1 : 0;
}

static void prt_solver_name(spec_t *s, char *buf, size_t cap)
{
    int i;
    size_t l = 0;
    buf[0] = 0;
    for (i = 0; i < s->argc && l + (size_t)s->argc < cap; i++) {
        size_t tl = strlen(s->argv[i]);
        if (l + tl + 2 >= cap) break;
        if (i > 0) buf[l++] = ' ';
        memcpy(buf + l, s->argv[i], tl);
        l += tl;
    }
    buf[l] = 0;
}

/* Run one solver spec on the mirrored g_cnf; store the result in
   ext_res_*. 0 on any completed run (status may still be "unknown"),
   -1 if the run itself failed (caller must fall back). */
static int run_single(spec_t *sp, int proto)
{
    int mode;
    solve_out_t o;

    mode = pick_mode(sp, &g_cnf, proto);
    if (run_solver(sp, &g_cnf, proto, mode, &o) == -1) return -1;
    ext_res_status = o.status;
    ext_res_model = o.model;
    ext_res_model_len = (int64_t)o.model_len;
    return 0;
}

/* Run spec i of sl as a child process; the child reports
   [kind i32, nvar i32, model i32[nvar]] on the pipe (kind: 0 no
   decisive result, 2 unsat, 3 sat). The child starts a new session so
   the supervisor can kill the child plus its solver descendant as one
   process group. */
static pid_t prt_fork_runner(spec_list_t *sl, int i, const int *rfd)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int j;
        int wfd = rfd[2 * i + 1];

        child_dies_with_parent();
        for (j = 0; j < sl->n; j++)
            if (j != i) { close(rfd[2 * j]); close(rfd[2 * j + 1]); }
        close(rfd[2 * i]);
        if (setsid() == (pid_t)-1) _exit(126);
        signal(SIGPIPE, SIG_IGN);
        {
            spec_t *sp = &sl->specs[i];
            solve_out_t o;
            int kind, rc;
            int32_t kind32, nv = (int32_t)g_cnf.maxvar;

            memset(&o, 0, sizeof(o));
            rc = run_solver(sp, &g_cnf, sl->proto[i],
                            pick_mode(sp, &g_cnf, sl->proto[i]), &o);
            /* rc is the solver's exit status (10 = sat, 20 = unsat by
               SAT convention); only -1 (fork/exec failure) is fatal,
               the status is authoritative from the parsed output */
            if (rc == -1)
                kind = 0;
            else if (o.status == 2)
                kind = 2;
            else if (o.status == 1 && o.model != NULL &&
                     o.model_len == g_cnf.maxvar)
                kind = 3;
            else
                kind = 0;
            kind32 = (int32_t)kind;
            if (kind == 3)
                (void)(write_all(wfd, &kind32, 4) == 0 &&
                       write_all(wfd, &nv, 4) == 0 &&
                       write_all(wfd, o.model,
                                 (size_t)nv * sizeof(int32_t)) == 0);
            else
                (void)(write_all(wfd, &kind32, 4) == 0 &&
                       write_all(wfd, &nv, 4) == 0);
        }
        _exit(0);
    }
    return pid;
}

/* First-wins portfolio: race sl->specs[0..n-1] on the mirrored g_cnf.
   Stores the winner's result in ext_res_*; if no decisive answer
   arrives within the wall budget, leaves ext_res_status = 0 so the
   caller falls back to the built-in solver. */
static int run_portfolio(spec_list_t *sl)
{
    int n = sl->n;
    int rfd[SATEXT_PRT_MAX * 2];
    pid_t pid[SATEXT_PRT_MAX] = { 0 };
    uint64_t t0[SATEXT_PRT_MAX] = { 0 }, tw;
    int done[SATEXT_PRT_MAX] = { 0 };
    int i, alive;
    long long budget = prt_budget_ms();
    int stats = prt_stats();
    int32_t *model = NULL;
    int win_kind = 0, win_i = -1;
    int have = 0, rc = 0;

    for (i = 0; i < n; i++) {
        if (pipe(&rfd[2 * i]) != 0) { rc = -1; break; }
        pid[i] = prt_fork_runner(sl, i, rfd);
        t0[i] = now_ms();
        close(rfd[2 * i + 1]);
        if (pid[i] < 0) {
            close(rfd[2 * i]);
            rc = -1;
            break;
        }
    }
    if (rc == -1) {
        for (i = 0; i < n; i++)
            if (pid[i] > 0) {
                kill(-pid[i], SIGKILL);
                (void)waitpid(pid[i], NULL, 0);
            }
        return -1;
    }

    memset(done, 0, sizeof(done));
    alive = n;
    for (;;) {
        struct pollfd pf[SATEXT_PRT_MAX];
        int nrf = 0, pr;
        int timeout = -1;

        if (budget > 0) {
            long long rem = (long long)(t0[0] + budget - now_ms());
            if (rem <= 0) break;
            timeout = (rem > 1000000000LL) ? 1000000000 : (int)rem;
        }
        for (i = 0; i < n; i++) {
            if (done[i]) continue;
            pf[nrf].fd = rfd[2 * i];
            pf[nrf].events = POLLIN;
            pf[nrf].revents = 0;
            nrf++;
        }
        if (nrf == 0) break;
        pr = poll(pf, nrf, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) break;          /* budget elapse */
        for (i = 0; i < n && !have; i++) {
            int prf = -1, k;
            for (k = 0; k < nrf; k++)
                if (pf[k].fd == rfd[2 * i]) prf = k;
            if (prf < 0 || (pf[prf].revents & (POLLIN | POLLHUP)) == 0)
                continue;
            {
                uint8_t hdr[8];             /* kind i32, nvar i32 */
                int32_t kind, nv;
                size_t got = 0;

                while (got < 8) {
                    ssize_t r = read(rfd[2 * i], hdr + got, 8 - got);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        r = 0;
                    }
                    if (r == 0) break;      /* runner died, no result */
                    got += (size_t)r;
                }
                if (got < 8) {
                    done[i] = 1; alive--;
                    continue;
                }
                memcpy(&kind, hdr, 4);
                memcpy(&nv, hdr + 4, 4);
                if (kind == 3) {
                    model = (int32_t *)malloc((size_t)nv * sizeof(int32_t));
                    got = 0;
                    while (model != NULL &&
                           got < (size_t)nv * sizeof(int32_t)) {
                        ssize_t r = read(rfd[2 * i],
                                          model + got / sizeof(int32_t),
                                          (size_t)nv * sizeof(int32_t) - got);
                        if (r < 0) {
                            if (errno == EINTR) continue;
                            r = 0;
                        }
                        if (r == 0) { model = NULL; break; }
                        got += (size_t)r;
                    }
                    if (model != NULL && (uint64_t)nv == g_cnf.maxvar) {
                        win_kind = 3;
                        win_i = i;
                        have = 1;
                        done[i] = 1;
                    } else {
                        if (model != NULL) free(model);
                        model = NULL;
                        done[i] = 1; alive--;
                    }
                } else if (kind == 2) {
                    win_kind = 2;
                    win_i = i;
                    have = 1;
                    done[i] = 1;
                } else {
                    done[i] = 1; alive--;
                }
            }
        }
        if (have || alive == 0) break;
    }
    for (i = 0; i < n; i++)
        close(rfd[2 * i]);
    tw = now_ms();

    for (i = 0; i < n; i++) {
        if (!done[i]) kill(-pid[i], SIGKILL);
        (void)waitpid(pid[i], NULL, 0);
    }

    if (stats) {
        char nm[SATEXT_PRT_MAX][160];
        fprintf(stderr, "satext: portfolio (%s):",
                (have ? (win_kind == 3) ? "sat" : "unsat" : "no result"));
        for (i = 0; i < n; i++) {
            prt_solver_name(&sl->specs[i], nm[i], sizeof(nm[i]));
            fprintf(stderr, " %s %llums%s", nm[i],
                    (unsigned long long)(tw - t0[i]),
                    (have && i == win_i) ? " WIN" : "");
        }
        fprintf(stderr, "\n");
    }

    if (!have) return 0;   /* ext_res_status stays 0: built-in takes over */
    ext_res_status = (win_kind == 3) ? 1 : 2;
    ext_res_model = model;
    ext_res_model_len = (model != NULL) ? (int64_t)g_cnf.maxvar : 0;
    return 0;
}

/* Run the selected external solver (or portfolio) on the mirrored
   g_cnf. */
int satext_ext_run(void)
{
    spec_list_t *sl;
    unsigned long long est;

    ext_res_free();
    if (!satext_active_list(&sl)) return -1;
    if (g_cnf.nclauses == 0) { ext_res_status = 1; return 0; }
    est = (unsigned long long)g_cnf.nlits * 8ULL + 64ULL;
    if (sl->n == 1 || est <= prt_min_bytes())
        return run_single(&sl->specs[0], sl->proto[0]);
    return run_portfolio(sl);
}

int satext_ext_status(void)
{
    return ext_res_status;
}

/* 1 iff SATEXT_NO_FALLBACK is set non-empty: when the external
   solver(s) return "unknown" (e.g. the portfolio wall budget
   elapsed), the caller must not run the built-in solver; the solve
   then fails. Spawn/transfer failures (satext_ext_run returning -1)
   always fall back, whatever this is. */
int satext_no_fallback(void)
{
    const char *e = getenv("SATEXT_NO_FALLBACK");
    return (e != NULL && *e != 0) ? 1 : 0;
}

/* How the most recent solve(Vars) call was resolved, readable from
   the Picat level via c_satext_last_status(St):
     1 = answered SAT (by the external solver or the built-in)
     2 = answered UNSAT (by the external solver or the built-in)
     0 = unknown/abandoned: the external solver(s) produced no
         decisive answer (e.g. the wall budget elapsed) and
         SATEXT_NO_FALLBACK suppressed the built-in fallback, so the
         solve failed WITHOUT a verdict (it is NOT an UNSAT result).
   c_sat_start records the outcome; the value is stable afterwards. */
static int ext_last_status = 0;

void satext_record_result(int st)
{
    ext_last_status = st;
}

int c_satext_last_status(void)
{
    BPLONG St = ARG(1, 1);

    return unify(St, MAKEINT((BPLONG)ext_last_status));
}

int satext_ext_model_value(int varnum)
{
    int a = varnum > 0 ? varnum : -varnum;
    if (ext_res_model == NULL || a < 1 || a > (int)ext_res_model_len)
        return 0;
    return ext_res_model[a - 1] ? 1 : -1;
}

/* ------------------------------------------------------------------
 * cpreds
 * ---------------------------------------------------------------- */

int c_satext_set_solver()
{
    BPLONG t = ARG(1, 1);

    DEREF(t);
    if (t == nil_sym || t == f_atom) {
        satext_solver_clear();
        return BP_TRUE;
    }
    {
        spec_list_t ns;

        if (!parse_spec_list(t, &ns)) return BP_FALSE;
        satext_solver_clear();
        g_sl = ns;
        g_sl_valid = 1;
        return BP_TRUE;
    }
}

int c_satext_solve()
{
    BPLONG specarg = ARG(1, 4), clauses = ARG(2, 4);
    BPLONG statusr = ARG(3, 4), modelr = ARG(4, 4);
    spec_t spec1;
    cnf_t c;
    solve_out_t o;
    int proto, mode;
    BPLONG status, model;

    if (!parse_spec(specarg, &spec1)) return BP_FALSE;
    if (!collect_cnf(clauses, &c)) { spec_free(&spec1); return BP_FALSE; }
    if (c.nclauses == 0) {
        /* no clauses: trivially satisfied, empty model */
        spec_free(&spec1);
        cnf_fin(&c);
        return unify(statusr, ADDTAG(BP_NEW_SYM("sat", 0), ATM)) &&
               unify(modelr, nil_sym);
    }
    proto = detect_protocol(spec1.exe, spec1.base);
    mode = pick_mode(&spec1, &c, proto);
    if (proto == PROTO_NONE) {
        spec_free(&spec1);
        cnf_fin(&c);
        return unify(statusr, ADDTAG(BP_NEW_SYM("unknown", 0), ATM)) &&
               unify(modelr, nil_sym);
    }
    if (run_solver(&spec1, &c, proto, mode, &o) == -1) {
        spec_free(&spec1);
        cnf_fin(&c);
        if (o.model) free(o.model);
        return BP_ERROR;
    }
    {
        BPLONG n = (BPLONG)c.maxvar;
        BPLONG status2;
        if (ensure_heap_room(2L * n + 32, &modelr, 4, 4) != 0) {
            spec_free(&spec1);
            cnf_fin(&c);
            if (o.model) free(o.model);
            return BP_ERROR;
        }
        status2 = (o.status == 1) ? ADDTAG(BP_NEW_SYM("sat", 0), ATM)
                : (o.status == 2) ? ADDTAG(BP_NEW_SYM("unsat", 0), ATM)
                : ADDTAG(BP_NEW_SYM("unknown", 0), ATM);
        status = status2;
        model = (o.have_model && o.model != NULL)
            ? bools_to_list(o.model, n) : nil_sym;
        spec_free(&spec1);
        cnf_fin(&c);
        if (o.model) free(o.model);
        return unify(statusr, status) && unify(modelr, model);
    }
}

int c_satext_cnf_info()
{
    BPLONG clauses = ARG(1, 4);
    BPLONG nv = ARG(2, 4), nc = ARG(3, 4), nl = ARG(4, 4);
    cnf_t c;

    if (!collect_cnf(clauses, &c)) return BP_FALSE;
    if (ensure_heap_room(64, &nv, 4, 2) != 0) { cnf_fin(&c); return BP_ERROR; }
    {
        int ok;
        nv = ARG(2, 4);
        nc = ARG(3, 4);
        nl = ARG(4, 4);
        ok = unify(nv, MAKEINT((BPLONG)c.maxvar)) &&
             unify(nc, MAKEINT((BPLONG)c.nclauses)) &&
             unify(nl, MAKEINT((BPLONG)c.nlits));
        cnf_fin(&c);
        return ok;
    }
}

int c_satext_write_dimacs()
{
    BPLONG clauses = ARG(1, 2), path = ARG(2, 2);
    cnf_t c;
    sb_t b;
    char *cs;
    FILE *f;
    int rc;

    DEREF(path);
    if (!term_to_cstr(path, &cs)) return BP_FALSE;
    if (!collect_cnf(clauses, &c)) { free(cs); return BP_FALSE; }
    b.b = NULL; b.len = 0; b.cap = 0;
    if (!render_dimacs(&c, &b)) { free(cs); cnf_fin(&c); return BP_ERROR; }
    f = fopen(cs, "wb");
    if (f == NULL) { free(cs); free(b.b); cnf_fin(&c); return BP_FALSE; }
    rc = (b.len == 0 || fwrite(b.b, 1, b.len, f) == b.len) ? BP_TRUE
                                                           : BP_FALSE;
    (void)fclose(f);
    free(cs);
    free(b.b);
    cnf_fin(&c);
    return rc;
}

void Cboot_satext(void)
{
    signal(SIGPIPE, SIG_IGN);
    insert_cpred("c_satext_solve", 4, c_satext_solve);
    insert_cpred("c_satext_cnf_info", 4, c_satext_cnf_info);
    insert_cpred("c_satext_write_dimacs", 2, c_satext_write_dimacs);
    insert_cpred("c_satext_set_solver", 1, c_satext_set_solver);
    insert_cpred("c_satext_last_status", 1, c_satext_last_status);
}
