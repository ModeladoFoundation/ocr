/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __HC_SCHEDULER_HEURISTIC_H__
#define __HC_SCHEDULER_HEURISTIC_H__

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_HC

#include "ocr-hal.h"
#include "ocr-scheduler-heuristic.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-scheduler-object.h"

/****************************************************/
/* HC SCHEDULER_HEURISTIC                           */
/****************************************************/

// Cached information about context
typedef struct _ocrSchedulerHeuristicContextHc_t {
    ocrSchedulerHeuristicContext_t base;
    ocrSchedulerObject_t *mySchedulerObject;    // The deque owned by a specific worker (context)
    u64 stealSchedulerObjectIndex;        // Cached index of the deque lasted visited during steal attempts
#if 0 // Example fields for simulation mode
    ocrSchedulerObjectActionSet_t singleActionSet;
    ocrSchedulerObjectAction_t insertAction;
    ocrSchedulerObjectAction_t removeAction;
#endif
} ocrSchedulerHeuristicContextHc_t;

typedef struct _ocrSchedulerHeuristicHc_t {
    ocrSchedulerHeuristic_t base;
#ifdef OCR_ENABLE_SCHEDULER_SPAWN_QUEUE
    lock_t lock;
#endif
} ocrSchedulerHeuristicHc_t;

/****************************************************/
/* HC SCHEDULER_HEURISTIC FACTORY                   */
/****************************************************/

typedef struct _paramListSchedulerHeuristicHc_t {
    paramListSchedulerHeuristic_t base;
} paramListSchedulerHeuristicHc_t;

typedef struct _ocrSchedulerHeuristicFactoryHc_t {
    ocrSchedulerHeuristicFactory_t base;
} ocrSchedulerHeuristicFactoryHc_t;

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryHc(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_SCHEDULER_HEURISTIC_HC */
#endif /* __HC_SCHEDULER_HEURISTIC_H__ */

