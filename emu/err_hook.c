/*
 * err_hook.c -- error diagnostics for debugging support.
 *
 * Hook points, all plain C (no interpreter re-entry, safe at every call
 * site):
 *
 *   err_save_main_file(path)      called from init_loading (init.c); remembers
 *                                 the first .pi argument for the exit lint and
 *                                 starts stderr capture (see below).
 *   err_didyoumean(name, arity)   called from the toam call path (emu_inst.h)
 *                                 for symbols whose call raises the
 *                                 undefined-procedure exception directly;
 *                                 prints the nearest defined predicate by
 *                                 Levenshtein distance.
 *   err_lint_main_file()          registered with atexit() in main.c.  At
 *                                 exit it (0) restores stderr and replays
 *                                 what the run wrote there (the blob prints
 *                                 its own messages to a temp file so that the
 *                                 run can be inspected afterwards), (1)
 *                                 reports undefined procedures found in the
 *                                 replayed output with their line in the main
 *                                 file and a did-you-mean suggestion, and
 *                                 (2) re-reads the saved main file and
 *                                 reports unbalanced brackets and keyword
 *                                 typos (skipping comments and strings).
 *                                 Silent when there is nothing to report.
 *
 * stderr capture: the compiled blob prints "Undefined procedure: X/N" and
 * exits before any C code we own runs, and it clears bp_exception, so the
 * name only survives as text on stderr.  err_save_main_file dup2()s stderr
 * to a temp file (created in $TMPDIR or /tmp) and the exit hook replays it,
 * so the user still sees all output -- stderr simply appears at exit rather
 * than live.  Set PICAT_NO_ERRHOOK to skip the capture and the lint.
 *
 * Trade-off: stderr is buffered until exit.  For error cases (the target)
 * the process exits right after the error, so the UX is unchanged; for
 * normal runs stderr messages (compile warnings, trace output) appear after
 * stdout instead of interleaved with it.  A temp file is used instead of a
 * pipe so that large stderr output can never deadlock the run.
 *
 * Compiled into the native build only (-DERRHOOK via Makefile.linux64); the
 * wasm build compiles the call sites out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "basic.h"
#include "term.h"
#include "bapi.h"
#include "extern_decl.h"

static char err_main_file[4096] = "";
static int err_real_stderr_fd = -1;
static int err_misleading_seen = 0;
static char err_stderr_path[512] = "";

static void err_capture_stderr(void);

void err_save_main_file(const char *path)
{
    if (path == NULL || path[0] == '\0') return;
    strncpy(err_main_file, path, sizeof(err_main_file) - 1);
    err_main_file[sizeof(err_main_file) - 1] = '\0';
    err_capture_stderr();
}

/* dup2 stderr to a temp file so the run's stderr output survives to exit */
static void err_capture_stderr(void)
{
    const char *dir;
    char path[512];
    int fd;

    if (getenv("PICAT_NO_ERRHOOK")) return;
    if (err_stderr_path[0] != '\0') return;            /* already capturing */
    dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0' || strlen(dir) > 400) dir = "/tmp";
    snprintf(path, sizeof(path), "%s/picat_err_XXXXXX", dir);
    fd = mkstemp(path);
    if (fd < 0) return;
    err_real_stderr_fd = dup(2);
    if (err_real_stderr_fd < 0) { close(fd); unlink(path); return; }
    fflush(stderr);
    if (dup2(fd, 2) != 2) {      /* stderr now writes into the temp file */
        dup2(err_real_stderr_fd, 2);
        close(err_real_stderr_fd); err_real_stderr_fd = -1;
        close(fd); unlink(path); return;
    }
    close(fd);                   /* fd 2 keeps the file open */
    strncpy(err_stderr_path, path, sizeof(err_stderr_path) - 1);
    err_stderr_path[sizeof(err_stderr_path) - 1] = '\0';
}

/* ---------------------------------------------------------------- */
/* Levenshtein distance, small strings only                          */

static int err_lev(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    int i, j, upper;
    static int row[64];
    if (la == 0) return lb;
    if (lb == 0) return la;
    if (la >= 64 || lb >= 64) return 99;
    upper = (la > lb ? la : lb);
    for (j = 0; j <= lb; j++) row[j] = j;
    for (i = 1; i <= la; i++) {
        int prev = row[0], cur;
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            cur = row[j-1] + 1;
            if (row[j] + 1 < cur) cur = row[j] + 1;
            if (prev + cost < cur) cur = prev + cost;
            prev = row[j];
            row[j] = cur;
        }
        if (row[lb] >= upper) return upper;  /* early exit: cannot beat */
    }
    return row[lb];
}

/* ---------------------------------------------------------------- */
/* Hook 1: undefined procedure at the direct toam throw point        */

void err_didyoumean(const char *name, BPLONG arity)
{
    SYM_REC_PTR sym_ptr;
    int i, best = 99, best_arity = -1;
    const char *best_name = NULL;
    size_t len;

    if (name == NULL || name[0] == '\0' || name[0] == '$') return;
    len = strlen(name);

    for (i = 0; i < BUCKET_CHAIN; ++i) {
        sym_ptr = sym_hash_table[i];
        while (sym_ptr != NULL) {
            switch (GET_ETYPE(sym_ptr)) {
            case T_ORDI: case T_DYNA: case T_INTP: case T_PRED: case C_PRED: {
                const char *nm = GET_NAME(sym_ptr);
                int d = err_lev(name, nm);
                if (d < best) { best = d; best_name = nm; best_arity = (int)GET_ARITY(sym_ptr); }
                break;
            }
            default: break;
            }
            sym_ptr = GET_NEXT(sym_ptr);
        }
    }
    if (best_name == NULL || best == 0 || best > 2 || best > (int)(len / 2) + 1) return;
    fprintf(stderr, "*** did you mean '%s/%d'? (edit distance %d)\n",
            best_name, best_arity, best);
}

