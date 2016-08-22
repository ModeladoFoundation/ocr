/*
* This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_PLACEMENT_AFFINITY

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "scheduler-heuristic/placement/placement-affinity-scheduler-heuristic.h"

#include "extensions/ocr-hints.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

/******************************************************/
/* OCR PLACEMENT AFFINITY SCHEDULER_HEURISTIC         */
/******************************************************/

#ifdef LOAD_BALANCING_TEST
#include "extensions/ocr-affinity.h"
#endif

ocrSchedulerHeuristic_t* newSchedulerHeuristicPlacementAffinity(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicPlacementAffinity_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    return self;
}

static u8 placerAffinitySchedHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        break;
    }
    case RL_MEMORY_OK:
    {
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            ocrSchedulerHeuristicPlacementAffinity_t * dself  = (ocrSchedulerHeuristicPlacementAffinity_t *) self;
            dself->lock = INIT_LOCK;
            dself->edtLastPlacementIndex = 0;
            // Following are cached from the PD
            dself->myLocation = PD->myLocation;
            dself->platformModel = NULL; // Not avail at this RL
        }
        break;
    }
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
    {
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            ocrSchedulerHeuristicPlacementAffinity_t * dself  = (ocrSchedulerHeuristicPlacementAffinity_t *) self;
            // Following are cached from the PD
            ASSERT(PD->platformModel != NULL);
            dself->platformModel = (ocrPlatformModelAffinity_t *) PD->platformModel;
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

static void placerAffinitySchedHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

static u8 placerAffinitySchedHeuristicUpdate(ocrSchedulerHeuristic_t *self, u32 properties) {
    return OCR_ENOTSUP;
}

static ocrSchedulerHeuristicContext_t* placerAffinitySchedHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    ASSERT(loc == self->scheduler->pd->myLocation);
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    return self->contexts[worker->id];
}

