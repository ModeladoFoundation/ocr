/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#if defined(ENABLE_TASK_HC) || defined(ENABLE_TASKTEMPLATE_HC)


#include "debug.h"
#include "event/hc/hc-event.h"
#include "ocr-datablock.h"
#include "ocr-event.h"
#include "ocr-errors.h"
#include "ocr-hal.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "ocr-worker.h"
#include "task/hc/hc-task.h"
#include "utils/ocr-utils.h"
#include "extensions/ocr-hints.h"
#include "ocr-policy-domain-tasks.h"

#include "ocr-perfmon.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#ifdef OCR_ENABLE_PROFILING_STATISTICS
#endif
#endif /* OCR_ENABLE_STATISTICS */

#include "utils/profiler/profiler.h"

#define DEBUG_TYPE TASK

/***********************************************************/
/* OCR-HC Task Hint Properties                             */
/* (Add implementation specific supported properties here) */
/***********************************************************/

u64 ocrHintPropTaskHc[] = {
#ifdef ENABLE_HINTS
    OCR_HINT_EDT_PRIORITY,
    OCR_HINT_EDT_SLOT_MAX_ACCESS,
    OCR_HINT_EDT_AFFINITY,
    OCR_HINT_EDT_DISPERSE,
    /* BUG #923 - Separation of runtime vs user hints ? */
    OCR_HINT_EDT_SPACE,
    OCR_HINT_EDT_TIME,
    OCR_HINT_EDT_STATS_HW_CYCLES,
    OCR_HINT_EDT_STATS_L1_HITS,
    OCR_HINT_EDT_STATS_L1_MISSES,
    OCR_HINT_EDT_STATS_FLOAT_OPS
#endif
};

// This is to exclude the RT EDTs from the "userCode" classification from the profiler
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
#endif

//Make sure OCR_HINT_COUNT_EDT_HC in hc-task.h is equal to the length of array ocrHintPropTaskHc
ocrStaticAssert((sizeof(ocrHintPropTaskHc)/sizeof(u64)) == OCR_HINT_COUNT_EDT_HC);
ocrStaticAssert(OCR_HINT_COUNT_EDT_HC < OCR_RUNTIME_HINT_PROP_BITS);

/******************************************************/
/* OCR-HC Task Template Factory                       */
/******************************************************/

#ifdef ENABLE_TASKTEMPLATE_HC

u8 destructTaskTemplateHc(ocrTaskTemplate_t *self) {
#ifdef OCR_ENABLE_STATISTICS
    {
        // Bug #225
        ocrPolicyDomain_t *pd = getCurrentPD();
        ocrGuid_t edtGuid = getCurrentEDT();

        statsTEMP_DESTROY(pd, edtGuid, NULL, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid.guid) = getObjectField(self, guid);
    PD_MSG_FIELD_I(guid.metaDataPtr) = self;
    PD_MSG_FIELD_I(properties) = 1;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

#ifdef ENABLE_EXTENSION_PERF
void addPerfEntry(ocrPolicyDomain_t *pd, void *executePtr,
                         ocrTaskTemplate_t *taskT) {
    u32 k;

    // Skip adding a new entry if we already have one
    if(taskT && (taskT->taskPerfsEntry!=NULL)) {
       // Nothing to do
    } else {
        for(k=0; k < queueGetSize(pd->taskPerfs); k++)
            if(((ocrPerfCounters_t*)queueGet(pd->taskPerfs, k))->edt == executePtr)
                break;

        if(k==queueGetSize(pd->taskPerfs)) {
            u32 j;
            ocrPerfCounters_t *cumulativeStats = (ocrPerfCounters_t *)pd->fcts.pdMalloc(pd, sizeof(ocrPerfCounters_t));
            for(j = 0; j<PERF_MAX; j++){
                cumulativeStats->stats[j].average = 0;
                cumulativeStats->stats[j].current = 0;
            }
            cumulativeStats->count = 0;
            cumulativeStats->steadyStateMask = ((1 << PERF_MAX) - 1); // Steady state not reached
            cumulativeStats->edt = executePtr;
            if(queueIsFull(pd->taskPerfs)) queueDoubleResize(pd->taskPerfs, true);
            queueAddLast(pd->taskPerfs, cumulativeStats);
            taskT->taskPerfsEntry = cumulativeStats;
        } else taskT->taskPerfsEntry = queueGet(pd->taskPerfs, k);
    }
}
#endif

ocrTaskTemplate_t * newTaskTemplateHc(ocrTaskTemplateFactory_t* factory, ocrEdt_t executePtr,
                                      u32 paramc, u32 depc, const char* fctName,
                                      ocrParamList_t *perInstance) {

    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);

    u32 hintc = OCR_HINT_COUNT_EDT_HC;
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(size) = sizeof(ocrTaskTemplateHc_t) + hintc*sizeof(u64);
    PD_MSG_FIELD_I(kind) = OCR_GUID_EDT_TEMPLATE;
    PD_MSG_FIELD_I(targetLoc) = pd->myLocation;
    PD_MSG_FIELD_I(properties) = 0;

    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);

    ocrTaskTemplate_t *base = (ocrTaskTemplate_t*)PD_MSG_FIELD_IO(guid.metaDataPtr);
    ASSERT(base);
    setObjectField(base, guid, PD_MSG_FIELD_IO(guid.guid));
    // base->guid = PD_MSG_FIELD_IO(guid.guid);
#undef PD_MSG
#undef PD_TYPE

    base->paramc = paramc;
    base->depc = depc;
    base->executePtr = executePtr;
#ifdef OCR_ENABLE_EDT_NAMING
    {
        // NOTE: don't assume the name fits in the buffer!
        u32 t = ocrStrlen(fctName);
        if(t*sizeof(char) >= sizeof(base->name)) {
            t = sizeof(base->name)/sizeof(char) - 1;
        }
        // copy and null-terminate the (possibly truncated) function name
        hal_memCopy(&(base->name[0]), fctName, t, false);
        *(char*)(base->name+t) = '\0';
    }
#endif
    base->base.fctId = factory->factoryId;
    base->fctId = factory->factoryId;

    ocrTaskTemplateHc_t *derived = (ocrTaskTemplateHc_t*)base;
    if (hintc == 0) {
        derived->hint.hintMask = 0;
        derived->hint.hintVal = NULL;
    } else {
        OCR_RUNTIME_HINT_MASK_INIT(derived->hint.hintMask, OCR_HINT_EDT_T, factory->factoryId);
        derived->hint.hintVal = (u64*)((u64)base + sizeof(ocrTaskTemplateHc_t));
        u32 i; for(i = 0; i<hintc; i++) derived->hint.hintVal[i] = 0ULL;
    }

#ifdef OCR_ENABLE_STATISTICS
    {
        // Bug #225
        ocrGuid_t edtGuid = getCurrentEDT();
        statsTEMP_CREATE(pd, edtGuid, NULL, base->guid, base);
    }
#endif /* OCR_ENABLE_STATISTICS */
#ifdef ENABLE_EXTENSION_PERF
    base->taskPerfsEntry = NULL;
    addPerfEntry(pd, base->executePtr, base);
#endif
    return base;
}

u8 setHintTaskTemplateHc(ocrTaskTemplate_t* self, ocrHint_t *hint) {
    ocrTaskTemplateHc_t *derived = (ocrTaskTemplateHc_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_SET(hint, rHint, OCR_HINT_COUNT_EDT_HC, ocrHintPropTaskHc, OCR_HINT_EDT_PROP_START);
    return 0;
}

u8 getHintTaskTemplateHc(ocrTaskTemplate_t* self, ocrHint_t *hint) {
    ocrTaskTemplateHc_t *derived = (ocrTaskTemplateHc_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_GET(hint, rHint, OCR_HINT_COUNT_EDT_HC, ocrHintPropTaskHc, OCR_HINT_EDT_PROP_START);
    return 0;
}

ocrRuntimeHint_t* getRuntimeHintTaskTemplateHc(ocrTaskTemplate_t* self) {
    ocrTaskTemplateHc_t *derived = (ocrTaskTemplateHc_t*)self;
    return &(derived->hint);
}

void destructTaskTemplateFactoryHc(ocrObjectFactory_t* factory) {
    runtimeChunkFree((u64)((ocrTaskTemplateFactory_t*)factory)->hintPropMap, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrTaskTemplateFactory_t * newTaskTemplateFactoryHc(ocrParamList_t* perType, u32 factoryId) {
    ocrTaskTemplateFactory_t* base = (ocrTaskTemplateFactory_t*)runtimeChunkAlloc(sizeof(ocrTaskTemplateFactoryHc_t), PERSISTENT_CHUNK);

    // Initialize the base's base
    base->base.fcts.processEvent = NULL;
    base->instantiate = FUNC_ADDR(ocrTaskTemplate_t* (*)(ocrTaskTemplateFactory_t*, ocrEdt_t, u32, u32, const char*, ocrParamList_t*), newTaskTemplateHc);
    base->base.destruct =  FUNC_ADDR(void (*)(ocrObjectFactory_t*), destructTaskTemplateFactoryHc);
    base->factoryId = factoryId;
    base->fcts.destruct = FUNC_ADDR(u8 (*)(ocrTaskTemplate_t*), destructTaskTemplateHc);
    base->fcts.setHint = FUNC_ADDR(u8 (*)(ocrTaskTemplate_t*, ocrHint_t*), setHintTaskTemplateHc);
    base->fcts.getHint = FUNC_ADDR(u8 (*)(ocrTaskTemplate_t*, ocrHint_t*), getHintTaskTemplateHc);
    base->fcts.getRuntimeHint = FUNC_ADDR(ocrRuntimeHint_t* (*)(ocrTaskTemplate_t*), getRuntimeHintTaskTemplateHc);
    //Setup hint framework
    base->hintPropMap = (u64*)runtimeChunkAlloc(sizeof(u64)*(OCR_HINT_EDT_PROP_END - OCR_HINT_EDT_PROP_START - 1), PERSISTENT_CHUNK);
    OCR_HINT_SETUP(base->hintPropMap, ocrHintPropTaskHc, OCR_HINT_COUNT_EDT_HC, OCR_HINT_EDT_PROP_START, OCR_HINT_EDT_PROP_END);
    return base;
}

#endif /* ENABLE_TASKTEMPLATE_HC */

#ifdef ENABLE_TASK_HC

#ifdef ENABLE_OCR_API_DEFERRABLE_MT
#define CONTINUATION_EDT_EPILOGUE 0
#endif

/******************************************************/
/* OCR HC utilities                                   */
/******************************************************/

// Factorize simple add satisfy call. The destination must be local.
static u8 doSatisfy(ocrPolicyDomain_t *pd, ocrPolicyMsg_t *msg,
                             ocrFatGuid_t edtCheckin, ocrFatGuid_t dest, ocrFatGuid_t payload, u32 slot) {
    // By construction and for performances the 'dest' used should be local.
#if defined(TG_X86_TARGET) || defined(TG_XE_TARGET) || defined(TG_CE_TARGET)
    // On TG, the event is created by the CE? and the XE does the satisfy
    // Shouldn't that become blocking ?
#else
    ASSERT(isLocalGuid(pd, dest.guid));
#endif
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    msg->type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(satisfierGuid) = edtCheckin;
    PD_MSG_FIELD_I(guid) = dest;
    PD_MSG_FIELD_I(payload) = payload;
    PD_MSG_FIELD_I(currentEdt) = edtCheckin;
    PD_MSG_FIELD_I(slot) = slot;
#ifdef REG_ASYNC_SGL
    PD_MSG_FIELD_I(mode) = -1;
#endif
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, true));
#undef PD_TYPE
#undef PD_MSG
    return 0;
}

// Factorize simple add dependence call
static u8 doAddDep(ocrPolicyDomain_t *pd, ocrPolicyMsg_t *msg,
                             ocrFatGuid_t edtCheckin, ocrFatGuid_t src, ocrFatGuid_t dest, u32 slot) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_ADD
    msg->type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(source) = src;
    PD_MSG_FIELD_I(dest) = dest;
    PD_MSG_FIELD_I(slot) = slot;
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(properties) = DB_MODE_CONST;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

/******************************************************/
/* Random helper functions                            */
/******************************************************/

static inline bool hasProperty(u32 properties, u32 property) {
    return properties & property;
}

#if !(defined(REG_ASYNC) || defined(REG_ASYNC_SGL))
static u8 registerOnFrontier(ocrTaskHc_t *self, ocrPolicyDomain_t *pd,
                             ocrPolicyMsg_t *msg, u32 slot) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_REGWAITER
    msg->type = PD_MSG_DEP_REGWAITER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(waiter.guid) = self->base.guid;
    PD_MSG_FIELD_I(waiter.metaDataPtr) = self;
    PD_MSG_FIELD_I(dest.guid) = self->signalers[slot].guid;
    PD_MSG_FIELD_I(dest.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(slot) = self->signalers[slot].slot;
    PD_MSG_FIELD_I(properties) = false; // not called from add-dependence
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, true));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}
#endif