/* ---------------------------------------------------------------- */
/* Hook 2: the exit lint                                             */

#define ERR_MAXKW 21
static const char *err_keywords[ERR_MAXKW] = {
    "end", "foreach", "foreach", "while", "do", "then", "else", "elseif",
    "if", "in", "import", "include", "module", "table", "index", "once",
    "external", "define", "fail", "true", "nl"
};

#define ERR_MAXNAMES 65536
static char *err_names[ERR_MAXNAMES];
static int err_nnames = 0;
static const char *err_suppress_name = NULL;
/* err_names/err_collect_names/err_best_name kept for err_didyoumean; the
   exit lint no longer uses them (the symbol table cannot distinguish a
   defined predicate from a merely-referenced one) */

/* collect the currently defined predicate names from the symbol table
   (used only by err_didyoumean at the direct throw point) */
static void err_collect_names(void)
{
    SYM_REC_PTR sym_ptr;
    int i;
    err_nnames = 0;
    for (i = 0; i < BUCKET_CHAIN && err_nnames < ERR_MAXNAMES; ++i) {
        sym_ptr = sym_hash_table[i];
        while (sym_ptr != NULL && err_nnames < ERR_MAXNAMES) {
            switch (GET_ETYPE(sym_ptr)) {
            case T_ORDI: case T_DYNA: case T_INTP: case T_PRED: case C_PRED: {
                const char *nm = GET_NAME(sym_ptr);
                if (nm != NULL && nm[0] != '$' && strlen(nm) < 64) {
                    err_names[err_nnames++] = (char *)nm;
                }
                break;
            }
            default: break;
            }
            sym_ptr = GET_NEXT(sym_ptr);
        }
    }
}

static const char *err_best_name(const char *tok, int *dist_out)
{
    int i, best = 99;
    const char *bn = NULL;
    for (i = 0; i < err_nnames; i++) {
        int d = err_lev(tok, err_names[i]);
        if (d < best) { best = d; bn = err_names[i]; if (best == 0) break; }
    }
    *dist_out = best;
    return bn;
}

/* read the whole file; returns malloc'd buffer or NULL */
static char *err_read_file(const char *path, long *outlen)
{
    FILE *fp = fopen(path, "r");
    long size, got;
    char *buf;
    if (fp == NULL) return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    if (size < 0 || size > 8L * 1024 * 1024) { fclose(fp); return NULL; }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 2);
    if (buf == NULL) { fclose(fp); return NULL; }
    got = (long)fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';
    if (outlen) *outlen = got;
    return buf;
}

/* copy one physical line into out, skipping block comments; updates lineno */
static char *err_next_line(char *p, char *out, int outsize, int *lineno)
{
    int li = 0;
    while (*p != '\0' && *p != '\n') {
        if (p[0] == '/' && p[1] == '*') {              /* block comment */
            char *q = p + 2;
            while (*q != '\0' && !(q[0] == '*' && q[1] == '/')) {
                if (*q == '\n') (*lineno)++;
                q++;
            }
            if (*q == '\0') return NULL;               /* unterminated */
            p = q + 2;
            continue;
        }
        if (li < outsize - 1) out[li++] = *p;
        p++;
    }
    out[li] = '\0';
    if (*p == '\n') { p++; (*lineno)++; }
    return p;
}

/* strip strings, % comments, and quoted atoms from a line in place-ish.
   Multi-line double-quoted strings are tracked: an unterminated " carries
   to the following lines (err_instring), so prose inside a multi-line
   string is not parsed as code.  Each pass resets err_instring to 0. */
#define ERR_LINEBUF 131072
static int err_instring = 0;

static void err_strip_noise(const char *line, char *out, int outsize)
{
    int i = 0, o = 0;
    while (line[i] != '\0' && o < outsize - 1) {
        char c = line[i];
        if (err_instring) {
            if (c == '\\' && line[i+1] != '\0') { i += 2; continue; }
            if (c == '"') { err_instring = 0; i++; continue; }
            i++;
            continue;
        }
        if (c == '%') break;
        if (c >= '0' && c <= '9' && line[i+1] == '\'') {
            /* a character literal 0'A: eat the digit, the quote, and the
               char (or an escape pair) so the quote is not mistaken for
               the start of a quoted atom; an immediately following ' (the
               0'A' pattern) is eaten as well */
            i += 2;
            if (line[i] == '\\' && line[i+1] != '\0') i += 2; else if (line[i] != '\0') i++;
            if (line[i] == '\'') i++;
            out[o++] = ' ';
            continue;
        }
        if (c == '"') {
            err_instring = 1;
            i++;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i+1] != '\0') { i += 2; continue; }
                if (line[i] == '"') { err_instring = 0; i++; break; }
                i++;
            }
            out[o++] = ' ';
            continue;
        }
        if (c == '\'') {
            char q = c;
            i++;
            while (line[i] != '\0' && line[i] != q) {
                if (line[i] == '\\' && line[i+1] != '\0') i++;
                i++;
            }
            if (line[i] == q) i++;
            out[o++] = ' ';
            continue;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
}

/* decode a char-code list culprit to a string.  Handles both shapes:
   [a,u,t,h,...] (char elements) and [104,101,...] (code elements).
   A digit element is treated as a code if it is printable, else the
   digits are kept literally. */
