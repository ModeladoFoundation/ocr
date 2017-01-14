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
#ifdef ENABLE_SCHEDULER_HEURISTIC_CE_AFF

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "extensions/ocr-hints.h"
#include "scheduler-heuristic/ce-affinitized/ce-aff-scheduler-heuristic.h"
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

ocrSchedulerHeuristic_t* newSchedulerHeuristicCeAff(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicCeAff_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
    derived->workCount = 0;
    derived->inPendingCount = 0;
    derived->pendingXeCount = 0;
    derived->outWorkVictimsAvailable[0] = derived->outWorkVictimsAvailable[1] = 0;
    derived->shutdownMode = false;
    derived->enforceAffinity = ((paramListSchedulerHeuristicCeAff_t*)perInstance)->enforceAffinity;
    return self;
}

u8 ceSchedulerHeuristicSwitchRunlevelAff(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
    {
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_MEMORY_OK\n");
        ASSERT(self->scheduler);
        self->contextCount = ((ocrPolicyDomainCe_t*)PD)->xeCount + PD->neighborCount;
        ASSERT(self->contextCount > 0);
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
            ocrSchedulerHeuristicContextCeAff_t *contextAlloc = (ocrSchedulerHeuristicContextCeAff_t *)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContextCeAff_t));
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                self->contexts[i] = context;
                context->id = i;
                context->actionSet = NULL;
                context->cost = NULL;
                context->properties = 0;
                ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
                ceContext->msgId = 0;
                ceContext->stealSchedulerContextIndex = ((u32)-1);
                ceContext->mySchedulerObject = NULL;
                ceContext->inWorkRequestPending = false;
                ceContext->outWorkRequestPending = NO_REQUEST;
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
            ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
            derived->outWorkVictimsAvailable[0] = PD->neighborCount; // Initially, we can ask all neighbors for affinitized work
            derived->rrXE = 0;
            derived->rrInsert = 0;
            derived->xeCount = ((ocrPolicyDomainCe_t*)PD)->xeCount;
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
                ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
                //BUG #920 - Revisit getSchedulerObjectForLocation API
                ceContext->mySchedulerObject = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_DEQUE,
                                                    context->location, OCR_SCHEDULER_OBJECT_MAPPING_MAPPED, 0);
                ASSERT(ceContext->mySchedulerObject && ceContext->mySchedulerObject != rootObj);
                if(i < ((ocrPolicyDomainCe_t*)PD)->xeCount)
                    ceContext->stealSchedulerContextIndex = (i + 1) % ((ocrPolicyDomainCe_t*)PD)->xeCount;
                DPRINTF(DEBUG_LVL_VVERB, "Scheduler object %p (fact: %"PRId32") for location: %"PRIx64"\n", ceContext->mySchedulerObject, ceContext->mySchedulerObject->fctId, context->location);
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

