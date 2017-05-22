/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_MAP

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/map/map-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

/******************************************************/
/* OCR-MAP SCHEDULER_OBJECT FUNCTIONS                 */
/******************************************************/

static void mapSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrMapType mapType, u32 nbBuckets) {
    self->loc = PD->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectMap_t* mapSchedObj = (ocrSchedulerObjectMap_t*)self;
    mapSchedObj->type = mapType;
    switch(mapType) {
    case OCR_MAP_TYPE_MODULO: {
            mapSchedObj->map = newHashtableModulo(PD, nbBuckets);
            mapSchedObj->mapFcts.get = &hashtableNonConcGet;
            mapSchedObj->mapFcts.put = &hashtableNonConcPut;
            mapSchedObj->mapFcts.tryPut = &hashtableNonConcTryPut;
            mapSchedObj->mapFcts.remove = &hashtableNonConcRemove;
        }
        break;
    case OCR_MAP_TYPE_MODULO_LOCKED: {
            mapSchedObj->map = newHashtableBucketLockedModulo(PD, nbBuckets);
            mapSchedObj->mapFcts.get = &hashtableConcBucketLockedGet;
            mapSchedObj->mapFcts.put = &hashtableConcBucketLockedPut;
            mapSchedObj->mapFcts.tryPut = &hashtableConcBucketLockedTryPut;
            mapSchedObj->mapFcts.remove = &hashtableConcBucketLockedRemove;
        }
        break;
    default:
        ASSERT(0);
        break;
    }
}

static void mapSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_MAP;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectMap_t* mapSchedObj = (ocrSchedulerObjectMap_t*)self;
    mapSchedObj->type = 0;
    mapSchedObj->map = NULL;
}

ocrSchedulerObject_t* newSchedulerObjectMap(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectMap_t), PERSISTENT_CHUNK);
    mapSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* mapSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrPolicyDomain_t *pd = factory->pd;
    ocrSchedulerObject_t *schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectMap_t));
    ADebug(AllocDebugAllPD, "map-scheduler-object.c/mapSchedulerObjectCreate() "
           "pdMalloc(%ld), addr=%p\n", sizeof(ocrSchedulerObjectMap_t), schedObj);
    mapSchedulerObjectInitialize(factory, schedObj);
    paramListSchedulerObjectMap_t *paramsMap = (paramListSchedulerObjectMap_t*)perInstance;
    mapSchedulerObjectStart(schedObj, pd, paramsMap->type, paramsMap->nbBuckets);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 mapSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = fact->pd;
        ocrSchedulerObjectMap_t* mapSchedObj = (ocrSchedulerObjectMap_t*)self;
        switch(mapSchedObj->type) {
        case OCR_MAP_TYPE_MODULO:
            destructHashtable(mapSchedObj->map, NULL, NULL);
            break;
        case OCR_MAP_TYPE_MODULO_LOCKED:
            destructHashtableBucketLocked(mapSchedObj->map, NULL, NULL);
            break;
        default:
            ASSERT(0);
            return OCR_ENOTSUP;
        }
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 mapSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ocrSchedulerObjectMap_t *mapObj = (ocrSchedulerObjectMap_t*)self;
    if (iterator && properties == 0) properties = (SCHEDULER_OBJECT_INSERT_INPLACE | SCHEDULER_OBJECT_INSERT_POSITION_ITERATOR);
    switch(properties & SCHEDULER_OBJECT_INSERT_POSITION) {
    case SCHEDULER_OBJECT_INSERT_POSITION_HEAD:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_INSERT_POSITION_TAIL:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_INSERT_POSITION_ITERATOR:
        {
            ocrSchedulerObjectMapIterator_t *mit = (ocrSchedulerObjectMapIterator_t*)iterator;
            ASSERT(mit->key);
            switch(properties & SCHEDULER_OBJECT_INSERT_KIND) {
            case SCHEDULER_OBJECT_INSERT_BEFORE:
                return OCR_ENOTSUP;
            case SCHEDULER_OBJECT_INSERT_AFTER:
                return OCR_ENOTSUP;
            case SCHEDULER_OBJECT_INSERT_INPLACE:
                {
                    void *value = NULL;
                    if (element && element->guid.metaDataPtr) {
                        value = element->guid.metaDataPtr;
                    } else {
                        value = iterator->data;
                    }
                    ASSERT(value);
                    mapObj->mapFcts.put(mapObj->map, mit->key, value);
                }
                break;
            default:
                ASSERT(0);
                break;
            }
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    return 0;
}

u8 mapSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    switch(properties) {
    case SCHEDULER_OBJECT_REMOVE_HEAD:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_REMOVE_TAIL:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_REMOVE_ITERATOR:
        {
            ocrSchedulerObjectMap_t *mapObj = (ocrSchedulerObjectMap_t*)iterator->schedObj;
            ocrSchedulerObjectMapIterator_t *mit = (ocrSchedulerObjectMapIterator_t*)iterator;
            ASSERT(mit->key);
            void* value = NULL;
            mapObj->mapFcts.remove(mapObj->map, mit->key, &value);
            iterator->data = value;
            if (dst && dst->guid.metaDataPtr == NULL) dst->guid.metaDataPtr = value;
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    return 0;
}

u64 mapSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObjectIterator_t* mapSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrPolicyDomain_t *pd = fact->pd;
    ocrSchedulerObjectIterator_t* iterator = (ocrSchedulerObjectIterator_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectMapIterator_t));
    ADebug(AllocDebugAllPD, "map-scheduler-object.c/mapSchedulerObjectCreateIterator() "
           "pdMalloc(%ld), addr=%p\n", sizeof(ocrSchedulerObjectMapIterator_t), iterator);
    iterator->schedObj = self;
    iterator->data = NULL;
    iterator->fctId = fact->factoryId;
    ocrSchedulerObjectMapIterator_t *mit = (ocrSchedulerObjectMapIterator_t*)iterator;
    mit->internal = self;
    mit->key = NULL;
    return iterator;
}

