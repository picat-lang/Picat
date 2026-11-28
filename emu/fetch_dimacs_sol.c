/********************************************************************
 *   File   : fetch_dimacs_sol.c
 *   Purpose: c_FETCH_DIMACS_SOL_cc(DimacsSolFile, Bs)
 *
 *   Reads a solution file written by a SAT solver and binds each
 *   variable B in Bs (a 0/1 variable with attribute number = Lit) to
 *   the value of Lit in the model. Fails, leaving Bs untouched, if the
 *   file reports UNSAT or cannot be opened.
 *
 *   Accepts both the SAT-competition format (s SATISFIABLE / v ... 0)
 *   and the MiniSat format (SAT / 1 -2 ... 0). Variables that do not
 *   occur in the model are taken to be false.
 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bprolog.h"

int c_FETCH_DIMACS_SOL_cc(void) {
    BPLONG DimacsSolFile = ARG(1, 2);
    BPLONG Bs = ARG(2, 2);
    char fname[MAX_STR_LEN], tok[64];
    unsigned char *bits = NULL;          /* bit v is 1 iff variable v is true */
    BPLONG nbytes = 0;
    FILE *fp;

    if (pp_str_to_c_str(DimacsSolFile, fname, sizeof(fname)) == BP_ERROR) return BP_ERROR;
    if ((fp = fopen(fname, "r")) == NULL) return BP_FALSE;

    /* Fetch the model into the bit array. */
    while (fscanf(fp, "%63s", tok) == 1) {
        char *end;
        BPLONG lit;
        if (strcmp(tok, "c") == 0) {                    /* comment line */
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF);
            continue;
        }
        if (strstr(tok, "UNSAT") != NULL) {             /* UNSATISFIABLE or UNSAT */
            fclose(fp);
            free(bits);
            return BP_FALSE;
        }
        lit = strtol(tok, &end, 10);
        if (*end != '\0' || lit <= 0) continue;         /* s, v, SAT, negative literals, 0 */
        if ((lit >> 3) >= nbytes) {
            BPLONG new_nbytes = nbytes ? nbytes : 1024;
            unsigned char *p;
            while ((lit >> 3) >= new_nbytes) new_nbytes *= 2;
            if ((p = (unsigned char *)realloc(bits, new_nbytes)) == NULL) {
                fclose(fp);
                free(bits);
                return BP_FALSE;
            }
            memset(p + nbytes, 0, new_nbytes - nbytes);
            bits = p;
            nbytes = new_nbytes;
        }
        bits[lit >> 3] |= (unsigned char)(1 << (lit & 7));
    }
    fclose(fp);

    /* Bind each variable to the value of its literal. */
    DEREF(Bs);
    while (ISLIST(Bs)) {
        BPLONG_PTR ptr = (BPLONG_PTR)UNTAGGED_ADDR(Bs);
        BPLONG B = FOLLOW(ptr);
        DEREF(B);
        if (IS_SUSP_VAR(B)) {
            BPLONG lit = fast_get_attr((BPLONG_PTR)UNTAGGED_TOPON_ADDR(B), et_NUMBER);
            BPLONG v, val;
            DEREF(lit);
            lit = INTVAL(lit);
            v = (lit < 0) ? -lit : lit;
            val = ((v >> 3) < nbytes) ? (bits[v >> 3] >> (v & 7)) & 1 : 0;
            if (lit < 0) val = 1 - val;
            if (!unify(B, MAKEINT(val))) {
                free(bits);
                return BP_FALSE;
            }
        }
        Bs = FOLLOW(ptr + 1);
        DEREF(Bs);
    }
    free(bits);
    return BP_TRUE;
}
