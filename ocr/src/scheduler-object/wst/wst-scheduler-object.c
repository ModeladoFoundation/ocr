/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_WST

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/wst/wst-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"
#include "worker/hc/hc-worker.h"

#define DEBUG_TYPE SCHEDULER_OBJECT

/******************************************************/
/* OCR-WST SCHEDULER_OBJECT FUNCTIONS                 */
/******************************************************/

static void wstSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, u32 numDeques) {
    u32 i, w;
    ASSERT(numDeques > 0);
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrScheduler_t *scheduler = PD->schedulers[0];
    ocrSchedulerHeuristic_t *masterSchedulerHeuristic = scheduler->schedulerHeuristics[scheduler->masterHeuristicId];
    ASSERT(masterSchedulerHeuristic->contextCount == numDeques);

    self->loc = pd->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self;
    wstSchedObj->numDeques = numDeques;
    wstSchedObj->deques = (ocrSchedulerObject_t**)pd->fcts.pdMalloc(pd, numDeques * sizeof(ocrSchedulerObject_t*));
#ifdef ENABLE_SCHEDULER_OBJECT_DEQ
    //Instantiate the deque schedulerObjects
    paramListSchedulerObjectDeq_t params;
    params.base.config = 0;
    params.base.guidRequired = 0;
    params.type = WORK_STEALING_DEQUE;
    ocrSchedulerObjectFactory_t *dequeFactory = pd->schedulerObjectFactories[schedulerObjectDeq_id];
    for (i = 0, w = 0; i < numDeques; i++) {
        if (wstSchedObj->config == SCHEDULER_OBJECT_WST_CONFIG_STATIC) {
            params.type = SEMI_CONCURRENT_DEQUE;
#ifdef ENABLE_WORKER_HC
            ocrWorkerHc_t *w0 = (ocrWorkerHc_t*)pd->workers[0];
            if (w0->hcType == HC_WORKER_COMM && i == 0) {
                params.type = WORK_STEALING_DEQUE;
            }
#endif
        }
        ocrSchedulerObject_t *deque = dequeFactory->fcts.create(dequeFactory, (ocrParamList_t*)(&params));
        wstSchedObj->deques[i] = deque;
        //BUG #920 cleanup - Should not be referencing master heuristic
        if (masterSchedulerHeuristic->contexts != NULL) {
            ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)masterSchedulerHeuristic->contexts[i];
            if (context->location == PD->myLocation) {
                dequeFactory->fcts.setLocationForSchedulerObject(dequeFactory, deque, w++, OCR_SCHEDULER_OBJECT_MAPPING_WORKER);
            } else {
                dequeFactory->fcts.setLocationForSchedulerObject(dequeFactory, deque, context->location, OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
            }
        }
    }
#else
    ASSERT(0);
#endif
}

static void wstSchedulerObjectFinish(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD) {
    u32 i;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self;
    for (i = 0; i < wstSchedObj->numDeques; i++) {
        ocrSchedulerObject_t *deque = wstSchedObj->deques[i];
        ocrSchedulerObjectFactory_t *dequeFactory = PD->schedulerObjectFactories[deque->fctId];
        dequeFactory->fcts.destroy(dequeFactory, wstSchedObj->deques[i]);
    }
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    pd->fcts.pdFree(pd, wstSchedObj->deques);
}

static void wstSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrParamList_t *perInstance) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_WST;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectWst_t* wstSchedObj = (ocrSchedulerObjectWst_t*)self;
    wstSchedObj->numDeques = 0;
    wstSchedObj->deques = NULL;
    wstSchedObj->config = SCHEDULER_OBJECT_WST_CONFIG_REGULAR;
    paramListSchedulerObjectWst_t *paramsWst = (paramListSchedulerObjectWst_t*)perInstance;
    // There's no specific initialization for now. Default should be regular and
    // if there's any init it should be done when paramWst is not null too.
    if (paramsWst != NULL) {
        wstSchedObj->config = paramsWst->config;
        switch(paramsWst->config) {
        case SCHEDULER_OBJECT_WST_CONFIG_REGULAR:
        case SCHEDULER_OBJECT_WST_CONFIG_STATIC:
            break;
        default:
            ASSERT(0);
            break;
        }
    }
}

