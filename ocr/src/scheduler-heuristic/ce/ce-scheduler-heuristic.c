/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * - A scheduler heuristic for CE's on a TG machine
 *
 *   This heuristic manages work for all XEs in the block.
 *   When out of work, work requests are sent out to neighboring CEs.
 *   This heuristic keeps a work request pending when it cannot respond.
 *   Contexts are maintained for every neighbor XE and CE.
 *   Each context maintains its own work queue.
 *   If any EDT is mapped to a specific location through hints,
 *   the EDT will be placed on the deque owned by that context (if it exists)
 *   or a parent context of that location.
 *   When a work request comes from a src location, that location's
 *   context is first chosen to respond with work.
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_CE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "extensions/ocr-hints.h"
#include "scheduler-heuristic/ce/ce-scheduler-heuristic.h"
#include "scheduler-object/wst/wst-scheduler-object.h"
#include "policy-domain/ce/ce-policy.h"

#include "mmio-table.h"
#include "xstg-map.h"

//Temporary until we get introspection support
#include "task/hc/hc-task.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

/******************************************************/
/* OCR-CE SCHEDULER_HEURISTIC                         */
/******************************************************/

ocrSchedulerHeuristic_t* newSchedulerHeuristicCe(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicCe_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    derived->workCount = 0;
    derived->inPendingCount = 0;
    derived->pendingXeCount = 0;
    derived->outWorkVictimsAvailable = 0;
    derived->shutdownMode = false;
    return self;
}

u8 ceSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                      phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ocrAssert(callback == NULL);

    // Verify properties for this call
    ocrAssert((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ocrAssert(!(properties & RL_FROM_MSG));

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
    {
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_MEMORY_OK\n");
        ocrAssert(self->scheduler);
        self->contextCount = ((ocrPolicyDomainCe_t*)PD)->xeCount + PD->neighborCount;
        ocrAssert(self->contextCount > 0);
        DPRINTF(DEBUG_LVL_INFO, "ContextCount: %"PRIu64" (XE: %"PRIu32" + Neighbors: %"PRIu32")\n", self->contextCount, ((ocrPolicyDomainCe_t*)PD)->xeCount, PD->neighborCount);
        break;
    }
    case RL_GUID_OK:
    {
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_GUID_OK\n");
        // Memory is up at this point. We can initialize ourself
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            u32 i;
            u32 myCluster = CLUSTER_FROM_ID(PD->myLocation);
            u32 myBlock = BLOCK_FROM_ID(PD->myLocation);
            u32 xeCount = ((ocrPolicyDomainCe_t*)PD)->xeCount;
            self->contexts = (ocrSchedulerHeuristicContext_t **)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*));
            ocrSchedulerHeuristicContextCe_t *contextAlloc = (ocrSchedulerHeuristicContextCe_t *)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContextCe_t));
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                self->contexts[i] = context;
                context->id = i;
                context->actionSet = NULL;
                context->cost = NULL;
                context->properties = 0;
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                ceContext->msgId = 0;
                ceContext->stealSchedulerObjectIndex = ((u32)-1);
                ceContext->mySchedulerObject = NULL;
                ceContext->inWorkRequestPending = false;
                ceContext->outWorkRequestPending = false;
                if (i < xeCount) {
                    context->location = MAKE_CORE_ID(0, 0, 0, myCluster, myBlock, (ID_AGENT_XE0 + i));
                    ceContext->canAcceptWorkRequest = false;
                    ceContext->isChild = true;
                } else {
                    context->location = PD->neighbors[i - xeCount];
                    ceContext->canAcceptWorkRequest = true;
                    ceContext->isChild = false;
                }
                if (ceContext->isChild) {
                    DPRINTF(DEBUG_LVL_VVERB, "Created context %"PRId32" for location: %"PRIx64" (CHILD)\n", i, context->location);
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "Created context %"PRId32" for location: %"PRIx64"\n", i, context->location);
                }
            }
            ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
            derived->outWorkVictimsAvailable = PD->neighborCount;
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
            PD->fcts.pdFree(PD, self->contexts[0]);
            PD->fcts.pdFree(PD, self->contexts);
        }
        break;
    }
    case RL_COMPUTE_OK:
    {
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_COMPUTE_OK\n");
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            u32 i;
            ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
            ocrSchedulerObjectFactory_t *rootFact = PD->schedulerObjectFactories[rootObj->fctId];
            DPRINTF(DEBUG_LVL_VVERB, "Root scheduler object %p (fact: %"PRId32")\n", rootObj, rootObj->fctId);
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t*)self->contexts[i];
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                //BUG #920 - Revisit getSchedulerObjectForLocation API
                ceContext->mySchedulerObject = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_DEQUE,
                                                    context->location, OCR_SCHEDULER_OBJECT_MAPPING_MAPPED, 0);
                ocrAssert(ceContext->mySchedulerObject && ceContext->mySchedulerObject != rootObj);
                ceContext->stealSchedulerObjectIndex = (i + 1) % self->contextCount;
                DPRINTF(DEBUG_LVL_VVERB, "Scheduler object %p (fact: %"PRId32") for location: %"PRIx64"\n", ceContext->mySchedulerObject, ceContext->mySchedulerObject->fctId, context->location);
            }
        }
        break;
    }
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ocrAssert(0);
    }
    return toReturn;
}

void ceSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

ocrSchedulerHeuristicContext_t* ceSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    u32 i;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    if (pd->myLocation == loc)
        return self->contexts[0];
    u32 xeCount = ((ocrPolicyDomainCe_t*)pd)->xeCount;
    u64 agentId = AGENT_FROM_ID(loc);
    if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7))
        return self->contexts[agentId - ID_AGENT_XE0];
    for (i = 0; i < pd->neighborCount; i++) {
        if (pd->neighbors[i] == loc)
            return self->contexts[i + xeCount];
    }
    return NULL;
}

/* Find EDT for the worker to execute - This uses random workstealing to find work if no work is found owned deque */
static u8 ceWorkStealingGet(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t *fguid) {
    ocrAssert(ocrGuidIsNull(fguid->guid));
    ocrAssert(fguid->metaDataPtr == NULL);

    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
    if (derived->workCount == 0) {
#if 0 //Enable to make scheduler chatty
        return 0;
#else
        return 1;
#endif
    }

    ocrSchedulerObject_t edtObj;
    edtObj.guid.guid = NULL_GUID;
    edtObj.guid.metaDataPtr = NULL;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;

    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrSchedulerObject_t *schedObj = ceContext->mySchedulerObject;
    ocrAssert(schedObj);
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    u8 retVal = 1;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)rootObj;
    ocrSchedulerObject_t *stealSchedulerObject;
#ifdef OCR_ENABLE_SCHEDULER_SPAWN_QUEUE
    //first if it's a req from a CE look in the spawning queue:
    if(AGENT_FROM_ID(context->location) == ID_AGENT_CE){
       DPRINTF(DEBUG_LVL_INFO, "Request was from another CE\n");
       stealSchedulerObject = wstSchedObj->spawn_queue;
       retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
    }
    if (!ocrGuidIsNull(edtObj.guid.guid) ) {
       DPRINTF(DEBUG_LVL_INFO, "Found guid in spawn_queue\n");
    }
#endif
    //try to pop from own deque
    if (ocrGuidIsNull(edtObj.guid.guid)) {
       DPRINTF(DEBUG_LVL_INFO, "self->scheduler->pd->schedulerObjectFactories[%"PRIx32"]\n", schedObj->fctId);
       retVal = fact->fcts.remove(fact, schedObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_TAIL);
    }
#if 1 //Turn off to disable stealing (serialize execution)
    //If pop fails, then try to steal from other deques
    if (ocrGuidIsNull(edtObj.guid.guid)) {
        //First try to steal from the last deque that was visited (probably had a successful steal)
        wstSchedObj = (ocrSchedulerObjectWst_t*)rootObj;
        stealSchedulerObject = wstSchedObj->deques[ceContext->stealSchedulerObjectIndex];
        DPRINTF(DEBUG_LVL_INFO, "wstSchedObj->deques[%"PRIx32"] \n", ceContext->stealSchedulerObjectIndex);
        ocrAssert(stealSchedulerObject->fctId == schedObj->fctId);
        retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD); //try cached deque first

        //If cached steal failed, then restart steal loop from starting index
        u32 i;
        for (i = 1; ocrGuidIsNull(edtObj.guid.guid) && i < wstSchedObj->numDeques; i++) {
            ceContext->stealSchedulerObjectIndex = (context->id + i) % wstSchedObj->numDeques;
            stealSchedulerObject = wstSchedObj->deques[ceContext->stealSchedulerObjectIndex];
            DPRINTF(DEBUG_LVL_INFO, "wstSchedObj->deques[%"PRIx32"] \n", ceContext->stealSchedulerObjectIndex);
            ocrAssert(stealSchedulerObject->fctId == schedObj->fctId);
            retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
        }
    }

