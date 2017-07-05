/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * - A scheduler heuristic for WST root schedulerObjects
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_HC

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "scheduler-heuristic/hc/hc-scheduler-heuristic.h"
#include "extensions/ocr-hints.h"
#include "scheduler-object/wst/wst-scheduler-object.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

#ifdef LOAD_BALANCING_TEST
#include "extensions/ocr-hints.h"
#endif

#ifdef ENABLE_RESILIENCY
#include "policy-domain/hc/hc-policy.h"
#endif
/******************************************************/
/* OCR-HC SCHEDULER_HEURISTIC                         */
/******************************************************/

ocrSchedulerHeuristic_t* newSchedulerHeuristicHc(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicHc_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    return self;
}

static void initializeContextHc(ocrSchedulerHeuristicContext_t *context, u64 contextId) {
    context->id = contextId;
    context->actionSet = NULL;
    context->cost = NULL;
    context->properties = 0;

    ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)context;
    hcContext->stealSchedulerObjectIndex = ((u64)-1);
    hcContext->mySchedulerObject = NULL;
    return;
}

u8 hcSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                      phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
    {
        ASSERT(self->scheduler);
        self->contextCount = PD->workerCount; //Shared mem heuristic
        ASSERT(self->contextCount > 0);
        break;
    }
    case RL_MEMORY_OK:
    {
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            u32 i;
            self->contexts = (ocrSchedulerHeuristicContext_t **)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*));
            ocrSchedulerHeuristicContextHc_t *contextAlloc = (ocrSchedulerHeuristicContextHc_t *)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContextHc_t));
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                initializeContextHc(context, i);
                self->contexts[i] = context;
                context->id = i;
                context->location = PD->myLocation;
                context->actionSet = NULL;
                context->cost = NULL;
                context->properties = 0;
                ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)context;
                hcContext->stealSchedulerObjectIndex = ((u64)-1);
                hcContext->mySchedulerObject = NULL;
            }
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
            PD->fcts.pdFree(PD, self->contexts[0]);
            PD->fcts.pdFree(PD, self->contexts);
        }
        break;
    }
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
    {
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            u32 i;
            ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
            ocrSchedulerObjectFactory_t *rootFact = PD->schedulerObjectFactories[rootObj->fctId];
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)self->contexts[i];
                hcContext->mySchedulerObject = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_DEQUE, i, OCR_SCHEDULER_OBJECT_MAPPING_WORKER, 0);
                ASSERT(hcContext->mySchedulerObject);
                hcContext->stealSchedulerObjectIndex = (i + 1) % self->contextCount;
            }
        }
        break;
    }
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void hcSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 hcSchedulerHeuristicUpdate(ocrSchedulerHeuristic_t *self, u32 properties) {
    return OCR_ENOTSUP;
}

ocrSchedulerHeuristicContext_t* hcSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    if (worker == NULL) return NULL;
    return self->contexts[worker->id];
}

/* Find EDT for the worker to execute - This uses random workstealing to find work if no work is found owned deque */
static u8 hcSchedulerHeuristicGetEdt(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context,
                                     ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints, ocrSchedulerObjectKind kind,
                                     u32 countProp)
{
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrSchedulerObject_t edtObj;
    ocrSchedulerObject_t* rootObj = self->scheduler->rootObj;
    ocrSchedulerObject_t* stealSchedulerObject;
    ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)context;
    ocrSchedulerObject_t *schedObj = hcContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObjectFactory_t* fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    edtObj.guid.guid = NULL_GUID;
    edtObj.guid.metaDataPtr = NULL;
    edtObj.kind = kind;
    u8 retVal;
#ifdef OCR_ENABLE_SCHEDULER_SPAWN_QUEUE
    // first look in spawn_queue
    DPRINTF(DEBUG_LVL_INFO, ">>> Look in spawn_queue\n");
    //TODO: you should check here to see if request is from another block/PD!!
    ocrSchedulerObjectWst_t* wstSchedObj = (ocrSchedulerObjectWst_t*)rootObj;
    stealSchedulerObject = wstSchedObj->spawn_queue;
    retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
    if (!ocrGuidIsNull(edtObj.guid.guid) ) {
       DPRINTF(DEBUG_LVL_INFO, ">>> Found guid in spawn_queue\n");
    }
