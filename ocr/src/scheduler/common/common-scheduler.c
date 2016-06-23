/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_COMMON

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "scheduler/common/common-scheduler.h"
// BUG #793 Probably need to make this not HC-specific
#include "scheduler/hc/scheduler-blocking-support.h"

#include "ocr-policy-domain-tasks.h"

#define DEBUG_TYPE SCHEDULER

// This scheduler is expecting at least two heuristics to be present
// - Heuristic zero is able to handle notify for tasks
// - Heuristic one makes placement decisions if first heuristic failed
// - Heuristic two is able to handle communications
// For now we still keep a "master" heuristic around. It currently defaults
// to either the heuristic that has the master flag set to true in the CFG file
// or heuristic 0, which must be the computation heuristic by contract

#define COMP_HEURISTIC_ID 0
#define PLACEMENT_HEURISTIC_ID 1
#define COMM_HEURISTIC_ID 2

/******************************************************/
/* OCR-COMMON SCHEDULER                               */
/******************************************************/

// Single ascii to integer
static u32 sglAtoi(char c) {
    return c - '0';
}

// Parses something like "comp0:plc2:comm1" in any order. String must be well formed
static void parseConfigStr(ocrSchedulerHeuristic_t ** pdSchedulerHeuristics, ocrSchedulerHeuristic_t ** schedulerHeuristics, char * cfgStr) {
    int r = 0;
    int l = 0;
    if (cfgStr == NULL)
        return;
    int lg = ocrStrlen(cfgStr);
    while (r < lg) {
        while (!ocrIsDigit((u8) cfgStr[r])) {
            r++;
        }
        u8 id = 0;
        if (ocrStrncmp((u8*)"comp", (u8*)&cfgStr[l], (r-l)) == 0) {
            DPRINTF(DEBUG_LVL_INFO, "Assigning heuristic %"PRId32" to slot COMP_HEURISTIC_ID\n", sglAtoi(cfgStr[r]));
            id = COMP_HEURISTIC_ID;
        } else if (ocrStrncmp((u8*)"comm", (u8*)&cfgStr[l], (r-l)) == 0) {
            DPRINTF(DEBUG_LVL_INFO, "Assigning heuristic %"PRId32" to slot COMM_HEURISTIC_ID\n", sglAtoi(cfgStr[r]));
            id = COMM_HEURISTIC_ID;
        } else if (ocrStrncmp((u8*)"plc", (u8*)&cfgStr[l], (r-l)) == 0) {
            DPRINTF(DEBUG_LVL_INFO, "Assigning heuristic %"PRId32" to slot PLACEMENT_HEURISTIC_ID\n", sglAtoi(cfgStr[r]));
            id = PLACEMENT_HEURISTIC_ID;
        } else {
            cfgStr[r] = '\0';
            DPRINTF(DEBUG_LVL_WARN, "error: Unrecognized heuristic %"PRId32"\n", (s32)cfgStr[l]);
            ASSERT(false);
        }
        schedulerHeuristics[id] = pdSchedulerHeuristics[sglAtoi(cfgStr[r])];
        r+=2; //skip digit and delimiter
        l = r;
    }
}