static void err_decode_charlist(const char *cl, char *out, int cap)
{
    int o = 0;
    if (*cl != '[') {
        int n2 = 0;
        while (cl[n2] != '\0' && cl[n2] != ')' && n2 < cap - 1) n2++;
        if (n2 > 0) { memcpy(out, cl, (size_t)n2); out[n2] = '\0'; }
        return;
    }
    cl++;
    while (*cl != '\0' && *cl != ']' && o < cap - 1) {
        if (*cl == ',') { cl++; continue; }
        if (*cl >= '0' && *cl <= '9') {
            int v = 0, nd = 0;
            while (*cl >= '0' && *cl <= '9') { v = v * 10 + (*cl - '0'); cl++; nd++; }
            if (v >= 32 && v < 127) out[o++] = (char)v;
            else if (o + nd < cap - 1) { memcpy(out + o, cl - nd, (size_t)nd); o += nd; }
            continue;
        }
        out[o++] = *cl;
        cl++;
    }
    out[o] = '\0';
}

/* restore stderr, replay what the run wrote there, and extract the first
   "Undefined procedure: NAME/ARITY" message if there is one; also extract
   the first typed error "*** error(TYPE(...),CONTEXT)" */
static void err_dump_stderr(char *undef_name, int namecap, int *undef_arity,
                            int *ttype, char *tculprit, int culprcap)
{
    FILE *fp;
    long size;
    char *sbuf;

    *undef_name = '\0';
    *undef_arity = -1;
    *ttype = 0;
    tculprit[0] = '\0';
    err_misleading_seen = 0;
    if (err_real_stderr_fd < 0) return;
    fflush(stderr);
    if (dup2(err_real_stderr_fd, 2) != 2) return;   /* stderr back to the real fd */
    fp = fopen(err_stderr_path, "r");
    unlink(err_stderr_path);
    err_stderr_path[0] = '\0';
    if (fp == NULL) return;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    if (size > 8L * 1024 * 1024) {                  /* too big to scan: stream it */
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) fwrite(chunk, 1, n, stderr);
        fclose(fp);
        return;
    }
    sbuf = (char *)malloc((size_t)size + 2);
    if (sbuf == NULL) { fclose(fp); return; }
    size = (long)fread(sbuf, 1, (size_t)size, fp);
    fclose(fp);
    sbuf[size] = '\0';
    {
        const char *p = strstr(sbuf, "Undefined procedure: ");
        if (p == NULL) p = strstr(sbuf, "Undefined predicate: ");
        if (p != NULL) {
            const char *q = p + strlen("Undefined procedure: ");
            int n = 0;
            while (q[n] != '\0' && q[n] != '/' && q[n] != '\n' &&
                   q[n] != ' ' && n < namecap - 1) n++;
            if (n > 0 && q[n] == '/') {
                int a = 0;
                memcpy(undef_name, q, (size_t)n);
                undef_name[n] = '\0';
                if (q[n+1] >= '0' && q[n+1] <= '9') a = atoi(q + n + 1);
                *undef_arity = a;
            }
        }
        /* typed errors: extract the culprit for the common shapes */
        {
            const char *e = sbuf;
            const char *c;
            while ((e = strstr(e, "*** error(")) != NULL) {
                const char *t = e + 10;
                if (strncmp(t, "existence_error(module,", 23) == 0) { *ttype = 1; c = t + 23; }
                else if (strncmp(t, "existence_error(source_sink,", 28) == 0) { *ttype = 3; c = t + 28; }
                else if (strncmp(t, "type_error(compound,", 20) == 0) { *ttype = 2; c = t + 20; }
                else if (strncmp(t, "collection_expected(", 20) == 0) { *ttype = 4; c = t + 20; }
                else if (strncmp(t, "number_expected(", 16) == 0) { *ttype = 5; c = t + 16; }
                else if (strncmp(t, "integer_list_expected(", 22) == 0) { *ttype = 6; c = t + 22; }
                else if (strncmp(t, "invalid_constraint_exp,", 23) == 0) { *ttype = 8; c = t + 23; }
                else if (strncmp(t, "string_expected(", 16) == 0) {
                    /* the same char-code list decode as file_not_exist */
                    *ttype = 9;
                    err_decode_charlist(t + 16, tculprit, culprcap);
                    if (tculprit[0] == '\0') *ttype = 0;
                    break;
                }
                else if (strncmp(t, "file_not_exist(", 15) == 0) {
                    /* decode the char-code list [d,i,g,...] to a string */
                    *ttype = 7;
                    err_decode_charlist(t + 15, tculprit, culprcap);
                    if (tculprit[0] == '\0') *ttype = 0;
                    break;
                }
                else { e += 10; continue; }
                {
                    int n2 = 0;
                    while (c[n2] != '\0' && c[n2] != ')' && n2 < culprcap - 1) n2++;
                    if (n2 > 0) { memcpy(tculprit, c, (size_t)n2); tculprit[n2] = '\0'; }
                    else *ttype = 0;
                }
                break;
            }
            /* marker-less syntax errors: the stderr only carries
               "*** error(syntax_error,picat)" (the compiler's own
               dot-notation message is stdout-only and unseen); report
               the digit-dot candidates as a hint */
            if (*ttype == 0 && strstr(sbuf, "error(syntax_error,picat)") != NULL)
                *ttype = 10;
            /* a misleading error: the message blames an operator, a callee,
               or an index operator instead of the unbound variable */
            if (strstr(sbuf, "instantiation_error") != NULL ||
                strstr(sbuf, "Free variable in expression") != NULL ||
                strstr(sbuf, "index_operator") != NULL)
                err_misleading_seen = 1;
        }
    }
    fputs(sbuf, stderr);
    free(sbuf);
}

/* clause registry, shared by the contiguity check and the undefined-
   procedure report (which needs the defined arities and their lines) */
