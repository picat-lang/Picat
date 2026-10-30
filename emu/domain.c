/********************************************************************
 *   File   : domain.c
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2026
 *   Purpose: Primitives on finite domains

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/

#include <string.h>
#include <math.h>
#include "basic.h"
#include "term.h"
#include "bapi.h"
#include "event.h"
#include "clpfd.h"

/* functor k/2, used to build hash keys in cpcon_decrement_counter (CP solver competition) */
static SYM_REC_PTR cpcon_k2_ptr;

#define DOMAIN_INCLUDE(dv_ptr1, dv_ptr2) (DV_size(dv_ptr1) >= DV_size(dv_ptr2) && \
                                          DV_first(dv_ptr1) <= DV_first(dv_ptr2) && \
                                          DV_last(dv_ptr1) >= DV_last(dv_ptr2) && \
                                          (IS_IT_DOMAIN(dv_ptr1) || domain_include_bv(dv_ptr1, dv_ptr2)))

/******** c_DM_CREATE *************/
/* Var :: From..To, creating the domain directly as a bit vector. */
int c_DM_CREATE_BV_DVAR() {
    BPLONG Var, From, To;
    BPLONG_PTR top;

    Var = ARG(1, 3);
    From = ARG(2, 3);
    To = ARG(3, 3);

    DEREF(Var); DEREF(From); DEREF(To);
    if (ISINT(From) && ISINT(To)) {
        From = INTVAL(From); To = INTVAL(To);
        return aux_create_bv_domain_var(Var, From, To);
    } else {
        bp_exception = integer_overflow;
        return BP_ERROR;
    }
}

/* C-callable wrapper for b_DM_CREATE_DVAR/3. */
int c_DM_CREATE_DVAR() {
    BPLONG Var, From, To;

    Var = ARG(1, 3);
    From = ARG(2, 3);
    To = ARG(3, 3);

    return b_DM_CREATE_DVAR(Var, From, To);
}

/* Var :: From..To. A non-integer bound means unbounded (BP_MININT_1W or BP_MAXINT_1W);
   From == To binds Var. */
int b_DM_CREATE_DVAR(BPLONG Var, BPLONG From, BPLONG To)
{
    BPLONG_PTR top;

    DEREF(Var);
    DEREF(From); DEREF(To);

    if (ISINT(From)) {
        From = INTVAL(From);
    } else {
        //    printf("%% WARNING: lower bound changed to: %ld\n",BP_MININT_1W);
        From = BP_MININT_1W;
    }
    if (ISINT(To)) {
        To = INTVAL(To);
    } else {
        //    printf("%%  WARNING: upper bound changed to: %ld\n",BP_MAXINT_1W);
        To = BP_MAXINT_1W;
    }
    if (From == To) {
        return unify(Var, MAKEINT(From));
    }
    return aux_create_domain_var(Var, From, To);
}

/* Vars :: From..To for every variable in the list Vars. */
int c_DM_CREATE_DVARS() {
    BPLONG Vars, From, To, Var;
    BPLONG_PTR sreg;
    BPLONG_PTR top;
    int res;

    Vars = ARG(1, 3);
    From = ARG(2, 3);
    To = ARG(3, 3);

    DEREF(From); DEREF(To);
    if (ISINT(From)) {
        From = INTVAL(From);
    } else {
        //    printf("\% WARNING: lower bound changed to: %ld\n",BP_MININT_1W);
        From = BP_MININT_1W;
    }
    if (ISINT(To)) {
        To = INTVAL(To);
    } else {
        //    printf("\% WARNING: upper bound changed to: %ld\n",BP_MAXINT_1W);
        To = BP_MAXINT_1W;
    }
    DEREF(Vars);
    while (ISLIST(Vars)) {
        sreg = (BPLONG_PTR)UNTAGGED_ADDR(Vars);
        Var = *sreg;
        DEREF(Var);
        Vars = *(sreg+1);
        DEREF(Vars);
        res = aux_create_domain_var(Var, From, To);
        if (res != 1) return res;
    }
    if (ISNIL(Vars)) return 1;
    bp_exception = illegal_arguments;
    return BP_ERROR;
}

/* Gives Var the interval domain from..to: creates a domain variable from a plain
   variable or a suspension variable without a domain, intersects an existing domain,
   or checks an integer. The Prolog callers ensure from < to. */
int aux_create_domain_var(BPLONG Var, BPLONG from, BPLONG to)
{
    BPLONG_PTR dv_ptr;

    if (ISREF(Var)) {
        CREATE_SUSP_VAR_nocs(Var);
        DV_first(dv_ptr) = from;
        DV_last(dv_ptr) = to;
        DV_size(dv_ptr) = to-from+1;
        DV_type(dv_ptr) = IT_DOMAIN;
        return 1;
    } else if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        if (IS_UN_DOMAIN(dv_ptr)) {
            DV_first(dv_ptr) = from;
            DV_last(dv_ptr) = to;
            DV_size(dv_ptr) = to-from+1;
            top = A_DV_type(dv_ptr);
            PUSHTRAIL_H_ATOMIC(top, FOLLOW(top));
            DV_type(dv_ptr) = IT_DOMAIN;
            return 1;
        } else {
            return domain_region_noint(dv_ptr, from, to);
        }
    } else if (ISINT(Var)) {
        BPLONG v = INTVAL(Var);
        return (v >= from) && (v <= to);
    } else {
        bp_exception = illegal_arguments; return BP_ERROR;
    }
}

/* Same as aux_create_domain_var, but a new domain is created as a bit vector. */
int aux_create_bv_domain_var(BPLONG Var, BPLONG from, BPLONG to)
{
    BPLONG_PTR dv_ptr;

    if (ISREF(Var)) {
        CREATE_SUSP_VAR_nocs(Var);
        DV_first(dv_ptr) = from;
        DV_last(dv_ptr) = to;
        DV_size(dv_ptr) = to-from+1;
        it_to_bv(dv_ptr);
        return 1;
    } else if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        if (IS_UN_DOMAIN(dv_ptr)) {
            DV_first(dv_ptr) = from;
            DV_last(dv_ptr) = to;
            DV_size(dv_ptr) = to-from+1;
            it_to_bv(dv_ptr);
            return 1;
        } else {
            return domain_region_noint(dv_ptr, from, to);
        }
    } else if (ISINT(Var)) {
        BPLONG v = INTVAL(Var);
        return (v >= from && v <= to);
    } else {
        bp_exception = illegal_arguments; return BP_ERROR;
    }
}

/* Makes a copy of the domain variable at dv_ptr on the heap, sharing its bit vector and
   attributes but with empty constraint lists. Used when two domain variables are unified. */
BPLONG_PTR dm_clone(BPLONG_PTR dv_ptr)
{
    BPLONG_PTR dv_ptr_cp;

    dv_ptr_cp = heap_top;
    DV_var(dv_ptr_cp) = ((BPLONG)heap_top | SUSP);
    DV_type(dv_ptr_cp) = DV_type(dv_ptr);  /* share the same bit vector */
    DV_first(dv_ptr_cp) = DV_first(dv_ptr);
    DV_last(dv_ptr_cp) = DV_last(dv_ptr);
    DV_size(dv_ptr_cp) = DV_size(dv_ptr);
    DV_ins_cs(dv_ptr_cp) = nil_sym;
    DV_minmax_cs(dv_ptr_cp) = nil_sym;
    DV_dom_cs(dv_ptr_cp) = nil_sym;
    DV_outer_dom_cs(dv_ptr_cp) = nil_sym;
    DV_attached(dv_ptr_cp) = DV_attached(dv_ptr);
    heap_top += SIZE_OF_DV;
    return dv_ptr_cp;
}

/* C-callable wrapper for create_susp_var/1. */
int c_create_susp_var() {
    BPLONG Var;

    Var = ARG(1, 1);
    return create_susp_var(Var);
}

/* Turns a plain variable into a suspension variable (no domain, no constraints).
   Fails if Var is not a plain variable. */
int create_susp_var(BPLONG Var)
{
    BPLONG_PTR dv_ptr;

    DEREF(Var);
    if (ISREF(Var)) {
        CREATE_SUSP_VAR_nocs(Var);
        return 1;
    } else
        return 0;
}

/* fd_vector_min_max(Low,Up): widens the range (fd_region_low..fd_region_up) of
   domains considered small; unbound arguments are bound to the current range. */
int fd_vector_min_max() {
    BPLONG low, up;
    BPLONG_PTR top;

    low = ARG(1, 2);
    up = ARG(2, 2);
    DEREF(low); DEREF(up);
    if (ISINT(low)) {
        if (fd_region_low > INTVAL(low)) fd_region_low = INTVAL(low);
    }
    if (ISINT(up)) {
        if (fd_region_up < INTVAL(up)) fd_region_up = INTVAL(up);
    }
    if (!ISINT(low)) {
        unify(low, MAKEINT(fd_region_low));
    }
    if (!ISINT(up)) {
        unify(up, MAKEINT(fd_region_up));
    }
    return 1;
}

/* Converts the interval domain at dv_ptr to a bit vector with all its values present.
   The bit vector covers whole words, so it may extend beyond first..last. */
void it_to_bv(BPLONG_PTR dv_ptr)
{
    BPLONG from, to, i, size;
    BPLONG_PTR top;

    from = DV_first(dv_ptr);
    to = DV_last(dv_ptr);
    /*
      if (from<fd_region_low || to>fd_region_up){
      domain_region_aux(dv_ptr,fd_region_low,fd_region_up);
      from = DV_first(dv_ptr);
      to = DV_last(dv_ptr);
      fprintf(stderr,"WARNING: Domain is reduced to %d..%d. Solutions may be lost. \n",from,to);
      fprintf(stderr,"         Use fd_vector_min_max(Min,Max) to increase the bit-vector's size\n");
      }
    */
    top = A_DV_bit_vector_ptr(dv_ptr);
    from = WORD_NUMBER(from);
    to = WORD_NUMBER(to);

    PUSHTRAIL_H_BIT_VECTOR(top, FOLLOW(top));
    FOLLOW(top) = (BPLONG)heap_top;

    FOLLOW(heap_top++) = from*NBITS_IN_LONG;  /* BV_low_val */
    FOLLOW(heap_top++) = (to+1)*NBITS_IN_LONG-1;  /* BV_up_val */

    size = (to-from+1);
    if (local_top-heap_top-size <= SMALL_MARGIN) {
        myquit(STACK_OVERFLOW, "ib");
    }
    for (i = from; i <= to; i++) {
        *heap_top++ = MASK_FF;
    }
}


/* Converts the interval domain at dv_ptr to a bit vector with elm removed, posting a
   dom event. Precondition: DV_first(dv_ptr) < elm < DV_last(dv_ptr). */
void it_to_bv_with_hole(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG from, to, i, size;
    BPLONG_PTR top;
    BPLONG_PTR w_ptr;
    BPULONG offset;

    from = DV_first(dv_ptr);
    to = DV_last(dv_ptr);

    /*
      if (from<fd_region_low || to>fd_region_up){
      domain_region_aux(dv_ptr,fd_region_low,fd_region_up);
      from = DV_first(dv_ptr);
      to = DV_last(dv_ptr);
      fprintf(stderr,"WARNING: Domain is reduced to %d..%d. Solutions may be lost. \n",from,to);
      fprintf(stderr,"         Use fd_vector_min_max(Min,Max) to increase the bit-vector's size\n");
      }
    */
    //  printf("it_to_bv_with_hole %d %d \n",from,to);

    top = A_DV_bit_vector_ptr(dv_ptr);
    from = WORD_NUMBER(from);
    to = WORD_NUMBER(to);
    PUSHTRAIL_H_BIT_VECTOR(top, FOLLOW(top));
    FOLLOW(top) = (BPLONG)heap_top;

    offset = elm-from*NBITS_IN_LONG;

    FOLLOW(heap_top++) = from*NBITS_IN_LONG;  /* BV_low_val */
    FOLLOW(heap_top++) = (to+1)*NBITS_IN_LONG-1;  /* BV_up_val */

    size = to-from+1;
    if (local_top-heap_top-size <= SMALL_MARGIN) {
        myquit(STACK_OVERFLOW, "ib");
    }
    w_ptr = heap_top+offset/NBITS_IN_LONG;
    for (i = from; i <= to; i++) {
        *heap_top++ = MASK_FF;
    }
    FOLLOW(w_ptr) = ~((BPULONG)1 << (offset%NBITS_IN_LONG));
    INSERT_TRIGGER_dom(dv_ptr, MAKEINT(elm));

    size = DV_size(dv_ptr);
    UPDATE_SIZE(dv_ptr, size, size-1);
}

/* Converts an interval domain to a bit vector with interval_start..interval_end removed,
   posting a dom event for each removed value.
   Precondition: DV_first(dv_ptr) < interval_start <= interval_end < DV_last(dv_ptr). */
void it_to_bv_with_interval_holes(BPLONG_PTR dv_ptr, BPLONG interval_start, BPLONG interval_end)
{
    BPLONG from, to, i, size;
    BPLONG_PTR top;
    BPLONG_PTR w_ptr;
    BPULONG offset, w;

    from = DV_first(dv_ptr);
    to = DV_last(dv_ptr);

    top = A_DV_bit_vector_ptr(dv_ptr);
    from = WORD_NUMBER(from);
    to = WORD_NUMBER(to);
    PUSHTRAIL_H_BIT_VECTOR(top, FOLLOW(top));
    FOLLOW(top) = (BPLONG)heap_top;

    FOLLOW(heap_top++) = from*NBITS_IN_LONG;  /* BV_low_val */
    FOLLOW(heap_top++) = (to+1)*NBITS_IN_LONG-1;  /* BV_up_val */

    size = to-from+1;
    if (local_top-heap_top-size <= LARGE_MARGIN) {
        myquit(STACK_OVERFLOW, "ib");
    }
    offset = interval_start-from*NBITS_IN_LONG;
    w_ptr = heap_top+offset/NBITS_IN_LONG;
    for (i = from; i <= to; i++) {
        *heap_top++ = MASK_FF;
    }

    w = (BPULONG)FOLLOW(w_ptr);
    offset = offset%NBITS_IN_LONG;
    for (i = interval_start; i <= interval_end; i++) {
        w &= ~((BPULONG)1 << offset);
        INSERT_TRIGGER_dom(dv_ptr, MAKEINT(i));
        offset++;
        if (offset == NBITS_IN_LONG) {
            FOLLOW(w_ptr) = w;
            offset = 0; w_ptr++;
            w = FOLLOW(w_ptr);
        }
    }
    FOLLOW(w_ptr) = w;
    size = DV_size(dv_ptr);
    UPDATE_SIZE(dv_ptr, size, size-(interval_end-interval_start+1));
}

/* Succeeds if Var1 and Var2 (domain variables or integers) have no common value. */
int b_DM_DISJOINT_cc(BPLONG Var1, BPLONG Var2)
{
    BPLONG_PTR dv_ptr1, dv_ptr2;

    DEREF_NONVAR(Var1);
    DEREF_NONVAR(Var2);

    if (IS_SUSP_VAR(Var1)) {
        dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var1);
        if (IS_SUSP_VAR(Var2)) {
            dv_ptr2 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var2);
            return dm_disjoint(dv_ptr1, dv_ptr2);
        }
        /* Var2 must be an int */
        return (dm_true(dv_ptr1, INTVAL(Var2)) ? BP_FALSE : BP_TRUE);
    }
    /* Var1 must be an int */
    if (IS_SUSP_VAR(Var2)) {
        dv_ptr2 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var2);
        return (dm_true(dv_ptr2, INTVAL(Var1)) ? BP_FALSE : BP_TRUE);
    }
    /* Var2 must be an int */
    return (Var1 == Var2 ? BP_FALSE : BP_TRUE);
}

/******** domain_include  *************/
/* C-callable wrapper for b_DM_INCLUDE/2. */
int c_DM_INCLUDE() {
    BPLONG Var1, Var2;

    Var1 = ARG(1, 2);
    Var2 = ARG(2, 2);

    return b_DM_INCLUDE(Var1, Var2);
}

/* Succeeds if Var1's domain is a superset of Var2's domain (either may be an integer). */
int b_DM_INCLUDE(BPLONG Var1, BPLONG Var2)
{
    BPLONG_PTR dv_ptr1, dv_ptr2;

    DEREF_NONVAR(Var1); DEREF_NONVAR(Var2);

    if (IS_SUSP_VAR(Var1)) {
        if (IS_SUSP_VAR(Var2)) {
            dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var1);
            dv_ptr2 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var2);
            return DOMAIN_INCLUDE(dv_ptr1, dv_ptr2);
        } else {
            dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var1);
            return dm_true(dv_ptr1, INTVAL(Var2));
        }
    }
    return Var1 == Var2 ? BP_TRUE : BP_FALSE;
}