void ceSchedulerHeuristicDestructAff(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

ocrSchedulerHeuristicContext_t* ceSchedulerHeuristicGetContextAff(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    u32 i;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    // If we are asked for our context, we return an XE context in a round-robin fashion
    if (pd->myLocation == loc) {
        ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
        return self->contexts[derived->rrXE];
    }
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

/* Find EDT for the worker to execute. This has two main modes of operation:
 *  For XEs:
 *    - if discardAffinity is false:
 *      - it will look in its own queue
 *      - if failed, it will look in other XE's queues
 *    - if discardAffinity is true:
 *      - same as for discardAffinity as false but also look in other CE's queues (outgoing)
 *  For CEs:
 *    - if discardAffinity is false:
 *      - it will look in its own queue (and that is it)
 *    - if discardAffinity is true:
 *      - it will look in other queues starting with other CEs
 *      - if all other CE queues are empty, it will turn to the XE queues
 *
 * The idea is to have a repellant at first (discardAffinity being false) and then try
 * to respect some amount of locality (block stuff stays in the block as long as possible)
 */
static u8 ceWorkStealingGetAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t *fguid,
    bool discardAffinity) {

    DPRINTF(DEBUG_LVL_INFO, "Location 0x%"PRIx64" attempting to GET WORK; discardAffinity:%"PRIu32"\n",
        (u64)context->location, (u32)discardAffinity);

    ASSERT(ocrGuidIsNull(fguid->guid));
    ASSERT(fguid->metaDataPtr == NULL);

    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
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
    ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
    ocrSchedulerObject_t *schedObj = ceContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    u8 retVal = fact->fcts.remove(fact, schedObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_TAIL);
    ocrLocation_t foundLocation __attribute__((unused)) = context->location;
    bool nonAffWork __attribute__((unused)) = false;

#if 1 //Turn off to disable stealing (serialize execution)
    //If pop fails, then try to steal from other deques
    if (ocrGuidIsNull(edtObj.guid.guid)) {
        DPRINTF(DEBUG_LVL_VVERB, "No self-work\n");
        if(!discardAffinity && !ceContext->isChild) {
            // This means that we are a CE and discardAffinity is false. In this case, we stop right here and return.
            DPRINTF(DEBUG_LVL_VERB, "Affinitized CE not stealing -- returning no work\n");
            return 1;
        }
        // In other cases, we try stealing. This is either if we are a XE or a CE and we don't care about affinities.
        //First try to steal from the last deque that was visited (probably had a successful steal)
        ocrSchedulerObject_t *stealSchedulerObject = NULL;
        if(ceContext->stealSchedulerContextIndex != (u32)-1) {
            stealSchedulerObject = ((ocrSchedulerHeuristicContextCeAff_t*)(self->contexts[ceContext->stealSchedulerContextIndex]))->mySchedulerObject;
            DPRINTF(DEBUG_LVL_VVERB, "Attempting steal in last successful place: 0x%"PRIx64"\n", self->contexts[ceContext->stealSchedulerContextIndex]->location);
            ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
            retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD); //try cached deque first
            foundLocation = self->contexts[ceContext->stealSchedulerContextIndex]->location;
        }

        if(ocrGuidIsNull(edtObj.guid.guid)) {
            // This failed, we loop looking for more work
            if(ceContext->isChild) {
                // We are a XE, first loop around the other XEs
                u32 i;
                for (i = 1; ocrGuidIsNull(edtObj.guid.guid) && i < derived->xeCount; ++i) {
                    ceContext->stealSchedulerContextIndex = (context->id + i) % derived->xeCount;
                    ocrSchedulerHeuristicContextCeAff_t *otherCtx = (ocrSchedulerHeuristicContextCeAff_t*)self->contexts[ceContext->stealSchedulerContextIndex];
                    stealSchedulerObject = otherCtx->mySchedulerObject;
                    DPRINTF(DEBUG_LVL_VVERB, "Attempting steal from XE: 0x%"PRIx64"\n", self->contexts[ceContext->stealSchedulerContextIndex]->location);
                    ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
                    retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                    foundLocation = self->contexts[ceContext->stealSchedulerContextIndex]->location;
                }
                if(ocrGuidIsNull(edtObj.guid.guid)) {
                    // We failed to find something in all XEs in our block
                    DPRINTF(DEBUG_LVL_VVERB, "XE found no XE work.\n");
                    if(discardAffinity) {
                        // We now loop around the CEs but we don't change the stealSchedulerContextIndex
                        nonAffWork = true;
                        for(i = derived->xeCount; ocrGuidIsNull(edtObj.guid.guid) && i < self->contextCount; ++i) {
                            ocrSchedulerHeuristicContextCeAff_t *otherCtx = (ocrSchedulerHeuristicContextCeAff_t*)self->contexts[i];
                            stealSchedulerObject = otherCtx->mySchedulerObject;
                            DPRINTF(DEBUG_LVL_VVERB, "Attempting steal from CE: 0x%"PRIx64"\n", self->contexts[i]->location);
                            ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
                            retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                            foundLocation = self->contexts[i]->location;
                        }
                    } else {
                        DPRINTF(DEBUG_LVL_VVERB, "XE affinitized steal -- not stealing from CEs\n");
                        return 1;
                    }
                }
            } else {
                nonAffWork = true;
                // We are a CE and we know that discardAffinity is true, first loop around the other CEs
                ASSERT(discardAffinity);
                u32 i;
                for(i = 1; ocrGuidIsNull(edtObj.guid.guid) && i < (self->contextCount - derived->xeCount); ++i) {
                    ceContext->stealSchedulerContextIndex = (context->id + i);
                    if(ceContext->stealSchedulerContextIndex >= self->contextCount) {
                        ceContext->stealSchedulerContextIndex = derived->xeCount;
                    }
                    ocrSchedulerHeuristicContextCeAff_t *otherCtx = (ocrSchedulerHeuristicContextCeAff_t*)self->contexts[ceContext->stealSchedulerContextIndex];
                    stealSchedulerObject = otherCtx->mySchedulerObject;
                    DPRINTF(DEBUG_LVL_VVERB, "Attempting steal from neighbor CE: 0x%"PRIx64"\n", self->contexts[ceContext->stealSchedulerContextIndex]->location);
                    ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
                    retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                    foundLocation = self->contexts[ceContext->stealSchedulerContextIndex]->location;
                }

                // If this did not work, we then go steal from XEs but don't update stealSchedulerContextIndex
                for(i = 0; ocrGuidIsNull(edtObj.guid.guid) && i < derived->xeCount; ++i) {
                    ocrSchedulerHeuristicContextCeAff_t *otherCtx = (ocrSchedulerHeuristicContextCeAff_t*)self->contexts[i];
                    stealSchedulerObject = otherCtx->mySchedulerObject;
                    DPRINTF(DEBUG_LVL_VVERB, "Attempting steal from internal XE: 0x%"PRIx64"\n", self->contexts[i]->location);
                    ASSERT(stealSchedulerObject->fctId == schedObj->fctId);
                    retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                    foundLocation = self->contexts[i]->location;
                }
            }
        }
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "Found work in my queue: "GUIDF"\n", GUIDA(edtObj.guid.guid));
    }