/******************************************************/
/* OCR-HC Support functions                           */
/******************************************************/

static u8 initTaskHcInternal(ocrTaskHc_t *task, ocrGuid_t taskGuid, ocrPolicyDomain_t * pd,
                             ocrTask_t *curTask, ocrFatGuid_t outputEvent,
                             ocrFatGuid_t parentLatch, u32 properties) {
    task->frontierSlot = 0;
#if !(defined(REG_ASYNC) || defined(REG_ASYNC_SGL))
    task->lock = INIT_LOCK;
#endif
    task->slotSatisfiedCount = 0;
    task->unkDbs = NULL;
    task->countUnkDbs = 0;
    task->maxUnkDbs = 0;
    task->resolvedDeps = NULL;
    task->mdState = MD_STATE_EDT_MASTER;
#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    task->evtHead = NULL;
    task->tailStrand = NULL;
#else
    task->evts = NULL;
#endif
#endif

    u32 i;
    for(i = 0; i < OCR_MAX_MULTI_SLOT; ++i) {
        task->doNotReleaseSlots[i] = 0ULL;
    }

    if(task->base.depc == 0) {
        task->signalers = END_OF_LIST;
    }

#ifdef ENABLE_EXTENSION_PERF
    for(i = 0; i < PERF_MAX - PERF_HW_MAX; i++) task->base.swPerfCtrs[i] = 0;
#endif

    return 0;
}

/**
 * @brief sort an array of regNode_t according to their GUID
 * Warning. 'notifyDbReleaseTaskHc' relies on this sort to be stable !
 */
 static void sortRegNode(regNode_t * array, u32 length) {
     if (length >= 2) {
        int idx;
        int sorted = 0;
        do {
            idx = sorted;
            regNode_t val = array[sorted+1];
            while((idx > -1) && (ocrGuidIsLt(val.guid, array[idx].guid))) {
                idx--;
            }
            if (idx < sorted) {
                // shift by one to insert the element
                hal_memMove(&array[idx+2], &array[idx+1], sizeof(regNode_t)*(sorted-idx), false);
                array[idx+1] = val;
            }
            sorted++;
        } while (sorted < (length-1));
    }
}

/**
 * @brief Advance the DB iteration frontier to the next DB
 * This implementation iterates on the GUID-sorted signaler vector
 * Returns false when the end of depv is reached
 */
static u8 iterateDbFrontier(ocrTask_t *self) {
    ocrTaskHc_t * rself = ((ocrTaskHc_t *) self);
    regNode_t * depv = rself->signalers;
    u32 i = rself->frontierSlot;
    for (; i < self->depc; ++i) {
        //ACQUIRE can be non-blocking so pre-increment the frontier
        //slot and adjust by -1 in dependenceResolvedTaskHc
        rself->frontierSlot++;
        if (!(ocrGuidIsNull(depv[i].guid))) {
            // Because the frontier is sorted, we can check for duplicates here
            // and remember them to avoid double release
            if ((i > 0) && (ocrGuidIsEq(depv[i-1].guid, depv[i].guid))) {
                rself->resolvedDeps[depv[i].slot].ptr = rself->resolvedDeps[depv[i-1].slot].ptr;
                // If the below asserts, rebuild OCR with a higher OCR_MAX_MULTI_SLOT (in build/common.mk)
                ASSERT(depv[i].slot / 64 < OCR_MAX_MULTI_SLOT);
                rself->doNotReleaseSlots[depv[i].slot / 64] |= (1ULL << (depv[i].slot % 64));
            } else {
                // Issue acquire request
                ocrPolicyDomain_t * pd = NULL;
                PD_MSG_STACK(msg);
                getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = depv[i].guid; // DB guid
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
                PD_MSG_FIELD_IO(edt.guid) = self->guid; // EDT guid
                PD_MSG_FIELD_IO(edt.metaDataPtr) = self;
                PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
                PD_MSG_FIELD_IO(edtSlot) = self->depc + 1; // RT slot
                PD_MSG_FIELD_IO(properties) = depv[i].mode;
                u8 returnCode = pd->fcts.processMessage(pd, &msg, false);
                // DB_ACQUIRE is potentially asynchronous, check completion.
                // In shmem and dist HC PD, ACQUIRE is two-way, processed asynchronously
                // (the false in 'processMessage'). For now the CE/XE PD do not support this
                // mode so we need to check for the returnDetail of the acquire message instead.
                if ((returnCode == OCR_EPEND) || (PD_MSG_FIELD_O(returnDetail) == OCR_EBUSY)) {
                    return true;
                }
#ifdef ENABLE_EXTENSION_PERF
                rself->base.swPerfCtrs[PERF_DB_TOTAL - PERF_HW_MAX] += PD_MSG_FIELD_O(size);
#endif
                // else, acquire took place and was successful, continue iterating
                ASSERT(msg.type & PD_MSG_RESPONSE); // 2x check
                rself->resolvedDeps[depv[i].slot].ptr = PD_MSG_FIELD_O(ptr);
#undef PD_MSG
#undef PD_TYPE
            }
        }
    }
    return false;
}


/**
 * @brief Give the task to the scheduler
 * Warning: The caller must ensure all dependencies have been satisfied
 * Note: static function only meant to factorize code.
 */
static u8 scheduleTask(ocrTask_t *self) {
    DPRINTF(DEBUG_LVL_INFO, "Schedule "GUIDF"\n", GUIDA(self->guid));
    self->state = ALLACQ_EDTSTATE;
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);

#ifdef OCR_MONITOR_SCHEDULER
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_SCHEDULER, OCR_ACTION_SCHED_MSG_SEND, self->guid);
#endif

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_NOTIFY
    msg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_EDT_READY;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid = self->guid;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.metaDataPtr = self;
    ASSERT(self != NULL);
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
    ASSERT(PD_MSG_FIELD_O(returnDetail) == 0);
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

/**
 * @brief Give the fully satisfied task to the scheduler
 */
static u8 scheduleSatisfiedTask(ocrTask_t *self) {
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_NOTIFY
    msg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_EDT_SATISFIED;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_SATISFIED).guid.guid = self->guid;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_SATISFIED).guid.metaDataPtr = self;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
    return PD_MSG_FIELD_O(returnDetail);
#undef PD_MSG
#undef PD_TYPE
}

/**
 * @brief Dependences of the tasks have been satisfied
 * Warning: The caller must ensure all dependencies have been satisfied
 * Note: static function only meant to factorize code.
 */
static u8 taskAllDepvSatisfied(ocrTask_t *self) {
    DPRINTF(DEBUG_LVL_INFO, "All dependences satisfied for task "GUIDF"\n", GUIDA(self->guid));
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_RUNNABLE, traceTaskRunnable, self->guid);
    // Now check if there's anything to do before scheduling
    // In this implementation we want to acquire locks for DBs in EW mode
    ocrTaskHc_t * rself = (ocrTaskHc_t *) self;
    rself->slotSatisfiedCount++; // Mark the slotSatisfiedCount as being all satisfied
    if (self->depc > 0) {
        ocrPolicyDomain_t * pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        // Initialize the dependence list to be transmitted to the EDT's user code.
        u32 depc = self->depc;
        ocrEdtDep_t * resolvedDeps = pd->fcts.pdMalloc(pd, sizeof(ocrEdtDep_t)* depc);
        rself->resolvedDeps = resolvedDeps;
        regNode_t * signalers = rself->signalers;
        u32 i = 0;
        while(i < depc) {
            ASSERT(!ocrGuidIsUninitialized(signalers[i].guid) && !ocrGuidIsError(signalers[i].guid));
            if (signalers[i].mode == DB_MODE_NULL) {
                signalers[i].guid = NULL_GUID;
            }
            rself->signalers[i].slot = i; // reset the slot info
            resolvedDeps[i].guid = signalers[i].guid; // DB guids by now
            resolvedDeps[i].ptr = NULL; // resolved by acquire messages
            resolvedDeps[i].mode = signalers[i].mode;
            i++;
        }
        // Sort regnode in guid's ascending order.
        // This is the order in which we acquire the DBs
        sortRegNode(signalers, self->depc);
        rself->frontierSlot = 0;
    }
    // Try to start the DB acquisition process if scheduler agreed
    // When scheduleSatisfiedTask returns zero it means the scheduler
    // has either decided to move the task or wants to start the DB
    // acquisition later.
    if (scheduleSatisfiedTask(self) != 0 && !iterateDbFrontier(self)) {
        //TODO: Keeping this here for 0.9 compatibility but
        //iterateDbFrontier and related code will eventually
        //move to the scheduler.
        scheduleTask(self);
    }
    return 0;
}

/******************************************************/
/* OCR-HC Task Implementation                         */
/******************************************************/


// Special sentinel values used to mark slots state

// A slot that contained an event has been satisfied
#define SLOT_SATISFIED_EVT              ((u32) -1)
// An ephemeral event has been registered on the slot
#define SLOT_REGISTERED_EPHEMERAL_EVT   ((u32) -2)
// A slot has been satisfied with a DB
#define SLOT_SATISFIED_DB               ((u32) -3)

u8 destructTaskHc(ocrTask_t* base) {
    DPRINTF(DEBUG_LVL_INFO,
            "Destroy "GUIDF"\n", GUIDA(base->guid));
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_DESTROY, traceTaskDestroy, base->guid);

    ocrPolicyDomain_t *pd = NULL;
    // If we are above ALLDEPS_EDTSTATE it's hard to determine exactly
    // what the task might be doing. For now just have a simple policy
    // that we'll let the task run to completion
    if (base->state < ALLDEPS_EDTSTATE) {
        ocrTask_t * curEdt = NULL;
        getCurrentEnv(&pd, NULL, &curEdt, NULL);
        ocrTaskHc_t* dself = (ocrTaskHc_t*)base;
#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
#else
        u32 i= 0;
        u32 ub = queueGetSize(dself->evts);
        while (i < ub) {
            pdEventMsg_t * evt = queueGet(dself->evts, i);
            pd->fcts.pdFree(pd, evt);
            i++;
        }
        queueDestroy(dself->evts);
#endif
#endif
        // Dealing with EDT movement here. When an EDT moves the current PD's
        // version should be deallocated, however we do not want to checkout from
        // finish scopes and destroy events. Only do that if the MD is in master state.
        if (dself->mdState == MD_STATE_EDT_MASTER) {
            // Clean up output-event
            if (!(ocrGuidIsNull(base->outputEvent))) {
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_DESTROY
                msg.type = PD_MSG_EVT_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(guid.guid) = base->outputEvent;
                PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(currentEdt.guid) = curEdt ? curEdt->guid : NULL_GUID;
                PD_MSG_FIELD_I(currentEdt.metaDataPtr) = curEdt;
                PD_MSG_FIELD_I(properties) = 0;
                u8 returnCode __attribute__((unused)) = pd->fcts.processMessage(pd, &msg, false);
                ASSERT(returnCode == 0);
#undef PD_MSG
#undef PD_TYPE
            }
            ASSERT(ocrGuidIsNull(base->finishLatch) || ocrGuidIsUninitialized(base->finishLatch));

            // Need to decrement the parent latch since the EDT didn't run
            if (!(ocrGuidIsNull(base->parentLatch))) {
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
                //TODO ABA issue here if not REQ_RESP ?
                msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(satisfierGuid.guid) = (curEdt ? curEdt->guid : NULL_GUID);
                PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = curEdt;
                PD_MSG_FIELD_I(guid.guid) = base->parentLatch;
                PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(payload.guid) = NULL_GUID;
                PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(currentEdt.guid) = curEdt ? curEdt->guid : NULL_GUID;
                PD_MSG_FIELD_I(currentEdt.metaDataPtr) = curEdt;
                PD_MSG_FIELD_I(slot) = OCR_EVENT_LATCH_DECR_SLOT;
#ifdef REG_ASYNC_SGL
                PD_MSG_FIELD_I(mode) = -1; //Doesn't matter for latch
#endif
                PD_MSG_FIELD_I(properties) = 0;
                u8 returnCode __attribute__((unused)) = pd->fcts.processMessage(pd, &msg, false);
                ASSERT(returnCode == 0);
#undef PD_MSG
#undef PD_TYPE
            }
        }
    } else {
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
        if (base->state == RESCHED_EDTSTATE) {
            DPRINTF(DEBUG_LVL_WARN, "error: Detected inconsistency, check the CFG file uses the LEGACY scheduler");
            ASSERT(false && "error: Detected inconsistency, check the CFG file uses the LEGACY scheduler");
            return OCR_EPERM;
        }
#endif
        if (base->state != REAPING_EDTSTATE) {
            DPRINTF(DEBUG_LVL_WARN, "Destroy EDT "GUIDF" is potentially racing with the EDT prelude or execution\n", GUIDA(base->guid));
            ASSERT(false && "EDT destruction is racing with EDT execution");
            return OCR_EPERM;
        }
    }

