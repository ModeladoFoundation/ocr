/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * - A scheduler heuristic for DOMAIN root schedulerObjects
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_PC

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "extensions/ocr-hints.h"
#include "scheduler-heuristic/pc/pc-scheduler-heuristic.h"
#include "scheduler-object/scheduler-object-all.h"

//Temporary until we get introspection support
#include "task/hc/hc-task.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

/******************************************************/
/* OCR-PC SCHEDULER_HEURISTIC                         */
/******************************************************/

ocrSchedulerHeuristic_t* newSchedulerHeuristicPc(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicPc_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    ocrSchedulerHeuristicPc_t *derived = (ocrSchedulerHeuristicPc_t*)self;
    derived->analyzeLocation = 0; //Centralized location for all analysis
    return self;
}

void initializeContext(ocrSchedulerHeuristicContext_t *context, u64 contextId) {
    context->id = contextId;
    context->actionSet = NULL;
    context->cost = NULL;
    context->properties = 0;

    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    pcContext->stealSchedulerObjectIndex = ((u64)-1);
    pcContext->mySchedulerObject = NULL;
    return;
}

u8 pcSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
    {
        // Memory is up at this point. We can initialize ourself
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
            u32 i;
            ocrScheduler_t *scheduler = self->scheduler;
            ASSERT(scheduler);
            ASSERT(SCHEDULER_OBJECT_KIND(self->scheduler->rootObj->kind) == OCR_SCHEDULER_OBJECT_DOMAIN);
            ASSERT(scheduler->pd != NULL);
            ASSERT(scheduler->contextCount > 0);
            ocrPolicyDomain_t *pd = scheduler->pd;
            ASSERT(pd == PD);

            self->contextCount = scheduler->contextCount;
            self->contexts = (ocrSchedulerHeuristicContext_t **)pd->fcts.pdMalloc(pd, self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*));
            ADebug(AllocDebugAllPD, "ce-scheduler-heuristic.c/pcSchedulerHeuristicSwitchRunlevel()/RL_GUID_OK[contexts] "
                   "pdMalloc(%ld), addr=%p\n",
                   (self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*)), self->contexts);
            ocrSchedulerHeuristicContextPc_t *contextAlloc = (ocrSchedulerHeuristicContextPc_t *)pd->fcts.pdMalloc(pd, self->contextCount * sizeof(ocrSchedulerHeuristicContextPc_t));
            ADebug(AllocDebugAllPD, "ce-scheduler-heuristic.c/pcSchedulerHeuristicSwitchRunlevel()/RL_GUID_OK[contextAlloc] "
                   "pdMalloc(%ld), addr=%p\n",
                   (self->contextCount * sizeof(ocrSchedulerHeuristicContextPc_t)), contextAlloc);

            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                initializeContext(context, i);
                self->contexts[i] = context;
            }
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_GUID_OK, phase)) {
            PD->fcts.pdFree(PD, self->contexts[0]);
            PD->fcts.pdFree(PD, self->contexts);
        }
        break;
    }
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void pcSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 pcSchedulerHeuristicUpdate(ocrSchedulerHeuristic_t *self, ocrSchedulerObject_t *schedulerObject, u32 properties) {
    return OCR_ENOTSUP;
}

ocrSchedulerHeuristicContext_t* pcSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    ASSERT(0);
    return NULL;
}