static u8 placerAffinitySchedHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 placerAffinitySchedHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 placerAffinitySchedHeuristicNotifyProcessMsgInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ASSERT(notifyArgs->kind == OCR_SCHED_NOTIFY_PRE_PROCESS_MSG);
    ocrPolicyMsg_t * msg = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG).msg;
    ocrSchedulerHeuristicPlacementAffinity_t * dself = (ocrSchedulerHeuristicPlacementAffinity_t *) self;
    ocrPlatformModelAffinity_t * model = dself->platformModel; // Cached from the PD initialization
    if ((model != NULL) && (msg->srcLocation == msg->destLocation)) {
        ASSERT(msg->srcLocation == dself->myLocation); // and message was local
        // Check if we need to place the DB/EDTs
        u64 msgType = (msg->type & PD_MSG_TYPE_ONLY);
        bool doAutoPlace = false;
        switch(msgType) {
            case PD_MSG_WORK_CREATE:
            {
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                if (PD_MSG_FIELD_I(workType) == EDT_USER_WORKTYPE) {
                    doAutoPlace = true;
                    if (PD_MSG_FIELD_I(hint) != NULL_HINT) {
                        ocrHint_t *hint = PD_MSG_FIELD_I(hint);
                        u64 hintValue = 0ULL;
                        if ((ocrGetHintValue(hint, OCR_HINT_EDT_AFFINITY, &hintValue) == 0) && (hintValue != 0)) {
                            ocrGuid_t affGuid;
#if GUID_BIT_COUNT == 64
                            affGuid.guid = hintValue;
#elif GUID_BIT_COUNT == 128
                            affGuid.upper = 0ULL;
                            affGuid.lower = hintValue;
#endif
                            ASSERT(!ocrGuidIsNull(affGuid));
                            affinityToLocation(&(msg->destLocation), affGuid);
                            doAutoPlace = false;
                        }
                    }
#ifdef LOAD_BALANCING_TEST
                    else { // Let the load balancing take the decision when there's no hints
                        doAutoPlace = false;
                    }
#endif
                } else { // For runtime EDTs, always local
                    doAutoPlace = false;
                }
#undef PD_MSG
#undef PD_TYPE
            break;
            }
            case PD_MSG_DB_CREATE:
            {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
                // For now a DB is always created where the current EDT executes unless
                // it has an affinity specified (i.e. no auto-placement)
                if (PD_MSG_FIELD_I(hint) != NULL_HINT) {
                    ocrHint_t *hint = PD_MSG_FIELD_I(hint);
                    u64 hintValue = 0ULL;
                    if ((ocrGetHintValue(hint, OCR_HINT_DB_AFFINITY, &hintValue) == 0) && (hintValue != 0)) {
                        ocrGuid_t affGuid;
#if GUID_BIT_COUNT == 64
                        affGuid.guid = hintValue;
#elif GUID_BIT_COUNT == 128
                        affGuid.upper = 0ULL;
                        affGuid.lower = hintValue;
#endif
                        ASSERT(!ocrGuidIsNull(affGuid));
                        affinityToLocation(&(msg->destLocation), affGuid);
                        return 0;
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
        // Auto placement
        if (doAutoPlace) {
            hal_lock(&(dself->lock));
            u32 placementIndex = dself->edtLastPlacementIndex;
            ocrGuid_t pdLocAffinity = model->pdLocAffinities[placementIndex];
            dself->edtLastPlacementIndex++;
            if (dself->edtLastPlacementIndex == model->pdLocAffinitiesSize) {
                dself->edtLastPlacementIndex = 0;
            }
            hal_unlock(&(dself->lock));
            affinityToLocation(&(msg->destLocation), pdLocAffinity);
            DPRINTF(DEBUG_LVL_VVERB,"Auto-Placement for msg %p, type 0x%"PRIx64", at location %"PRId32"\n",
                    msg, (msg->type & PD_MSG_TYPE_ONLY), (u32)placementIndex);
        }
    }

    return 0;
}

#ifdef LOAD_BALANCING_TEST

static void scheduleEdtMovement(ocrPolicyDomain_t * pd, ocrFatGuid_t edtFGuid, ocrLocation_t srcLocation, ocrLocation_t dstLocation) {
    DPRINTF(DEBUG_LVL_VVERB, "EDT-MV Scheduler posts MD_MOVE call\n");
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
    msg.type = PD_MSG_GUID_METADATA_CLONE | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(guid) = edtFGuid;
    PD_MSG_FIELD_I(type) = MD_MOVE;
    PD_MSG_FIELD_I(dstLocation) = dstLocation;
    pd->fcts.processMessage(pd, &msg, false);
    // Warning: after this call we're potentially concurrent with the MD being registered on the GP
#undef PD_MSG
#undef PD_TYPE
}

//TODO-MD need micro-tasking here to avoid redistributing RT work
extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static u8 placerAffinitySchedulerHeuristicNotifyEdtSatisfiedInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrSchedulerObject_t edtObj;
    edtObj.guid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_SATISFIED).guid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrPolicyDomain_t * pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
#ifdef OCR_ASSERT
    ocrLocation_t edtLoc;
    pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], edtObj.guid.guid, &edtLoc);
    ASSERT(edtLoc == pd->myLocation);
