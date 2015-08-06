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
    return self;
}

void initializeContext(ocrSchedulerHeuristicContext_t *context, u64 contextId) {
    context->id = contextId;
    context->location = INVALID_LOCATION;
    context->actionSet = NULL;
    context->cost = NULL;
    context->properties = 0;

    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ceContext->stealSchedulerObjectIndex = ((u64)-1);
    ceContext->mySchedulerObject = NULL;
    ceContext->inWorkRequestPending = false;
    ceContext->outWorkRequestPending = false;
    ceContext->canAcceptWorkRequest = false;
    ceContext->isChild = false;
    return;
}

u8 ceSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
            ASSERT(SCHEDULER_OBJECT_KIND(self->scheduler->rootObj->kind) == OCR_SCHEDULER_OBJECT_WST);
            ASSERT(scheduler->pd != NULL);
            ASSERT(scheduler->contextCount > 0);
            ocrPolicyDomain_t *pd = scheduler->pd;
            ASSERT(pd == PD);

            DPRINTF(DEBUG_LVL_VVERB, "ContextCount: %lu\n", scheduler->contextCount);
            self->contextCount = scheduler->contextCount;
            self->contexts = (ocrSchedulerHeuristicContext_t **)pd->fcts.pdMalloc(pd, self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*));
            ocrSchedulerHeuristicContextCe_t *contextAlloc = (ocrSchedulerHeuristicContextCe_t *)pd->fcts.pdMalloc(pd, self->contextCount * sizeof(ocrSchedulerHeuristicContextCe_t));
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                initializeContext(context, i);
                self->contexts[i] = context;
            }

            ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
            derived->outWorkVictimsAvailable = pd->neighborCount;
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

void ceSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

ocrSchedulerHeuristicContext_t* ceSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, u64 contextId) {
    return self->contexts[contextId];
}

bool isChildCe(ocrLocation_t myLocation, ocrLocation_t loc) {
    if (AGENT_FROM_ID(loc) != ID_AGENT_CE)
        return false;
    if ((myLocation & ID_BLOCK_MASK) == 0) {                              //If I am the block0 CE, ...
        if ((myLocation & ID_UNIT_MASK) == 0) {                           //If I am the unit0,block0 CE
            return true;                                                  //then, everyone is my child
        } else if ((myLocation & ID_UNIT_MASK) == (loc & ID_UNIT_MASK)) { //else if, context is a CE in my unit
            return true;                                                  //then, this non-block0 CE is my child
        }
    }
    return false;
}

/* Setup the context for this contextId */
u8 ceSchedulerHeuristicRegisterContext(ocrSchedulerHeuristic_t *self, u64 contextId, ocrLocation_t loc) {
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    ocrSchedulerHeuristicContext_t *context = self->contexts[contextId];
    ASSERT(context->location == INVALID_LOCATION && loc != INVALID_LOCATION);
    context->location = loc;

    //Reserve a deque for this context
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self->scheduler->rootObj;
    ASSERT(contextId < wstSchedObj->numDeques);
    ceContext->mySchedulerObject = wstSchedObj->deques[contextId];
    ASSERT(ceContext->mySchedulerObject);
    ceContext->stealSchedulerObjectIndex = (contextId + 1) % wstSchedObj->numDeques;

    //Set deque's mapping to this context
    ocrSchedulerObjectFactory_t *fact = pd->schedulerObjectFactories[ceContext->mySchedulerObject->fctId];
    fact->fcts.setLocationForSchedulerObject(fact, ceContext->mySchedulerObject, contextId, OCR_SCHEDULER_OBJECT_MAPPING_PINNED);

    if (AGENT_FROM_ID(loc) == ID_AGENT_CE) {
        ceContext->canAcceptWorkRequest = true;
        if (isChildCe(pd->myLocation, loc)) {
            ceContext->isChild = true;
        }
        DPRINTF(DEBUG_LVL_VVERB, "[CE %lx] Registering CE location: %lx (isChild: %u)\n", pd->myLocation, loc, (u32)ceContext->isChild);
    } else {
        ceContext->isChild = true;                                                //XE is always a child
        DPRINTF(DEBUG_LVL_VVERB, "[CE %lx] Registering XE location: %lx\n", pd->myLocation, loc);
    }
    return 0;
}