#ifdef OCR_ENABLE_SCHEDULER_SPAWN_QUEUE
    DPRINTF(DEBUG_LVL_INFO, "OCR_ENABLE_SCHEDULER_SPAWN_QUEUE enabled\n");
    //  if no work was found even after all of that, check the spawn_queue
    if(ocrGuidIsNull(edtObj.guid.guid) && (AGENT_FROM_ID(context->location) != ID_AGENT_CE)){
       DPRINTF(DEBUG_LVL_INFO, "XE needs work from SPAWN_QUEUE\n");
       stealSchedulerObject = wstSchedObj->spawn_queue;
       retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
    }
#endif
#endif

    if (!(ocrGuidIsNull(edtObj.guid.guid))) {
        ocrAssert(retVal == 0);
        *fguid = edtObj.guid;
        derived->workCount--;
    } else {
        ocrAssert(retVal != 0);
        ocrAssert(0); //Check done early
    }
    return retVal;
}

static u8 ceSchedulerHeuristicWorkEdtUserInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    // REC: Hack for now because it seems that messages like this still get through
    // even after a shutdown is noticed. This may be due to messages that are not fully
    // drained. At any rate, we will, for now, return an error code and print a warning
    if(derived->shutdownMode) {
        DPRINTF(DEBUG_LVL_WARN, "Request for work received after shutdown invoked.\n");
        return 1;
    }
    //ASSERT(!derived->shutdownMode);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    u8 retVal = 0;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        {
            ocrFatGuid_t *fguid = &(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
            retVal = ceWorkStealingGet(self, context, fguid);
        }
        break;
    case OCR_SCHED_WORK_MULTI_EDTS_USER:
        {
            ocrFatGuid_t *fguids = taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_MULTI_EDTS_USER).edts;
            u32 reqGuidCount = taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_MULTI_EDTS_USER).guidCount;
            u32 guidCount = 0;
            if(derived->workCount > 1) {
              if(derived->workCount <= reqGuidCount*2){
                guidCount = derived->workCount/2;
              } else {
                guidCount = reqGuidCount;
              }
            } else {
              if(derived->workCount == 0){
                 retVal = 1;
              }
              guidCount = derived->workCount;
            }

            u32 idx, actualCount = 0;
            DPRINTF(DEBUG_LVL_VVERB, "Trying to steal %d EDTs\n", guidCount);
            for (idx = 0; idx < guidCount && retVal == 0; idx++, actualCount++) {
                retVal = ceWorkStealingGet(self, context, &fguids[idx]);
                if (retVal != 0) actualCount--;
            }
            taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_MULTI_EDTS_USER).guidCount = actualCount;
            DPRINTF(DEBUG_LVL_VVERB, "Got %d EDTs\n", actualCount);
            if (idx > 1) {
                retVal = 0; // Well, we got at least one, so I call that a success!
            }
        }
        break;
    default:
        ocrAssert(0);
        return OCR_ENOTSUP;
    }
    if (retVal) {
        ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
        ocrAssert(ceContext->inWorkRequestPending == false);
        DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE from %"PRIx64" (pending)\n", context->location);
        // If the receiver is an XE, put it to sleep
        u64 agentId = AGENT_FROM_ID(context->location);
        if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7)) {
            //FIXME: enable this with #861
            //DPRINTF(DEBUG_LVL_INFO, "XE %"PRIx64" put to sleep\n", context->location);
            //hal_sleep(agentId);
            derived->pendingXeCount++;
        }
        ceContext->inWorkRequestPending = true;
        ocrAssert(ceContext->msgId == 0 && hints != NULL);
        ocrPolicyMsg_t *message = (ocrPolicyMsg_t*)hints;
        ceContext->msgId = message->msgId; //HACK: Store the msgId for future response
        derived->inPendingCount++;
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE from %"PRIx64" (found)\n", context->location);
    }
    DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE DONE from %"PRIx64" \n", context->location);
    return retVal;
}