/* Setup the context for this contextId */
u8 pcSchedulerHeuristicRegisterContext(ocrSchedulerHeuristic_t *self, u64 contextId, ocrLocation_t loc) {
    ocrSchedulerHeuristicContext_t *context = self->contexts[contextId];
    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
    ocrSchedulerObjectFactory_t *rootFact = self->scheduler->pd->schedulerObjectFactories[rootObj->fctId];
    pcContext->mySchedulerObject = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, contextId, OCR_SCHEDULER_OBJECT_MAPPING_PINNED, SCHEDULER_OBJECT_MAPPING_WST | SCHEDULER_OBJECT_CREATE_IF_ABSENT);
    ASSERT(pcContext->mySchedulerObject);
    pcContext->stealSchedulerObjectIndex = (contextId + 1) % self->contextCount;
    pcContext->listIterator = NULL;
    ocrSchedulerObjectDomain_t *domainObj = (ocrSchedulerObjectDomain_t*)rootObj;
    ASSERT(domainObj->dbMap);
    ocrSchedulerObjectFactory_t *tablFact = self->scheduler->pd->schedulerObjectFactories[domainObj->dbMap->fctId];
    paramListSchedulerObject_t params;
    params.kind = OCR_SCHEDULER_OBJECT_ITERATOR;
    params.guidRequired = 0;
    pcContext->mapIterator = (ocrSchedulerObjectIterator_t*)tablFact->fcts.create(tablFact, (ocrParamList_t*)&params);
    RESULT_ASSERT(tablFact->fcts.iterate(tablFact, domainObj->dbMap, pcContext->mapIterator, SCHEDULER_OBJECT_ITERATE_BEGIN), ==, 0);
    return 0;
}

/* Find EDT for the worker to execute - This uses random workstealing to find work if no work is found owned deque */
static u8 pcSchedulerHeuristicWorkEdtUserInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrSchedulerObject_t edtObj;
    edtObj.guid.guid = NULL_GUID;
    edtObj.guid.metaDataPtr = NULL;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;

    //First try to pop from own deque
    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    ocrSchedulerObject_t *schedObj = pcContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    u8 retVal = fact->fcts.remove(fact, schedObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_DEQ_POP);

    //If pop fails, then try to steal from other deques
    if (edtObj.guid.guid == NULL_GUID) {
        //First try to steal from the last deque that was visited (probably had a successful steal)
        ocrSchedulerObject_t *stealSchedulerObject = ((ocrSchedulerHeuristicContextPc_t*)self->contexts[pcContext->stealSchedulerObjectIndex])->mySchedulerObject;
        if (stealSchedulerObject)
            retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_DEQ_STEAL); //try cached deque first

        //If cached steal failed, then restart steal loop from starting index
        ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
        ocrSchedulerObjectFactory_t *sFact = self->scheduler->pd->schedulerObjectFactories[rootObj->fctId];
        while (edtObj.guid.guid == NULL_GUID && sFact->fcts.count(sFact, rootObj, (SCHEDULER_OBJECT_COUNT_EDT | SCHEDULER_OBJECT_COUNT_RECURSIVE) ) != 0) {
            u32 i;
            for (i = 1; edtObj.guid.guid == NULL_GUID && i < self->contextCount; i++) {
                pcContext->stealSchedulerObjectIndex = (context->id + i) % self->contextCount;
                stealSchedulerObject = ((ocrSchedulerHeuristicContextPc_t*)self->contexts[pcContext->stealSchedulerObjectIndex])->mySchedulerObject;
                if (stealSchedulerObject)
                    retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_DEQ_STEAL);
            }
        }
    }

    if (!(ocrGuidIsNull(edtObj.guid.guid))) {
#ifdef OCR_MONITOR_SCHEDULER
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_WORKER, OCR_ACTION_WORK_TAKEN, edtObj.guid.guid, schedObj);
        ocrWorker_t *wrkr;
        getCurrentEnv(NULL, &wrkr, NULL, NULL);
        wrkr->isSeeking = false;
#endif
        if (edtObj.guid.metaDataPtr == NULL)
            fact->pd->guidProviders[0]->fcts.getVal(fact->pd->guidProviders[0], edtObj.guid.guid, (u64*)(&(edtObj.guid.metaDataPtr)), NULL);
        ASSERT(edtObj.guid.metaDataPtr);
        ocrTask_t *currentEdt = (ocrTask_t*)edtObj.guid.metaDataPtr;
        ocrHint_t edtHint;
        ocrHintInit(&edtHint, OCR_HINT_EDT_T);
        RESULT_ASSERT(ocrSetHintValue(&edtHint, OCR_HINT_EDT_PHASE, 0), ==, 0);
        (ocrTaskFactory_t*)(fact->pd->factories[pd->taskFactoryIdx])->fcts.setHint(currentEdt, &edtHint);
        taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt = edtObj.guid;
    }

    return retVal;
}