void commonSchedulerDestruct(ocrScheduler_t * self) {
    u64 i;
    // Destruct the root scheduler object
    ocrSchedulerObjectFactory_t *rootFact = (ocrSchedulerObjectFactory_t*)self->pd->schedulerObjectFactories[self->rootObj->fctId];
    rootFact->fcts.destroy(rootFact, self->rootObj);

    //scheduler heuristics
    u64 schedulerHeuristicCount = self->schedulerHeuristicCount;
    for(i = 0; i < schedulerHeuristicCount; ++i) {
        self->schedulerHeuristics[i]->fcts.destruct(self->schedulerHeuristics[i]);
    }

    runtimeChunkFree((u64)(self->schedulerHeuristics), PERSISTENT_CHUNK);
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 commonSchedulerSwitchRunlevel(ocrScheduler_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                             phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    u64 i;
    if(runlevel == RL_CONFIG_PARSE && (properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_CONFIG_PARSE, phase)) {
        // First transition, setup some backpointers
        // By contract, this scheduler implementation requires two heuristics
        // Note: Improvement could be to check for heuristic capabilities to make the contract more flexible
        bool masterFound = false;
        for(i = 0; i < self->schedulerHeuristicCount; ++i) {
            ocrSchedulerHeuristic_t *heuristic = self->schedulerHeuristics[i];
            heuristic->scheduler = self;
            if (heuristic->isMaster) {
                self->masterHeuristicId = i;
                ASSERT(!masterFound);
                masterFound = true;
            }
        }
        if (!masterFound) {
            //If master is not specified, choose the first heuristic to be master
            self->masterHeuristicId = 0;
            self->schedulerHeuristics[0]->isMaster = true;
        }
    }

    if(properties & RL_BRING_UP) {
        //TODO for now this root object is only for COMP_HEURISTIC_ID. We need to add a new root type to cover 2*deques + ioboxes
        // Take care of all other sub-objects
        ocrSchedulerObjectFactory_t *rootFact = (ocrSchedulerObjectFactory_t*)PD->schedulerObjectFactories[self->rootObj->fctId];
        toReturn |= rootFact->fcts.switchRunlevel(self->rootObj, PD, runlevel, phase, properties, NULL, 0);
        // Do not re-order: Scheduler object root should be brought up before heuristics
        for(i = 0; i < self->schedulerHeuristicCount; ++i) {
            toReturn |= self->schedulerHeuristics[i]->fcts.switchRunlevel(
                self->schedulerHeuristics[i], PD, runlevel, phase, properties, NULL, 0);
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
        ocrSchedulerObjectFactory_t *rootFact = (ocrSchedulerObjectFactory_t*)PD->schedulerObjectFactories[self->rootObj->fctId];
        toReturn |= rootFact->fcts.switchRunlevel(self->rootObj, PD, runlevel, phase, properties, NULL, 0);

        for(i = 0; i < self->schedulerHeuristicCount; ++i) {
            toReturn |= self->schedulerHeuristics[i]->fcts.switchRunlevel(
                self->schedulerHeuristics[i], PD, runlevel, phase, properties, NULL, 0);
        }
    }
    return toReturn;
}

u8 commonSchedulerTakeEdt (ocrScheduler_t *self, u32 *count, ocrFatGuid_t *edts) {
    ASSERT(false && "deprecated scheduler interface");
    return OCR_ENOTSUP;
}

u8 commonSchedulerGiveEdt (ocrScheduler_t* base, u32* count, ocrFatGuid_t* edts) {
    ASSERT(false && "deprecated scheduler interface");
    return OCR_ENOTSUP;
}

u8 commonSchedulerTakeComm(ocrScheduler_t *self, u32* count, ocrFatGuid_t* handlers, u32 properties) {
    ASSERT(false && "deprecated scheduler interface");
    return OCR_ENOTSUP;
}

u8 commonSchedulerGiveComm(ocrScheduler_t *self, u32* count, ocrFatGuid_t* handlers, u32 properties) {
    ASSERT(false && "deprecated scheduler interface");
    return OCR_ENOTSUP;
}

// BUG #793 deprecate this api in favor of update
u8 commonSchedulerMonitorProgress(ocrScheduler_t *self, ocrMonitorProgress_t type, void * monitoree) {
#ifdef ENABLE_SCHEDULER_BLOCKING_SUPPORT
    // Current implementation assumes the worker is blocked.
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    handleWorkerNotProgressing(worker);
#endif
    return 0;
}


u8 commonSchedulerGetWorkInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) self;
    // Dispatch notify to the correct scheduler
    ocrSchedulerHeuristic_t *schedulerHeuristic;
    ocrSchedulerOpWorkArgs_t * notifyArgs = (ocrSchedulerOpWorkArgs_t *) opArgs;
    if (notifyArgs->kind == OCR_SCHED_WORK_COMM) {
        schedulerHeuristic = dself->schedulerHeuristics[COMM_HEURISTIC_ID];
    } else {
        schedulerHeuristic = dself->schedulerHeuristics[COMP_HEURISTIC_ID];
    }
    return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke(schedulerHeuristic, opArgs, hints);
}