/* Find EDT for the worker to execute - This uses random workstealing to find work if no work is found owned deque */
static u8 ceWorkStealingGet(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t *fguid) {
    ASSERT(fguid->guid == NULL_GUID);
    ASSERT(fguid->metaDataPtr == NULL);

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

    //First try to pop from own deque
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrSchedulerObject_t *schedObj = ceContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    u8 retVal = fact->fcts.remove(fact, schedObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_DEQ_POP);

#if 1 //Turn off to disable stealing (serialize execution)
    //If pop fails, then try to steal from other deques
    if (edtObj.guid.guid == NULL_GUID) {
        //First try to steal from the last deque that was visited (probably had a successful steal)
        ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)rootObj;
        ocrSchedulerObject_t *stealSchedulerObject = wstSchedObj->deques[ceContext->stealSchedulerObjectIndex];
        ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
        retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_DEQ_POP); //try cached deque first

        //If cached steal failed, then restart steal loop from starting index
        u32 i;
        for (i = 1; edtObj.guid.guid == NULL_GUID && i < wstSchedObj->numDeques; i++) {
            ceContext->stealSchedulerObjectIndex = (context->id + i) % wstSchedObj->numDeques;
            stealSchedulerObject = wstSchedObj->deques[ceContext->stealSchedulerObjectIndex];
            ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
            retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_DEQ_POP);
        }
    }
#endif

    if (edtObj.guid.guid != NULL_GUID) {
        ASSERT(retVal == 0);
        *fguid = edtObj.guid;
        derived->workCount--;
    } else {
        ASSERT(retVal != 0);
        ASSERT(0); //Check done early
    }
    return retVal;
}

static u8 ceSchedulerHeuristicWorkEdtUserInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    ocrPolicyDomainCe_t * cePolicy = (ocrPolicyDomainCe_t *)pd;
    if (cePolicy->shutdownMode) {
        return 0;
    }
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrFatGuid_t *fguid = &(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
    u8 retVal = ceWorkStealingGet(self, context, fguid);
    if (retVal) {
        ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
        ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
        ASSERT(ceContext->inWorkRequestPending == false);
        DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE from %lx (pending)\n", context->location);
        // If the receiver is an XE, put it to sleep
        u64 agentId = AGENT_FROM_ID(context->location);
        if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7)) {
            DPRINTF(DEBUG_LVL_INFO, "XE %lx put to sleep\n", context->location);
            hal_sleep(agentId);
            derived->pendingXeCount++;
        }
        ceContext->inWorkRequestPending = true;
        derived->inPendingCount++;
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE from %lx (found)\n", context->location);
    }
    DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE DONE from %lx \n", context->location);
    return retVal;
}

u8 ceSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        return ceSchedulerHeuristicWorkEdtUserInvoke(self, context, opArgs, hints);
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 handleEmptyResponse(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context) {
    ASSERT(0); //We should not hit this
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ASSERT(AGENT_FROM_ID(context->location) == ID_AGENT_CE);
    ASSERT(ceContext->outWorkRequestPending);
    ceContext->outWorkRequestPending = false;
    derived->outWorkVictimsAvailable++;
    return 0;
}