#endif

    if (!(ocrGuidIsNull(edtObj.guid.guid))) {
        ASSERT(retVal == 0);
        *fguid = edtObj.guid;
        derived->workCount--;
        DPRINTF(DEBUG_LVL_INFO, "Found work: "GUIDF" from 0x%"PRIx64" (nonAffinitized: %"PRIu32")\n", GUIDA(edtObj.guid.guid),
            (u64)foundLocation, (u32)nonAffWork);
    } else {
        ASSERT(retVal != 0);
        ASSERT(0); //Check done early
    }
    return retVal;
}

static u8 ceSchedulerHeuristicWorkEdtUserInvokeAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs,
    ocrRuntimeHint_t *hints) {

    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
    // REC: Hack for now because it seems that messages like this still get through
    // even after a shutdown is noticed. This may be due to messages that are not fully
    // drained. At any rate, we will, for now, return an error code and print a warning
    if(derived->shutdownMode) {
        DPRINTF(DEBUG_LVL_WARN, "Request for work received after shutdown invoked.\n");
        return 1;
    }
    //ASSERT(!derived->shutdownMode);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrFatGuid_t *fguid = &(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
    bool discardAffinity = taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).discardAffinity;
    if(derived->enforceAffinity) {
        if(discardAffinity) {
            DPRINTF(DEBUG_LVL_VERB, "Overriding discardAffinity -- setting to false since affinity enforcement is true\n");
        }
        discardAffinity = false;
    }
    u8 retVal = ceWorkStealingGetAff(self, context, fguid, discardAffinity);
    if (retVal) {
        ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
        // The neighbor should not make multiple requests before we answer.
        ASSERT(!ceContext->inWorkRequestPending);
        DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE from %"PRIx64" (pending)\n", context->location);
        // If this is a non-affinitized request, we won't respond until we do get work
        if(discardAffinity || derived->enforceAffinity) {
            // If the receiver is an XE, put it to sleep
            u64 agentId = AGENT_FROM_ID(context->location);
            if ((agentId >= ID_AGENT_XE0) && (agentId <= ID_AGENT_XE7)) {
                //FIXME: enable this with #861
                //DPRINTF(DEBUG_LVL_INFO, "XE %"PRIx64" put to sleep\n", context->location);
                //hal_sleep(agentId);
                derived->pendingXeCount++;
            }
            derived->inPendingCount++;
            ceContext->inWorkRequestPending = true;
            ASSERT(ceContext->msgId == 0 && hints != NULL);
            ocrPolicyMsg_t *message = (ocrPolicyMsg_t*)hints;
            ceContext->msgId = message->msgId; //HACK: Store the msgId for future response
        } else {
            // We will respond that we don't have work (affinitized). This will cause the requester to request again
            DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE affinitized from %"PRIx64" returned no work\n", context->location);
            // Just to be safe and clean
            taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid = NULL_GUID;
            taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.metaDataPtr = NULL;
            return 0;
        }
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE from %"PRIx64" (found)\n", context->location);
    }
    DPRINTF(DEBUG_LVL_VVERB, "TAKE_WORK_INVOKE DONE from %"PRIx64" \n", context->location);
    return retVal;
}

