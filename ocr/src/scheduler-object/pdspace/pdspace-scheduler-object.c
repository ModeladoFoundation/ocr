/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_PDSPACE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/pdspace/pdspace-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

/******************************************************/
/* OCR-PDSPACE SCHEDULER_OBJECT FUNCTIONS             */
/******************************************************/

static void pdspaceSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD) {
    self->loc = PD->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectPdspace_t *pdspaceSchedObj = (ocrSchedulerObjectPdspace_t*)self;
#ifdef ENABLE_SCHEDULER_OBJECT_MAP
    paramListSchedulerObjectMap_t paramMap;
    paramMap.base.config = 0;
    paramMap.base.guidRequired = 0;
    paramMap.type = OCR_MAP_TYPE_MODULO_LOCKED;
    paramMap.nbBuckets = 16;
    ocrSchedulerObjectFactory_t *mapFactory = PD->schedulerObjectFactories[schedulerObjectMap_id];
    pdspaceSchedObj->dbMap = mapFactory->fcts.create(mapFactory, (ocrParamList_t*)(&paramMap));
#else
    ASSERT(0);
#endif

#ifdef ENABLE_SCHEDULER_OBJECT_WST
    paramListSchedulerObjectWst_t paramWst;
    paramMap.base.config = 0;
    paramWst.base.guidRequired = 0;
    ocrSchedulerObjectFactory_t *wstFactory = PD->schedulerObjectFactories[schedulerObjectWst_id];
    pdspaceSchedObj->wst = wstFactory->fcts.create(wstFactory, (ocrParamList_t*)(&paramWst));
#else
    ASSERT(0);
#endif
}

static void pdspaceSchedulerObjectFinish(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD) {
    ocrSchedulerObjectPdspace_t *pdspaceSchedObj = (ocrSchedulerObjectPdspace_t*)self;
#ifdef ENABLE_SCHEDULER_OBJECT_MAP
    ocrSchedulerObjectFactory_t *mapFactory = PD->schedulerObjectFactories[schedulerObjectMap_id];
    mapFactory->fcts.destroy(mapFactory, pdspaceSchedObj->dbMap);
#else
    ASSERT(0);
#endif

#ifdef ENABLE_SCHEDULER_OBJECT_WST
    ocrSchedulerObjectFactory_t *wstFactory = PD->schedulerObjectFactories[schedulerObjectWst_id];
    wstFactory->fcts.destroy(wstFactory, pdspaceSchedObj->wst);
#else
    ASSERT(0);
#endif
}

static void pdspaceSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_PDSPACE;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectPdspace_t* pdspaceSchedObj = (ocrSchedulerObjectPdspace_t*)self;
    pdspaceSchedObj->dbMap = NULL;
    pdspaceSchedObj->wst = NULL;

}

ocrSchedulerObject_t* newSchedulerObjectPdspace(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif

    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectPdspace_t), PERSISTENT_CHUNK);
    pdspaceSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* pdspaceSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrPolicyDomain_t *pd = factory->pd;
    ocrSchedulerObject_t *schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectPdspace_t));
    pdspaceSchedulerObjectInitialize(factory, schedObj);
    pdspaceSchedulerObjectStart(schedObj, pd);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 pdspaceSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = fact->pd;
        pdspaceSchedulerObjectFinish(self, pd);
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 pdspaceSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pdspaceSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObject_t *element, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u64 pdspaceSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrSchedulerObjectPdspace_t *pdspaceSchedObj = (ocrSchedulerObjectPdspace_t*)self;
    ocrPolicyDomain_t *pd = fact->pd;
    u64 count = 0;
    if (properties & SCHEDULER_OBJECT_COUNT_EDT) {
        ocrSchedulerObjectFactory_t *wstFactory = pd->schedulerObjectFactories[pdspaceSchedObj->wst->fctId];
        count += wstFactory->fcts.count(wstFactory, pdspaceSchedObj->wst, properties);
    }
    return count;
}

ocrSchedulerObjectIterator_t* pdspaceSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *factory, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 pdspaceSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * factory, ocrSchedulerObjectIterator_t *iterator) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pdspaceSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObject_t* pdspaceGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ocrSchedulerObject_t *schedObj = NULL;
    /*if (properties & SCHEDULER_OBJECT_MAPPING_WST) {
    ocrPolicyDomain_t *pd = fact->pd;
    ocrSchedulerObjectPdspace_t *pdspaceSchedObj = (ocrSchedulerObjectPdspace_t*)self;
        ocrSchedulerObjectFactory_t *wstFactory = pd->schedulerObjectFactories[pdspaceSchedObj->wst->fctId];
        schedObj = wstFactory->fcts.getSchedulerObjectForLocation(wstFactory, pdspaceSchedObj->wst, loc, mapping, properties);
    }*/ //TODO:
    if (properties & SCHEDULER_OBJECT_CREATE_IF_ABSENT)
        ASSERT(schedObj);
    return schedObj;
}

u8 pdspaceSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* pdspaceSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 pdspaceSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pdspaceSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
            u32 i;
            // The scheduler calls this before switching itself. Do we want
            // to invert this?
            for(i = 0; i < PD->schedulerObjectFactoryCount; ++i) {
                PD->schedulerObjectFactories[i]->pd = PD;
            }
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        // Memory is up
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
                pdspaceSchedulerObjectStart(self, PD);
            }
        } else {
            // Tear down
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_GUID_OK, phase)) {
                pdspaceSchedulerObjectFinish(self, PD);
            }
        }
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        break;
    default:
        ASSERT(0);
    }
    return toReturn;
    /* BUG #583: There was this code on STOP, not sure if we still need it
       ocrSchedulerObject_t *schedObj = (ocrSchedulerObject_t*)self;
       ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
       ASSERT(pdspaceSchedulerObjectCount(fact, schedObj, 0) == 0);
    */
}

u8 pdspaceSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pdspaceSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 pdspaceSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-PDSPACE SCHEDULER_OBJECT FACTORY FUNCTIONS     */
/******************************************************/

void destructSchedulerObjectFactoryPdspace(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryPdspace(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t *schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryPdspace_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectPdspace_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_PDSPACE;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryPdspace;
    schedObjFact->instantiate = &newSchedulerObjectPdspace;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), pdspaceSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), pdspaceSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), pdspaceSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), pdspaceSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), pdspaceSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), pdspaceSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), pdspaceSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), pdspaceSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), pdspaceSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), pdspaceGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), pdspaceSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), pdspaceSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), pdspaceSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), pdspaceSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), pdspaceSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), pdspaceSchedulerObjectOcrPolicyMsgUnMarshallMsg);

    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_PDSPACE */