#endif
    //First try to pop from own deque
    if (ocrGuidIsNull(edtObj.guid.guid)) {
       retVal = fact->fcts.remove(fact, schedObj, kind , 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_TAIL);
    }

    //If pop fails, then try to steal from other deques
    if (ocrGuidIsNull(edtObj.guid.guid)) {

        //First try to steal from the last deque that was visited (probably had a successful steal)
        stealSchedulerObject = ((ocrSchedulerHeuristicContextHc_t*)self->contexts[hcContext->stealSchedulerObjectIndex])->mySchedulerObject;
        ASSERT(stealSchedulerObject);
        retVal = fact->fcts.remove(fact, stealSchedulerObject, kind, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD); //try cached deque first

        //If cached steal failed, then restart steal loop from starting index
        //*rootObj = self->scheduler->rootObj;
        ocrSchedulerObjectFactory_t *sFact = self->scheduler->pd->schedulerObjectFactories[rootObj->fctId];
        while (ocrGuidIsNull(edtObj.guid.guid) && sFact->fcts.count(sFact, rootObj, countProp) != 0) {
            u32 i;
            for (i = 1; ocrGuidIsNull(edtObj.guid.guid) && i < self->contextCount; i++) {
                hcContext->stealSchedulerObjectIndex = (context->id + i) % self->contextCount; //simple round robin stealing
                stealSchedulerObject = ((ocrSchedulerHeuristicContextHc_t*)self->contexts[hcContext->stealSchedulerObjectIndex])->mySchedulerObject;
                if (stealSchedulerObject){
                    retVal = fact->fcts.remove(fact, stealSchedulerObject, kind, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                }
            }
        }
    }

    if (!(ocrGuidIsNull(edtObj.guid.guid))){
        DPRINTF(DEBUG_LVL_INFO, ">>> Found GUID "GUIDF" from somewhere\n",GUIDA(edtObj.guid.guid));
        taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt = edtObj.guid;
#ifdef OCR_MONITOR_SCHEDULER
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_WORKER, OCR_ACTION_WORK_TAKEN, edtObj.guid.guid, schedObj);
        ocrWorker_t *wrkr;
        getCurrentEnv(NULL, &wrkr, NULL, NULL);
        wrkr->isSeeking = false;
#endif
    }

    return retVal;
}

#ifdef LOAD_BALANCING_TEST
// Will go away with MT
extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static u8 hcSchedulerHeuristicNotifyEdtSatisfiedInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)context;
    ASSERT(hcContext->mySchedulerObject);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrPolicyDomain_t * pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrLocation_t edtLoc;
    pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], edtObj.guid.guid, &edtLoc);
    if (edtLoc != pd->myLocation) {
        DPRINTF(DEBUG_LVL_VVERB, "[LB] Scheduler: Received foreign EDT for execution\n");
    } else {
        ocrTask_t * edt = (ocrTask_t *) edtObj.guid.metaDataPtr;
        ASSERT(edt != NULL);
        // These checks short-circuit a little bit the call path so that the placement
        // heuristic is not called to end up doing the same kind of checks
        ocrHint_t edtHints;
        ocrHintInit(&edtHints, OCR_HINT_EDT_T);
        u8 noHint = ((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.getHint(edt, &edtHints);
        u64 edtAff;
        u8 noPlcHint = noHint || (!noHint && ocrGetHintValue(&edtHints, OCR_HINT_EDT_AFFINITY, &edtAff));
        bool loadBalance = ((edt->funcPtr != &processRequestEdt) && noPlcHint);
        if (loadBalance) {
            DPRINTF(DEBUG_LVL_VVERB, "[LB] Scheduler node-level load balancing "GUIDF"\n", GUIDA(edtObj.guid.guid));
            return OCR_ENOSPC;
        } // else fall-through and continue with scheduling
    }
    return 0;
}
#endif

u8 hcSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        {
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
            ASSERT(ocrGuidIsNull(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid));
            u8 retVal = hcSchedulerHeuristicGetEdt(self, context, opArgs, hints, OCR_SCHEDULER_OBJECT_RUNTIME_EDT, SCHEDULER_OBJECT_COUNT_RUNTIME_EDT);
            if (!(ocrGuidIsNull(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid)))
                return retVal;
#ifdef ENABLE_RESILIENCY
            ocrPolicyDomain_t * pd;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            ocrPolicyDomainHc_t *hcPolicy = (ocrPolicyDomainHc_t*)pd;
            if (hcPolicy->stateOfCheckpoint != 0 || hcPolicy->stateOfRestart != 0) {
                return 0; //When checkpoint is in progress, we will not pick up anymore user edts
            }
#endif
#endif
            return hcSchedulerHeuristicGetEdt(self, context, opArgs, hints, OCR_SCHEDULER_OBJECT_EDT, SCHEDULER_OBJECT_COUNT_EDT);
        }
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 hcSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 hcSchedulerHeuristicNotifyEdtReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)context;
    ocrSchedulerObject_t *schedObj = hcContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    ocrTask_t *task = (ocrTask_t*)notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.metaDataPtr;
    if ((task->flags & OCR_TASK_FLAG_RUNTIME_EDT) != 0) {
        edtObj.kind = OCR_SCHEDULER_OBJECT_RUNTIME_EDT;
    } else {
        ASSERT(task->state == ALLACQ_EDTSTATE);
    }