/* Succeeds if every element of dv_ptr2's domain is in dv_ptr1's domain.
   Preconditions: (1) dv_ptr1 has at least the same size as dv_ptr2;
   (2) dv_ptr1's range contains dv_ptr2's range; and
   (3) dv_ptr1 is a bit vector.
   When both bit vectors have the same base, whole words are compared at once.
*/
int domain_include_bv(BPLONG_PTR dv_ptr1, BPLONG_PTR dv_ptr2)
{
    BPLONG elm, last;
    BPLONG_PTR w_ptr1, bv_ptr1, w_ptr2, bv_ptr2, last_w_ptr2;
    BPULONG w1, offset, w2, last_w2, last_offset, mask;

    elm = DV_first(dv_ptr2);
    last = DV_last(dv_ptr2);
    bv_ptr1 = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr1);
    bv_ptr2 = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr2);
    if (IS_BV_DOMAIN(dv_ptr2) && BV_low_val(bv_ptr1) == BV_low_val(bv_ptr2)) {
        WORD_OFFSET(bv_ptr1, elm, w1, w_ptr1, offset);
        mask = (MASK_FF << offset);
        w1 &= mask;
        w_ptr2 = bv_ptr2 + (w_ptr1-bv_ptr1);
        w2 = FOLLOW(w_ptr2);
        w2 &= mask;
        WORD_OFFSET(bv_ptr2, last, last_w2, last_w_ptr2, last_offset);
        while (w_ptr2 != last_w_ptr2) {
            /* printf("w1 = %x w2 =%x (~w1 & w2)=%x\n",w1,w2,~w1&w2); */
            if ((~w1 & w2) != 0) return 0;  /* not included */
            w_ptr1++; w1 = FOLLOW(w_ptr1);
            w_ptr2++; w2 = FOLLOW(w_ptr2);
        }
        mask = ((BPULONG)MASK_FF >> (NBITS_IN_LONG-last_offset-1));
        w1 &= mask;
        w2 &= mask;
        /*    printf("w1 = %x w2 =%x (~w1 & w2)=%x\n",w1,w2,~w1&w2); */
        if ((~w1 & w2) != 0) return 0;  /* not included */
        return 1;
    } else {
        if (!(dm_true_bv_nbt(dv_ptr1, elm) && dm_true_bv_nbt(dv_ptr1, last))) return 0;

        elm++;
        if (IS_IT_DOMAIN(dv_ptr2)) {
            while (elm < last) {
                if (!dm_true_bv_nbt(dv_ptr1, elm)) return 0;
                elm++;
            };
            return 1;
        } else {  /* both are bit vectors */
            while (elm < last) {
                elm = domain_next_bv(dv_ptr2, elm);
                if (!dm_true_bv_nbt(dv_ptr1, elm)) return 0;
                elm++;
            }
            return 1;
        }
    }
}

/******** set_false *************/
/* Excludes elm from X. X must be either an integer or a domain variable. */
int varorint_set_false(BPLONG X, BPLONG elm)
{
    BPLONG_PTR dv_ptr;
    DEREF_NONVAR(X);
    if (ISINT(X)) {
        if (INTVAL(X) == elm) return BP_FALSE; return BP_TRUE;
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    domain_set_false_noint(dv_ptr, elm);
    return BP_TRUE;
}


/* Excludes elm from the domain at dv_ptr, posting the matching events.
   Precondition: dv_ptr's variable is not bound. */
void domain_set_false_noint(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG first, last, count;
    BPLONG_PTR top;

    first = DV_first(dv_ptr);
    last = DV_last(dv_ptr);
    if (elm > first && elm < last) {
        if (IS_IT_DOMAIN(dv_ptr)) {
            it_to_bv_with_hole(dv_ptr, elm);
            return;
        } else {
            exclude_inner_elm_bv(dv_ptr, elm);
            return;
        }
    }
    if (elm == first) {
        count = DV_size(dv_ptr);
        if (count == 2) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(DV_last(dv_ptr)));
            return;
        }
        INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(elm));
        if (IS_IT_DOMAIN(dv_ptr)) {
            elm++;
        } else {
            elm = domain_next_bv(dv_ptr, elm+1);
        }
        UPDATE_FIRST_SIZE(dv_ptr, first, elm, count-1);
        return;
    }
    if (elm == last) {
        count = DV_size(dv_ptr);
        if (count == 2) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(first));
            return;
        }
        INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(elm));
        if (IS_IT_DOMAIN(dv_ptr)) {
            elm--;
        } else {
            elm = domain_prev_bv(dv_ptr, elm-1);
        }
        UPDATE_LAST_SIZE(dv_ptr, last, elm, count-1);
        return;
    }
}

/* Same as domain_set_false_noint, but dv_ptr's variable may be bound, in which case
   the call fails if its value is elm. */
int domain_set_false_aux(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG first, last, count;
    BPLONG_PTR top;

    if (ISINT(FOLLOW(dv_ptr))) {
        return (INTVAL(FOLLOW(dv_ptr)) == elm) ? BP_FALSE : BP_TRUE;
    }
    first = DV_first(dv_ptr);
    last = DV_last(dv_ptr);
    if (elm > first && elm < last) {
        if (IS_IT_DOMAIN(dv_ptr)) {
            it_to_bv_with_hole(dv_ptr, elm);
            return 1;
        } else {
            exclude_inner_elm_bv(dv_ptr, elm);
            return 1;
        }
    }
    if (elm == first) {
        count = DV_size(dv_ptr);
        if (count == 2) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(DV_last(dv_ptr)));
            return 1;
        }
        INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(elm));
        if (IS_IT_DOMAIN(dv_ptr)) {
            elm++;
        } else {
            elm = domain_next_bv(dv_ptr, elm+1);
        }
        UPDATE_FIRST_SIZE(dv_ptr, first, elm, count-1);
        return 1;
    }
    if (elm == last) {
        count = DV_size(dv_ptr);
        if (count == 2) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(first));
            return 1;
        }
        INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(elm));
        if (IS_IT_DOMAIN(dv_ptr)) {
            elm--;
        } else {
            elm = domain_prev_bv(dv_ptr, elm-1);
        }
        UPDATE_LAST_SIZE(dv_ptr, last, elm, count-1);
        return 1;
    }
    return 1;
}

/* Same as domain_set_false_aux, for a bit-vector domain (dv_ptr's variable may be bound). */
int domain_set_false_bv(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG first, last, count;
    BPLONG_PTR top;

    if (ISINT(FOLLOW(dv_ptr))) {
        return (INTVAL(FOLLOW(dv_ptr)) == elm) ? BP_FALSE : BP_TRUE;
    }
    first = DV_first(dv_ptr);
    last = DV_last(dv_ptr);
    if (elm > first && elm < last) {
        exclude_inner_elm_bv(dv_ptr, elm);
        return 1;
    }
    if (elm == first) {
        count = DV_size(dv_ptr);
        if (count == 2) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(DV_last(dv_ptr)));
            return 1;
        }
        INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(elm));
        if (IS_IT_DOMAIN(dv_ptr)) {
            elm++;
        } else {
            elm = domain_next_bv(dv_ptr, elm+1);
        }
        UPDATE_FIRST_SIZE(dv_ptr, first, elm, count-1);
        return 1;
    }
    if (elm == last) {
        count = DV_size(dv_ptr);
        if (count == 2) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(first));
            return 1;
        }
        INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(elm));
        if (IS_IT_DOMAIN(dv_ptr)) {
            elm--;
        } else {
            elm = domain_prev_bv(dv_ptr, elm-1);
        }
        UPDATE_LAST_SIZE(dv_ptr, last, elm, count-1);
        return 1;
    }
    return 1;
}


/* Removes the smallest element of a bit-vector domain.
   Precondition: dv_ptr's variable is not instantiated yet. */
void domain_exclude_first_bv(BPLONG_PTR dv_ptr)
{
    BPLONG count, elm, first;
    BPULONG w, mask, offset;
    BPLONG_PTR top, w_ptr, bv_ptr;

    count = DV_size(dv_ptr);
    if (count == 2) {
        ASSIGN_DVAR(dv_ptr, MAKEINT(DV_last(dv_ptr)));
        return;
    }
    first = DV_first(dv_ptr);
    INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(first));
    elm = first+1;
    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    BV_NEXT_IN(bv_ptr, elm, w, w_ptr, offset, mask);
    UPDATE_FIRST_SIZE(dv_ptr, first, elm, count-1);
}

/* Removes the largest element of a bit-vector domain.
   Precondition: dv_ptr's variable is not instantiated yet. */
void domain_exclude_last_bv(BPLONG_PTR dv_ptr)
{
    BPLONG count, elm, last;
    BPLONG_PTR top, bv_ptr, w_ptr;
    BPULONG w, offset, mask;

    count = DV_size(dv_ptr);
    if (count == 2) {
        ASSIGN_DVAR(dv_ptr, MAKEINT(DV_first(dv_ptr)));
        return;
    }
    last = DV_last(dv_ptr);
    INSERT_TRIGGER_outer_dom(dv_ptr, MAKEINT(last));
    elm = last-1;
    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    BV_PREV_IN(bv_ptr, elm, w, w_ptr, offset, mask);
    UPDATE_LAST_SIZE(dv_ptr, last, elm, count-1);
}

/* Clears the bit of elm in a bit-vector domain, posting a dom event if it was set.
   Precondition: DV_first(dv_ptr) < elm < DV_last(dv_ptr). */
void exclude_inner_elm_bv(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG count;
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, mask, offset, w1;

    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, elm, w, w_ptr, offset);
    mask = ((BPULONG)0x1 << offset);
    w1 = (w & ~mask);
    if (w == w1) return;  /* already out */
    PUSHTRAIL_H_ATOMIC(w_ptr, w);
    FOLLOW(w_ptr) = w1;
    INSERT_TRIGGER_dom(dv_ptr, MAKEINT(elm));
    count = DV_size(dv_ptr);
    UPDATE_SIZE(dv_ptr, count, count-1);
}

/* Removes from Y every element that is also in X (Y := Y minus X). */
int exclude_dom_aux(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y)
{
    BPLONG first, last, first_y, last_y;

    first = DV_first(dv_ptr_x); last = DV_last(dv_ptr_x);
    first_y = DV_first(dv_ptr_y); last_y = DV_last(dv_ptr_y);
    if (first > last_y || first_y > last) return BP_TRUE;

    first = (first > first_y) ? first : first_y;
    last = (last < last_y) ? last : last_y;

    while (first <= last && !dm_true(dv_ptr_x, first)) first++;
    if (first > last) return BP_TRUE;
    for (; ; ) {
        if (domain_set_false_aux(dv_ptr_y, first) == BP_FALSE) return BP_FALSE;
        if (ISINT(FOLLOW(dv_ptr_y))) {  /* Y got instantiated: its value must not be in X */
            return dm_true(dv_ptr_x, INTVAL(FOLLOW(dv_ptr_y))) ? BP_FALSE : BP_TRUE;
        }
        if (first >= last) return BP_TRUE;
        first++;
        if (!IS_IT_DOMAIN(dv_ptr_x)) first = domain_next_bv(dv_ptr_x, first);
    }
}

/* Removes from Y every element that is not in X (Y := Y intersect X). Used when two domain
   variables are unified. */
int exclude_dom_comp_aux(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y)
{
    BPLONG first, last;
    /*  write_term(dv_ptr_x); write_term(dv_ptr_y); */

    first = DV_first(dv_ptr_y); last = DV_last(dv_ptr_y);
    for (; ; ) {  /* walk Y's elements only */
        if (!dm_true(dv_ptr_x, first)) {
            if (domain_set_false_aux(dv_ptr_y, first) == BP_FALSE) return BP_FALSE;
            if (ISINT(FOLLOW(dv_ptr_y))) {  /* Y got instantiated: its value must be in X */
                return dm_true(dv_ptr_x, INTVAL(FOLLOW(dv_ptr_y)));
            }
        }
        if (first >= last) return BP_TRUE;
        first++;
        if (!IS_IT_DOMAIN(dv_ptr_y)) first = domain_next_bv(dv_ptr_y, first);
    }
}

/******** next *************/
/* NextE is the smallest element of DVar's domain greater than E; fails if there is none. */
int b_DM_NEXT_ccf(BPLONG DVar, BPLONG E, BPLONG NextE)
{
    BPLONG_PTR dv_ptr;
    BPLONG elm;
    BPLONG_PTR top;

    DEREF(DVar); DEREF(E);
    if (!IS_SUSP_VAR(DVar)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(DVar);
    elm = INTVAL(E);
    if (elm < DV_first(dv_ptr)) {
        elm = DV_first(dv_ptr);
        ASSIGN_f_atom(NextE, MAKEINT(elm));
        return BP_TRUE;
    } else if (elm >= DV_last(dv_ptr)) {
        return BP_FALSE;
    }
    if (IS_IT_DOMAIN(dv_ptr))
        elm += 1;
    else
        elm = domain_next_bv(dv_ptr, elm+1);
    ASSIGN_f_atom(NextE, MAKEINT(elm));
    return 1;
}

/* Returns the smallest element >= elm in a bit-vector domain. The caller must make sure
   such an element exists (e.g. elm <= DV_last), or the scan runs past the bit vector. */
BPLONG domain_next_bv(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG_PTR bv_ptr, w_ptr;
    BPULONG w, mask, offset;

    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, elm, w, w_ptr, offset);

    /*  printf("low=%d up=%d elm=%d offset=%d w=%x\n",BV_low_val(bv_ptr),BV_up_val(bv_ptr),elm,offset,w);
     */
    NEXT_IN_WORD(elm, w, w_ptr, offset, mask);
    NEXT_IN_ELM(elm, w, offset, mask);
    return elm;
}

/******** prev *************/
/* Same as b_DM_PREV_ccf, also counting a backtrack (used when labeling downwards). */
int b_DM_PREV0_ccf(BPLONG DVar, BPLONG E, BPLONG PrevE)
{
    n_backtracks++;
    return b_DM_PREV_ccf(DVar, E, PrevE);
}

/* PrevE is the largest element of DVar's domain smaller than E; fails if there is none. */
int b_DM_PREV_ccf(BPLONG DVar, BPLONG E, BPLONG PrevE)
{
    BPLONG_PTR dv_ptr;
    BPLONG elm;
    BPLONG_PTR top;

    DEREF(DVar); DEREF(E);
    if (!IS_SUSP_VAR(DVar)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(DVar);
    elm = INTVAL(E);
    if (elm > DV_last(dv_ptr)) {
        elm = DV_last(dv_ptr);
        ASSIGN_f_atom(PrevE, MAKEINT(elm));
        return BP_TRUE;
    } else if (elm <= DV_first(dv_ptr))
        return 0;
    if (IS_IT_DOMAIN(dv_ptr))
        elm -= 1;
    else
        elm = domain_prev_bv(dv_ptr, elm-1);
    ASSIGN_f_atom(PrevE, MAKEINT(elm));
    return BP_TRUE;
}

/* Returns the largest element <= elm in a bit-vector domain. The caller must make sure
   such an element exists (e.g. elm >= DV_first). */
BPLONG domain_prev_bv(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, mask, offset;

    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, elm, w, w_ptr, offset);
    PREV_IN_WORD(elm, w, w_ptr, offset, mask);
    PREV_IN_ELM(elm, w, offset, mask);
    return elm;
}

/******** true *************/
/* Succeeds if E is in Var's domain; if Var is not a domain variable, unifies it with E. */
int b_DM_TRUE_cc(BPLONG Var, BPLONG E)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(Var);
    DEREF(E);
    if (!IS_SUSP_VAR(Var)) {
        return unify(Var, E);
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
    return dm_true(dv_ptr, INTVAL(E));
}

/* Succeeds if E is not in Var's domain (or differs from Var if Var is not a domain variable). */
int b_DM_FALSE_cc(BPLONG Var, BPLONG E)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(Var);
    DEREF(E);
    if (!IS_SUSP_VAR(Var)) {
        return (Var != E) ? BP_TRUE : BP_FALSE;
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
    return !dm_true(dv_ptr, INTVAL(E));
}

/* Succeeds if E is in Var's domain and is neither its minimum nor its maximum
   (used by CLP(Set), whose domains carry dummy end elements). */
int b_DM_INNER_TRUE_cc(BPLONG Var, BPLONG E)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(Var);
    DEREF(E);
    if (!ISINT(E)) return 0;
    E = INTVAL(E);
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
    return (DV_first(dv_ptr) != E &&
            DV_last(dv_ptr) != E &&
            dm_true(dv_ptr, E));
}

/* Tests if elm is in X. X must be either an integer or a domain variable. */
int varorint_dm_true(BPLONG X, BPLONG elm)
{
    BPLONG_PTR dv_ptr;

    DEREF_NONVAR(X);
    if (ISINT(X)) {
        return INTVAL(X) == elm;
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    return dm_true(dv_ptr, elm);
}


/* Returns 1 if elm is in the domain at dv_ptr (interval or bit vector). */
int dm_true(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, mask, offset;

    if (elm <= DV_first(dv_ptr)) {
        if (elm == DV_first(dv_ptr)) return BP_TRUE;
        return BP_FALSE;
    }
    if (elm >= DV_last(dv_ptr)) {
        if (elm == DV_last(dv_ptr)) return BP_TRUE;
        return BP_FALSE;
    }

    if (IS_IT_DOMAIN(dv_ptr))
        return 1;

    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, elm, w, w_ptr, offset);
    mask = ((BPULONG)0x1 << offset);
    return ((w & mask) != 0);
}

