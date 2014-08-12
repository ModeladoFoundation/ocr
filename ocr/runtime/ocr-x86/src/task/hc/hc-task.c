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

#ifdef OCR_ENABLE_EDT_PROFILING
extern struct _profileStruct gProfilingTable[] __attribute__((weak));
extern struct _dbWeightStruct gDbWeights[] __attribute__((weak));
#endif

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#ifdef OCR_ENABLE_PROFILING_STATISTICS
#endif
#endif /* OCR_ENABLE_STATISTICS */

#include "utils/profiler/profiler.h"

#define DEBUG_TYPE TASK

/******************************************************/
/* OCR-HC Task Template Factory                       */
/******************************************************/

#ifdef ENABLE_TASKTEMPLATE_HC

u8 destructTaskTemplateHc(ocrTaskTemplate_t *self) {
#ifdef OCR_ENABLE_STATISTICS
    {
        // TODO: FIXME
        ocrPolicyDomain_t *pd = getCurrentPD();
        ocrGuid_t edtGuid = getCurrentEDT();

        statsTEMP_DESTROY(pd, edtGuid, NULL, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */
    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    PD_MSG_FIELD(guid.guid) = self->guid;
    PD_MSG_FIELD(guid.metaDataPtr) = self;
    PD_MSG_FIELD(properties) = 1;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

ocrTaskTemplate_t * newTaskTemplateHc(ocrTaskTemplateFactory_t* factory, ocrEdt_t executePtr,
                                      u32 paramc, u32 depc, const char* fctName,
                                      ocrParamList_t *perInstance) {

    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;

    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD(guid.guid) = NULL_GUID;
    PD_MSG_FIELD(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD(size) = sizeof(ocrTaskTemplateHc_t);
    PD_MSG_FIELD(kind) = OCR_GUID_EDT_TEMPLATE;
    PD_MSG_FIELD(properties) = 0;

    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);

    ocrTaskTemplate_t *base = (ocrTaskTemplate_t*)PD_MSG_FIELD(guid.metaDataPtr);
    ASSERT(base);
    base->guid = PD_MSG_FIELD(guid.guid);
#undef PD_MSG
#undef PD_TYPE

    base->paramc = paramc;
    base->depc = depc;
    base->executePtr = executePtr;
#ifdef OCR_ENABLE_EDT_NAMING
    base->name = fctName;
#endif
    base->fctId = factory->factoryId;

#ifdef OCR_ENABLE_STATISTICS
    {
        // TODO: FIXME
        ocrGuid_t edtGuid = getCurrentEDT();
        statsTEMP_CREATE(pd, edtGuid, NULL, base->guid, base);
    }
#endif /* OCR_ENABLE_STATISTICS */
#ifdef OCR_ENABLE_EDT_PROFILING
    base->profileData = NULL;
    if(gProfilingTable) {
      int i;
      for(i = 0; ; i++) {
	if(gProfilingTable[i].fname == NULL) break;
	if(!ocrStrcmp((u8*)fctName, gProfilingTable[i].fname)) {
	  base->profileData = &(gProfilingTable[i]);
	  break;
	}
      }
    }

    base->dbWeights = NULL;
    if(gDbWeights) {
      int i;
      for(i = 0; ; i++) {
	if(gDbWeights[i].fname == NULL) break;
	if(!ocrStrcmp((u8*)fctName, gDbWeights[i].fname)) {
	  base->dbWeights = &(gDbWeights[i]);
	  break;
	}
      }
    }
#endif

    return base;
}

void destructTaskTemplateFactoryHc(ocrTaskTemplateFactory_t* base) {
    runtimeChunkFree((u64)base, NULL);
}

ocrTaskTemplateFactory_t * newTaskTemplateFactoryHc(ocrParamList_t* perType, u32 factoryId) {
    ocrTaskTemplateFactory_t* base = (ocrTaskTemplateFactory_t*)runtimeChunkAlloc(sizeof(ocrTaskTemplateFactoryHc_t), NULL);

    base->instantiate = FUNC_ADDR(ocrTaskTemplate_t* (*)(ocrTaskTemplateFactory_t*, ocrEdt_t, u32, u32, const char*, ocrParamList_t*), newTaskTemplateHc);
    base->destruct =  FUNC_ADDR(void (*)(ocrTaskTemplateFactory_t*), destructTaskTemplateFactoryHc);
    base->factoryId = factoryId;
    base->fcts.destruct = FUNC_ADDR(u8 (*)(ocrTaskTemplate_t*), destructTaskTemplateHc);
    return base;
}

#endif /* ENABLE_TASKTEMPLATE_HC */

#ifdef ENABLE_TASK_HC

/******************************************************/
/* OCR HC latch utilities                             */
/******************************************************/

static ocrFatGuid_t getFinishLatch(ocrTask_t * edt) {
    ocrFatGuid_t result = {.guid = NULL_GUID, .metaDataPtr = NULL};
    if (edt != NULL) { //  NULL happens in main when there's no edt yet
        if(edt->finishLatch)
            result.guid = edt->finishLatch;
        else
            result.guid = edt->parentLatch;
    }
    return result;
}

// satisfies the incr slot of a finish latch event
static u8 finishLatchCheckin(ocrPolicyDomain_t *pd, ocrPolicyMsg_t *msg,
                             ocrFatGuid_t sourceEvent, ocrFatGuid_t latchEvent) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    msg->type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD(guid) = latchEvent;
    PD_MSG_FIELD(payload.guid) = NULL_GUID;
    PD_MSG_FIELD(payload.metaDataPtr) = NULL;
    PD_MSG_FIELD(slot) = OCR_EVENT_LATCH_INCR_SLOT;
    PD_MSG_FIELD(properties) = 0;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, false));
#undef PD_TYPE
#define PD_TYPE PD_MSG_DEP_ADD
    msg->type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
    PD_MSG_FIELD(source) = sourceEvent;
    PD_MSG_FIELD(dest) = latchEvent;
    PD_MSG_FIELD(slot) = OCR_EVENT_LATCH_DECR_SLOT;
    PD_MSG_FIELD(properties) = 0; // TODO: do we want a mode for this?
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

static u8 registerOnFrontier(ocrTaskHc_t *self, ocrPolicyDomain_t *pd,
                             ocrPolicyMsg_t *msg, u32 slot) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_REGWAITER
    msg->type = PD_MSG_DEP_REGWAITER | PD_MSG_REQUEST;
    PD_MSG_FIELD(waiter.guid) = self->base.guid;
    PD_MSG_FIELD(waiter.metaDataPtr) = self;
    PD_MSG_FIELD(dest.guid) = self->signalers[slot].guid;
    PD_MSG_FIELD(dest.metaDataPtr) = NULL;
    PD_MSG_FIELD(slot) = self->signalers[slot].slot;
    PD_MSG_FIELD(properties) = false; // not called from add-dependence
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}
/******************************************************/
/* OCR-HC Support functions                           */
/******************************************************/