#ifdef OCR_ENABLE_STATISTICS
    {
        // Bug #225
        // An EDT is destroyed just when it finishes running so
        // the source is basically itself
        statsEDT_DESTROY(pd, base->guid, base, base->guid, base);
    }
#endif /* OCR_ENABLE_STATISTICS */
    // Destroy the EDT GUID and metadata
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid.guid) = base->guid;
    PD_MSG_FIELD_I(guid.metaDataPtr) = base;
    PD_MSG_FIELD_I(properties) = 1; // Free metadata
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

//TODO-MD discrepancy between szMd here and in events where they do not account for embedded MD
static u8 allocateNewTaskHc(ocrPolicyDomain_t *pd, ocrFatGuid_t * resultGuid,
                            u8 *returnDetail, u32 szMd, ocrLocation_t targetLoc,
                            u32 properties) {
    PD_MSG_STACK(msg);
    // Create the task itself by getting a GUID
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = *resultGuid;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD_I(size) = szMd;
    PD_MSG_FIELD_I(kind) = OCR_GUID_EDT;
    PD_MSG_FIELD_I(targetLoc) = targetLoc;
    PD_MSG_FIELD_I(properties) = properties;
    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), 1);
    *resultGuid = PD_MSG_FIELD_IO(guid);
    if(returnDetail)
        *returnDetail = PD_MSG_FIELD_O(returnDetail);
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

u8 newTaskHc(ocrTaskFactory_t* factory, ocrFatGuid_t * edtGuid, ocrFatGuid_t edtTemplate,
                      u32 paramc, u64* paramv, u32 depc, u32 properties,
                      ocrHint_t *hint, ocrFatGuid_t * outputEventPtr,
                      ocrTask_t *curEdt, ocrFatGuid_t parentLatch,
                      ocrParamList_t *perInstance) {
    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    getCurrentEnv(&pd, NULL, &curTask, NULL);
    ocrFatGuid_t currentEdt = {.guid = ((curTask) ? curTask->guid : NULL_GUID), curTask};
    ocrFatGuid_t resultGuid = *edtGuid;
    u32 hintc = hasProperty(properties, EDT_PROP_NO_HINT) ? 0 : OCR_HINT_COUNT_EDT_HC;
    u32 szMd = sizeof(ocrTaskHc_t) + paramc*sizeof(u64) + depc*sizeof(regNode_t) + hintc*sizeof(u64);

    ocrLocation_t targetLoc = pd->myLocation;
    if (hint != NULL_HINT) {
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
            affinityToLocation(&(targetLoc), affGuid);
       }
    }
    // Paths:
    // - GUID_PROP_ISVALID | GUID_PROP_TORECORD:
    //      - Deferred creation
    //      - MD creation clone
    //      - MD creation move
    // - GUID_PROP_TORECORD:
    //      - MD creation master
    ASSERT(properties & GUID_PROP_TORECORD);

    u8 returnValue = 0;
    allocateNewTaskHc(pd, &resultGuid, &returnValue,
                      szMd, targetLoc, properties);

    ocrTask_t * self = (ocrTask_t*)(resultGuid.metaDataPtr);
    ocrTaskHc_t* dself = (ocrTaskHc_t*)self;
    ocrGuid_t taskGuid = resultGuid.guid; // Temporary storage for task GUID
    ASSERT(dself);
    // Labeled case most likely. We return and don't create the event
    if(returnValue)
        return returnValue;

    // We need an output event if the user requested it.
    // This is always initialized and the guid is either uninitialized or null_guid
    ASSERT(outputEventPtr != NULL);
    ocrFatGuid_t outputEvent = {.guid = NULL_GUID, .metaDataPtr = NULL};
    // Check if we need an output event created
#ifdef ENABLE_OCR_API_DEFERRABLE
    if (!ocrGuidIsNull(outputEventPtr->guid)) {
        // In deferred the GUID is either null or valid but no instance attached to it
        ASSERT(!ocrGuidIsUninitialized(outputEventPtr->guid));
#else
    if (ocrGuidIsUninitialized(outputEventPtr->guid)) {
        // In non-deferred, an unitialized guid indicates the user requested a creation
#endif
        PD_MSG_STACK(msg2);
        getCurrentEnv(NULL, NULL, NULL, &msg2);
#define PD_MSG (&msg2)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg2.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = outputEventPtr->guid; // InOut depending on props
        PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(currentEdt) = currentEdt;
#ifdef ENABLE_EXTENSION_PARAMS_EVT
        PD_MSG_FIELD_I(params) = NULL;
#endif
        PD_MSG_FIELD_I(properties) = (properties & GUID_RT_PROP_ALL) | GUID_PROP_TORECORD;
        PD_MSG_FIELD_I(type) = OCR_EVENT_ONCE_T; // Default OE type
        RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg2, true), 1);
        outputEvent = PD_MSG_FIELD_IO(guid);
        ASSERT(!ocrGuidIsNull(outputEvent.guid));
        ASSERT(!ocrGuidIsUninitialized(outputEvent.guid));