u8 ceSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrAssert(context);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
    case OCR_SCHED_WORK_MULTI_EDTS_USER:
        return ceSchedulerHeuristicWorkEdtUserInvoke(self, context, opArgs, hints);
    default:
        ocrAssert(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrAssert(0);
    return OCR_ENOTSUP;
}

static u8 handleEmptyResponse(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context) {
    ocrAssert(0); //We should not hit this
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrAssert(AGENT_FROM_ID(context->location) == ID_AGENT_CE);
    ocrAssert(ceContext->outWorkRequestPending);
    ceContext->outWorkRequestPending = false;
    derived->outWorkVictimsAvailable++;
    return 0;
}

static ocrSchedulerHeuristicContext_t* ceAffinitizeTask(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t fguid) {
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    ocrTask_t *task = (ocrTask_t*)fguid.metaDataPtr;
    ocrAssert(task);

    //Schedule EDT according to its DB affinity
    ocrSchedulerHeuristicContext_t *insertContext = context;
    u64 affinitySlot = ((u64)-1);
    ocrHint_t edtHint;
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    RESULT_ASSERT(ocrGetHint(task->guid, &edtHint), ==, 0);
    if (ocrGetHintValue(&edtHint, OCR_HINT_EDT_SLOT_MAX_ACCESS, &affinitySlot) == 0) {
        ocrAssert(affinitySlot < task->depc);
        ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //TODO:This is temporary until we get proper introspection support
        ocrEdtDep_t *depv = hcTask->resolvedDeps;
        ocrGuid_t dbGuid = depv[affinitySlot].guid;
        ocrDataBlock_t *db = NULL;
        pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dbGuid, (u64*)(&(db)), NULL, MD_LOCAL, NULL);
        ocrAssert(db);
        u64 dbMemAffinity = ((u64)-1);
        ocrHint_t dbHint;
        ocrHintInit(&dbHint, OCR_HINT_DB_T);
        RESULT_ASSERT(ocrGetHint(dbGuid, &dbHint), ==, 0);
        if (ocrGetHintValue(&dbHint, OCR_HINT_DB_AFFINITY, &dbMemAffinity) == 0) {
            ocrLocation_t myLoc = pd->myLocation;
            ocrLocation_t dbLoc = dbMemAffinity;
            ocrLocation_t affinityLoc = dbLoc;
            u64 dbLocCluster = CLUSTER_FROM_ID(dbLoc);
            u64 dbLocBlk = BLOCK_FROM_ID(dbLoc);
            if (dbLocCluster != CLUSTER_FROM_ID(myLoc)) {
                affinityLoc = MAKE_CORE_ID(0, 0, 0, dbLocCluster, 0, ID_AGENT_CE); //Map it to block 0 for dbLocCluster
            } else if (dbLocBlk != BLOCK_FROM_ID(myLoc)) {
                affinityLoc = MAKE_CORE_ID(0, 0, 0, dbLocCluster, dbLocBlk, ID_AGENT_CE); //Map it to dbLocBlk of current unit
            }
            u32 i;
            bool found __attribute__((unused)) = false;
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *ctxt = self->contexts[i];
                if (ctxt->location == affinityLoc) {
                    insertContext = ctxt;
                    found = true;
                    break;
                }
            }
            ocrAssert(found);
        }
    }
    return insertContext;
}

