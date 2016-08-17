/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * - A scheduler heuristic for WST root schedulerObjects
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_STATIC

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "scheduler-heuristic/static/static-scheduler-heuristic.h"
#include "extensions/ocr-hints.h"
#include "extensions/ocr-affinity.h"
#include "worker/hc/hc-worker.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

/******************************************************/
/* OCR-STATIC SCHEDULER_HEURISTIC                         */
/******************************************************/

ocrSchedulerHeuristic_t* newSchedulerHeuristicStatic(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicStatic_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    ocrSchedulerHeuristicStatic_t *dself = (ocrSchedulerHeuristicStatic_t*)self;
    dself->rrCounter = 0;
    dself->isDistributed = false;
    dself->isTraceActive = false;
    return self;
}

static void initializeContextStatic(ocrSchedulerHeuristicContext_t *context, u64 contextId) {
    context->id = contextId;
    context->actionSet = NULL;
    context->cost = NULL;
    context->properties = 0;

    ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)context;
    staticContext->mySchedulerObject = NULL;
    staticContext->commSchedulerObject = NULL;
    return;
}

u8 staticSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
            ocrSchedulerHeuristicContextStatic_t *contextAlloc = (ocrSchedulerHeuristicContextStatic_t *)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContextStatic_t));
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                initializeContextStatic(context, i);
                self->contexts[i] = context;
                context->id = i;
                context->location = PD->myLocation;
                context->actionSet = NULL;
                context->cost = NULL;
                context->properties = 0;
                ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)context;
                staticContext->mySchedulerObject = NULL;
                staticContext->commSchedulerObject = NULL;
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
                ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)self->contexts[i];
                staticContext->mySchedulerObject = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_DEQUE, i, OCR_SCHEDULER_OBJECT_MAPPING_WORKER, 0);
                ASSERT(staticContext->mySchedulerObject);
            }
#ifdef ENABLE_WORKER_HC
#ifdef ENABLE_WORKER_HC_COMM
            ocrWorkerHc_t *w0 = (ocrWorkerHc_t*)PD->workers[0];
            if (w0->hcType == HC_WORKER_COMM) {
                //NOTE: For distributed, we are making the assumption that only worker 0 is comm worker.
                ASSERT(self->contextCount > 1);
                ocrSchedulerHeuristicStatic_t *dself = (ocrSchedulerHeuristicStatic_t*)self;
                dself->isDistributed = true;
                ocrSchedulerHeuristicContextStatic_t *commContext = (ocrSchedulerHeuristicContextStatic_t*)self->contexts[0];
                for (i = 1; i < self->contextCount; i++) {
                    ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)self->contexts[i];
                    staticContext->commSchedulerObject = commContext->mySchedulerObject;
                }
            }
#endif
#else
#error
#endif
            //NOTE: By convenvtion, worker[numWorkers-1] is the system worker.
            ocrWorkerHc_t *wMax = (ocrWorkerHc_t *)PD->workers[PD->workerCount-1];
            if(wMax->hcType == HC_WORKER_SYSTEM){
                ASSERT(self->contextCount > 1);
                ocrSchedulerHeuristicStatic_t *dself = (ocrSchedulerHeuristicStatic_t*)self;
                dself->isTraceActive = true;

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

void staticSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 staticSchedulerHeuristicUpdate(ocrSchedulerHeuristic_t *self, u32 properties) {
    return OCR_ENOTSUP;
}

ocrSchedulerHeuristicContext_t* staticSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    if (worker == NULL) return NULL;
    return self->contexts[worker->id];
}

/* Find EDT for the worker to execute - This uses random workstealing to find work if no work is found owned deque */
static u8 staticSchedulerHeuristicWorkEdtUserInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    u8 retVal = 0;
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrSchedulerObject_t edtObj;
    edtObj.guid.guid = NULL_GUID;
    edtObj.guid.metaDataPtr = NULL;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;

    //Try to pop from own deque until there is no work
    ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)context;
    ocrSchedulerObject_t *schedObj = staticContext->mySchedulerObject;  //The deque owned by this worker
    ocrSchedulerObject_t *commObj = staticContext->commSchedulerObject; //The comm worker deque for distributed scheduling
    ASSERT(schedObj);

    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
    ocrSchedulerObjectFactory_t *rootFact = self->scheduler->pd->schedulerObjectFactories[rootObj->fctId];
    while (ocrGuidIsNull(edtObj.guid.guid) && rootFact->fcts.count(rootFact, rootObj, (SCHEDULER_OBJECT_COUNT_EDT | SCHEDULER_OBJECT_COUNT_RECURSIVE)) != 0) {
        retVal = fact->fcts.remove(fact, schedObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
        if (commObj && ocrGuidIsNull(edtObj.guid.guid)) {
            //For distributed scheduling, we need to pick up comm tasks from the comm worker
            ocrSchedulerObjectFactory_t *commFact = self->scheduler->pd->schedulerObjectFactories[commObj->fctId];
            retVal = commFact->fcts.remove(commFact, commObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
        }
    }

    if (!ocrGuidIsNull(edtObj.guid.guid)) {
        ASSERT(retVal == 0);
        taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt = edtObj.guid;
        //Add same steal determining flag
#ifdef OCR_MONITOR_SCHEDULER
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_WORKER, OCR_ACTION_WORK_TAKEN, edtObj.guid.guid, schedObj);
        ocrWorker_t *wrkr;
        getCurrentEnv(NULL, &wrkr, NULL, NULL);
        wrkr->isSeeking = false;
#endif

    }

    return retVal;
}