static u8 ceSchedulerHeuristicNotifyEdtReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    DPRINTF(DEBUG_LVL_VVERB, "GIVE_WORK_INVOKE from %lx\n", context->location);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    ocrFatGuid_t fguid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;

    if (fguid.guid == NULL_GUID) {
        return handleEmptyResponse(self, context);
    }

    ocrTask_t *task = (ocrTask_t*)fguid.metaDataPtr;
    ASSERT(task);

    //Schedule EDT according to its DB affinity
    ocrSchedulerHeuristicContext_t *insertContext = context;
    u64 affinitySlot = ((u64)-1);
    ocrHint_t edtHint;
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    ASSERT(ocrGetHint(task->guid, &edtHint) == 0);
    if (ocrGetHintValue(&edtHint, OCR_HINT_EDT_SLOT_MAX_ACCESS, &affinitySlot) == 0) {
        ASSERT(affinitySlot < task->depc);
        ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //TODO:This is temporary until we get proper introspection support
        ocrEdtDep_t *depv = hcTask->resolvedDeps;
        ocrGuid_t dbGuid = depv[affinitySlot].guid;
        ocrDataBlock_t *db = NULL;
        pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dbGuid, (u64*)(&(db)), NULL);
        ASSERT(db);
        u64 dbMemAffinity = ((u64)-1);
        ocrHint_t dbHint;
        ocrHintInit(&dbHint, OCR_HINT_DB_T);
        ASSERT(ocrGetHint(dbGuid, &dbHint) == 0);
        if (ocrGetHintValue(&dbHint, OCR_HINT_DB_MEM_AFFINITY, &dbMemAffinity) == 0) {
            ocrLocation_t myLoc = pd->myLocation;
            ocrLocation_t dbLoc = dbMemAffinity;
            ocrLocation_t affinityLoc = dbLoc;
            u64 dbLocUnt = UNIT_FROM_ID(dbLoc);
            u64 dbLocBlk = BLOCK_FROM_ID(dbLoc);
            if (dbLocUnt != UNIT_FROM_ID(myLoc)) {
                affinityLoc = MAKE_CORE_ID(0, 0, 0, dbLocUnt, 0, ID_AGENT_CE); //Map it to block 0 for dbLocUnt
            } else if (dbLocBlk != BLOCK_FROM_ID(myLoc)) {
                affinityLoc = MAKE_CORE_ID(0, 0, 0, dbLocUnt, dbLocBlk, ID_AGENT_CE); //Map it to dbLocBlk of current unit
            }
            u32 i;
            bool found = false;
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *ctxt = self->contexts[i];
                if (ctxt->location == affinityLoc) {
                    insertContext = ctxt;
                    found = true;
                    break;
                }
            }
            ASSERT(found);
        }
    }

    ocrSchedulerHeuristicContextCe_t *ceInsertContext = (ocrSchedulerHeuristicContextCe_t*)insertContext;
    ocrSchedulerObject_t *insertSchedObj = ceInsertContext->mySchedulerObject;
    ASSERT(insertSchedObj);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = fguid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrSchedulerObjectFactory_t *fact = pd->schedulerObjectFactories[insertSchedObj->fctId];
    ASSERT(fact->fcts.insert(fact, insertSchedObj, &edtObj, 0) == 0);
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    derived->workCount++;
    if (AGENT_FROM_ID(context->location) == ID_AGENT_CE) {
        ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
        ASSERT(ceContext->outWorkRequestPending);
        ceContext->outWorkRequestPending = false;
        derived->outWorkVictimsAvailable++;
    }
    DPRINTF(DEBUG_LVL_VVERB, "GIVE_WORK_INVOKE DONE from %lx\n", context->location);
    return 0;
}