u8 ceSchedulerHeuristicGetWorkInvokeAff(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ASSERT(context);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        return ceSchedulerHeuristicWorkEdtUserInvokeAff(self, context, opArgs, hints);
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerHeuristicGetWorkSimulateAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 handleEmptyResponseAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context) {
    // We should only hit this if we made an affinitized request to a CE. The asserts check this
    DPRINTF(DEBUG_LVL_VVERB, "Got an empty work request response from 0x%"PRIx64"\n", context->location);
    ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
    ASSERT(AGENT_FROM_ID(context->location) == ID_AGENT_CE);
    ASSERT(ceContext->outWorkRequestPending == AFF_REQUEST);
    ceContext->outWorkRequestPending = AFF_REQUEST_FAIL;
    // We now say that we are available for non-affinitized work requests
    derived->outWorkVictimsAvailable[1]++;
    return 0;
}

static u8 ceSchedulerHeuristicNotifyEdtReadyInvokeAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
    // We should definitely not have EDTs becoming ready after the shutdown is called
    // Maybe if the shutdown is not the last EDT but even then...
    ASSERT(!derived->shutdownMode);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    ocrFatGuid_t fguid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid;

    DPRINTF(DEBUG_LVL_INFO, "Location 0x%"PRIx64" GIVE WORK "GUIDF"\n", (u64)opArgs->location,
        GUIDA(fguid.guid));

    if (ocrGuidIsNull(fguid.guid)) {
        return handleEmptyResponseAff(self, context);
    }

    // Check if this notification is from a XE or a CE
    bool fromXE = true;
    if(AGENT_FROM_ID(opArgs->location) == ID_AGENT_CE) {
        DPRINTF(DEBUG_LVL_VERB, "EDT ready notification from 0x%"PRIx64" (CE)\n", (u64)opArgs->location);
        fromXE = false;
    } else {
        DPRINTF(DEBUG_LVL_VERB, "EDT ready notification from 0x%"PRIx64" (XE)\n", (u64)opArgs->location);
    }
    ocrTask_t *task = (ocrTask_t*)fguid.metaDataPtr;
    ASSERT(task);

    // The general algorithm here is as follows:
    //  - check the affinity of the work
    //    - if no affinity:
    //      - if coming from a CE, push in a round-robin fashion to a XE
    //      - if coming from a XE, push to that XE's queue
    //    - if affinity:
    //      - if coming from a CE and for my affinity, push in a round-robin fashion to a XE
    //      - if coming from a CE but not for me, push to neighbor CE's queue
    //      - if coming from a XE and for my affinity, push to that XE's queue
    //      - if coming from a XE but not for me, push to neighbor CE's
    ocrSchedulerHeuristicContext_t *insertContext = context;
    u64 affinitySlot = ((u64)-1);
    ocrHint_t edtHint;
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    RESULT_ASSERT(ocrGetHint(task->guid, &edtHint), ==, 0);
    bool isHintedLocation = false;
    if(ocrGetHintValue(&edtHint, OCR_HINT_EDT_AFFINITY, &affinitySlot) == 0) {
        isHintedLocation = true;
        // There is an affinity, check the source and the affinity destination
        ocrLocation_t myLoc = pd->myLocation;
        ocrLocation_t affLoc;
        ocrGuid_t affGuid;
#if GUID_BIT_COUNT == 64
        affGuid.guid = affinitySlot;
#elif GUID_BIT_COUNT == 128
        affGuid.upper = 0ULL;
        affGuid.lower = hintValue;
#endif
        ASSERT(!ocrGuidIsNull(affGuid));
        affinityToLocation(&affLoc, affGuid);
        u32 mySocketAndBeyond, affSocketAndBeyond, myCluster, affCluster, myBlock, affBlock;
        mySocketAndBeyond = myLoc >> MAP_SOCKET_SHIFT;
        affSocketAndBeyond = affLoc >> MAP_SOCKET_SHIFT;
        myCluster = CLUSTER_FROM_ID(myLoc);
        affCluster = CLUSTER_FROM_ID(affLoc);
        myBlock = BLOCK_FROM_ID(myLoc);
        affBlock = BLOCK_FROM_ID(affLoc);

        // The neighbors are all other CEs in my cluster and if not in my cluster, I should
        // send it to CE 0 so it can send it to the proper cluster which will then forward to
        // the proper block.
        DPRINTF(DEBUG_LVL_VVERB, "EDT is ready; affinitized to 0x%"PRIx64" and my location is 0x%"PRIx64"\n",
                affLoc, myLoc);
        if(mySocketAndBeyond == affSocketAndBeyond &&
            myCluster == affCluster &&
            myBlock == affBlock) {
            // This is "my block"
            if(fromXE) {
                // Do nothing, this will use the context we got on entry
                DPRINTF(DEBUG_LVL_INFO, "EDT local to XE (on XE 0x%"PRIx64")\n", context->location);
            } else {
                // Figure out the next context to use in a RR fashion
                insertContext = self->contexts[derived->rrXE];
                DPRINTF(DEBUG_LVL_INFO, "EDT distributed locally to block (on XE 0x%"PRIx64")\n", insertContext->location);
                derived->rrXE = (derived->rrXE + 1) % derived->xeCount;
            }
        } else {
            // We have to put it somewhere else
            // If this is not true, it means we are going outside of a socket which is not
            // yet supported.
            ASSERT(mySocketAndBeyond == affSocketAndBeyond);
            ocrLocation_t affinityLoc;
            if(myCluster == affCluster) {
                // We are in the same cluster so we know that we have that we have a queue for that neighbor
                affinityLoc = MAKE_CORE_ID(0, 0, 0, affCluster, affBlock, ID_AGENT_CE);
            } else {
                // Here we need to send to our block 0 (which can be ourself)
                affinityLoc = MAKE_CORE_ID(0, 0, 0, myCluster, 0, ID_AGENT_CE);
            }
            bool found __attribute__((unused)) = false;
            int i;
            for(i = derived->xeCount; i < self->contextCount; ++i) {
                if(self->contexts[i]->location == affinityLoc) {
                    insertContext = self->contexts[i];
                    found = true;
                    break;
                }
            }
            // If this fails, it means we somehow don't have the right neighbors
            ASSERT(found);
            DPRINTF(DEBUG_LVL_INFO, "EDT will be affinitized to remote 0x%"PRIx64" for final affinity 0x%"PRIx64"\n",
                insertContext->location, affinityLoc);
        }
    } else if (ocrGetHintValue(&edtHint, OCR_HINT_EDT_SLOT_MAX_ACCESS, &affinitySlot) == 0) {
        // If there is no affinity information directly, we see if we can get something from the data-block
        ASSERT(affinitySlot < task->depc);
        ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //TODO:This is temporary until we get proper introspection support
        ocrEdtDep_t *depv = hcTask->resolvedDeps;
        ocrGuid_t dbGuid = depv[affinitySlot].guid;
        ocrDataBlock_t *db = NULL;
        pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dbGuid, (u64*)(&(db)), NULL, MD_LOCAL, NULL);
        ASSERT(db);
        u64 dbMemAffinity = ((u64)-1);
        ocrHint_t dbHint;
        ocrHintInit(&dbHint, OCR_HINT_DB_T);
        RESULT_ASSERT(ocrGetHint(dbGuid, &dbHint), ==, 0);
        if (ocrGetHintValue(&dbHint, OCR_HINT_DB_AFFINITY, &dbMemAffinity) == 0) {
            isHintedLocation = true;
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
            ASSERT(found);
        }
    }

    if(derived->enforceAffinity && !isHintedLocation) {
        // We select a round robin-context so that we spread work out a bit
        insertContext = self->contexts[derived->rrInsert];
        DPRINTF(DEBUG_LVL_INFO, "Non-affinitized work RR to location 0x%"PRIx64")\n", insertContext->location);
        derived->rrInsert = (derived->rrInsert + 1) % self->contextCount;
    }

    ocrSchedulerHeuristicContextCeAff_t *ceInsertContext = (ocrSchedulerHeuristicContextCeAff_t*)insertContext;
    ocrSchedulerObject_t *insertSchedObj = ceInsertContext->mySchedulerObject;
    ASSERT(insertSchedObj);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = fguid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrSchedulerObjectFactory_t *fact = pd->schedulerObjectFactories[insertSchedObj->fctId];
    RESULT_ASSERT(fact->fcts.insert(fact, insertSchedObj, &edtObj, NULL, (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL)), ==, 0);
    derived->workCount++;
    if (AGENT_FROM_ID(context->location) == ID_AGENT_CE) {
        // This means the *source* was a CE (as opposed to an XE) so this means that we had requested work
        // from it and now it answered us.
        ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
        // Assert that we had actually requested work
        ASSERT(ceContext->outWorkRequestPending == AFF_REQUEST ||
            ceContext->outWorkRequestPending == NO_AFF_REQUEST);
        // Update the outWorkRequestPending properly as well as outWorkVictimsAvailable
        // Update: we no longer have a request pending
        // Update: we can make a new affinitized request to the node (this is whether or not
        // our initial request was affinitized).
        ceContext->outWorkRequestPending = NO_REQUEST;
        derived->outWorkVictimsAvailable[0]++;

        // If we have non-affinitized things pending, we reset them to look for affinitized stuff
        if(derived->outWorkVictimsAvailable[1]) {
            // Go over the contexts
            u32 i = 0;
            for(i = 0; i < self->contextCount; ++i) {
                ocrSchedulerHeuristicContextCeAff_t *t = (ocrSchedulerHeuristicContextCeAff_t*)(self->contexts[i]);
                if(t->outWorkRequestPending == AFF_REQUEST_FAIL) {
                    // This means that we are supposed to request for non-affinitized stuff later
                    t->outWorkRequestPending = NO_REQUEST;
                    --derived->outWorkVictimsAvailable[1];
                    ++derived->outWorkVictimsAvailable[0];
                }
            }
            // We should have switched everyone
            ASSERT(derived->outWorkVictimsAvailable[1] == 0);
        }
    }
    DPRINTF(DEBUG_LVL_VVERB, "GIVEN WORK to context %"PRIx64"\n", insertContext->location);
    return 0;
}

