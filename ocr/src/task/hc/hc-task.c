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
    OCR_HINT_EDT_TIME
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
        derived->hint.hintVal = (ocrHintVal_t*)((u64)base + sizeof(ocrTaskTemplateHc_t));
    }

#ifdef OCR_ENABLE_STATISTICS
    {
        // Bug #225
        ocrGuid_t edtGuid = getCurrentEDT();
        statsTEMP_CREATE(pd, edtGuid, NULL, base->guid, base);
    }
#endif /* OCR_ENABLE_STATISTICS */

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
/* OCR HC latch utilities                             */
/******************************************************/

// satisfies the incr slot of a finish latch event
static u8 finishLatchCheckin(ocrPolicyDomain_t *pd, ocrPolicyMsg_t *msg,
                             ocrFatGuid_t edtCheckin, ocrFatGuid_t sourceEvent, ocrFatGuid_t latchEvent) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    //BUG #207 This is a long shot but if the latchEvent is not local (which it shouldn't)
    //then this could be in-flight and child EDT incr/decr is seen before completion,
    //leading to the finish scope being considered completed.
    //TODO => Really need to make this blocking I think

    // Account for this EDT as being part of its parent finish scope
    msg->type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(satisfierGuid) = edtCheckin;
    PD_MSG_FIELD_I(guid) = latchEvent;
    PD_MSG_FIELD_I(payload.guid) = NULL_GUID;
    PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(slot) = OCR_EVENT_LATCH_INCR_SLOT;
#ifdef REG_ASYNC_SGL
    PD_MSG_FIELD_I(mode) = -1; //Doesn't matter for latch
#endif
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, false));
#undef PD_TYPE
    // Tie the local latch event for this EDT's finish scope to its parent finish scope.
    // All of the current EDT children will report to the local finish scope and when the
    // local finish scope completes, it will notify the parent finish scope.
#define PD_TYPE PD_MSG_DEP_ADD
    msg->type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(properties) = DB_MODE_CONST; // not called from add-dependence
    PD_MSG_FIELD_I(source) = sourceEvent;
    PD_MSG_FIELD_I(dest) = latchEvent;
    PD_MSG_FIELD_I(slot) = OCR_EVENT_LATCH_DECR_SLOT;
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
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

