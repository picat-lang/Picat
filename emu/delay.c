/********************************************************************
 *   File   : delay.c
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2026
 *   Purpose: Primitives for suspension variables and agents

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/
#include <string.h>
#include "bprolog.h"
#include "event.h"
#include "frame.h"

BPLONG build_delayed_call_on_the_heap(BPLONG_PTR frame)
{
    SYM_REC_PTR sym_ptr;
    BPLONG arity, i;
    BPLONG call, arg;
    BPLONG_PTR top;

    sym_ptr = (SYM_REC_PTR)FOLLOW((BPLONG_PTR)AR_REEP(frame)+2);
    arity = GET_ARITY(sym_ptr);

    /*  printf("name=%s/arity=%d\n",GET_NAME(sym_ptr),arity); */

    for (i = arity; i > 0; i--) {
        arg = FOLLOW(frame+i);
        if (ISREF(arg) && ISFREE(arg)) {
            FOLLOW(arg) = (BPLONG)heap_top;
            PUSHTRAIL(arg);
            NEW_HEAP_FREE;
        }
    }

    call = ADDTAG(heap_top, STR);
    FOLLOW(heap_top++) = (BPLONG)sym_ptr;
    for (i = arity; i > 0; i--) {
        arg = FOLLOW(frame+i);
        DEREF(arg);
        if (IS_SUSP_VAR(arg))
            NEW_HEAP_NODE(UNTAGGED_TOPON_ADDR(arg));
        else
            NEW_HEAP_NODE(arg);
    }
    LOCAL_OVERFLOW_CHECK("delay");
    return call;
}