u8 staticSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        return staticSchedulerHeuristicWorkEdtUserInvoke(self, context, opArgs, hints);
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 staticSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 staticSchedulerHeuristicNotifyEdtReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrSchedulerObject_t *schedObj = NULL;

    ocrTask_t *task = (ocrTask_t*)notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.metaDataPtr;
    ASSERT(task);
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    u64 contextId = (u64)(-1);
    ocrHint_t edtHint;
    ocrHintVal_t hintVal = {};
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.getHint(task, &edtHint), ==, 0);
    if (ocrHintGetValue(&edtHint, OCR_HINT_EDT_SPACE, &hintVal) == 0) {
        contextId = hintVal.u64Val;
        ASSERT(contextId < self->contextCount);
        ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)self->contexts[contextId];
        schedObj = staticContext->mySchedulerObject;
    } else {
        ocrSchedulerHeuristicContextStatic_t *staticContext = (ocrSchedulerHeuristicContextStatic_t*)context;
        schedObj = staticContext->mySchedulerObject;
    }
    ASSERT(schedObj);

    ocrSchedulerObject_t edtObj;
    edtObj.guid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
#ifdef OCR_MONITOR_SCHEDULER
    ocrGuid_t taskGuid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid;
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_SCHEDULED, taskGuid, schedObj);
#endif
    return fact->fcts.insert(fact, schedObj, &edtObj, NULL, (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL));
}

static u8 staticSchedulerHeuristicNotifyPreProcessMsgInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    ocrWorker_t *worker;
    getCurrentEnv(&pd, &worker, NULL, NULL);
    ocrSchedulerHeuristicStatic_t *dself = (ocrSchedulerHeuristicStatic_t*)self;
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrPolicyMsg_t * msg = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG).msg;
    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_WORK_CREATE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
            //During work create in the local PD, this function will identify where the EDT
            //will ultimately execute when ready in terms of the destination worker and PD.
            //If a "disperse" hint is provided by the user, this function will schedule the EDT
            //in round-robin fashion across all workers in the system.
            //Note: This code assumes worker 0 in distributed OCR to be a communication worker
            //Once the destination worker is calculated it is set as a runtime hint field,
            //while the destination PD is set as the msg destination.
            //If no "disperse" hint is given, the EDT will be scheduled to execute in the
            //worker that created it.
            if (PD_MSG_FIELD_I(workType) == EDT_USER_WORKTYPE) {
                ASSERT((msg->type & PD_MSG_REQUEST) && (msg->srcLocation == pd->myLocation) && (msg->destLocation == pd->myLocation));
                u64 workerId = ((u64)-1);
                ocrHint_t *edtHint = NULL;
                ocrHintVal_t hintVal = {};
                if (PD_MSG_FIELD_I(hint) != NULL_HINT) {
                    ASSERT(PD_MSG_FIELD_I(hint->type) == OCR_HINT_EDT_T);
                    edtHint = PD_MSG_FIELD_I(hint);

                    //Read the affinity hint if any
                    u64 userAffinity = ((u64)-1);
                    bool usesAffinity = (ocrHintGetValue(edtHint, OCR_HINT_EDT_AFFINITY, &hintVal) == 0);
                    userAffinity = hintVal.u64Val;
                    if (usesAffinity && dself->isDistributed) {
                        ocrGuid_t affGuid = NULL_GUID;
#if GUID_BIT_COUNT == 64
                        affGuid.guid = userAffinity;
#elif GUID_BIT_COUNT == 128
                        affGuid.upper = 0ULL;
                        affGuid.lower = userAffinity;
#endif
                        msg->destLocation = affinityToLocation(affGuid);
                    }

                    //Check if the disperse hint is set
                    //Set the msg destination and worker hint accordingly
                    u64 val = 0;
                    if (ocrHintGetValue(edtHint, OCR_HINT_EDT_DISPERSE, &hintVal) == 0) {
                        val = hintVal.u64Val;
                        u64 contextId = hal_xadd64(&dself->rrCounter, 1);
                        if (dself->isDistributed) {
                            if (!usesAffinity && val != OCR_HINT_EDT_DISPERSE_NEAR) {
                                //setup destination PD
                                ocrPlatformModelAffinity_t * platformModel = ((ocrPlatformModelAffinity_t*)pd->platformModel);
                                ocrLocation_t dstLoc = (contextId / (self->contextCount - 1)) % platformModel->pdLocAffinitiesSize; //Note: Assume worker 0 is comm worker
                                ASSERT(dstLoc < platformModel->pdLocAffinitiesSize);
                                ocrGuid_t affGuid = platformModel->pdLocAffinities[dstLoc];
                                hintVal.u64Val = ocrAffinityToHintValue(affGuid);
                                RESULT_ASSERT(ocrHintSetValue(edtHint, OCR_HINT_EDT_AFFINITY, hintVal), ==, 0);
                                msg->destLocation = dstLoc;
                            }

                            //If tracing is active along with distributed
                            if(dself->isTraceActive)
                                workerId = (contextId % (self->contextCount - 2)) + 1; //Note: Assume worker 0 is comm worker and worker <numWorkers-1> is system worker
                            else
                                workerId = (contextId % (self->contextCount - 1)) + 1; //Note: Assume worker 0 is comm worker
                        } else {

                            //If tracing is active without distributed
                            if(dself->isTraceActive)
                                workerId = contextId % (self->contextCount-1);
                            else
                                workerId = contextId % self->contextCount;
                        }
                    } else {
                        workerId = worker->id;
                    }
                } else {
                    workerId = worker->id;
                    edtHint = pd->fcts.pdMalloc(pd, sizeof(ocrHint_t));
                    ocrHintInit(edtHint, OCR_HINT_EDT_T);
                    PD_MSG_FIELD_I(hint) = edtHint;
                    PD_MSG_FIELD_I(properties) |= EDT_PROP_RT_HINT_ALLOC;
                }
                ASSERT(edtHint);
                hintVal.u64Val = workerId;
                RESULT_ASSERT(ocrHintSetValue(edtHint, OCR_HINT_EDT_SPACE, hintVal), ==, 0);
                DPRINTF(DEBUG_LVL_VERB, "WORK_CREATE: msg: %p msgId: %"PRIx64" Affinity (PD: %"PRIx64" Worker: %"PRIx64")\n", msg, msg->msgId, msg->destLocation, workerId);
            }
