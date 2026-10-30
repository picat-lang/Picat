/********************************************************************
 *   File   : clpfd_libs.c
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2026
 *   Purpose: Primitives on linear constraints

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/

#include "bprolog.h"
#include "event.h"
#include "clpfd.h"

extern FILE *curr_out;

/*
  #ifdef ToamProfileFD
  int domain_updated_in_reducer;
  int n_useful_reducer=0;
  #endif
*/
/* Type codes computed by b_CONSTR_COES_TYPE for linear constraints. Odd codes
   are the 0/1-variable versions; 0 means "may overflow" (general case). */
#define coes_all_one_bool 1
#define coes_all_one 2
#define coes_all_mone_bool 3
#define coes_all_mone 4
#define coes_all_pos_bool 5
#define coes_all_pos 6
#define coes_all_neg_bool 7
#define coes_all_neg 8
#define coes_one_mone_bool 9
#define coes_one_mone 10
#define cv_ge_v 12

/* maximum number of terms the propagators' stack arrays can hold */
#define MAX_NUM_VARS_EQ 551
/* Interval (bounds) consistency for c+a1*x1+...+an*xn = 0. The frame holds the
   n variables, the n coefficients, c and a type code from b_CONSTR_COES_TYPE.
   A forward pass computes bounds [tl[i],tu[i]] of the partial sums
   t(i) = c+a1*x1+...+ai*xi, the last term is narrowed so that t(n) can be 0, and a
   backward pass narrows each xi. The type selects specialised code: unit
   coefficients, 0/1 variables (counting instead of bounds, killing the
   propagator once entailed), and arithmetic without overflow checks when
   b_CONSTR_COES_TYPE has proved that no partial sum can overflow. */