u8 ceSchedulerHeuristicNotifyInvokeAff(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ASSERT(context);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_EDT_READY:
        return ceSchedulerHeuristicNotifyEdtReadyInvokeAff(self, context, opArgs, hints);
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
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 ceSchedulerHeuristicNotifySimulateAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicTransactInvokeAff(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicTransactSimulateAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicAnalyzeInvokeAff(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 ceSchedulerHeuristicAnalyzeSimulateAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 makeWorkRequestAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, bool isBlocking) {
    u8 returnCode = 0;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    DPRINTF(DEBUG_LVL_VVERB, "MAKE_WORK_REQUEST to %"PRIx64"\n", context->location);
    ASSERT(AGENT_FROM_ID(context->location) == ID_AGENT_CE);
    ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
    ASSERT(ceContext->outWorkRequestPending == NO_REQUEST ||
        ceContext->outWorkRequestPending == AFF_REQUEST_FAIL);
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
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).discardAffinity = (ceContext->outWorkRequestPending == AFF_REQUEST_FAIL);
#ifdef OCR_HACK_DB_MOVE
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).preOpsCount = 0;
#endif
    PD_MSG_FIELD_I(properties) = 0;

    if (isBlocking) {
        returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
        ASSERT(returnCode == 0);
    } else {
        returnCode = pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
    }
#undef PD_MSG
#undef PD_TYPE
    if (returnCode == 0) {
        ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
        ASSERT(ceContext->outWorkRequestPending == AFF_REQUEST_FAIL ||
            ceContext->outWorkRequestPending == NO_REQUEST);
        if(ceContext->outWorkRequestPending == AFF_REQUEST_FAIL) {
            ceContext->outWorkRequestPending = NO_AFF_REQUEST;
            ASSERT(derived->outWorkVictimsAvailable[1]);
            --derived->outWorkVictimsAvailable[1];
        } else {
            ceContext->outWorkRequestPending = AFF_REQUEST;
            ASSERT(derived->outWorkVictimsAvailable[0]);
            --derived->outWorkVictimsAvailable[0];
        }
        DPRINTF(DEBUG_LVL_VVERB, "MAKE_WORK_REQUEST SUCCESS to %"PRIx64"\n", context->location);
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "MAKE WORK REQUEST ERROR CODE: %"PRIu32"\n", returnCode);
    }
    return returnCode;
}

