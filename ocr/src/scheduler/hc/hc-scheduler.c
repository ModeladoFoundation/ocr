/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HC

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "scheduler/hc/hc-scheduler.h"
#include "scheduler/hc/scheduler-blocking-support.h"

#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
#include "worker/hc/hc-worker.h"
#endif

/******************************************************/
/* Support structures                                 */
/******************************************************/
static inline void workpileIteratorReset(hcWorkpileIterator_t *base) {
    base->curr = ((base->id) + 1) % base->mod;
}

static inline bool workpileIteratorHasNext(hcWorkpileIterator_t * base) {
    return base->id != base->curr;
}

static inline ocrWorkpile_t * workpileIteratorNext(hcWorkpileIterator_t * base) {
    u64 current = base->curr;
    ocrWorkpile_t * toBeReturned = base->workpiles[current];
    base->curr = (current+1) % base->mod;
    return toBeReturned;
}

static inline void initWorkpileIterator(hcWorkpileIterator_t *base, u64 id,
                                        u64 workpileCount, ocrWorkpile_t ** workpiles ) {

    base->workpiles = workpiles;
    base->id = id;
    base->mod = workpileCount;
    // The 'curr' field is initialized by reset
    workpileIteratorReset(base);
}

/******************************************************/
/* OCR-HC SCHEDULER                                   */
/******************************************************/

static inline ocrWorkpile_t * popMappingOneToOne (ocrScheduler_t* base, u64 workerId ) {
    u64 idx = (workerId - ((ocrSchedulerHc_t*)base)->workerIdFirst);
    return base->workpiles[idx];
}

static inline ocrWorkpile_t * pushMappingOneToOne (ocrScheduler_t* base, u64 workerId ) {
    u64 idx = (workerId - ((ocrSchedulerHc_t*)base)->workerIdFirst);
    return base->workpiles[idx];
}

static inline hcWorkpileIterator_t* stealMappingOneToAllButSelf (ocrScheduler_t* base, u64 workerId ) {
    u64 idx = (workerId - ((ocrSchedulerHc_t*)base)->workerIdFirst);
    ocrSchedulerHc_t* derived = (ocrSchedulerHc_t*) base;
    hcWorkpileIterator_t * stealIterator = &(derived->stealIterators[idx]);
    workpileIteratorReset(stealIterator);
    return stealIterator;
}

