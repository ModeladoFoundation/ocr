/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __CE_SCHEDULER_HEURISTIC_H__
#define __CE_SCHEDULER_HEURISTIC_H__

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_CE

#include "ocr-scheduler-heuristic.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-scheduler-object.h"

#include "rmd-map.h"

/****************************************************/
/* CE SCHEDULER_HEURISTIC                           */
/****************************************************/

// Cached information about context
typedef struct _ocrSchedulerHeuristicContextCe_t {
    ocrSchedulerHeuristicContext_t base;
    ocrSchedulerObject_t *mySchedulerObject;    // The deque owned by a specific worker (context)
    u64 stealSchedulerObjectIndex;              // Cached index of the deque lasted visited during steal attempts
    bool inWorkRequestPending;                  // Work request coming in from remote location (remote loc is out of work)
    bool outWorkRequestPending;                 // Work request sent out to this context's remote location (current loc is out of work)
    bool canAcceptWorkRequest;                  // Identifies a context that can accept a work request from current CE
} ocrSchedulerHeuristicContextCe_t;

typedef struct _ocrSchedulerHeuristicCe_t {
    ocrSchedulerHeuristic_t base;
    u64 workCount;
    u32 inPendingCount;
    u32 pendingXeCount;                         // Number of XE's currently active (not sleeping)
    u32 workRequestStartIndex;                  // Context id of the location to which last out work request was sent
    u32 outWorkVictimsAvailable;                // The remaining number of requests that can be made
} ocrSchedulerHeuristicCe_t;

/****************************************************/
/* CE SCHEDULER_HEURISTIC FACTORY                   */
/****************************************************/

typedef struct _paramListSchedulerHeuristicCe_t {
    paramListSchedulerHeuristic_t base;
} paramListSchedulerHeuristicCe_t;

typedef struct _ocrSchedulerHeuristicFactoryCe_t {
    ocrSchedulerHeuristicFactory_t base;
} ocrSchedulerHeuristicFactoryCe_t;

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryCe(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_SCHEDULER_HEURISTIC_CE */
#endif /* __CE_SCHEDULER_HEURISTIC_H__ */