static u8 ceInsertTask(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerHeuristicContext_t *insertContext, ocrFatGuid_t fguid) {
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    ocrSchedulerHeuristicContextCe_t *ceInsertContext = (ocrSchedulerHeuristicContextCe_t*)insertContext;
    ocrSchedulerObject_t *insertSchedObj = ceInsertContext->mySchedulerObject;
    ocrAssert(insertSchedObj);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = fguid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrSchedulerObjectFactory_t *fact = pd->schedulerObjectFactories[insertSchedObj->fctId];

#ifdef OCR_ENABLE_SCHEDULER_SPAWN_QUEUE
    //if the OCR_HINT_EDT_SPAWNING hint is set on the EDT, put it in the spawning queue
    ocrHint_t spawnHint;
    ocrHintInit(&spawnHint, OCR_HINT_EDT_T);
    u64 hintVal = 0ULL;
    RESULT_ASSERT(ocrGetHint(fguid.guid, &spawnHint), ==, 0);
    if(ocrGetHintValue(&spawnHint, OCR_HINT_EDT_SPAWNING, &hintVal) == 0){
       DPRINTF(DEBUG_LVL_INFO, "Edt has hint OCR_HINT_EDT_SPAWNING\n");
       DPRINTF(DEBUG_LVL_INFO, ">>>  put it in spawn_queue!\n");
       ocrSchedulerObject_t* rootObj = self->scheduler->rootObj;
       ocrSchedulerObjectWst_t* wstSchedObj = (ocrSchedulerObjectWst_t*)rootObj;
       insertSchedObj = wstSchedObj->spawn_queue;
    }
#endif

    DPRINTF(DEBUG_LVL_VVERB, ">>>> inserting EDT\n");
    RESULT_ASSERT(fact->fcts.insert(fact, insertSchedObj, &edtObj, NULL, (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL)), ==, 0);
    derived->workCount++;
    DPRINTF(DEBUG_LVL_VVERB, "GIVEN WORK to context %"PRIx64"\n", insertContext->location);

    return 0;
}

static u8 ceSchedulerHeuristicNotifyEdtReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    // We should definitely not have EDTs becoming ready after the shutdown is called
    // Maybe if the shutdown is not the last EDT but even then...
    ocrAssert(!derived->shutdownMode);
    DPRINTF(DEBUG_LVL_VVERB, "GIVE WORK INVOKE from %"PRIx64"\n", opArgs->location);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrSchedulerHeuristicContext_t *insertContext = NULL;

    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
        {
            DPRINTF(DEBUG_LVL_VVERB, "One EDT ready\n");
            ocrFatGuid_t fguid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;
            if (ocrGuidIsNull(fguid.guid)) {
                return handleEmptyResponse(self, context);
            }
            insertContext = ceAffinitizeTask(self, context, fguid);
            ceInsertTask(self, context, insertContext, fguid);
        }
        break;
    case OCR_SCHED_NOTIFY_MULTI_EDTS_READY:
        {
            u32 idx, actualCount = 0;
            u32 guidCount = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_MULTI_EDTS_READY).guidCount;
            DPRINTF(DEBUG_LVL_VVERB, "%d EDTs ready\n", guidCount);
            for (idx = 0; idx < guidCount; idx++, actualCount++) {
                ocrFatGuid_t fguid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_MULTI_EDTS_READY).guids[idx];
                if (ocrGuidIsNull(fguid.guid)) {
                    return handleEmptyResponse(self, context);
                }
                insertContext = ceAffinitizeTask(self, context, fguid);
                ceInsertTask(self, context, insertContext, fguid);
            }
        }
        break;
    default:
        ocrAssert(0);
        return OCR_ENOTSUP;
    }

    if (AGENT_FROM_ID(context->location) == ID_AGENT_CE) {
        ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
        ocrAssert(ceContext->outWorkRequestPending);
        ceContext->outWorkRequestPending = false;
        derived->outWorkVictimsAvailable++;
    }

    return 0;
}