/* Same as dm_true, for a bit-vector domain. */
int dm_true_bv(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, mask, offset;

    if (elm <= DV_first(dv_ptr)) {
        if (elm == DV_first(dv_ptr)) return 1;
        return 0;
    }
    if (elm >= DV_last(dv_ptr)) {
        if (elm == DV_last(dv_ptr)) return 1;
        return 0;
    }
    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, elm, w, w_ptr, offset);
    mask = ((BPULONG)0x1 << offset);
    return ((w & mask) != 0);
}


/* Same as dm_true_bv, but without the bound tests: elm must lie within the bit vector. */
int dm_true_bv_nbt(BPLONG_PTR dv_ptr, BPLONG elm)
{
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, mask, offset;

    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, elm, w, w_ptr, offset);
    mask = ((BPULONG)0x1 << offset);
    return ((w & mask) != 0);
}

/******** min_max *************/
/* Min and Max are the bounds of Var's domain (Var itself if it is an integer). */
int b_DM_MIN_MAX_cff(BPLONG Var, BPLONG Min, BPLONG Max)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(Var);
    if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        if (!IS_UN_DOMAIN(dv_ptr)) {
            ASSIGN_f_atom(Min, MAKEINT(DV_first(dv_ptr)));
            ASSIGN_f_atom(Max, MAKEINT(DV_last(dv_ptr)));
            return 1;
        } else {
            bp_exception = illegal_arguments; return BP_ERROR;
        }
    } else if (ISINT(Var)) {
        ASSIGN_f_atom(Min, Var);
        ASSIGN_f_atom(Max, Var);
        return 1;
    }
    bp_exception = illegal_arguments; return BP_ERROR;
}

/******** min *************/
/* Min is the lower bound of Var's domain (Var itself if it is an integer). */
int b_DM_MIN_cf(BPLONG Var, BPLONG Min)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(Var);
    if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        if (!IS_UN_DOMAIN(dv_ptr)) {
            ASSIGN_f_atom(Min, MAKEINT(DV_first(dv_ptr)));
            return 1;
        } else {
            bp_exception = illegal_arguments; return BP_ERROR;
        }
    } else if (ISINT(Var)) {
        ASSIGN_f_atom(Min, Var);
        return 1;
    }
    bp_exception = illegal_arguments; return BP_ERROR;
}

/******** max *************/
/* Max is the upper bound of Var's domain (Var itself if it is an integer). */
int b_DM_MAX_cf(BPLONG Var, BPLONG Max)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(Var);
    if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        if (!IS_UN_DOMAIN(dv_ptr)) {
            ASSIGN_f_atom(Max, MAKEINT(DV_last(dv_ptr)));
            return 1;
        } else {
            bp_exception = illegal_arguments; return BP_ERROR;
        }
    } else if (ISINT(Var)) {
        ASSIGN_f_atom(Max, Var);
        return 1;
    }
    bp_exception = illegal_arguments; return BP_ERROR;
}

/******** count *************/
/* Count is the size of Var's domain (1 if Var is an integer). */
int b_DM_COUNT_cf(BPLONG Var, BPLONG Count)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;
    BPLONG size;

    DEREF(Var);
    if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        size = MAKEINT(DV_size(dv_ptr));
    } else if (ISINT(Var)) {
        size = BP_ONE;
    } else {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    ASSIGN_f_atom(Count, size);
    return BP_TRUE;
}

/* number of 1 bits in w */
static int count_bits(BPULONG w)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll((unsigned long long)w);
#else
    int n = 0;
    while (w != 0) {w &= w-1; n++;}
    return n;
#endif
}

/* Returns the number of elements of a bit-vector domain within from..to (from <= to,
   both within the bit vector), counting whole words at once. */
int count_domain_elms(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, offset, last_offset;
    BPLONG_PTR last_w_ptr;
    int count;

    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, from, w, w_ptr, offset);
    WORD_OFFSET(bv_ptr, to, w, last_w_ptr, last_offset);
    w = FOLLOW(w_ptr) & (MASK_FF << offset);  /* drop the bits below from */
    count = 0;
    while (w_ptr != last_w_ptr) {
        count += count_bits(w);
        w_ptr++; w = FOLLOW(w_ptr);
    }
    w &= ((BPULONG)MASK_FF >> (NBITS_IN_LONG-last_offset-1));  /* drop the bits above to */
    return count + count_bits(w);
}

/* Same as count_domain_elms, but also posts an outer_dom event for each element counted
   (the caller is about to remove them all). */
int exclude_domain_elms(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{
    BPLONG_PTR w_ptr, bv_ptr;
    BPULONG w, mask, offset;
    int count;

    count = 0;
    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, from, w, w_ptr, offset);
    mask = ((BPULONG)0x1 << offset);
    while (from <= to) {
        if (mask == 1 && w == 0) {  /* skip a whole empty word */
            from += NBITS_IN_LONG;
            w_ptr++; w = FOLLOW(w_ptr);
            continue;
        }
        if ((w & mask) != 0) {
            count++;
            INSERT_TRIGGER_outer_dom0(dv_ptr, MAKEINT(from));
        }
        mask <<= 1;
        if (mask == 0) {
            mask = (BPULONG)0x1; w_ptr++; w = FOLLOW(w_ptr);
        }
        from++;
    }
    return count;
}

/*
  c_reachability_test(VarsVect): VarsVect = {X1,...,Xn}, where Xi's domain holds the
  possible successors of node i. Succeeds if every node can be reached from node 1
  (breadth-first search), as required for the nodes to form a single circuit.
*/
int c_reachability_test() {
    BPLONG N, VarsVect, i, count, var, elm, last;
    BPLONG_PTR var_vect_ptr, scc_nodes, front, rear, dv_ptr;
    SYM_REC_PTR sym_ptr;
    VarsVect = ARG(1, 1);
    //    printf("=>reachability"); write_term(VarsVect); printf("\n");
    DEREF_NONVAR(VarsVect);
    var_vect_ptr = (BPLONG_PTR)UNTAGGED_ADDR(VarsVect);
    sym_ptr = (SYM_REC_PTR)FOLLOW(var_vect_ptr);
    N = GET_ARITY(sym_ptr);

    QUIT_IF_OVERFLOW_HEAP(2*N+100);
    scc_nodes = heap_top;
    scc_nodes[1] = 1;
    for (i = 2; i <= N; i++) {
        scc_nodes[i] = 0;  /* 0 means not reached yet */
    }
    front = rear = heap_top+N+1;
    FOLLOW(rear++) = 1; count = 1;
    while (front < rear) {
        i = FOLLOW(front++);
        var = FOLLOW(var_vect_ptr+i); DEREF_NONVAR(var);
        if (IS_SUSP_VAR(var)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(var);
            elm = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            for (; ; ) {
                if (scc_nodes[elm] == 0) {  /* not reached yet */
                    scc_nodes[elm] = 1;
                    FOLLOW(rear++) = elm; count++;
                    if (count == N) return BP_TRUE;
                }
                if (elm == last) break;
                elm++;
                if (!IS_IT_DOMAIN(dv_ptr)) elm = domain_next_bv(dv_ptr, elm);
            }
        } else {
            elm = INTVAL(var);
            if (scc_nodes[elm] == 0) {
                scc_nodes[elm] = 1;
                FOLLOW(rear++) = elm; count++;
                if (count == N) return BP_TRUE;
            }
        }
    }
    //    printf("<=reachability %ld\n", count);
    if (count == N) return BP_TRUE; else return BP_FALSE;
}

/*
  c_subcircuit_test(Begin,VarsVect): nodes that are not reachable from Begin cannot be
  in the circuit, so each such node i is fixed to point to itself (Xi = i).
*/
int c_subcircuit_test() {
    BPLONG N, Begin, VarsVect, i, var, elm, last;
    BPLONG_PTR var_vect_ptr, scc_nodes, front, rear, dv_ptr;
    SYM_REC_PTR sym_ptr;

    Begin = ARG(1, 2);
    VarsVect = ARG(2, 2);
    DEREF_NONVAR(Begin);
    DEREF_NONVAR(VarsVect);

    //  printf("subcircuit "); write_term(Begin); write_term(VarsVect); printf("\n");
    var_vect_ptr = (BPLONG_PTR)UNTAGGED_ADDR(VarsVect);
    sym_ptr = (SYM_REC_PTR)FOLLOW(var_vect_ptr);
    N = GET_ARITY(sym_ptr);
    Begin = INTVAL(Begin);

    QUIT_IF_OVERFLOW_HEAP(2*N+100);
    scc_nodes = heap_top;
    for (i = 1; i <= N; i++) {
        scc_nodes[i] = 0;  /* 0 means not reachable */
    }
    scc_nodes[Begin] = 1;
    front = rear = heap_top+N+1;
    FOLLOW(rear++) = Begin;
    while (front < rear) {
        i = FOLLOW(front++);
        var = FOLLOW(var_vect_ptr+i); DEREF_NONVAR(var);
        if (IS_SUSP_VAR(var)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(var);
            elm = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
            for (; ; ) {
                if (scc_nodes[elm] == 0) {  /* not visited yet */
                    scc_nodes[elm] = 1;
                    FOLLOW(rear++) = elm;
                }
                if (elm == last) break;
                elm++;
                if (!IS_IT_DOMAIN(dv_ptr)) elm = domain_next_bv(dv_ptr, elm);
            }
        } else {
            elm = INTVAL(var);
            if (scc_nodes[elm] == 0) {
                scc_nodes[elm] = 1;
                FOLLOW(rear++) = elm;
            }
        }
    }
    for (i = 1; i <= N; i++) {
        if (scc_nodes[i] == 0) {
            elm = FOLLOW(var_vect_ptr+i); DEREF_NONVAR(elm);
            if (IS_SUSP_VAR(elm)) {
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(elm);
                if (!dm_true(dv_ptr, i)) return BP_FALSE;
                ASSIGN_DVAR(dv_ptr, MAKEINT(i));
            } else {
                if (INTVAL(elm) != i) return BP_FALSE;
            }
        }
    }
    return BP_TRUE;
}


/*
  c_path_from_to_reachability_test(F,T,Vector): succeeds if node T can be reached from
  node F in the graph whose successor domains are in Vector.
*/
int c_path_from_to_reachability_test() {
    BPLONG from, to, N, Vector;
    BPLONG_PTR visited;
    BPLONG_PTR var_vector;
    SYM_REC_PTR sym_ptr;
    BPLONG i;

    from = ARG(1, 3); DEREF_NONVAR(from); from = INTVAL(from);
    to = ARG(2, 3); DEREF_NONVAR(to); to = INTVAL(to);
    if (from == to) return BP_TRUE;
    Vector = ARG(3, 3); DEREF_NONVAR(Vector);
    var_vector = (BPLONG_PTR)UNTAGGED_ADDR(Vector);
    sym_ptr = (SYM_REC_PTR)FOLLOW(var_vector);
    N = GET_ARITY(sym_ptr);

    QUIT_IF_OVERFLOW_HEAP(2*N+100);
    visited = heap_top;
    for (i = 1; i <= N; i++) {  /* initialize the array */
        visited[i] = 0;
    }
    visited[0] = 1;  /* 0 is a dummy node */
    visited[from] = 1;
    return path_from_to_reachable(from, to, var_vector, visited, visited+N+1);
}

/* Breadth-first search from node from for node to. visited[] marks the nodes already
   queued; queue must have room for all the nodes. */
int path_from_to_reachable(BPLONG from, BPLONG to, BPLONG_PTR var_vector, BPLONG_PTR visited, BPLONG_PTR queue)
{
    BPLONG var, next, last;
    BPLONG_PTR dv_ptr, front, rear;

    front = rear = queue;
    *rear++ = from;
    while (front < rear) {
        var = FOLLOW(var_vector + *front++); DEREF_NONVAR(var);
        if (IS_SUSP_VAR(var)) {
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(var);
            next = DV_first(dv_ptr);
            last = DV_last(dv_ptr);
        } else {
            next = last = INTVAL(var);
            dv_ptr = NULL;
        }
        for (; ; ) {
            if (next == to) return BP_TRUE;
            if (visited[next] == 0) {  /* not yet visited */
                visited[next] = 1;
                *rear++ = next;
            }
            if (next == last) break;
            next++;
            if (!IS_IT_DOMAIN(dv_ptr)) next = domain_next_bv(dv_ptr, next);
        }
    }
    return BP_FALSE;
}

/* A directed graph is given as a structure v(node(V1,Neibs_1),...,node(Vn,Neibs_n))
   where Vi is a domain variable (the node's label) and Neibs_i is a domain variable
   whose domain contains the successors that can be directly reached from i. For each
   connection (Start,End,Lab), the nodes are labeled so that there is a path from Start
   to End in which all the nodes are labeled Lab. The Prolog frame is as follows:

   paths_from_to_in_dgraph_propagator(ConnectionVec,Graph,Vars),{ins(Vars)} => b_PATH_FROM_TO_REACHABLE.
*/
int b_PATH_FROM_TO_REACHABLE()
{
    BPLONG ConnectionVec, Start, End, n, nConnections, Graph, Lab;
    BPLONG_PTR VisitedArray, LabVarArray, SuccVarArray;
    BPLONG_PTR ConStartArray, ConEndArray, ConLabArray, ConFlagArray;
    BPLONG_PTR convec_struct_ptr, graph_struct_ptr;
    SYM_REC_PTR sym_ptr;
    BPLONG i;

    /*  printf("=>from_to n"); */

    ConnectionVec = FOLLOW(arreg+2); DEREF_NONVAR(ConnectionVec);
    Graph = FOLLOW(arreg+1); DEREF_NONVAR(Graph);

    convec_struct_ptr = (BPLONG_PTR)UNTAGGED_ADDR(ConnectionVec);
    sym_ptr = (SYM_REC_PTR)FOLLOW(convec_struct_ptr);
    nConnections = GET_ARITY(sym_ptr);


    graph_struct_ptr = (BPLONG_PTR)UNTAGGED_ADDR(Graph);
    sym_ptr = (SYM_REC_PTR)FOLLOW(graph_struct_ptr);
    n = GET_ARITY(sym_ptr);

    /*  printf("=>from_to %d \%d n", n, nConnections); */

    QUIT_IF_OVERFLOW_HEAP(5*n+4*nConnections+100);
    ConStartArray = local_top-nConnections-1;
    ConEndArray = ConStartArray-nConnections-1;
    ConLabArray = ConEndArray-nConnections-1;
    ConFlagArray = ConLabArray-nConnections-1;
    VisitedArray = ConFlagArray-n-1;
    LabVarArray = VisitedArray-n-1;
    SuccVarArray = LabVarArray-n-1;
    for (i = 1; i <= n; i++) {
        BPLONG node, SuccVar, LabVar;
        BPLONG_PTR node_ptr;
        node = FOLLOW(graph_struct_ptr+i); DEREF_NONVAR(node);
        node_ptr = (BPLONG_PTR)UNTAGGED_ADDR(node);
        LabVar = FOLLOW(node_ptr+1); DEREF_NONVAR(LabVar);
        LabVarArray[i] = LabVar;
        SuccVar = FOLLOW(node_ptr+2); DEREF_NONVAR(SuccVar);
        SuccVarArray[i] = SuccVar;
    }

    /* ConnectionVec=v(v(Start_1,End_1,Lab_1,Flag_1),v(Start_2,End_2,Lab_2,Flag_2),...) */
    for (i = 1; i <= nConnections; i++) {
        BPLONG Con, Flag, TaggedLab;
        BPLONG_PTR con_struct_ptr;

        Con = FOLLOW(convec_struct_ptr+i); DEREF_NONVAR(Con);
        con_struct_ptr = (BPLONG_PTR)UNTAGGED_ADDR(Con);

        Start = FOLLOW(con_struct_ptr+1); DEREF_NONVAR(Start); Start = INTVAL(Start); ConStartArray[i] = Start;
        End = FOLLOW(con_struct_ptr+2); DEREF_NONVAR(End); End = INTVAL(End); ConEndArray[i] = End;
        TaggedLab = FOLLOW(con_struct_ptr+3); DEREF_NONVAR(TaggedLab); ConLabArray[i] = Lab = INTVAL(TaggedLab);
        Flag = FOLLOW(con_struct_ptr+4); DEREF(Flag); ConFlagArray[i] = Flag;

        if (ISREF(Flag)) {
            if (check_reach_cuts_in_one_connection(n, Start, End, Lab, TaggedLab, LabVarArray, SuccVarArray, VisitedArray) == BP_FALSE) return BP_FALSE;
        }
    }
    /* too expensive
       printf("=>two cuts %d \%d n", n, nConnections);
       for (i=1;i<nConnections;i++){
       BPLONG Flag;
       Flag = ConFlagArray[i];
       if (ISREF(Flag)) {
       check_two_cuts_in_two_connections(n,i,nConnections,ConStartArray,ConEndArray,ConLabArray,ConFlagArray,LabVarArray,SuccVarArray,VisitedArray);
       }
       }
    */
    /*  printf("<=from_to \n"); */
    return BP_TRUE;
}