void hcSchedulerDestruct(ocrScheduler_t * self) {
    u64 i;
    // Destruct the workpiles
    u64 count = self->workpileCount;
    for(i = 0; i < count; ++i) {
        self->workpiles[i]->fcts.destruct(self->workpiles[i]);
    }

    runtimeChunkFree((u64)(self->workpiles), PERSISTENT_CHUNK);
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 hcSchedulerSwitchRunlevel(ocrScheduler_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                             phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    u64 i;
    if(properties & RL_BRING_UP) {
        // Take care of all other sub-objects
        for(i = 0; i < self->workpileCount; ++i) {
            toReturn |= self->workpiles[i]->fcts.switchRunlevel(
                self->workpiles[i], PD, runlevel, phase, properties, NULL, 0);
        }
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
        if((properties & RL_BRING_UP) && RL_IS_LAST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            // allocate steal iterator cache. Use pdMalloc since this is something
            // local to the policy domain and that will never be shared
            hcWorkpileIterator_t * stealIteratorsCache = self->pd->fcts.pdMalloc(
                self->pd, sizeof(hcWorkpileIterator_t)*self->workpileCount);
            ADebug(AllocDebugAllPD, "hc-scheduler.c/hcSchedulerSwitchRunlevel()/RL_MEMORY_OK "
                   "pdMalloc(%ld), addr=%p\n",
                   (sizeof(hcWorkpileIterator_t)*self->workpileCount), stealIteratorsCache);

            // Initialize steal iterator cache
            for(i = 0; i < self->workpileCount; ++i) {
                // Note: here we assume workpile 'i' will match worker 'i' => Not great
                initWorkpileIterator(&(stealIteratorsCache[i]), i, self->workpileCount,
                                     self->workpiles);
            }
            ocrSchedulerHc_t * derived = (ocrSchedulerHc_t *) self;
            derived->stealIterators = stealIteratorsCache;
        }

        if((properties & RL_TEAR_DOWN) && RL_IS_FIRST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
            ocrSchedulerHc_t *derived = (ocrSchedulerHc_t*)self;
            self->pd->fcts.pdFree(self->pd, derived->stealIterators);
        }
        break;
    case RL_GUID_OK:
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

    if(properties & RL_TEAR_DOWN) {
        // Take care of all other sub-objects
        for(i = 0; i < self->workpileCount; ++i) {
            toReturn |= self->workpiles[i]->fcts.switchRunlevel(
                self->workpiles[i], PD, runlevel, phase, properties, NULL, 0);
        }
    }
    return toReturn;
}

u8 hcSchedulerTakeEdt (ocrScheduler_t *self, u32 *count, ocrFatGuid_t *edts) {
    // Source must be a worker guid and we rely on indices to map
    // workers to workpiles (one-to-one)
    // BUG #586: This is a non-portable assumption but will do for now.

    if(*count == 0) return 1; // No room to put anything

    ocrWorker_t *worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ocrFatGuid_t popped;
    u64 workerId = worker->id;
    ASSERT(edts != NULL); // Array should be allocated at least
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    ocrWorkerHc_t * hcWorker = (ocrWorkerHc_t *) worker;
    if (!hcWorker->stealFirst) {
#endif
        {
            START_PROFILE(sched_hc_Pop);
            // First try to pop
            ocrWorkpile_t * wpToPop = popMappingOneToOne(self, workerId);
            popped = wpToPop->fcts.pop(wpToPop, POP_WORKPOPTYPE, NULL);
            EXIT_PROFILE;
        }
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    } else {
        hcWorker->stealFirst = false;
        popped.guid = NULL_GUID;
    }
#endif

    {
        START_PROFILE(sched_hc_Steal);
        if(ocrGuidIsNull(popped.guid)) {
            // If popping failed, try to steal
            hcWorkpileIterator_t* it = stealMappingOneToAllButSelf(self, workerId);
            while(workpileIteratorHasNext(it) && (ocrGuidIsNull(popped.guid))) {
                ocrWorkpile_t * next = workpileIteratorNext(it);
                popped = next->fcts.pop(next, STEAL_WORKPOPTYPE, NULL);
            }
        }
        EXIT_PROFILE;
    }

    // In this implementation we expect the caller to have
    // allocated memory for us since we can return at most one
    // guid (most likely store using the address of a local)
    if(!(ocrGuidIsNull(popped.guid))) {
        *count = 1;
        edts[0] = popped;
    } else {
        *count = 0;
    }
    return 0;
}

u8 hcSchedulerGiveEdt (ocrScheduler_t* base, u32* count, ocrFatGuid_t* edts) {
    // Source must be a worker guid and we rely on indices to map
    // workers to workpiles (one-to-one)
    // BUG #586: This is a non-portable assumption but will do for now.
    ocrWorker_t *worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // Source must be a worker guid
    ocrWorkpile_t * wpToPush = pushMappingOneToOne(base, worker->id);
    u32 i = 0;
    for ( ; i < *count; ++i ) {
        if (((ocrTask_t *)edts[i].metaDataPtr)->state == ALLACQ_EDTSTATE) {
            wpToPush->fcts.push(wpToPush, PUSH_WORKPUSHTYPE, edts[i]);
            edts[i].guid = NULL_GUID;
        }
    }
    *count = 0;
    return 0;
}

u8 hcSchedulerTakeComm(ocrScheduler_t *self, u32* count, ocrFatGuid_t* handlers, u32 properties) {
    return 0;
}

u8 hcSchedulerGiveComm(ocrScheduler_t *self, u32* count, ocrFatGuid_t* handlers, u32 properties) {
    return 0;
}

u8 hcSchedulerMonitorProgress(ocrScheduler_t *self, ocrMonitorProgress_t type, void * monitoree) {
#ifdef ENABLE_SCHEDULER_BLOCKING_SUPPORT
    // Current implementation assumes the worker is blocked.
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    handleWorkerNotProgressing(worker);
#endif
    return 0;
}

///////////////////////////////
//      Scheduler 1.0        //
///////////////////////////////

u8 hcSchedulerGetWorkInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER: {
            u32 count = 1;
            return self->fcts.takeEdt(self, &count, &taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
        }
    case OCR_SCHED_WORK_COMM: {
            return self->fcts.takeComm(self, &taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount, taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guids, 0);
        }
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 hcSchedulerNotifyInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_PRE_PROCESS_MSG: {
#ifndef PLACER_LEGACY //BUG #476 - This code is being deprecated
    // Try to automatically place datablocks and edts. Only support naive PD-based placement for now.
    ocrPolicyDomain_t * pd = self->pd;
    ocrPolicyMsg_t * msg = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG).msg;
    suggestLocationPlacement(pd, pd->myLocation, (ocrPlatformModelAffinity_t *) pd->platformModel,
                             (ocrLocationPlacer_t *) pd->placer, msg);
    return 0;
#else
    return OCR_ENOP;
#endif
    }
    case OCR_SCHED_NOTIFY_EDT_READY: {
            u32 count = 1;
            return self->fcts.giveEdt(self, &count, &notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid);
        }
    case OCR_SCHED_NOTIFY_EDT_DONE: {
            ocrTask_t * curTask __attribute__((unused)) = (ocrTask_t *) notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid.metaDataPtr;
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
            //TODO move to common scheduler ?
            if (curTask->state == RESCHED_EDTSTATE) {
                // We push the EDT at the back
                ocrWorker_t *worker = NULL;
                getCurrentEnv(NULL, &worker, NULL, NULL);
                ocrWorkerHc_t * hcWorker = (ocrWorkerHc_t *) worker;
                hcWorker->stealFirst = true;
                curTask->state = ALLACQ_EDTSTATE;
                // Source must be a worker guid
                ocrWorkpile_t * wpToPush = pushMappingOneToOne(self, worker->id);
                // TODO: in common scheduler this is going through scheduler-objects, don't know if they support that :/
                wpToPush->fcts.push(wpToPush, PUSH_WORKPUSHBACKTYPE, notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid);
                // Address deadlock cycle for mpilite
                // Additionally record the next time we'll do a steal instead of a pop
                // In theory another worker can do the steal and break the cycle without
                // the scheduler implementation to do anything. Doing a steal explicitly
                // ensure more fairness.
            } else {
#endif
                ASSERT(curTask->state == REAPING_EDTSTATE);
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
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
            }
#endif
        break;
        }
    case OCR_SCHED_NOTIFY_COMM_READY: {
            u32 count = 1;
            return self->fcts.giveComm(self, &count, &notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_COMM_READY).guid, 0);
        }
    // Notifies ignored by this scheduler
    case OCR_SCHED_NOTIFY_EDT_CREATE:
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
    case OCR_SCHED_NOTIFY_DB_CREATE:
    case OCR_SCHED_NOTIFY_DB_ACQUIRE:
    case OCR_SCHED_NOTIFY_DB_RELEASE:
    case OCR_SCHED_NOTIFY_DB_DESTROY:
        return OCR_ENOP;
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 hcSchedulerTransactInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    return OCR_ENOTSUP;
}

u8 hcSchedulerAnalyzeInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    return OCR_ENOTSUP;
}

u8 hcSchedulerUpdate(ocrScheduler_t *self, u32 properties) {
    return OCR_ENOTSUP;
}

ocrScheduler_t* newSchedulerHc(ocrSchedulerFactory_t * factory, ocrParamList_t *perInstance) {
    ocrScheduler_t* base = (ocrScheduler_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHc_t), PERSISTENT_CHUNK);
    factory->initialize(factory, base, perInstance);
    return base;
}

void initializeSchedulerHc(ocrSchedulerFactory_t * factory, ocrScheduler_t *self, ocrParamList_t *perInstance) {
    initializeSchedulerOcr(factory, self, perInstance);
    ocrSchedulerHc_t* derived = (ocrSchedulerHc_t*) self;
    paramListSchedulerHcInst_t *mapper = (paramListSchedulerHcInst_t*)perInstance;
    derived->workerIdFirst = mapper->workerIdFirst;
}

void destructSchedulerFactoryHc(ocrSchedulerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerFactory_t * newOcrSchedulerFactoryHc(ocrParamList_t *perType) {
    ocrSchedulerFactory_t* base = (ocrSchedulerFactory_t*) runtimeChunkAlloc(
        sizeof(ocrSchedulerFactoryHc_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newSchedulerHc;
    base->initialize  = &initializeSchedulerHc;
    base->destruct = &destructSchedulerFactoryHc;
    base->schedulerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                          phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcSchedulerSwitchRunlevel);
    base->schedulerFcts.destruct = FUNC_ADDR(void (*)(ocrScheduler_t*), hcSchedulerDestruct);
    base->schedulerFcts.takeEdt = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*), hcSchedulerTakeEdt);
    base->schedulerFcts.giveEdt = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*), hcSchedulerGiveEdt);
    base->schedulerFcts.takeComm = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*, u32), hcSchedulerTakeComm);
    base->schedulerFcts.giveComm = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*, u32), hcSchedulerGiveComm);
    base->schedulerFcts.monitorProgress = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrMonitorProgress_t, void*), hcSchedulerMonitorProgress);

    //Scheduler 1.0
    base->schedulerFcts.update = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32), hcSchedulerUpdate);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerGetWorkInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerNotifyInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerTransactInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerAnalyzeInvoke);
    return base;
}

#endif /* ENABLE_SCHEDULER_HC */