#undef PD_MSG
#undef PD_TYPE
    } else {
        outputEvent.guid = outputEventPtr->guid;
    }

    // Set up the base's base
    self->base.fctId = factory->factoryId;
    // Set-up base structures
    self->templateGuid = edtTemplate.guid;
    ASSERT(edtTemplate.metaDataPtr); // For now we just assume it is passed whole
    self->funcPtr = ((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->executePtr;
    self->paramv = (paramc > 0) ? ((u64*)((u64)self + sizeof(ocrTaskHc_t))) : NULL;
#ifdef OCR_ENABLE_EDT_NAMING
    hal_memCopy(&(self->name[0]), &(((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->name[0]),
                ocrStrlen(&(((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->name[0])) + 1, false);
#endif
    self->outputEvent = outputEvent.guid;
    // Sentinel value to remember a finish scope must be created when the EDT start executing
    self->finishLatch = (hasProperty(properties, EDT_PROP_FINISH)) ? UNINITIALIZED_GUID : NULL_GUID;
    self->parentLatch = parentLatch.guid;
    u32 i;
    for(i = 0; i < ELS_SIZE; ++i) {
        self->els[i] = NULL_GUID;
    }
    self->state = CREATED_EDTSTATE;
    self->paramc = paramc;
    self->depc = depc;
    self->flags = 0;
    self->fctId = factory->factoryId;
    for(i = 0; i < paramc; ++i) {
        self->paramv[i] = paramv[i];
    }
    dself->signalers = (regNode_t*)((u64)dself + sizeof(ocrTaskHc_t) + paramc*sizeof(u64));
    // Initialize the signalers properly
    for(i = 0; i < depc; ++i) {
        dself->signalers[i].guid = UNINITIALIZED_GUID;
        dself->signalers[i].slot = i;
        dself->signalers[i].mode = -1; //Systematically set when adding dependence
    }

    if (hintc == 0) {
        dself->hint.hintMask = 0;
        dself->hint.hintVal = NULL;
    } else {
        self->flags |= OCR_TASK_FLAG_USES_HINTS;
        ocrTaskTemplateHc_t *derived = (ocrTaskTemplateHc_t*)(edtTemplate.metaDataPtr);
        dself->hint.hintMask = derived->hint.hintMask;
        dself->hint.hintVal = (u64*)((u64)self + sizeof(ocrTaskHc_t) + paramc*sizeof(u64) + depc*sizeof(regNode_t));
        u64 hintSize = OCR_RUNTIME_HINT_GET_SIZE(derived->hint.hintMask);
        for (i = 0; i < hintc; i++) dself->hint.hintVal[i] = (hintSize == 0) ? 0 : derived->hint.hintVal[i]; //copy the hints from the template
        if (hint != NULL_HINT) factory->fcts.setHint(self, hint);
#ifdef ENABLE_EXTENSION_PERF
        self->taskPerfsEntry = derived->base.taskPerfsEntry;
        ASSERT(self->taskPerfsEntry);
        u64 hwCycles = 0;
        if(hint != NULL_HINT) ocrGetHintValue(hint, OCR_HINT_EDT_STATS_HW_CYCLES, &hwCycles);

        // If there are EDT statistics hint values, don't monitor and
        // set the task counter values directly
        if(hwCycles) {
            self->flags &= ~OCR_TASK_FLAG_PERFMON_ME;
        } else {
            // Look up the steadyStateMask value
            if(self->taskPerfsEntry->steadyStateMask) self->flags |= OCR_TASK_FLAG_PERFMON_ME;
         }
#endif
    }

    if (perInstance != NULL) {
        paramListTask_t *taskparams = (paramListTask_t*)perInstance;
        if (taskparams->workType == EDT_RT_WORKTYPE) {
            self->flags |= OCR_TASK_FLAG_RUNTIME_EDT;
        }
    }

    u64 val = 0;
    if (hint != NULL_HINT && (ocrGetHintValue(hint, OCR_HINT_EDT_AFFINITY, &val) == 0)) {
      self->flags |= OCR_TASK_FLAG_USES_AFFINITY;
    }

#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    if (hasProperty(properties, EDT_PROP_LONG)) {
        self->flags |= OCR_TASK_FLAG_LONG;
    }
#endif

    // Set up HC specific stuff
    RESULT_PROPAGATE2(initTaskHcInternal(dself, taskGuid, pd, curEdt, outputEvent, parentLatch, properties), 1);

    // If there's a local parent latch for this EDT, register to it
    if (!(ocrGuidIsNull(parentLatch.guid)) && isLocalGuid(pd, parentLatch.guid)) {
        DPRINTF(DEBUG_LVL_INFO, "Checkin "GUIDF" on local parent finish latch "GUIDF"\n", GUIDA(taskGuid), GUIDA(parentLatch.guid));
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
        ocrFatGuid_t edtCheckin = {.guid = taskGuid, .metaDataPtr = NULL};
        ocrFatGuid_t nullFGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
        RESULT_PROPAGATE(doSatisfy(pd, &msg, edtCheckin, parentLatch, nullFGuid, OCR_EVENT_LATCH_INCR_SLOT));
    }
    // For remote creations of EDTs, the registration is handled
    // in the distributed policy domain code instead. Not great.
    // Also, other finish-scope scenari are handled in taskExecute.

    // Write back the output event GUID parameter result
    if(!ocrGuidIsNull(outputEventPtr->guid)) {
        ASSERT(!ocrGuidIsUninitialized(self->outputEvent));
        outputEventPtr->guid = self->outputEvent;
    }
#undef PD_MSG
#undef PD_TYPE

#ifdef OCR_ENABLE_STATISTICS
    // Bug #225
    {
        ocrGuid_t edtGuid = getCurrentEDT();
        if(edtGuid) {
            // Usual case when the EDT is created within another EDT
            ocrTask_t *task = NULL;
            deguidify(pd, edtGuid, (u64*)&task, NULL);

            statsTEMP_USE(pd, edtGuid, task, taskTemplate->guid, taskTemplate);
            statsEDT_CREATE(pd, edtGuid, task, self->guid, self);
        } else {
            statsTEMP_USE(pd, edtGuid, NULL, taskTemplate->guid, taskTemplate);
            statsEDT_CREATE(pd, edtGuid, NULL, self->guid, self);
        }
    }
#endif /* OCR_ENABLE_STATISTICS */
    DPRINTF(DEBUG_LVL_INFO, "Create "GUIDF" depc %"PRId32" outputEvent "GUIDF"\n", GUIDA(taskGuid), depc, GUIDA(outputEventPtr?outputEventPtr->guid:NULL_GUID));
    edtGuid->guid = taskGuid;
    self->guid = taskGuid;
    edtGuid->metaDataPtr = self;
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_EDT, OCR_ACTION_CREATE, traceTaskCreate, edtGuid->guid, depc, paramc, paramv);
    // Check to see if the EDT can be ran
    if(self->depc == dself->slotSatisfiedCount) {
        DPRINTF(DEBUG_LVL_INFO,
                "Scheduling task "GUIDF" due to initial satisfactions\n", GUIDA(self->guid));
        //TODO-MD-EDT: Is this an issue if the scheduler calls move here ?
        RESULT_PROPAGATE2(taskAllDepvSatisfied(self), 1);
    }
    return 0;
}

u8 dependenceResolvedTaskHc(ocrTask_t * self, ocrGuid_t dbGuid, void * localDbPtr, u32 slot) {
    ocrTaskHc_t * rself = (ocrTaskHc_t *) self;
    //BUG #924 - We need to decouple satisfy and acquire. Until then, we will
    //use this workaround of using the slot info to do that.
    if (slot == EDT_SLOT_NONE) {
        //This is called after the scheduler moves an EDT to the right place,
        //and also decides the right time for the EDT to start acquiring the DBs.
        ASSERT(ocrGuidIsNull(dbGuid) && localDbPtr == NULL);
        //I believe the signalers are already sorted, this assert should
        //fail if that's not the case and we can revisit why
        ASSERT(rself->frontierSlot == 0);
        // Sort regnode in guid's ascending order.
        // This is the order in which we acquire the DBs
        sortRegNode(rself->signalers, self->depc);
        // Start the DB acquisition process
        rself->frontierSlot = 0;
    } else {
        // EDT already has all its dependences satisfied, now we're getting acquire notifications
        // should only happen on RT event slot to manage DB acquire
        ASSERT(slot == (self->depc+1));
        ASSERT(rself->slotSatisfiedCount == slot);
        // Implementation acquires DB sequentially, so the DB's GUID
        // must match the frontier's DB and we do not need to lock this code
        ASSERT(ocrGuidIsEq(dbGuid, rself->signalers[rself->frontierSlot-1].guid));
        rself->resolvedDeps[rself->signalers[rself->frontierSlot-1].slot].ptr = localDbPtr;
    }
    if (!iterateDbFrontier(self)) {
        scheduleTask(self);
    }
    return 0;
}

#ifdef REG_ASYNC_SGL
u8 satisfyTaskHcWithMode(ocrTask_t * base, ocrFatGuid_t data, u32 slot, ocrDbAccessMode_t mode) {
    ASSERT (((!ocrGuidIsNull(data.guid)) ? (mode != -1) : 1) && "Mode should alway be provided");
    ASSERT(!ocrGuidIsUninitialized(data.guid) && !ocrGuidIsError(data.guid));
    ASSERT((slot >= 0) && (slot < base->depc));
    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    self->signalers[slot].guid = data.guid;
    self->signalers[slot].mode = mode;
    hal_fence();
    u32 oldValue = hal_xadd32(&(self->slotSatisfiedCount), 1);

#ifdef REG_ASYNC_SGL_DEBUG
    DPRINTF(DEBUG_LVL_WARN, "Satisfied task oldValue is %"PRIu32" and depc is %"PRIu32"\n", oldValue, base->depc);
#endif
    if (oldValue == (base->depc-1)) {
#ifdef REG_ASYNC_SGL_DEBUG
        DPRINTF(DEBUG_LVL_WARN, "all deps known from satisfy\n");
#endif
        // All dependences known
        ASSERT(self->slotSatisfiedCount = base->depc);
        taskAllDepvSatisfied(base);
    }
    return 0;
}

u8 satisfyTaskHc(ocrTask_t * base, ocrFatGuid_t data, u32 slot) {
    ASSERT(false && "mode required for satisfy in REG_ASYNC_SGL");
    return 0;
}

u8 registerSignalerTaskHc(ocrTask_t * base, ocrFatGuid_t signalerGuid, u32 slot,
                            ocrDbAccessMode_t mode, bool isDepAdd) {
    ASSERT(false && "no signaler registration in REG_ASYNC_SGL");
    return 0;
}

#else

#ifndef REG_ASYNC

u8 satisfyTaskHc(ocrTask_t * base, ocrFatGuid_t data, u32 slot) {
    // An EDT has a list of signalers, but only registers
    // incrementally as signals arrive AND on non-persistent
    // events (latch or ONCE)
    // Assumption: signal frontier is initialized at slot zero
    // Whenever we receive a signal:
    //  - it can be from the frontier (we registered on it)
    //  - it can be a ONCE event
    //  - it can be a data-block being added (causing an immediate satisfy)

    ocrTaskHc_t * self = (ocrTaskHc_t *) base;

    // Replace the signaler's guid by the data guid, this is to avoid
    // further references to the event's guid, which is good in general
    // and crucial for once-event since they are being destroyed on satisfy.
    hal_lock(&(self->lock));
    DPRINTF(DEBUG_LVL_INFO,
            "Satisfy on task "GUIDF" slot %"PRId32" with "GUIDF" slotSatisfiedCount=%"PRIu32" frontierSlot=%"PRIu32" depc=%"PRIu32"\n",
            GUIDA(self->base.guid), slot, GUIDA(data.guid), self->slotSatisfiedCount, self->frontierSlot, base->depc);
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_SATISFY, traceTaskSatisfyDependence, self->base.guid, data.guid);

    // Check to see if not already satisfied
    ASSERT_BLOCK_BEGIN(self->signalers[slot].slot != SLOT_SATISFIED_EVT)
    ocrTask_t * taskPut = NULL;
    getCurrentEnv(NULL, NULL, &taskPut, NULL);
    DPRINTF(DEBUG_LVL_WARN, "detected double satisfy on sticky for task "GUIDF" on slot %"PRId32" by "GUIDF"\n", GUIDA(base->guid), slot, GUIDA(taskPut->guid));
    ASSERT_BLOCK_END
    ASSERT(self->slotSatisfiedCount < base->depc);

    self->slotSatisfiedCount++;
    // If a valid DB is expected, assign the GUID
    if(self->signalers[slot].mode == DB_MODE_NULL)
        self->signalers[slot].guid = NULL_GUID;
    else
        self->signalers[slot].guid = data.guid;

    if(self->slotSatisfiedCount == base->depc) {
        DPRINTF(DEBUG_LVL_VERB, "Scheduling task "GUIDF", satisfied dependences %"PRId32"/%"PRId32"\n",
                GUIDA(self->base.guid), self->slotSatisfiedCount , base->depc);

        hal_unlock(&(self->lock));
        // All dependences have been satisfied, schedule the edt
        RESULT_PROPAGATE(taskAllDepvSatisfied(base));
    } else {
        // Decide to keep both SLOT_SATISFIED_DB and SLOT_SATISFIED_EVT to be able to
        // disambiguate between events and db satisfaction. Not strictly necessary but
        // nice to have for debug.
        if (self->signalers[slot].slot != SLOT_SATISFIED_DB) {
            self->signalers[slot].slot = SLOT_SATISFIED_EVT;
        }
        // When we're here we can make few assumptions about the frontier.
        // - If the frontier is greater than the current slot, then it was
        //   a dependence registration carrying a DB that marked the slot
        //   as SLOT_SATISFIED_DB. The satisfy still needs to happen, but
        //   it's already marked to let the frontier progress faster.
        ASSERT((self->frontierSlot > slot) ? (self->signalers[slot].slot == SLOT_SATISFIED_DB) : 1);
        // - The frontier is less than the current slot (the satisfy is ahead of the frontier). We just
        // need to mark down the slot as satisfied. These would have to be either ephemeral event or direct dbs.
        ASSERT((self->frontierSlot < slot) ?
               ((self->signalers[slot].slot == SLOT_SATISFIED_DB) ||
                (self->signalers[slot].slot == SLOT_SATISFIED_EVT)) : 1);
        // - The frontier is equal to the current slot, we need to iterate
        if (slot == self->frontierSlot) { // we are on the frontier slot
            // Try to advance the frontier over all consecutive satisfied events
            // and DB dependence that may be in flight (safe because we have the lock)
            u32 fsSlot = 0;
            bool cond = true;
            while ((self->frontierSlot != (base->depc-1)) && cond) {
                self->frontierSlot++;
                DPRINTF(DEBUG_LVL_VERB, "Slot Increment on task "GUIDF" slot %"PRId32" with "GUIDF" slotCount=%"PRIu32" slotFrontier=%"PRIu32" depc=%"PRIu32"\n",
                    GUIDA(self->base.guid), slot, GUIDA(data.guid), self->slotSatisfiedCount, self->frontierSlot, base->depc);
                ASSERT(self->frontierSlot < base->depc);
                fsSlot = self->signalers[self->frontierSlot].slot;
                cond = ((fsSlot == SLOT_SATISFIED_EVT) || (fsSlot == SLOT_SATISFIED_DB));
            }
            // If here, there must be that at least one satisfy hasn't happened yet.
            ASSERT(self->slotSatisfiedCount < base->depc);
            // The slot we found is either:
            // 1- not known: addDependence hasn't occured yet (UNINITIALIZED_GUID)
            // 2- known: but the edt hasn't registered on it yet
            // 3- a once event not yet satisfied: (.slot == SLOT_REGISTERED_EPHEMERAL_EVT, registered but not yet satisfied)
            // Note: the "last" dependence, which is either one of the above or has already been satisfied.
            //       Note that if it's a pure data dependence (SLOT_SATISFIED_DB), the operation may still be in flight.
            //       Its .slot has been set, which is why we skipped over its slot but the corresponding satisfy hasn't
            //       been executed yet. When it is, slotSatisfiedCount will equal depc and the task will be scheduled.
            if ((!(ocrGuidIsUninitialized(self->signalers[self->frontierSlot].guid))) &&
                (self->signalers[self->frontierSlot].slot == self->frontierSlot)) {
                ocrPolicyDomain_t *pd = NULL;
                PD_MSG_STACK(msg);
                getCurrentEnv(&pd, NULL, NULL, &msg);
 #ifdef OCR_ASSERT
                // Just for debugging purpose
                ocrFatGuid_t signalerGuid;
                signalerGuid.guid = self->signalers[self->frontierSlot].guid;
                // Warning double check if that works for regular implementation
                signalerGuid.metaDataPtr = NULL; // should be ok because guid encodes the kind in distributed
                ocrGuidKind signalerKind = OCR_GUID_NONE;
                deguidify(pd, &signalerGuid, &signalerKind);
                bool cond = (signalerKind == OCR_GUID_EVENT_STICKY) || (signalerKind == OCR_GUID_EVENT_IDEM);
#ifdef ENABLE_EXTENSION_COUNTED_EVT
                cond |= (signalerKind == OCR_GUID_EVENT_COUNTED);
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
                cond |= (signalerKind == OCR_GUID_EVENT_CHANNEL);
#endif
                ASSERT(cond);
#endif
                hal_unlock(&(self->lock));
                // Case 2: A sticky, the EDT registers as a lazy waiter
                // Here it should be ok to read the frontierSlot since we are on the frontier
                // only a satisfy on the event in that slot can advance the frontier and we
                // haven't registered on it yet.
                u8 res = registerOnFrontier(self, pd, &msg, self->frontierSlot);
                return res;
            }
            //else:
            // case 1, registerSignaler will do the registration
            // case 3, just have to wait for the satisfy on the once event to happen.
        }
        //else: not on frontier slot, nothing to do
        // Two cases:
        // - The slot has just been marked as satisfied but the frontier
        //   hasn't reached that slot yet. Most likely the satisfy is on
        //   an ephemeral event or directly with a db. The frontier will
        //   eventually reach this slot at a later point.
        // - There's a race between 'register' setting the .slot to the DB guid
        //   and a concurrent satisfy incrementing the frontier. i.e. it skips
        //   over the DB guid because its .slot is 'SLOT_SATISFIED_DB'.
        //   When the DB satisfy happens it falls-through here.
        hal_unlock(&(self->lock));
    }
    return 0;
}

/**
 * Can be invoked concurrently, however each invocation should be for a different slot
 */
u8 registerSignalerTaskHc(ocrTask_t * base, ocrFatGuid_t signalerGuid, u32 slot,
                            ocrDbAccessMode_t mode, bool isDepAdd) {
    ASSERT(isDepAdd); // This should only be called when adding a dependence

    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    ocrGuidKind signalerKind = OCR_GUID_NONE;
    deguidify(pd, &signalerGuid, &signalerKind);
    regNode_t * node = &(self->signalers[slot]);
    hal_lock(&(self->lock));
    node->mode = mode;
    ASSERT_BLOCK_BEGIN(node->slot < base->depc);
    DPRINTF(DEBUG_LVL_WARN, "User-level error detected: add dependence slot is out of bounds: EDT="GUIDF" slot=%"PRIu32" depc=%"PRIu32"\n",
                            GUIDA(base->guid), slot, base->depc);
    ASSERT_BLOCK_END
    ASSERT(!(ocrGuidIsNull(signalerGuid.guid))); // This should have been caught earlier on
    ASSERT(node->slot == slot); // assumption from initialization
    node->guid = signalerGuid.guid;
    //BUG #162 metadata cloning: Had to introduce new kinds of guids because we don't
    //         have support for cloning metadata around yet
    if(signalerKind & OCR_GUID_EVENT) {
        bool cond = (signalerKind == OCR_GUID_EVENT_ONCE) || (signalerKind == OCR_GUID_EVENT_LATCH);
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
        cond = cond || (signalerKind == OCR_GUID_EVENT_CHANNEL);
#endif
        if(cond) {
            node->slot = SLOT_REGISTERED_EPHEMERAL_EVT; // To record this slot is for a once event
            hal_unlock(&(self->lock));
        } else {
            // Must be a sticky event. Read the frontierSlot now that we have the lock.
            // If 'register' is on the frontier, do the registration. Otherwise the edt
            // will lazily register on the signalerGuid when the frontier reaches the
            // signaler's slot.
            bool doRegister = (slot == self->frontierSlot);
            hal_unlock(&(self->lock));
            if(doRegister) {
                // The EDT registers itself as a waiter here
                ocrPolicyDomain_t *pd = NULL;
                PD_MSG_STACK(msg);
                getCurrentEnv(&pd, NULL, NULL, &msg);
                RESULT_PROPAGATE(registerOnFrontier(self, pd, &msg, slot));
            }
        }
    } else {
        ASSERT(signalerKind == OCR_GUID_DB);
        // Here we could use SLOT_SATISFIED_EVT directly, but if we do,
        // when satisfy is called we won't be able to figure out if the
        // value was set for a DB here, or by a previous satisfy.
        node->slot = SLOT_SATISFIED_DB;
        // Setting the slot and incrementing the frontier in two steps
        // introduce a race between the satisfy here after and another
        // concurrent satisfy advancing the frontier.
        hal_unlock(&(self->lock));
        //Convert to a satisfy now that we've recorded the mode
        //NOTE: Could improve implementation by figuring out how
        //to properly iterate the frontier when adding the DB
        //potentially concurrently with satifies.
        PD_MSG_STACK(registerMsg);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, &registerMsg);
        ocrFatGuid_t currentEdt;
        currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
        currentEdt.metaDataPtr = curTask;
    #define PD_MSG (&registerMsg)
    #define PD_TYPE PD_MSG_DEP_SATISFY
        registerMsg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(satisfierGuid) = currentEdt;
        PD_MSG_FIELD_I(guid.guid) = base->guid;
        PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(payload.guid) = signalerGuid.guid;
        PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(currentEdt) = currentEdt;
        PD_MSG_FIELD_I(slot) = slot;
        PD_MSG_FIELD_I(properties) = 0;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &registerMsg, true));
    #undef PD_MSG
    #undef PD_TYPE
    }

    DPRINTF(DEBUG_LVL_INFO, "AddDependence from "GUIDF" to "GUIDF" slot %"PRId32"\n",
        GUIDA(signalerGuid.guid), GUIDA(base->guid), slot);
    return 0;
}

