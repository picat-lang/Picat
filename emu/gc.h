/********************************************************************
 *   File   : gc.h
 *   Author : Neng-Fa ZHOU Copyright (C) 1994-2022

 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 ********************************************************************/

/* GC scratch state is per-engine: see PAR_TLS in basic.h. basic.h is
   included before gc.h in every GC compilation unit. */
#ifndef PAR_TLS
#define PAR_TLS
#endif

typedef struct {
    BPLONG_PTR addr;
    BPLONG term;
} GcQueueCell;

extern PAR_TLS GcQueueCell *gcQueue;

#define gcQueueInit {                           \
        gcQueueFront = 0;                       \
        gcQueueRear = -1;                       \
        gcQueueCount = 0;                       \
    }

extern PAR_TLS BPLONG gcQueueSize;
extern PAR_TLS BPLONG gcQueueFront, gcQueueRear, gcQueueCount;

extern BPLONG cg_all_components;

#define GCQueueAdd(ptr, cont) {                         \
        if (gcQueueCount == gcQueueSize) {              \
            gcQueueExpand();                            \
        }                                               \
        gcQueueRear = (gcQueueRear+1) % gcQueueSize;    \
        gcQueue[gcQueueRear].addr = (BPLONG_PTR)ptr;    \
        gcQueue[gcQueueRear].term = cont;               \
        gcQueueCount++;                                 \
    }

#define GCQueueAddTerm(cont) {                          \
        if (gcQueueCount == gcQueueSize) {              \
            gcQueueExpand();                            \
        }                                               \
        gcQueueRear = (gcQueueRear+1) % gcQueueSize;    \
        gcQueue[gcQueueRear].term = cont;               \
        gcQueueCount++;                                 \
    }

#define GCQueueGet(ptr, cont) {                         \
        ptr = gcQueue[gcQueueFront].addr;               \
        cont = gcQueue[gcQueueFront].term;              \
        gcQueueFront = (gcQueueFront+1) % gcQueueSize;  \
        gcQueueCount--;                                 \
    }

#define GCQueuePop(term1, term2) {                      \
        term1 = (BPLONG)gcQueue[gcQueueRear].addr;      \
        term2 = gcQueue[gcQueueRear].term;              \
        gcQueueRear = (gcQueueRear-1) % gcQueueSize;    \
        gcQueueCount--;                                 \
    }

#define GCQueueGetTerm(cont) {                          \
        cont = gcQueue[gcQueueFront].term;              \
        gcQueueFront = (gcQueueFront+1) % gcQueueSize;  \
        gcQueueCount--;                                 \
    }

/* use the queue as a stack */
#define GCQueueGetFromRear(ptr, cont) {                 \
        ptr = gcQueue[gcQueueRear].addr;                \
        cont = gcQueue[gcQueueRear].term;               \
        gcQueueRear = (gcQueueRear-1) % gcQueueSize;    \
        gcQueueCount--;                                 \
    }

extern PAR_TLS int gcDynamicArraySize;
extern PAR_TLS int gcDynamicArrayCount;
extern PAR_TLS BPLONG_PTR gcDynamicArray;

#define gcAddDynamicArray(elm) {                                \
        if (gcDynamicArrayCount >= gcDynamicArraySize) {        \
            gcExpandDynamicArray();                             \
        }                                                       \
        FOLLOW(gcDynamicArray+gcDynamicArrayCount) = elm;       \
        gcDynamicArrayCount++;                                  \
    }

extern PAR_TLS BPLONG_PTR global_mask_ptr;
extern PAR_TLS BPLONG global_mask_size;

#define GCSetMaskBit(addr, base) {                                      \
        BPULONG offset = ((BPULONG)addr-(BPULONG)base)/sizeof(BPLONG);  \
        BPLONG_PTR word_ptr = global_mask_ptr+offset/NBITS_IN_LONG;     \
        BPULONG word = *word_ptr;                                       \
        BPULONG bitPosition = offset % NBITS_IN_LONG;                   \
        *(word_ptr) = (word | ((BPULONG)0x1 << bitPosition));           \
    }

#define MARK_FRAME(f) AR_AR(f) = ADDTAG3(AR_AR(f), 0x1)
#define FRAME_IS_MARKED(f) (AR_AR(f) & 0x1)
#define UNMARK_FRAME(f) AR_AR(f) = UNTAGGED3(AR_AR(f))

