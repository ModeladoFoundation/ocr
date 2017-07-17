/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#include "extensions/ocr-hints.h"
#ifdef ENABLE_SCHEDULER_OBJECT_BIN_HEAP

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/bin-heap/bin-heap-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

/******************************************************/
/* OCR-BIN_HEAP SCHEDULER_OBJECT FUNCTIONS                 */
/******************************************************/

static void binHeapSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrBinHeapType_t binHeapType) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    self->loc = pd->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectBinHeap_t* binHeapSchedObj = (ocrSchedulerObjectBinHeap_t*)self;
    binHeapSchedObj->binHeapType = binHeapType;
#ifdef SAL_FSIM_CE //TODO: This needs to be removed when bug #802 is fixed
    binHeapSchedObj->binHeap = NULL;
#else
    binHeapSchedObj->binHeap = newBinHeap(pd, binHeapType);
#endif
}

static void binHeapSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_BIN_HEAP;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectBinHeap_t* binHeapSchedObj = (ocrSchedulerObjectBinHeap_t*)self;
    binHeapSchedObj->binHeapType = 0;
    binHeapSchedObj->binHeap = NULL;
}

ocrSchedulerObject_t* newSchedulerObjectBinHeap(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
    paramListSchedulerObject_t *paramSchedObj __attribute__((unused)) = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectBinHeap_t), PERSISTENT_CHUNK);
    binHeapSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* binHeapSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
    paramListSchedulerObject_t *paramSchedObj __attribute__((unused)) = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectBinHeap_t));
    binHeapSchedulerObjectInitialize(factory, schedObj);
    paramListSchedulerObjectBinHeap_t *paramBinHeap = (paramListSchedulerObjectBinHeap_t*)perInstance;
    binHeapSchedulerObjectStart(schedObj, pd, paramBinHeap->type);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 binHeapSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        ocrSchedulerObjectBinHeap_t* binHeapSchedObj = (ocrSchedulerObjectBinHeap_t*)self;
        if (binHeapSchedObj->binHeap) binHeapSchedObj->binHeap->destruct(pd, binHeapSchedObj->binHeap);
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 binHeapSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ocrSchedulerObjectBinHeap_t *schedObj = (ocrSchedulerObjectBinHeap_t*)self;
    ASSERT(IS_SCHEDULER_OBJECT_TYPE_SINGLETON(element->kind));
    binHeap_t * heap = schedObj->binHeap;
    ocrGuid_t edtGuid = element->guid.guid;
    // FIXME: should default to ZERO, but that doesn't play well with OCR_TASK_FLAG_RUNTIME_EDT,
    // since none of the runtime EDTs execute when you expect...
    s64 priority = 0;
    ocrTask_t* task = element->guid.metaDataPtr;
    ASSERT(task);
    if(task->flags & OCR_TASK_FLAG_RUNTIME_EDT){
        //give runtime EDTs maximum priority
        priority = INT64_MAX;
    }

    { // read EDT hint
        ASSERT(element->kind == OCR_SCHEDULER_OBJECT_EDT);
        ocrHint_t edtHints;
        ocrHintInit(&edtHints, OCR_HINT_EDT_T);
        ocrGetHint(edtGuid, &edtHints);
        ocrGetHintValue(&edtHints, OCR_HINT_EDT_PRIORITY, (u64*)&priority);
#ifdef ENABLE_SCHEDULER_OBJECT_SPAWN_QUEUE //really don't care about queue itself, re-using define
        //NOTE: this overrides the OCR_HINT_EDT_PRIORITY value if present:
        ocrGetHintValue(&edtHints, OCR_HINT_EDT_SPAWNING, (u64*)&priority);
#endif

    }

    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    heap->push(heap, (void *)edtGuid.guid, priority, 0);
#elif GUID_BIT_COUNT == 128
    heap->push(heap, (void *)edtGuid.lower, priority, 0);
#endif

    return 0;
}

static inline ocrGuid_t _popGuid(binHeap_t *binHeap, int flag) {
    // See BUG #928 on GUID issues
    void *data = binHeap->pop(binHeap, flag);
    ocrGuid_t retGuid = NULL_GUID;
#if GUID_BIT_COUNT == 64
    if (data) retGuid.guid = (intptr_t)data;
#elif GUID_BIT_COUNT == 128
    if (data) retGuid.lower = (intptr_t)data;
#endif
    return retGuid;
}


u8 binHeapSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    u32 i;
    ocrSchedulerObjectBinHeap_t *schedObj = (ocrSchedulerObjectBinHeap_t*)self;
    ASSERT(IS_SCHEDULER_OBJECT_TYPE_SINGLETON(kind));
    binHeap_t * binHeap = schedObj->binHeap;
    if (binHeap == NULL) return count;

    for (i = 0; i < count; i++) {
        ocrGuid_t retGuid = NULL_GUID;
        switch(properties) {
        case SCHEDULER_OBJECT_REMOVE_TAIL:
            {
                START_PROFILE(sched_binHeap_Pop);
                retGuid = _popGuid(binHeap, 0);
                EXIT_PROFILE;
            }
            break;
        case SCHEDULER_OBJECT_REMOVE_HEAD:
            {
                START_PROFILE(sched_binHeap_Steal);
                retGuid = _popGuid(binHeap, 1);
                EXIT_PROFILE;
            }
            break;
        default:
            ASSERT(0);
            return OCR_ENOTSUP;
        }

        if(ocrGuidIsNull(retGuid))
            break;

        if (IS_SCHEDULER_OBJECT_TYPE_SINGLETON(dst->kind)) {
            ASSERT(ocrGuidIsNull(dst->guid.guid) && count == 1);
            dst->guid.guid = retGuid;
        } else {
            ocrSchedulerObject_t taken;
            taken.guid.guid = retGuid;
            taken.kind = kind;
            ocrSchedulerObjectFactory_t *dstFactory = fact->pd->schedulerObjectFactories[dst->fctId];
            dstFactory->fcts.insert(dstFactory, dst, &taken, NULL, 0);
        }
    }

    // Success (0) if at least one element has been removed
    return (i == 0);
}

u64 binHeapSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrSchedulerObjectBinHeap_t *schedObj = (ocrSchedulerObjectBinHeap_t*)self;
    binHeap_t * binHeap = schedObj->binHeap;
    if (binHeap == NULL) return 0;
    return binHeap->count; //this may be racy but ok for approx count
}

ocrSchedulerObjectIterator_t* binHeapSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 binHeapSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 binHeapSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObject_t* binHeapGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ASSERT(self->loc == loc && self->mapping == mapping);
    return self;
}

u8 binHeapSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* binHeapSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 binHeapSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 binHeapSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                    phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 binHeapSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 binHeapSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 binHeapSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-BIN_HEAP SCHEDULER_OBJECT FACTORY FUNCTIONS         */
/******************************************************/

void destructSchedulerObjectFactoryBinHeap(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryBinHeap(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t* schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryBinHeap_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectBinHeap_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_BIN_HEAP;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryBinHeap;
    schedObjFact->instantiate = &newSchedulerObjectBinHeap;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), binHeapSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), binHeapSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), binHeapSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), binHeapSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), binHeapSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), binHeapSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), binHeapSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), binHeapSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), binHeapSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), binHeapGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), binHeapSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), binHeapSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), binHeapSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), binHeapSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), binHeapSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), binHeapSchedulerObjectOcrPolicyMsgUnMarshallMsg);
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_BIN_HEAP */