/* For connection (Start,End,Lab): removes Lab from the labels of nodes that can't be
   reached from Start through Lab-labeled nodes, and fixes to Lab every cutting node,
   i.e. one whose removal disconnects End from Start. */
int check_reach_cuts_in_one_connection(BPLONG n, BPLONG Start, BPLONG End, BPLONG Lab, BPLONG TaggedLab, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray, BPLONG_PTR VisitedArray)
{
    BPLONG i;

    /*
      printf("=>path_label Start=%d End=%d n=%d\n",Start,End,n);
      write_term(Graph);
      printf("\n");
    */
    for (i = 1; i <= n; i++) {
        VisitedArray[i] = 0;
    }

    check_reach_withno_cut(Start, Lab, TaggedLab, LabVarArray, SuccVarArray, VisitedArray);

    /* no node that is unreachable can be marked with Lab */
    for (i = 1; i <= n; i++) {
        if (VisitedArray[i] == 0) {
            BPLONG LabVar;
            LabVar = LabVarArray[i];
            if (ISINT(LabVar)) {
                if (LabVar == TaggedLab) return BP_FALSE;  /* this node labeled Lab is unreachable */
            } else {
                BPLONG_PTR dv_ptr;
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                domain_set_false_noint(dv_ptr, Lab);
                LabVarArray[i] = FOLLOW(dv_ptr);
            }
        }
    }

    /* check for cutting nodes: a node that every path from Start to End must go through */
    for (i = 1; i <= n; i++) {
        BPLONG LabVar;
        BPLONG_PTR dv_ptr;
        if (i != Start && i != End && path_from_to_node_degree(n, i, Lab, TaggedLab, LabVarArray, SuccVarArray) == 2) {
            LabVar = LabVarArray[i];
            if (IS_SUSP_VAR(LabVar)) {
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                if (dm_true(dv_ptr, Lab)) {  /* test if i is a cutting node only if the domain contains Lab */
                    BPLONG j;
                    for (j = 1; j <= n; j++) VisitedArray[j] = 0;
                    check_reach_with_one_cut(Start, Lab, TaggedLab, LabVarArray, SuccVarArray, VisitedArray, i);
                    if (VisitedArray[End] == 0) {  /* i is a cutting node */
                        // printf("cnf %d ",i);
                        ASSIGN_DVAR(dv_ptr, TaggedLab);
                        LabVarArray[i] = TaggedLab;
                    }
                }
            }
        }
    }
    /*  printf("<= path_label\n"); */
    return BP_TRUE;
}

/* Returns the number of successors of node NodeNum that can still be labeled Lab. */
int path_from_to_node_degree(BPLONG n, BPLONG NodeNum, BPLONG Lab, BPLONG TaggedLab, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray)
{
    BPLONG LabVar, SuccVar;

    SuccVar = SuccVarArray[NodeNum];
    if (IS_SUSP_VAR(SuccVar)) {
        BPLONG_PTR dv_ptr;
        BPLONG curNodeNum, lastNodeNum, degree;

        degree = 0;
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SuccVar);
        curNodeNum = DV_first(dv_ptr);
        lastNodeNum = DV_last(dv_ptr);
        for (; ; ) {
            LabVar = LabVarArray[curNodeNum];
            if (ISINT(LabVar)) {
                if (LabVar == TaggedLab) {
                    degree++;
                }
            } else {
                BPLONG_PTR lab_dv_ptr;
                lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                if (dm_true(lab_dv_ptr, Lab)) {
                    degree++;
                }
            }
            if (curNodeNum == lastNodeNum) return degree;
            curNodeNum++;
            if (!IS_IT_DOMAIN(dv_ptr)) curNodeNum = domain_next_bv(dv_ptr, curNodeNum);
        }
    } else {  /* only one successor */
        BPLONG curNodeNum;
        curNodeNum = INTVAL(SuccVar);
        if (curNodeNum < 1 || curNodeNum > n) quit("Strange ");
        LabVar = LabVarArray[curNodeNum];
        if (ISINT(LabVar)) {
            if (LabVar == TaggedLab) {
                return 1;
            }
        } else {
            BPLONG_PTR lab_dv_ptr;
            lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
            if (dm_true(lab_dv_ptr, Lab)) {
                return 1;
            }
        }
        return 0;
    }
}

/* Marks in VisitedArray the nodes reachable from Start through nodes that can be labeled Lab. */
void check_reach_withno_cut(BPLONG Start, BPLONG Lab, BPLONG TaggedLab, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray, BPLONG_PTR VisitedArray)
{
    BPLONG curNodeNum, LabVar, SuccVar;
    BPLONG_PTR q_front, q_rear;

    q_front = q_rear = SuccVarArray-1;  /* use the free stack area below the arrays as the queue */
    FOLLOW(q_rear--) = Start;
    VisitedArray[Start] = 1;
    while (q_front > q_rear) {
        curNodeNum = FOLLOW(q_front--);
        SuccVar = SuccVarArray[curNodeNum];
        if (ISINT(SuccVar)) {  /* only one successor */
            BPLONG succNodeNum = INTVAL(SuccVar);
            if (VisitedArray[succNodeNum] == 0) {
                LabVar = LabVarArray[succNodeNum];
                if (ISINT(LabVar)) {
                    if (LabVar == TaggedLab) {
                        VisitedArray[succNodeNum] = 1;
                        FOLLOW(q_rear--) = succNodeNum;
                    }
                } else {
                    BPLONG_PTR lab_dv_ptr;
                    lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                    if (dm_true(lab_dv_ptr, Lab)) {
                        VisitedArray[succNodeNum] = 1;
                        FOLLOW(q_rear--) = succNodeNum;
                    }
                }
            }
        } else {  /* multiple successor nodes */
            BPLONG_PTR dv_ptr;
            BPLONG succNodeNum, lastNodeNum;

            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SuccVar);
            succNodeNum = DV_first(dv_ptr);
            lastNodeNum = DV_last(dv_ptr);


            for (; ; ) {
                if (VisitedArray[succNodeNum] == 0) {
                    LabVar = LabVarArray[succNodeNum];
                    if (ISINT(LabVar)) {
                        if (LabVar == TaggedLab) {
                            VisitedArray[succNodeNum] = 1;
                            FOLLOW(q_rear--) = succNodeNum;
                        }
                    } else {
                        BPLONG_PTR lab_dv_ptr;
                        lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                        if (dm_true(lab_dv_ptr, Lab)) {
                            VisitedArray[succNodeNum] = 1;
                            FOLLOW(q_rear--) = succNodeNum;
                        }
                    }
                }
                if (succNodeNum == lastNodeNum) break;  /* exit for(;;) */
                succNodeNum++;
                if (!IS_IT_DOMAIN(dv_ptr)) succNodeNum = domain_next_bv(dv_ptr, succNodeNum);
            }
        }
    }
}

/* Same as check_reach_withno_cut, with node cutNodeNum removed from the graph. */
void check_reach_with_one_cut(BPLONG Start, BPLONG Lab, BPLONG TaggedLab, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray, BPLONG_PTR VisitedArray, BPLONG cutNodeNum)
{
    BPLONG curNodeNum, LabVar, SuccVar;
    BPLONG_PTR q_front, q_rear;

    q_front = q_rear = SuccVarArray-1;  /* use the free stack area below the arrays as the queue */
    FOLLOW(q_rear--) = Start;
    VisitedArray[Start] = 1;
    while (q_front > q_rear) {
        curNodeNum = FOLLOW(q_front--);
        SuccVar = SuccVarArray[curNodeNum];
        if (ISINT(SuccVar)) {  /* only one successor */
            BPLONG succNodeNum = INTVAL(SuccVar);
            if (VisitedArray[succNodeNum] == 0 && succNodeNum != cutNodeNum) {
                LabVar = LabVarArray[succNodeNum];
                if (ISINT(LabVar)) {
                    if (LabVar == TaggedLab) {
                        VisitedArray[succNodeNum] = 1;
                        FOLLOW(q_rear--) = succNodeNum;
                    }
                } else {
                    BPLONG_PTR lab_dv_ptr;
                    lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                    if (dm_true(lab_dv_ptr, Lab)) {
                        VisitedArray[succNodeNum] = 1;
                        FOLLOW(q_rear--) = succNodeNum;
                    }
                }
            }
        } else {  /* multiple successor nodes */
            BPLONG_PTR dv_ptr;
            BPLONG succNodeNum, lastNodeNum;

            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SuccVar);
            succNodeNum = DV_first(dv_ptr);
            lastNodeNum = DV_last(dv_ptr);


            for (; ; ) {
                if (VisitedArray[succNodeNum] == 0 && succNodeNum != cutNodeNum) {
                    LabVar = LabVarArray[succNodeNum];
                    if (ISINT(LabVar)) {
                        if (LabVar == TaggedLab) {
                            VisitedArray[succNodeNum] = 1;
                            FOLLOW(q_rear--) = succNodeNum;
                        }
                    } else {
                        BPLONG_PTR lab_dv_ptr;
                        lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                        if (dm_true(lab_dv_ptr, Lab)) {
                            VisitedArray[succNodeNum] = 1;
                            FOLLOW(q_rear--) = succNodeNum;
                        }
                    }
                }
                if (succNodeNum == lastNodeNum) break;  /* exit for(;;) */
                succNodeNum++;
                if (!IS_IT_DOMAIN(dv_ptr)) succNodeNum = domain_next_bv(dv_ptr, succNodeNum);
            }
        }
    }
}

/* For connection firstCon, looks for pairs of nodes that together cut every path from
   Start to End (currently disabled as too expensive). */