int nary_interval_consistent_eq(BPLONG n)
{
    register BPLONG_PTR dv_ptr, var_ptr, coe_ptr;
    register BPLONG i, first, last;
    register BPLONG a, x;
    register BPLONG_PTR top;
    BPLONG tl[MAX_NUM_VARS_EQ], tu[MAX_NUM_VARS_EQ],
        aa[MAX_NUM_VARS_EQ], xx[MAX_NUM_VARS_EQ];  /* indexed 0..n-1, n <= MAX_NUM_VARS_EQ */
    BPLONG first0, last0, dv_ptr_first, dv_ptr_last, size, temp_i, c;
    int k;
    BPLONG type;

    var_ptr = arreg+n+1;
    coe_ptr = var_ptr+n;
    c = INTVAL(FOLLOW(coe_ptr));  /* constant term; fixed terms are already folded into it */
    type = INTVAL(FOLLOW(coe_ptr+1));
    /*
      printf("c = %d type=%d\n",c,type);
      for (i = 1; i<=n; i++){
      write_term(FOLLOW(coe_ptr-i)); printf("*"); write_term(FOLLOW(var_ptr-i)); printf(" ");
      }
      printf("\n");
    */

    switch (type) {
    case 0:{  /* general case: partial sums may overflow, use checked arithmetic */
        tl[0] = tu[0] = c;
        for (i = 1; i < n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (a > 0) {
                if (a == 1) {
                    COMPUTE_LOW_UPPER_BOUNDS_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    COMPUTE_LOW_UPPER_BOUNDS_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            } else {
                if (a == -1) {
                    COMPUTE_LOW_UPPER_BOUNDS_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    a = -a;
                    COMPUTE_LOW_UPPER_BOUNDS_m(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            }
        }

        a = FOLLOW(coe_ptr-n); a = INTVAL(a);  /* last term: narrow x so that t(n-1)+a*x = 0 can hold */
        x = FOLLOW(var_ptr-n); DEREF_NONVAR(x);
        if (a > 0) {
            if (a == 1) {
                REDUCE_LAST_TERM_DOMAIN_p1(x, tl[n-1], tu[n-1]);
            } else {
                REDUCE_LAST_TERM_DOMAIN_p(a, x, tl[n-1], tu[n-1]);
            }
        } else {
            if (a == -1) {
                REDUCE_LAST_TERM_DOMAIN_m1(x, tl[n-1], tu[n-1]);
            } else {
                a = -a;
                REDUCE_LAST_TERM_DOMAIN_m(a, x, tl[n-1], tu[n-1]);
            }
        }

        for (i = n-1; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            a = aa[i];  /* cached by the forward pass */
            x = xx[i];  /* cached by the forward pass */
            if (a > 0) {
                if (a == 1) {
                    REDUCE_DOMAIN_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    REDUCE_DOMAIN_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            } else {
                if (a == -1) {
                    REDUCE_DOMAIN_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    a = -a;
                    REDUCE_DOMAIN_m(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            }
        }
        return BP_TRUE;
    }

    case coes_all_mone:  /* c-x1-...-xk = 0: same as -c+x1+...+xk = 0; falls through */
        c = -c;

    case coes_all_one:  /* c+x1+...+xk = 0 */
        /* forward pass: bounds of the partial sums t(i) */
        tl[0] = tu[0] = c;
        for (i = 1; i < n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            xx[i] = x;
            COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
        }

        x = FOLLOW(var_ptr-n); DEREF_NONVAR(x);
        REDUCE_LAST_TERM_DOMAIN_p1(x, tl[n-1], tu[n-1]);

        for (i = n-1; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            x = xx[i];
            REDUCE_DOMAIN_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
        }
        return BP_TRUE;

    case coes_all_one_bool:  /* c+x1+...+xk = 0 with 0/1 vars: x1+...+xk = -c; falls through */
        c = -c;

    case coes_all_mone_bool:  /* c-x1-...-xk = 0 with 0/1 vars: c counts the 1s still needed */
    {
        int nvs;

        nvs = 0;
        for (i = 1; i <= n; i++) {  /* fold fixed values into c and collect the variables */
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (x == BP_ONE) {
                c--;
            } else if (x != BP_ZERO) {
                xx[nvs++] = x;
            }
        }
        /* printf("c=%d n1s=%d n0s=%d\n",c,n1s,n0s); */
        if (c <= 0) {
            if (c < 0) return BP_FALSE;
            for (i = 0; i < nvs; i++) {  /* bind remaining vars to 0 */
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(xx[i]);
                ASSIGN_DVAR(dv_ptr, BP_ZERO);
            }
            KILL_SUSP_FRAME;
        } else if (c >= nvs) {
            if (c > nvs) return BP_FALSE;
            for (i = 0; i < nvs; i++) {  /* bind remaining vars to 1 */
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(xx[i]);
                ASSIGN_DVAR(dv_ptr, BP_ONE);
            }
            KILL_SUSP_FRAME;
        }
        return BP_TRUE;
    }

    case coes_all_pos:
        tl[0] = tu[0] = c;
        for (i = 1; i < n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (a == 1) {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            } else {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
            }
        }

        a = FOLLOW(coe_ptr-n); a = INTVAL(a);  /* last term: narrow x so that t(n-1)+a*x = 0 can hold */
        x = FOLLOW(var_ptr-n); DEREF_NONVAR(x);
        if (a == 1) {
            REDUCE_LAST_TERM_DOMAIN_p1(x, tl[n-1], tu[n-1]);
        } else {
            REDUCE_LAST_TERM_DOMAIN_nooverflow_p(a, x, tl[n-1], tu[n-1]);
        }

        for (i = n-1; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            a = aa[i];  /* cached by the forward pass */
            x = xx[i];  /* cached by the forward pass */
            if (a == 1) {
                REDUCE_DOMAIN_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            } else {
                REDUCE_DOMAIN_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
            }
        }
        return BP_TRUE;

    case coes_one_mone_bool: {  /* c+A1*V1+...+An*Vn = 0 where Ai=1 or -1 and the Vi are 0/1
                                  (a flow-conservation constraint) */
        int npvs, nnvs;
        npvs = nnvs = 0;
        for (i = 1; i <= n; i++) {  /* fold fixed values into c and collect the variables */
            a = FOLLOW(coe_ptr-i);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (x == BP_ONE) {
                if (a == BP_ONE)
                    c++;
                else
                    c--;
            } else if (x != BP_ZERO) {  /* x is a variable */
                if (a == BP_ONE)
                    npvs++;
                else
                    nnvs++;
            }
        }

        if (c >= nnvs) {  /* all -1 variables must be 1 and all +1 variables 0 (all outgoing arcs on) */
            if (c > nnvs) return BP_FALSE;
            for (i = 1; i <= n; i++) {
                a = FOLLOW(coe_ptr-i);
                x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
                if (IS_SUSP_VAR(x)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
                    if (a == BP_ONE) {
                        ASSIGN_DVAR(dv_ptr, BP_ZERO);
                    } else {
                        ASSIGN_DVAR(dv_ptr, BP_ONE);
                    }
                }
            }
            KILL_SUSP_FRAME;
        } else if (c <= -npvs) {  /* all +1 variables must be 1 and all -1 variables 0 (all incoming arcs on) */
            if (c < -npvs) return BP_FALSE;
            for (i = 1; i <= n; i++) {
                a = FOLLOW(coe_ptr-i);
                x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
                if (IS_SUSP_VAR(x)) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
                    if (a == BP_ONE) {
                        ASSIGN_DVAR(dv_ptr, BP_ONE);
                    } else {
                        ASSIGN_DVAR(dv_ptr, BP_ZERO);
                    }
                }
            }
            KILL_SUSP_FRAME;
        }
        return BP_TRUE;
    }

    case coes_one_mone:
        tl[0] = tu[0] = c;
        for (i = 1; i < n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            aa[i] = FOLLOW(coe_ptr-i);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (aa[i] == BP_ONE) {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            } else {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            }
        }

        a = FOLLOW(coe_ptr-n);  /* last term: narrow x so that t(n-1)+a*x = 0 can hold */
        x = FOLLOW(var_ptr-n); DEREF_NONVAR(x);
        if (a == BP_ONE) {
            REDUCE_LAST_TERM_DOMAIN_p1(x, tl[n-1], tu[n-1]);
        } else {
            REDUCE_LAST_TERM_DOMAIN_m1(x, tl[n-1], tu[n-1]);
        }

        for (i = n-1; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            x = xx[i];  /* cached by the forward pass */
            if (aa[i] == BP_ONE) {
                REDUCE_DOMAIN_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            } else {
                REDUCE_DOMAIN_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            }
        }
        return 1;

    default:  /* other types: no overflow possible, use plain arithmetic */
        tl[0] = tu[0] = c;
        for (i = 1; i < n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (a > 0) {
                if (a == 1) {
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            } else {
                if (a == -1) {
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    a = -a;
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            }
        }

        a = FOLLOW(coe_ptr-n); a = INTVAL(a);  /* last term: narrow x so that t(n-1)+a*x = 0 can hold */
        x = FOLLOW(var_ptr-n); DEREF_NONVAR(x);
        if (a > 0) {
            if (a == 1) {
                REDUCE_LAST_TERM_DOMAIN_p1(x, tl[n-1], tu[n-1]);
            } else {
                REDUCE_LAST_TERM_DOMAIN_nooverflow_p(a, x, tl[n-1], tu[n-1]);
            }
        } else {
            if (a == -1) {
                REDUCE_LAST_TERM_DOMAIN_m1(x, tl[n-1], tu[n-1]);
            } else {
                a = -a;
                REDUCE_LAST_TERM_DOMAIN_nooverflow_m(a, x, tl[n-1], tu[n-1]);
            }
        }

        for (i = n-1; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            a = aa[i];  /* cached by the forward pass */
            x = xx[i];  /* cached by the forward pass */
            if (a > 0) {
                if (a == 1) {
                    REDUCE_DOMAIN_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    REDUCE_DOMAIN_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            } else {
                if (a == -1) {
                    REDUCE_DOMAIN_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    a = -a;
                    REDUCE_DOMAIN_nooverflow_m(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            }
        }
        return 1;
    }
}


#define MAX_NUM_VARS_NOCOE_EQ 6
/* Interval consistency for c+x1+...+xn = 0 (all coefficients 1, n < 6). Currently
   unused: the interval_consistent_eq_nocoe instruction aborts instead of calling it. */
int nary_interval_consistent_nocoe(BPLONG n)
{
    register BPLONG_PTR dv_ptr, var_ptr;
    register BPLONG i, first, last;
    register BPLONG x;
    register BPLONG_PTR top;
    BPLONG tl[MAX_NUM_VARS_NOCOE_EQ], tu[MAX_NUM_VARS_NOCOE_EQ],
        xx[MAX_NUM_VARS_NOCOE_EQ];  /* indexed 0..n-1, n <= MAX_NUM_VARS_NOCOE_EQ */
    BPLONG dv_ptr_first, dv_ptr_last, size, c;
    int k;

    /*
      #ifdef ToamProfileFD
      domain_updated_in_reducer = 0;
      #endif
    */
    var_ptr = arreg+n+1;
    c = INTVAL(FOLLOW(var_ptr+n));  /* constant term; fixed terms are already folded into it */
    /* forward pass: bounds of the partial sums t(i) */
    tl[0] = tu[0] = c;
    for (i = 1; i < n; i++) {  /* forward pass: bounds of the partial sums t(i) */
        x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
        xx[i] = x;
        COMPUTE_LOW_UPPER_BOUNDS_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
    }

    x = FOLLOW(var_ptr-n); DEREF_NONVAR(x);
    REDUCE_LAST_TERM_DOMAIN_p1(x, tl[n-1], tu[n-1]);

    for (i = n-1; i >= 1; i--) {  /* backward pass: narrow each x(i) */
        x = xx[i];
        REDUCE_DOMAIN_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
    }
    return 1;
}

/* c+a1*x1+...+an*xn >= 0

   With partial sums t(0) = c, t(i) = t(i-1)+a(i)*x(i), the constraint is t(n) >= l(n)
   with l(n) = 0. Working backwards from i = n, t(i-1)+a(i)*x(i) >= l(i) gives

     x(i) >= (l(i)-max(t(i-1)))/a(i)   if a(i) > 0
     x(i) =< (l(i)-max(t(i-1)))/a(i)   if a(i) < 0

   and the lower bound passed on to the previous partial sum:

     l(i-1) = l(i)-a(i)*max(x(i))     if a(i) > 0
     l(i-1) = l(i)-a(i)*min(x(i))     if a(i) < 0
*/
#define MAX_NUM_VARS_GE 551
/* Interval consistency for c+a1*x1+...+an*xn >= 0, using the scheme above.
   Kills the propagator when the constraint is entailed (min of the sum >= 0) and
   fails when it cannot hold (max of the sum < 0). Same type codes as the EQ version,
   plus cv_ge_v (c+x1-x2 >= 0) and the 0/1 cases with negative coefficients. */
int nary_interval_consistent_ge(BPLONG n)
{
    register BPLONG_PTR dv_ptr, var_ptr, coe_ptr;
    register BPLONG i, first, last;
    register BPLONG a, x;
    register BPLONG_PTR top;
    BPLONG tl[MAX_NUM_VARS_GE+1], tu[MAX_NUM_VARS_GE+1],
        aa[MAX_NUM_VARS_GE+1], xx[MAX_NUM_VARS_GE+1];  /* indexed 0..n, n <= MAX_NUM_VARS_GE */
    BPLONG first0, last0, dv_ptr_first, dv_ptr_last, size, temp_i, c;
    int k;
    BPLONG type;

    var_ptr = arreg+n+1;
    coe_ptr = var_ptr+n;
    c = INTVAL(FOLLOW(coe_ptr));  /* constant term; fixed terms are already folded into it */
    type = INTVAL(FOLLOW(coe_ptr+1));

    /*
      printf("c = %d type=%d\n",c,type);
      for (i = 1; i<=n; i++){
      write_term(FOLLOW(coe_ptr-i)); printf("*"); write_term(FOLLOW(var_ptr-i)); printf(" ");
      }
      printf("\n");
    */

        switch (type) {
    case 0:  /* general case: partial sums may overflow, use checked arithmetic */
        tl[0] = tu[0] = c;
        for (i = 1; i <= n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (a > 0) {
                if (a == 1) {
                    COMPUTE_LOW_UPPER_BOUNDS_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    COMPUTE_LOW_UPPER_BOUNDS_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            } else {
                if (a == -1) {
                    COMPUTE_LOW_UPPER_BOUNDS_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    a = -a;
                    COMPUTE_LOW_UPPER_BOUNDS_m(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            }
        }

        if (tl[n] >= 0) {  /* entailed: nothing to reduce */
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[n] < 0) {
            return BP_FALSE;
        }

        tl[n] = 0;

        for (i = n; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            a = aa[i];
            x = xx[i];

            if (a > 0) {
                if (a == 1) {
                    REDUCE_DOMAIN_GE_p1(x, tl[i-1], tu[i-1], tl[i]);
                } else {
                    REDUCE_DOMAIN_GE_p(a, x, tl[i-1], tu[i-1], tl[i]);
                }
            } else {
                if (a == -1) {
                    REDUCE_DOMAIN_GE_m1(x, tl[i-1], tu[i-1], tl[i]);
                } else {
                    a = -a;
                    REDUCE_DOMAIN_GE_m(a, x, tl[i-1], tu[i-1], tl[i]);
                }
            }
        }
        return 1;

    case coes_all_one: {  /* c+x1+x2+...+xn >= 0 */
        tl[0] = tu[0] = c;
        for (i = 1; i <= n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
        }

        if (tl[n] >= 0) {  /* entailed: nothing to reduce */
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[n] < 0) {
            return BP_FALSE;
        }
        tl[n] = 0;

        for (i = n; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            x = xx[i];
            REDUCE_DOMAIN_GE_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i]);
        }
        return 1;
    }

    case coes_all_mone: {  /* c-x1-x2-...-xn >= 0 */
        tl[0] = tu[0] = c;
        for (i = 1; i <= n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
        }
        if (tl[n] >= 0) {  /* entailed: nothing to reduce */
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[n] < 0) {
            return BP_FALSE;
        }

        tl[n] = 0;
        for (i = n; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            x = xx[i];
            REDUCE_DOMAIN_GE_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i]);
        }
        return 1;
    }

    case coes_all_one_bool:{  /* c+x1+x2+...+xn >= 0 with 0/1 vars: c becomes the number of 1s needed */
        int nvs = 0;
        c = -c;
        for (i = 1; i <= n; i++) {  /* fold fixed values into c and collect the variables */
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (x == BP_ONE) {
                c--;
            } else if (x != BP_ZERO) {
                xx[nvs++] = x;
            }
        }
        if (c <= 0) {
            KILL_SUSP_FRAME;
        } else if (nvs <= c) {
            if (nvs < c) return BP_FALSE;
            for (i = 0; i < nvs; i++) {  /* bind remaining vars to 1 */
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(xx[i]);
                ASSIGN_DVAR(dv_ptr, BP_ONE);
            }
            KILL_SUSP_FRAME;
        }
        return BP_TRUE;
    }

    case coes_all_mone_bool:{  /* c-x1-x2-...-xn >= 0 with 0/1 vars: c is the number of 1s allowed */
        int nvs = 0;
        for (i = 1; i <= n; i++) {  /* fold fixed values into c and collect the variables */
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (x == BP_ONE) {
                c--;
            } else if (x != BP_ZERO) {
                xx[nvs++] = x;
            }
        }
        if (c <= 0) {
            if (c < 0) return BP_FALSE;
            for (i = 0; i < nvs; i++) {  /* bind remaining vars to 0 */
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(xx[i]);
                ASSIGN_DVAR(dv_ptr, BP_ZERO);
            }
            KILL_SUSP_FRAME;
        } else if (c >= nvs) {
            KILL_SUSP_FRAME;
        }
        return BP_TRUE;
    }

    case cv_ge_v: {  /* c+x1-x2 >= 0 */
        tl[0] = tu[0] = c;
        x = FOLLOW(var_ptr-1); DEREF_NONVAR(x); xx[1] = x;
        COMPUTE_LOW_UPPER_BOUNDS_p1(x, tl[0], tu[0], tl[1], tu[1]);
        x = FOLLOW(var_ptr-2); DEREF_NONVAR(x); xx[2] = x;
        COMPUTE_LOW_UPPER_BOUNDS_m1(x, tl[1], tu[1], tl[2], tu[2]);

        if (tl[2] >= 0) {
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[2] < 0) {
            return BP_FALSE;
        }
        tl[2] = 0;

        x = xx[2];
        REDUCE_DOMAIN_GE_m1(x, tl[1], tu[1], tl[2]);
        x = xx[1];
        REDUCE_DOMAIN_GE_p1(x, tl[0], tu[0], tl[1]);
        return BP_TRUE;
    }

    case coes_all_pos:
        tl[0] = tu[0] = c;
        for (i = 1; i <= n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (a == 1) {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            } else {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
            }
        }
        if (tl[n] >= 0) {  /* entailed: nothing to reduce */
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[n] < 0) {
            return BP_FALSE;
        }

        tl[n] = 0;

        for (i = n; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            a = aa[i];
            x = xx[i];

            if (a == 1) {
                REDUCE_DOMAIN_GE_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i]);
            } else {
                REDUCE_DOMAIN_GE_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i]);
            }
        }
        return 1;

    case coes_all_neg_bool:  /* c+a1*B1+...+an*Bn >= 0 with 0/1 vars and all ai < 0 (a capacity constraint) */
        for (i = 1; i <= n; i++) {  /* subtract the weights of the Bi already 1 from the capacity c */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (x == BP_ONE) {
                c += a;
            }
        }
        if (c < 0) return BP_FALSE;
        for (i = 1; i <= n; i++) {  /* a Bi whose weight no longer fits must be 0 */
            if (IS_SUSP_VAR(xx[i])) {
                if (c+aa[i] < 0) {
                    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(xx[i]);
                    ASSIGN_DVAR(dv_ptr, BP_ZERO);
                }
            }
        }
        return BP_TRUE;

    case coes_one_mone:
    case coes_one_mone_bool:
        tl[0] = tu[0] = c;
        for (i = 1; i <= n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            aa[i] = FOLLOW(coe_ptr-i);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (aa[i] == BP_ONE) {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            } else {
                COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
            }
        }

        if (tl[n] >= 0) {  /* entailed: nothing to reduce */
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[n] < 0) {
            return BP_FALSE;
        }

        tl[n] = 0;

        for (i = n; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            x = xx[i];
            if (aa[i] == BP_ONE) {
                REDUCE_DOMAIN_GE_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i]);
            } else {
                REDUCE_DOMAIN_GE_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i]);
            }
        }
        return 1;

    default :
        tl[0] = tu[0] = c;
        for (i = 1; i <= n; i++) {  /* forward pass: bounds of the partial sums t(i) */
            a = FOLLOW(coe_ptr-i); aa[i] = a = INTVAL(a);
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x); xx[i] = x;
            if (a > 0) {
                if (a == 1) {
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            } else {
                if (a == -1) {
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i], tu[i]);
                } else {
                    a = -a;
                    COMPUTE_LOW_UPPER_BOUNDS_nooverflow_m(a, x, tl[i-1], tu[i-1], tl[i], tu[i]);
                }
            }
        }

        if (tl[n] >= 0) {  /* entailed: nothing to reduce */
            KILL_SUSP_FRAME;
            return BP_TRUE;
        }
        if (tu[n] < 0) {
            return BP_FALSE;
        }

        tl[n] = 0;

        for (i = n; i >= 1; i--) {  /* backward pass: narrow each x(i) */
            a = aa[i];
            x = xx[i];

            if (a > 0) {
                if (a == 1) {
                    REDUCE_DOMAIN_GE_nooverflow_p1(x, tl[i-1], tu[i-1], tl[i]);
                } else {
                    REDUCE_DOMAIN_GE_nooverflow_p(a, x, tl[i-1], tu[i-1], tl[i]);
                }
            } else {
                if (a == -1) {
                    REDUCE_DOMAIN_GE_nooverflow_m1(x, tl[i-1], tu[i-1], tl[i]);
                } else {
                    a = -a;
                    REDUCE_DOMAIN_GE_nooverflow_m(a, x, tl[i-1], tu[i-1], tl[i]);
                }
            }
        }
        return 1;
    }
}