u8 ceSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrAssert(context);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
    case OCR_SCHED_NOTIFY_MULTI_EDTS_READY:
        return ceSchedulerHeuristicNotifyEdtReadyInvoke(self, context, opArgs, hints);
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
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
        return OCR_ENOP;
    // Notifies ignored by this heuristic
    case OCR_SCHED_NOTIFY_DB_CREATE:
        break;
    default:
        ocrAssert(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrAssert(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrAssert(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrAssert(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrAssert(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrAssert(0);
    return OCR_ENOTSUP;
}

static u8 makeWorkRequest(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, bool isBlocking) {
    u8 returnCode = 0;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    DPRINTF(DEBUG_LVL_VVERB, "MAKE_WORK_REQUEST to %"PRIx64"\n", context->location);
    ocrAssert(AGENT_FROM_ID(context->location) == ID_AGENT_CE);
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrAssert(ceContext->outWorkRequestPending == false);
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.srcLocation = pd->myLocation;
    msg.destLocation = context->location;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_GET_WORK
    msg.type = PD_MSG_SCHED_GET_WORK | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
#ifdef OCR_ENABLE_CE_GET_MULTI_WORK
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_MULTI_EDTS_USER;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_MULTI_EDTS_USER).guidCount = CE_GET_MULTI_WORK_MAX_SIZE;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_MULTI_EDTS_USER).edts = NULL;
#else /* !OCR_ENABLE_CE_GET_MULTI_WORK */
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_EDT_USER;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid = NULL_GUID;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.metaDataPtr = NULL;
#endif
    PD_MSG_FIELD_I(properties) = 0;

    if (isBlocking) {
        returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
        ocrAssert(returnCode == 0);
    } else {
        returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
    }
#undef PD_MSG
#undef PD_TYPE
    if (returnCode == 0) {
        ocrAssert(ceContext->outWorkRequestPending == false);
        ceContext->outWorkRequestPending = true;
        ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
        derived->outWorkVictimsAvailable--;
        DPRINTF(DEBUG_LVL_VVERB, "MAKE_WORK_REQUEST SUCCESS to %"PRIx64"\n", context->location);
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "MAKE WORK REQUEST ERROR CODE: %"PRIu32"\n", returnCode);
    }
    return returnCode;
}

static u8 respondWorkRequest(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t *fguid) {
    DPRINTF(DEBUG_LVL_VVERB, "RESPOND_WORK_REQUEST to %"PRIx64"\n", context->location);
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrAssert(ceContext->inWorkRequestPending);
    ocrAssert(fguid);
    bool ceMessage = false;

    // If the receiver is an XE, wake it up
    u64 agentId = AGENT_FROM_ID(context->location);
    if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7)) {
        //FIXME: enable this with #861
        //hal_wake(agentId);
        //DPRINTF(DEBUG_LVL_INFO, "XE %"PRIx64" woken up\n", context->location);
        derived->pendingXeCount--;
    } else {
        ceMessage = true;
    }
    ceContext->inWorkRequestPending = false;
    u64 msgId = ceContext->msgId;
    ceContext->msgId = 0;
    derived->inPendingCount--;

    //TODO: For now pretend we are an XE until
    //we get the transact messages working
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.srcLocation = pd->myLocation;
    msg.destLocation = context->location;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_GET_WORK
    msg.type = PD_MSG_SCHED_GET_WORK | PD_MSG_RESPONSE | PD_MSG_REQ_RESPONSE;
    msg.msgId = msgId; //HACK: Use the msgId from the original request
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_EDT_USER;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt = *fguid;
    PD_MSG_FIELD_I(properties) = 0;
    if (ceMessage) {
        RESULT_ASSERT(pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0), ==, 0);
    } else {
        RESULT_ASSERT(pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0), ==, 0);
    }
#undef PD_MSG
#undef PD_TYPE
    ocrAssert(ceContext->inWorkRequestPending == false);
    DPRINTF(DEBUG_LVL_VVERB, "RESPOND_WORK_REQUEST SUCCESS to %"PRIx64"\n", context->location);
    return 0;
}