void check_two_cuts_in_two_connections(BPLONG n, BPLONG firstCon, BPLONG nConnections, BPLONG_PTR ConStartArray, BPLONG_PTR ConEndArray, BPLONG_PTR ConLabArray, BPLONG_PTR ConFlagArray, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray, BPLONG_PTR VisitedArray)
{
    BPLONG Start, End, Lab, TaggedLab, LabVar, v1, v2;
    BPLONG_PTR dv_ptr;

    Start = ConStartArray[firstCon];
    End = ConEndArray[firstCon];
    Lab = ConLabArray[firstCon];
    TaggedLab = MAKEINT(Lab);


    /* check if <v1,v2> are cutting nodes */
    for (v1 = 1; v1 < n; v1++) {
        if (v1 != Start && v1 != End) {
            LabVar = LabVarArray[v1];
            if (IS_SUSP_VAR(LabVar)) {
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                if (DV_size(dv_ptr) > 2 && dm_true(dv_ptr, Lab)) {
                    for (v2 = v1+1; v2 <= n; v2++) {
                        if (v2 != Start && v2 != End) {
                            LabVar = LabVarArray[v2];
                            if (IS_SUSP_VAR(LabVar)) {
                                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                                if (DV_size(dv_ptr) > 2 && dm_true(dv_ptr, Lab)) {
                                    int i;
                                    for (i = 1; i <= n; i++) VisitedArray[i] = 0;
                                    check_reach_with_two_cuts(Start, Lab, TaggedLab, LabVarArray, SuccVarArray, VisitedArray, v1, v2);
                                    if (VisitedArray[End] == 0) {  /* v1 and v2 are cutting nodes */
                                        check_cuts_in_another_connection(v1, v2, n, firstCon, Lab, nConnections, ConStartArray, ConEndArray, ConLabArray, ConFlagArray, LabVarArray, SuccVarArray, VisitedArray);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/* v1 and v2 have been found to be cutting nodes for connection firstCon with label l1.
   Check if they are also cutting nodes for another connection with label l2. If so,
   then no labels other than l1 and l2 are allowed for v1 and v2.
*/
void check_cuts_in_another_connection(BPLONG v1, BPLONG v2, BPLONG n, BPLONG firstCon, BPLONG firstLab, BPLONG nConnections, BPLONG_PTR ConStartArray, BPLONG_PTR ConEndArray, BPLONG_PTR ConLabArray, BPLONG_PTR ConFlagArray, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray, BPLONG_PTR VisitedArray)
{
    BPLONG secondCon, LabVar, Start, End, Lab, TaggedLab, Flag;
    BPLONG_PTR dv_ptr1, dv_ptr2;

    for (secondCon = firstCon+1; secondCon <= nConnections; secondCon++) {
        Flag = ConFlagArray[secondCon];
        if (ISREF(Flag)) {
            Start = ConStartArray[secondCon];
            End = ConEndArray[secondCon];
            Lab = ConLabArray[secondCon];
            TaggedLab = MAKEINT(Lab);

            LabVar = LabVarArray[v1];
            if (IS_SUSP_VAR(LabVar)) {
                dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                if (DV_size(dv_ptr1) > 2 && dm_true(dv_ptr1, Lab)) {
                    LabVar = LabVarArray[v2];
                    if (IS_SUSP_VAR(LabVar)) {
                        dv_ptr2 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                        if (DV_size(dv_ptr2) > 2 && dm_true(dv_ptr2, Lab)) {
                            int i;
                            for (i = 1; i <= n; i++) VisitedArray[i] = 0;
                            check_reach_with_two_cuts(Start, Lab, TaggedLab, LabVarArray, SuccVarArray, VisitedArray, v1, v2);
                            if (VisitedArray[End] == 0) {  /* v1 and v2 are also cutting nodes of the jth connection */
                                domain_reduce_to_two(dv_ptr1, firstLab, Lab);
                                LabVarArray[v1] = FOLLOW(dv_ptr1);
                                domain_reduce_to_two(dv_ptr2, firstLab, Lab);
                                LabVarArray[v2] = FOLLOW(dv_ptr2);
                                /* printf("TWO CUTTING NODES %d %d\n",v1,v2); */
                            }
                        }
                    }
                }
            }
        }
    }
}


/* Same as check_reach_withno_cut, with nodes cutNodeNum1 and cutNodeNum2 removed. */
void check_reach_with_two_cuts(BPLONG Start, BPLONG Lab, BPLONG TaggedLab, BPLONG_PTR LabVarArray, BPLONG_PTR SuccVarArray, BPLONG_PTR VisitedArray, BPLONG cutNodeNum1, BPLONG cutNodeNum2)
{
    BPLONG curNodeNum, LabVar, SuccVar;
    BPLONG_PTR q_front, q_rear;

    q_front = q_rear = SuccVarArray-1;  /* use the free stack area below the arrays as the queue */
    FOLLOW(q_rear--) = Start;
    VisitedArray[Start] = 1;
    while (q_front > q_rear) {
        curNodeNum = FOLLOW(q_front--);
        SuccVar = SuccVarArray[curNodeNum];
        if (ISINT(SuccVar)) {  /* only one successor */
            BPLONG succNodeNum = INTVAL(SuccVar);
            if (VisitedArray[succNodeNum] == 0 && succNodeNum != cutNodeNum1 && succNodeNum != cutNodeNum2) {
                LabVar = LabVarArray[succNodeNum];
                if (ISINT(LabVar)) {
                    if (LabVar == TaggedLab) {
                        VisitedArray[succNodeNum] = 1;
                        FOLLOW(q_rear--) = succNodeNum;
                    }
                } else {
                    BPLONG_PTR lab_dv_ptr;
                    lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                    if (dm_true(lab_dv_ptr, Lab)) {
                        VisitedArray[succNodeNum] = 1;
                        FOLLOW(q_rear--) = succNodeNum;
                    }
                }
            }
        } else {  /* multiple successor nodes */
            BPLONG_PTR dv_ptr;
            BPLONG succNodeNum, lastNodeNum;

            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(SuccVar);
            succNodeNum = DV_first(dv_ptr);
            lastNodeNum = DV_last(dv_ptr);


            for (; ; ) {
                if (VisitedArray[succNodeNum] == 0 && succNodeNum != cutNodeNum1 && succNodeNum != cutNodeNum2) {
                    LabVar = LabVarArray[succNodeNum];
                    if (ISINT(LabVar)) {
                        if (LabVar == TaggedLab) {
                            VisitedArray[succNodeNum] = 1;
                            FOLLOW(q_rear--) = succNodeNum;
                        }
                    } else {
                        BPLONG_PTR lab_dv_ptr;
                        lab_dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(LabVar);
                        if (dm_true(lab_dv_ptr, Lab)) {
                            VisitedArray[succNodeNum] = 1;
                            FOLLOW(q_rear--) = succNodeNum;
                        }
                    }
                }
                if (succNodeNum == lastNodeNum) break;  /* exit for(;;) */
                succNodeNum++;
                if (!IS_IT_DOMAIN(dv_ptr)) succNodeNum = domain_next_bv(dv_ptr, succNodeNum);
            }
        }
    }
}

/* Excludes all elements but val1 and val2 (val1 != val2). */
void domain_reduce_to_two(BPLONG_PTR dv_ptr, BPLONG val1, BPLONG val2)
{
    BPLONG t;
    // printf("reduce_to_two %d %d ",val1,val2); write_term1(dv_ptr,stderr); printf("\n");
    if (val1 > val2) {
        t = val1; val1 = val2; val2 = t;
    }
    for (t = val1+1; t < val2; t++) {
        domain_set_false_aux(dv_ptr, t);
    }
    domain_region_aux(dv_ptr, val1, val2);
    //  printf("<=reduce_to_two %d %d ",val1,val2); write_term1(dv_ptr,stderr); printf("\n");
}

/* C implementation of the following Prolog code. X's domain is a Hall set if exactly
   |dom(X)| variables in L and R (including X) have domains contained in it; its values
   are then removed from all the other variables.

  $alldistinct_check_hall_var(X,NumElms,L,R):-
  dvar(X) :
  b_DM_COUNT_cf(X,Size),
  c_alldistinct_count_subsets(X,L,R,Size,NumElms,M),
  (M>Size -> 
  fail
  ;
  M=:=Size ->
  $alldistinct_exclude_hall_set(X,L),
  $alldistinct_exclude_hall_set(X,R)
  ;
  true).
  $alldistinct_check_hall_var(_X,_NumElms,_L,_R):-
  true : true.

  $alldistinct_exclude_hall_set(X,[]):-true : true.
  $alldistinct_exclude_hall_set(X,[Y|Ys]):-dvar(Y) :
  (b_DM_INCLUDE(X,Y)->true; c_exclude_dom(X,Y)),
  $alldistinct_exclude_hall_set(X,Ys).
  $alldistinct_exclude_hall_set(X,[_|Ys]):-true : 
  $alldistinct_exclude_hall_set(X,Ys).
*/
int b_ALLDISTINCT_CHECK_HALL_VAR_cccc(BPLONG X, BPLONG NumElmsLeft, BPLONG L, BPLONG R)
{
    BPLONG L0, NumElmsLeft0;
    BPLONG_PTR dv_ptr1, dv_ptr2, ptr, flag_ptr;
    BPLONG elm, size, count, processing_part;

    /*
      printf("=>HALL_VAR ");
      write_term(X); printf(" ");
      write_term(NumElmsLeft); printf(" ");
      write_term(L); printf(" ");
      write_term(R); printf("\n");
    */
    DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;  /* does nothing */
    dv_ptr1 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    DEREF_NONVAR(NumElmsLeft); NumElmsLeft = INTVAL(NumElmsLeft);
    DEREF_NONVAR(L); DEREF_NONVAR(R);

    L0 = L; NumElmsLeft0 = NumElmsLeft;
    ALIGN(CHAR_PTR, curr_fence);
    flag_ptr = (BPLONG_PTR)curr_fence;  /* scratch flags: which domains are subsets of X's */

    size = DV_size(dv_ptr1);
    count = 1;  /* X is a subset of itself */
    processing_part = 1;  /* counting L or R */
start_count:
    while (ISLIST(L)) {
        if (count+NumElmsLeft < size) return BP_TRUE;  /* not a Hall set */
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(L);
        elm = FOLLOW(ptr); DEREF_NONVAR(elm);
        if (IS_SUSP_VAR(elm)) {
            dv_ptr2 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(elm);
            if (DOMAIN_INCLUDE(dv_ptr1, dv_ptr2)) {
                count++;
                FOLLOW(flag_ptr+NumElmsLeft) = 1;
            } else {
                FOLLOW(flag_ptr+NumElmsLeft) = 0;
            }
        }
        L = FOLLOW(ptr+1); DEREF_NONVAR(L);
        NumElmsLeft--;
    }
    if (processing_part == 1) {
        L = R;
        processing_part = 2;
        goto start_count;
    }

    if (count > size) return BP_FALSE;
    if (count < size) return BP_TRUE;

    processing_part = 1;
    NumElmsLeft = NumElmsLeft0;
    L = L0;
start_exclude:
    while (ISLIST(L)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(L);
        elm = FOLLOW(ptr); DEREF_NONVAR(elm);
        if (IS_SUSP_VAR(elm)) {
            dv_ptr2 = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(elm);
            if (FOLLOW(flag_ptr+NumElmsLeft) == 0) {  /* dv_ptr2 is not a subset of dv_ptr1 */
                if (exclude_dom_aux(dv_ptr1, dv_ptr2) == BP_FALSE) return BP_FALSE;
            }
        }
        L = FOLLOW(ptr+1); DEREF_NONVAR(L);
        NumElmsLeft--;
    }
    if (processing_part == 1) {
        L = R;
        processing_part = 2;
        goto start_exclude;
    }
    return BP_TRUE;
}

/******** misc *************/
/* Debugging: prints the domain of op, if it is a domain variable. */
void print_domain_var(BPLONG op)
{
    BPLONG_PTR dv_ptr;
    BPLONG_PTR top;

    DEREF(op);
    if (IS_SUSP_VAR(op)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(op);
        if (IS_UN_DOMAIN(dv_ptr)) write_term(op);
        else print_domain(dv_ptr);
    }
}

/* Prints a domain as ::[a..b,c,...], merging consecutive values into intervals. */
void print_domain(BPLONG_PTR dv_ptr)
{
    BPLONG i, first, last;
    BPLONG low, high;

    first = DV_first(dv_ptr);
    last = DV_last(dv_ptr);

    if (IS_IT_DOMAIN(dv_ptr)) {
        fprintf(curr_out, "::[" BPLONG_FMT_STR " .." BPLONG_FMT_STR "]", first, last);
    } else {
        low = first;
        i = first+1;
        fprintf(curr_out, "::[");
        while (i < last) {
            while(i < last && dm_true(dv_ptr, i)) i++;
            if (i == last) break;
            high = i-1;
            if (low == high) fprintf(curr_out, BPLONG_FMT_STR ",", low);
            else fprintf(curr_out, BPLONG_FMT_STR ".." BPLONG_FMT_STR ",", low, high);
            i++;
            while (!dm_true(dv_ptr, i) && i < last) i++;
            low = i;
        }
        high = last;
        if (low == high) fprintf(curr_out, BPLONG_FMT_STR "]", low);
        else fprintf(curr_out, BPLONG_FMT_STR ".." BPLONG_FMT_STR "]", low, high);
    }
}

/* X := (X intersect Y). Each of X and Y may be an integer or a domain variable. */
int b_DM_INTERSECT(BPLONG X, BPLONG Y)
{
    DEREF(Y); DEREF(X);
    if (ISINT(X)) {
        if (ISINT(Y)) {
            return (X == Y) ? BP_TRUE : BP_FALSE;
        } else if (IS_SUSP_VAR(Y)) {
            BPLONG_PTR dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
            return (dm_true(dv_ptr_y, INTVAL(X))) ? BP_TRUE : BP_FALSE;
        }
    } else if (IS_SUSP_VAR(X)) {
        BPLONG_PTR dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
        if (ISINT(Y)) {
            if (dm_true(dv_ptr_x, INTVAL(Y))) {
                ASSIGN_DVAR(dv_ptr_x, Y);
                return BP_TRUE;
            } else return BP_FALSE;
        } else if (IS_SUSP_VAR(Y)) {
            BPLONG_PTR dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
            return dm_intersect(dv_ptr_x, dv_ptr_y);
        }
    }
    return BP_FALSE;
}

/* X := (X /\ Y) for two domain variables; fails if the result is empty. */
int dm_intersect(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y)
{
    BPLONG first, last;

    if (IS_IT_DOMAIN(dv_ptr_y)) {
        return domain_region_noint(dv_ptr_x, DV_first(dv_ptr_y), DV_last(dv_ptr_y));
    }
    first = DV_first(dv_ptr_y);
    if (first < DV_first(dv_ptr_x)) first = DV_first(dv_ptr_x);
    last = DV_last(dv_ptr_y);
    if (last > DV_last(dv_ptr_x)) last = DV_last(dv_ptr_x);
    if (first > last) return BP_FALSE;
    if (domain_region_noint(dv_ptr_x, first, last) == BP_FALSE) return BP_FALSE;
    if (ISINT(FOLLOW(dv_ptr_x))) return dm_true(dv_ptr_y, INTVAL(FOLLOW(dv_ptr_x)));
    first = DV_first(dv_ptr_x);
    last = DV_last(dv_ptr_x);
    for (; ; ) {  /* walk X's elements only */
        if (!dm_true(dv_ptr_y, first)) {
            if (domain_set_false_aux(dv_ptr_x, first) == BP_FALSE) return BP_FALSE;
            if (ISINT(FOLLOW(dv_ptr_x))) {  /* X got instantiated: its value must be in Y */
                return dm_true(dv_ptr_y, INTVAL(FOLLOW(dv_ptr_x)));
            }
        }
        if (first >= last) return BP_TRUE;
        first++;
        if (!IS_IT_DOMAIN(dv_ptr_x)) first = domain_next_bv(dv_ptr_x, first);
    }
}

/* X in {Low,Up}: restricts X to the two values Low and Up (Low <= Up). */
int b_DM_INTERSECT2(BPLONG X, BPLONG Low, BPLONG Up)
{
    BPLONG_PTR dv_ptr;
    DEREF_NONVAR(Low);
    DEREF_NONVAR(Up);
    DEREF_NONVAR(X);
    if (IS_SUSP_VAR(X)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    } else {
        return (X == Low || X == Up) ? BP_TRUE : BP_FALSE;
    }
    if (Low == Up) {
        if (dm_true(dv_ptr, INTVAL(Low))) {
            ASSIGN_DVAR(dv_ptr, Low);
            return BP_TRUE;
        } else {
            return BP_FALSE;
        }
    }

    Low = INTVAL(Low);
    Up = INTVAL(Up);
    if (dm_true(dv_ptr, Low)) {
        if (dm_true(dv_ptr, Up)) {
            domain_region_noint(dv_ptr, Low, Up);
            Low++; Up--;
            if (Low <= Up) domain_exclude_interval_aux(dv_ptr, Low, Up);
            return BP_TRUE;
        } else {
            ASSIGN_DVAR(dv_ptr, MAKEINT(Low));
            return BP_TRUE;
        }
    } else {
        if (dm_true(dv_ptr, Up)) {
            ASSIGN_DVAR(dv_ptr, MAKEINT(Up));
            return BP_TRUE;
        } else
            return BP_FALSE;
    }
}

/* Succeeds if the domains of dv_ptr1 and dv_ptr2 have no common element. */
int dm_disjoint(BPLONG_PTR dv_ptr1, BPLONG_PTR dv_ptr2)
{
    BPLONG min, max, min2, max2;
    BPLONG_PTR w_ptr1, bv_ptr1, w_ptr2, bv_ptr2, last_w_ptr1;
    BPULONG w1, offset, w2, last_w1, last_offset;

    min = DV_first(dv_ptr1); min2 = DV_first(dv_ptr2);
    if (min == min2) {
        return BP_FALSE;
    }
    max = DV_last(dv_ptr1); max2 = DV_last(dv_ptr2);
    if (max == max2) {
        return BP_FALSE;
    }
    min = (min > min2 ? min : min2);
    max = (max < max2 ? max : max2);
    if (min == max) return BP_FALSE;
    if (min > max) return BP_TRUE;

    if (!IS_IT_DOMAIN(dv_ptr1) && !IS_IT_DOMAIN(dv_ptr2)) {
        bv_ptr1 = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr1);
        bv_ptr2 = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr2);
        if (BV_low_val(bv_ptr1) == BV_low_val(bv_ptr2) && BV_up_val(bv_ptr1) == BV_up_val(bv_ptr2)) {
            WORD_OFFSET(bv_ptr1, min, w1, w_ptr1, offset);
            w1 &= (MASK_FF << offset);
            WORD_OFFSET(bv_ptr2, min, w2, w_ptr2, offset);
            WORD_OFFSET(bv_ptr1, max, last_w1, last_w_ptr1, last_offset);
            while (w_ptr1 != last_w_ptr1) {
                if (w1 & w2) return BP_FALSE;  /* intersect */
                w_ptr1++; w1 = FOLLOW(w_ptr1);
                w_ptr2++; w2 = FOLLOW(w_ptr2);
            }
            w1 &= ((BPULONG)MASK_FF >> (NBITS_IN_LONG-last_offset-1));
            if (w1 & w2) return BP_FALSE;  /* intersect */
            return BP_TRUE;
        }
    }
    /* intervals, or bit vectors of different bases */
    for (; ; ) {
        if (dm_true(dv_ptr1, min) && dm_true(dv_ptr2, min)) return BP_FALSE;
        if (min >= max) break;
        min++;
        if (!IS_IT_DOMAIN(dv_ptr2)) min = domain_next_bv(dv_ptr2, min);
    }
    return BP_TRUE;
}

/* C-callable wrapper for b_VAR_NOTIN_D_cc/2. */
int c_var_notin_ints() {
    BPLONG X, D;

    X = ARG(1, 2);
    D = ARG(2, 2);
    return b_VAR_NOTIN_D_cc(X, D);
}

/*
  X notin List: excludes the elements in List from the domain of X,
  where List is a list of integers and integer intervals L..U.
*/
int b_VAR_NOTIN_D_cc(BPLONG X, BPLONG List)
{
    BPLONG elm, low, up, minX;
    BPLONG_PTR lst_ptr, interval_ptr, dv_ptr;

    DEREF_NONVAR(X);

    if (ISINT(X)) {
        return check_var_notin_d(INTVAL(X), List);
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    minX = DV_first(dv_ptr);
    DEREF_NONVAR(List);
    while (ISLIST(List)) {
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(List);
        elm = FOLLOW(lst_ptr); DEREF_NONVAR(elm);
        if (ISINT(elm)) {
            elm = INTVAL(elm);
            if (domain_set_false_aux(dv_ptr, elm) == 0) return 0;
        } else {
            interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
            low = FOLLOW(interval_ptr+1); DEREF_NONVAR(low);
            low = INTVAL(low);
            up = FOLLOW(interval_ptr+2); DEREF_NONVAR(up);
            up = INTVAL(up);
            if (domain_exclude_interval_aux(dv_ptr, low, up) == 0) return 0;
        }
        List = FOLLOW(lst_ptr+1); DEREF_NONVAR(List);
    }
    if (!ISNIL(List)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    return BP_TRUE;
}

/* Succeeds if the integer x is not in List (integers and intervals L..U). */
int check_var_notin_d(BPLONG x, BPLONG List)
{
    BPLONG elm, low, up;
    BPLONG_PTR lst_ptr, interval_ptr;

    DEREF_NONVAR(List);
    while (ISLIST(List)) {
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(List);
        elm = FOLLOW(lst_ptr); DEREF_NONVAR(elm);
        if (ISINT(elm)) {
            elm = INTVAL(elm);
            if (elm == x) return BP_FALSE;
        } else {
            interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
            low = FOLLOW(interval_ptr+1); DEREF_NONVAR(low);
            low = INTVAL(low);
            up = FOLLOW(interval_ptr+2); DEREF_NONVAR(up);
            up = INTVAL(up);
            if (x >= low && x <= up) return BP_FALSE;
        }
        List = FOLLOW(lst_ptr+1); DEREF_NONVAR(List);
    }
    if (!ISNIL(List)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    return BP_TRUE;
}

/*
  Disjunctive scheduling: no two tasks (S,D) may overlap.
  est -- earliest start time
  lst -- latest start time
  lct -- latest completion time (lst+duration)
*/
/* If task 0 is so tightly bounded that another task (S,D) can't fit inside its window
   (lst0-est0 < D0+D-1), then S can't be in lst0-D+1 .. est0+D0-1:
   wherever task 0 is placed, it occupies part of that range. */
#define TASKS_ARC_CONSISTENT(est0, lst0, duration0, start, duration) {  \
        if (lst0-est0 < duration0+duration-1) {                         \
            if (ISINT(start)) {                                         \
                start = INTVAL(start);                                  \
                if (start > lst0-duration && start < est0+duration0) return BP_FALSE; \
            } else {                                                    \
                dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);        \
                if (domain_exclude_interval_noint(dv_ptr, lst0-duration+1, est0+duration0-1) == BP_FALSE) return BP_FALSE; \
            }                                                           \
        }                                                               \
    }

/* Applies TASKS_ARC_CONSISTENT between the task (start,duration) and each task in tasks. */
int b_TASKS_EXCLUDE_NOGOOD_INTERVAL_ccc(BPLONG start, BPLONG duration, BPLONG tasks)
{
    BPLONG_PTR dv_ptr, ptr;
    BPLONG est0, lst0, task, duration0;

    DEREF_NONVAR(start);
    DEREF_NONVAR(duration); duration0 = INTVAL(duration);
    if (IS_SUSP_VAR(start)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);
        est0 = DV_first(dv_ptr);
        lst0 = DV_last(dv_ptr);
    } else {
        est0 = lst0 = INTVAL(start);
    }
    DEREF_NONVAR(tasks);
    while (ISLIST(tasks)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(tasks);
        task = FOLLOW(ptr); DEREF_NONVAR(task);
        tasks = FOLLOW(ptr+1); DEREF_NONVAR(tasks);

        ptr = (BPLONG_PTR)UNTAGGED_ADDR(task);
        start = FOLLOW(ptr+1); DEREF_NONVAR(start);
        duration = FOLLOW(ptr+2); DEREF_NONVAR(duration); duration = INTVAL(duration);
        TASKS_ARC_CONSISTENT(est0, lst0, duration0, start, duration);
    }
    return BP_TRUE;
}

/*
  $disjunctive_tasks(D0,...,Dn,S0,...,Sn)
  The tasks (S0,D0),...,(Sn,Dn) are mutually disjunctive.
  Triggered when the bound of S0 is updated.
*/
int b_DISJUNCTIVE_TASKS_AC(BPLONG n)
{
    BPLONG_PTR dv_ptr, starts_ptr;
    BPLONG start, duration, est0, lst0, duration0;

    n = INTVAL(n);
    starts_ptr = arreg+n;
    start = FOLLOW(starts_ptr);  /* S0 */
    duration0 = FOLLOW(starts_ptr+n);  /* D0 */
    starts_ptr--;

    //  write_term(start);write_term(duration0);printf("\n");
    DEREF_NONVAR(start);
    DEREF_NONVAR(duration0);
    duration0 = INTVAL(duration0);
    if (IS_SUSP_VAR(start)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);
        est0 = DV_first(dv_ptr);
        lst0 = DV_last(dv_ptr);
    } else {
        est0 = lst0 = INTVAL(start);
    }
    while (starts_ptr > arreg) {
        start = FOLLOW(starts_ptr); DEREF_NONVAR(start);
        duration = FOLLOW(starts_ptr+n); DEREF_NONVAR(duration); duration = INTVAL(duration);
        TASKS_ARC_CONSISTENT(est0, lst0, duration0, start, duration);
        starts_ptr--;
    }
    return BP_TRUE;
}

/* Edge finding with tasks T0, T1 and T:
   if min(est0,est1)+duration0+duration1+duration > max(lct0,lct1,lct) then T << T0, T << T1
   if min(est0,est1,est)+duration0+duration1+duration > max(lct0,lct1) then T0 << T, T1 << T
   (A << B: A ends before B starts.)
*/
#define EDGE_FINDER01(start, duration) {                                \
        BPLONG acc_est1, acc_lct1, acc_duration1, acc_est2, acc_lct2, acc_duration2; \
        acc_duration1 = duration0+duration1;                            \
        acc_duration2 = acc_duration1 + duration;                       \
        acc_est1 = (est0 < est1) ? est0 : est1;                         \
        acc_lct1 = (lct0 > lct1) ? lct0 : lct1;                         \
        if (ISINT(start)) {                                             \
            est = INTVAL(start);                                        \
            lct = est+duration;                                         \
            if (est < acc_est1) acc_est1 = est;                         \
            if (lct > acc_lct1) acc_lct1 = lct;                         \
            if (acc_lct1-acc_est1 < acc_duration2) {return BP_FALSE;}   \
        } else {                                                        \
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);            \
            est = DV_first(dv_ptr);                                     \
            lct = DV_last(dv_ptr)+duration;                             \
            acc_est2 = (est < acc_est1) ? est : acc_est1;               \
            acc_lct2 = (lct > acc_lct1) ? lct : acc_lct1;               \
            if (acc_est1+acc_duration2 > acc_lct2) {                    \
                BPLONG new_lst = acc_lct1-acc_duration1;                \
                if (new_lst < DV_last(dv_ptr)) {                        \
                    if (domain_region_noint(dv_ptr, est, new_lst) == BP_FALSE) return BP_FALSE; \
                }                                                       \
            } else if (acc_est2+acc_duration2 > acc_lct1) {             \
                BPLONG new_est = acc_est1 + acc_duration1;              \
                if (new_est > est) {                                    \
                    if (domain_region_noint(dv_ptr, new_est, DV_last(dv_ptr)) == BP_FALSE) return BP_FALSE; \
                }                                                       \
            }                                                           \
        }                                                               \
    }

/* Edge finding with a set of tasks Q (accumulated in acc_est, acc_lct, acc_duration) and T:
   if acc_est+acc_duration1 > acc_lct1 then T << Q
   if acc_est1+acc_duration1 > acc_lct then Q << T
   where the *1 values include T.
*/
#define EDGE_FINDER(acc_est, acc_lct, acc_duration, start, duration) {  \
        if (ISINT(start)) {                                             \
            est = INTVAL(start);                                        \
            lct = est+duration;                                         \
            if (est < acc_est) acc_est = est;                           \
            if (lct > acc_lct) acc_lct = lct;                           \
            acc_duration += duration;                                   \
            if (acc_lct-acc_est < acc_duration) {return BP_FALSE;}      \
        } else {                                                        \
            BPLONG acc_est1, acc_lct1, acc_duration1;                   \
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);            \
            est = DV_first(dv_ptr);                                     \
            lct = DV_last(dv_ptr)+duration;                             \
            acc_est1 = (est < acc_est) ? est : acc_est;                 \
            acc_lct1 = (lct > acc_lct) ? lct : acc_lct;                 \
            acc_duration1 = acc_duration+duration;                      \
            if (acc_est+acc_duration1 > acc_lct1) {                     \
                BPLONG new_lst = acc_lct-acc_duration1;                 \
                if (new_lst < DV_last(dv_ptr)) {                        \
                    if (domain_region_noint(dv_ptr, est, new_lst) == BP_FALSE) return BP_FALSE; \
                    acc_lct1 = acc_lct;                                 \
                }                                                       \
            } else if (acc_est1+acc_duration1 > acc_lct) {              \
                BPLONG new_est = acc_est + acc_duration;                \
                if (new_est > est) {                                    \
                    if (domain_region_noint(dv_ptr, new_est, DV_last(dv_ptr)) == BP_FALSE) return BP_FALSE; \
                    acc_est1 = acc_est;                                 \
                }                                                       \
            }                                                           \
            acc_est = acc_est1;                                         \
            acc_lct = acc_lct1;                                         \
            acc_duration = acc_duration1;                               \
            if (acc_lct-acc_est < acc_duration) {return BP_FALSE;}      \
        }                                                               \
    }

#define FORWARD_EDGE_FINDER {                                           \
        start = starts[0]; DEREF_NONVAR(start);                         \
        duration1 = durations[0];                                       \
        if (IS_SUSP_VAR(start)) {                                       \
            dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);            \
            est1 = DV_first(dv_ptr);                                    \
            lct1 = DV_last(dv_ptr)+duration1;                           \
        } else {                                                        \
            est1 = INTVAL(start);                                       \
            lct1 = est1+duration1;                                      \
        }                                                               \
        acc_est = (est1 < est0) ? est1 : est0;                          \
        acc_lct = (lct1 > lct0) ? lct1 : lct0;                          \
        acc_duration = duration0+duration1;                             \
        for (i = 1; i < m; i++) {                                       \
            start = starts[i]; DEREF_NONVAR(start);                     \
            duration = durations[i];                                    \
            EDGE_FINDER01(start, duration);                             \
            EDGE_FINDER(acc_est, acc_lct, acc_duration, start, duration); \
            est1 = est;                                                 \
            lct1 = lct;                                                 \
            duration1 = duration;                                       \
        }                                                               \
    }

/* Edge finding for $disjunctive_tasks (same frame layout as b_DISJUNCTIVE_TASKS_AC).
   Handles at most 20 other tasks. */
int b_DISJUNCTIVE_TASKS_EF(BPLONG n)
{
    BPLONG_PTR dv_ptr;
    BPLONG i, j, m, h, est, lct, start, duration, acc_est, acc_lct, acc_duration, est0, lst0, lct0, duration0, est1, lct1, duration1;
    BPLONG starts[20], durations[20], t_starts[20], t_durations[20];

    n = INTVAL(n); m = n-1;  /* m : number of tasks except for the seed task */
    if (m < 1 || m > 20) return BP_TRUE;  /* nothing to do, or too many tasks for the arrays below */
    start = FOLLOW(arreg+n); DEREF_NONVAR(start);  /* S0 */
    duration0 = FOLLOW(arreg+n+n);  /* D0 */
    DEREF_NONVAR(duration0); duration0 = INTVAL(duration0);

    if (IS_SUSP_VAR(start)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(start);
        est0 = DV_first(dv_ptr);
        lst0 = DV_last(dv_ptr);
    } else {
        est0 = lst0 = INTVAL(start);
    }
    lct0 = lst0+duration0;

    for (i = n-1; i >= 1; i--) {
        t_starts[n-i-1] = FOLLOW(arreg+i);
        duration = FOLLOW(arreg+n+i);  /* Di */
        DEREF_NONVAR(duration);
        t_durations[n-i-1] = INTVAL(duration);
    }

/* Edge finding: for each set Q={T1,T2,..T_{i-1}} where Ti=(Si,Di)
   if est(Q)+d(Q)+Di > lct(Q U {Ti}) then Ti << Q (i.e., Si =< lct(Q)-Di-d(Q))
   if est(Q U {Ti})+d(Q)+Di > lct(Q) then Q << Ti (i.e., Si >= est(Q)+d(Q))
   The sets are built incrementally, so several task orders are tried below.
*/
/* forward */
    for (i = 0; i < m; i++) {
        starts[i] = t_starts[i];
        durations[i] = t_durations[i];
    }
    FORWARD_EDGE_FINDER;

/* backward */
    for (i = m-1; i >= 0; i--) {
        starts[m-i-1] = t_starts[i];
        durations[m-i-1] = t_durations[i];
    }
    FORWARD_EDGE_FINDER;

/* in-out 1 */
    h = m/2;
    j = 0;
    for (i = h; i >= 0; i--) {
        starts[j] = t_starts[i];
        durations[j] = t_durations[i];
        j++;
    }
    for (i = h+1; i < m; i++) {
        starts[j] = t_starts[i];
        durations[j] = t_durations[i];
        j++;
    }
    FORWARD_EDGE_FINDER;

/* in-out 2 */
    j = 0;
    for (i = h+1; i < m; i++) {
        starts[j] = t_starts[i];
        durations[j] = t_durations[i];
        j++;
    }
    for (i = h; i >= 0; i--) {
        starts[j] = t_starts[i];
        durations[j] = t_durations[i];
        j++;
    }
    FORWARD_EDGE_FINDER;

/* merge 1 */
    i = 0; j = m-1; h = 0;
    while (i < j) {
        starts[h] = t_starts[i];
        durations[h] = t_durations[i];
        h++;
        starts[h] = t_starts[j];
        durations[h] = t_durations[j];
        h++; i++; j--;
    }
    if (i == j) {  /* odd m: the middle task */
        starts[h] = t_starts[i];
        durations[h] = t_durations[i];
    }
    FORWARD_EDGE_FINDER;

/* merge 2 */
    i = m/2; j = i+1; h = 0;
    while (i >= 0 && j < m) {
        starts[h] = t_starts[i];
        durations[h] = t_durations[i];
        h++;
        starts[h] = t_starts[j];
        durations[h] = t_durations[j];
        h++; i--; j++;
    }
    while (i >= 0) {
        starts[h] = t_starts[i];
        durations[h] = t_durations[i];
        h++; i--;
    }
    while (j < m) {
        starts[h] = t_starts[j];
        durations[h] = t_durations[j];
        h++; j++;
    }
    FORWARD_EDGE_FINDER;

    return BP_TRUE;
}

/* Excludes Low..Up from Var (Var may be an integer, in which case it is just checked). */
int b_EXCLUDE_NOGOOD_INTERVAL_ccc(BPLONG Var, BPLONG Low, BPLONG Up)
{
    BPLONG_PTR dv_ptr;
    BPLONG val;

    DEREF_NONVAR(Low); Low = INTVAL(Low);
    DEREF_NONVAR(Up); Up = INTVAL(Up);
    DEREF_NONVAR(Var);
    if (ISINT(Var)) {
        val = INTVAL(Var);
        if (val >= Low && val <= Up) return BP_FALSE; else return BP_TRUE;
    } else {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Var);
        return domain_exclude_interval_noint(dv_ptr, Low, Up);
    }
}