u8 pcSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        return pcSchedulerHeuristicWorkEdtUserInvoke(self, context, opArgs, hints);
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 pcSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 pcSchedulerHeuristicNotifyEdtReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    ocrSchedulerObject_t *schedObj = pcContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
#ifdef OCR_MONITOR_SCHEDULER
    ocrGuid_t taskGuid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid;
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_SCHEDULED, taskGuid, schedObj);
#endif
    return fact->fcts.insert(fact, schedObj, &edtObj, 0);
}

static u8 pcSchedulerHeuristicNotifyEdtSatisfiedInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicPc_t *derived = (ocrSchedulerHeuristicPc_t*)self;
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrTask_t *task = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.metaDataPtr;
    ASSERT(task);
    ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //TODO:This is temporary until we get proper introspection support

    //Analyze phase, location and affinity
    ocrPolicyDomain_t *pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);

    ocrSchedulerObject_t edtObj;
    edtObj.guid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    edtObj.fctId = task->fctId;
    edtObj.loc = pd->myLocation;
    edtObj.mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
    msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
    msg.destLocation = derived->analyzeLocation;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_PHASE;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.depc = task->depc;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.depv = hcTask->resolvedDeps;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.hint = &(hcTask->hint);
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).dstObj = NULL;
    PD_MSG_FIELD_IO(schedArgs).srcObj = &edtObj;
    RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 pcSchedulerHeuristicNotifyEdtDoneInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    // Destroy the work
    ocrPolicyDomain_t *pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
    msg.type = PD_MSG_WORK_DESTROY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid;
    PD_MSG_FIELD_I(currentEdt) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid;
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 pcSchedulerHeuristicNotifyDbCreateInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrDataBlock_t *db = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_DB_CREATE).guid.metaDataPtr;
    ASSERT(db);

    //Inherit current phase from currently executing EDT
    u64 currentPhase = 0;
    ocrTask_t *currentEdt = NULL;
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, &currentEdt, NULL);
    if (currentEdt) {
        ocrHint_t edtHint;
        ocrHintInit(&edtHint, OCR_HINT_EDT_T);
        (ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx])->fcts.getHint(currentEdt, &edtHint);
        RESULT_ASSERT(ocrGetHintValue(&edtHint, OCR_HINT_EDT_PHASE, &currentPhase), ==, 0);
    } else {
        //ASSERT(0); //TODO: We should assert if ocrDbCreate is called outside an EDT. We need identify user vs runtime calls.
    }

    //Create a DB scheduler object
    paramListSchedulerObjectDbNode_t dbParams;
    dbParams.base.kind = OCR_SCHEDULER_OBJECT_DBNODE;
    dbParams.base.guidRequired = 0;
    dbParams.initialPhase = currentPhase;
    dbParams.dbSize = db->size;
    dbParams.dataPtr = db->ptr;
    ocrSchedulerObjectFactory_t *dbFact = self->scheduler->pd->schedulerObjectFactories[schedulerObjectDbNode_id];
    ocrSchedulerObject_t *dbNode = dbFact->fcts.create(dbFact, (ocrParamList_t*)&dbParams);
    ASSERT(dbNode);

    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    ocrSchedulerObjectIterator_t *it = pcContext->mapIterator;
    ocrSchedulerObjectDomain_t *domainObj = (ocrSchedulerObjectDomain_t*)self->scheduler->rootObj;
    ocrSchedulerObjectFactory_t *tablFact = self->scheduler->pd->schedulerObjectFactories[domainObj->dbMap->fctId];
    it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).key = (void*)db->guid;
    it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value = (void*)dbNode;
    DPRINTF(DEBUG_LVL_VERB, "DB node. Key: "GUIDF" Value: %"PRIu64"\n", GUIDA(db->guid), dbNode);
    RESULT_ASSERT(tablFact->fcts.insert(tablFact, domainObj->dbMap, (ocrSchedulerObject_t*)it, SCHEDULER_OBJECT_INSERT_MAP_CONC_PUT), ==, 0);
    return 0;
}