#define ERR_MAXHEADS 512
static struct { char name[64]; int arity; int last_seq; int last_line; int first_line; } err_heads[ERR_MAXHEADS];
static int err_nheads = 0;
static int err_clause_seq = 0;

/* report an undefined procedure: find its line in the main file and the
   nearest keyword or file identifier by edit distance (distance 1, or 2
   for names of length >= 4).  When the name is defined with a different
   arity, list the arities seen.  When the name is not in the file at all,
   point to the closest named predicate's line. */
static void err_report_undef(const char *name, int arity)
{
    char *buf, *p;
    long len;
    int lineno = 1;
    int found_line = -1;
    char cands[256][64];
    int cand_line[256];
    int ncands = 0;
    struct { int ar; int line; } ars[8];
    int nars = 0;
    int k, best = 99;
    char best_name[64] = "";
    int best_line = -1;

    if (name[0] == '\0' || name[0] == '$') return;
    err_instring = 0;
    buf = err_read_file(err_main_file, &len);
    if (buf == NULL) {                    /* call is not in the main file */
        fprintf(stderr, "*** lint: '%s/%d' is undefined.\n", name, arity);
        return;
    }
    p = buf;
    while (p != NULL && *p != '\0') {
        char raw[ERR_LINEBUF], clean[ERR_LINEBUF];
        int curlin = lineno;
        p = err_next_line(p, raw, (int)sizeof(raw), &lineno);
        if (p == NULL) break;
        err_strip_noise(raw, clean, (int)sizeof(clean));
        {
            int i = 0;
            while (clean[i] != '\0') {
                int start, tlen;
                char tok[64];
                int dup = 0, q;
                if (!((clean[i] >= 'a' && clean[i] <= 'z') ||
                      (clean[i] >= 'A' && clean[i] <= 'Z') || clean[i] == '_')) { i++; continue; }
                start = i;
                while ((clean[i] >= 'a' && clean[i] <= 'z') ||
                       (clean[i] >= 'A' && clean[i] <= 'Z') ||
                       (clean[i] >= '0' && clean[i] <= '9') || clean[i] == '_') i++;
                tlen = i - start;
                if (tlen >= (int)sizeof(tok)) continue;
                memcpy(tok, clean + start, (size_t)tlen);
                tok[tlen] = '\0';
                if (clean[start] >= 'A' && clean[start] <= 'Z') continue;  /* variable */
                if (found_line < 0 && strcmp(tok, name) == 0) found_line = curlin;
                for (q = 0; q < ncands; q++)
                    if (strcmp(cands[q], tok) == 0) { dup = 1; break; }
                if (!dup && ncands < 256 && tlen >= 2) {
                    memcpy(cands[ncands], tok, (size_t)tlen + 1);
                    cand_line[ncands] = curlin;
                    ncands++;
                }
            }
        }
    }
    free(buf);
    for (k = 0; k < ERR_MAXKW; k++) {            /* keywords only at distance 1 */
        int d = err_lev(name, err_keywords[k]);
        if (d == 1 && d < best) { best = d; strncpy(best_name, err_keywords[k], sizeof(best_name) - 1); best_name[sizeof(best_name) - 1] = '\0'; best_line = -1; }
    }
    for (k = 0; k < ncands; k++) {               /* file names: distance 1, or 2 for longer names
                                                    when the candidate is a DEFINED predicate */
        int d = err_lev(name, cands[k]);
        int ok = (d == 1);
        if (d == 2 && (int)strlen(name) >= 4) {
            int q;
            for (q = 0; q < err_nheads; q++)
                if (strcmp(err_heads[q].name, cands[k]) == 0) { ok = 1; break; }
        }
        if (ok && d < best) { best = d; strncpy(best_name, cands[k], sizeof(best_name) - 1); best_name[sizeof(best_name) - 1] = '\0'; best_line = cand_line[k]; }
    }
    /* the wrong-arity case: the name is defined, but not with this arity
       (from the clause registry, so calls do not pollute the list) */
    {
        int i, match = 0;
        for (i = 0; i < err_nheads && nars < 8; i++)
            if (strcmp(err_heads[i].name, name) == 0) {
                int seen = 0;
                for (k = 0; k < nars; k++) if (ars[k].ar == err_heads[i].arity) { seen = 1; break; }
                if (!seen) { ars[nars].ar = err_heads[i].arity; ars[nars].line = err_heads[i].first_line; nars++; }
            }
        for (k = 0; k < nars; k++) if (ars[k].ar == arity) { match = 1; break; }
        if (found_line > 0 && nars > 0 && !match) {
            fprintf(stderr, "*** lint: line %d: '%s/%d' is undefined -- '%s' is defined with arity %d (line %d)",
                    found_line, name, arity, name, ars[0].ar, ars[0].line);
            for (k = 1; k < nars; k++)
                fprintf(stderr, ", %d (line %d)", ars[k].ar, ars[k].line);
            fprintf(stderr, "\n");
            return;
        }
    }
    if (found_line > 0) {
        if (best_name[0] != '\0')
            fprintf(stderr, "*** lint: line %d: '%s/%d' is undefined -- did you mean '%s'?\n",
                    found_line, name, arity, best_name);
        else
            fprintf(stderr, "*** lint: line %d: '%s/%d' is undefined here.\n", found_line, name, arity);
    } else {
        if (best_name[0] != '\0') {
            if (best_line > 0)
                fprintf(stderr, "*** lint: '%s/%d' is undefined -- did you mean '%s' (line %d)?\n",
                        name, arity, best_name, best_line);
            else
                fprintf(stderr, "*** lint: '%s/%d' is undefined -- did you mean '%s'?\n", name, arity, best_name);
        } else
            fprintf(stderr, "*** lint: '%s/%d' is undefined.\n", name, arity);
    }
}