/******** region *************/
/* Restricts X to from..to. X must be either an integer or a domain variable. */
int varorint_domain_region(BPLONG X, BPLONG from, BPLONG to)
{
    BPLONG_PTR dv_ptr;

    DEREF_NONVAR(X);
    if (ISINT(X)) {
        X = INTVAL(X);
        if (X < from || X > to) return BP_FALSE;
        return BP_TRUE;
    } else {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        return domain_region_noint(dv_ptr, from, to);
    }
}

/* Restricts the domain of the (unbound) variable at dv_ptr to from..to.
   Returns 0 if the result would be empty. */
int domain_region_noint(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{
    BPLONG dv_ptr_first, dv_ptr_last, size;
    int k;

    dv_ptr_first = DV_first(dv_ptr);
    dv_ptr_last = DV_last(dv_ptr);
    CALL_DOMAIN_REGION_noseed(dv_ptr, from, to);
    return 1;
}

/* Same as domain_region_noint, but dv_ptr's variable may already be bound,
   in which case its value is just checked against from..to. */
int domain_region_aux(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{
    BPLONG dv_ptr_first, dv_ptr_last, size, val;
    int k;

    if (ISINT(FOLLOW(dv_ptr))) {
        val = INTVAL(FOLLOW(dv_ptr));
        if (val < from || val > to) return BP_FALSE;
        return BP_TRUE;
    }
    dv_ptr_first = DV_first(dv_ptr);
    dv_ptr_last = DV_last(dv_ptr);
    CALL_DOMAIN_REGION_noseed(dv_ptr, from, to);
    return 1;
}

/* Debugging: prints the linear constraint stored in the current frame as c+a*x+...=0. */
void print_linear_constr(BPLONG n)
{
    BPLONG i;
    BPLONG c = FOLLOW(arreg+2*n+1);
    printf("n=" BPLONG_FMT_STR "\n", n);
    /*
      for (i=n;i>0;i--){
      printf("%x ", FOLLOW(arreg+n+i));
      }
      printf("\n");
    */
    write_term(c); fprintf(curr_out, "+");
    for (i = n; i > 0; i--) {
        write_term(FOLLOW(arreg+n+i)); fprintf(curr_out, "*");
        write_term(FOLLOW(arreg+i)); fprintf(curr_out, "+");
    }
    fprintf(curr_out, "=0 \n");
}

/* Returns 1 if |a*v| <= bound, computed without overflowing. */
static int coe_term_fits(BPLONG a, BPLONG v, BPLONG bound)
{
    if (a < 0) a = -a;
    if (v < 0) v = -v;
    if (a == 0 || v == 0) return 1;
    return a <= bound / v;
}

/* Classifies the linear constraint in the current frame and stores the type code
   (coes_* or cv_ge_v above, 0 = may overflow, 14 = mixed signs) after the constant,
   for the nary_interval_consistent_* propagators. Also dereferences the constant and
   the coefficients in place. A constraint is typed as non-overflowing only if c and
   every term a*x stay within BP_MAXINT_1W/(n+1), so no partial sum can overflow. */
int b_CONSTR_COES_TYPE(BPLONG n)
{
    BPLONG_PTR dv_ptr, var_ptr, coe_ptr;
    BPLONG a, c, x, first, last;
    BPLONG term_lb, term_ub;  /* must be BPLONG: BP_MAXINT_1W/(n+1) does not fit in an int on 64-bit */
    int property, type, all_bool_vars, i;

    n = INTVAL(n);
    property = 0;
    all_bool_vars = 1;

    var_ptr = arreg+n+1;
    coe_ptr = var_ptr+n;
    c = FOLLOW(coe_ptr); DEREF_NONVAR(c); FOLLOW(coe_ptr) = c; c = INTVAL(c);

    term_lb = BP_MININT_1W/(n+1); term_ub = BP_MAXINT_1W/(n+1);
    if (c < term_lb || c > term_ub) {FOLLOW(coe_ptr+1) = BP_ZERO; return BP_TRUE;}

    for (i = 1; i <= n; i++) {
        a = FOLLOW(coe_ptr-i); DEREF_NONVAR(a); FOLLOW(coe_ptr-i) = a; a = INTVAL(a);
        x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
        if (IS_SUSP_VAR(x)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
            first = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            if (first == BP_MININT_1W || last == BP_MAXINT_1W) {FOLLOW(coe_ptr+1) = BP_ZERO; return BP_TRUE;}
        } else {
            first = last = INTVAL(x);
        }
        /* both a*first and a*last must be within [term_lb,term_ub]; with a<0 the product
           of last is the low end, so check both magnitudes */
        if (!coe_term_fits(a, first, term_ub) || !coe_term_fits(a, last, term_ub)) {FOLLOW(coe_ptr+1) = BP_ZERO; return BP_TRUE;}
        if (first < 0 || last > 1) all_bool_vars = 0;  /* 0..1 vars and the integers 0 and 1 */

        if (a == 1) {
            property |= 1;
        } else if (a == -1) {
            property |= 2;
        } else if (a > 0) {
            property |= 4;
        } else {
            property |= 8;
        }
    }
    switch (property) {
    case 1:  /* all coefficients are 1 */
        if (all_bool_vars)
            type = coes_all_one_bool;
        else
            type = coes_all_one;
        break;
    case 2:  /* all coefficients are -1 */
        if (all_bool_vars)
            type = coes_all_mone_bool;
        else
            type = coes_all_mone;
        break;
    case 3:  /* only 1 and -1 */
        if (all_bool_vars)
            type = coes_one_mone_bool;
        else
            type = coes_one_mone;
        break;
    case 4:  /* all positive, none is 1 */
    case 5:  /* all positive, some are 1 */
        if (all_bool_vars)
            type = coes_all_pos_bool;
        else
            type = coes_all_pos;
        break;
    case 8:  /* all negative, none is -1 */
    case 10:  /* all negative, some are -1 */
        if (all_bool_vars)
            type = coes_all_neg_bool;
        else
            type = coes_all_neg;
        break;
    default:
        type = 14;
    }
    FOLLOW(coe_ptr+1) = MAKEINT(type);
    return BP_TRUE;
}

/* Succeeds if the type code is one of the 0/1-variable types (the odd codes). */
int b_IS_BOOLEAN_VAR_CONSTR(BPLONG coes_type)
{
    DEREF_NONVAR(coes_type);
    coes_type = INTVAL(coes_type);
    if (coes_type & 0x1) return BP_TRUE; return BP_FALSE;
}

/* Install the coefficients and variables on to the stack so that
   the nary_interval_consistent_eq or nary_interval_consistent_ge
   can be called. Terms is a list of (A,V) pairs; the type is set to 0 (general case).
*/
void install_coes_vars_to_stack(int n, BPLONG Terms, BPLONG Const) {
    int i;
    BPLONG_PTR var_ptr, coe_ptr;

    var_ptr = arreg+n+1;
    coe_ptr = var_ptr+n;
    FOLLOW(coe_ptr+1) = BP_ZERO;  /* type 0: the general case */
    FOLLOW(coe_ptr) = Const;

    i = 1;
    while (ISLIST(Terms)) {
        BPLONG pair, tmp;
        BPLONG_PTR lst_ptr, pair_ptr;
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(Terms);
        pair = FOLLOW(lst_ptr); DEREF(pair);
        pair_ptr = (BPLONG_PTR)UNTAGGED_ADDR(pair);
        tmp = FOLLOW(pair_ptr+1); DEREF(tmp); FOLLOW(coe_ptr-i) = tmp;
        tmp = FOLLOW(pair_ptr+2); FOLLOW(var_ptr-i) = tmp;
        i++;
        Terms = FOLLOW(lst_ptr+1); DEREF(Terms);
    }
}

/* Same as install_coes_vars_to_stack, with the terms in reversed order.
   Running the propagator in both orders gives each variable a chance to be
   narrowed using the bounds of all the others. */
void install_coes_vars_to_stack_rev(int n, BPLONG Terms, BPLONG Const) {
    int i;
    BPLONG_PTR var_ptr, coe_ptr;

    var_ptr = arreg+n+1;
    coe_ptr = var_ptr+n;
    FOLLOW(coe_ptr+1) = BP_ZERO;  /* type 0: the general case */
    FOLLOW(coe_ptr) = Const;

    i = n;
    while (ISLIST(Terms)) {
        BPLONG pair, tmp;
        BPLONG_PTR lst_ptr, pair_ptr;
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(Terms);
        pair = FOLLOW(lst_ptr); DEREF(pair);
        pair_ptr = (BPLONG_PTR)UNTAGGED_ADDR(pair);
        tmp = FOLLOW(pair_ptr+1); DEREF(tmp); FOLLOW(coe_ptr-i) = tmp;
        tmp = FOLLOW(pair_ptr+2); FOLLOW(var_ptr-i) = tmp;
        i--;
        Terms = FOLLOW(lst_ptr+1); DEREF(Terms);
    }
}

/* Terms=[(An,Vn),...,(A1,V1)].
   Reduce domains of Vi's such that the constraint An*Vn+...,A1*V1+Const=0 is interval consistent.
   Builds a temporary frame below local_top and runs nary_interval_consistent_eq twice
   (forward and reversed term order); constraints with more than MAX_NUM_VARS_EQ terms are skipped.
*/
int c_REDUCE_DOMAINS_IC_EQ() {
    BPLONG Terms, Const;
    BPLONG_PTR old_arreg;
    int n, res;

    Terms = ARG(1, 2); DEREF(Terms);
    Const = ARG(2, 2); DEREF(Const);
    /*
      printf("=>reduce ");
      write_term(Terms); printf("\n");
      write_term(Const); printf("\n");
    */
    n = list_length(Terms, Terms);
    if (n > MAX_NUM_VARS_EQ) return BP_TRUE;  /* too large for the propagator's arrays: skip */
    old_arreg = arreg;
    arreg = local_top-2*n-2;
    install_coes_vars_to_stack(n, Terms, Const);
    res = nary_interval_consistent_eq(n);
    if (res == BP_FALSE){
        arreg = old_arreg;
        return BP_FALSE;
    }
        
    install_coes_vars_to_stack_rev(n, Terms, Const);
    res = nary_interval_consistent_eq(n);
    arreg = old_arreg;
    /*
      printf("<=reduce %d\n",res);  
      write_term(Terms); printf("\n");
      write_term(Const); printf("\n");
    */
    return res;
}

/* Terms=[(An,Vn),...,(A1,V1)].
   Reduce domains of Vi's such that the constraint An*Vn+...,A1*V1+Const>=0 is interval consistent.
   Same scheme as c_REDUCE_DOMAINS_IC_EQ, using nary_interval_consistent_ge.
*/
int c_REDUCE_DOMAINS_IC_GE() {
    BPLONG Terms, Const;
    BPLONG_PTR old_arreg;
    int n, res;

    Terms = ARG(1, 2); DEREF(Terms);
    Const = ARG(2, 2); DEREF(Const);

    n = list_length(Terms, Terms);
    if (n > MAX_NUM_VARS_GE) return BP_TRUE;  /* too large for the propagator's arrays: skip */
    old_arreg = arreg;
    arreg = local_top-2*n-2;
    install_coes_vars_to_stack( n, Terms, Const);
    res = nary_interval_consistent_ge(n);
    if (res == BP_FALSE){
        arreg = old_arreg;
        return BP_FALSE;
    }
        
    install_coes_vars_to_stack_rev(n, Terms, Const);
    res = nary_interval_consistent_ge(n);
        
    arreg = old_arreg;
    return res;
}

/* X1 = (X2 or ... or Xn), n<=15, Xi's are in the current frame.
   X1 unknown: some Xi=1 forces X1=1; all Xi=0 forces X1=0.
   X1=0: all Xi become 0. X1=1: unit propagation when only one Xi is unknown. */
int b_BOOL_OR_c(BPLONG n) {
    BPLONG_PTR dv_ptr, var_ptr;
    BPLONG x1, x;
    int i;
    //  printf("=> bool_or\n");
    DEREF_NONVAR(n); n = INTVAL(n);
    var_ptr = arreg+n;
    x1 = FOLLOW(var_ptr); DEREF_NONVAR(x1);
    if (IS_SUSP_VAR(x1)) {
        int num_of_vars = 0;
        for (i = 1; i < n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (IS_SUSP_VAR(x)) {
                num_of_vars++;
            } else if (x == BP_ONE) {  /* some xi is 1: X1 becomes 1 and the propagator is done */
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x1);
                ASSIGN_DVAR(dv_ptr, BP_ONE);
                AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
                return BP_TRUE;
            }
        }
        if (num_of_vars == 0) {  /* all Xi (i=2,...,n) are 0 */
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x1);
            ASSIGN_DVAR(dv_ptr, BP_ZERO);
            AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
            return BP_TRUE;
        }
    } else if (x1 == BP_ZERO) {  /* X1=0: every Xi (i=2,...,n) must be 0 */
        for (i = 1; i < n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (IS_SUSP_VAR(x)) {
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
                ASSIGN_DVAR(dv_ptr, BP_ZERO);
            } else if (x == BP_ONE) {
                return BP_FALSE;
            }
        }
        AR_STATUS(arreg) = SUSP_EXIT;  /* entailed: kill this propagator */
        return BP_TRUE;
    } else {  /* X1=1: unit propagation */
        int num_of_vars = 0;
        BPLONG last_var;

        for (i = 1; i < n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (x == BP_ONE) {  /* already satisfied */
                AR_STATUS(arreg) = SUSP_EXIT;
                return BP_TRUE;
            } else if (x != BP_ZERO) {  /* x is a variable */
                num_of_vars++;
                last_var = x;
            }
        }
        if (num_of_vars == 1) {  /* only one variable left: it is forced */
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(last_var);
            ASSIGN_DVAR(dv_ptr, BP_ONE);
            AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
            return BP_TRUE;
        } else if (num_of_vars == 0) {  /* all Xi (i=2,...,n) are 0: the disjunction can't hold */
            return BP_FALSE;
        }
        return BP_TRUE;
    }
    return BP_TRUE;  /* not reachable */
}

