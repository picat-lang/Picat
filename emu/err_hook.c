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

/* strip strings, % comments, and quoted atoms from a line in place-ish */
static void err_strip_noise(const char *line, char *out, int outsize)
{
    int i = 0, o = 0;
    while (line[i] != '\0' && o < outsize - 1) {
        char c = line[i];
        if (c == '%') break;
        if (c == '"' || c == '\'') {
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

/* restore stderr, replay what the run wrote there, and extract the first
   "Undefined procedure: NAME/ARITY" message if there is one */
static void err_dump_stderr(char *undef_name, int namecap, int *undef_arity)
{
    FILE *fp;
    long size;
    char *sbuf;

    *undef_name = '\0';
    *undef_arity = -1;
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
    }
    fputs(sbuf, stderr);
    free(sbuf);
}

/* report an undefined procedure: find its line in the main file and the
   nearest keyword or file identifier by edit distance */
static void err_report_undef(const char *name, int arity)
{
    char *buf, *p;
    long len;
    int lineno = 1;
    int found_line = -1;
    char cands[256][64];
    int ncands = 0;
    int k, best = 99;
    char best_name[64] = "";

    if (name[0] == '\0' || name[0] == '$') return;
    buf = err_read_file(err_main_file, &len);
    if (buf == NULL) {                    /* call is not in the main file */
        fprintf(stderr, "*** lint: '%s/%d' is undefined.\n", name, arity);
        return;
    }
    p = buf;
    while (p != NULL && *p != '\0' && found_line < 0) {
        char raw[2048], clean[2048];
        int curlin = lineno;
        p = err_next_line(p, raw, (int)sizeof(raw), &lineno);
        if (p == NULL) break;
        err_strip_noise(raw, clean, (int)sizeof(clean));
        {
            int i = 0;
            while (clean[i] != '\0') {
                int start, tlen;
                char tok[64];
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
                if (ncands < 256 && tlen >= 2) {
                    memcpy(cands[ncands], tok, (size_t)tlen + 1);
                    ncands++;
                }
            }
        }
    }
    free(buf);
    for (k = 0; k < ERR_MAXKW; k++) {            /* keywords win ties */
        int d = err_lev(name, err_keywords[k]);
        if (d == 1 && d < best) { best = d; strncpy(best_name, err_keywords[k], sizeof(best_name) - 1); best_name[sizeof(best_name) - 1] = '\0'; }
    }
    for (k = 0; k < ncands; k++) {
        int d = err_lev(name, cands[k]);
        if (d == 1 && d < best) { best = d; strncpy(best_name, cands[k], sizeof(best_name) - 1); best_name[sizeof(best_name) - 1] = '\0'; }
    }
    if (found_line > 0) {
        if (best == 1)
            fprintf(stderr, "*** lint: line %d: '%s/%d' is undefined -- did you mean '%s'?\n",
                    found_line, name, arity, best_name);
        else
            fprintf(stderr, "*** lint: line %d: '%s/%d' is undefined here.\n", found_line, name, arity);
    } else {
        if (best == 1)
            fprintf(stderr, "*** lint: '%s/%d' is undefined -- did you mean '%s'?\n", name, arity, best_name);
        else
            fprintf(stderr, "*** lint: '%s/%d' is undefined.\n", name, arity);
    }
}

void err_lint_main_file(void)
{
    char *buf, *p;
    long len;
    int lineno = 1;
    int paren[24], brack[24], brace[24];
    int np = 0, nb = 0, nc = 0;
    int errors = 0;
    char undef_name[64];
    int undef_arity = -1;

    err_dump_stderr(undef_name, (int)sizeof(undef_name), &undef_arity);
    if (getenv("PICAT_NO_ERRHOOK")) return;
    if (undef_name[0] != '\0') err_report_undef(undef_name, undef_arity);
    if (err_main_file[0] == '\0') return;
    buf = err_read_file(err_main_file, &len);
    if (buf == NULL) return;
    err_suppress_name = undef_name;

    p = buf;
    while (p != NULL && *p != '\0') {
        char raw[2048];
        char clean[2048];
        int curlin = lineno;
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
                if (c == '(') { if (np < 24) paren[np++] = curlin; }
                else if (c == '[') { if (nb < 24) brack[nb++] = curlin; }
                else if (c == '{') { if (nc < 24) brace[nc++] = curlin; }
                else if (c == ')') {
                    if (np == 0) { fprintf(stderr, "*** lint: unbalanced ')' at line %d\n", curlin); errors++; }
                    else np--;
                } else if (c == ']') {
                    if (nb == 0) { fprintf(stderr, "*** lint: unbalanced ']' at line %d\n", curlin); errors++; }
                    else nb--;
                } else if (c == '}') {
                    if (nc == 0) { fprintf(stderr, "*** lint: unbalanced '}' at line %d\n", curlin); errors++; }
                    else nc--;
                }
            }
        }

        /* (b) keyword typos, only at statement positions (the line start or
           right after '=>' / '?=>') and not for locally defined names */
        {
            int i = 0;
            int bol = 1;   /* at the beginning of the (code) line */
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
    }
    if (np > 0) { fprintf(stderr, "*** lint: '(' opened at line %d never closed\n", paren[0]); errors++; }
    if (nb > 0) { fprintf(stderr, "*** lint: '[' opened at line %d never closed\n", brack[0]); errors++; }
    if (nc > 0) { fprintf(stderr, "*** lint: '{' opened at line %d never closed\n", brace[0]); errors++; }
    (void)errors;
    free(buf);
}