/* ---------------------------------------------------------------- */
/* Contiguity check: a predicate's clauses must be grouped together  */

#define ERR_MAXCONTIG 8
static struct { int line; int prev_line; char name[64]; int arity; } err_contig[ERR_MAXCONTIG];
static int err_ncontig = 0;
/* per-NAME contiguity: interleaved different-arity clauses of the same
   name are a common intentional pattern and must not be flagged */
static struct { char name[64]; int last_seq; } err_namecontig[ERR_MAXHEADS];
static int err_nnamecontig = 0;

/* scanner state, carried across lines */
static int err_expect_head = 1;
static int err_in_head = 0;
static int err_h_depth = 0, err_h_commas = 0, err_h_empty = 1;
static int err_h_nest = 0;          /* bracket/brace nesting inside the args */
static char err_h_name[64] = "";
static int err_h_line = 0;
static int err_brack_bad = 0;

static void err_register_clause(const char *name, int arity, int line)
{
    int i;
    for (i = 0; i < err_nheads; i++)
        if (err_heads[i].arity == arity && strcmp(err_heads[i].name, name) == 0) break;
    if (i == err_nheads) {
        if (err_nheads >= ERR_MAXHEADS) return;
        err_nheads++;
        err_heads[i].name[0] = '\0';
        strncpy(err_heads[i].name, name, sizeof(err_heads[i].name) - 1);
        err_heads[i].arity = arity;
        err_heads[i].last_seq = -1;
        err_heads[i].last_line = 0;
        err_heads[i].first_line = line;
    }
    /* per-name contiguity: the same name's clauses (any arity) adjacent */
    {
        int j, name_contig = 0;
        for (j = 0; j < err_nnamecontig; j++)
            if (strcmp(err_namecontig[j].name, name) == 0) break;
        if (j == err_nnamecontig) {
            if (err_nnamecontig < ERR_MAXHEADS) {
                err_nnamecontig++;
                err_namecontig[j].name[0] = '\0';
                strncpy(err_namecontig[j].name, name, sizeof(err_namecontig[j].name) - 1);
                err_namecontig[j].last_seq = -1;
            } else j = 0;
        }
        if (err_namecontig[j].last_seq == err_clause_seq - 1) name_contig = 1;
        err_namecontig[j].last_seq = err_clause_seq;
        if (!name_contig && err_heads[i].last_seq >= 0 &&
            err_heads[i].last_seq != err_clause_seq - 1 &&
            err_ncontig < ERR_MAXCONTIG) {
            err_contig[err_ncontig].line = line;
            err_contig[err_ncontig].prev_line = err_heads[i].last_line;
            err_contig[err_ncontig].name[0] = '\0';
            strncpy(err_contig[err_ncontig].name, name, sizeof(err_contig[err_ncontig].name) - 1);
            err_contig[err_ncontig].arity = arity;
            err_ncontig++;
        }
    }
    err_heads[i].last_seq = err_clause_seq;
    err_heads[i].last_line = line;
    err_clause_seq++;
}

/* report a typed error: locate the culprit in the main file.
   type 1: existence_error(module,M)     -- find the token M
   type 2: type_error(compound,C)        -- find "C[" (the value indexed)
   type 3: existence_error(source_sink,F)-- find F in the raw lines
   type 4: collection_expected(V)        -- foreach lines (candidates)
   type 5: number_expected(V)            -- range lines (candidates)
   type 6: integer_list_expected(V)      -- find V; fallback: :: lines
   type 7: file_not_exist(F)             -- find the decoded F, raw lines */