/* X1 = (X2 and ... and Xn), n<=15, Xi's are in the current frame.
   Dual of b_BOOL_OR_c: some Xi=0 forces X1=0; all Xi=1 forces X1=1;
   X1=1 sets all Xi to 1; X1=0 with one unknown Xi (the rest 1) forces it to 0. */
int b_BOOL_AND_c(BPLONG n) {
    BPLONG_PTR dv_ptr, var_ptr;
    BPLONG x1, x;
    int i;

    //  printf("=> bool_and\n");
    DEREF_NONVAR(n); n = INTVAL(n);
    /*
      for (i=n; i>=1; i--){
      write_term(FOLLOW(arreg+i)); printf(" ");
      }
      printf("\n");
    */
    var_ptr = arreg+n;
    x1 = FOLLOW(var_ptr); DEREF_NONVAR(x1);
    if (IS_SUSP_VAR(x1)) {
        int num_of_vars = 0;
        for (i = 1; i < n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (IS_SUSP_VAR(x)) {
                num_of_vars++;
            } else if (x == BP_ZERO) {  /* some xi is 0: X1 becomes 0 and the propagator is done */
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x1);
                ASSIGN_DVAR(dv_ptr, BP_ZERO);
                AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
                return BP_TRUE;
            }
        }
        if (num_of_vars == 0) {  /* all Xi (i=2,...,n) are 1 */
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x1);
            ASSIGN_DVAR(dv_ptr, BP_ONE);
            AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
            return BP_TRUE;
        }
    } else if (x1 == BP_ONE) {  /* X1=1: every Xi (i=2,...,n) must be 1 */
        for (i = 1; i < n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (IS_SUSP_VAR(x)) {
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
                ASSIGN_DVAR(dv_ptr, BP_ONE);
            } else if (x == BP_ZERO) {
                return BP_FALSE;
            }
        }
        AR_STATUS(arreg) = SUSP_EXIT;  /* entailed: kill this propagator */
        return BP_TRUE;
    } else {  /* X1=0: unit propagation on the negated literals */
        int num_of_vars = 0;
        BPLONG last_var;

        for (i = 1; i < n; i++) {
            x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
            if (x == BP_ZERO) {  /* already satisfied */
                AR_STATUS(arreg) = SUSP_EXIT;
                return BP_TRUE;
            } else if (x != BP_ONE) {  /* x is a variable */
                num_of_vars++;
                last_var = x;
            }
        }
        if (num_of_vars == 1) {  /* only one variable left: it is forced */
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(last_var);
            ASSIGN_DVAR(dv_ptr, BP_ZERO);
            AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
            return BP_TRUE;
        } else if (num_of_vars == 0) {  /* all Xi (i=2,...,n) are 1: the conjunction can't be 0 */
            return BP_FALSE;
        }
        return BP_TRUE;
    }
    return BP_TRUE;  /* not reachable */
}