int c_frozen_cf() {
    register BPLONG var, return_goal;
    BPLONG goal;
    BPLONG_PTR dv_ptr, dcs, list;
    BPLONG_PTR top;

    var = ARG(1, 2);
    return_goal = ARG(2, 2);

    DEREF(var);
    list = heap_top;
    goal = (BPLONG)list;
    *heap_top = (BPLONG)heap_top; heap_top++;
    if (IS_SUSP_VAR(var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_ADDR(var);
        dcs = (BPLONG_PTR)DV_ins_cs(dv_ptr);
        list = frozen_cs(dcs, list);
        if ((BPLONG)list == -1) return BP_ERROR;

        dcs = (BPLONG_PTR)DV_minmax_cs(dv_ptr);
        list = frozen_cs(dcs, list);
        if ((BPLONG)list == -1) return BP_ERROR;

        dcs = (BPLONG_PTR)DV_dom_cs(dv_ptr);
        list = frozen_cs(dcs, list);
        if ((BPLONG)list == -1) return BP_ERROR;

        dcs = (BPLONG_PTR)DV_outer_dom_cs(dv_ptr);
        list = frozen_cs(dcs, list);
        if ((BPLONG)list == -1) return BP_ERROR;

        FOLLOW(list) = nil_sym;

        return unify(return_goal, goal);
    }
    else return unify(return_goal, true_atom);
}

BPLONG_PTR frozen_cs(BPLONG_PTR cs, BPLONG_PTR Plist)
{
    BPLONG tmp;
    BPLONG_PTR frame;

    while (ISLIST((BPLONG)cs)) {
        cs = (BPLONG_PTR)UNTAGGED_ADDR(cs);
        frame = (BPLONG_PTR)((BPULONG)stack_up_addr-(BPULONG)UNTAGGED_CONT(FOLLOW(cs)));

        /*  A suspension frame lives in the basic region (allocated from the
            LOCAL_TOP front, mirrored into the thread-local variable
            local_top by SAVE_TOP before every built-in call).
            frame > local_top iff its block is still allocated;
            otherwise its memory was reclaimed by a backtrack and it
            must never be resurrected.  */
        if (!FRAME_IS_DEAD(frame) && frame > local_top) {
            tmp = build_delayed_call_on_the_heap(frame);
            if (tmp == -1) return (BPLONG_PTR)-1;
            FOLLOW(Plist) = ADDTAG(heap_top, LST);
            NEW_HEAP_NODE(tmp);
            Plist = heap_top; heap_top++;
        }
        cs = (BPLONG_PTR)LIST_NEXT(cs);
    }
    return Plist;
}

/*  A suspension frame is born when the instantiation event of a dvar
    launches a frozen/delayed call that then suspends; the source dvar
    appears among the frame's call arguments.  The frame is meaningful
    only while some dvar of its call is still instantiated (the trigger
    still holds; re-firing after a search rewind re-derives the same
    constraint, which is harmless).  When a search exhausts and backtracks
    away, its still-sleeping frames sit below every remaining choice-point
    snapshot, so no backtrack ever abandons them; with their source dvars
    re-opened (uninstantiated), they are zombie calls that must be
    discarded (Bug E).  Frames whose arguments contain no dvar keep the
    legacy behaviour.  */
int sf_frame_source_alive(BPLONG_PTR frame)
{
    SYM_REC_PTR sym_ptr;
    BPLONG arity, i;
    BPLONG arg;
    BPLONG_PTR dv;
    int seen_dvar = 0;

    sym_ptr = (SYM_REC_PTR)FOLLOW((BPLONG_PTR)AR_REEP(frame)+2);
    arity = GET_ARITY(sym_ptr);
    for (i = arity; i > 0; i--) {
        arg = FOLLOW(frame+i);
        DEREF(arg);
        if (IS_SUSP_VAR(arg)) {
            seen_dvar = 1;
            dv = (BPLONG_PTR)UNTAGGED_ADDR(arg);
            /*  instantiated iff its variable cell no longer holds a
                suspended (free) value (see unify.c, dvar unification).  */
            if (!IS_SUSP_VAR(FOLLOW(dv)))
                return 1;
        }
    }
    return seen_dvar ? 0 : 1;
}

int c_frozen_f() {
    BPLONG P_goal, P_goal_rest;
    BPLONG cell, tmp;
    BPLONG_PTR frame;
    BPLONG_PTR top;

    P_goal = ARG(1, 1);
    DEREF(P_goal);
    if (!ISREF(P_goal)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }

    frame = sfreg;
    while (AR_PREV(frame) != (BPLONG)frame) {  /* end of chain */
        /*  sfreg is append-only, so it accumulates frames of dead
            searches.  A frame is usable only while its block is still
            allocated (frame > local_top, see frozen_cs) and its trigger
            still holds (sf_frame_source_alive).  Frames abandoned by a
            backtrack are already SUSP_EXIT (SF_MARK_ABANDONED in
            emu_inst.h).  What remains is zombie material from an
            exhausted or failed search (Bug E): discard it here.  */
        if (!FRAME_IS_DEAD(frame) && frame > local_top) {
            if (sf_frame_source_alive(frame)) {
                tmp = build_delayed_call_on_the_heap(frame);
                if (tmp == -1) return BP_ERROR;
                cell = bp_build_list();
                unify(bp_get_car(cell), tmp);
                P_goal_rest = bp_get_cdr(cell);
                unify(P_goal, cell);
                P_goal = P_goal_rest;
            }
            else
                AR_STATUS(frame) = SUSP_EXIT;  /* zombie: kill it here */
        }
        frame = (BPLONG_PTR)AR_PREV(frame);
    }
    return unify(P_goal, bp_build_nil());
}

/*
  susp_attach_term(Var,Term) 
  attach T to the suspension variable Var.
  Exception if Var is non-variable
*/
int b_SUSP_ATTACH_TERM_cc(BPLONG Var, BPLONG Term)
{

    BPLONG_PTR top, dv_ptr;

    DEREF(Var);
    DEREF(Term);
    if (ISREF(Term)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
    if (ISREF(Var)) {
        CREATE_SUSP_VAR_nocs(Var);  /* dv_ptr set */
        DV_attached(dv_ptr) = Term;
        return 1;
    } else if (IS_SUSP_VAR(Var)) {
        dv_ptr = (BPLONG_PTR)UNTAGGED_ADDR(Var);
        top = A_DV_attached(dv_ptr);
        PUSHTRAIL_H_NONATOMIC(top, FOLLOW(top));
        DV_attached(dv_ptr) = Term;
        return 1;
    } else {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    }
}

/*
  susp_attached_term(Var,Term) 
  the attached term to Var is Term
  Exception if Var is not a suspension variable
*/
int b_SUSP_ATTACHED_TERM_cf(BPLONG Var, BPLONG Term)
{
    BPLONG_PTR top, dv_ptr;

    DEREF(Var);
    if (!IS_SUSP_VAR(Var)) {
        bp_exception = illegal_arguments;
        return BP_ERROR;
    } else {
        dv_ptr = (BPLONG_PTR)UNTAGGED_ADDR(Var);
        ASSIGN_sv_heap_term(Term, DV_attached(dv_ptr));
        return 1;
    }
}

int b_SUSP_VAR_c(BPLONG var)
{
    BPLONG_PTR top;

    DEREF(var);
    if (IS_SUSP_VAR(var)) return 1; else return 0;
}

/* skip all the dead constraints in the list */
BPLONG next_alive_susp_call(BPLONG cs_list, BPLONG_PTR breg)
{
    BPLONG_PTR constr_ar, ptr;

    while (cs_list != nil_sym) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(cs_list);
        constr_ar = (BPLONG_PTR)((BPULONG)stack_up_addr-(BPULONG)UNTAGGED_CONT(FOLLOW(ptr)));
        if (AR_STATUS(constr_ar) == SUSP_EXIT && constr_ar < breg) {  /* permanently dead */
            /*
              printf("skip %x (breg=%x)\n",constr_ar,breg);
              show_frame(constr_ar);
            */
            cs_list = FOLLOW(ptr+1);
        } else return cs_list;
    }
    return cs_list;
}

void print_cs(BPLONG cs_list)
{
    BPLONG_PTR constr_ar, ptr;

    while (cs_list != nil_sym) {
        ptr = (BPLONG_PTR)UNTAGGED_ADDR(cs_list);
        constr_ar = (BPLONG_PTR)((BPULONG)stack_up_addr-(BPULONG)UNTAGGED_CONT(FOLLOW(ptr)));
        if (AR_STATUS(constr_ar) == SUSP_EXIT) {
            printf("-");
        } else {
            printf("*");
        }
        cs_list = FOLLOW(ptr+1);
    }
    printf("\n");
}

int c_reset_store(){
    BPLONG_PTR frame;

    frame = sfreg;
    while (AR_PREV(frame) != (BPLONG)frame) {  /* end of chain */
        if (!FRAME_IS_DEAD(frame)) {
            AR_STATUS(frame) = SUSP_EXIT;
        }
        frame = (BPLONG_PTR)AR_PREV(frame);
    }
    return BP_TRUE;
}

/*  Reset the process-global CP store at a problem boundary.  Marks every
    non-EXIT suspension (delay/watcher) frame in the active frame chain
    (sfreg) as SUSP_EXIT, clears the pending trigger queue (mirroring
    lab_fail in emu_inst.h) and restores fd_region_low/up to their initial
    values (domain.c).  Stale frames that outlived their search (see
    SF_MARK_ABANDONED in emu_inst.h and the Bug E discussion in the
    project docs) are thus never resurrected by c_frozen_f.  Frames of
    the live computation are included: call only at
    a boundary where no pending suspended call is meant to survive (e.g.
    between two count_all calls).  */
int c_cp_reset_store()
{
    BPLONG_PTR frame;

    frame = sfreg;
    while (AR_PREV(frame) != (BPLONG)frame) {
        if (!FRAME_IS_DEAD(frame))
            AR_STATUS(frame) = SUSP_EXIT;
        frame = (BPLONG_PTR)AR_PREV(frame);
    }
    trigger_no = 0;
    toam_signal_vec &= (INTERRUPT | EVENT_POOL_NONEMPTY);
    fd_region_low = -3200;
    fd_region_up = 3200;
    return BP_TRUE;
}

void Cboot_delay()
{

    insert_cpred("c_frozen_cf", 2, c_frozen_cf);
    insert_cpred("c_frozen_f", 1, c_frozen_f);
    insert_cpred("c_reset_store", 0, c_reset_store);
    insert_cpred("cp_reset_store", 0, c_cp_reset_store);
}