ocrSchedulerObject_t* newSchedulerObjectWst(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectWst_t), PERSISTENT_CHUNK);
    wstSchedulerObjectInitialize(factory, schedObj, perInstance);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* wstSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectWst_t));
    wstSchedulerObjectInitialize(factory, schedObj, perInstance);
    paramListSchedulerObjectWst_t *paramsWst = (paramListSchedulerObjectWst_t*)perInstance;
    wstSchedulerObjectStart(schedObj, pd, paramsWst->numDeques);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 wstSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        wstSchedulerObjectFinish(self, pd);
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 wstSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 wstSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u64 wstSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    u64 count = 0;
    u32 i;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self;
    ocrPolicyDomain_t *pd = fact->pd;
    for (i = 0; i < wstSchedObj->numDeques; i++) {
        ocrSchedulerObject_t *deque = wstSchedObj->deques[i];
        ocrSchedulerObjectFactory_t *dequeFactory = pd->schedulerObjectFactories[deque->fctId];
        count += dequeFactory->fcts.count(dequeFactory, deque, properties);
    }
    return count;
}

ocrSchedulerObjectIterator_t* wstSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 wstSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 wstSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObject_t* wstGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    u32 i;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self;
    for (i = 0; i < wstSchedObj->numDeques; i++) {
        if (wstSchedObj->deques[i]->loc == loc && wstSchedObj->deques[i]->mapping == mapping)
            return wstSchedObj->deques[i];
    }
    return NULL;
}

u8 wstSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* wstSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 wstSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 wstSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
                ocrScheduler_t *scheduler = PD->schedulers[0];
                ocrSchedulerHeuristic_t *masterSchedulerHeuristic = scheduler->schedulerHeuristics[scheduler->masterHeuristicId];
                wstSchedulerObjectStart(self, PD, masterSchedulerHeuristic->contextCount);
            }
        } else {
            // Tear down
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
                wstSchedulerObjectFinish(self, PD);
            }
        }
        break;
    case RL_COMPUTE_OK:
        DPRINTF(DEBUG_LVL_VVERB, "Runlevel: RL_COMPUTE_OK\n");
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
                u32 i, w;
                ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self;
                ocrScheduler_t *scheduler = PD->schedulers[0];
                ocrSchedulerHeuristic_t *masterSchedulerHeuristic = scheduler->schedulerHeuristics[scheduler->masterHeuristicId];
                for (i = 0, w = 0; i < masterSchedulerHeuristic->contextCount; i++) {
                    ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)masterSchedulerHeuristic->contexts[i];
                    ocrSchedulerObject_t *deque = wstSchedObj->deques[i];
                    ocrSchedulerObjectFactory_t *dequeFactory = PD->schedulerObjectFactories[deque->fctId];
                    if (context->location == PD->myLocation) {
                        dequeFactory->fcts.setLocationForSchedulerObject(dequeFactory, deque, w++, OCR_SCHEDULER_OBJECT_MAPPING_WORKER);
                    } else {
                        dequeFactory->fcts.setLocationForSchedulerObject(dequeFactory, deque, context->location, OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
                    }
                }
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
       ASSERT(wstSchedulerObjectCount(fact, schedObj, 0) == 0);
    */
}

u8 wstSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 wstSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 wstSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

#ifdef ENABLE_RESILIENCY
u8 wstSchedulerObjectReset(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    u32 i;
    ocrSchedulerObjectWst_t *wstSchedObj = (ocrSchedulerObjectWst_t*)self;
    ocrPolicyDomain_t *pd = fact->pd;
    for (i = 0; i < wstSchedObj->numDeques; i++) {
        ocrSchedulerObject_t *deque = wstSchedObj->deques[i];
        ocrSchedulerObjectFactory_t *dequeFactory = pd->schedulerObjectFactories[deque->fctId];
        dequeFactory->fcts.reset(dequeFactory, deque, properties);
    }
    return 0;
}
#endif

/******************************************************/
/* OCR-WST SCHEDULER_OBJECT FACTORY FUNCTIONS         */
/******************************************************/

void destructSchedulerObjectFactoryWst(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryWst(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t *schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryWst_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectWst_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_WST;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryWst;
    schedObjFact->instantiate = &newSchedulerObjectWst;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), wstSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), wstSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), wstSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), wstSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), wstSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), wstSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), wstSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), wstSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), wstSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), wstGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), wstSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), wstSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), wstSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), wstSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), wstSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), wstSchedulerObjectOcrPolicyMsgUnMarshallMsg);

#ifdef ENABLE_RESILIENCY
    schedObjFact->fcts.reset = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), wstSchedulerObjectReset);
#endif
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_WST */
