/********************************************************************
 *   File   : sat_bp.c
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2026
 *   Purpose: SAT interface for B-Prolog and Picat

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bprolog.h"

#ifdef SAT
#include "kissat/src/kissat.h"
static kissat *sat_solver = (kissat *)NULL;

#define SAT_INIT if (sat_solver != (kissat *)NULL){kissat_release(sat_solver);} sat_solver = kissat_init(); 
#define SAT_ADD_LIT(lit) kissat_add(sat_solver, lit)
#define SAT_START_SOLVER res = kissat_solve(sat_solver)
#define SAT_SATISFIABLE res == 10
#endif

int sat_nvars;
int sat_nvars_limit;  /* used by plglib, the size of the dynamic arrays */
// static int num_threads = 0;

int b_SAT_GET_INC_VAR_NUM_f(BPLONG Num){
    ASSIGN_f_atom(Num,MAKEINT(sat_nvars));
    sat_nvars++;
    return BP_TRUE;
}

#ifdef SAT
/* cl is a list of literals */
int b_SAT_ADD_CL_c(BPLONG cl) {
    BPLONG_PTR ptr, lit_ptr;

    lit_ptr = local_top;  /* reuse Picat't local stack , asumming that the gap is big enough for holding the literals */
    DEREF_NONVAR(cl);

    //  printf(" => add_cl "); write_term(cl); printf("\n");

    /* skip this clause if it contains 't' */
    while (ISLIST(cl)) {
        BPLONG lit;
        BPLONG_PTR lst_ptr;
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(cl);
        lit = FOLLOW(lst_ptr); DEREF_NONVAR(lit);
        if (lit == t_atom) {
            return BP_TRUE;
        }
        if (lit != f_atom) {
            *lit_ptr-- = lit;
        }

        cl = FOLLOW(lst_ptr+1); DEREF_NONVAR(cl);
    }

    for (ptr = local_top; ptr != lit_ptr; ptr--) {
        SAT_ADD_LIT(INTVAL(*ptr));
    }
    SAT_ADD_LIT(0);
        
    return BP_TRUE;
}

int c_sat_init(){
    BPLONG NVars;

    //    NThreads = ARG(1,2);  DEREF_NONVAR(NThreads);
    //    num_threads = (int)INTVAL(NThreads);
    NVars = ARG(2,2);  DEREF_NONVAR(NVars);  
    sat_nvars =  sat_nvars_limit = (int)INTVAL(NVars);
  
    SAT_INIT;
    return BP_TRUE;
}

int c_sat_start(){
    BPLONG lst,res;
    BPLONG_PTR top;
    lst = ARG(1,1);
    DEREF_NONVAR(lst); 

    //  printf("=>sat_start "); write_term(lst); printf("\n");
    //    printf("in c nvars = %d\n", sat_nvars);        
    SAT_START_SOLVER;

    //  printf("<= solver\n");
        
    if (SAT_SATISFIABLE){
        BPLONG_PTR ptr;
        BPLONG var, varNum, bit;

        while (ISLIST(lst)){
            BPLONG_PTR sv_ptr;
            ptr = (BPLONG_PTR)UNTAGGED_ADDR(lst);
            var = FOLLOW(ptr); DEREF(var);
            if (IS_SUSP_VAR(var)){
                sv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(var);
                varNum = fast_get_attr(sv_ptr,et_NUMBER);
                DEREF(varNum);
                varNum = INTVAL(varNum);
                if (varNum > 0){
                    bit = (kissat_value(sat_solver, varNum) > 0) ? BP_ONE : BP_ZERO;
                } else {
                    bit = (kissat_value(sat_solver, -varNum) > 0) ? BP_ZERO : BP_ONE;
                }
                unify(var, bit);
            }
            lst = FOLLOW(ptr+1); DEREF(lst);
        }
        return BP_TRUE;
    } 
    return BP_FALSE;
}
#else
int c_sat_init(){
    BPLONG er  = ADDTAG(BP_NEW_SYM("sat_not_supported",0),ATM);

    printf("SAT not supported for MVC. Please use the cygwin version.\n");
    exception = er;
    return BP_ERROR;
}

int c_sat_start(){
    printf("SAT not supported for MVC. Please use the cygwin version.\n");
    return BP_FALSE;
}

void plgl_resize_dyn_arrays(){
}
#endif

