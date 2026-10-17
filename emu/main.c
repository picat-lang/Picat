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
#ifndef PICAT_BUILD_DATE
#define PICAT_BUILD_DATE "unknown"
#endif
int main(int argc, char *argv[]);
static INLINE int bprolog_main(int argc, char *argv[]);
#ifdef ERRHOOK
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
    extern void err_lint_main_file(void);
    err_lint_main_file();
}
#endif