#endif
#ifdef OCR_MONITOR_SCHEDULER
    ocrGuid_t taskGuid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid;
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_SCHEDULED, taskGuid, schedObj);
#endif
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
#ifdef OCR_ENABLE_SCHEDULER_SPAWN_QUEUE
    DPRINTF(DEBUG_LVL_INFO, ">>> should we put guid: "GUIDF" in the spawn_queue?\n",GUIDA(edtObj.guid.guid));
    ocrHint_t spawnHint;
    ocrHintInit(&spawnHint, OCR_HINT_EDT_T);
    u64 hintVal = 0ULL;
    RESULT_ASSERT(ocrGetHint(edtObj.guid.guid, &spawnHint), ==, 0);
    if (ocrGetHintValue(&spawnHint, OCR_HINT_EDT_SPAWNING, &hintVal) == 0) {
        DPRINTF(DEBUG_LVL_INFO, ">>> put it into spawn_queue guid: "GUIDF"\n",GUIDA(edtObj.guid.guid));
        ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
        ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)rootObj;
        schedObj = wstSchedObj->spawn_queue;
    }
#endif
    return fact->fcts.insert(fact, schedObj, &edtObj, NULL, (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL));
}

u8 hcSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
        return hcSchedulerHeuristicNotifyEdtReadyInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_DONE:
        {
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
        }
        break;
    // Notifies ignored by this heuristic
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
#ifdef LOAD_BALANCING_TEST
        return hcSchedulerHeuristicNotifyEdtSatisfiedInvoke(self, context, opArgs, hints);
#else
        return OCR_ENOP;
#endif
    // Unknown ops
    default:
        return OCR_ENOTSUP;
    }
    return 0;
}

#if 0 //Example simulate op
u8 hcSchedulerHeuristicGiveSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(opArgs->schedulerObject->kind == OCR_SCHEDULER_OBJECT_EDT);
    ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t*)self->contexts[opArgs->contextId];
    ocrSchedulerHeuristicContextHc_t *hcContext = (ocrSchedulerHeuristicContextHc_t*)context;
    ASSERT(context->actionSet == NULL);

    ocrSchedulerObjectActionSet_t *actionSet = &(hcContext->singleActionSet);
    actionSet->actions = &(hcContext->insertAction);
    ASSERT(actionSet->actions->schedulerObject == hcContext->mySchedulerObject);
    actionSet->actions->args.insert.el = opArgs->schedulerObject;
    context->actionSet = actionSet;
    ASSERT(0);
    return OCR_ENOTSUP;
}
#endif

u8 hcSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 hcSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 hcSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 hcSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 hcSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-HC SCHEDULER_HEURISTIC FACTORY                 */
/******************************************************/

void destructSchedulerHeuristicFactoryHc(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryHc(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryHc_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicHc;
    base->destruct = &destructSchedulerHeuristicFactoryHc;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcSchedulerHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), hcSchedulerHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), hcSchedulerHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), hcSchedulerHeuristicGetContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_HC */
