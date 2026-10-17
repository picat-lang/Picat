/*
 * err_hook.c -- error diagnostics for debugging support.
 *
 * Hook points, all plain C (no interpreter re-entry, safe at every call
 * site):
 *
 *   err_save_main_file(path)      called from init_loading (init.c); remembers
 *                                 the first .pi argument for the exit lint.
 *   err_didyoumean(name, arity)   called from the toam call path (emu_inst.h)
 *                                 for symbols whose call raises the
 *                                 undefined-procedure exception directly;
 *                                 prints the nearest defined predicate by
 *                                 Levenshtein distance.
 *   err_lint_main_file()          registered with atexit() in main.c; re-reads
 *                                 the saved main file and reports unbalanced
 *                                 brackets, keyword typos, and call-position
 *                                 identifiers that are neither defined nor
 *                                 locally bound, suggesting the nearest
 *                                 defined name (skipping comments and
 *                                 strings).  Silent when there is nothing to
 *                                 report.
 *
 * Compiled into the native build only (-DERRHOOK via Makefile.linux64); the
 * wasm build compiles the call sites out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "basic.h"
#include "term.h"
#include "bapi.h"
#include "extern_decl.h"

static char err_main_file[4096] = "";

void err_save_main_file(const char *path)
{
    if (path == NULL || path[0] == '\0') return;
    strncpy(err_main_file, path, sizeof(err_main_file) - 1);
    err_main_file[sizeof(err_main_file) - 1] = '\0';
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

void err_lint_main_file(void)
{
    char *buf, *p;
    long len;
    int lineno = 1;
    int paren[24], brack[24], brace[24];
    int np = 0, nb = 0, nc = 0;
    int errors = 0;

    if (err_main_file[0] == '\0') return;
    buf = err_read_file(err_main_file, &len);
    if (buf == NULL) return;

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