#endif
    ASSERT(edtObj.guid.metaDataPtr != NULL);
    ocrTask_t * edt = ((ocrTask_t *)edtObj.guid.metaDataPtr);
    // Do not try to move the EDT if there's already a hint placement.
    ocrHint_t edtHints;
    ocrHintInit(&edtHints, OCR_HINT_EDT_T);
    u8 noHint = pd->taskFactories[0]->fcts.getHint(edt, &edtHints);
    u64 edtAff;
    u8 noPlcHint = noHint || (!noHint && ocrGetHintValue(&edtHints, OCR_HINT_EDT_AFFINITY, &edtAff));
    //TODO-MT need micro-tasking activated here to avoid redistributing RT work
    bool doMove = ((edt->funcPtr != &processRequestEdt) && noPlcHint);
    DPRINTF(DEBUG_LVL_VVERB, "[LB] workEdt=%d doMove=%d noHint=%d noPlcHint=%d\n", (edt->funcPtr != &processRequestEdt), (int) doMove, (int) noHint, (int) noPlcHint);
    if (doMove) {
        ocrSchedulerHeuristicPlacementAffinity_t * dself = (ocrSchedulerHeuristicPlacementAffinity_t *) self;
        ocrPlatformModelAffinity_t * model = dself->platformModel; // Cached from the PD initialization
        hal_lock(&(dself->lock)); // TODO this is concurrent with placement at the WORK creation time
        u32 placementIndex = dself->edtLastPlacementIndex;
        ocrGuid_t pdLocAffinity = model->pdLocAffinities[placementIndex];
        dself->edtLastPlacementIndex++;
        if (dself->edtLastPlacementIndex == model->pdLocAffinitiesSize) {
            dself->edtLastPlacementIndex = 0;
        }
        hal_unlock(&(dself->lock));
        ASSERT(pd != NULL);
        if(noHint) {
            ocrHintInit(&edtHints, OCR_HINT_EDT_T);
        }
        ocrLocation_t dstLocation = affinityToLocation(pdLocAffinity);
        ocrSetHintValue(&edtHints, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(pdLocAffinity));
        RESULT_ASSERT(pd->taskFactories[0]->fcts.setHint(edt, &edtHints), ==, 0);
#ifdef OCR_ASSERT
        RESULT_ASSERT(pd->taskFactories[0]->fcts.getHint(edt, &edtHints), ==, 0);
        u8 hasHint = !ocrGetHintValue(&edtHints, OCR_HINT_EDT_AFFINITY, &edtAff);
        ASSERT(hasHint);
#endif
        if (dstLocation != pd->myLocation) {
            ASSERT((((u64)pd->myLocation) == 0) || (((u64)pd->myLocation) == 1));
            DPRINTF(DEBUG_LVL_VVERB,"[LB] Moving EDT "GUIDF" from PD[%"PRIu64"] to PD[%"PRIu64"]\n",
                    edtObj.guid.guid, (u64) pd->myLocation, (u64) dstLocation);
            scheduleEdtMovement(pd, edtObj.guid, pd->myLocation, dstLocation);
            return 0;
        }
    }
    return OCR_ENOP;
}
#endif

static u8 placerAffinitySchedHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
#ifdef LOAD_BALANCING_TEST
    // Only alter EDT placement AFTER their creation and dependences are all resolved to DBs
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
        return placerAffinitySchedulerHeuristicNotifyEdtSatisfiedInvoke(self, /*context*/ NULL, opArgs, hints);
#endif
    case OCR_SCHED_NOTIFY_PRE_PROCESS_MSG:
        return placerAffinitySchedHeuristicNotifyProcessMsgInvoke(self, /*context*/ NULL, opArgs, hints);
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
            PD_MSG_FIELD_I(currentEdt) = PD_MSG_FIELD_I(guid);
            PD_MSG_FIELD_I(properties) = 0;
            ASSERT(pd->fcts.processMessage(pd, &msg, false) == 0);
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    // Notifies ignored by this heuristic
#ifndef LOAD_BALANCING_TEST
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
#endif
    case OCR_SCHED_NOTIFY_DB_CREATE:
        return OCR_ENOP;
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

static u8 placerAffinitySchedHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 placerAffinitySchedHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 placerAffinitySchedHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 placerAffinitySchedHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 placerAffinitySchedHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR PLACEMENT AFFINITY SCHEDULER_HEURISTIC FACTORY */
/******************************************************/

static void destructSchedulerHeuristicFactoryPlacementAffinity(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryPlacementAffinity(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryPlacementAffinity_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicPlacementAffinity;
    base->destruct = &destructSchedulerHeuristicFactoryPlacementAffinity;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), placerAffinitySchedHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), placerAffinitySchedHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), placerAffinitySchedHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), placerAffinitySchedHeuristicGetContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), placerAffinitySchedHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_PLACEMENT_AFFINITY */