static void err_report_typed(int type, const char *culprit)
{
    char *buf, *p;
    long len;
    int lineno = 1;
    int found = -1, ncand = 0, fallback = -1;
    char desc[128];

    if (culprit[0] == '\0' && type != 10) return;
    err_instring = 0;
    buf = err_read_file(err_main_file, &len);
    if (buf == NULL) {
        if (type == 10)
            fprintf(stderr, "*** lint: typed error (source not readable).\n");
        else
            fprintf(stderr, "*** lint: '%s' -- typed error (source not readable).\n", culprit);
        return;
    }
    p = buf;
    while (p != NULL && *p != '\0' && found < 0) {
        char raw[ERR_LINEBUF], clean[ERR_LINEBUF];
        int curlin = lineno;
        p = err_next_line(p, raw, (int)sizeof(raw), &lineno);
        if (p == NULL) break;
        err_strip_noise(raw, clean, (int)sizeof(clean));
        if (type == 3 || type == 7 || type == 9) {
            if (strstr(raw, culprit) != NULL) { found = curlin; break; }
            continue;
        }
        if (type == 10) {
            /* dot notation on a non-module: digit '.' name-char */
            int i;
            for (i = 0; clean[i] != '\0' && clean[i+1] != '\0' && clean[i+2] != '\0'; i++) {
                if (clean[i] >= '0' && clean[i] <= '9' && clean[i+1] == '.' &&
                    ((clean[i+2] >= 'a' && clean[i+2] <= 'z') ||
                     (clean[i+2] >= 'A' && clean[i+2] <= 'Z') || clean[i+2] == '_')) {
                    if (ncand < 3) {
                        fprintf(stderr, "*** lint: line %d: possible dot notation on a non-module (candidate)\n",
                                curlin);
                        ncand++;
                    }
                    break;
                }
            }
            continue;
        }
        if (type == 6) {
            if (strstr(clean, culprit) != NULL) { found = curlin; break; }
            if (fallback < 0 && strstr(clean, "::") != NULL) fallback = curlin;
            continue;
        }
        if (type == 4 || type == 5) {
            const char *pat = (type == 4) ? "foreach" : "..";
            if (strstr(clean, pat) != NULL) {
                if (ncand < 3) {
                    fprintf(stderr, "*** lint: line %d: '%s' -- %s (candidate)\n",
                            curlin, culprit, (type == 4) ?
                            "foreach may be over a non-collection" :
                            "range may have a non-number bound");
                    ncand++;
                }
            }
            continue;
        }
        if (type == 2) {
            char pat[128];
            if (strlen(culprit) < sizeof(pat) - 2) {
                snprintf(pat, sizeof(pat), "%s[", culprit);
                if (strstr(clean, pat) != NULL) { found = curlin; break; }
                /* fallback: the line where the culprit value is bound */
                if (fallback < 0 && strstr(clean, culprit) != NULL) fallback = curlin;
            }
            continue;
        }
        /* type 1 and type 8: the culprit as a token */
        {
            int i = 0;
            while (clean[i] != '\0') {
                int start, tlen;
                char tok[128];
                if (!((clean[i] >= 'a' && clean[i] <= 'z') ||
                      (clean[i] >= 'A' && clean[i] <= 'Z') || clean[i] == '_')) { i++; continue; }
                start = i;
                while ((clean[i] >= 'a' && clean[i] <= 'z') ||
                       (clean[i] >= 'A' && clean[i] <= 'Z') ||
                       (clean[i] >= '0' && clean[i] <= '9') || clean[i] == '_') i++;
                tlen = i - start;
                if (tlen >= (int)sizeof(tok)) continue;
                memcpy(tok, clean + start, (size_t)tlen);
                tok[tlen] = '\0';
                if (strcmp(tok, culprit) == 0) { found = curlin; break; }
            }
            if (found > 0) break;
            continue;
        }
    }
    free(buf);
    switch (type) {
    case 1: snprintf(desc, sizeof(desc), "module load failed"); break;
    case 2: snprintf(desc, sizeof(desc), "indexing a non-compound value"); break;
    case 3: snprintf(desc, sizeof(desc), "file open failed"); break;
    case 4: snprintf(desc, sizeof(desc), "foreach over a non-collection"); break;
    case 5: snprintf(desc, sizeof(desc), "range with a non-number bound"); break;
    case 6: snprintf(desc, sizeof(desc), "non-integer in a domain list"); break;
    case 8: snprintf(desc, sizeof(desc), "invalid constraint expression"); break;
    case 9: snprintf(desc, sizeof(desc), "non-string argument"); break;
    case 10: snprintf(desc, sizeof(desc), "dot notation on a non-module"); break;
    case 7: snprintf(desc, sizeof(desc), "file not found"); break;
    default: return;
    }
    if (found > 0)
        fprintf(stderr, "*** lint: line %d: '%s' -- %s\n", found, culprit, desc);
    else if (fallback > 0)
        fprintf(stderr, "*** lint: line %d: '%s' -- %s (value bound here)\n", fallback, culprit, desc);
    else if (ncand == 0 && culprit[0] != '\0')
        fprintf(stderr, "*** lint: '%s' -- %s (not found in the source)\n", culprit, desc);
}

/* report variables that occur only once in the file (singletons) as style
   warnings; _-prefixed don't-care variables are excluded.  When a misleading
   error (instantiation_error / free variable) was seen this run, phrase them
   as possible unbound variables.  Advisory only: never affects the exit
   code. */
static void err_report_singletons(const char *path)
{
    struct { char name[64]; int count; int line; } tab[256];
    char *buf, *p;
    long len;
    int lineno = 1;
    int ntab = 0;
    int i, shown = 0, total = 0;

    err_instring = 0;
    buf = err_read_file(path, &len);
    if (buf == NULL) return;
    p = buf;
    while (*p != '\0') {
        char raw[ERR_LINEBUF], clean[ERR_LINEBUF];
        int curlin = lineno;
        p = err_next_line(p, raw, (int)sizeof(raw), &lineno);
        if (p == NULL) break;
        err_strip_noise(raw, clean, (int)sizeof(clean));
        for (i = 0; clean[i] != '\0'; i++) {
            int start, tlen, k, found = 0;
            if (!((clean[i] >= 'A' && clean[i] <= 'Z') || clean[i] == '_')) continue;
            start = i;
            while ((clean[i] >= 'a' && clean[i] <= 'z') ||
                   (clean[i] >= 'A' && clean[i] <= 'Z') ||
                   (clean[i] >= '0' && clean[i] <= '9') || clean[i] == '_') i++;
            tlen = i - start;
            if (clean[start] == '_') continue;          /* don't-care */
            if (tlen >= (int)sizeof(tab[0].name)) continue;
            for (k = 0; k < ntab; k++)
                if (tab[k].name[tlen] == '\0' &&
                    strncmp(tab[k].name, clean + start, (size_t)tlen) == 0) {
                    tab[k].count++; found = 1; break;
                }
            if (!found && ntab < (int)(sizeof(tab) / sizeof(tab[0]))) {
                memcpy(tab[ntab].name, clean + start, (size_t)tlen);
                tab[ntab].name[tlen] = '\0';
                tab[ntab].count = 1;
                tab[ntab].line = curlin;
                ntab++;
            }
        }
    }
    free(buf);
    for (i = 0; i < ntab; i++)
        if (tab[i].count == 1) total++;
    for (i = 0; i < ntab && shown < 8; i++) {
        if (tab[i].count != 1) continue;
        if (err_misleading_seen)
            fprintf(stderr,
                    "*** lint: line %d: '%s' occurs only once -- possible unbound variable (candidate)\n",
                    tab[i].line, tab[i].name);
        else
            fprintf(stderr,
                    "*** lint: line %d: '%s' occurs only once (singleton)\n",
                    tab[i].line, tab[i].name);
        shown++;
    }
    if (total > shown)
        fprintf(stderr, "*** lint: ... and %d more singleton variable(s)\n", total - shown);
}