#else /* REG_ASYNC */

u8 satisfyTaskHc(ocrTask_t * base, ocrFatGuid_t data, u32 slot) {
    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    self->signalers[slot].guid = data.guid;
    hal_fence();
    u32 oldValue = hal_xadd32(&(self->slotSatisfiedCount), 1);
#ifdef REG_ASYNC_SGL_DEBUG
    DPRINTF(DEBUG_LVL_WARN, "Satisfied task oldValue is %"PRIu32" and depc is %"PRIu32"\n", oldValue, base->depc);
#endif
    if (oldValue == ((base->depc*2)-1)) {
#ifdef REG_ASYNC_SGL_DEBUG
        DPRINTF(DEBUG_LVL_WARN, "all deps known from satisfy\n");
#endif
        // All dependences known
        self->slotSatisfiedCount = base->depc;
        taskAllDepvSatisfied(base);
    }
    return 0;
}

u8 registerSignalerTaskHc(ocrTask_t * base, ocrFatGuid_t signalerGuid, u32 slot,
                            ocrDbAccessMode_t mode, bool isDepAdd) {
    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    self->signalers[slot].mode = mode;
    hal_fence();
    u32 oldValue = hal_xadd32(&(self->slotSatisfiedCount), 1);
#ifdef REG_ASYNC_SGL_DEBUG
    DPRINTF(DEBUG_LVL_WARN, "Registered on task oldValue is %"PRIu32" and depc is %"PRIu32"\n", oldValue, base->depc);
#endif
    if (oldValue == ((base->depc*2)-1)) {
#ifdef REG_ASYNC_SGL_DEBUG
        DPRINTF(DEBUG_LVL_WARN, "all deps known from registerSignaler\n");
#endif
        // All dependences known
        self->slotSatisfiedCount = base->depc;
        taskAllDepvSatisfied(base);
    }
    return 0;
}

#endif /* REG_ASYNC */

#endif /* not REG_ASYNC_SGL */

u8 unregisterSignalerTaskHc(ocrTask_t * base, ocrFatGuid_t signalerGuid, u32 slot, bool isDepRem) {
    ASSERT(0); // We don't support this at this time...
    return 0;
}

u8 notifyDbAcquireTaskHc(ocrTask_t *base, ocrFatGuid_t db) {
    // This implementation does NOT support EDTs moving while they are executing
    ocrTaskHc_t *derived = (ocrTaskHc_t*)base;
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if(derived->maxUnkDbs == 0) {
        derived->unkDbs = (ocrGuid_t*)pd->fcts.pdMalloc(pd, sizeof(ocrGuid_t)*8);
        derived->maxUnkDbs = 8;
    } else {
        if(derived->maxUnkDbs == derived->countUnkDbs) {
            ocrGuid_t *oldPtr = derived->unkDbs;
            derived->unkDbs = (ocrGuid_t*)pd->fcts.pdMalloc(pd, sizeof(ocrGuid_t)*derived->maxUnkDbs*2);
            ASSERT(derived->unkDbs);
            hal_memCopy(derived->unkDbs, oldPtr, sizeof(ocrGuid_t)*derived->maxUnkDbs, false);
            pd->fcts.pdFree(pd, oldPtr);
            derived->maxUnkDbs *= 2;
        }
    }
    // Tack on this DB
    derived->unkDbs[derived->countUnkDbs] = db.guid;
    ++derived->countUnkDbs;
    DPRINTF(DEBUG_LVL_VERB, "EDT (GUID: "GUIDF") added DB (GUID: "GUIDF") to its list of dyn. acquired DBs (have %"PRId32")\n",
            GUIDA(base->guid), GUIDA(db.guid), derived->countUnkDbs);
    return 0;
}

u8 notifyDbReleaseTaskHc(ocrTask_t *base, ocrFatGuid_t db) {
    ocrTaskHc_t *derived = (ocrTaskHc_t*)base;
    if ((derived->unkDbs != NULL) || (base->depc != 0)) {
        // Search in the list of DBs created by the EDT
        u64 maxCount = derived->countUnkDbs;
        u64 count = 0;
        DPRINTF(DEBUG_LVL_VERB, "Notifying EDT (GUID: "GUIDF") that it released db (GUID: "GUIDF")\n",
                GUIDA(base->guid), GUIDA(db.guid));
        while(count < maxCount) {
            // We bound our search (in case there is an error)
            if(ocrGuidIsEq(db.guid, derived->unkDbs[count])) {
                DPRINTF(DEBUG_LVL_VVERB, "Dynamic Releasing DB @ %p (GUID "GUIDF") from EDT "GUIDF", match in unkDbs list for count %"PRIu64"\n",
                       db.metaDataPtr, GUIDA(db.guid), GUIDA(base->guid), count);
                derived->unkDbs[count] = derived->unkDbs[maxCount - 1];
                --(derived->countUnkDbs);
                return 0;
            }
            ++count;
        }

        // Search DBs in dependences
        maxCount = base->depc;
        count = 0;
        while(count < maxCount) {
            // We bound our search (in case there is an error)
            if(ocrGuidIsEq(db.guid, derived->resolvedDeps[count].guid)) {
                DPRINTF(DEBUG_LVL_VVERB, "Dynamic Releasing DB (GUID "GUIDF") from EDT "GUIDF", "
                        "match in dependence list for count %"PRIu64"\n",
                        GUIDA(db.guid), GUIDA(base->guid), count);
                // If the below asserts, rebuild OCR with a higher OCR_MAX_MULTI_SLOT (in build/common.mk)
                ASSERT(count / 64 < OCR_MAX_MULTI_SLOT);
                if(derived->doNotReleaseSlots[count / 64 ] & (1ULL << (count % 64))) {
                    DPRINTF(DEBUG_LVL_VVERB, "DB (GUID "GUIDF") already released from EDT "GUIDF" (dependence %"PRIu64")\n",
                            GUIDA(db.guid), GUIDA(base->guid), count);
                    return OCR_ENOENT;
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "Dynamic Releasing DB (GUID "GUIDF") from EDT "GUIDF", "
                            "match in dependence list for count %"PRIu64"\n",
                            GUIDA(db.guid), GUIDA(base->guid), count);

                    derived->doNotReleaseSlots[count / 64] |= (1ULL << (count % 64));
                    // we can return on the first instance found since iterateDbFrontier
                    // already marked duplicated DB and the selection sort in sortRegNode is stable.
                    return 0;
                }
            }
            ++count;
        }
    }
    // not found means it's an error or it has already been released
    return OCR_ENOENT;
}