//The scheduler update function is called by the worker/policy-domain proactively
//for the scheduler to make progress or organize itself. There are two kinds of
//update properties:
//1. IDLE:
//    The scheduler is notified that the worker is currently sitting idle.
//    This allows the scheduler to make progress on pending work.
//2. SHUTDOWN:
//    The scheduler is notified that shutdown is occuring.
//    This allows the scheduler to start preparing for shutdown, such as,
//    releasing all pending XEs etc.
u8 ceSchedulerHeuristicUpdate(ocrSchedulerHeuristic_t *self, u32 properties) {
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    u32 i;
    switch(properties) {
    case OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_IDLE: {
            if (derived->shutdownMode)
                break; //We are shutting down, no more processing

            DPRINTF(DEBUG_LVL_VVERB, "IDLE [work: %"PRIu64" pending: (%"PRIu32", %"PRIu32") victims: %"PRIu32"]\n", derived->workCount, derived->inPendingCount, derived->pendingXeCount, derived->outWorkVictimsAvailable);
            //High-level logic:
            //First check if any remote agent (XE or CE) is waiting for a response from this CE.
            //If nobody is waiting, then we return back to caller.
            //If anybody is waiting, we try to make progress based on the requester.
            //If request is from a CE/XE, then check if work is available to service that request.
            //If not, then try to send out work requests to as many neighbors as the number of pending XEs.

            if (derived->inPendingCount == 0)
                break; //Nobody is waiting. Great!... break out and return.
            ocrAssert(derived->inPendingCount <= self->contextCount);

            //If we have work, respond to the pending requests...
            //We satisfy the XEs requests first before giving work to CEs
            ocrFatGuid_t fguid;
            for (i = 0; i < self->contextCount && derived->workCount != 0; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                if (ceContext->inWorkRequestPending &&                   /*We have a pending context, and ...*/
                    ((derived->workCount > derived->pendingXeCount) ||   /*either we have enough work to serve anyone, ...*/
                     (AGENT_FROM_ID(context->location) != ID_AGENT_CE))) /*we have very little and we want to serve the XEs first*/
                {
                    fguid.guid = NULL_GUID;
                    fguid.metaDataPtr = NULL;
                    if (ceWorkStealingGet(self, context, &fguid) == 0) {
                        respondWorkRequest(self, context, &fguid);
                    }
                }
            }

            if (derived->workCount == 0 && derived->inPendingCount != 0 && derived->outWorkVictimsAvailable != 0) {
                ocrAssert(derived->outWorkVictimsAvailable <= pd->neighborCount);

                //If my current block is out of work, ensure parent gets a work request
#ifdef OCR_ENABLE_XE_GET_MULTI_WORK
                if ((pd->myLocation != pd->parentLocation) && (derived->pendingXeCount == 1)) {
#else
                if ((pd->myLocation != pd->parentLocation) && (derived->pendingXeCount == ((ocrPolicyDomainCe_t*)pd)->xeCount)) {
#endif
                    for (i = 0; i < self->contextCount; i++) {
                        ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                        ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                        if (context->location == pd->parentLocation && ceContext->outWorkRequestPending == false) {
                            DPRINTF(DEBUG_LVL_VVERB, "FORCE WORK REQUEST to parent %"PRIx64"\n", context->location);
                            RESULT_ASSERT(makeWorkRequest(self, context, true), ==, 0);
                            DPRINTF(DEBUG_LVL_VVERB, "FORCE WORK REQUEST SUCCESS to parent %"PRIx64"\n", context->location);
                            break;
                        }
                    }
                }

                //No work left... some are still waiting... so, try to find work from neighbors
                //If work request fails, no problem, try later
                //If work request fails due to DEAD neighbor, make sure we don't try to send any more requests to that neighbor
                for (i = 0; i < self->contextCount && derived->outWorkVictimsAvailable != 0; i++) {
                    ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                    if (ceContext->canAcceptWorkRequest && ceContext->outWorkRequestPending == false) {
                        u8 returnCode = makeWorkRequest(self, context, false);
                        if (returnCode != 0) {
                            if (returnCode == 2) { //Location is dead
                                ocrAssert(context->location != pd->parentLocation); //Make sure parent is not dead
                                ceContext->canAcceptWorkRequest = false;
                                derived->outWorkVictimsAvailable--;
                            }
                        } else {
                            ocrAssert(ceContext->outWorkRequestPending);
                            ocrAssert(derived->outWorkVictimsAvailable <= pd->neighborCount);
                        }
                    }
                }
            }
        }
        break;
    case OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_SHUTDOWN: {
            ocrAssert(!derived->shutdownMode);
            derived->shutdownMode = true;

            ocrFatGuid_t fguid;
            fguid.guid = NULL_GUID;
            fguid.metaDataPtr = NULL;

            //Ensure shutdown response is sent to all pending XE children... (they are sitting around doing nothing)
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                if ((ceContext->inWorkRequestPending) && (ceContext->isChild)) {
                    //respondShutdown(self, context, true);
                    respondWorkRequest(self, context, &fguid);
                }
            }

            // No need to inform CEs as the PD takes care of this.

#if 0
            //Finally try to shutdown to non-pending CE children as well
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                if ((AGENT_FROM_ID(context->location) == ID_AGENT_CE) && (ceContext->isChild)) {
                    respondShutdown(self, context, true);
                }
            }
#endif
        }
        break;
    default:
        ocrAssert(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

/******************************************************/
/* OCR-CE SCHEDULER_HEURISTIC FACTORY                 */
/******************************************************/

void destructSchedulerHeuristicFactoryCe(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryCe(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryCe_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicCe;
    base->destruct = &destructSchedulerHeuristicFactoryCe;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), ceSchedulerHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), ceSchedulerHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), ceSchedulerHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), ceSchedulerHeuristicGetContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_CE */