u8 commonSchedulerNotifyInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) self;
    // Dispatch notify to the correct scheduler
    ocrSchedulerHeuristic_t * schedulerHeuristic;
    ocrSchedulerOpNotifyArgs_t * notifyArgs = (ocrSchedulerOpNotifyArgs_t *) opArgs;

    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_PRE_PROCESS_MSG: {
        //BUG #917
        // Still open to debate what we should really be doing here.
        // Situation:
        // For now this type of notify is used to detect EDTs and DBs creation REQUESTS.
        // This is not an after the fact call like OCR_SCHED_NOTIFY_DB_CREATE. The OCR object
        // hasn't been created yet and the policy message contains everything the runtime
        // needs to enact the creation.
        // Implementation:
        // Here, we directly let the placement heuristic decide what to do with the message
        // to mimic the previous implementation behavior (nothing set in stone).
        // The msg can be left unmodified, so that the PD will process the message and create the EDT
        // OR the destination field of the message is altered to specify a different location for
        // the message processing. The PD will in turn send the message there.
        // Alternative:
        // We could systematically let notify first go to the COMP heuristic to have a look at it
        // and assess whether or not it's a good idea to keep the EDT/DB creation local. For instance,
        // the heuristic could say well my deques are almost full etc... In that case, the heuristic
        // returns an error code that signals (this) scheduler to invoke the placement heuristic.
        schedulerHeuristic = dself->schedulerHeuristics[PLACEMENT_HEURISTIC_ID];
        break;
    }
#ifdef LOAD_BALANCING_TEST
    case OCR_SCHED_NOTIFY_EDT_SATISFIED: {
        // Call the computation heuristic and it may check how full is his queue or whatever else
        schedulerHeuristic = dself->schedulerHeuristics[COMP_HEURISTIC_ID];
        u8 res = schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke(schedulerHeuristic, opArgs, hints);
        if (res == OCR_ENOSPC) { //TODO just made up the error code
            // Decided there was no space available for this task so try to load-balance it somewhere.
            // The load balancer impl must be careful not to bounce things around too much.
            // Here we set hints once and it becomes a must not a may
            schedulerHeuristic = dself->schedulerHeuristics[PLACEMENT_HEURISTIC_ID];
            // returns OCR_ENOP if no decisions have been made. In that case the EDT stays in the current PD.
            return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke(schedulerHeuristic, opArgs, hints);
        } else {
            // COMP heuristic agreed to take on this EDT
            ASSERT(res == 0);
            return OCR_ENOP; // Allows caller to proceed
        }
        break;
    }
#endif
    case OCR_SCHED_NOTIFY_COMM_READY: {
        schedulerHeuristic = dself->schedulerHeuristics[COMM_HEURISTIC_ID];
        break;
    }
    default: {
        //We assume the master heuristic is the default target for all notify unless overriden
        ASSERT(COMP_HEURISTIC_ID == self->masterHeuristicId);
        schedulerHeuristic = dself->schedulerHeuristics[COMP_HEURISTIC_ID];
    }
    }
#ifdef OCR_MONITOR_SCHEDULER
    if(notifyArgs->kind == OCR_SCHED_NOTIFY_EDT_READY){
        ocrGuid_t taskGuid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid;
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_SCHEDULER, OCR_ACTION_SCHED_INVOKE, taskGuid);
    }
#endif
    return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke(schedulerHeuristic, opArgs, hints);
}

u8 commonSchedulerTransactInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) self;
    u32 heuristicId = self->masterHeuristicId;
    u32 i;
    for(i = 0; i < self->schedulerHeuristicCount; ++i) {
        if (dself->schedulerHeuristics[i]->factoryId == opArgs->heuristicId) {
            heuristicId = i;
            break;
        }
    }
    ocrSchedulerHeuristic_t *schedulerHeuristic = dself->schedulerHeuristics[heuristicId];
    return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke(schedulerHeuristic, opArgs, hints);
}

u8 commonSchedulerAnalyzeInvoke(ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) self;
    u32 heuristicId = self->masterHeuristicId;
    u32 i;
    for(i = 0; i < self->schedulerHeuristicCount; ++i) {
        if (dself->schedulerHeuristics[i]->factoryId == opArgs->heuristicId) {
            heuristicId = i;
            break;
        }
    }
    ocrSchedulerHeuristic_t *schedulerHeuristic = dself->schedulerHeuristics[heuristicId];
    return schedulerHeuristic->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke(schedulerHeuristic, opArgs, hints);
}

u8 commonSchedulerUpdate(ocrScheduler_t *self, u32 properties) {
    ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) self;
    ocrSchedulerHeuristic_t *schedulerHeuristic = dself->schedulerHeuristics[self->masterHeuristicId];
    return schedulerHeuristic->fcts.update(schedulerHeuristic, properties);
}