#ifdef ENABLE_OCR_API_DEFERRABLE
static void deferredExecute(ocrTask_t* self) {
    ocrTaskHc_t* dself = (ocrTaskHc_t*)self;
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    if (dself->evtHead) {
        ocrPolicyDomain_t * pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        // Make the head MT of the deferred API calls ready and eligible for scheduling
        RESULT_ASSERT(pdMarkReadyEvent(pd, dself->evtHead), ==, 0);
    }
#else
    u32 i = 0;
    if (dself->evts) {
        ocrPolicyDomain_t * pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        u32 ub = queueGetSize(dself->evts);
        while (i < ub) {
            pdEventMsg_t * evt = queueGet(dself->evts, i);
            DPRINTF(DEBUG_LVL_VERB, "[DFRD] Executing msg type=0x%"PRIx32"\n", evt->msg->type);
            // This is not the right way to use MT...
            // pd->fcts.processEvent(pd, (pdEvent_t **)&evt, 0);
            pd->fcts.processMessage(pd, evt->msg, true);
            i++;
        }
        queueDestroy(dself->evts);
    }
#endif
}
#endif

//TODO-DEFERRED: These operations depend on the state of the EDT after the user code has
//been fully executed. Either we 1) postpone the whole epilogue to happen after all the deferred
//operations or 2) we'd need to do some of the operation's book-keeping that have side-effects.
//Go with the first one as it naturally fits in the deferred model.
//Ideally this would be a MT-continuation but we can also make it some kind of a micro-task callback
//to be enqueued after all the deferred operations
static u8 taskEpilogue(ocrTask_t * base, ocrPolicyDomain_t *pd, ocrWorker_t * curWorker, ocrGuid_t retGuid) {
    ocrTaskHc_t * derived = (ocrTaskHc_t *) base;
    u32 depc = base->depc;
    ocrEdtDep_t * depv = derived->resolvedDeps;
    ocrFatGuid_t currentEdt = {.guid = base->guid, .metaDataPtr = base};
    PD_MSG_STACK(msg);
    START_PROFILE(ta_hc_executeCleanup);
#ifdef OCR_ENABLE_STATISTICS
    ocrPolicyCtx_t *ctx = getCurrentWorkerContext();
    // We now say that the worker is done executing the EDT
    statsEDT_END(pd, ctx->sourceObj, curWorker, base->guid, base);
#endif /* OCR_ENABLE_STATISTICS */
    DPRINTF(DEBUG_LVL_INFO, "End_Execution "GUIDF"\n", GUIDA(base->guid));
#if !defined(OCR_ENABLE_SIMULATOR)
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_EDT, OCR_ACTION_FINISH, traceTaskFinish, base->guid);
#endif
    // edt user code is done, if any deps, release data-blocks
    if(depc != 0) {
        START_PROFILE(ta_hc_dbRel);
        u32 i;
        for(i=0; i < depc; ++i) {
            u32 j = i / 64;
            if ((!(ocrGuidIsNull(depv[i].guid))) &&
                ((j >= OCR_MAX_MULTI_SLOT) || (derived->doNotReleaseSlots[j] == 0) ||
                 ((j < OCR_MAX_MULTI_SLOT) && (((1ULL << (i % 64)) & derived->doNotReleaseSlots[j]) == 0)))) {
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
                msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = depv[i].guid;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(edt) = currentEdt;
                PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
                PD_MSG_FIELD_I(ptr) = NULL;
                PD_MSG_FIELD_I(size) = 0;
                PD_MSG_FIELD_I(properties) = 0;
                // Ignore failures at this point
                pd->fcts.processMessage(pd, &msg, true);
#undef PD_MSG
#undef PD_TYPE
            }
        }
        pd->fcts.pdFree(pd, depv);
        EXIT_PROFILE;
    }

    // We now release all other data-blocks that we may potentially
    // have acquired along the way
    if(derived->unkDbs != NULL) {
        ocrGuid_t *extraToFree = derived->unkDbs;
        u64 count = derived->countUnkDbs;
        while(count) {
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
            msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_IO(guid.guid) = extraToFree[0];
            PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(edt) = currentEdt;
            PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
            PD_MSG_FIELD_I(ptr) = NULL;
            PD_MSG_FIELD_I(size) = 0;
            PD_MSG_FIELD_I(properties) = 0; // Not a runtime free xosince it was acquired using DB create
            if(pd->fcts.processMessage(pd, &msg, true)) {
                DPRINTF(DEBUG_LVL_WARN, "EDT (GUID: "GUIDF") could not release dynamically acquired DB (GUID: "GUIDF")\n",
                        GUIDA(base->guid), GUIDA(extraToFree[0]));
                break;
            }
#undef PD_MSG
#undef PD_TYPE
            --count;
            ++extraToFree;
        }
        pd->fcts.pdFree(pd, derived->unkDbs);
    }
    // If marked to be rescheduled, do not satisfy output
    // event and do not update the task state to reaping
    if (base->state == RUNNING_EDTSTATE) {
        bool hasParent = !ocrGuidIsNull(base->parentLatch);
        bool hasFinish = !ocrGuidIsNull(base->finishLatch);
        bool hasOutputEvent = !ocrGuidIsNull(base->outputEvent);
        if(hasFinish) {
            DPRINTF(DEBUG_LVL_VVERB, "EDT epilogue: satisfy finish scope "GUIDF"\n", GUIDA(base->finishLatch));
            // If we have a finish scope, just need to satisfy that event, which
            // does take care of, if any, the parent latch and maybe the output event.
            getCurrentEnv(NULL, NULL, NULL, &msg);
            ocrFatGuid_t finishLatchFGuid = {.guid = base->finishLatch, .metaDataPtr = NULL};
            ocrFatGuid_t retFGuid = {.guid = retGuid, .metaDataPtr = NULL};
            RESULT_PROPAGATE(doSatisfy(pd, &msg, currentEdt, finishLatchFGuid, retFGuid, OCR_EVENT_LATCH_DECR_SLOT));
            // If the finish scope is really a proxy scope, i.e. we conserved the parent latch GUID,
            // we also need to satisfy the output event.
            bool isProxyFinish = (hasParent && !isLocalGuid(pd, base->parentLatch));
            if (isProxyFinish && hasOutputEvent) {
                DPRINTF(DEBUG_LVL_VVERB, "EDT epilogue: satisfy output event "GUIDF"\n", GUIDA(base->outputEvent));
                getCurrentEnv(NULL, NULL, NULL, &msg);
                ocrFatGuid_t outputEventFGuid = {.guid = base->outputEvent, .metaDataPtr = NULL};
                doAddDep(pd, &msg, currentEdt, retFGuid, outputEventFGuid, 0);
            }
        } else if (hasParent) {
            DPRINTF(DEBUG_LVL_VVERB, "EDT epilogue: satisfy parent finish scope "GUIDF"\n", GUIDA(base->finishLatch));
            // If we have a parent latch but no finish scope, satisfy its decrement slot.
            ASSERT(ocrGuidIsNull(base->finishLatch));
            ASSERT(isLocalGuid(pd, base->parentLatch)); // By construction for perfs
            getCurrentEnv(NULL, NULL, NULL, &msg);
            ocrFatGuid_t parentLatchFGuid = {.guid = base->parentLatch, .metaDataPtr = NULL};
            ocrFatGuid_t nullFGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
            RESULT_PROPAGATE(doSatisfy(pd, &msg, currentEdt, parentLatchFGuid, nullFGuid, OCR_EVENT_LATCH_DECR_SLOT));
            // Also take care of satisfying the output event
            if (hasOutputEvent) {
                DPRINTF(DEBUG_LVL_VVERB, "EDT epilogue: satisfy output event "GUIDF"\n", GUIDA(base->outputEvent));
                getCurrentEnv(NULL, NULL, NULL, &msg);
                ocrFatGuid_t outputEventFGuid = {.guid = base->outputEvent, .metaDataPtr = NULL};
                ocrFatGuid_t retFGuid = {.guid = retGuid, .metaDataPtr = NULL};
                doAddDep(pd, &msg, currentEdt, retFGuid, outputEventFGuid, 0);
            }
        } else if (hasOutputEvent) {
            DPRINTF(DEBUG_LVL_VVERB, "EDT epilogue: satisfy output event "GUIDF"\n", GUIDA(base->outputEvent));
            // No finish scope nor parent latch, just need to satisfy the output ovent
            ASSERT(ocrGuidIsNull(base->finishLatch));
            ASSERT(ocrGuidIsNull(base->parentLatch));
            getCurrentEnv(NULL, NULL, NULL, &msg);
            ocrFatGuid_t outputEventFGuid = {.guid = base->outputEvent, .metaDataPtr = NULL};
            ocrFatGuid_t retFGuid = {.guid = retGuid, .metaDataPtr = NULL};
            doAddDep(pd, &msg, currentEdt, retFGuid, outputEventFGuid, 0);
        }
        // Because the output event is non-persistent it is deallocated automatically
        base->outputEvent = NULL_GUID;
        base->state = REAPING_EDTSTATE;
    }
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    else { // else EDT must be rescheduled
        ASSERT(base->state == RESCHED_EDTSTATE);
        ASSERT(base->depc == 0); //Limitation
    }
#endif
    EXIT_PROFILE;

//TODO-DEFERRED: In non-deferred this is in the worker code after task->execute. Pondering if that
//should be enqueued by the worker on an event/strand that represents the task being done ?
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    START_PROFILE(wo_hc_wrapupWork);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_NOTIFY
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_EDT_DONE;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid.guid = base->guid;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid.metaDataPtr = base;
    RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
    EXIT_PROFILE;
#undef PD_MSG
#undef PD_TYPE
#endif

    return 0;
}