u8 ceSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
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
            ASSERT(pd->fcts.processMessage(pd, &msg, false) == 0);
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    // Notifies ignored by this heuristic
    case OCR_SCHED_NOTIFY_DB_CREATE:
        break;
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 makeWorkRequest(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, bool isBlocking) {
    u8 returnCode = 0;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    DPRINTF(DEBUG_LVL_VVERB, "MAKE_WORK_REQUEST to %lx\n", context->location);
    ASSERT(AGENT_FROM_ID(context->location) == ID_AGENT_CE);
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ASSERT(ceContext->outWorkRequestPending == false);
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.srcLocation = pd->myLocation;
    msg.destLocation = context->location;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_GET_WORK
    msg.type = PD_MSG_SCHED_GET_WORK | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_EDT_USER;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid = NULL_GUID;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.metaDataPtr = NULL;
    PD_MSG_FIELD_I(properties) = 0;

    if (isBlocking) {
        do {
            returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
            if (returnCode == 1) {
                PD_MSG_STACK(myMsg);
                ocrMsgHandle_t myHandle;
                myHandle.msg = &myMsg;
                ocrMsgHandle_t *handle = &myHandle;
                while(!pd->fcts.pollMessage(pd, &handle))
                    RESULT_ASSERT(pd->fcts.processMessage(pd, handle->response, true), ==, 0);
            }
        } while(returnCode == 1);
    } else {
        returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
    }
#undef PD_MSG
#undef PD_TYPE
    if (returnCode == 0) {
        ASSERT(ceContext->outWorkRequestPending == false);
        ceContext->outWorkRequestPending = true;
        ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
        derived->outWorkVictimsAvailable--;
        DPRINTF(DEBUG_LVL_VVERB, "MAKE_WORK_REQUEST SUCCESS to %lx\n", context->location);
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "MAKE WORK REQUEST ERROR CODE: %u\n", returnCode);
    }
    return returnCode;
}

static u8 respondWorkRequest(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t *fguid) {
    DPRINTF(DEBUG_LVL_VVERB, "RESPOND_WORK_REQUEST to %lx\n", context->location);
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    ASSERT(ceContext->inWorkRequestPending);
    ASSERT(fguid && fguid->guid != NULL_GUID);
    u8 returnCode = 0;
    bool ceMessage = false;

    // If the receiver is an XE, wake it up
    u64 agentId = AGENT_FROM_ID(context->location);
    if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7)) {
        hal_wake(agentId);
        DPRINTF(DEBUG_LVL_INFO, "XE %lx woken up\n", context->location);
        derived->pendingXeCount--;
    } else {
        ceMessage = true;
    }
    ceContext->inWorkRequestPending = false;
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
    PD_MSG_FIELD_IO(schedArgs).base.seqId = 0;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_EDT_USER;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt = *fguid;
    PD_MSG_FIELD_I(properties) = 0;
    if (ceMessage) {
        do {
            returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
            if (returnCode == 1) {
                PD_MSG_STACK(myMsg);
                ocrMsgHandle_t myHandle;
                myHandle.msg = &myMsg;
                ocrMsgHandle_t *handle = &myHandle;
                while(!pd->fcts.pollMessage(pd, &handle))
                    RESULT_ASSERT(pd->fcts.processMessage(pd, handle->response, true), ==, 0);
            }
        } while(returnCode == 1);
        ASSERT(returnCode == 0);
    } else {
        ASSERT(pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0) == 0);
    }
#undef PD_MSG
#undef PD_TYPE
    ASSERT(ceContext->inWorkRequestPending == false);
    DPRINTF(DEBUG_LVL_VVERB, "RESPOND_WORK_REQUEST SUCCESS to %lx\n", context->location);
    return 0;
}

static u8 respondShutdown(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, bool isBlocking) {
    DPRINTF(DEBUG_LVL_VVERB, "SCHEDULER SHUTDOWN RESPONSE to %lx\n", context->location);
    ocrSchedulerHeuristicCe_t *derived = (ocrSchedulerHeuristicCe_t*)self;
    ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
    u8 returnCode = 0;
    bool ceMessage = false;

    // If the receiver is an XE, wake it up
    u64 agentId = AGENT_FROM_ID(context->location);
    if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7)) {
        hal_wake(agentId);
        DPRINTF(DEBUG_LVL_INFO, "XE %lx woken up\n", context->location);
        derived->pendingXeCount--;
    } else {
        ceMessage = true;
    }

    //TODO: For now pretend we are an XE until
    //we get the transact messages working
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.srcLocation = pd->myLocation;
    msg.destLocation = context->location;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
    msg.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_RESPONSE | PD_MSG_REQ_RESPONSE;
    if (ceMessage) {
        if (isBlocking) {
            do {
                returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
                if (returnCode == 1) {
                    PD_MSG_STACK(myMsg);
                    ocrMsgHandle_t myHandle;
                    myHandle.msg = &myMsg;
                    ocrMsgHandle_t *handle = &myHandle;
                    while(!pd->fcts.pollMessage(pd, &handle))
                        RESULT_ASSERT(pd->fcts.processMessage(pd, handle->response, true), ==, 0);
                }
            } while(returnCode == 1);
        } else {
            returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
        }
    } else {
        ASSERT(pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0) == 0);
    }