/* X1 = max(X2, ..., Xn), n<=15, Xi's are in the current frame.
   Bounds reasoning: every Xi <= max(X1); X1 lies in [max_i min(Xi), max_i max(Xi)].
   When X1 is fixed, no fixed Xi equals it, and exactly one variable Xi can still
   reach X1's value, that Xi must equal X1. */
int b_PROP_MAX_c(BPLONG n) {
    BPLONG_PTR dv_ptr1, dv_ptr, var_ptr, last_dv_ptr = NULL;
    BPLONG x1, x, first1, last1, first, last, num_of_vars, num_reach, max_first, max_last, max_fixed;
    int i;

    DEREF_NONVAR(n); n = INTVAL(n);
    var_ptr = arreg+n;
    x1 = FOLLOW(var_ptr); DEREF_NONVAR(x1);
    if (IS_SUSP_VAR(x1)) {
        dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x1);
        first1 = DV_first(dv_ptr1);
        last1 = DV_last(dv_ptr1);
    } else {
        dv_ptr1 = NULL;
        first1 = last1 = INTVAL(x1);
    }

    num_of_vars = num_reach = 0;
    max_first = max_last = max_fixed = BP_MININT_1W;
    for (i = 1; i < n; i++) {
        x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
        if (IS_SUSP_VAR(x)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
            first = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            if (last > last1) {  /* Xi <= max(X1) */
                if (domain_region_noint(dv_ptr, first, last1) == BP_FALSE) return BP_FALSE;
                x = FOLLOW(dv_ptr);  /* the region may have instantiated Xi */
            }
        }
        if (IS_SUSP_VAR(x)) {
            num_of_vars++;
            first = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            if (last >= first1) {num_reach++; last_dv_ptr = dv_ptr;}  /* can take X1's lowest value */
        } else {
            first = last = INTVAL(x);
            if (first > last1) return BP_FALSE;
            if (first > max_fixed) max_fixed = first;
        }
        if (first > max_first) max_first = first;
        if (last > max_last) max_last = last;
    }
    if (dv_ptr1 != NULL) {  /* X1 is a variable */
        if (num_of_vars == 0) {  /* X1 = max of the fixed values */
            if (!dm_true(dv_ptr1, max_fixed)) return BP_FALSE;
            ASSIGN_DVAR(dv_ptr1, MAKEINT(max_fixed));
            AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
            return BP_TRUE;
        }
        if (max_first > first1 || max_last < last1) {
            if (domain_region_noint(dv_ptr1, max_first, max_last) == BP_FALSE) return BP_FALSE;
        }
        return BP_TRUE;
    }
    /* X1 is fixed */
    if (num_of_vars == 0) return (first1 == max_fixed) ? BP_TRUE : BP_FALSE;
    if (max_fixed == first1) {  /* attained by a fixed Xi; the others are already bounded by X1 */
        AR_STATUS(arreg) = SUSP_EXIT;  /* entailed: kill this propagator */
        return BP_TRUE;
    }
    if (num_reach == 0) return BP_FALSE;  /* no Xi can reach X1 */
    if (num_reach == 1) {  /* the only Xi that can reach X1 must take its value */
        if (!dm_true(last_dv_ptr, first1)) return BP_FALSE;
        ASSIGN_DVAR(last_dv_ptr, MAKEINT(first1));
        AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
    }
    return BP_TRUE;
}