u8 taskExecute(ocrTask_t* base) {
    START_PROFILE(ta_hc_execute);
    ocrTaskHc_t* derived = (ocrTaskHc_t*)base;
    // In this implementation each time a signaler has been satisfied, its guid
    // has been replaced by the db guid it has been satisfied with.
    u32 paramc = base->paramc;
    u64 * paramv = base->paramv;
    u32 depc = base->depc;
    ocrPolicyDomain_t *pd = NULL;
    ocrWorker_t *curWorker = NULL;
    getCurrentEnv(&pd, &curWorker, NULL, NULL);
    ocrEdtDep_t * depv = derived->resolvedDeps;
    {
        START_PROFILE(ta_hc_executeSetup);
        // Deal with finish scopes
        // If creating a new finish scope
        //   Create a new latch event
        //   If there's a parent latch
        //     Checkin with the new latch event
        //     Note there's no need to check-in with the parent latch since it is done on EDT's creation.
        //     Add dependence between the new finish scope and the parent latch decr
        // Else
        //   If there's a parent latch
        //     If it is remote
        //       Create a new latch event
        //       Incr by one
        //       Add dependence between the new latch and my output event
        //       Add dependence between the new latch and the parent latch
        //     Else
        //       Nothing to do as the parent latch is incremented on creation
        ocrGuid_t taskGuid = base->guid;
        bool hasOutputEvent = !ocrGuidIsNull(base->outputEvent);
        bool hasFinish = ocrGuidIsUninitialized(base->finishLatch);
        bool hasParent = !ocrGuidIsNull(base->parentLatch);
        bool hasLocalParent = hasParent && isLocalGuid(pd, base->parentLatch);
        bool needProxy = (hasParent && !hasLocalParent);

        if (hasFinish || needProxy) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
            ocrFatGuid_t currentEdt;
            currentEdt.guid = taskGuid;
            currentEdt.metaDataPtr = base;
            ocrFatGuid_t newFinishLatchFGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
            msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_IO(guid) = newFinishLatchFGuid;
            PD_MSG_FIELD_I(currentEdt) = currentEdt;
            PD_MSG_FIELD_I(type) = OCR_EVENT_LATCH_T;
#ifdef ENABLE_EXTENSION_PARAMS_EVT
            ocrEventParams_t latchParams;
            latchParams.EVENT_LATCH.counter = 1;
            PD_MSG_FIELD_I(params) = &latchParams;
#endif
            PD_MSG_FIELD_I(properties) = 0;
            RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
            newFinishLatchFGuid = PD_MSG_FIELD_IO(guid);
            ASSERT(!(ocrGuidIsNull(newFinishLatchFGuid.guid)) && newFinishLatchFGuid.metaDataPtr != NULL);
            base->finishLatch = newFinishLatchFGuid.guid;
#undef PD_MSG
#undef PD_TYPE
#ifndef ENABLE_EXTENSION_PARAMS_EVT
            // Account for this EDT as being part of its local finish scope
            getCurrentEnv(NULL, NULL, NULL, &msg);
            ocrFatGuid_t nullFGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
            RESULT_PROPAGATE(doSatisfy(pd, &msg, currentEdt, newFinishLatchFGuid, nullFGuid, OCR_EVENT_LATCH_INCR_SLOT));
#endif
            // Link the new finish scope to the parent finish scope
            if (hasParent) {
                DPRINTF(DEBUG_LVL_VVERB, "EDT check in on parent finish latch "GUIDF"\n", GUIDA(base->parentLatch));
                getCurrentEnv(NULL, NULL, NULL, &msg);
#if defined(TG_X86_TARGET) || defined(TG_XE_TARGET) || defined(TG_CE_TARGET)
                // On TG, the event is created by the CE? and the XE does the satisfy
                // Shouldn't that become blocking ?
#else
                ASSERT(isLocalGuid(pd, newFinishLatchFGuid.guid)); // By construction for perfs
#endif
                ocrFatGuid_t parentLatchFGuid = {.guid = base->parentLatch, .metaDataPtr = NULL};
                doAddDep(pd, &msg, currentEdt, newFinishLatchFGuid, parentLatchFGuid, OCR_EVENT_LATCH_DECR_SLOT);
            }
            if (hasFinish) {
                // Now that we've setup everything, if we're defining a new finish scope,
                // we forget we had a parent. This is essentially a sentinel value that
                // helps disambiguate which actions need to be carried out in the epilogue.
                base->parentLatch = NULL_GUID;
                if (hasOutputEvent) {
                    // Link the new finish scope to the output event only if it's not a proxy finish scope
                    // Otherwise we would unnecessarily delay the output event satisfaction to the proxy scope.
                    DPRINTF(DEBUG_LVL_VVERB, "Link finish scope "GUIDF" to output event "GUIDF"\n", GUIDA(newFinishLatchFGuid.guid), GUIDA(base->outputEvent));
                    getCurrentEnv(NULL, NULL, NULL, &msg);
                    ocrFatGuid_t outputEventFGuid = {.guid = base->outputEvent, .metaDataPtr = NULL};
                    doAddDep(pd, &msg, currentEdt, newFinishLatchFGuid, outputEventFGuid, 0);
                }
            }
        }

#ifdef NANNYMODE_SAFE_DEPV
        ocrEdtDep_t defensiveDepv[depc];
        hal_memCopy(defensiveDepv, depv, sizeof(ocrEdtDep_t)*depc, false);
#endif
        base->state = RUNNING_EDTSTATE;

        //TODO Execute can be considered user on x86, but need to differentiate processRequestEdts in x86-mpi
        DPRINTF(DEBUG_LVL_VERB, "Execute "GUIDF" paramc:%"PRId32" depc:%"PRId32"\n", GUIDA(base->guid), base->paramc, base->depc);
        OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_EDT, OCR_ACTION_EXECUTE, traceTaskExecute, base->guid, base->funcPtr, depc, paramc, paramv);

        ASSERT(derived->unkDbs == NULL); // Should be no dynamically acquired DBs before running

#ifdef OCR_ENABLE_STATISTICS
        // Bug #225
        ocrPolicyCtx_t *ctx = getCurrentWorkerContext();
        ocrWorker_t *curWorker = NULL;

        deguidify(pd, ctx->sourceObj, (u64*)&curWorker, NULL);

        // We first have the message of using the EDT Template
        statsTEMP_USE(pd, base->guid, base, taskTemplate->guid, taskTemplate);

        // We now say that the worker is starting the EDT
        statsEDT_START(pd, ctx->sourceObj, curWorker, base->guid, base, depc != 0);

#endif /* OCR_ENABLE_STATISTICS */
        EXIT_PROFILE;
    }
    ocrGuid_t retGuid = NULL_GUID;
    {

#ifdef OCR_ENABLE_VISUALIZER
        u64 startTime = salGetTime();
#endif
#ifdef OCR_TRACE
// ifdef because the compiler doesn't get rid off this call
// even when subsequent TPRINTF end up not being compiled
        char location[32];
        curWorker->fcts.printLocation(curWorker, &(location[0]));
#endif
#ifdef OCR_ENABLE_EDT_NAMING
        TPRINTF("EDT Start: %s 0x%"PRIx64" in %s\n",
                base->name, base->guid, location);
#else
        TPRINTF("EDT Start: 0x%"PRIx64" 0x%"PRIx64" in %s\n",
                base->funcPtr, base->guid, location);
#endif
#ifdef NANNYMODE_SAFE_DEPV
        depv = defensiveDepv;
#endif
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
        if(base->funcPtr == &processRequestEdt) {
            START_PROFILE(procIncMsg);
            retGuid = base->funcPtr(paramc, paramv, depc, depv);
            EXIT_PROFILE;
        } else {
            START_PROFILE(userCode);
            retGuid = base->funcPtr(paramc, paramv, depc, depv);
            EXIT_PROFILE;
        }
#else
        START_PROFILE(userCode);
        retGuid = base->funcPtr(paramc, paramv, depc, depv);
        EXIT_PROFILE;
#endif /* ENABLE_POLICY_DOMAIN_HC_DIST */
#ifdef OCR_ENABLE_EDT_NAMING
        TPRINTF("EDT End: %s 0x%"PRIx64" in %s\n",
                base->name, base->guid, location);
#else
        TPRINTF("EDT End: 0x%"PRIx64" 0x%"PRIx64" in %s\n",
                base->funcPtr, base->guid, location);
#endif

#ifdef NANNYMODE_SAFE_DEPV
        depv = derived->resolvedDeps;
        u32 ch = 0;
        while (ch < depc) {
            if (memcmp((char *)&defensiveDepv[ch], (char *)&depv[ch], sizeof(ocrEdtDep_t)) != 0) {
                DPRINTF(DEBUG_LVL_WARN, "Warning: EDT %s "GUIDF" had depv[%"PRIu32"] modified by user code",
#ifdef OCR_ENABLE_EDT_NAMING
                    base->name,
#else
                    "",
#endif
                    GUIDA(base->guid), ch);
                ASSERT(false);
            }
            ch++;
        }
#endif

#ifdef OCR_ENABLE_VISUALIZER
        u64 endTime = salGetTime();
        DPRINTF(DEBUG_LVL_INFO, "Execute "GUIDF" FctName: %s Start: %"PRIu64" End: %"PRIu64"\n", GUIDA(base->guid), base->name, startTime, endTime);
#endif
    }

#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    // For now we do post-EDT execution, hence mark the head event as being ready
    deferredExecute(base);
    //Create a processEvent function callback event
    pdEvent_t * callbackEvent;
    RESULT_ASSERT(pdCreateEvent(pd, &callbackEvent, PDEVT_TYPE_FCT, 0), ==, 0);
    //Record some calling context info
    pdEventFct_t * devt = ((pdEventFct_t *) callbackEvent);
    devt->id = CONTINUATION_EDT_EPILOGUE;
    devt->ctx = base;
    //TODO missing the DEEP_GC
    if (!ocrGuidIsNull(retGuid)) {
        ocrGuid_t * ptrRetGuid = pd->fcts.pdMalloc(pd, sizeof(ocrGuid_t));
        *ptrRetGuid = retGuid;
        devt->args = ptrRetGuid;
    } else {
        devt->args = NULL;
    }
    // Create the process event action and enqueue it to the callbackEvent's strand
    pdStrand_t * callbackStrand;
    RESULT_ASSERT(
        pdGetNewStrand(pd, &callbackStrand, pd->strandTables[PDSTT_EVT-1], callbackEvent, 0 /*unused*/),
        ==, 0);
    pdAction_t* callbackAction = pdGetProcessEventAction((ocrObject_t *) base);
    RESULT_ASSERT(
        pdEnqueueActions(pd, callbackStrand, 1, &callbackAction, true/*clear hold*/),
        ==, 0);
    RESULT_ASSERT(pdUnlockStrand(callbackStrand), ==, 0);
    // Chain the epilogue to the last deferred call
    pdStrand_t * oldTailStrand = derived->tailStrand;
    if(oldTailStrand) {
        pdAction_t* satisfyAction = pdGetMarkReadyAction(callbackEvent);
        RESULT_ASSERT(pdLockStrand(oldTailStrand, 0), ==, 0);
        RESULT_ASSERT(pdEnqueueActions(pd, oldTailStrand, 1, &satisfyAction, true/*clear hold*/), ==, 0);
        RESULT_ASSERT(pdUnlockStrand(oldTailStrand), ==, 0);
        derived->tailStrand = callbackStrand;
    } else {
        derived->evtHead = callbackEvent;
        deferredExecute(base);
    }
#else
    deferredExecute(base);
    taskEpilogue(base, pd, curWorker, retGuid);
#endif
#else
    taskEpilogue(base, pd, curWorker, retGuid);
#endif
    RETURN_PROFILE(0);
}

u8 processEventTaskHc(ocrObject_t *self, pdEvent_t** evt, u32 idx) {
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    DPRINTF(DEBUG_LVL_VVERB, "processEventTaskHc executing CONTINUATION_EDT_EPILOGUE\n");
    ASSERT((evt != NULL) && (*evt != NULL));
    pdEventFct_t * devt = (pdEventFct_t *) *evt;
    ASSERT(devt->id == CONTINUATION_EDT_EPILOGUE);
    ocrGuid_t * retGuid = devt->args;
    ocrPolicyDomain_t * pd;
    ocrWorker_t * curWorker;
    getCurrentEnv(&pd, &curWorker, NULL, NULL);
    curWorker->curTask = devt->ctx;
    taskEpilogue((ocrTask_t*)self, pd, curWorker, ((retGuid == NULL) ? NULL_GUID : retGuid[0]));
    curWorker->curTask = NULL;
    if (devt->args) {
        pd->fcts.pdFree(pd, devt->args);
    }
#endif
    return 0;
}