static u8 initTaskHcInternal(ocrTaskHc_t *task, ocrPolicyDomain_t * pd,
                             ocrTask_t *curTask, ocrFatGuid_t outputEvent,
                             ocrFatGuid_t parentLatch, u32 properties) {

    ocrPolicyMsg_t msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);

    task->frontierSlot = 0;
    task->slotSatisfiedCount = 0;
    task->lock = 0;
    task->unkDbs = NULL;
    task->countUnkDbs = 0;
    task->maxUnkDbs = 0;

    if(task->base.depc == 0) {
        task->signalers = END_OF_LIST;
    }
    // If we are creating a finish-edt
    if (hasProperty(properties, EDT_PROP_FINISH)) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD(guid.guid) = NULL_GUID;
        PD_MSG_FIELD(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD(type) = OCR_EVENT_LATCH_T;
        PD_MSG_FIELD(properties) = 0;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));

        ocrFatGuid_t latchFGuid = PD_MSG_FIELD(guid);
#undef PD_MSG
#undef PD_TYPE
        ASSERT(latchFGuid.guid != NULL_GUID && latchFGuid.metaDataPtr != NULL);

        if (parentLatch.guid != NULL_GUID) {
            DPRINTF(DEBUG_LVL_INFO, "Checkin 0x%lx on parent flatch 0x%lx\n", task->base.guid, parentLatch.guid);
            // Check in current finish latch
            RESULT_PROPAGATE(finishLatchCheckin(pd, &msg, latchFGuid, parentLatch));
        }

        DPRINTF(DEBUG_LVL_INFO, "Checkin 0x%lx on self flatch 0x%lx\n", task->base.guid, latchFGuid.guid);
        // Check in the new finish scope
        // This will also link outputEvent to latchFGuid
        RESULT_PROPAGATE(finishLatchCheckin(pd, &msg, outputEvent, latchFGuid));
        // Set edt's ELS to the new latch
        task->base.finishLatch = latchFGuid.guid;
    } else {
        // If the currently executing edt is in a finish scope,
        // but is not a finish-edt itself, just register to the scope
        if(parentLatch.guid != NULL_GUID) {
            DPRINTF(DEBUG_LVL_INFO, "Checkin 0x%lx on current flatch 0x%lx\n", task->base.guid, parentLatch.guid);
            // Check in current finish latch
            RESULT_PROPAGATE(finishLatchCheckin(pd, &msg, outputEvent, parentLatch));
        }
    }
    return 0;
}

/**
 * @brief Schedules a task.
 * Warning: The caller must ensure all dependencies have been satisfied
 * Note: static function only meant to factorize code.
 */
static u8 taskSchedule(ocrTask_t *self) {
    DPRINTF(DEBUG_LVL_INFO, "Schedule 0x%lx\n", self->guid);

    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);

    ocrFatGuid_t toGive = {.guid = self->guid, .metaDataPtr = self};

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_COMM_GIVE
    msg.type = PD_MSG_COMM_GIVE | PD_MSG_REQUEST;
    PD_MSG_FIELD(guids) = &toGive;
    PD_MSG_FIELD(guidCount) = 1;
    PD_MSG_FIELD(properties) = 0;
    PD_MSG_FIELD(type) = OCR_GUID_EDT;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

/******************************************************/
/* OCR-HC Task Implementation                         */
/******************************************************/

u8 destructTaskHc(ocrTask_t* base) {
    DPRINTF(DEBUG_LVL_INFO, "Destroy 0x%lx\n", base->guid);

    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);
#ifdef OCR_ENABLE_STATISTICS
    {
        // TODO: FIXME
        // An EDT is destroyed just when it finishes running so
        // the source is basically itself
        statsEDT_DESTROY(pd, base->guid, base, base->guid, base);
    }
#endif /* OCR_ENABLE_STATISTICS */
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    // These next two statements may be not required. Just to be safe
    PD_MSG_FIELD(guid.guid) = base->guid;
    PD_MSG_FIELD(guid.metaDataPtr) = base;
    PD_MSG_FIELD(properties) = 1; // Free metadata
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

