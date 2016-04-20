/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __HC_EVENT_H__
#define __HC_EVENT_H__

#include "ocr-config.h"
#ifdef ENABLE_EVENT_HC

#include "hc/hc.h"
#include "ocr-event.h"
#include "ocr-hal.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"

#ifdef ENABLE_HINTS
/**< The number of hint properties supported by this implementation */
#define OCR_HINT_COUNT_EVT_HC   0
#else
#define OCR_HINT_COUNT_EVT_HC   0
#endif

// Size for the static waiter array
#ifndef HCEVT_WAITER_STATIC_COUNT
#define HCEVT_WAITER_STATIC_COUNT 64
#endif

// Size for the dynamically allocated waiter array
#ifndef HCEVT_WAITER_DYNAMIC_COUNT
#define HCEVT_WAITER_DYNAMIC_COUNT 4
#endif

typedef struct {
    ocrEventFactory_t base;
} ocrEventFactoryHc_t;

typedef struct ocrEventHc_t {
    ocrEvent_t base;
    regNode_t waiters[HCEVT_WAITER_STATIC_COUNT]; /**< hold waiters. If overflows a dynamically
                                              allocated waiter list is stored in waitersDb */
    ocrFatGuid_t waitersDb; /**< DB containing an array of regNode_t listing the
                             * events/EDTs depending on this event */
    volatile u32 waitersCount; /**< Number of waiters in waitersDb */
    u32 waitersMax; /**< Maximum number of waiters in waitersDb */
    lock_t waitersLock;
    ocrRuntimeHint_t hint;
} ocrEventHc_t;

typedef struct _ocrEventHcPersist_t {
    ocrEventHc_t base;
    ocrGuid_t data;
} ocrEventHcPersist_t;

typedef struct _ocrEventHcCounted_t {
    ocrEventHcPersist_t base;
    u64 nbDeps; // this is only updated inside a lock
} ocrEventHcCounted_t;

typedef struct _ocrEventHcLatch_t {
    ocrEventHc_t base;
    s32 counter;
} ocrEventHcLatch_t;

typedef struct _ocrEventHcChannel_t {
    ocrEventHc_t base;
    u32 maxGen; // Maximum number of generations simultaneously in flight
    u32 nbSat;  // Number of Satisfy per generation
    u32 nbDeps; // Number of dependences per generation
    // Data-Structure to hold satisfy values
    u32 headSat;
    u32 tailSat;
    u32 satBufSz; // = maxGen * nbSat
    ocrGuid_t * satBuffer; // An array of GUID values, possibly multi-dimensional and linearized
    // Data-Structure to hold dependence registrations
    u32 headWaiter;
    u32 tailWaiter;
    u32 waitBufSz; // = maxGen * nbDeps
    regNode_t * waiters; // An array of registration node, possibly multi-dimensional and linearized
} ocrEventHcChannel_t;


ocrEventFactory_t* newEventFactoryHc(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_EVENT_HC */
#endif /* __HC_EVENT_H__ */