#undef PD_MSG
#undef PD_TYPE
    if (returnCode == 0 || returnCode == 2) {
        if (ceContext->inWorkRequestPending) derived->inPendingCount--;
        ceContext->inWorkRequestPending = false;
        ceContext->isChild = false;
    }
    DPRINTF(DEBUG_LVL_VVERB, "SCHEDULER SHUTDOWN RESPONSE SUCCESS (%u) to %lx\n", returnCode, context->location);
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
            DPRINTF(DEBUG_LVL_VVERB, "IDLE [work: %lu pending: (%u, %u) victims: %u]\n", derived->workCount, derived->inPendingCount, derived->pendingXeCount, derived->outWorkVictimsAvailable);
            //High-level logic:
            //First check if any remote agent (XE or CE) is waiting for a response from this CE.
            //If nobody is waiting, then we return back to caller.
            //If anybody is waiting, we try to make progress based on the requester.
            //If request is from a CE/XE, then check if work is available to service that request.
            //If not, then try to send out work requests to as many neighbors as the number of pending XEs.

            if (derived->inPendingCount == 0)
                break; //Nobody is waiting. Great!... break out and return.
            ASSERT(derived->inPendingCount <= self->contextCount);

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
                ASSERT(derived->outWorkVictimsAvailable <= pd->neighborCount);

                //If my current block is out of work, ensure parent gets a work request
                if ((pd->myLocation != pd->parentLocation) && (derived->pendingXeCount == ((ocrPolicyDomainCe_t*)pd)->xeCount)) {
                    for (i = 0; i < self->contextCount; i++) {
                        ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                        ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                        if (context->location == pd->parentLocation && ceContext->outWorkRequestPending == false) {
                            DPRINTF(DEBUG_LVL_VVERB, "FORCE WORK REQUEST to parent %lx\n", context->location);
                            ASSERT(makeWorkRequest(self, context, true) == 0);
                            DPRINTF(DEBUG_LVL_VVERB, "FORCE WORK REQUEST SUCCESS to parent %lx\n", context->location);
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
                                ASSERT(context->location != pd->parentLocation); //Make sure parent is not dead
                                ceContext->canAcceptWorkRequest = false;
                                derived->outWorkVictimsAvailable--;
                            }
                        } else {
                            ASSERT(ceContext->outWorkRequestPending);
                            ASSERT(derived->outWorkVictimsAvailable <= pd->neighborCount);
                        }
                    }
                }
            }
        }
        break;
    case OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_SHUTDOWN: {
            //First, ensure shutdown response is sent to all pending children...
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                if ((ceContext->inWorkRequestPending) && (ceContext->isChild)) {
                    respondShutdown(self, context, true);
                }
            }

            //Next, try to send shutdown to other pending agents
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCe_t *ceContext = (ocrSchedulerHeuristicContextCe_t*)context;
                if (ceContext->inWorkRequestPending) {
                    respondShutdown(self, context, false);
                }
            }

#if 1
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
        ASSERT(0);
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
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, u64), ceSchedulerHeuristicGetContext);
    base->fcts.registerContext = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u64, ocrLocation_t), ceSchedulerHeuristicRegisterContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), ceSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_CE */