u8 pcSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
        return pcSchedulerHeuristicNotifyEdtReadyInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
        return pcSchedulerHeuristicNotifyEdtSatisfiedInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_DONE:
        return pcSchedulerHeuristicNotifyEdtDoneInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_DB_CREATE:
        if (notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_DB_CREATE).dbType == USER_DBTYPE)
            return pcSchedulerHeuristicNotifyDbCreateInvoke(self, context, opArgs, hints);
        break;
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 pcSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pcSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pcSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 sendPhaseResponse(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpAnalyzeArgs_t *analyzeArgs, u64 phase, ocrLocation_t loc, ocrGuid_t affinity) {
    u32 i;
    //Reset depv.ptr which were repurposed earlier
    u32 depc = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.depc;
    ocrEdtDep_t *depv = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.depv;
    for (i = 0; i < depc; i++) depv[i].ptr = NULL;

    DPRINTF(DEBUG_LVL_VERB, "EDT Phase Response: Task: "GUIDF" Phase: %"PRIu64" Location: %"PRIu64" Affinity: %"PRIu64"\n", GUIDA(analyzeArgs->srcObj->guid.guid), phase, loc, affinity);
    //Respond with phase, location and affinity
    ocrPolicyDomain_t *pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
    msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
    msg.destLocation = analyzeArgs->srcObj->loc;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_PHASE;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).resp.scheduledPhase = phase;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).resp.scheduledLocation = loc;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).resp.affinity = affinity;
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_RESPONSE;
    PD_MSG_FIELD_IO(schedArgs).dstObj = analyzeArgs->srcObj;
    PD_MSG_FIELD_IO(schedArgs).srcObj = NULL;
    RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 pcSchedulerHeuristicAnalyzePhaseRequestInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    u32 i = 0;
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    u32 depc = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.depc;
    ocrEdtDep_t *depv = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).req.depv;

    //Get the DB nodes for each dep
    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    ocrSchedulerObjectIterator_t *it = pcContext->mapIterator;
    ocrSchedulerObjectDomain_t *domainObj = (ocrSchedulerObjectDomain_t*)self->scheduler->rootObj;
    ocrSchedulerObjectFactory_t *tablFact = self->scheduler->pd->schedulerObjectFactories[domainObj->dbMap->fctId];
    for (i = 0; i < depc; i++) {
        ASSERT(depv[i].ptr == NULL);
        if (!ocrGuidIsNull(depv[i].guid)) {
            it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).key = (void*)depv[i].guid;
            it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value = NULL;
            RESULT_ASSERT(tablFact->fcts.iterate(tablFact, domainObj->dbMap, it, SCHEDULER_OBJECT_ITERATE_MAP_GET_NON_CONC), ==, 0);
            ASSERT(it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value);
            depv[i].ptr = it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value; //Repurpose the data ptr to hold the DB node pointer
        }
    }

    //sortAndLockDbs(); //TODO

    u64 phase = 0;
    ocrLocation_t runningLoc = analyzeArgs->srcObj->loc;
    ocrGuid_t affinityGuid = NULL_GUID;

    //Check for existing common phase among all the deps
    for (i = 0; i < depc && depv[i].guid == NULL_GUID; i++);
    if (i < depc) {
        ocrSchedulerObjectDbNode_t *dbNode = (ocrSchedulerObjectDbNode_t*)depv[i].ptr;
        runningLoc = dbNode->currentLoc;
        u64 maxSize = dbNode->dbSize;
        ocrSchedulerObjectDbNode_t *maxDbNode = dbNode;
        for (; i < depc; i++) {
            if (!ocrGuidIsNull(depv[i].guid)) {
                ocrSchedulerObjectDbNode_t *dbNode = (ocrSchedulerObjectDbNode_t*)depv[i].ptr;
                if (runningLoc != dbNode->currentLoc)
                    break;
                if (dbNode->dbSize > maxSize) {
                    maxSize = dbNode->dbSize;
                    maxDbNode = dbNode;
                }
            }
        }
        phase = maxDbNode->currentPhase;
        affinityGuid = maxDbNode->base.guid.guid;
    }

    if (i == depc) {
        //Success. Current phase is common across all deps
        return sendPhaseResponse(self, context, analyzeArgs, phase, runningLoc, affinityGuid);
    }
    ASSERT(0); //Currently we only support shared-mem; So, the first check should suffice.
    return 0;
}