/*
  For X :: List, where List is a list of integers and integer intervals L..U:
  computes Min and Max of the elements, and binds Unsorted to 1 if the list is not
  sorted in ascending order. Fails if List is not such a list.
*/
int c_integers_intervals_list() {
    BPLONG List, Min, Max, Unsorted;
    BPLONG elm, low, up, min0, max0;
    BPLONG_PTR top, ptr, interval_ptr;

    List = ARG(1, 4); DEREF(List);
    Min = ARG(2, 4);
    Max = ARG(3, 4);
    Unsorted = ARG(4, 4);

    if (!ISLIST(List)) return BP_FALSE;  /* an empty domain has no min/max */
    {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(List);
        elm = FOLLOW(ptr); DEREF(elm);
        List = FOLLOW(ptr+1); DEREF(List);
        if (ISINT(elm)) {
            min0 = max0 = INTVAL(elm);
        } else if ISSTRUCT(elm) {
                interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
                if (FOLLOW(interval_ptr) != (BPLONG)interval_psc) {
                    return BP_FALSE;
                }
                min0 = FOLLOW(interval_ptr+1); DEREF(min0);
                max0 = FOLLOW(interval_ptr+2); DEREF(max0);
                if (!ISINT(min0) || !ISINT(max0)) {
                    //  bp_exception = illegal_arguments;
                    //  return BP_ERROR;
                    return BP_FALSE;
                }
                min0 = INTVAL(min0);
                max0 = INTVAL(max0);
                if (min0 > max0) {
                    // bp_exception = illegal_arguments;
                    // return BP_ERROR;
                    return BP_FALSE;
                }
            } else return BP_FALSE;
    }
    while (ISLIST(List)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(List);
        elm = FOLLOW(ptr); DEREF(elm);
        List = FOLLOW(ptr+1); DEREF(List);
        if (ISINT(elm)) {
            elm = INTVAL(elm);
            if (elm <= max0) {
                unify(Unsorted, MAKEINT(1));
                if (elm < min0) min0 = elm;
            } else {
                max0 = elm;
            }
        } else if ISSTRUCT(elm) {
                interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
                if (FOLLOW(interval_ptr) != (BPLONG)interval_psc) {
                    return BP_FALSE;
                }
                low = FOLLOW(interval_ptr+1); DEREF(low);
                up = FOLLOW(interval_ptr+2); DEREF(up);
                if (!ISINT(low) || !ISINT(up)) {
                    return BP_FALSE;
                    // bp_exception = illegal_arguments;
                    // return BP_ERROR;
                }
                low = INTVAL(low);
                up = INTVAL(up);
                if (low > up) {
                    return BP_FALSE;
                    // bp_exception = illegal_arguments;
                    // return BP_ERROR;
                }
                if (low <= max0) {
                    unify(Unsorted, MAKEINT(1));
                    if (low < min0) min0 = low;
                    if (up > max0) max0 = up;
                } else {
                    if (up > max0) max0 = up;
                }
            } else return BP_FALSE;
    }
    if (!ISNIL(List)) {
        return BP_FALSE;
        // bp_exception = illegal_arguments;
        // return BP_ERROR;
    }

    unify(Min, MAKEINT(min0));
    unify(Max, MAKEINT(max0));
    return BP_TRUE;
}

/* X :: List, where List is a sorted list of integers and intervals L..U: removes the gaps
   between the elements of List from X's domain. X's bounds are restricted by the caller. */
int b_VAR_IN_D_cc(BPLONG X, BPLONG List)
{
    BPLONG elm, low, up, Min;
    BPLONG_PTR lst_ptr, interval_ptr, dv_ptr;

    /*  printf("var_in_d "); write_term(X); write_term(List); printf("\n"); */

    DEREF_NONVAR(X);
    if (ISINT(X)) {
        return check_var_in_d(INTVAL(X), List);
    }
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    Min = DV_first(dv_ptr);
    DEREF_NONVAR(List);
    while (ISLIST(List)) {
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(List);
        elm = FOLLOW(lst_ptr); DEREF_NONVAR(elm);
        if (ISINT(elm)) {
            elm = INTVAL(elm);
            if (Min < elm) {
                if (domain_exclude_interval_aux(dv_ptr, Min, elm-1) == 0) return 0;
                Min = elm+1;
            } else if (Min == elm) {
                Min = elm+1;
            }
        } else {
            interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
            low = FOLLOW(interval_ptr+1); DEREF_NONVAR(low); low = INTVAL(low);
            up = FOLLOW(interval_ptr+2); DEREF_NONVAR(up); up = INTVAL(up);
            if (Min < low) {
                if (domain_exclude_interval_aux(dv_ptr, Min, low-1) == 0) return 0;
            }
            if (Min <= up) Min = up+1;
        }
        List = FOLLOW(lst_ptr+1); DEREF_NONVAR(List);
    }
    return BP_TRUE;
}