/* X1 = min(X2, ..., Xn), n<=15, Xi's are in the current frame.
   Mirror image of b_PROP_MAX_c: every Xi >= min(X1);
   X1 lies in [min_i min(Xi), min_i max(Xi)]. */
int b_PROP_MIN_c(BPLONG n) {
    BPLONG_PTR dv_ptr1, dv_ptr, var_ptr, last_dv_ptr = NULL;
    BPLONG x1, x, first1, last1, first, last, num_of_vars, num_reach, min_first, min_last, min_fixed;
    int i;

    DEREF_NONVAR(n); n = INTVAL(n);
    var_ptr = arreg+n;
    x1 = FOLLOW(var_ptr); DEREF_NONVAR(x1);
    if (IS_SUSP_VAR(x1)) {
        dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x1);
        first1 = DV_first(dv_ptr1);
        last1 = DV_last(dv_ptr1);
    } else {
        dv_ptr1 = NULL;
        first1 = last1 = INTVAL(x1);
    }

    num_of_vars = num_reach = 0;
    min_first = min_last = min_fixed = BP_MAXINT_1W;
    for (i = 1; i < n; i++) {
        x = FOLLOW(var_ptr-i); DEREF_NONVAR(x);
        if (IS_SUSP_VAR(x)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(x);
            first = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            if (first < first1) {  /* Xi >= min(X1) */
                if (domain_region_noint(dv_ptr, first1, last) == BP_FALSE) return BP_FALSE;
                x = FOLLOW(dv_ptr);  /* the region may have instantiated Xi */
            }
        }
        if (IS_SUSP_VAR(x)) {
            num_of_vars++;
            first = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            if (first <= first1) {num_reach++; last_dv_ptr = dv_ptr;}  /* can take X1's value */
        } else {
            first = last = INTVAL(x);
            if (first < first1) return BP_FALSE;
            if (first < min_fixed) min_fixed = first;
        }
        if (first < min_first) min_first = first;
        if (last < min_last) min_last = last;
    }
    if (dv_ptr1 != NULL) {  /* X1 is a variable */
        if (num_of_vars == 0) {  /* X1 = min of the fixed values */
            if (!dm_true(dv_ptr1, min_fixed)) return BP_FALSE;
            ASSIGN_DVAR(dv_ptr1, MAKEINT(min_fixed));
            AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
            return BP_TRUE;
        }
        if (min_first > first1 || min_last < last1) {
            if (domain_region_noint(dv_ptr1, min_first, min_last) == BP_FALSE) return BP_FALSE;
        }
        return BP_TRUE;
    }
    /* X1 is fixed */
    if (num_of_vars == 0) return (first1 == min_fixed) ? BP_TRUE : BP_FALSE;
    if (min_fixed == first1) {  /* attained by a fixed Xi; the others are already bounded by X1 */
        AR_STATUS(arreg) = SUSP_EXIT;  /* entailed: kill this propagator */
        return BP_TRUE;
    }
    if (num_reach == 0) return BP_FALSE;  /* no Xi can reach X1 */
    if (num_reach == 1) {  /* the only Xi that can reach X1 must take its value */
        if (!dm_true(last_dv_ptr, first1)) return BP_FALSE;
        ASSIGN_DVAR(last_dv_ptr, MAKEINT(first1));
        AR_STATUS(arreg) = SUSP_EXIT;  /* kill this propagator */
    }
    return BP_TRUE;
}