static u8 respondWorkRequestAff(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrFatGuid_t *fguid) {
    DPRINTF(DEBUG_LVL_VVERB, "RESPOND_WORK_REQUEST to %"PRIx64"\n", context->location);
    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
    ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
    ASSERT(ceContext->inWorkRequestPending);
    ASSERT(fguid);
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
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).discardAffinity = false; // Irrelevant on response but being clean
#ifdef OCR_HACK_DB_MOVE
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).preOpsCount = 0;
#endif
    PD_MSG_FIELD_I(properties) = 0;
    if (ceMessage) {
        RESULT_ASSERT(pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0), ==, 0);
    } else {
        RESULT_ASSERT(pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0), ==, 0);
    }
#undef PD_MSG
#undef PD_TYPE
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
u8 ceSchedulerHeuristicUpdateAff(ocrSchedulerHeuristic_t *self, u32 properties) {
    ocrSchedulerHeuristicCeAff_t *derived = (ocrSchedulerHeuristicCeAff_t*)self;
    ocrPolicyDomain_t *pd = self->scheduler->pd;
    u32 i;
    switch(properties) {
    case OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_IDLE: {
            if (derived->shutdownMode)
                break; //We are shutting down, no more processing

            DPRINTF(DEBUG_LVL_VVERB, "IDLE [work: %"PRIu64" pending: (%"PRIu32", %"PRIu32") victims: aff:%"PRIu32"; nonaff:%"PRIu32"]\n", derived->workCount, derived->inPendingCount, derived->pendingXeCount, derived->outWorkVictimsAvailable[0],
                derived->outWorkVictimsAvailable[1]);
            //High-level logic:
            //First check if any remote agent (XE or CE) is waiting for a response from this CE.
            //If nobody is waiting, then we return back to caller.
            //If anybody is waiting, we try to make progress based on the requester.
            //If request is from a CE/XE, then check if work is available to service that request.
            //If not, then try to send out work requests to as many neighbors as the number of pending XEs.
            // Finally, if we are block 0, redistribute work that we may have received from other CEs but that are not
            // for us.

            if (derived->inPendingCount == 0)
                break; //Nobody is waiting. Great!... break out and return.
            ASSERT(derived->inPendingCount <= self->contextCount);

            //If we have work, respond to the pending requests...
            //We satisfy the XEs requests first before giving work to CEs
            ocrFatGuid_t fguid;
            for (i = 0; i < self->contextCount && derived->workCount != 0; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
                if (ceContext->inWorkRequestPending &&                   /*We have a pending context, and ...*/
                     ((derived->enforceAffinity) ||                      /*either we enforce affinity (so there is no stealing risk) */
                     (derived->workCount > derived->pendingXeCount) ||   /*or we have enough work to serve anyone, ...*/
                     (AGENT_FROM_ID(context->location) != ID_AGENT_CE))) /*or we have very little and we want to serve the XEs first*/
                {
                    fguid.guid = NULL_GUID;
                    fguid.metaDataPtr = NULL;
                    // If things are pending, this means this is the second request
                    // If enforce affinity, we do not discard affinity. Otherwise, we do.
                    if (ceWorkStealingGetAff(self, context, &fguid, !derived->enforceAffinity) == 0) {
                        respondWorkRequestAff(self, context, &fguid);
                    }
                }
            }

            // Now we start the stealing. We will initially steal in an affinitized manner before moving on
            // to a non-affinitized manner.
            u32 stealIdx = 0; // Affinitized manner
            if(derived->outWorkVictimsAvailable[stealIdx] == 0)
                stealIdx = 1; // Non-affinitized manner
            if (derived->workCount == 0 && derived->inPendingCount != 0 && derived->outWorkVictimsAvailable[stealIdx] != 0) {
                ASSERT(derived->outWorkVictimsAvailable[stealIdx] <= pd->neighborCount);

                //No work left... some are still waiting... so, try to find work from neighbors
                //If work request fails, no problem, try later
                //If work request fails due to DEAD neighbor, make sure we don't try to send any more requests to that neighbor
                for (i = 0; i < self->contextCount && derived->outWorkVictimsAvailable[stealIdx] != 0; ++i) {
                    ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                    ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
                    if (ceContext->canAcceptWorkRequest && ceContext->outWorkRequestPending == (stealIdx==0?NO_REQUEST:AFF_REQUEST_FAIL)) {
                        u8 returnCode = makeWorkRequestAff(self, context, false);
                        if (returnCode != 0) {
                            if (returnCode == 2) { //Location is dead
                                ASSERT(context->location != pd->parentLocation); //Make sure parent is not dead
                                ceContext->canAcceptWorkRequest = false;
                                --derived->outWorkVictimsAvailable[stealIdx];
                            }
                        } else {
                            ASSERT(ceContext->outWorkRequestPending != NO_REQUEST &&
                                ceContext->outWorkRequestPending != AFF_REQUEST_FAIL);
                            ASSERT(derived->outWorkVictimsAvailable[stealIdx] <= pd->neighborCount);
                        }
                    }
                }
            }
        }
        break;
    case OCR_SCHEDULER_HEURISTIC_UPDATE_PROP_SHUTDOWN: {
            ASSERT(!derived->shutdownMode);
            derived->shutdownMode = true;

            ocrFatGuid_t fguid;
            fguid.guid = NULL_GUID;
            fguid.metaDataPtr = NULL;

            //Ensure shutdown response is sent to all pending XE children... (they are sitting around doing nothing)
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
                if ((ceContext->inWorkRequestPending) && (ceContext->isChild)) {
                    //respondShutdown(self, context, true);
                    respondWorkRequestAff(self, context, &fguid);
                }
            }

            // No need to inform CEs as the PD takes care of this.

#if 0
            //Finally try to shutdown to non-pending CE children as well
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = self->contexts[i];
                ocrSchedulerHeuristicContextCeAff_t *ceContext = (ocrSchedulerHeuristicContextCeAff_t*)context;
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

void destructSchedulerHeuristicFactoryCeAff(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryCeAff(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryCeAff_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicCeAff;
    base->destruct = &destructSchedulerHeuristicFactoryCeAff;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), ceSchedulerHeuristicSwitchRunlevelAff);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), ceSchedulerHeuristicDestructAff);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), ceSchedulerHeuristicUpdateAff);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t),
        ceSchedulerHeuristicGetContextAff);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicGetWorkInvokeAff);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicGetWorkSimulateAff);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicNotifyInvokeAff);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicNotifySimulateAff);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicTransactInvokeAff);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicTransactSimulateAff);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicAnalyzeInvokeAff);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(
        ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*),
        ceSchedulerHeuristicAnalyzeSimulateAff);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_CE */