/* Succeeds if the integer x is in List (integers and intervals L..U). */
int check_var_in_d(BPLONG x, BPLONG List)
{
    BPLONG elm, low, up;
    BPLONG_PTR lst_ptr, interval_ptr;

    DEREF_NONVAR(List);
    while (ISLIST(List)) {
        lst_ptr = (BPLONG_PTR)UNTAGGED_ADDR(List);
        elm = FOLLOW(lst_ptr); DEREF_NONVAR(elm);
        if (ISINT(elm)) {
            elm = INTVAL(elm);
            if (x == elm) return BP_TRUE;
        } else {
            interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
            low = FOLLOW(interval_ptr+1); DEREF_NONVAR(low); low = INTVAL(low);
            up = FOLLOW(interval_ptr+2); DEREF_NONVAR(up); up = INTVAL(up);
            if (x >= low && x <= up) return BP_TRUE;
        }
        List = FOLLOW(lst_ptr+1); DEREF_NONVAR(List);
    }
    return BP_FALSE;
}

/* Clears the bits of from..to in a bit-vector domain, posting a dom event for each
   element removed. Precondition: DV_first(dv_ptr) < from <= to < DV_last(dv_ptr). */
void exclude_inner_interval_bv(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{
    BPULONG w, mask, offset;
    BPLONG_PTR w_ptr, bv_ptr, last_trailed_addr;
    BPLONG count = DV_size(dv_ptr);

    last_trailed_addr = NULL;
    bv_ptr = (BPLONG_PTR)DV_bit_vector_ptr(dv_ptr);
    WORD_OFFSET(bv_ptr, from, w, w_ptr, offset);
    while (from <= to) {
        if (offset == 0 && w == 0) {  /* skip a whole empty word */
            from += NBITS_IN_LONG;
            w_ptr++; w = FOLLOW(w_ptr);
            continue;
        }
        mask = ((BPULONG)0x1 << offset);
        if (w & mask) {
            if (last_trailed_addr != w_ptr) {
                PUSHTRAIL_H_ATOMIC(w_ptr, w);
                last_trailed_addr = w_ptr;
            }
            w = FOLLOW(w_ptr) = (w & ~mask);
            INSERT_TRIGGER_dom(dv_ptr, MAKEINT(from));
            count--;
        }
        from++;
        offset++;
        if (offset == NBITS_IN_LONG) {offset = 0; w_ptr++; w = FOLLOW(w_ptr);}
    }
    if (last_trailed_addr != NULL) {
        UPDATE_SIZE(dv_ptr, DV_size(dv_ptr), count);
    }
}

/* Excludes from..to from the domain at dv_ptr, whose variable may be bound. */
int domain_exclude_interval_aux(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{
    BPLONG val;

    // printf("exclude_interval  %x %d %d\n",dv_ptr,from,to);

    if (ISINT(FOLLOW(dv_ptr))) {
        val = INTVAL(FOLLOW(dv_ptr));
        return (val >= from && val <= to) ? BP_FALSE : BP_TRUE;
    }
    if (from <= DV_first(dv_ptr)) {
        return domain_region_noint(dv_ptr, to+1, DV_last(dv_ptr));
    } else if (to >= DV_last(dv_ptr)) {
        return domain_region_noint(dv_ptr, DV_first(dv_ptr), from-1);
    } else {
        if (IS_IT_DOMAIN(dv_ptr)) {
            it_to_bv_with_interval_holes(dv_ptr, from, to);
        } else {
            exclude_inner_interval_bv(dv_ptr, from, to);
        }
    }
    return BP_TRUE;
}

/* Excludes from..to from the domain at dv_ptr.
   Preconditions: from <= to; dv_ptr's variable is not bound. */
int domain_exclude_interval_noint(BPLONG_PTR dv_ptr, BPLONG from, BPLONG to)
{

    // printf("exclude_interval  %x %d %d\n",dv_ptr,from,to);
    if (from <= DV_first(dv_ptr)) {
        return domain_region_noint(dv_ptr, to+1, DV_last(dv_ptr));
    } else if (to >= DV_last(dv_ptr)) {
        return domain_region_noint(dv_ptr, DV_first(dv_ptr), from-1);
    } else {
        if (IS_IT_DOMAIN(dv_ptr)) {
            it_to_bv_with_interval_holes(dv_ptr, from, to);
        } else {
            exclude_inner_interval_bv(dv_ptr, from, to);
        }
    }
    return BP_TRUE;
}

/* returns floor(a/b) (b != 0) */
BPLONG sound_low_div(BPLONG a, BPLONG b) {
    if (b < 0) {
        b = -b;
        a = -a;
    }
    if (a >= 0) {
        return a/b;
    } else {
        BPLONG val = a/b;
        if (val*b != a) val--;
        return val;
    }
}

/* returns ceiling(a/b) (b != 0) */
BPLONG sound_up_div(BPLONG a, BPLONG b) {
    if (b < 0) {
        b = -b;
        a = -a;
    }
    if (a <= 0) {
        return a/b;
    } else {
        BPLONG val = a/b;
        if (val*b != a) val++;
        return val;
    }
}

/* *p_low and *p_up are the smallest and the largest of the four values. */
void min_max4(BPLONG val1, BPLONG val2, BPLONG val3, BPLONG val4, BPLONG_PTR p_low, BPLONG_PTR p_up) {
    BPLONG low, up;

    low = up = val1;
    if (val2 < low) low = val2;
    if (val2 > up) up = val2;
    if (val3 < low) low = val3;
    if (val3 > up) up = val3;
    if (val4 < low) low = val4;
    if (val4 > up) up = val4;
    *p_low = low;
    *p_up = up;
}

/* Returns the smallest of the four values. */
BPLONG min4(BPLONG val1, BPLONG val2, BPLONG val3, BPLONG val4) {
    BPLONG low;

    low = val1;
    if (val2 < low) low = val2;
    if (val3 < low) low = val3;
    if (val4 < low) low = val4;
    return low;
}

/* Returns the largest of the four values. */
BPLONG max4(BPLONG val1, BPLONG val2, BPLONG val3, BPLONG val4) {
    BPLONG up;

    up = val1;
    if (val2 > up) up = val2;
    if (val3 > up) up = val3;
    if (val4 > up) up = val4;
    return up;
}

/* X*Y = Z:
   For each element eY in Y's domain, if there are no eX in X's domain and eZ in Z's domain
   such that eX*eY = eZ, then exclude eY from Y's domain. Special attention needed when X=Y!
*/
void exclude_unsupported_y_constr_xy_eq_z(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y, BPLONG_PTR dv_ptr_z) {
    BPLONG minX, maxX, eY, maxY, eX, eZ;
    int supported;

    minX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    eY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);

    for (; ; ) {  /* loop_Y: enumerate Y's domain */
        supported = 0;
        if (eY == 0) {  /* 0 is supported if 0 is in Z's domain */
            if (dm_true(dv_ptr_z, 0)) supported = 1;
        } else if (dv_ptr_x == dv_ptr_y) {  /* X and Y are identical */
            if (dm_true(dv_ptr_z, eY*eY)) supported = 1;
        } else {
            eX = minX;
            for (; ; ) {  /* loop_X: enumerate X's domain */
                eZ = eX*eY;
                if (dm_true(dv_ptr_z, eZ)) {
                    supported = 1;
                    break;  /* exit loop_X */
                }
                if (eX == maxX) break;  /* exit loop_X */
                eX++;
                if (!IS_IT_DOMAIN(dv_ptr_x)) eX = domain_next_bv(dv_ptr_x, eX);
            }  /* end of loop_X */
        }
        if (supported == 0) {
            domain_set_false_noint(dv_ptr_y, eY);
            if (ISINT(FOLLOW(dv_ptr_y))) return;  /* Y has been instantiated */
        }
        if (eY == maxY) return;
        eY++;
        if (!IS_IT_DOMAIN(dv_ptr_y)) eY = domain_next_bv(dv_ptr_y, eY);
    }  /* end of loop_Y */
}

/* X*Y = cz: Special attention needed when X=Y! */
void exclude_unsupported_y_constr_xy_eq_c(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y, BPLONG cz) {
    BPLONG minX, maxX, eY, maxY, eX;
    int supported;

    minX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    eY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);

    for (; ; ) {  /* loop_Y: enumerate Y's domain */
        supported = 0;
        if (eY == 0) {  /* 0 is supported if cz=0 */
            if (cz == 0) supported = 1;
        } else if (dv_ptr_x == dv_ptr_y) {  /* X and Y are identical */
            if (eY*eY == cz) supported = 1;
        } else {
            eX = cz/eY;
            if (eX*eY == cz && dm_true(dv_ptr_x, eX)) supported = 1;
        }
        if (supported == 0) {
            domain_set_false_noint(dv_ptr_y, eY);
            if (ISINT(FOLLOW(dv_ptr_y))) return;  /* Y has been instantiated */
        }
        if (eY == maxY) return;
        eY++;
        if (!IS_IT_DOMAIN(dv_ptr_y)) eY = domain_next_bv(dv_ptr_y, eY);
    }  /* end of loop_Y */
}

/* X*Y = Z:
   For each element eZ in Z's domain, if there are no eX in X's domain and eY in Y's domain
   such that eX*eY = eZ, then exclude eZ from Z's domain. Special attention needed when X=Y!
*/
void exclude_unsupported_z_constr_xy_eq_z(BPLONG_PTR dv_ptr_x, BPLONG_PTR dv_ptr_y, BPLONG_PTR dv_ptr_z) {
    BPLONG eX, minX, maxX, eY, eZ, maxZ;
    int supported;

    if (DV_size(dv_ptr_x) > DV_size(dv_ptr_y)) {
        BPLONG_PTR tmp;
        tmp = dv_ptr_x;
        dv_ptr_x = dv_ptr_y;
        dv_ptr_y = tmp;
    }
    minX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    eZ = DV_first(dv_ptr_z); maxZ = DV_last(dv_ptr_z);

    for (; ; ) {  /* loop_Z: enumerate Z's domain */
        supported = 0;
        if (eZ == 0) {
            if (dm_true(dv_ptr_x, 0) || dm_true(dv_ptr_y, 0)) supported = 1;
        } else if (dv_ptr_x == dv_ptr_y) {
            if (eZ > 0) {
                eX = (BPLONG)sqrt(eZ);
                if (eX*eX == eZ) supported = 1;
            }
        } else {
            eX = minX;
            for (; ; ) {  /* loop_X: enumerate X's domain */
                if (eX != 0) {
                    eY = eZ/eX;
                    if (eX*eY == eZ && dm_true(dv_ptr_y, eY)) {
                        supported = 1;
                        break;  /* exit loop_X */
                    }
                }
                if (eX == maxX) break;  /* exit loop_X */
                eX++;
                if (!IS_IT_DOMAIN(dv_ptr_x)) eX = domain_next_bv(dv_ptr_x, eX);
            }  /* end of loop_X */
        }
        if (supported == 0) {
            domain_set_false_noint(dv_ptr_z, eZ);
            if (ISINT(FOLLOW(dv_ptr_z))) return;  /* Z has been instantiated */
        }
        if (eZ == maxZ) return;
        eZ++;
        if (!IS_IT_DOMAIN(dv_ptr_z)) eZ = domain_next_bv(dv_ptr_z, eZ);
    }  /* end of loop_Z */
}

/* X+Y = Z, X is a bit-vector domain. The constraint is already interval consistent.
   Removes from Z every inner value that is not the sum of a value of X and a value of Y.
   Only done when Z's domain is small; minZ and maxZ are supported by interval consistency.
*/
int c_CLPFD_ADD_AC_ccc() {
    BPLONG X, Y, Z, i, size;
    BPLONG_PTR dv_ptr_x, dv_ptr_y, dv_ptr_z, ptr;
    BPLONG elmX, maxX, elmY, minY, maxY, elmZ, minZ, maxZ;

    X = ARG(1, 3); DEREF_NONVAR(X);
    Y = ARG(2, 3); DEREF_NONVAR(Y);
    Z = ARG(3, 3); DEREF_NONVAR(Z);

//    printf("=> ADD_AC "); write_term(X); printf(" + "); write_term(Y); printf(" = "); write_term(Z); printf("\n");

    if (!IS_SUSP_VAR(X)) return BP_TRUE;
    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    if (DV_size(dv_ptr_x) == (DV_last(dv_ptr_x) - DV_first(dv_ptr_x) + 1)) return BP_TRUE;
    if (!IS_SUSP_VAR(Z)) return BP_TRUE;
    dv_ptr_z = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Z);
    if (!IS_SMALL_DOMAIN(dv_ptr_z)) return BP_TRUE;

    /* enforce AC on Z */
    minZ = DV_first(dv_ptr_z);
    maxZ = DV_last(dv_ptr_z);
    size = maxZ-minZ+1;
    if (local_top - heap_top <= size) {
        return BP_TRUE;  /* not enough free space: do not enforce AC on Z */
    }
    ptr = local_top-size;
    for (i = 1; i < size-1; i++) {
        *(ptr+i) = 0;  /* 0 means unsupported, minZ and maxZ are supported */
    }
    elmX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    if (IS_SUSP_VAR(Y)) {
        dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
        minY = DV_first(dv_ptr_y);
        maxY = DV_last(dv_ptr_y);
        for (; ; ) {  /* iterate over X */
            /* only sums strictly between minZ and maxZ need marking */
            BPLONG lowY = minZ+1-elmX, upY = maxZ-1-elmX;
            if (lowY < minY) lowY = minY;
            if (upY > maxY) upY = maxY;
            if (IS_IT_DOMAIN(dv_ptr_y)) {
                for (elmY = lowY; elmY <= upY; elmY++) {
                    *(ptr+elmX+elmY-minZ) = 1;
                }
            } else if (lowY <= upY) {
                elmY = (lowY == minY) ? minY : domain_next_bv(dv_ptr_y, lowY);
                while (elmY <= upY) {  /* iterate over Y */
                    *(ptr+elmX+elmY-minZ) = 1;
                    if (elmY == maxY) break;  /* exit loop of Y */
                    elmY = domain_next_bv(dv_ptr_y, elmY+1);
                }
            }
            if (elmX == maxX) break;  /* exit loop of X */
            elmX++;
            elmX = domain_next_bv(dv_ptr_x, elmX);
        }
    } else {  /* Y is an int */
        elmY = INTVAL(Y);
        for (; ; ) {  /* iterate over X */
            elmZ = elmX+elmY;
            if (elmZ > minZ && elmZ < maxZ) {
                *(ptr+elmZ-minZ) = 1;
            }
            if (elmX == maxX) break;  /* exit loop of X */
            elmX++;
            elmX = domain_next_bv(dv_ptr_x, elmX);
        }
    }

    elmZ = minZ+1;
    for (i = 1; i < size-1; i++) {
        if (*(ptr+i) == 0) {  /* elmZ is unsupported */
            domain_set_false_noint(dv_ptr_z, elmZ);
        }
        elmZ++;
    }

//    printf("<= ADD_AC "); write_term(X); printf(" + "); write_term(Y); printf(" = "); write_term(Z); printf("\n"); 

    return BP_TRUE;
}

/* Const-X = Y, X is a bit-vector domain. The constraint is already interval consistent.
   Removes from Y every inner value v for which Const-v is not in X. */
int c_CLPFD_SUB_AC_ccc() {
    BPLONG Const, X, Y, i, sizeY;
    BPLONG_PTR dv_ptr_x, dv_ptr_y, ptr;
    BPLONG elmX, maxX, elmY, minY, maxY;

    Const = ARG(1, 3); DEREF_NONVAR(Const); Const = INTVAL(Const);
    X = ARG(2, 3); DEREF_NONVAR(X);
    if (!IS_SUSP_VAR(X)) return BP_TRUE;
    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    if (DV_size(dv_ptr_x) == (DV_last(dv_ptr_x) - DV_first(dv_ptr_x) + 1)) return BP_TRUE;
    Y = ARG(3, 3); DEREF_NONVAR(Y);

    // printf("=> SUB_AC "); write_term(MAKEINT(Const)); printf(" - "); write_term(X); printf(" = "); write_term(Y); printf("\n");
    if (!IS_SUSP_VAR(Y)) return BP_TRUE;
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
    minY = DV_first(dv_ptr_y);
    maxY = DV_last(dv_ptr_y);
    sizeY = maxY-minY+1;
    if (local_top - heap_top <= sizeY || DV_size(dv_ptr_y) > 512) {
        return BP_TRUE;  /* do not enforce AC on Y */
    }
    ptr = local_top-sizeY;
    for (i = 1; i < sizeY-1; i++) {
        *(ptr+i) = 0;  /* 0 means unsupported, minY and maxY are supported */
    }
    elmX = DV_first(dv_ptr_x);
    maxX = DV_last(dv_ptr_x);
    for (; ; ) {  /* iterate over X */
        elmY = Const-elmX;
        if (elmY > minY && elmY < maxY) {
            *(ptr+(elmY-minY)) = 1;  /* elmY is supported in Y */
        }
        if (elmX == maxX) break;  /* exit loop of X */
        elmX++;
        elmX = domain_next_bv(dv_ptr_x, elmX);
    }

    elmY = minY+1;
    for (i = 1; i < sizeY-1; i++) {
        if (*(ptr+i) == 0) {  /* elmY is unsupported */
            domain_set_false_noint(dv_ptr_y, elmY);
        }
        elmY++;
    }
    //    printf("<= SUB_AC "); write_term(MAKEINT(Const)); printf(" - "); write_term(X); printf(" = "); write_term(Y); printf("\n");

    return BP_TRUE;
}