/* (Disabled.) Maintain arc consistency for X+Y = Z when one of the domains is a bit-vector domain
   (a domain with holes), and Z's domain is small (which means both X's and Y's domains
   are small as well). Note that the constraint is already interval consistent.

   For each ex in X's domain and ey in Y's domain, the value ex+ey is supported. 
   For each ez in Z's domain, if ez is not supported, then remove ez from Z's domain.

   For each ey in Y's domain and ez in Z's domain, the value ez-ey is supported. 
   For each ex in X's domain, if ex is not supported, then remove ex from X's domain.
*/
/*
  #define NUM_MARK_WORDS 3200

  int c_REDUCE_DOMAIN_AC_ADD(){
  BPLONG X, Y, Z, ex, min_x, max_x, ey, min_y, max_y, ez, min_z, max_z, size;
  BPLONG_PTR dv_ptr_x, dv_ptr_y, dv_ptr_z, tmp_dv_ptr;
  int i, flag = 0;
  int mark_words[NUM_MARK_WORDS];
  
  X = ARG(1,3);
  Y = ARG(2,3);
  Z = ARG(3,3);

  printf("=> AC_ADD "); write_term(X); printf(" "); write_term(Y); printf(" "); write_term(Z); printf("\n");
  
  DEREF_NONVAR(Z);
  if (IS_SUSP_VAR(Z)){
  dv_ptr_z = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Z);
  if (!IS_SMALL_DOMAIN(dv_ptr_z)) return BP_TRUE;
  if (!IS_IT_DOMAIN(dv_ptr_z)) flag = 1;        
  min_z = DV_first(dv_ptr_z);  max_z = DV_last(dv_ptr_z);
  } else {
  dv_ptr_z = NULL;
  min_z = max_z = INTVAL(Z);
  }
  DEREF_NONVAR(X);
  if (IS_SUSP_VAR(X)){
  dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
  if (!IS_IT_DOMAIN(dv_ptr_x)) flag = 1;
  min_x = DV_first(dv_ptr_x); max_x = DV_last(dv_ptr_x);
  } else {
  dv_ptr_x = NULL;
  min_x = max_x = INTVAL(X);
  }
  DEREF_NONVAR(Y);
  if (IS_SUSP_VAR(Y)){
  dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
  if (!IS_IT_DOMAIN(dv_ptr_y)) flag = 1;        
  min_y = DV_first(dv_ptr_y); max_y = DV_last(dv_ptr_y);
  } else {
  dv_ptr_y = NULL;
  min_y = max_y = INTVAL(Y);
  }
  if (flag == 0) return BP_TRUE;                     // none of the domains is a bit-vector domain 

  if (dv_ptr_z != NULL && IS_IT_DOMAIN(dv_ptr_z))
  goto lab_ac_on_z;
  if (dv_ptr_x != NULL && IS_IT_DOMAIN(dv_ptr_x))
  goto lab_ac_on_x;
  if (dv_ptr_y != NULL && IS_IT_DOMAIN(dv_ptr_y)) {  // swap X with Y 
  tmp_dv_ptr = dv_ptr_x; dv_ptr_x = dv_ptr_y; dv_ptr_y = tmp_dv_ptr;
  min_y = min_x; max_y = max_x;
  min_x = DV_first(dv_ptr_x); max_x = DV_last(dv_ptr_x);
  goto lab_ac_on_x;
  }
  return BP_TRUE;                                    // none of the domains is interval 

  lab_ac_on_x:  
  size = max_x - min_x + 1;
  if (size > NUM_MARK_WORDS) return BP_TRUE;         // no enough space for marking 

  for (i = 0; i < size; i++){
  mark_words[i] = 0;                               // initialize the marks, no value is supported 
  }
  
  for (ez = min_z; ez <= max_z; ez++){
  if (dv_ptr_z != NULL && !dm_true(dv_ptr_z, ez)) continue;
  for (ey = min_y; ey <= max_y; ey++){
  if (dv_ptr_y != NULL && !dm_true(dv_ptr_y, ey)) continue;
  ex = ez-ey;
  mark_words[ex - min_x] = 1;
  }
  }
  for (ex = min_x+1; ex < max_x; ex++){
  if (mark_words[ex - min_x] == 0){
  domain_set_false_noint(dv_ptr_x, ex);
  }
  }
  printf("<= AC_ADD "); write_term(X); printf(" "); write_term(Y); printf(" "); write_term(Z); printf("\n");
  return BP_TRUE;
  
  lab_ac_on_z:
  size = max_z - min_z + 1;
  if (size > NUM_MARK_WORDS) return BP_TRUE;         // no enough space for marking 

  for (i = 0; i < size; i++){
  mark_words[i] = 0;                               // initialize the marks, no value is supported 
  }
  
  for (ex = min_x; ex <= max_x; ex++){
  if (dv_ptr_x != NULL && !dm_true(dv_ptr_x, ex)) continue;
  for (ey = min_y; ey <= max_y; ey++){
  if (dv_ptr_y != NULL && !dm_true(dv_ptr_y, ey)) continue;
  ez = ex+ey;
  mark_words[ez - min_z] = 1;
  }
  }
  for (ez = min_z+1; ez < max_z; ez++){
  if (mark_words[ez - min_z] == 0){
  domain_set_false_noint(dv_ptr_z, ez);
  }
  }
  printf("<= AC_ADD "); write_term(X); printf(" "); write_term(Y); printf(" "); write_term(Z); printf("\n");
  return BP_TRUE;
  }
*/