ocrScheduler_t* newSchedulerCommon(ocrSchedulerFactory_t * factory, ocrParamList_t *perInstance) {
    ocrScheduler_t* base = (ocrScheduler_t*) runtimeChunkAlloc(sizeof(ocrSchedulerCommon_t), PERSISTENT_CHUNK);
    factory->initialize(factory, base, perInstance);
    paramListSchedulerCommonInst_t * params = (paramListSchedulerCommonInst_t *) perInstance;
    ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) base;

    char * config = params->config;
    ocrSchedulerHeuristic_t ** heuristics = params->heuristics;
    // See comment in header for low/high meaning
    u32 heuristicIdLow = params->heuristicIdLow;
    u32 heuristicIdHigh = params->heuristicIdHigh;
    if (config == NULL) {
        DPRINTF(DEBUG_LVL_INFO, "Warning: No config option specified for COMMON scheduler heuristics. Assume order is comp/plc/comm \n");
        // Do best effort here for backward compatibility
        // - Assume heuristics are given in order.
        // - If not all specified, assume default is the master heuristic or heuristic '0'.
        u32 i = heuristicIdLow;
        u32 masterHeuristicId = -1;
        for (; i <= heuristicIdHigh; i++) { // <= because heuristicIdHigh is inclusive
            ocrSchedulerHeuristic_t * heuristic = heuristics[i];
            dself->schedulerHeuristics[i] = heuristic;
            if (heuristic->isMaster) {
                // Report if there are two master heuristics defined
                ASSERT((masterHeuristicId == -1) && "error: Multiple master heuristics defiend for COMMON scheduler.");
                masterHeuristicId = i;
            }
        }
        if (masterHeuristicId == -1) {
            masterHeuristicId = heuristicIdLow;
        }
        for (i = 0; i < MAX_SCHEDULER_HEURISTICS_COUNT; i++) {
            if (dself->schedulerHeuristics[i] == NULL) {
                dself->schedulerHeuristics[i] = dself->schedulerHeuristics[masterHeuristicId];
             }
        }
    } else {
        ocrSchedulerCommon_t * dself = (ocrSchedulerCommon_t *) base;
        parseConfigStr(heuristics, (ocrSchedulerHeuristic_t **) &(dself->schedulerHeuristics), config);
    }

    return base;
}

void initializeSchedulerCommon(ocrSchedulerFactory_t * factory, ocrScheduler_t *self, ocrParamList_t *perInstance) {
    initializeSchedulerOcr(factory, self, perInstance);
}

void destructSchedulerFactoryCommon(ocrSchedulerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerFactory_t * newOcrSchedulerFactoryCommon(ocrParamList_t *perType) {
    ocrSchedulerFactory_t* base = (ocrSchedulerFactory_t*) runtimeChunkAlloc(
        sizeof(ocrSchedulerFactoryCommon_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newSchedulerCommon;
    base->initialize  = &initializeSchedulerCommon;
    base->destruct = &destructSchedulerFactoryCommon;
    base->schedulerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                          phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), commonSchedulerSwitchRunlevel);
    base->schedulerFcts.destruct = FUNC_ADDR(void (*)(ocrScheduler_t*), commonSchedulerDestruct);
    base->schedulerFcts.takeEdt = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*), commonSchedulerTakeEdt);
    base->schedulerFcts.giveEdt = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*), commonSchedulerGiveEdt);
    base->schedulerFcts.takeComm = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*, u32), commonSchedulerTakeComm);
    base->schedulerFcts.giveComm = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32*, ocrFatGuid_t*, u32), commonSchedulerGiveComm);
    base->schedulerFcts.monitorProgress = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrMonitorProgress_t, void*), commonSchedulerMonitorProgress);

    //Scheduler 1.0
    base->schedulerFcts.update = FUNC_ADDR(u8 (*)(ocrScheduler_t*, u32), commonSchedulerUpdate);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), commonSchedulerGetWorkInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), commonSchedulerNotifyInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), commonSchedulerTransactInvoke);
    base->schedulerFcts.op[OCR_SCHEDULER_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrScheduler_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), commonSchedulerAnalyzeInvoke);
    return base;
}

#endif /* ENABLE_SCHEDULER_COMMON */
