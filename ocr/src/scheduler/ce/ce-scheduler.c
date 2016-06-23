/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_CE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "policy-domain/ce/ce-policy.h"
#include "scheduler/ce/ce-scheduler.h"

// BUG #619: This relies on data in ce-worker (its ID to do the mapping)
// This is non-portable (CE scheduler does not work with non
// CE worker) but works for now
#include "worker/ce/ce-worker.h" // NON PORTABLE FOR NOW

#define DEBUG_TYPE SCHEDULER

#define SCHED10 1
#if !SCHED10
#pragma message "Scheduler framework is v0.9"
#endif

/******************************************************/
/* Support structures                                 */
/******************************************************/
static inline void workpileIteratorReset(ceWorkpileIterator_t *base) {
    base->curr = ((base->id) + 1) % base->mod;
}

static inline bool workpileIteratorHasNext(ceWorkpileIterator_t * base) {
    return base->id != base->curr;
}

static inline ocrWorkpile_t * workpileIteratorNext(ceWorkpileIterator_t * base) {
    u64 current = base->curr;
    ocrWorkpile_t * toBeReturned = base->workpiles[current];
    base->curr = (current+1) % base->mod;
    return toBeReturned;
}

static inline void initWorkpileIterator(ceWorkpileIterator_t *base, u64 id,
                                        u64 workpileCount, ocrWorkpile_t ** workpiles ) {

    base->workpiles = workpiles;
    base->id = id;
    base->mod = workpileCount;
    // The 'curr' field is initialized by reset
    workpileIteratorReset(base);
}

/******************************************************/
/* OCR-CE SCHEDULER                                   */
/******************************************************/

static inline ocrWorkpile_t * popMappingOneToOne (ocrScheduler_t* base, u64 workerId ) {
    return base->workpiles[0];
}

static inline ocrWorkpile_t * pushMappingOneToOne (ocrScheduler_t* base, u64 workerId ) {
    return base->workpiles[0];
}

static inline ceWorkpileIterator_t* stealMappingOneToAllButSelf (ocrScheduler_t* base, u64 workerId ) {
    ocrSchedulerCe_t* derived = (ocrSchedulerCe_t*) base;
    ceWorkpileIterator_t * stealIterator = &(derived->stealIterators[0]);
    workpileIteratorReset(stealIterator);
    return stealIterator;
}

void ceSchedulerDestruct(ocrScheduler_t * self) {
    u64 i;
    // Destruct the workpiles
    u64 count = self->workpileCount;
    for(i = 0; i < count; ++i) {
        self->workpiles[i]->fcts.destruct(self->workpiles[i]);
    }
    runtimeChunkFree((u64)(self->workpiles), NULL);

    runtimeChunkFree((u64)self, NULL);
}

u8 ceSchedulerSwitchRunlevel(ocrScheduler_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                             phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    u64 i;
    if(runlevel == RL_PD_OK && (properties & RL_BRING_UP) && phase == 0) {
        // First transition, setup some backpointers
        for(i = 0; i < self->schedulerHeuristicCount; ++i) {
            self->schedulerHeuristics[i]->scheduler = self;
        }
    }

    // Take care of all other sub-objects
    for(i = 0; i < self->workpileCount; ++i) {
        toReturn |= self->workpiles[i]->fcts.switchRunlevel(
            self->workpiles[i], PD, runlevel, phase, properties, NULL, 0);
    }

    ocrSchedulerObjectFactory_t *rootFact = (ocrSchedulerObjectFactory_t*)PD->schedulerObjectFactories[self->rootObj->fctId];
    toReturn |= rootFact->fcts.switchRunlevel(self->rootObj, PD, runlevel, phase, properties, NULL, 0);
    // Do not re-order: Scheduler object root should be brought up before heuristics
    for(i = 0; i < self->schedulerHeuristicCount; ++i) {
        toReturn |= self->schedulerHeuristics[i]->fcts.switchRunlevel(
            self->schedulerHeuristics[i], PD, runlevel, phase, properties, NULL, 0);
    }

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        if((properties & RL_BRING_UP) && phase == 0) {
            RL_ENSURE_PHASE_UP(PD, RL_MEMORY_OK, RL_PHASE_SCHEDULER, 2);
            RL_ENSURE_PHASE_DOWN(PD, RL_MEMORY_OK, RL_PHASE_SCHEDULER, 2);
        }
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            self->pd = PD;
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        // We have memory, allocate all our structures
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
            // allocate steal iterator cache. Use pdMalloc since this is something
            // local to the policy domain and that will never be shared
            ceWorkpileIterator_t * stealIteratorsCache = self->pd->fcts.pdMalloc(
                self->pd, sizeof(ceWorkpileIterator_t)*self->workpileCount);

            // Initialize steal iterator cache
            for(i = 0; i < self->workpileCount; ++i) {
                // Note: here we assume workpile 'i' will match worker 'i' => Not great
                initWorkpileIterator(&(stealIteratorsCache[i]), i, self->workpileCount,
                                     self->workpiles);
            }
            ocrSchedulerCe_t * derived = (ocrSchedulerCe_t *) self;
            derived->stealIterators = stealIteratorsCache;
        }

        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_GUID_OK, phase)) {
            ocrSchedulerCe_t *derived = (ocrSchedulerCe_t*)self;
            self->pd->fcts.pdFree(self->pd, derived->stealIterators);
        }
        break;
    case RL_COMPUTE_OK:
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
                // We get a GUID for ourself
                guidify(self->pd, (u64)self, &(self->fguid), OCR_GUID_SCHEDULER);
            }
        } else {
            // Tear-down
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
                msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(guid) = self->fguid;
                PD_MSG_FIELD_I(properties) = 0;
                toReturn |= self->pd->fcts.processMessage(self->pd, &msg, false);
                self->fguid.guid = NULL_GUID;
#undef PD_MSG
#undef PD_TYPE
            }
        }
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