static u8 initTaskHcInternal(ocrTaskHc_t *task, ocrPolicyDomain_t * pd,
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
    // If we are creating a finish-edt
    if (hasProperty(properties, EDT_PROP_FINISH)) {
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
        ocrFatGuid_t edtCheckin;
        edtCheckin.guid = task->base.guid;
        edtCheckin.metaDataPtr = task;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = task->base.finishLatch;
        PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(currentEdt.guid) = curTask!=NULL?curTask->guid:NULL_GUID;
        PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(type) = OCR_EVENT_LATCH_T;
#ifdef ENABLE_EXTENSION_PARAMS_EVT
        PD_MSG_FIELD_I(params) = NULL;
#endif
        //For deferred we may feed a valid GUID for the finish latch
        PD_MSG_FIELD_I(properties) = (ocrGuidIsNull(task->base.finishLatch) ? 0 : GUID_PROP_ISVALID);
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
        ocrFatGuid_t latchFGuid = PD_MSG_FIELD_IO(guid);
#undef PD_MSG
#undef PD_TYPE

        ASSERT(!(ocrGuidIsNull(latchFGuid.guid)) && latchFGuid.metaDataPtr != NULL);

        if (!(ocrGuidIsNull(parentLatch.guid))) {
            DPRINTF(DEBUG_LVL_INFO, "Checkin "GUIDF" on parent flatch "GUIDF"\n", GUIDA(task->base.guid), GUIDA(parentLatch.guid));
            // Check in this EDT finish latch with the parent's one and setup the dependence between current latch and parent's one
            getCurrentEnv(NULL, NULL, NULL, &msg);
            RESULT_PROPAGATE(finishLatchCheckin(pd, &msg, edtCheckin, latchFGuid, parentLatch));
        }

        // Check in the current EDT into the new finish scope and link its outputEvent to the current finish latch
        DPRINTF(DEBUG_LVL_INFO, "Checkin "GUIDF" on self flatch "GUIDF"\n", GUIDA(task->base.guid), GUIDA(latchFGuid.guid));
        getCurrentEnv(NULL, NULL, NULL, &msg);
        RESULT_PROPAGATE(finishLatchCheckin(pd, &msg, edtCheckin, outputEvent, latchFGuid));
        // Set edt's ELS to the new latch
        task->base.finishLatch = latchFGuid.guid;
    } else {
        // If the currently executing edt is in a finish scope,
        // but is not a finish-edt itself, just register to the scope
        if(!(ocrGuidIsNull(parentLatch.guid))) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
            DPRINTF(DEBUG_LVL_INFO, "Checkin "GUIDF" on current flatch "GUIDF"\n", GUIDA(task->base.guid), GUIDA(parentLatch.guid));
            // Check in current finish latch
            ocrFatGuid_t edtCheckin;
            edtCheckin.guid = task->base.guid;
            edtCheckin.metaDataPtr = task;
            RESULT_PROPAGATE(finishLatchCheckin(pd, &msg, edtCheckin, outputEvent, parentLatch));
        }
    }
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
                PD_MSG_FIELD_IO(edt.guid) = self->guid; // EDT guid
                PD_MSG_FIELD_IO(edt.metaDataPtr) = self;
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
#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
#else
        ocrTaskHc_t* dself = (ocrTaskHc_t*)base;
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
        ocrTask_t * curEdt = NULL;
        getCurrentEnv(&pd, NULL, &curEdt, NULL);
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

        // If this is a finish EDT and it hasn't ran yet just destroy
        if (!(ocrGuidIsNull(base->finishLatch))) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_DESTROY
            msg.type = PD_MSG_EVT_DESTROY | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(guid.guid) = base->finishLatch;
            PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(currentEdt.guid) = curEdt ? curEdt->guid : NULL_GUID;
            PD_MSG_FIELD_I(currentEdt.metaDataPtr) = curEdt;
            PD_MSG_FIELD_I(properties) = 0;
            u8 returnCode __attribute__((unused)) = pd->fcts.processMessage(pd, &msg, false);
            ASSERT(returnCode == 0);
#undef PD_MSG
#undef PD_TYPE
        }

        // Need to decrement the parent latch since the EDT didn't run
        if (!(ocrGuidIsNull(base->parentLatch))) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
            //TODO ABA issue here if not REQ_RESP ?
            msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
            // PD_MSG_FIELD_I(satisfierGuid) = {(curEdt ? curEdt->guid : NULL_GUID), curEdt};
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