ocrTask_t * newDataParallelTaskHc(ocrTaskFactory_t* factory, ocrFatGuid_t edtTemplate,
                      u32 paramc, u64* paramv, u32 depc, u32 properties,
                      ocrFatGuid_t affinity, ocrFatGuid_t * outputEventPtr,
                      ocrTask_t *curEdt, ocrParamList_t *perInstance) {

    // Get the current environment
    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    u32 i;

    ASSERT(!hasProperty(properties, EDT_PROP_FINISH));
    getCurrentEnv(&pd, NULL, NULL, &msg);

    //Create a latch event for data parallel EDTs
    //The data parallel Iteration EDTs will signal this latch
    //The data parallel Sink EDT will wait on the satisfaction of this latch
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD(guid.guid) = NULL_GUID;
        PD_MSG_FIELD(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD(type) = OCR_EVENT_LATCH_T;
        PD_MSG_FIELD(properties) = 0;
        RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
        ocrFatGuid_t dpLatchFGuid = PD_MSG_FIELD(guid);
#undef PD_MSG
#undef PD_TYPE

    // Create the data parallel EDT
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD(guid.guid) = NULL_GUID;
    PD_MSG_FIELD(guid.metaDataPtr) = NULL;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD(size) = sizeof(ocrDataParallelTaskHc_t) + paramc*sizeof(u64) + depc*sizeof(regNode_t);
    PD_MSG_FIELD(kind) = OCR_GUID_EDT;
    PD_MSG_FIELD(properties) = 0;
    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
    ocrDataParallelTaskHc_t *dpEdt = (ocrDataParallelTaskHc_t*)PD_MSG_FIELD(guid.metaDataPtr);
    ocrTaskHc_t *dpTaskHc = (ocrTaskHc_t*)dpEdt;
    ocrTask_t *dpTask = (ocrTask_t*)dpEdt;
    ASSERT(dpEdt);
    dpTask->guid = PD_MSG_FIELD(guid.guid);
#undef PD_MSG
#undef PD_TYPE
    dpTask->templateGuid = edtTemplate.guid;
    ASSERT(edtTemplate.metaDataPtr); // For now we just assume it is passed whole
    dpTask->funcPtr = ((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->executePtr;
    dpTask->paramv = (u64*)((u64)dpTask + sizeof(ocrTaskHc_t));
#ifdef OCR_ENABLE_EDT_NAMING
    dpTask->name = ((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->name;
#endif
    dpTask->outputEvent = dpLatchFGuid.guid;
    dpTask->finishLatch = NULL_GUID;
    dpTask->parentLatch = NULL_GUID;
    for(i = 0; i < ELS_SIZE; ++i) {
        dpTask->els[i] = NULL_GUID;
    }
    dpTask->state = DATA_PARALLEL_CREATED_EDTSTATE;
    dpTask->paramc = paramc;
    dpTask->depc = depc;
    paramListTask_t * pListTask = (paramListTask_t*)perInstance;
    ASSERT(pListTask->dpRange > 0);
    dpTask->dataParallelRange = pListTask->dpRange;
    dpTask->fctId = factory->factoryId;
    for(i = 0; i < paramc; ++i) {
        dpTask->paramv[i] = paramv[i];
    }

    dpTaskHc->signalers = (regNode_t*)((u64)dpTaskHc + sizeof(ocrTaskHc_t) + paramc*sizeof(u64));
    // Initialize the signalers properly
    for(i = 0; i < depc; ++i) {
        dpTaskHc->signalers[i].guid = UNINITIALIZED_GUID;
        dpTaskHc->signalers[i].slot = i;
    }
    if (depc == 0) dpTaskHc->signalers = END_OF_LIST;

    dpTaskHc->frontierSlot = 0;
    dpTaskHc->slotSatisfiedCount = 0;
    dpTaskHc->lock = 0;
    dpTaskHc->unkDbs = NULL;
    dpTaskHc->countUnkDbs = 0;
    dpTaskHc->maxUnkDbs = 0;

    dpEdt->depv = NULL;
    dpEdt->maxAcquiredDb = 0;
    dpEdt->doNotReleaseSlots = 0;
    dpEdt->unkLock = 0;
    
    //Add the newly created data parallel EDT to the latch event
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD(guid) = dpLatchFGuid;
    PD_MSG_FIELD(payload.guid) = NULL_GUID;
    PD_MSG_FIELD(payload.metaDataPtr) = NULL;
    PD_MSG_FIELD(slot) = OCR_EVENT_LATCH_INCR_SLOT;
    PD_MSG_FIELD(properties) = 0;
    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, false), NULL);
#undef PD_TYPE
#undef PD_MSG

    ocrFatGuid_t parentLatch = getFinishLatch(curEdt);
    ocrFatGuid_t outputEvent = {.guid = NULL_GUID, .metaDataPtr = NULL};
    // We need an output event for the EDT if either:
    //  - the user requested one (outputEventPtr is non NULL)
    //  - the EDT is within a finish scope (and we need to link to
    //    that latch event)
    if (outputEventPtr != NULL || parentLatch.guid != NULL_GUID) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD(guid.guid) = NULL_GUID;
        PD_MSG_FIELD(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD(properties) = 0;
        PD_MSG_FIELD(type) = OCR_EVENT_ONCE_T; // Output events of EDTs are non sticky

        RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
        outputEvent = PD_MSG_FIELD(guid);
#undef PD_MSG
#undef PD_TYPE
    }

    // Create a *sink* EDT for the data parallel EDT 
    // This sink EDT depends on the satisfaction of 
    // the data parallel latch created above.
    // After the sink EDT runs, it satisfies the 
    // output event of the data parallel computation.
    // The sink EDT's function is to release all DBs 
    // acquired by the data parallel EDT.
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD(guid.guid) = NULL_GUID;
    PD_MSG_FIELD(guid.metaDataPtr) = NULL;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD(size) = sizeof(ocrDataParallelSinkTaskHc_t) + sizeof(regNode_t);
    PD_MSG_FIELD(kind) = OCR_GUID_EDT;
    PD_MSG_FIELD(properties) = 0;
    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
    ocrTaskHc_t *sinkEdt = (ocrTaskHc_t*)PD_MSG_FIELD(guid.metaDataPtr);
    ocrTask_t *sinkTask = (ocrTask_t*)sinkEdt;
    ocrDataParallelSinkTaskHc_t *sinkDpEdt = (ocrDataParallelSinkTaskHc_t*)sinkEdt;
    ASSERT(sinkEdt);
    sinkTask->guid = PD_MSG_FIELD(guid.guid);
#undef PD_MSG
#undef PD_TYPE
    sinkTask->templateGuid = NULL_GUID;
    sinkTask->funcPtr = NULL;
    sinkTask->paramv = NULL;
#ifdef OCR_ENABLE_EDT_NAMING
    sinkTask->name = NULL;
#endif
    sinkTask->outputEvent = outputEvent.guid;
    sinkTask->finishLatch = NULL_GUID;
    sinkTask->parentLatch = parentLatch.guid;
    for(i = 0; i < ELS_SIZE; ++i) {
        sinkTask->els[i] = NULL_GUID;
    }
    sinkTask->state = DATA_PARALLEL_SINK_EDTSTATE;
    sinkTask->paramc = 0;
    sinkTask->depc = 1;
    sinkTask->fctId = factory->factoryId;

    sinkEdt->signalers = (regNode_t*)((u64)sinkEdt + sizeof(ocrDataParallelSinkTaskHc_t));
    // Initialize the signalers properly
    sinkEdt->signalers->guid = UNINITIALIZED_GUID;
    sinkEdt->signalers->slot = 0;
    sinkEdt->frontierSlot = 0;
    sinkEdt->slotSatisfiedCount = 0;
    sinkEdt->lock = 0;
    sinkEdt->unkDbs = NULL;
    sinkEdt->countUnkDbs = 0;
    sinkEdt->maxUnkDbs = 0;

    sinkDpEdt->dpTask = dpTask->guid;
    dpEdt->sinkTask = sinkTask->guid;

    //Setup the dependence between the data parallel finish latch and the sink EDT
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_ADD
        msg.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
        PD_MSG_FIELD(source) = dpLatchFGuid;
        PD_MSG_FIELD(dest.guid) = sinkTask->guid;
        PD_MSG_FIELD(dest.metaDataPtr) = sinkTask;
        PD_MSG_FIELD(slot) = 0;
        PD_MSG_FIELD(properties) = DB_DEFAULT_MODE;
        PD_MSG_FIELD(currentEdt.guid) = curEdt->guid;
        PD_MSG_FIELD(currentEdt.metaDataPtr) = curEdt;
        RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
#undef PD_MSG
#undef PD_TYPE

    // If the data parallel computation was created within a finish scope, 
    // then checkin sink EDT in that scope
    if(parentLatch.guid != NULL_GUID) {
        DPRINTF(DEBUG_LVL_INFO, "Checkin 0x%lx on parent flatch 0x%lx\n", sinkTask->guid, parentLatch.guid);
        RESULT_PROPAGATE2(finishLatchCheckin(pd, &msg, outputEvent, parentLatch), NULL);
    }

    // Set up outputEventPtr:
    // This is the output event of the data parallel computation
    if(outputEventPtr) {
        outputEventPtr->guid = sinkTask->outputEvent;
    }

    // ALL SETUP AND CONNECTIONS COMPLETE
    // Check to see if the data parallel EDT can be run
    if(dpTask->depc == dpTaskHc->slotSatisfiedCount) {
        DPRINTF(DEBUG_LVL_VVERB, "Scheduling task 0x%lx due to initial satisfactions\n",
                dpTask->guid);
        RESULT_PROPAGATE2(taskSchedule(dpTask), NULL);
    }

    return dpTask;
}

ocrTask_t * newTaskHc(ocrTaskFactory_t* factory, ocrFatGuid_t edtTemplate,
                      u32 paramc, u64* paramv, u32 depc, u32 properties,
                      ocrFatGuid_t affinity, ocrFatGuid_t * outputEventPtr,
                      ocrTask_t *curEdt, ocrParamList_t *perInstance) {

    if (hasProperty(properties, EDT_PROP_DATA_PARALLEL)) {
        return newDataParallelTaskHc(factory, edtTemplate, paramc, paramv, depc, properties, affinity, outputEventPtr, curEdt, perInstance);
    }

    // Get the current environment
    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    u32 i;

    getCurrentEnv(&pd, NULL, NULL, &msg);

    ocrFatGuid_t parentLatch = getFinishLatch(curEdt);
    ocrFatGuid_t outputEvent = {.guid = NULL_GUID, .metaDataPtr = NULL};
    // We need an output event for the EDT if either:
    //  - the user requested one (outputEventPtr is non NULL)
    //  - the EDT is a finish EDT (and therefore we need to link
    //    the output event to the latch event)
    //  - the EDT is within a finish scope (and we need to link to
    //    that latch event)
    if (outputEventPtr != NULL || hasProperty(properties, EDT_PROP_FINISH) ||
            parentLatch.guid != NULL_GUID) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        msg.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD(guid.guid) = NULL_GUID;
        PD_MSG_FIELD(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD(properties) = 0;
        PD_MSG_FIELD(type) = OCR_EVENT_ONCE_T; // Output events of EDTs are non sticky

        RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
        outputEvent = PD_MSG_FIELD(guid);

#undef PD_MSG
#undef PD_TYPE
    }

    // Create the task itself by getting a GUID
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD(guid.guid) = NULL_GUID;
    PD_MSG_FIELD(guid.metaDataPtr) = NULL;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD(size) = sizeof(ocrTaskHc_t) + paramc*sizeof(u64) + depc*sizeof(regNode_t);
    PD_MSG_FIELD(kind) = OCR_GUID_EDT;
    PD_MSG_FIELD(properties) = 0;
    RESULT_PROPAGATE2(pd->fcts.processMessage(pd, &msg, true), NULL);
    ocrTaskHc_t *edt = (ocrTaskHc_t*)PD_MSG_FIELD(guid.metaDataPtr);
    ocrTask_t *base = (ocrTask_t*)edt;
    ASSERT(edt);

    // Set-up base structures
    base->guid = PD_MSG_FIELD(guid.guid);
    base->templateGuid = edtTemplate.guid;
    ASSERT(edtTemplate.metaDataPtr); // For now we just assume it is passed whole
    base->funcPtr = ((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->executePtr;
    base->paramv = (u64*)((u64)base + sizeof(ocrTaskHc_t));
#ifdef OCR_ENABLE_EDT_NAMING
    base->name = ((ocrTaskTemplate_t*)(edtTemplate.metaDataPtr))->name;
#endif
    base->outputEvent = outputEvent.guid;
    base->finishLatch = NULL_GUID;
    base->parentLatch = parentLatch.guid;
    for(i = 0; i < ELS_SIZE; ++i) {
        base->els[i] = NULL_GUID;
    }
    base->state = CREATED_EDTSTATE;
    base->paramc = paramc;
    base->depc = depc;
    base->dataParallelRange = 0;
    base->fctId = factory->factoryId;
    for(i = 0; i < paramc; ++i) {
        base->paramv[i] = paramv[i];
    }

    edt->signalers = (regNode_t*)((u64)edt + sizeof(ocrTaskHc_t) + paramc*sizeof(u64));
    // Initialize the signalers properly
    for(i = 0; i < depc; ++i) {
        edt->signalers[i].guid = UNINITIALIZED_GUID;
        edt->signalers[i].slot = i;
    }

    // Set up HC specific stuff
    RESULT_PROPAGATE2(initTaskHcInternal(edt, pd, curEdt, outputEvent, parentLatch, properties), NULL);

    // Set up outputEventPtr:
    //   - if a finish EDT, wait on its latch event
    //   - if not a finish EDT, wait on its output event
    if(outputEventPtr) {
        if(base->finishLatch) {
            outputEventPtr->guid = base->finishLatch;
        } else {
            outputEventPtr->guid = base->outputEvent;
        }
    }
#undef PD_MSG
#undef PD_TYPE

#ifdef OCR_ENABLE_STATISTICS
    // TODO FIXME
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
    DPRINTF(DEBUG_LVL_INFO, "Create 0x%lx depc %d outputEvent 0x%lx\n", base->guid, depc, outputEventPtr?outputEventPtr->guid:NULL_GUID);

    // Check to see if the EDT can be run
    if(base->depc == edt->slotSatisfiedCount) {
        DPRINTF(DEBUG_LVL_VVERB, "Scheduling task 0x%lx due to initial satisfactions\n",
                base->guid);
        RESULT_PROPAGATE2(taskSchedule(base), NULL);
    }
    return base;
}

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
    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);

    // Replace the signaler's guid by the data guid, this is to avoid
    // further references to the event's guid, which is good in general
    // and crucial for once-event since they are being destroyed on satisfy.

    // Could be moved a little later if the ASSERT was not here
    // Should not make a huge difference
    hal_lock32(&(self->lock));
    ASSERT(slot < base->depc); // Check to make sure non crazy value is passed in
    ASSERT(self->signalers[slot].slot != (u32)-1); // Check to see if not already satisfied
    ASSERT((self->signalers[slot].slot == slot && (self->signalers[slot].slot == self->frontierSlot)) ||
           (self->signalers[slot].slot == (u32)-2) || /* Checks if ONCE/LATCH event satisfaction */
           (slot > self->frontierSlot)); /* A DB or NULL_GUID being added as ocrAddDependence */

    self->signalers[slot].guid = data.guid;
    self->signalers[slot].slot = (u32)-1; // Say that it is satisfied
    if(++self->slotSatisfiedCount == base->depc) {
        ++(self->slotSatisfiedCount); // So others don't catch the satisfaction
        hal_unlock32(&(self->lock));
        // All dependences have been satisfied, schedule the edt
        DPRINTF(DEBUG_LVL_VERB, "Scheduling task 0x%lx due to last satisfied dependence\n",
                self->base.guid);
        RESULT_PROPAGATE(taskSchedule(base));
    } else if(self->frontierSlot == slot) {
        // We need to go register to the next non-once dependence
        while(++self->frontierSlot < base->depc &&
                self->signalers[self->frontierSlot].slot != ++slot) ;
        // We found a slot that is == to slot (so unsatisfied and not once)
        if(self->frontierSlot < base->depc &&
                self->signalers[self->frontierSlot].guid != UNINITIALIZED_GUID) {
            u32 tslot = self->frontierSlot;
            hal_unlock32(&(self->lock));
            //TODO There seems to be a race here between registerSignalerTaskHc that
            //gets a db signaler on slot 'n', set the guid to db's buid, then get stalled
            //before setting up slot to -1. Then a satisfy on n-1 happens and lead us here
            // because slot[n] is still n. registerOnFrontier would then crash because we
            // cannot register on a db.
            RESULT_PROPAGATE(registerOnFrontier(self, pd, &msg, tslot));
        } else {
            // If the slot's guid is UNITIALIZED_GUID it means add-dependence
            // hasn't happened yet for this slot.
            hal_unlock32(&(self->lock));
        }
    } else {
        hal_unlock32(&(self->lock));
    }
    return 0;
}

/**
 * Can be invoked concurrently, however each invocation should be for a different slot
 */
u8 registerSignalerTaskHc(ocrTask_t * base, ocrFatGuid_t signalerGuid, u32 slot, bool isDepAdd) {
    ASSERT(isDepAdd); // This should only be called when adding a dependence

    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    regNode_t * node = &(self->signalers[slot]);

    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);
    ocrGuidKind signalerKind = OCR_GUID_NONE;
    deguidify(pd, &signalerGuid, &signalerKind);
    if(signalerKind == OCR_GUID_EVENT) {
        node->slot = slot;
        ocrEventTypes_t evtKind = eventType(pd, signalerGuid);
        if(evtKind == OCR_EVENT_ONCE_T ||
           evtKind == OCR_EVENT_LATCH_T) {

            node->slot = (u32)-2; // To signal that this is a once event

            // We need to move the frontier slot over
            hal_lock32(&(self->lock));
            node->guid = signalerGuid.guid;
            while(self->frontierSlot < base->depc &&
                  self->signalers[self->frontierSlot].slot != self->frontierSlot)
                  self->frontierSlot++;

            // We found a slot that is == to slot (so unsatisfied and not once)
            if(self->frontierSlot < base->depc &&
                    self->signalers[self->frontierSlot].guid != UNINITIALIZED_GUID) {
                u32 tslot = self->frontierSlot;
                hal_unlock32(&(self->lock));
                RESULT_PROPAGATE(registerOnFrontier(self, pd, &msg, tslot));
                // If we are UNITIALIZED_GUID, we will do the REGWAITER
                // when we add the dependence (just below)
            } else {
                hal_unlock32(&(self->lock));
            }
        } else {
            // By setting node->guid inside the lock, we order registerSignaler
            // and satisfy concurrent execution. The later updates the frontierSlot
            // but cannot proceed with registerOnFrontier since the guid is still
            // UNINITIALIZED
            hal_lock32(&(self->lock));
            node->guid = signalerGuid.guid;
            if(slot == self->frontierSlot) {
                hal_unlock32(&(self->lock));
                // We actually need to register ourself as a waiter here
                ocrPolicyDomain_t *pd = NULL;
                ocrPolicyMsg_t msg;
                getCurrentEnv(&pd, NULL, NULL, &msg);
                RESULT_PROPAGATE(registerOnFrontier(self, pd, &msg, slot));
            } else {
                hal_unlock32(&(self->lock));
            }
            // else The edt will lazily register on the signalerGuid when
            // the frontier reaches the signaler's slot.
        }
    } else {
        if(signalerKind == OCR_GUID_DB) {
            ASSERT(false);
            //TODO Seems this is always handled beforehand by converting add-dep to satisfy
            //TODO additionally, think there'a bug here because we need to try and iterate
            //the frontier here because the first slot could be db, followed by other dependences.
            node->slot = (u32)-1; // Already satisfied
            hal_lock32(&(self->lock));
            ++(self->slotSatisfiedCount);
            if(base->depc == self->slotSatisfiedCount) {
                ++(self->slotSatisfiedCount); // We make it go one over to not schedule twice
                hal_unlock32(&(self->lock));
                DPRINTF(DEBUG_LVL_VERB, "Scheduling task 0x%lx due to an add dependence\n",
                        self->base.guid);
                RESULT_PROPAGATE(taskSchedule(base));
            } else {
                hal_unlock32(&(self->lock));
            }
        } else {
            ASSERT(0);
        }
    }

    DPRINTF(DEBUG_LVL_INFO, "AddDependence from 0x%lx to 0x%lx slot %d\n", signalerGuid.guid, base->guid, slot);
    return 0;
}

u8 unregisterSignalerTaskHc(ocrTask_t * base, ocrFatGuid_t signalerGuid, u32 slot, bool isDepRem) {
    ASSERT(0); // We don't support this at this time...
    return 0;
}

u8 notifyDbAcquireTaskHc(ocrTask_t *base, ocrFatGuid_t db) {
    if (base->state == DATA_PARALLEL_ACTIVE_EDTSTATE) {
        ocrDataParallelTaskHc_t *dpEdt = (ocrDataParallelTaskHc_t*)base;
        hal_lock32(&(dpEdt->unkLock));
    }

    // This implementation does NOT support EDTs moving while they are executing
    ocrTaskHc_t *derived = (ocrTaskHc_t*)base;
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if(derived->maxUnkDbs == 0) {
        derived->unkDbs = (ocrGuid_t*)pd->fcts.pdMalloc(pd, sizeof(ocrGuid_t)*8);
        ASSERT(derived->unkDbs);
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
    DPRINTF(DEBUG_LVL_VERB, "EDT (GUID: 0x%lx) added DB (GUID: 0x%lx) to its list of dyn. acquired DBs (have %d)\n",
            base->guid, db.guid, derived->countUnkDbs);

    if (base->state == DATA_PARALLEL_ACTIVE_EDTSTATE) {
        ocrDataParallelTaskHc_t *dpEdt = (ocrDataParallelTaskHc_t*)base;
        hal_unlock32(&(dpEdt->unkLock));
    }
    return 0;
}

u8 notifyDbReleaseTaskHc(ocrTask_t *base, ocrFatGuid_t db) {
    if (base->state == DATA_PARALLEL_ACTIVE_EDTSTATE) {
        ocrDataParallelTaskHc_t *dpEdt = (ocrDataParallelTaskHc_t*)base;
        hal_lock32(&(dpEdt->unkLock));
    }

    ocrTaskHc_t *derived = (ocrTaskHc_t*)base;
    if(derived->unkDbs == NULL)
        return 0; // May be a release we don't care about
    u64 maxCount = derived->countUnkDbs;
    u64 count = 0;
    DPRINTF(DEBUG_LVL_VERB, "Notifying EDT (GUID: 0x%lx) that it acquired db (GUID: 0x%lx)\n",
            base->guid, db.guid);
    while(count < maxCount) {
        // We bound our search (in case there is an error)
        if(db.guid == derived->unkDbs[count]) {
            DPRINTF(DEBUG_LVL_VVERB, "Found a match for count %lu\n", count);
            derived->unkDbs[count] = derived->unkDbs[maxCount - 1];
            --(derived->countUnkDbs);
            return 0;
        }
        ++count;
    }
    // We did not find it but it may be that we never acquired it
    // Should not be an error code

    if (base->state == DATA_PARALLEL_ACTIVE_EDTSTATE) {
        ocrDataParallelTaskHc_t *dpEdt = (ocrDataParallelTaskHc_t*)base;
        hal_unlock32(&(dpEdt->unkLock));
    }

    return 0;
}

u8 regularTaskExecute(ocrTask_t* base) {
    DPRINTF(DEBUG_LVL_INFO, "Execute 0x%lx\n", base->guid);
    ocrTaskHc_t* derived = (ocrTaskHc_t*)base;
    // In this implementation each time a signaler has been satisfied, its guid
    // has been replaced by the db guid it has been satisfied with.
    u32 paramc = base->paramc;
    u64 * paramv = base->paramv;
    u32 depc = base->depc;

    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);

    ocrEdtDep_t * depv = NULL;
    u64 doNotReleaseSlots = 0; // Used to support an EDT acquiring the same DB
                               // multiple times. For now limit to 64 DBs
                               // (ie: an EDT that does this should not have more
                               // than 64 DBs, others can have as many as they want)
    // If any dependencies, acquire their data-blocks
    u32 maxAcquiredDb = 0;

    ASSERT(derived->unkDbs == NULL); // Should be no dynamically acquired DBs before running

    if (depc != 0) {
        START_PROFILE(ta_hc_dbAcq);
        //TODO would be nice to resolve regNode into guids before
        depv = pd->fcts.pdMalloc(pd, sizeof(ocrEdtDep_t)*depc);
        // Double-check we're not rescheduling an already executed edt
        ASSERT(derived->signalers != END_OF_LIST);
        // Make sure the task was actually fully satisfied
        ASSERT(derived->slotSatisfiedCount == depc+1);
        while( maxAcquiredDb < depc ) {
            //TODO would be nice to standardize that on satisfy
            depv[maxAcquiredDb].guid = derived->signalers[maxAcquiredDb].guid;
            if(depv[maxAcquiredDb].guid != NULL_GUID) {
                // We send a message that we want to acquire the DB
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD(guid.guid) = depv[maxAcquiredDb].guid;
                PD_MSG_FIELD(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD(edt.guid) = base->guid;
                PD_MSG_FIELD(edt.metaDataPtr) = base;
                PD_MSG_FIELD(properties) = 1; // Runtime acquire
                // This call may fail if the policy domain goes down
                // while we are staring to execute

                if(pd->fcts.processMessage(pd, &msg, true)) {
                    // We are not going to launch the EDT
                    break;
                }
                switch(PD_MSG_FIELD(returnDetail)) {
                case 0:
                    ASSERT(PD_MSG_FIELD(ptr));
                    break; // Everything went fine
                case OCR_EACQ:
                    // The EDT was already acquired
                    ASSERT(PD_MSG_FIELD(ptr));
                    ASSERT(maxAcquiredDb < 64);
                    DPRINTF(DEBUG_LVL_VERB, "EDT (GUID: 0x%lx) acquiring DB (GUID: 0x%lx) multiple times. Ignoring acquire on slot %d\n",
                            base->guid, depv[maxAcquiredDb].guid, maxAcquiredDb);
                    doNotReleaseSlots |= (1ULL << maxAcquiredDb);
                    break;
                default:
                    ASSERT(0);
                }
                depv[maxAcquiredDb].ptr = PD_MSG_FIELD(ptr);
#undef PD_MSG
#undef PD_TYPE
            } else {
                depv[maxAcquiredDb].ptr = NULL;
            }
            ++maxAcquiredDb;
        }
        derived->signalers = END_OF_LIST;
        EXIT_PROFILE;
    }

#ifdef OCR_ENABLE_STATISTICS
    // TODO: FIXME
    ocrPolicyCtx_t *ctx = getCurrentWorkerContext();
    ocrWorker_t *curWorker = NULL;

    deguidify(pd, ctx->sourceObj, (u64*)&curWorker, NULL);

    // We first have the message of using the EDT Template
    statsTEMP_USE(pd, base->guid, base, taskTemplate->guid, taskTemplate);

    // We now say that the worker is starting the EDT
    statsEDT_START(pd, ctx->sourceObj, curWorker, base->guid, base, depc != 0);

#endif /* OCR_ENABLE_STATISTICS */

    ocrGuid_t retGuid = NULL_GUID;
    {
        START_PROFILE(userCode);
        if(depc == 0 || (maxAcquiredDb == depc)) {
            retGuid = base->funcPtr(paramc, paramv, depc, depv);
        }
        EXIT_PROFILE;
    }
#ifdef OCR_ENABLE_STATISTICS
    // We now say that the worker is done executing the EDT
    statsEDT_END(pd, ctx->sourceObj, curWorker, base->guid, base);
#endif /* OCR_ENABLE_STATISTICS */

    // edt user code is done, if any deps, release data-blocks
    if(depc != 0) {
        START_PROFILE(ta_hc_dbRel);
        u32 i;
        for(i=0; i < maxAcquiredDb; ++i) { // Only release the ones we managed to grab
            if((depv[i].guid != NULL_GUID) &&
               ((i >= 64) || (doNotReleaseSlots == 0) ||
                ((i < 64) && (((1ULL << i) & doNotReleaseSlots) == 0)))) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
                msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST;
                PD_MSG_FIELD(guid.guid) = depv[i].guid;
                PD_MSG_FIELD(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD(edt.guid) = base->guid;
                PD_MSG_FIELD(edt.metaDataPtr) = base;
                PD_MSG_FIELD(properties) = 1; // Runtime release
                // Ignore failures at this point
                pd->fcts.processMessage(pd, &msg, false);
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
        // We acquire this DB
        ocrGuid_t *extraToFree = derived->unkDbs;
        u64 count = derived->countUnkDbs;
        while(count) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
            msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST;
            PD_MSG_FIELD(guid.guid) = extraToFree[0];
            PD_MSG_FIELD(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD(edt.guid) = base->guid;
            PD_MSG_FIELD(edt.metaDataPtr) = base;
            PD_MSG_FIELD(properties) = 0; // Not a runtime free since it was acquired using DB create
            if(pd->fcts.processMessage(pd, &msg, false)) {
                DPRINTF(DEBUG_LVL_WARN, "EDT (GUID: 0x%lx) could not release dynamically acquired DB (GUID: 0x%lx)\n",
                        base->guid, PD_MSG_FIELD(guid.guid));
                break;
            }
#undef PD_MSG
#undef PD_TYPE
            --count;
            ++extraToFree;
        }
        pd->fcts.pdFree(pd, derived->unkDbs);
    }

    // Now deal with the output event
    if(base->outputEvent != NULL_GUID) {
        if(retGuid != NULL_GUID) {
    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_DEP_ADD
            msg.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
            PD_MSG_FIELD(source.guid) = retGuid;
            PD_MSG_FIELD(dest.guid) = base->outputEvent;
            PD_MSG_FIELD(slot) = 0; // Always satisfy on slot 0. This will trickle to
            // the finish latch if needed
            PD_MSG_FIELD(properties) = 0;
            // Ignore failure for now
            // FIXME: Probably need to be a bit more selective
            pd->fcts.processMessage(pd, &msg, false);
    #undef PD_MSG
    #undef PD_TYPE
        } else {
    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_DEP_SATISFY
            msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
            PD_MSG_FIELD(guid.guid) = base->outputEvent;
            PD_MSG_FIELD(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD(payload.guid) = retGuid;
            PD_MSG_FIELD(payload.metaDataPtr) = NULL;
            PD_MSG_FIELD(slot) = 0; // Always satisfy on slot 0. This will trickle to
            // the finish latch if needed
            PD_MSG_FIELD(properties) = 0;
            // Ignore failure for now
            // FIXME: Probably need to be a bit more selective
            pd->fcts.processMessage(pd, &msg, false);
    #undef PD_MSG
    #undef PD_TYPE
        }
    }
    return 0;
}

u8 dataParallelActiveTaskExecute(ocrTask_t* base) {
    ocrDataParallelTaskHc_t *dpEdt = (ocrDataParallelTaskHc_t *)base;
    ocrGuid_t retGuid = NULL_GUID;
    ocrPolicyDomain_t *pd = NULL;
    ocrWorker_t *worker = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, &worker, NULL, &msg);
    ocrDpCtxt_t *dpCtxt = &(worker->dpCtxt);

    while(dpCtxt->lb < dpCtxt->ub) {
        dpCtxt->curIndex = dpCtxt->lb;
        START_PROFILE(userCode);
        retGuid = base->funcPtr(base->paramc, base->paramv, base->depc, dpEdt->depv);
        EXIT_PROFILE;
        ASSERT(retGuid == NULL_GUID);
        dpCtxt->lb++;
    }

    hal_lock32(&(dpCtxt->lock));
    dpCtxt->active = 0;
    hal_fence();
    while(dpCtxt->lb < dpCtxt->ub) {
        dpCtxt->curIndex = dpCtxt->lb;
        START_PROFILE(userCode);
        retGuid = base->funcPtr(base->paramc, base->paramv, base->depc, dpEdt->depv);
        EXIT_PROFILE;
        ASSERT(retGuid == NULL_GUID);
        dpCtxt->lb++;
    }
    hal_unlock32(&(dpCtxt->lock));

    // Decrement on the data parallel latch
    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_DEP_SATISFY
    msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD(guid.guid) = dpCtxt->latch;
    PD_MSG_FIELD(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD(payload.guid) = NULL_GUID;
    PD_MSG_FIELD(payload.metaDataPtr) = NULL;
    PD_MSG_FIELD(slot) = OCR_EVENT_LATCH_DECR_SLOT; 
    PD_MSG_FIELD(properties) = 0;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
    #undef PD_MSG
    #undef PD_TYPE

    dpCtxt->task = NULL_GUID;
    dpCtxt->latch = NULL_GUID;
    dpCtxt->curIndex = (u64)(-1);
    return 0;
}

u8 dataParallelSinkTaskExecute(ocrTask_t* sinkBase) {
    ocrDataParallelSinkTaskHc_t *sinkTask = (ocrDataParallelSinkTaskHc_t*)sinkBase;
    ocrDataParallelTaskHc_t *dpEdt = NULL;
    ocrPolicyDomain_t *pd = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, NULL, NULL, &msg);
    pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], sinkTask->dpTask, (u64*)(&dpEdt), NULL);
    ASSERT(dpEdt);
    ocrTask_t* base = (ocrTask_t*)dpEdt;
    ocrTaskHc_t* derived = (ocrTaskHc_t*)base;

    ocrEdtDep_t * depv = dpEdt->depv;
    u32 maxAcquiredDb = dpEdt->maxAcquiredDb;
    u64 doNotReleaseSlots = dpEdt->doNotReleaseSlots;
    u32 depc = base->depc;

    // edt user code is done, if any deps, release data-blocks
    if(depc != 0) {
        START_PROFILE(ta_hc_dbRel);
        u32 i;
        for(i=0; i < maxAcquiredDb; ++i) { // Only release the ones we managed to grab
            if((depv[i].guid != NULL_GUID) &&
               ((i >= 64) || (doNotReleaseSlots == 0) ||
                ((i < 64) && (((1ULL << i) & doNotReleaseSlots) == 0)))) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
                msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST;
                PD_MSG_FIELD(guid.guid) = depv[i].guid;
                PD_MSG_FIELD(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD(edt.guid) = base->guid;
                PD_MSG_FIELD(edt.metaDataPtr) = base;
                PD_MSG_FIELD(properties) = 1; // Runtime release
                // Ignore failures at this point
                pd->fcts.processMessage(pd, &msg, false);
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
        // We acquire this DB
        ocrGuid_t *extraToFree = derived->unkDbs;
        u64 count = derived->countUnkDbs;
        while(count) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
            msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST;
            PD_MSG_FIELD(guid.guid) = extraToFree[0];
            PD_MSG_FIELD(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD(edt.guid) = base->guid;
            PD_MSG_FIELD(edt.metaDataPtr) = base;
            PD_MSG_FIELD(properties) = 0; // Not a runtime free since it was acquired using DB create
            if(pd->fcts.processMessage(pd, &msg, false)) {
                DPRINTF(DEBUG_LVL_WARN, "EDT (GUID: 0x%lx) could not release dynamically acquired DB (GUID: 0x%lx)\n",
                        base->guid, PD_MSG_FIELD(guid.guid));
                break;
            }
#undef PD_MSG
#undef PD_TYPE
            --count;
            ++extraToFree;
        }
        pd->fcts.pdFree(pd, derived->unkDbs);
    }

    // Now deal with the output event
    if(sinkBase->outputEvent != NULL_GUID) {
    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_DEP_SATISFY
            msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
            PD_MSG_FIELD(guid.guid) = sinkBase->outputEvent;
            PD_MSG_FIELD(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD(payload.guid) = NULL_GUID;
            PD_MSG_FIELD(payload.metaDataPtr) = NULL;
            PD_MSG_FIELD(slot) = 0; // Always satisfy on slot 0. This will trickle to
            // the finish latch if needed
            PD_MSG_FIELD(properties) = 0;
            // Ignore failure for now
            // FIXME: Probably need to be a bit more selective
            pd->fcts.processMessage(pd, &msg, false);
    #undef PD_MSG
    #undef PD_TYPE
    }
    return 0;
}

u8 dataParallelCreatedTaskExecute(ocrTask_t* base) {
    DPRINTF(DEBUG_LVL_INFO, "Execute 0x%lx\n", base->guid);
    ocrTaskHc_t* derived = (ocrTaskHc_t*)base;
    // In this implementation each time a signaler has been satisfied, its guid
    // has been replaced by the db guid it has been satisfied with.
    u32 depc = base->depc;

    ocrPolicyDomain_t *pd = NULL;
    ocrWorker_t *worker = NULL;
    ocrPolicyMsg_t msg;
    getCurrentEnv(&pd, &worker, NULL, &msg);

    ocrEdtDep_t * depv = NULL;
    u64 doNotReleaseSlots = 0; // Used to support an EDT acquiring the same DB
                               // multiple times. For now limit to 64 DBs
                               // (ie: an EDT that does this should not have more
                               // than 64 DBs, others can have as many as they want)
    // If any dependencies, acquire their data-blocks
    u32 maxAcquiredDb = 0;

    ASSERT(derived->unkDbs == NULL); // Should be no dynamically acquired DBs before running

    if (depc != 0) {
        START_PROFILE(ta_hc_dbAcq);
        //TODO would be nice to resolve regNode into guids before
        depv = pd->fcts.pdMalloc(pd, sizeof(ocrEdtDep_t)*depc);
        // Double-check we're not rescheduling an already executed edt
        ASSERT(derived->signalers != END_OF_LIST);
        // Make sure the task was actually fully satisfied
        ASSERT(derived->slotSatisfiedCount == depc+1);
        while( maxAcquiredDb < depc ) {
            //TODO would be nice to standardize that on satisfy
            depv[maxAcquiredDb].guid = derived->signalers[maxAcquiredDb].guid;
            if(depv[maxAcquiredDb].guid != NULL_GUID) {
                // We send a message that we want to acquire the DB
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD(guid.guid) = depv[maxAcquiredDb].guid;
                PD_MSG_FIELD(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD(edt.guid) = base->guid;
                PD_MSG_FIELD(edt.metaDataPtr) = base;
                PD_MSG_FIELD(properties) = 1; // Runtime acquire
                // This call may fail if the policy domain goes down
                // while we are staring to execute

                if(pd->fcts.processMessage(pd, &msg, true)) {
                    // We are not going to launch the EDT
                    break;
                }
                switch(PD_MSG_FIELD(returnDetail)) {
                case 0:
                    ASSERT(PD_MSG_FIELD(ptr));
                    break; // Everything went fine
                case OCR_EACQ:
                    // The EDT was already acquired
                    ASSERT(PD_MSG_FIELD(ptr));
                    ASSERT(maxAcquiredDb < 64);
                    DPRINTF(DEBUG_LVL_VERB, "EDT (GUID: 0x%lx) acquiring DB (GUID: 0x%lx) multiple times. Ignoring acquire on slot %d\n",
                            base->guid, depv[maxAcquiredDb].guid, maxAcquiredDb);
                    doNotReleaseSlots |= (1ULL << maxAcquiredDb);
                    break;
                default:
                    ASSERT(0);
                }
                depv[maxAcquiredDb].ptr = PD_MSG_FIELD(ptr);
#undef PD_MSG
#undef PD_TYPE
            } else {
                depv[maxAcquiredDb].ptr = NULL;
            }
            ++maxAcquiredDb;
        }
        derived->signalers = END_OF_LIST;
        EXIT_PROFILE;
    }

    //Setup data parallel task
    ocrDataParallelTaskHc_t * dpEdt = (ocrDataParallelTaskHc_t *)base;
    dpEdt->depv = depv;
    dpEdt->maxAcquiredDb = maxAcquiredDb;
    dpEdt->doNotReleaseSlots = doNotReleaseSlots;

    if(depc == 0 || (maxAcquiredDb == depc)) {
        base->state = DATA_PARALLEL_ACTIVE_EDTSTATE;
        //Setup data parallel context
        ocrDpCtxt_t *dpCtxt = &(worker->dpCtxt);
        dpCtxt->task = base->guid;
        dpCtxt->latch = base->outputEvent;
        dpCtxt->lb = 0;
        dpCtxt->ub = base->dataParallelRange;
        dpCtxt->curIndex = (u64)(-1);
        hal_fence();
        dpCtxt->active = 1;
        return dataParallelActiveTaskExecute(base);
    } else {
        ocrTask_t *sinkBase = NULL;
        pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dpEdt->sinkTask, (u64*)(&sinkBase), NULL);
        ASSERT(sinkBase);
        return dataParallelSinkTaskExecute(sinkBase);
    }
}

u8 taskExecute(ocrTask_t* base) {
    switch(base->state) {
    case CREATED_EDTSTATE: return regularTaskExecute(base);
    case DATA_PARALLEL_CREATED_EDTSTATE: return dataParallelCreatedTaskExecute(base);
    case DATA_PARALLEL_ACTIVE_EDTSTATE: return dataParallelActiveTaskExecute(base);
    case DATA_PARALLEL_SINK_EDTSTATE: return dataParallelSinkTaskExecute(base);
    default: 
        ASSERT(0);
        break;
    }
}

void destructTaskFactoryHc(ocrTaskFactory_t* base) {
    runtimeChunkFree((u64)base, NULL);
}

ocrTaskFactory_t * newTaskFactoryHc(ocrParamList_t* perInstance, u32 factoryId) {
    ocrTaskFactory_t* base = (ocrTaskFactory_t*)runtimeChunkAlloc(sizeof(ocrTaskFactoryHc_t), NULL);

    base->instantiate = FUNC_ADDR(ocrTask_t* (*) (ocrTaskFactory_t*, ocrFatGuid_t, u32, u64*, u32, u32, ocrFatGuid_t, ocrFatGuid_t*, ocrTask_t *curEdt, ocrParamList_t*), newTaskHc);
    base->destruct =  FUNC_ADDR(void (*) (ocrTaskFactory_t*), destructTaskFactoryHc);
    base->factoryId = factoryId;

    base->fcts.destruct = FUNC_ADDR(u8 (*)(ocrTask_t*), destructTaskHc);
    base->fcts.satisfy = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32), satisfyTaskHc);
    base->fcts.registerSignaler = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32, bool), registerSignalerTaskHc);
    base->fcts.unregisterSignaler = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t, u32, bool), unregisterSignalerTaskHc);
    base->fcts.notifyDbAcquire = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t), notifyDbAcquireTaskHc);
    base->fcts.notifyDbRelease = FUNC_ADDR(u8 (*)(ocrTask_t*, ocrFatGuid_t), notifyDbReleaseTaskHc);
    base->fcts.execute = FUNC_ADDR(u8 (*)(ocrTask_t*), taskExecute);

    return base;
}

#endif /* ENABLE_TASK_HC */

#endif /* ENABLE_TASK_HC || ENABLE_TASKTEMPLATE_HC */