/* X*Y = Z: bounds reasoning on the three variables, plus value-level pruning when the
   domains are small and one of them has holes. When X or Y is a constant a (other than
   0, 1 and -1), only Z is pruned: every z needs z/a in Y. */
int b_CLPFD_MULTIPLY_INT_ccc(BPLONG X, BPLONG Y, BPLONG Z)
{
    BPLONG_PTR dv_ptr_x, dv_ptr_y, dv_ptr_z;
    BPLONG minX, maxX, minY, maxY, minZ, maxZ, lowX, upX, lowY, upY, lowZ, upZ;
    int z_includes_0 = 1;

    DEREF_NONVAR(X); DEREF_NONVAR(Y); DEREF_NONVAR(Z);
x_is_int:
    if (!IS_SUSP_VAR(X)) {
        BPLONG a = INTVAL(X);
        if (a != 0 && a != 1 && a != -1 && IS_SUSP_VAR(Y) && IS_SUSP_VAR(Z)) {
            dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
            dv_ptr_z = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Z);
            minZ = DV_first(dv_ptr_z); maxZ = DV_last(dv_ptr_z);
            if (DV_size(dv_ptr_z) <= 100) {
                for (; ; ) {
                    BPLONG elm = minZ/a;
                    if (a*elm != minZ || !dm_true(dv_ptr_y, elm)) {  /* minZ has no support in Y */
                        if (domain_set_false_aux(dv_ptr_z, minZ) == BP_FALSE) return BP_FALSE;
                        if (ISINT(FOLLOW(dv_ptr_z))) {  /* Z got instantiated: check its value */
                            BPLONG v = INTVAL(FOLLOW(dv_ptr_z));
                            return (v % a == 0 && dm_true(dv_ptr_y, v/a)) ? BP_TRUE : BP_FALSE;
                        }
                    }
                    if (minZ == maxZ) return BP_TRUE;
                    minZ++;
                    if (!IS_IT_DOMAIN(dv_ptr_z)) minZ = domain_next_bv(dv_ptr_z, minZ);
                }
            }
        }
        return BP_TRUE;
    }
    if (!IS_SUSP_VAR(Y)) {
        BPLONG tmp;
        tmp = X; X = Y; Y = tmp;
        goto x_is_int;
    }
    /* both X and Y are domain variables */

    //  printf("=> multi "); write_term(X); printf(" "); write_term(Y); printf(" "); write_term(Z); printf("\n");

    dv_ptr_x = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(X);
    dv_ptr_y = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Y);
    if (IS_SUSP_VAR(Z)) {
        dv_ptr_z = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(Z);
        minZ = DV_first(dv_ptr_z); maxZ = DV_last(dv_ptr_z);

        if (!dm_true(dv_ptr_z, 0)) {
            z_includes_0 = 0;
        }
    } else if (ISINT(Z)) {
        minZ = maxZ = INTVAL(Z);
        if (minZ > 0 || maxZ < 0) {
            z_includes_0 = 0;
        }
    } else {
        bp_exception = c_type_error(et_INTEGER, Z);
        return BP_ERROR;
    }
    if (minZ > 0) {
        if (DV_first(dv_ptr_x) >= 0) {
            if (domain_region_noint(dv_ptr_y, 1, DV_last(dv_ptr_y)) == BP_FALSE) return BP_FALSE;
            if (ISINT(FOLLOW(dv_ptr_y))) return BP_TRUE;
        }
        if (DV_first(dv_ptr_y) >= 0) {
            if (domain_region_noint(dv_ptr_x, 1, DV_last(dv_ptr_x)) == BP_FALSE) return BP_FALSE;
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
    } else if (z_includes_0 == 0) {
        if (IS_SMALL_DOMAIN(dv_ptr_x)) {
            domain_set_false_aux(dv_ptr_x, 0);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
        if (IS_SMALL_DOMAIN(dv_ptr_y)) {
            domain_set_false_aux(dv_ptr_y, 0);
            if (ISINT(FOLLOW(dv_ptr_y))) return BP_TRUE;
        }
    }

    minX = DV_first(dv_ptr_x); maxX = DV_last(dv_ptr_x);
    minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);
    if (minX >= 0 && minY >= 0 && IS_SUSP_VAR(Z)){
        if (domain_region_noint(dv_ptr_z, 0, DV_last(dv_ptr_z)) == BP_FALSE) return BP_FALSE;
    }
    if (minX >= -BP_MAXINT_1W_SQRT && maxX <= BP_MAXINT_1W_SQRT &&
        minY >= -BP_MAXINT_1W_SQRT && maxY <= BP_MAXINT_1W_SQRT) {  /* products of the bounds can't overflow */
        if (minX >= 0 && minY >= 0) {
            lowZ = minX*minY;
            upZ = maxX*maxY;
        } else {
            min_max4(minX*minY, minX*maxY, maxX*minY, maxX*maxY, &lowZ, &upZ);
        }
        if (IS_SUSP_VAR(Z)) {
            if (domain_region_noint(dv_ptr_z, lowZ, upZ) == BP_FALSE) return BP_FALSE;
            DEREF_NONVAR(Z);
            if (IS_SUSP_VAR(Z)) {
                minZ = DV_first(dv_ptr_z); maxZ = DV_last(dv_ptr_z);
            } else {
                minZ = maxZ = INTVAL(Z);
            }
        } else {
            if (maxZ < lowZ || minZ > upZ) return BP_FALSE;
        }
    }

    if (minZ == BP_MININT_1W || maxZ == BP_MAXINT_1W) return BP_TRUE;
    if (minX >= 0 && minY >= 0 && minZ > 0) {
        if (domain_region_noint(dv_ptr_x, minX, maxZ) == BP_FALSE) return BP_FALSE;
        if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        maxX = DV_last(dv_ptr_x);
        if (domain_region_noint(dv_ptr_y, minY, maxZ) == BP_FALSE) return BP_FALSE;
        if (ISINT(FOLLOW(dv_ptr_y))) return BP_TRUE;
        maxY = DV_last(dv_ptr_y);
    }
    if ((minX > 0 && maxX < BP_MAXINT_1W) || (maxX < 0 && minX > BP_MININT_1W)) {
        lowY = min4(sound_low_div(minZ, minX), sound_low_div(minZ, maxX), sound_low_div(maxZ, minX), sound_low_div(maxZ, maxX));
        upY = max4(sound_up_div(minZ, minX), sound_up_div(minZ, maxX), sound_up_div(maxZ, minX), sound_up_div(maxZ, maxX));
        if (domain_region_noint(dv_ptr_y, lowY, upY) == BP_FALSE) return BP_FALSE;
        if (ISINT(FOLLOW(dv_ptr_y))) return BP_TRUE;
        minY = DV_first(dv_ptr_y); maxY = DV_last(dv_ptr_y);
    }
    if ((minY > 0 && maxY < BP_MAXINT_1W) || (maxY < 0 && minY > BP_MININT_1W)) {
        lowX = min4(sound_low_div(minZ, minY), sound_low_div(minZ, maxY), sound_low_div(maxZ, minY), sound_low_div(maxZ, maxY));
        upX = max4(sound_up_div(minZ, minY), sound_up_div(minZ, maxY), sound_up_div(maxZ, minY), sound_up_div(maxZ, maxY));
        if (domain_region_noint(dv_ptr_x, lowX, upX) == BP_FALSE) return BP_FALSE;
        if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
    }

    //  printf("=> multi-3 "); write_term(X); printf(" "); write_term(Y); printf(" "); write_term(Z); printf("\n");
    if (IS_SUSP_VAR(Z)) {
        if (IS_SMALL_DOMAIN(dv_ptr_y) && maxX-minX <= 65536 && (IS_BV_DOMAIN(dv_ptr_x) || IS_BV_DOMAIN(dv_ptr_z))) {
            exclude_unsupported_y_constr_xy_eq_z(dv_ptr_x, dv_ptr_y, dv_ptr_z);
            if (ISINT(FOLLOW(dv_ptr_y))) return BP_TRUE;
        }
        if (IS_SMALL_DOMAIN(dv_ptr_x) && maxY-minY <= 65536 && (IS_BV_DOMAIN(dv_ptr_y) || IS_BV_DOMAIN(dv_ptr_z))) {
            exclude_unsupported_y_constr_xy_eq_z(dv_ptr_y, dv_ptr_x, dv_ptr_z);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
        if (IS_SMALL_DOMAIN2(minZ, maxZ) && (IS_BV_DOMAIN(dv_ptr_x) || IS_BV_DOMAIN(dv_ptr_y))) {
            exclude_unsupported_z_constr_xy_eq_z(dv_ptr_x, dv_ptr_y, dv_ptr_z);
        }
        //        printf("<= multi "); write_term(X); printf(" "); write_term(Y); printf(" "); write_term(Z); printf("\n");
    } else {
        Z = INTVAL(Z);
        if (IS_SMALL_DOMAIN(dv_ptr_y) && maxX-minX <= 65536) {
            exclude_unsupported_y_constr_xy_eq_c(dv_ptr_x, dv_ptr_y, Z);
            if (ISINT(FOLLOW(dv_ptr_y))) return BP_TRUE;
        }
        if (IS_SMALL_DOMAIN(dv_ptr_x) && maxY-minY <= 65536) {
            exclude_unsupported_y_constr_xy_eq_c(dv_ptr_y, dv_ptr_x, Z);
            if (ISINT(FOLLOW(dv_ptr_x))) return BP_TRUE;
        }
    }

    return BP_TRUE;
}

/******************************************************************/
/* For the CP solver competition                                  */
/* Looks up key in a $hshtb(Count,Buckets) hash table. */
BPLONG con05_hashtable_get(BPLONG table, BPLONG key)
{
    SYM_REC_PTR sym_ptr;
    BPLONG buckets;
    BPLONG_PTR ptr, top;
    BPLONG index, size;

    /*  write_term(key);printf("   "); write_term(table);printf("\n"); */
    DEREF_NONVAR(table);
    ptr = (BPLONG_PTR)UNTAGGED_ADDR(table);
    buckets = FOLLOW(ptr+2);  /* $hshtb(Count,Buckets) */
    DEREF(buckets);
    ptr = (BPLONG_PTR)UNTAGGED_ADDR(buckets);
    sym_ptr = (SYM_REC_PTR)FOLLOW(ptr);
    size = GET_ARITY(sym_ptr);
    index = bp_hashval(key) % size + 1;
    return hashtable_lookup_chain(FOLLOW(ptr+index), key);
}

/* C implementation of the following Prolog code. Es lists values (and intervals) that
   lost a support; each decrement_counter reduces the support count of (J,E) and removes
   E from Xj when no support is left.

  decrement_counters(Counters,J,_,Xj):-nonvar(Xj) : true.
  decrement_counters(Counters,J,[],Xj):-true : true.
  decrement_counters(Counters,J,[L..U|Es],Xj):-true :
  decrement_counters_interval(Counters,J,L,U,Xj),
  decrement_counters(Counters,J,Es,Xj).
  decrement_counters(Counters,J,[E|Es],Xj):-true :
  decrement_counter(Counters,J,E,Xj),
  decrement_counters(Counters,J,Es,Xj).
*/
int c_cpcon_decrement_counters() {
    BPLONG Counters, J, Es, var, elm, low, up;
    BPLONG_PTR dv_ptr, ptr, interval_ptr;

    Counters = ARG(1, 4);
    J = ARG(2, 4); DEREF_NONVAR(J);
    Es = ARG(3, 4); DEREF_NONVAR(Es);
    var = ARG(4, 4); DEREF_NONVAR(var);
    /*
      printf("=>decrement ");
      write_term(J);printf(" ");
      write_term(Es);printf(" ");
      write_term(var);printf("\n ");
    */
    if (!IS_SUSP_VAR(var)) return BP_TRUE;
    dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(var);
    while (ISLIST(Es)) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(Es);
        elm = FOLLOW(ptr); DEREF_NONVAR(elm);
        if (ISINT(elm)) {
            if (cpcon_decrement_counter(Counters, J, INTVAL(elm), dv_ptr) == BP_FALSE) return BP_FALSE;
            if (ISINT(FOLLOW(dv_ptr))) return BP_TRUE;
        } else {  /* an interval */
            interval_ptr = (BPLONG_PTR)UNTAGGED_ADDR(elm);
            low = FOLLOW(interval_ptr+1); DEREF_NONVAR(low); low = INTVAL(low);
            up = FOLLOW(interval_ptr+2); DEREF_NONVAR(up); up = INTVAL(up);
            for (elm = low; elm <= up; elm++) {
                if (cpcon_decrement_counter(Counters, J, elm, dv_ptr) == BP_FALSE) return BP_FALSE;
                if (ISINT(FOLLOW(dv_ptr))) return BP_TRUE;
            }
        }
        Es = FOLLOW(ptr+1); DEREF_NONVAR(Es);
    }
    return BP_TRUE;
}

/* If elm is in Xj's domain, decrements the support counter of (J,elm), removing elm
   from the domain when it reaches 0. */
int cpcon_decrement_counter(BPLONG Counters, BPLONG J, BPLONG elm, BPLONG_PTR dv_ptr)
{
    BPLONG counter, count, p_key;
    BPLONG_PTR struct_ptr;

    if (dm_true(dv_ptr, elm)) {
        p_key = ADDTAG((BPLONG)heap_top, STR);
        FOLLOW(heap_top) = (BPLONG)cpcon_k2_ptr;
        FOLLOW(heap_top+1) = J;
        FOLLOW(heap_top+2) = MAKEINT(elm);
        counter = con05_hashtable_get(Counters, p_key);
        DEREF_NONVAR(counter);
        struct_ptr = (BPLONG_PTR)UNTAGGED_ADDR(counter);
        count = FOLLOW(struct_ptr+1);
        DEREF_NONVAR(count); count = INTVAL(count);
        count--;
        if (count == 0) {
            domain_set_false_noint(dv_ptr, elm);
        } else {
            PUSHTRAIL_H_ATOMIC(struct_ptr+1, FOLLOW(struct_ptr+1));
            FOLLOW(struct_ptr+1) = MAKEINT(count);
        }
    }
    return BP_TRUE;
}

/* Succeeds if op is a domain variable whose domain is 0..1. */
int b_BOOL_DVAR_c(BPLONG op)
{
    BPLONG_PTR dv_ptr;

    DEREF(op);
    if (IS_SUSP_VAR(op)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_TOPON_ADDR(op);
        if (IS_UN_DOMAIN(dv_ptr)) {
            return BP_FALSE;
        }
        if (DV_first(dv_ptr) == 0 && DV_last(dv_ptr) == 1) {
            return BP_TRUE;
        }
    }
    return BP_FALSE;
}

/* Initializes the small-domain range and registers this file's C predicates. */
void Cboot_domain() {

    fd_region_low = -3200;
    fd_region_up = 3200;

    insert_cpred("c_DM_CREATE_DVARS", 3, c_DM_CREATE_DVARS);
    insert_cpred("c_DM_CREATE_DVAR", 3, c_DM_CREATE_DVAR);
    insert_cpred("c_DM_CREATE_BV_DVAR", 3, c_DM_CREATE_BV_DVAR);
    insert_cpred("exclude_elm_dvars", 3, exclude_elm_dvars);
    insert_cpred("exclude_elm_vcs", 2, exclude_elm_vcs);
    insert_cpred("display_constraints", 0, display_constraints);
    insert_cpred("c_DM_INCLUDE", 2, c_DM_INCLUDE);
    insert_cpred("fd_vector_min_max", 2, fd_vector_min_max);
    insert_cpred("c_VV_EQ_C_CON", 3, c_VV_EQ_C_CON);
    insert_cpred("c_V_EQ_VC_CON", 3, c_V_EQ_VC_CON);
    insert_cpred("c_UU_EQ_C_CON", 5, c_UU_EQ_C_CON);
    insert_cpred("c_U_EQ_UC_CON", 5, c_U_EQ_UC_CON);

    insert_cpred("c_reachability_test", 1, c_reachability_test);
    insert_cpred("c_subcircuit_test", 2, c_subcircuit_test);
    insert_cpred("c_path_from_to_reachability_test", 3, c_path_from_to_reachability_test);

    insert_cpred("c_create_susp_var", 1, c_create_susp_var);
    insert_cpred("c_integers_intervals_list", 4, c_integers_intervals_list);

    cpcon_k2_ptr = BP_NEW_SYM("k", 2);
    insert_cpred("c_cpcon_decrement_counters", 4, c_cpcon_decrement_counters);
    insert_cpred("c_CLPFD_ADD_AC_ccc", 3, c_CLPFD_ADD_AC_ccc);
    insert_cpred("c_CLPFD_SUB_AC_ccc", 3, c_CLPFD_SUB_AC_ccc);
}