u8 newTaskHc(ocrTaskFactory_t* factory, ocrFatGuid_t * edtGuid, ocrFatGuid_t edtTemplate,
                      u32 paramc, u64* paramv, u32 depc, u32 properties,
                      ocrHint_t *hint, ocrFatGuid_t * outputEventPtr,
                      ocrTask_t *curEdt, ocrFatGuid_t parentLatch,
                      ocrParamList_t *perInstance) {

    // Get the current environment
    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    u32 i;
    getCurrentEnv(&pd, NULL, &curTask, NULL);
    ASSERT(outputEventPtr != NULL);
    ocrFatGuid_t outputEvent = {.guid = NULL_GUID, .metaDataPtr = NULL};
#ifdef ENABLE_OCR_API_DEFERRABLE
    ocrFatGuid_t defFinish;
    // In deferred, when we have a finish the output event is the latch event
    defFinish.guid = NULL_GUID;
    defFinish.metaDataPtr = NULL;
    if (hasProperty(properties, EDT_PROP_FINISH)) {
        // Creating a finish EDT and requested an output-event
        // With PARAM_DEF the call are synchronous even in deferred. In that case
        // the outputEventPtr doesn't hold a valid GUID. Hence only read if non-uninitialized
        if (!ocrGuidIsUninitialized(outputEventPtr->guid)) {
            //TODO-DEFERRED: Seems ill defined, now I can't know if the user did
            //request an outputEventPtr but it doesn't really matter since we generate one
            //anyway when we have a finish scope. In deferred the user/rt interface will have
            //read the guid from outputEventPtr anyway
            defFinish = *outputEventPtr;
        } else {
            outputEventPtr->guid = UNINITIALIZED_GUID;
        }
    }
#endif
    // We need an output event for the EDT if either:
    //  - the user requested one (outputEventPtr is non NULL)
    //  - the EDT is a finish EDT (and therefore we need to link
    //    the output event to the latch event)
    //  - the EDT is within a finish scope (and we need to link to
    //    that latch event)
    if (!ocrGuidIsNull(outputEventPtr->guid) || hasProperty(properties, EDT_PROP_FINISH) ||
            !(ocrGuidIsNull(parentLatch.guid))) {
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = outputEventPtr->guid; // May or may not be read depending on props
        PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(currentEdt.guid) = curTask!=NULL?curTask->guid:NULL_GUID;
        PD_MSG_FIELD_I(currentEdt.metaDataPtr) = curTask;
#ifdef ENABLE_EXTENSION_PARAMS_EVT
        PD_MSG_FIELD_I(params) = NULL;
#endif
        //In deferred properties carry the valid and torecord flags
#ifdef ENABLE_OCR_API_DEFERRABLE
        PD_MSG_FIELD_I(properties) = (properties & GUID_RT_PROP_ALL) | GUID_PROP_TORECORD;
        // Since we used the output event to store the finish latch event, we must remove
        // the isvalid flag so as to generate a proper output event.
        // Also, the valid flag could be set for the EDT guid while the output event hadn't been requested
        if (ocrGuidIsNull(outputEventPtr->guid) || ocrGuidIsUninitialized(outputEventPtr->guid)) {
            PD_MSG_FIELD_I(properties) &= ~GUID_PROP_ISVALID;
        }
#else
        PD_MSG_FIELD_I(properties) = (properties & GUID_RT_PROP_ALL) | GUID_PROP_TORECORD;
#endif
        PD_MSG_FIELD_I(type) = OCR_EVENT_ONCE_T; // Output events of EDTs are non sticky

        RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), 1);
        outputEvent = PD_MSG_FIELD_IO(guid);
        ASSERT(!ocrGuidIsNull(outputEvent.guid));
        ASSERT(!ocrGuidIsUninitialized(outputEvent.guid));
