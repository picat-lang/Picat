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
 *                       ("@file" token supported); nil/false clears
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
 *
 *            Environment:
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
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "term.h"
#include "basic.h"
#include "bapi.h"

#define SATEXT_UNSAT_EXIT 20
#define SATEXT_SHIM_FD    3    /* fd carrying the memfd in the shim child */
#define SATEXT_OUT_FD     7    /* fd the solver's stdout is dup'd onto */
#define SATEXT_SHIM_MIN_BYTES (4L << 20)

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
    if (strncmp(base, "cadical", 7) == 0) return PROTO_IPASIR;
    if (strncmp(base, "kissat", 6) == 0 ||
        strncmp(base, "minisat", 7) == 0 ||
        strncmp(base, "picosat", 7) == 0 ||
        strncmp(base, "glucose", 7) == 0 ||
        strncmp(base, "cryptominisat", 13) == 0 ||
        strncmp(base, "maplechrono", 11) == 0 ||
        strncmp(base, "lingeling", 9) == 0)
        return PROTO_DIMACS;
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
        "s 3 3\n1 2 0\n-1 3 0\n-1 -2 0\nsolve\n";
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
        char *prog = str_dup(exe);
        dup2(p[0], 0);
        dup2(q[1], 1);
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

static int render_clauses(const cnf_t *c, sb_t *b)
{
    uint64_t off;
    for (off = 0; off < c->nlits; off++) {
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
    return render_clauses(c, b);
}

static int render_ipasir(const cnf_t *c, sb_t *b)
{
    char h[64];
    int hl = snprintf(h, sizeof(h), "s %llu %llu\n",
                      (unsigned long long)c->maxvar,
                      (unsigned long long)c->nclauses);
    if (hl < 0) return 0;
    if (!sb_put(b, h, (size_t)hl)) return 0;
    if (!render_clauses(c, b)) return 0;
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

static spec_t g_spec;
static int g_spec_valid = 0;
static int g_proto = PROTO_NONE;

/* env-var fallback, cached per value */
static char *env_key = NULL;
static spec_t env_spec;
static int env_spec_valid = 0;
static int env_proto = PROTO_NONE;

static int spec_from_cstr(const char *str, spec_t *s)
{
    char *cs = str_dup(str);
    if (cs == NULL) return 0;
    s->argv = (char **)malloc(2 * sizeof(char *));
    if (s->argv == NULL) { free(cs); return 0; }
    s->argv[0] = cs;
    s->argv[1] = NULL;
    s->argc = 1;
    s->atfile = -1;
    s->exe = cs;                     /* alias, freed with argv */
    s->base = str_dup(base_name(cs));
    return (s->base != NULL);
}

void satext_solver_clear(void)
{
    if (g_spec_valid) spec_free(&g_spec);
    g_spec_valid = 0;
    g_proto = PROTO_NONE;
}

static int satext_active_spec(spec_t **sp, int *proto)
{
    if (g_spec_valid) {
        *sp = &g_spec;
        *proto = g_proto;
        return 1;
    }
    {
        const char *e = getenv("SATEXT_SOLVER");
        if (e && *e) {
            if (!env_spec_valid || env_key == NULL || strcmp(env_key, e) != 0) {
                if (spec_from_cstr(e, &env_spec)) {
                    if (env_key != NULL) free(env_key);
                    env_key = str_dup(e);
                    env_spec_valid = (env_key != NULL) ? 1 : 0;
                    env_proto = env_spec_valid
                        ? detect_protocol(env_spec.exe, env_spec.base)
                        : PROTO_NONE;
                }
            }
            if (env_spec_valid) {
                *sp = &env_spec;
                *proto = env_proto;
                return 1;
            }
        }
    }
    return 0;
}

/* 1 if an external solver is selected and usable */
int satext_ext_prepare(void)
{
    spec_t *sp;
    int proto;

    if (!satext_active_spec(&sp, &proto)) return 0;
    if (proto == PROTO_NONE) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "satext: cannot detect the protocol of \"%s\"; "
                    "using the built-in solver\n", sp->exe);
        }
        return 0;
    }
    return 1;
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

/* Run the selected external solver on the mirrored g_cnf. */
int satext_ext_run(void)
{
    spec_t *sp;
    int proto, mode;
    solve_out_t o;

    ext_res_free();
    if (!satext_active_spec(&sp, &proto)) return -1;
    if (g_cnf.nclauses == 0) { ext_res_status = 1; return 0; }
    mode = pick_mode(sp, &g_cnf, proto);
    if (run_solver(sp, &g_cnf, proto, mode, &o) == -1) return -1;
    ext_res_status = o.status;
    ext_res_model = o.model;
    ext_res_model_len = (int64_t)o.model_len;
    return 0;
}

int satext_ext_status(void)
{
    return ext_res_status;
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
        spec_t ns;
        int pr;

        if (!parse_spec_term(t, &ns)) return BP_FALSE;
        pr = detect_protocol(ns.exe, ns.base);
        satext_solver_clear();
        g_spec = ns;
        g_spec_valid = 1;
        g_proto = pr;
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
}