void err_lint_main_file(void)
{
    char *buf, *p;
    long len;
    int lineno = 1;
    int paren[24], brack[24], brace[24];
    int np = 0, nb = 0, nc = 0;
    int errors = 0;
    int i;
    char undef_name[64];
    int undef_arity = -1;
    int ttype = 0;
    char tculprit[128];

    err_dump_stderr(undef_name, (int)sizeof(undef_name), &undef_arity,
                    &ttype, tculprit, (int)sizeof(tculprit));
    if (getenv("PICAT_NO_ERRHOOK")) return;
    if (undef_name[0] == '\0' && ttype != 0) err_report_typed(ttype, tculprit);
    if (err_main_file[0] == '\0') {
        if (undef_name[0] != '\0') err_report_undef(undef_name, undef_arity);
        return;
    }
    buf = err_read_file(err_main_file, &len);
    if (buf == NULL) return;
    err_instring = 0;
    err_suppress_name = undef_name;
    err_nheads = 0; err_clause_seq = 0; err_ncontig = 0; err_nnamecontig = 0;
    err_expect_head = 1; err_in_head = 0; err_brack_bad = 0;
    int depth = 0;   /* running bracket depth, for statement positions */

    p = buf;
    while (p != NULL && *p != '\0') {
        char raw[ERR_LINEBUF];
        char clean[ERR_LINEBUF];
        int curlin = lineno;
        int start_depth = depth;
        p = err_next_line(p, raw, (int)sizeof(raw), &lineno);
        if (p == NULL) {
            fprintf(stderr, "*** lint: unterminated /* comment starting at line %d\n", lineno);
            errors++;
            break;
        }
        err_strip_noise(raw, clean, (int)sizeof(clean));

        /* (a) bracket balance */
        {
            int i;
            for (i = 0; clean[i] != '\0'; i++) {
                char c = clean[i];
                if (c == '(') { depth++; if (np < 24) paren[np++] = curlin; }
                else if (c == '[') { depth++; if (nb < 24) brack[nb++] = curlin; }
                else if (c == '{') { depth++; if (nc < 24) brace[nc++] = curlin; }
                else if (c == ')') {
                    if (depth > 0) depth--;
                    if (np == 0) { fprintf(stderr, "*** lint: unbalanced ')' at line %d\n", curlin); errors++; err_brack_bad = 1; }
                    else np--;
                } else if (c == ']') {
                    if (depth > 0) depth--;
                    if (nb == 0) { fprintf(stderr, "*** lint: unbalanced ']' at line %d\n", curlin); errors++; err_brack_bad = 1; }
                    else nb--;
                } else if (c == '}') {
                    if (depth > 0) depth--;
                    if (nc == 0) { fprintf(stderr, "*** lint: unbalanced '}' at line %d\n", curlin); errors++; err_brack_bad = 1; }
                    else nc--;
                }
            }
        }

        /* (b) keyword typos, only at statement positions (the line start
           OUTSIDE brackets, or right after '=>' / '?=>') and not for
           locally defined names; DCG rule heads (token '-->') are skipped */
        {
            int i = 0;
            int bol = (start_depth == 0);   /* a line start outside brackets */
            while (clean[i] != '\0') {
                int start, tlen;
                char tok[64];
                int iskw = 0, k, stmt = 0;
                if (clean[i] == ' ' || clean[i] == '\t') { i++; continue; }
                if (!((clean[i] >= 'a' && clean[i] <= 'z') ||
                      (clean[i] >= 'A' && clean[i] <= 'Z') || clean[i] == '_')) {
                    /* check whether we just passed a '=>' (a new statement) */
                    if (clean[i] == '=' && clean[i+1] == '>') {
                        bol = 1; i += 2; continue;
                    }
                    if (clean[i] == '?' && clean[i+1] == '=' && clean[i+2] == '>') {
                        bol = 1; i += 3; continue;
                    }
                    bol = 0; i++; continue;
                }
                start = i;
                while ((clean[i] >= 'a' && clean[i] <= 'z') ||
                       (clean[i] >= 'A' && clean[i] <= 'Z') ||
                       (clean[i] >= '0' && clean[i] <= '9') || clean[i] == '_') i++;
                tlen = i - start;
                if (tlen >= (int)sizeof(tok)) { bol = 0; continue; }
                memcpy(tok, clean + start, (size_t)tlen);
                tok[tlen] = '\0';
                stmt = bol; bol = 0;
                if (clean[start] >= 'A' && clean[start] <= 'Z') continue;  /* variable */
                if (!stmt) continue;                                       /* inside an expression */
                if (tlen >= 2 && clean[i] == '(') continue;                /* a head/call: locally defined */

                for (k = 0; k < ERR_MAXKW; k++)
                    if (strcmp(tok, err_keywords[k]) == 0) { iskw = 1; break; }

                if (!iskw) {
                    if (err_suppress_name != NULL && strcmp(tok, err_suppress_name) == 0) continue;
                    {
                        /* a DCG rule head: token '-->' (skipping whitespace) */
                        int j = i;
                        while (clean[j] == ' ' || clean[j] == '\t') j++;
                        if (clean[j] == '-' && clean[j+1] == '-' && clean[j+2] == '>') continue;
                    }
                    for (k = 0; k < ERR_MAXKW; k++) {
                        size_t tl = strlen(tok), kl = strlen(err_keywords[k]);
                        if (tl < 2) continue;
                        if (err_lev(tok, err_keywords[k]) == 1 && (int)tl >= (int)kl - 1) {
                            fprintf(stderr, "*** lint: line %d: '%s' -- did you mean '%s'?\n",
                                    curlin, tok, err_keywords[k]);
                            errors++;
                            break;
                        }
                    }
                }
            }
        }

        /* (c) contiguity: track rule heads; a predicate's clauses must be
           grouped together (skipping directive keywords and variables) */
        {
            i = 0;
            while (clean[i] != '\0') {
                char c = clean[i];
                if (c == ' ' || c == '\t') { i++; continue; }
                if (err_in_head) {
                    if (c == '(') { err_h_depth++; i++; }
                    else if (c == '[' || c == '{') { err_h_nest++; i++; }
                    else if (c == ']' || c == '}') { err_h_nest--; i++; }
                    else if (c == ')') {
                        err_h_depth--; i++;
                        if (err_h_depth == 0) {
                            /* a DCG rule head (token '-->' after the args):
                               the compiler allows DCG splits; not tracked */
                            int j = i;
                            while (clean[j] == ' ' || clean[j] == '\t') j++;
                            if (!(clean[j] == '-' && clean[j+1] == '-' && clean[j+2] == '>')) {
                                int ar = err_h_empty ? 0 : err_h_commas + 1;
                                err_register_clause(err_h_name, ar, err_h_line);
                            }
                            err_in_head = 0; err_expect_head = 0;
                        }
                    } else if (c == ',') {
                        if (err_h_depth == 1 && err_h_nest == 0) err_h_commas++;
                        err_h_empty = 0; i++;
                    } else { if (c != ' ') err_h_empty = 0; i++; }
                    continue;
                }
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
                    int start = i, tlen; char tok[64]; int k, iskw = 0;
                    while ((clean[i] >= 'a' && clean[i] <= 'z') ||
                           (clean[i] >= 'A' && clean[i] <= 'Z') ||
                           (clean[i] >= '0' && clean[i] <= '9') || clean[i] == '_') i++;
                    tlen = i - start;
                    if (tlen >= (int)sizeof(tok)) continue;
                    memcpy(tok, clean + start, (size_t)tlen);
                    tok[tlen] = '\0';
                    if (clean[start] >= 'A' && clean[start] <= 'Z') continue;  /* variable */
                    for (k = 0; k < ERR_MAXKW; k++)
                        if (strcmp(tok, err_keywords[k]) == 0) { iskw = 1; break; }
                    if (iskw) {
                        /* a directive: skip its args so they are not
                           mistaken for clause heads (table (+,-,min)) */
                        if (err_expect_head) while (clean[i] != '\0') i++;
                        continue;
                    }
                    if (err_expect_head) {
                        if (clean[i] == '(') {
                            err_in_head = 1; err_h_depth = 1;
                            err_h_commas = 0; err_h_empty = 1; err_h_nest = 0;
                            strncpy(err_h_name, tok, sizeof(err_h_name) - 1);
                            err_h_name[sizeof(err_h_name) - 1] = '\0';
                            err_h_line = curlin;
                            i++; continue;
                        }
                        err_register_clause(tok, 0, curlin);
                        err_expect_head = 0;
                        continue;
                    }
                    continue;
                }
                if (c == '(' && err_expect_head) {
                    /* a quoted-atom head ('!'(N) = ...): the strip ate the
                       name; skip to the matching ')' without registering */
                    int hd = 1, j = i + 1;
                    while (clean[j] != '\0' && hd > 0) {
                        if (clean[j] == '(') hd++;
                        else if (clean[j] == ')') hd--;
                        j++;
                    }
                    i = j;
                    err_expect_head = 0;
                    continue;
                }
                if (c == '.') {
                    char n = clean[i+1];
                    char pr = (i > 0) ? clean[i-1] : '\0';
                    /* a rule end only when followed by EOL or whitespace and
                       not the second dot of a range (1.. N); '.' followed by
                       '.', a digit, or an identifier is a range, a float, or
                       property access (L.length) */
                    if ((n == '\0' || n == ' ' || n == '\t') && pr != '.')
                        err_expect_head = 1;
                    i++; continue;
                }
                if (c == '=' && clean[i+1] == '>') { err_expect_head = 0; i += 2; continue; }
                if (c == '?' && clean[i+1] == '=' && clean[i+2] == '>') { err_expect_head = 0; i += 3; continue; }
                i++;
            }
        }
    }
    if (np > 0) { fprintf(stderr, "*** lint: '(' opened at line %d never closed\n", paren[0]); errors++; err_brack_bad = 1; }
    if (nb > 0) { fprintf(stderr, "*** lint: '[' opened at line %d never closed\n", brack[0]); errors++; err_brack_bad = 1; }
    if (nc > 0) { fprintf(stderr, "*** lint: '{' opened at line %d never closed\n", brace[0]); errors++; err_brack_bad = 1; }
    /* the undef report comes after the pass so the clause registry is built */
    if (undef_name[0] != '\0' && !err_brack_bad)
        err_report_undef(undef_name, undef_arity);
    /* the contiguity findings only make sense for otherwise well-formed files */
    if (!err_brack_bad) {
        for (i = 0; i < err_ncontig; i++)
            fprintf(stderr,
                    "*** lint: line %d: '%s/%d' clauses are not contiguous (previous group ended at line %d)\n",
                    err_contig[i].line, err_contig[i].name, err_contig[i].arity,
                    err_contig[i].prev_line);
    }
    if (!err_brack_bad) err_report_singletons(err_main_file);
    (void)errors;
    free(buf);
}