u8 mapSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator) {
    ocrPolicyDomain_t *pd = fact->pd;
    pd->fcts.pdFree(pd, iterator);
    return 0;
}

u8 mapSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ocrSchedulerObjectMap_t *mapObj = (ocrSchedulerObjectMap_t*)iterator->schedObj;
    ASSERT(mapObj);
    ocrSchedulerObjectMapIterator_t *mit = (ocrSchedulerObjectMapIterator_t*)iterator;
    if (mit->internal != iterator->schedObj) {
        mit->internal = iterator->schedObj;
        mit->key = NULL;
    }
    switch(properties) {
    case SCHEDULER_OBJECT_ITERATE_CURRENT:
        {
            iterator->data = NULL;
            if (mit->key) iterator->data = mapObj->mapFcts.get(mapObj->map, mit->key);
        }
        break;
    case SCHEDULER_OBJECT_ITERATE_HEAD:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_ITERATE_TAIL:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_ITERATE_NEXT:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_ITERATE_PREV:
        return OCR_ENOTSUP;
    case SCHEDULER_OBJECT_ITERATE_SEARCH_KEY:
        {
            ASSERT(iterator->data);
            mit->key = iterator->data;
            iterator->data = mapObj->mapFcts.get(mapObj->map, mit->key);
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    return 0;
}

ocrSchedulerObject_t* mapGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 mapSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* mapSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 mapSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 mapSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                    phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 mapSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 mapSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 mapSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-MAP SCHEDULER_OBJECT FACTORY FUNCTIONS         */
/******************************************************/

void destructSchedulerObjectFactoryMap(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryMap(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t* schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryMap_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectMap_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_MAP;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryMap;
    schedObjFact->instantiate = &newSchedulerObjectMap;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), mapSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), mapSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), mapSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), mapSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), mapSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), mapSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), mapSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), mapSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), mapSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), mapGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), mapSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), mapSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), mapSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), mapSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), mapSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), mapSchedulerObjectOcrPolicyMsgUnMarshallMsg);
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_MAP */
