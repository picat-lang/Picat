/********************************************************************
 *   File   : main.c
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2026

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/

#include "bprolog.h"
#ifndef PICAT_BUILD_COMMIT
#define PICAT_BUILD_COMMIT "unknown"
#endif
#include <stdlib.h>
#include <unistd.h>
#ifndef PICAT_BUILD_DATE
#define PICAT_BUILD_DATE "unknown"
#endif
int main(int argc, char *argv[]);
static INLINE int bprolog_main(int argc, char *argv[]);
#ifdef ERRHOOK
extern int err_check_only;
static void err_atexit_lint(void);
#endif

int bprolog_main(int argc, char *argv[])
{
#ifdef ERRHOOK
    atexit(err_atexit_lint);
#endif
    initialize_bprolog(argc, argv);
    if (disassem) {
        dis();
        printf("The byte code file is dumped in the file dump.pil\n");
        return (0);
    }
    else {
#ifdef ERRHOOK
        if (err_check_only) {
            /* -c: compile + lint the main file, do not run the program.
               The compile reports its errors as usual (captured stderr is
               re-emitted by the lint); the exit code is nonzero when the
               compile fails or the lint finds errors. */
            extern const char *err_get_main_file(void);
            extern int err_lint_main_file(void);
            const char *mf = err_get_main_file();
            int bad = 0;
            if (mf != NULL && mf[0] != '\0') {
                if (access(mf, R_OK) != 0) {
                    fprintf(stderr, "picat -c: file '%s' not found\n", mf);
                    bad = 1;
                } else {
                    BPLONG_PTR h = heap_top;
                    int saved_out;
                    /* the compile's own error count ("1 error(s)") goes to
                       stdout: route it into the captured stderr for the
                       duration of the load */
                    fflush(stdout);
                    saved_out = dup(1);
                    if (saved_out >= 0) dup2(2, 1);
                    /* catch(picat_load(F), _E, true): a module-load
                       exception (existence_error(module,...)) must not
                       halt the process before the lint runs; the
                       exception var is inspected afterwards and reported
                       in the VM's format so the lint's marker count and
                       typed report see it */
                    { BPLONG fsym = (BPLONG)insert_sym("picat_load", 10, 1);
                      BPLONG fatom = (BPLONG)insert_sym(mf, strlen(mf), 0);
                      BPLONG csym = (BPLONG)insert_sym("$catch", 6, 3);
                      BPLONG tsym = (BPLONG)insert_sym("true", 4, 0);
                      BPLONG_PTR h = heap_top;
                      FOLLOW(h) = fsym;                    /* picat_load/1 functor */
                      FOLLOW(h + 1) = ADDTAG(fatom, ATM);  /* the file path */
                      FOLLOW(h + 2) = csym;                /* $catch/3 functor */
                      FOLLOW(h + 3) = ADDTAG(h, STR);      /* arg1: picat_load(F) */
                      FOLLOW(h + 4) = ADDTAG(h + 4, REF);  /* arg2: fresh var E */
                      FOLLOW(h + 5) = ADDTAG(tsym, ATM);   /* arg3: true */
                      heap_top += 6;
                      if (!bp_call_term(ADDTAG(h + 2, STR))) bad = 1;
                      if (FOLLOW(h + 4) != ADDTAG(h + 4, REF)) {
                          extern void write_term1(BPLONG, FILE *);
                          BPLONG et = FOLLOW(h + 4);
                          bad = 1;
                          if (ISSTRUCT(et)) {
                              /* error(Type,Ctx): report in the VM's exact
                                 format so the lint's marker count and
                                 typed report recognize it */
                              BPLONG_PTR p = (BPLONG_PTR)UNTAGGED_ADDR(et);
                              fprintf(stderr, "*** error(");
                              write_term1(FOLLOW(p + 1), stderr);
                              fprintf(stderr, ",");
                              write_term1(FOLLOW(p + 2), stderr);
                              fprintf(stderr, ")\n");
                          } else {
                              fprintf(stderr, "*** error(");
                              write_term1(et, stderr);
                              fprintf(stderr, ")\n");
                          }
                      }
                    }
                    fflush(stdout);
                    if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
                }
            }
            bad += err_lint_main_file();
            return (bad != 0) ? 1 : 0;
        }
#endif
#ifdef PICAT
        if (main_args != nil_sym) {
            use_gl_getline = 0;
        }
        else {
            printf("Picat experimental, built on %s (commit %s), "
                   "based on Picat 4.0b8, (C) picat-lang.org, "
                   "2013-2026\n", PICAT_BUILD_DATE,
                   PICAT_BUILD_COMMIT);
        }
#endif
#ifdef FDN
        { extern void fdn_init(void); fdn_init(); }
#endif
        bp_call_term(ADDTAG(insert_sym("$bp_first_call", 14, 0), ATM));
        // toam(inst_begin,arreg,local_top);
        return (0);
    }
}

int main(int argc, char *argv[])
{
    return bprolog_main(argc, argv);
}

#ifdef ERRHOOK
/* the exit lint: re-read the main .pi file and report unbalanced brackets
   and keyword typos; silent when the file has no findings */
static void err_atexit_lint(void)
{
    extern int err_lint_main_file(void);
    (void)err_lint_main_file();
}
#endif