static u8 pcSchedulerHeuristicAnalyzePhaseResponseInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    u32 i;
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    u64 scheduledPhase = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).resp.scheduledPhase;
    ocrLocation_t scheduledLocation = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_PHASE).resp.scheduledLocation;
    ASSERT(scheduledPhase == 0 && scheduledLocation == self->scheduler->pd->myLocation); //TODO: Currently only shared mem support

    //Below, all code only deals with shared-mem support for RW mode only. Other modes and distributed support is TODO.
    ocrTask_t *task = analyzeArgs->dstObj->guid.metaDataPtr;
    ASSERT(task);
    ocrTaskHc_t *taskHc = (ocrTaskHc_t*)task; //TODO: Temporary until we get introspection support
    ocrEdtDep_t *depv = taskHc->resolvedDeps;
    u32 depc = task->depc;

    //Get the DB nodes for each dep
    ocrSchedulerHeuristicContextPc_t *pcContext = (ocrSchedulerHeuristicContextPc_t*)context;
    ocrSchedulerObjectIterator_t *it = pcContext->mapIterator;
    ocrSchedulerObjectDomain_t *domainObj = (ocrSchedulerObjectDomain_t*)self->scheduler->rootObj;
    ocrSchedulerObjectFactory_t *tablFact = self->scheduler->pd->schedulerObjectFactories[domainObj->dbMap->fctId];
    for (i = 0; i < depc; i++) {
        ASSERT(depv[i].ptr == NULL);
        if (!ocrGuidIsNull(depv[i].guid)) {
            it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).key = (void*)depv[i].guid;
            it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value = NULL;
            RESULT_ASSERT(tablFact->fcts.iterate(tablFact, domainObj->dbMap, it, SCHEDULER_OBJECT_ITERATE_MAP_GET_NON_CONC), ==, 0);
            ASSERT(it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value);
            ocrSchedulerObjectDbNode_t *dbNode = (ocrSchedulerObjectDbNode_t*)it->ITERATOR_ARG_FIELD(OCR_SCHEDULER_OBJECT_MAP).value; //Repurpose the data ptr to hold the DB node pointer
            depv[i].ptr = dbNode->dataPtr;
        }
    }

    //EDT is READY. Time to schedule for execution.
	DPRINTF(DEBUG_LVL_VERB, "EDT Ready: Task: "GUIDF"\n", GUIDA(task->guid));
    task->state = ALLACQ_EDTSTATE;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_NOTIFY
    msg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_EDT_READY;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid = task->guid;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.metaDataPtr = task;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
    ASSERT(PD_MSG_FIELD_O(returnDetail) == 0);
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 pcSchedulerHeuristicAnalyzePhaseInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    switch(analyzeArgs->properties) {
    case OCR_SCHED_ANALYZE_REQUEST:
        return pcSchedulerHeuristicAnalyzePhaseRequestInvoke(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_RESPONSE:
        return pcSchedulerHeuristicAnalyzePhaseResponseInvoke(self, context, opArgs, hints);
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 pcSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    switch(analyzeArgs->kind) {
    case OCR_SCHED_ANALYZE_PHASE:
        return pcSchedulerHeuristicAnalyzePhaseInvoke(self, context, opArgs, hints);
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 pcSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-PC SCHEDULER_HEURISTIC FACTORY                 */
/******************************************************/

void destructSchedulerHeuristicFactoryPc(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryPc(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryPc_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicPc;
    base->destruct = &destructSchedulerHeuristicFactoryPc;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), pcSchedulerHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), pcSchedulerHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), pcSchedulerHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), pcSchedulerHeuristicGetContext);
    base->fcts.registerContext = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u64, ocrLocation_t), pcSchedulerHeuristicRegisterContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), pcSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_PC */