#undef PD_MSG
#undef PD_TYPE
        break;
        }
    case PD_MSG_DB_CREATE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
            // When we do place DBs make sure we only place USER DBs
            if (PD_MSG_FIELD_I(dbType) == USER_DBTYPE && PD_MSG_FIELD_I(hint) != NULL_HINT && dself->isDistributed) {
                ASSERT(PD_MSG_FIELD_I(hint->type) == OCR_HINT_DB_T);
                ocrHint_t *dbHint = PD_MSG_FIELD_I(hint);
                ocrHintVal_t hintVal = {};
                //Read the affinity hint if any
                u64 userAffinity = (u64)(-1);
                if (ocrHintGetValue(dbHint, OCR_HINT_DB_AFFINITY, &hintVal) == 0) {
                    userAffinity = hintVal.u64Val;
                    ocrGuid_t affGuid = NULL_GUID;
#if GUID_BIT_COUNT == 64
                    affGuid.guid = userAffinity;
#elif GUID_BIT_COUNT == 128
                    affGuid.upper = 0ULL;
                    affGuid.lower = userAffinity;
#endif
                    msg->destLocation = affinityToLocation(affGuid);
                }
            }
#undef PD_MSG
#undef PD_TYPE
        break;
        }
    default:
            // Fall-through
        break;
    }
    return 0;
}

static u8 staticSchedulerHeuristicNotifyPostProcessMsgInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    return OCR_ENOTSUP;
}

u8 staticSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_PRE_PROCESS_MSG:
        return staticSchedulerHeuristicNotifyPreProcessMsgInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_POST_PROCESS_MSG:
        return staticSchedulerHeuristicNotifyPostProcessMsgInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_READY:
        return staticSchedulerHeuristicNotifyEdtReadyInvoke(self, context, opArgs, hints);
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
    case OCR_SCHED_NOTIFY_DB_CREATE:
        return OCR_ENOP;
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 staticSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 staticSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 staticSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 staticSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 staticSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-STATIC SCHEDULER_HEURISTIC FACTORY                 */
/******************************************************/

void destructSchedulerHeuristicFactoryStatic(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryStatic(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryStatic_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicStatic;
    base->destruct = &destructSchedulerHeuristicFactoryStatic;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), staticSchedulerHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), staticSchedulerHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), staticSchedulerHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), staticSchedulerHeuristicGetContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), staticSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_STATIC */
