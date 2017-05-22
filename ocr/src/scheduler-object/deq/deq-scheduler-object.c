/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_DEQ

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/deq/deq-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

/******************************************************/
/* OCR-DEQ SCHEDULER_OBJECT FUNCTIONS                 */
/******************************************************/

static void deqSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrDequeType_t deqType) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    self->loc = pd->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectDeq_t* deqSchedObj = (ocrSchedulerObjectDeq_t*)self;
    deqSchedObj->dequeType = deqType;
#ifdef SAL_FSIM_CE //TODO: This needs to be removed when bug #802 is fixed
    deqSchedObj->deque = NULL;
#else
    deqSchedObj->deque = newDeque(pd, NULL, deqType);
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    deqSchedObj->dequeRt = newDeque(pd, NULL, deqType);
#endif
#endif
}

static void deqSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_DEQUE;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectDeq_t* deqSchedObj = (ocrSchedulerObjectDeq_t*)self;
    deqSchedObj->dequeType = 0;
    deqSchedObj->deque = NULL;
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    deqSchedObj->dequeRt = NULL;
#endif
}

ocrSchedulerObject_t* newSchedulerObjectDeq(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectDeq_t), PERSISTENT_CHUNK);
    deqSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* deqSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectDeq_t));
    ADebug(AllocDebugAllPD, "deq-scheduler-object/deqSchedulerObjectCreate() "
           "pdMalloc(%ld), addr=%p\n", sizeof(ocrSchedulerObjectDeq_t), schedObj);
    deqSchedulerObjectInitialize(factory, schedObj);
    paramListSchedulerObjectDeq_t *paramDeq = (paramListSchedulerObjectDeq_t*)perInstance;
    deqSchedulerObjectStart(schedObj, pd, paramDeq->type);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 deqSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        ocrSchedulerObjectDeq_t* deqSchedObj = (ocrSchedulerObjectDeq_t*)self;
        if (deqSchedObj->deque) deqSchedObj->deque->destruct(pd, deqSchedObj->deque);
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
        if (deqSchedObj->dequeRt) deqSchedObj->dequeRt->destruct(pd, deqSchedObj->dequeRt);
#endif
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 deqSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ocrSchedulerObjectDeq_t *schedObj = (ocrSchedulerObjectDeq_t*)self;
    ASSERT(IS_SCHEDULER_OBJECT_TYPE_SINGLETON(element->kind));
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    if (IS_SCHEDULER_OBJECT_TYPE_RUNTIME(element->kind)) {
        deque_t * deq = schedObj->dequeRt;
        if (deq == NULL) {
            ocrPolicyDomain_t *pd = NULL;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            deq = newDeque(pd, NULL, schedObj->dequeType);
            schedObj->dequeRt = deq;
        }
        //Sanity check - Ensure work is local
        ASSERT(element->guid.metaDataPtr != NULL);
        deq->pushAtTail(deq, (void *)(element->guid.metaDataPtr), 0);
        return 0;
    }
#endif
    deque_t * deq = schedObj->deque;
    if (deq == NULL) {
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        deq = newDeque(pd, NULL, schedObj->dequeType);
        schedObj->deque = deq;
    }
    //Sanity check - Ensure work is local
    ASSERT(element->guid.metaDataPtr != NULL);
    deq->pushAtTail(deq, (void *)(element->guid.metaDataPtr), 0);

    return 0;
}

u8 deqSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    u32 i;
    ocrSchedulerObjectDeq_t *schedObj = (ocrSchedulerObjectDeq_t*)self;
    ASSERT(IS_SCHEDULER_OBJECT_TYPE_SINGLETON(kind));
    deque_t * deq = schedObj->deque;
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    if (IS_SCHEDULER_OBJECT_TYPE_RUNTIME(kind))
        deq = schedObj->dequeRt;
#endif
    if (deq == NULL) return count;

    for (i = 0; i < count; i++) {
        ocrGuid_t retGuid = NULL_GUID;
        switch(properties) {
        case SCHEDULER_OBJECT_REMOVE_TAIL:
            {
                START_PROFILE(sched_deq_Pop);

                void *popVal = deq->popFromTail(deq, 0);
                if(popVal != NULL){
                    ocrTask_t *popTask = (ocrTask_t *)popVal;
                    retGuid = popTask->guid;
                }

                EXIT_PROFILE;
            }
            break;
        case SCHEDULER_OBJECT_REMOVE_HEAD:
            {
                START_PROFILE(sched_deq_Steal);

                void *popVal = deq->popFromHead(deq, 1);
                if(popVal != NULL){
                    ocrTask_t *popTask = (ocrTask_t *)popVal;
                    retGuid = popTask->guid;
                }

                EXIT_PROFILE;
            }
            break;
        default:
            ASSERT(0);
            return OCR_ENOTSUP;
        }
        if (ocrGuidIsNull(retGuid))
            break;

        if (IS_SCHEDULER_OBJECT_TYPE_SINGLETON(dst->kind)) {
            ASSERT((ocrGuidIsNull(dst->guid.guid)) && count == 1);
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

u64 deqSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrSchedulerObjectDeq_t *schedObj = (ocrSchedulerObjectDeq_t*)self;
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    if (properties == SCHEDULER_OBJECT_COUNT_RUNTIME_EDT) {
        deque_t * deq = schedObj->dequeRt;
        if (deq == NULL) return 0;
        return deq->size(deq);
    }
#endif
    deque_t * deq = schedObj->deque;
    if (deq == NULL) return 0;
    return deq->size(deq);
}

ocrSchedulerObjectIterator_t* deqSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 deqSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 deqSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObject_t* deqGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ASSERT(self->loc == loc && self->mapping == mapping);
    return self;
}

u8 deqSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* deqSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 deqSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 deqSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                    phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 deqSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 deqSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 deqSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

#ifdef ENABLE_RESILIENCY
u8 deqSchedulerObjectReset(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrSchedulerObjectDeq_t *schedObj = (ocrSchedulerObjectDeq_t*)self;
#ifdef ENABLE_RUNTIME_OBJECT_DEQUE
    schedObj->dequeRt->tail = schedObj->dequeRt->head;
#endif
    schedObj->deque->tail = schedObj->deque->head;
    return 0;
}
#endif

/******************************************************/
/* OCR-DEQ SCHEDULER_OBJECT FACTORY FUNCTIONS         */
/******************************************************/

void destructSchedulerObjectFactoryDeq(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryDeq(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t* schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryDeq_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectDeq_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_DEQUE;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryDeq;
    schedObjFact->instantiate = &newSchedulerObjectDeq;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), deqSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), deqSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), deqSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), deqSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), deqSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), deqSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), deqSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), deqSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), deqSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), deqGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), deqSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), deqSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), deqSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), deqSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), deqSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), deqSchedulerObjectOcrPolicyMsgUnMarshallMsg);

#ifdef ENABLE_RESILIENCY
    schedObjFact->fcts.reset = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), deqSchedulerObjectReset);
#endif
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_DEQ */