u8 setHintTaskHc(ocrTask_t* self, ocrHint_t *hint) {
    ocrTaskHc_t *derived = (ocrTaskHc_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_SET(hint, rHint, OCR_HINT_COUNT_EDT_HC, ocrHintPropTaskHc, OCR_HINT_EDT_PROP_START);
    return 0;
}

u8 getHintTaskHc(ocrTask_t* self, ocrHint_t *hint) {
    ocrTaskHc_t *derived = (ocrTaskHc_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_GET(hint, rHint, OCR_HINT_COUNT_EDT_HC, ocrHintPropTaskHc, OCR_HINT_EDT_PROP_START);
    return 0;
}

ocrRuntimeHint_t* getRuntimeHintTaskHc(ocrTask_t* self) {
    ocrTaskHc_t *derived = (ocrTaskHc_t*)self;
    return &(derived->hint);
}

#define MSG_MDCOMM_SZ       (_PD_MSG_SIZE_IN(PD_MSG_METADATA_COMM))

// Simple flat serialization
// #define SER_WRITE(dest, src, sz) (memcpy(dest, src, sz), ((char *)dest)+(sz))
#define SER_WRITE(dest, src, sz) {hal_memCopy(dest, src, sz, false); char * _tmp__ = ((char *) dest) + (sz); dest=_tmp__;}

//TODO-MD-EDT: I think we can pretty much automate all the serialization with some xmacro magic
// Compute the size of a serialized data-structure defined in 'self' (and derived)
#define SZ_PARAMV(self)         (sizeof(u64)*((ocrTask_t*)self)->paramc)
#define SZ_SIGNALERS(self)      (sizeof(regNode_t)*((ocrTask_t*)self)->depc)
#define SZ_HINTS(self)          ((hasProperty(((ocrTask_t*)self)->flags, OCR_TASK_FLAG_USES_HINTS) ? OCR_HINT_COUNT_EDT_HC : 0)*sizeof(u64))
#define SZ_UNKDBS(self)         (((ocrTaskHc_t *)self)->countUnkDbs*sizeof(ocrGuid_t))
#define SZ_RESOLVEDDEPS(self)   ((((ocrTaskHc_t *)self)->resolvedDeps == NULL) ? 0 : (((ocrTask_t*)self)->depc*sizeof(ocrEdtDep_t)))

// Computes the address of a data-structure embedded in 'self'
#define OFF_PARAMV(self)            (((char *) self) + sizeof(ocrTaskHc_t))
#define OFF_SIGNALERS(self)         (OFF_PARAMV(self) + SZ_PARAMV(self))
#define OFF_HINTS(self)             (OFF_SIGNALERS(self) + SZ_SIGNALERS(self))
#define OFF_UNKDBS(self)            (OFF_HINTS(self) + SZ_HINTS(self))
#define OFF_RESOLVEDDEPS(self)      (OFF_UNKDBS(self) + SZ_UNKDBS(self))

//TODO-MD-EDT:
//This is returning the size for a deep copy. Mode, whether it is an action or a type of size should reflect that.
u8 mdSizeTaskFactoryHc(ocrObject_t *dest, u64 mode, u64 * size) {
    ocrTask_t * self = (ocrTask_t *) dest;
    *size = sizeof(ocrTaskHc_t) +
        SZ_PARAMV(self) + SZ_SIGNALERS(self) + SZ_HINTS(self) + SZ_UNKDBS(self) + SZ_RESOLVEDDEPS(self);
    return 0;
}

u8 serializeTaskFactoryHc(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t * src, u64 * mode, ocrLocation_t destLocation, void ** destBuffer, u64 * destSize) {
    //TODO-MD-EDT: what's the mode here ?
    //NOTE: Don't really have a use for the destLocation here
    ASSERT(destBuffer != NULL);
    ocrTask_t * self = (ocrTask_t *) src;
#ifdef OCR_ASSERT
    u64 mdSizeCheck = 0; //TODO-MD-EDT mode
    mdSizeTaskFactoryHc((ocrObject_t *) self, 0, &mdSizeCheck);
    ASSERT(*destSize >= mdSizeCheck);
#endif
    ocrTaskHc_t * dself = (ocrTaskHc_t *) src;
    ASSERT(ocrGuidIsEq(guid, self->guid));
    ocrTaskHc_t * dst = (ocrTaskHc_t *) *destBuffer;
    *dst = *dself;
    dst->maxUnkDbs = dself->countUnkDbs;
    // Fixup embedded pointers and heap-allocated:
    // paramv | signalers | hintc | unkDbs | resolvedDeps
    char * cur = OFF_PARAMV(dst);
    SER_WRITE(cur, self->paramv, SZ_PARAMV(self));
    SER_WRITE(cur, dself->signalers, SZ_SIGNALERS(dself));
    SER_WRITE(cur, dself->hint.hintVal, SZ_HINTS(dself));
    ASSERT(dself->unkDbs == NULL); // No use for it currently but code is here
    SER_WRITE(cur, dself->unkDbs, SZ_UNKDBS(dself));
    SER_WRITE(cur, dself->resolvedDeps, SZ_RESOLVEDDEPS(dself));
    return 0;
}

u8 deserializeTaskFactoryHc(ocrObjectFactory_t * pfactory, ocrGuid_t edtGuid, ocrObject_t ** dest, u64 mode, void * srcBuffer, u64 srcSize) {
    //TODO-MD-EDT: shouldn't we have a M_ mode here ?
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ASSERT(srcBuffer != NULL);
    ASSERT(dest != NULL);
    //TODO-MD-EDT: Make a copy but we could actually just piggy back on the srcBuffer ?
    ocrTaskHc_t * src = (ocrTaskHc_t *) srcBuffer;
    // Create a new instance whose size can fit all the embedded data-structures others will be heap allocated
    u32 dstSz = sizeof(ocrTaskHc_t) + SZ_PARAMV(src) + SZ_SIGNALERS(src) + SZ_HINTS(src);
    ocrFatGuid_t resultGuid = {.guid = edtGuid, .metaDataPtr = NULL};
    // Here the targetLoc doesn't matter since we already have a guid, just use current loc
    allocateNewTaskHc(pd, &resultGuid, NULL, dstSz, /*targetLoc*/ pd->myLocation, GUID_PROP_ISVALID | GUID_PROP_TORECORD);
    ocrTaskHc_t * dst = resultGuid.metaDataPtr;
    // Do this copy before so that all the fields are initialized
    *dst = *src;
    // Copy the serialized pointers to destination
    hal_memCopy(OFF_PARAMV(dst), OFF_PARAMV(src), SZ_PARAMV(src), false);
    ((ocrTask_t*) dst)->paramv = (u64 *) OFF_PARAMV(dst);
    hal_memCopy(OFF_SIGNALERS(dst), OFF_SIGNALERS(src), SZ_SIGNALERS(src), false);
    dst->signalers = (regNode_t *) OFF_SIGNALERS(dst);
    hal_memCopy(OFF_HINTS(dst), OFF_HINTS(src), SZ_HINTS(src), false);
    dst->hint.hintVal = (u64 *) OFF_HINTS(dst);
    // Do the heap allocated ones
    ASSERT(((SZ_UNKDBS(src) == 0) && (dst->unkDbs == NULL)) || 1);
    if (SZ_UNKDBS(src)) {
        dst->unkDbs = pd->fcts.pdMalloc(pd, SZ_UNKDBS(src));
        hal_memCopy(dst->unkDbs, OFF_UNKDBS(src), SZ_UNKDBS(src), false);
    }
    ASSERT(((SZ_RESOLVEDDEPS(src) == 0) && (dst->resolvedDeps == NULL)) || 1);
    dst->resolvedDeps = pd->fcts.pdMalloc(pd, SZ_RESOLVEDDEPS(src));
    hal_memCopy(dst->resolvedDeps, OFF_RESOLVEDDEPS(src), SZ_RESOLVEDDEPS(src), false);
    *dest = (ocrObject_t *) dst;
    return 0;
}

//TODO-MD-EDT: This is just to see if it impl works but it my make sense
//to add a move operation since it's contorted now
u8 cloneTaskFactoryHc(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t ** mdPtr) {
    ocrLocation_t destLocation = *((ocrLocation_t *) mdPtr);
    ocrTask_t * self = (ocrTask_t *) factory;
    // Create a policy-domain message
    ocrPolicyMsg_t * msg;
    PD_MSG_STACK(msgStack);
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ASSERT(destLocation != pd->myLocation);

    // Query the serialized size
    u64 mdMode = 0; //TODO-MD-EDT: Should this be MD_MOVE ? What are the modes ?
    u64 mdSize = 0;
    mdSizeTaskFactoryHc((ocrObject_t *) self, 0, &mdSize);

    u64 msgSize = MSG_MDCOMM_SZ + mdSize;
    u32 msgProp = 0;
    if (msgSize > sizeof(ocrPolicyMsg_t)) {
        //TODO-MD-SLAB
        msg = (ocrPolicyMsg_t *) pd->fcts.pdMalloc(pd, msgSize);
        initializePolicyMessage(msg, msgSize);
        getCurrentEnv(NULL, NULL, NULL, msg);
        msgProp = PERSIST_MSG_PROP;
    } else {
        msg = &msgStack;
        getCurrentEnv(NULL, NULL, NULL, &msgStack);
    }

    // Fill in this call specific arguments
    msg->destLocation = destLocation;
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid) = self->guid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/ //TODO-MD-OP not clearly defined yet
    PD_MSG_FIELD_I(mode) = mdMode;
    PD_MSG_FIELD_I(factoryId) = self->fctId;
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    DPRINTF(DEBUG_LVL_VVERB, "edt-md: push "GUIDF" in mode=%"PRIu64"\n", GUIDA(guid), mdMode);
    PD_MSG_FIELD_I(sizePayload) = mdSize;
    char * ptr = &(PD_MSG_FIELD_I(payload));
    ocrObjectFactory_t * ffactory = pd->factories[self->fctId];
    serializeTaskFactoryHc(ffactory, guid, (ocrObject_t *) self, &mdMode, destLocation, (void **) &ptr, &mdSize);
#undef PD_MSG
#undef PD_TYPE
    ((ocrTaskHc_t*)self)->mdState = MD_STATE_EDT_GHOST; // Update current MD to ghost state
    pd->fcts.sendMessage(pd, destLocation, msg, NULL, msgProp);
    // Destroy the EDT metadata
    destructTaskHc(self);
    return 0;
}

void destructTaskFactoryHc(ocrTaskFactory_t* factory) {
    runtimeChunkFree((u64)((ocrTaskFactory_t*)factory)->hintPropMap, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrTaskFactory_t * newTaskFactoryHc(ocrParamList_t* perInstance, u32 factoryId) {
    ocrObjectFactory_t * bbase = (ocrObjectFactory_t *)
                                  runtimeChunkAlloc(sizeof(ocrTaskFactoryHc_t), PERSISTENT_CHUNK);
    bbase->clone = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**), cloneTaskFactoryHc);
    bbase->mdSize = FUNC_ADDR(u8 (*)(ocrObject_t * dest, u64, u64*), mdSizeTaskFactoryHc);
    bbase->serialize = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t*, u64*, ocrLocation_t, void**, u64*), serializeTaskFactoryHc);
    bbase->deserialize = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**, u64, void*, u64), deserializeTaskFactoryHc);

    ocrTaskFactory_t* base = (ocrTaskFactory_t*) bbase;

    // Initialize the base's base
    base->base.fcts.processEvent = FUNC_ADDR(u8 (*) (ocrObject_t*, pdEvent_t**, u32), processEventTaskHc);

    base->instantiate = FUNC_ADDR(u8 (*) (ocrTaskFactory_t*, ocrFatGuid_t*, ocrFatGuid_t, u32, u64*, u32, u32, ocrHint_t*,
        ocrFatGuid_t*, ocrTask_t *, ocrFatGuid_t, ocrParamList_t*), newTaskHc);
    base->base.destruct =  FUNC_ADDR(void (*) (ocrObjectFactory_t*), destructTaskFactoryHc);
    base->factoryId = factoryId;

    base->fcts.destruct = FUNC_ADDR(u8 (*)(ocrTask_t*), destructTaskHc);
#ifdef REG_ASYNC_SGL
    base->fcts.satisfyWithMode = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32, ocrDbAccessMode_t), satisfyTaskHcWithMode);
#else
    base->fcts.satisfy = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32), satisfyTaskHc);
#endif
    base->fcts.registerSignaler = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32, ocrDbAccessMode_t, bool), registerSignalerTaskHc);
    base->fcts.unregisterSignaler = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32, bool), unregisterSignalerTaskHc);
    base->fcts.notifyDbAcquire = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t), notifyDbAcquireTaskHc);
    base->fcts.notifyDbRelease = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t), notifyDbReleaseTaskHc);
    base->fcts.execute = FUNC_ADDR(u8 (*)(ocrTask_t*), taskExecute);
    base->fcts.dependenceResolved = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrGuid_t, void*, u32), dependenceResolvedTaskHc);
    base->fcts.setHint = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrHint_t*), setHintTaskHc);
    base->fcts.getHint = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrHint_t*), getHintTaskHc);
    base->fcts.getRuntimeHint = FUNC_ADDR(ocrRuntimeHint_t* (*)(ocrTask_t*), getRuntimeHintTaskHc);
    //Setup hint framework
    base->hintPropMap = (u64*)runtimeChunkAlloc(sizeof(u64)*(OCR_HINT_EDT_PROP_END - OCR_HINT_EDT_PROP_START - 1), PERSISTENT_CHUNK);
    OCR_HINT_SETUP(base->hintPropMap, ocrHintPropTaskHc, OCR_HINT_COUNT_EDT_HC, OCR_HINT_EDT_PROP_START, OCR_HINT_EDT_PROP_END);
    return base;
}
#endif /* ENABLE_TASK_HC */

#endif /* ENABLE_TASK_HC || ENABLE_TASKTEMPLATE_HC */
