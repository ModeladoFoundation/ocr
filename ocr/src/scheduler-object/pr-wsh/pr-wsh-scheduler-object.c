/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_PR_WSH

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/pr-wsh/pr-wsh-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

#define DEBUG_TYPE SCHEDULER_OBJECT

/*********************************************************/
/* OCR PR-WSH SCHEDULER_OBJECT FUNCTIONS                 */
/*********************************************************/

static void prWshSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    self->loc = pd->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectPrWsh_t *prWshSchedObj = (ocrSchedulerObjectPrWsh_t*)self;
#ifdef ENABLE_SCHEDULER_OBJECT_BIN_HEAP
    //Instantiate the binHeap schedulerObjects
    paramListSchedulerObjectBinHeap_t params;
    params.base.config = 0;
    params.base.guidRequired = 0;
    params.type = LOCKED_BIN_HEAP;
    ocrSchedulerObjectFactory_t *binHeapFactory = PD->schedulerObjectFactories[schedulerObjectBinHeap_id];
    prWshSchedObj->heap = binHeapFactory->fcts.create(binHeapFactory, (ocrParamList_t*)(&params));
    binHeapFactory->fcts.setLocationForSchedulerObject(binHeapFactory, prWshSchedObj->heap, 0, OCR_SCHEDULER_OBJECT_MAPPING_PINNED);
#else
    ASSERT(0);
#endif
}

static void prWshSchedulerObjectFinish(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD) {
    ocrSchedulerObjectPrWsh_t *prWshSchedObj = (ocrSchedulerObjectPrWsh_t*)self;
    ocrSchedulerObject_t *binHeap = prWshSchedObj->heap;
    ocrSchedulerObjectFactory_t *binHeapFactory = PD->schedulerObjectFactories[binHeap->fctId];
    binHeapFactory->fcts.destroy(binHeapFactory, binHeap);
}

static void prWshSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_PR_WSH;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectPrWsh_t* prWshSchedObj = (ocrSchedulerObjectPrWsh_t*)self;
    prWshSchedObj->heap = NULL;
}

ocrSchedulerObject_t* newSchedulerObjectPrWsh(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
    paramListSchedulerObject_t *paramSchedObj __attribute__((unused)) = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectPrWsh_t), PERSISTENT_CHUNK);
    prWshSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* prWshSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
    paramListSchedulerObject_t *paramSchedObj __attribute__((unused)) = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectPrWsh_t));
    ADebug(AllocDebugAllPD, "pr-wsh-scheduler-object.c/prWshSchedulerObjectCreate() "
       "pdMalloc(%ld), addr=%p\n", sizeof(ocrSchedulerObjectPrWsh_t), schedObj);
    prWshSchedulerObjectInitialize(factory, schedObj);
    prWshSchedulerObjectStart(schedObj, pd);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 prWshSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        prWshSchedulerObjectFinish(self, pd);
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 prWshSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 prWshSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u64 prWshSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrSchedulerObjectPrWsh_t *prWshSchedObj = (ocrSchedulerObjectPrWsh_t*)self;
    ocrPolicyDomain_t *pd = fact->pd;
    ocrSchedulerObject_t *heap = prWshSchedObj->heap;
    ocrSchedulerObjectFactory_t *heapFactory = pd->schedulerObjectFactories[heap->fctId];
    const u64 count = heapFactory->fcts.count(heapFactory, heap, properties);
    return count;
}

ocrSchedulerObjectIterator_t* prWshSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 prWshSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 prWshSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObject_t* prWshGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ocrSchedulerObjectPrWsh_t *prWshSchedObj = (ocrSchedulerObjectPrWsh_t*)self;
    return prWshSchedObj->heap;
}

u8 prWshSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* prWshSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 prWshSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 prWshSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_MEMORY_OK\n");
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
            u32 i;
            // The scheduler calls this before switching itself. Do we want
            // to invert this?
            for(i = 0; i < PD->schedulerObjectFactoryCount; ++i) {
                if(PD->schedulerObjectFactories[i])
                    PD->schedulerObjectFactories[i]->pd = PD;
            }
        }
        break;
    case RL_GUID_OK:
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_GUID_OK\n");
        // Memory is up
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
                prWshSchedulerObjectStart(self, PD);
            }
        } else {
            // Tear down
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
                prWshSchedulerObjectFinish(self, PD);
            }
        }
        break;
    case RL_COMPUTE_OK:
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_COMPUTE_OK\n");
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
                ocrSchedulerObjectPrWsh_t *prWshSchedObj = (ocrSchedulerObjectPrWsh_t*)self;
                ocrSchedulerObject_t *heap = prWshSchedObj->heap;
                ocrSchedulerObjectFactory_t *heapFactory = PD->schedulerObjectFactories[heap->fctId];
                heapFactory->fcts.setLocationForSchedulerObject(heapFactory, heap, PD->myLocation, OCR_SCHEDULER_OBJECT_MAPPING_PINNED);
            }
        }
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
       ASSERT(prWshSchedulerObjectCount(fact, schedObj, 0) == 0);
    */
}

u8 prWshSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 prWshSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 prWshSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/*********************************************************/
/* OCR PR-WSH SCHEDULER_OBJECT FACTORY FUNCTIONS         */
/*********************************************************/

void destructSchedulerObjectFactoryPrWsh(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryPrWsh(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t *schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryPrWsh_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectPrWsh_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_PR_WSH;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryPrWsh;
    schedObjFact->instantiate = &newSchedulerObjectPrWsh;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), prWshSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), prWshSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), prWshSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), prWshSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), prWshSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), prWshSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), prWshSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), prWshSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), prWshSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), prWshGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), prWshSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), prWshSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), prWshSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), prWshSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), prWshSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), prWshSchedulerObjectOcrPolicyMsgUnMarshallMsg);
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_PR_WSH */