/* 
   If BV is a variable, then return the number attribute attached to BV.
   BV can be the negation of another variable '$\\'(BV1) or a constant (0 or 1).
   If BV=0, then Num=f; if BV=1; then Num=t 
*/
int b_SAT_RETRIEVE_BNUM_cff(BPLONG BV, BPLONG Num, BPLONG MNum){
lab_start:
    DEREF_NONVAR(BV);
    //  printf("=> BNUM %x ",BV); write_term(BV); printf("\n");
  
    if (ISINT(BV)){
        if (BV == BP_ONE){
            ASSIGN_f_atom(Num,t_atom);
            ASSIGN_f_atom(MNum,f_atom);
        } else {
            ASSIGN_f_atom(Num,f_atom);
            ASSIGN_f_atom(MNum,t_atom);
        }
    } else if (IS_SUSP_VAR(BV)){
        BPLONG varNum;
        BPLONG_PTR sv_ptr;
        
        sv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(BV);
        varNum = fast_get_attr(sv_ptr,et_NUMBER);
        DEREF_NONVAR(varNum);
        ASSIGN_f_atom(Num,varNum);
        ASSIGN_f_atom(MNum,MAKEINT(-(INTVAL(varNum))));
    } else {   /* it must be  '$\\'(B) */
        BPLONG_PTR ptr;
        BPLONG tmp;
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(BV);
        BV = FOLLOW(ptr+1);
        tmp = Num; Num = MNum; MNum = tmp;
        goto lab_start;
    }
    //  printf("<= BNUM %x ",BV); write_term(Num); printf(" "); write_term(MNum); printf("\n");
    return BP_TRUE;
}

void dec_to_bin(BPLONG val, int bits[], int k){
    int i;    
    for (i = 0; i < k; i++){
        bits[i] = val%2;
        val = val/2;
    }
}

/*
  Let LogBitVect= <X(n-1),...,X1,X0>. For each bit position i, if all the values
  in X's domain have the same bit value B in the position, then set Xi = B.
*/
int c_sat_propagate_dom_bits() {
    BPLONG X, LogBitVect;
    SYM_REC_PTR sym_ptr;
    BPLONG_PTR dv_ptr, vect_ptr;
    int i, elm, last, n;
    int mark_vect[64];  /* the number of bits can't exceed 64 */
    BPLONG lb, ub;
    int lb_bits[64];
    int ub_bits[64];    
    
    X = ARG(1, 2);
    LogBitVect = ARG(2, 2);

    DEREF_NONVAR(X);
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    lb = DV_first(dv_ptr);
    ub = DV_last(dv_ptr);
    if (lb <= 0 && ub > 0 || ub >= 0 && lb < 0){
        return BP_TRUE;
    }
    if (lb < 0){
        lb = -lb;
    }
    if (ub < 0){
        ub = -ub;
    }
    if (lb > ub){
        BPLONG t = lb;
        lb = ub;
        ub = t;
    }
    
    DEREF_NONVAR(LogBitVect);
    vect_ptr = (BPLONG_PTR)UNTAGGED_ADDR(LogBitVect);
    sym_ptr = (SYM_REC_PTR)FOLLOW(vect_ptr);
    n = GET_ARITY(sym_ptr);
    
    //    printf("=> c_sat_propagate_dom_bits (%d)", n); write_term(X); printf(" "); write_term(LogBitVect); printf("\n");
    // reason about top bits
    dec_to_bin(lb, lb_bits, n);
    dec_to_bin(ub, ub_bits, n);

    vect_ptr++;  /* the vector changes from 1-based to 0-based */
    for (i = n-1; i >= 0; i--){
        if (lb_bits[i] == ub_bits[i]){
            if (!unify(*(vect_ptr+i), MAKEINT(lb_bits[i]))){
                return BP_FALSE;
            }
        } else {
            break;
        }
    }

    // reason about other bits
    if ((IS_BV_DOMAIN(dv_ptr) && DV_size(dv_ptr) <= 100) || (DV_first(dv_ptr) >= -15 && DV_first(dv_ptr) <= 15)){
        for (i = 0; i < n; i++) {
            mark_vect[i] = 0;
        }

        elm = DV_first(dv_ptr);
        last = DV_last(dv_ptr);
        for (; ; ) {
            int val;

            val = (elm >= 0) ? elm : -elm;
            for (i = 0; i < n; i++) {
                mark_vect[i] = (mark_vect[i] | ((val%2 == 0) ? 2 : 1));  // mark if this bit is 0 or 1
                val >>= 1;
            }

            if (elm == last) break;
            elm++;
            if (!IS_IT_DOMAIN(dv_ptr)) elm = domain_next_bv(dv_ptr, elm);
        }

        for (i = 0; i < n; i++) {
            int flag = mark_vect[i];
            if (flag == 1) {  // all the domain values have 1 in ith position
                if (!unify(*(vect_ptr+i), BP_ONE)) return BP_FALSE;
            } else if (flag == 2) {  // all the domain values have 0 in ith position
                if (!unify(*(vect_ptr+i), BP_ZERO)) return BP_FALSE;
            }
        }
    }
    //    printf("<= c_sat_propagate_dom_bits (%d)", n); write_term(X); printf(" "); write_term(LogBitVect); printf("\n");
    return BP_TRUE;
}

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


void Cboot_sat(){
    insert_cpred("c_sat_init",2,c_sat_init);
    insert_cpred("c_sat_start",1,c_sat_start);
    insert_cpred("c_FETCH_DIMACS_SOL_cc",2,c_FETCH_DIMACS_SOL_cc);
}