#undef PD_MSG
#undef PD_TYPE
    }

    u32 hintc = hasProperty(properties, EDT_PROP_NO_HINT) ? 0 : OCR_HINT_COUNT_EDT_HC;
    u32 schedc = factory->usesSchedulerObject;

    PD_MSG_STACK(msg);
    // Create the task itself by getting a GUID
    getCurrentEnv(NULL, NULL, NULL, &msg);

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
            targetLoc = affinityToLocation(affGuid);
        }
    }

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = edtGuid->guid;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD_I(size) = sizeof(ocrTaskHc_t) + paramc*sizeof(u64) + depc*sizeof(regNode_t) + hintc*sizeof(u64) + schedc*sizeof(u64);
    PD_MSG_FIELD_I(kind) = OCR_GUID_EDT;
    PD_MSG_FIELD_I(targetLoc) = targetLoc;
    //In deferred properties carry the valid and torecord flags
    PD_MSG_FIELD_I(properties) = (properties & GUID_RT_PROP_ALL) | GUID_PROP_TORECORD;
    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), 1);
    ocrTaskHc_t *edt = (ocrTaskHc_t*)PD_MSG_FIELD_IO(guid.metaDataPtr);
    ocrTask_t *base = (ocrTask_t*)edt;
    ASSERT(edt);

    // Set up the base's base
    base->base.fctId = factory->factoryId;
    // Set-up base structures
    base->guid = PD_MSG_FIELD_IO(guid.guid);
    base->templateGuid = edtTemplate.guid;
    ASSERT(edtTemplate.metaDataPtr); // For now we just assume it is passed whole
    base->funcPtr = ((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->executePtr;
    base->paramv = (paramc > 0) ? ((u64*)((u64)base + sizeof(ocrTaskHc_t))) : NULL;
#ifdef OCR_ENABLE_EDT_NAMING
    hal_memCopy(&(base->name[0]), &(((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->name[0]),
                ocrStrlen(&(((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->name[0])) + 1, false);
#endif
    base->outputEvent = outputEvent.guid;
#ifdef ENABLE_OCR_API_DEFERRABLE
    base->finishLatch = defFinish.guid;
#else
    base->finishLatch = NULL_GUID;
#endif
    base->parentLatch = parentLatch.guid;
    for(i = 0; i < ELS_SIZE; ++i) {
        base->els[i] = NULL_GUID;
    }
    base->state = CREATED_EDTSTATE;
    base->paramc = paramc;
    base->depc = depc;
    base->flags = 0;
    base->fctId = factory->factoryId;
    for(i = 0; i < paramc; ++i) {
        base->paramv[i] = paramv[i];
    }
    edt->signalers = (regNode_t*)((u64)edt + sizeof(ocrTaskHc_t) + paramc*sizeof(u64));
    // Initialize the signalers properly
    for(i = 0; i < depc; ++i) {
        edt->signalers[i].guid = UNINITIALIZED_GUID;
        edt->signalers[i].slot = i;
        edt->signalers[i].mode = -1; //Systematically set when adding dependence
    }

    if (hintc == 0) {
        edt->hint.hintMask = 0;
        edt->hint.hintVal = NULL;
    } else {
        base->flags |= OCR_TASK_FLAG_USES_HINTS;
        ocrTaskTemplateHc_t *derived = (ocrTaskTemplateHc_t*)(edtTemplate.metaDataPtr);
        edt->hint.hintMask = derived->hint.hintMask;
        edt->hint.hintVal = (ocrHintVal_t*)((u64)base + sizeof(ocrTaskHc_t) + paramc*sizeof(u64) + depc*sizeof(regNode_t));
        u64 hintSize = OCR_RUNTIME_HINT_GET_SIZE(derived->hint.hintMask);
        for (i = 0; i < hintc; i++) edt->hint.hintVal[i] = (hintSize == 0) ? (ocrHintVal_t){} : derived->hint.hintVal[i]; //copy the hints from the template
        if (hint != NULL_HINT) factory->fcts.setHint(base, hint);
    }

    if (schedc != 0) {
        base->flags |= OCR_TASK_FLAG_USES_SCHEDULER_OBJECT;
        u64* schedObjPtr = (u64*)HC_TASK_SCHED_OBJ_PTR(edt);
        *schedObjPtr = 0;
    }
    if (perInstance != NULL) {
        paramListTask_t *taskparams = (paramListTask_t*)perInstance;
        if (taskparams->workType == EDT_RT_WORKTYPE) {
            base->flags |= OCR_TASK_FLAG_RUNTIME_EDT;
        }
    }

    ocrHintVal_t val = {};
    if (hint != NULL_HINT && (ocrHintGetValue(hint, OCR_HINT_EDT_AFFINITY, &val) == 0)) {
      base->flags |= OCR_TASK_FLAG_USES_AFFINITY;
    }

#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    if (hasProperty(properties, EDT_PROP_LONG)) {
        base->flags |= OCR_TASK_FLAG_LONG;
    }
#endif

    // Set up HC specific stuff
    RESULT_PROPAGATE2(initTaskHcInternal(edt, pd, curEdt, outputEvent, parentLatch, properties), 1);

    // Set up outputEventPtr:
    //   - if a finish EDT, wait on its latch event
    //   - if not a finish EDT, wait on its output event
    if(!ocrGuidIsNull(outputEventPtr->guid)) { // If output event required i.e. not NULL_GUID
        if(!(ocrGuidIsNull(base->finishLatch))) {
            outputEventPtr->guid = base->finishLatch;
#ifdef ENABLE_OCR_API_DEFERRABLE
            ASSERT(ocrGuidIsEq(outputEventPtr->guid, defFinish.guid) || (ocrGuidIsNull(defFinish.guid)));
#endif
        } else {
            ASSERT(!ocrGuidIsUninitialized(base->outputEvent));
            outputEventPtr->guid = base->outputEvent;
        }
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
            statsEDT_CREATE(pd, edtGuid, task, base->guid, base);
        } else {
            statsTEMP_USE(pd, edtGuid, NULL, taskTemplate->guid, taskTemplate);
            statsEDT_CREATE(pd, edtGuid, NULL, base->guid, base);
        }
    }
#endif /* OCR_ENABLE_STATISTICS */
    DPRINTF(DEBUG_LVL_INFO, "Create "GUIDF" depc %"PRId32" outputEvent "GUIDF"\n", GUIDA(base->guid), depc, GUIDA(outputEventPtr?outputEventPtr->guid:NULL_GUID));
    edtGuid->guid = base->guid;
    edtGuid->metaDataPtr = base;
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_EDT, OCR_ACTION_CREATE, traceTaskCreate, edtGuid->guid);
    // Check to see if the EDT can be run
    if(base->depc == edt->slotSatisfiedCount) {
        DPRINTF(DEBUG_LVL_INFO,
                "Scheduling task "GUIDF" due to initial satisfactions\n", GUIDA(base->guid));
        RESULT_PROPAGATE2(taskAllDepvSatisfied(base), 1);
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
    DPRINTF(DEBUG_LVL_WARN, "Satisfied task oldValue is %d and depc is %d \n", (int) oldValue, (int) base->depc);
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
    DPRINTF(DEBUG_LVL_WARN, "Satisfied task oldValue is %d and depc is %d \n", (int) oldValue, (int) base->depc);
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
    DPRINTF(DEBUG_LVL_WARN, "Registered on task oldValue is %d and depc is %d \n", (int) oldValue, (int) base->depc);
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
            DPRINTF(DEBUG_LVL_WARN, "[DFRD] Executing msg type=0x%"PRIx32"\n", evt->msg->type);
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
    PD_MSG_STACK(msg);
    START_PROFILE(ta_hc_executeCleanup);
#ifdef OCR_ENABLE_STATISTICS
    ocrPolicyCtx_t *ctx = getCurrentWorkerContext();
    // We now say that the worker is done executing the EDT
    statsEDT_END(pd, ctx->sourceObj, curWorker, base->guid, base);
#endif /* OCR_ENABLE_STATISTICS */
    DPRINTF(DEBUG_LVL_INFO, "End_Execution "GUIDF"\n", GUIDA(base->guid));
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_EDT, OCR_ACTION_FINISH, traceTaskFinish, base->guid);
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
                PD_MSG_FIELD_I(edt.guid) = base->guid;
                PD_MSG_FIELD_I(edt.metaDataPtr) = base;
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
            PD_MSG_FIELD_I(edt.guid) = base->guid;
            PD_MSG_FIELD_I(edt.metaDataPtr) = base;
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
        // Now deal with the output event
        if(!(ocrGuidIsNull(base->outputEvent))) {
            if(!(ocrGuidIsNull(retGuid))) {
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_ADD
                msg.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(source.guid) = retGuid;
                PD_MSG_FIELD_I(source.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(dest.guid) = base->outputEvent;
                PD_MSG_FIELD_I(dest.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(currentEdt.guid) = base->guid;
                PD_MSG_FIELD_I(currentEdt.metaDataPtr) = base;
                PD_MSG_FIELD_I(slot) = 0; // Always satisfy on slot 0. This will trickle to
                // the finish latch if needed
                PD_MSG_FIELD_IO(properties) = DB_MODE_CONST;
                // Ignore failure for now
                // Bug #615
                pd->fcts.processMessage(pd, &msg, false);
#undef PD_MSG
#undef PD_TYPE
            } else {
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
                msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(satisfierGuid.guid) = base->guid;
                PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = base;
                PD_MSG_FIELD_I(guid.guid) = base->outputEvent;
                PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(payload.guid) = retGuid;
                PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(currentEdt.guid) = base->guid;
                PD_MSG_FIELD_I(currentEdt.metaDataPtr) = base;
                PD_MSG_FIELD_I(slot) = 0; // Always satisfy on slot 0. This will trickle to
                // the finish latch if needed
#ifdef REG_ASYNC_SGL
                PD_MSG_FIELD_I(mode) = -1; //db is a NULL_GUID
#endif
                PD_MSG_FIELD_I(properties) = 0;
                // Ignore failure for now
                // Bug #615
                pd->fcts.processMessage(pd, &msg, false);
#undef PD_MSG
#undef PD_TYPE
            }
            // Because the output event is non-persistent it is deallocated automatically
            base->outputEvent = NULL_GUID;
        }
        base->state = REAPING_EDTSTATE;
    }
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    else { // else EDT must be rescheduled
        ASSERT(base->state == RESCHED_EDTSTATE);
        ASSERT(base->depc == 0); //Limitation
    }
#endif
    EXIT_PROFILE;
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
    ocrEdtDep_t * depv = derived->resolvedDeps;
    {
        START_PROFILE(ta_hc_executeSetup);
        base->state = RUNNING_EDTSTATE;

        //TODO Execute can be considered user on x86, but need to differentiate processRequestEdts in x86-mpi
        DPRINTF(DEBUG_LVL_INFO, "Execute "GUIDF" paramc:%"PRId32" depc:%"PRId32"\n", GUIDA(base->guid), base->paramc, base->depc);
        OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_EDT, OCR_ACTION_EXECUTE, traceTaskExecute, base->guid, base->funcPtr);

        ASSERT(derived->unkDbs == NULL); // Should be no dynamically acquired DBs before running
        getCurrentEnv(&pd, &curWorker, NULL, NULL);

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
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
        if(base->funcPtr == &processRequestEdt) {
            retGuid = base->funcPtr(paramc, paramv, depc, depv);
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

#ifdef OCR_ENABLE_VISUALIZER
        u64 endTime = salGetTime();
        DPRINTF(DEBUG_LVL_INFO, "Execute "GUIDF" FctName: %s Start: %"PRIu64" End: %"PRIu64"\n", GUIDA(base->guid), base->name, startTime, endTime);
#endif
    }

#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    deferredExecute(base);
    //Create a processEvent function callback event
    pdEvent_t * callbackEvent;
    RESULT_ASSERT(pdCreateEvent(pd, &callbackEvent, PDEVT_TYPE_FCT, 0), ==, 0);
    //Record some calling context info
    pdEventFct_t * devt = ((pdEventFct_t *) callbackEvent);
    devt->object = (ocrObject_t *) base;
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
    pdMarkReadyEvent(pd, callbackEvent);
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
    pdAction_t* satisfyAction = pdGetMarkReadyAction(callbackEvent);
    RESULT_ASSERT(pdLockStrand(oldTailStrand, 0), ==, 0);
    RESULT_ASSERT(pdEnqueueActions(pd, oldTailStrand, 1, &satisfyAction, true/*clear hold*/), ==, 0);
    RESULT_ASSERT(pdUnlockStrand(oldTailStrand), ==, 0);
    derived->tailStrand = callbackStrand;
#else
    deferredExecute(base);
    taskEpilogue(base, pd, curWorker, retGuid);
#endif
#else
    taskEpilogue(base, pd, curWorker, retGuid);
#endif
    RETURN_PROFILE(0);
}

u8 processEventTaskHc(ocrPolicyDomain_t *self, pdEvent_t** evt, u32 idx) {
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    ASSERT((evt != NULL) && (*evt != NULL));
    pdEventFct_t * devt = (pdEventFct_t *) *evt;
    ASSERT(devt->id == CONTINUATION_EDT_EPILOGUE);
    ocrGuid_t * retGuid = devt->args;
    ocrWorker_t * curWorker;
    getCurrentEnv(NULL, &curWorker, NULL, NULL);
    curWorker->curTask = devt->ctx;
    taskEpilogue((ocrTask_t *) devt->object, self, curWorker, ((retGuid == NULL) ? NULL_GUID : retGuid[0]));
    curWorker->curTask = NULL;
    if (devt->args) {
        self->fcts.pdFree(self, devt->args);
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

void destructTaskFactoryHc(ocrObjectFactory_t* factory) {
    runtimeChunkFree((u64)((ocrTaskFactory_t*)factory)->hintPropMap, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrTaskFactory_t * newTaskFactoryHc(ocrParamList_t* perInstance, u32 factoryId) {
    ocrTaskFactory_t* base = (ocrTaskFactory_t*)runtimeChunkAlloc(sizeof(ocrTaskFactoryHc_t), PERSISTENT_CHUNK);

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

    paramListTaskFact_t *paramTaskFact = (paramListTaskFact_t*)perInstance;
    base->usesSchedulerObject = paramTaskFact->usesSchedulerObject;
    return base;
}
#endif /* ENABLE_TASK_HC */

#endif /* ENABLE_TASK_HC || ENABLE_TASKTEMPLATE_HC */