u8 ceSchedulerTake (ocrScheduler_t *self, u32 *count, ocrFatGuid_t *edts) {
    // BUG #619: Source must be a worker guid and we rely on indices to map
    // workers to workpiles (one-to-one)
    // This is a non-portable assumption but will do for now.
    ocrWorker_t *worker = NULL;
    ocrWorkerCe_t *ceWorker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ceWorker = (ocrWorkerCe_t*)worker;

    if(*count == 0) return 1; // No room to put anything

    ASSERT(edts != NULL); // Array should be allocated at least

    u64 workerId = ceWorker->id;
    // First try to pop
    ocrWorkpile_t * wpToPop = popMappingOneToOne(self, workerId);
    // BUG #619: Add cost again
    ocrFatGuid_t popped = wpToPop->fcts.pop(wpToPop, POP_WORKPOPTYPE, NULL);
    // In this implementation we expect the caller to have
    // allocated memory for us since we can return at most one
    // guid (most likely store using the address of a local)
    if(NULL_GUID != popped.guid) {
        *count = 1;
    } else {
        *count = 0;
        popped.guid = NULL_GUID;
        popped.metaDataPtr = NULL;
    }
    edts[0] = popped;
    return 0;
}

u8 ceSchedulerGive (ocrScheduler_t* base, u32* count, ocrFatGuid_t* edts) {
    // BUG #619: Source must be a worker guid and we rely on indices to map
    // workers to workpiles (one-to-one)
    // This is a non-portable assumption but will do for now.
    ocrWorker_t *worker = NULL;
    ocrWorkerCe_t *ceWorker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ceWorker = (ocrWorkerCe_t*)worker;

    // Source must be a worker guid
    u64 workerId = ceWorker->id;
    ocrWorkpile_t * wpToPush = pushMappingOneToOne(base, workerId);
    u32 i = 0;
    for ( ; i < *count; ++i ) {
        wpToPush->fcts.push(wpToPush, PUSH_WORKPUSHTYPE, edts[i]);
        edts[i].guid = NULL_GUID;
    }
    *count = 0;
    return 0;
}

u8 ceSchedulerTakeComm(ocrScheduler_t *self, u32* count, ocrFatGuid_t* handlers, u32 properties) {
    return OCR_ENOSYS;
}

u8 ceSchedulerGiveComm(ocrScheduler_t *self, u32* count, ocrFatGuid_t* handlers, u32 properties) {
    return OCR_ENOSYS;
}

///////////////////////////////
//      Scheduler 1.0        //
///////////////////////////////

u8 ceSchedulerGetWorkInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        {
#if SCHED10
            ocrSchedulerHeuristic_t *schedulerHeuristic = self->schedulerHeuristics[0];
            return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke(schedulerHeuristic, opArgs, hints);
#else
            u32 count = 1;
            return self->fcts.takeEdt(self, &count, &taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
#endif
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    return OCR_ENOTSUP;
}

u8 ceSchedulerNotifyInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
        {
#if SCHED10
            ocrSchedulerHeuristic_t *schedulerHeuristic = self->schedulerHeuristics[0];
            return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke(schedulerHeuristic, opArgs, hints);
#else
            u32 count = 1;
            return self->fcts.giveEdt(self, &count, &notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid);
#endif
        }
    case OCR_SCHED_NOTIFY_EDT_DONE: {
#if SCHED10
            ocrSchedulerHeuristic_t *schedulerHeuristic = self->schedulerHeuristics[0];
            return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke(schedulerHeuristic, opArgs, hints);
#else
            // Destroy the work
            ocrPolicyDomain_t *pd;
            PD_MSG_STACK(msg);
            getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
            getCurrentEnv(NULL, NULL, NULL, &msg);
            msg.type = PD_MSG_WORK_DESTROY | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(guid) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid;
            PD_MSG_FIELD_I(currentEdt) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid;
            PD_MSG_FIELD_I(properties) = 0;
            RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
#endif
        }
        break;
    // Notifies ignored by this scheduler
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerTransactInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    return OCR_ENOTSUP;
}

u8 ceSchedulerAnalyzeInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    return OCR_ENOTSUP;
}

u8 ceSchedulerUpdate(ocrScheduler_t *self, u32 properties) {
#if SCHED10
    u32 i;
    switch(properties) {
    case OCR_SCHEDULER_UPDATE_PROP_IDLE: {
            for (i = 0; i < self->schedulerHeuristicCount; i++) {
                ocrSchedulerHeuristic_t *schedulerHeuristic = self->schedulerHeuristics[i];
                schedulerHeuristic->fcts.update(schedulerHeuristic, OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_IDLE);
            }
        }
        break;
    case OCR_SCHEDULER_UPDATE_PROP_SHUTDOWN: {
            for (i = 0; i < self->schedulerHeuristicCount; i++) {
                ocrSchedulerHeuristic_t *schedulerHeuristic = self->schedulerHeuristics[i];
                schedulerHeuristic->fcts.update(schedulerHeuristic, OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_SHUTDOWN);
            }
        }
        break;
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
#endif
    return 0;
}

ocrScheduler_t* newSchedulerCe(ocrSchedulerFactory_t * factory, ocrParamList_t *perInstance) {
    ocrScheduler_t* base = (ocrScheduler_t*) runtimeChunkAlloc(
                               sizeof(ocrSchedulerCe_t), PERSISTENT_CHUNK);
    factory->initialize(factory, base, perInstance);
    return base;
}

void initializeSchedulerCe(ocrSchedulerFactory_t *factory, ocrScheduler_t *self, ocrParamList_t *perInstance) {
    initializeSchedulerOcr(factory, self, perInstance);
}

void destructSchedulerFactoryCe(ocrSchedulerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrSchedulerFactory_t * newOcrSchedulerFactoryCe(ocrParamList_t *perType) {
    ocrSchedulerFactoryCe_t* derived = (ocrSchedulerFactoryCe_t*) runtimeChunkAlloc(
                                           sizeof(ocrSchedulerFactoryCe_t), NONPERSISTENT_CHUNK);

    ocrSchedulerFactory_t* base = (ocrSchedulerFactory_t*) derived;
    base->instantiate = &newSchedulerCe;
    base->initialize  = &initializeSchedulerCe;
    base->destruct = &destructSchedulerFactoryCe;
    base->schedulerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                          phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), ceSchedulerSwitchRunlevel);
    base->schedulerFcts.destruct = FUNC_ADDR(void (*)(ocrScheduler_t*), ceSchedulerDestruct);
    base->schedulerFcts.takeEdt = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*), ceSchedulerTake);
    base->schedulerFcts.giveEdt = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*), ceSchedulerGive);
    base->schedulerFcts.takeComm = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*, u32), ceSchedulerTakeComm);
    base->schedulerFcts.giveComm = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*, u32), ceSchedulerGiveComm);

    //Scheduler 1.0
    base->schedulerFcts.update = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32), ceSchedulerUpdate);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerGetWorkInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerNotifyInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerTransactInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerAnalyzeInvoke);
    return base;
}

#endif /* ENABLE_SCHEDULER_CE */
