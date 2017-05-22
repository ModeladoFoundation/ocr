/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_DBSPACE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/dbspace/dbspace-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

/******************************************************/
/* OCR-DBSPACE SCHEDULER_OBJECT FUNCTIONS             */
/******************************************************/

static void dbspaceSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrGuid_t dbGuid) {
    self->loc = PD->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectDbspace_t* dbspaceSchedObj = (ocrSchedulerObjectDbspace_t*)self;
#ifdef ENABLE_SCHEDULER_OBJECT_LIST
    paramListSchedulerObjectList_t paramList;
    paramList.base.config = 0;
    paramList.base.guidRequired = 0;
    paramList.type = OCR_LIST_TYPE_SINGLE;
    paramList.elSize = 0;
    paramList.arrayChunkSize = 8;
    ocrSchedulerObjectFactory_t *listFactory = PD->schedulerObjectFactories[schedulerObjectList_id];
    dbspaceSchedObj->dbTimeList = listFactory->fcts.create(listFactory, (ocrParamList_t*)(&paramList));
    dbspaceSchedObj->schedList = listFactory->fcts.create(listFactory, (ocrParamList_t*)(&paramList));
    dbspaceSchedObj->listIterator = listFactory->fcts.createIterator(listFactory, dbspaceSchedObj->dbTimeList, 0);
#else
    ASSERT(0);
#endif
    dbspaceSchedObj->dbGuid = dbGuid;
}

static void dbspaceSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_DBSPACE;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectDbspace_t* dbspaceSchedObj = (ocrSchedulerObjectDbspace_t*)self;
    dbspaceSchedObj->dbGuid = NULL_GUID;
    dbspaceSchedObj->dbSize = 0;
    dbspaceSchedObj->dbPtr = NULL;
    dbspaceSchedObj->state = DB_STATE_PROXY;
    dbspaceSchedObj->time = 0;
    dbspaceSchedObj->lock = INIT_LOCK;
    dbspaceSchedObj->activeCount = 0;
    dbspaceSchedObj->mode = DB_ACQUIRE_NONE;
    dbspaceSchedObj->free = false;
    dbspaceSchedObj->dbTimeList = NULL;
    dbspaceSchedObj->schedList = NULL;
    dbspaceSchedObj->listIterator = NULL;
}

ocrSchedulerObject_t* newSchedulerObjectDbspace(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrSchedulerObject_t* schedObj = NULL;
    schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectDbspace_t), PERSISTENT_CHUNK);
    dbspaceSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* dbspaceSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrPolicyDomain_t *pd = factory->pd;
    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectDbspace_t));
    ADebug(AllocDebugAllPD, "dbspace-scheduler-object/dbspaceSchedulerObjectCreate() "
           "pdMalloc(%ld), addr=%p\n", sizeof(ocrSchedulerObjectDbspace_t), schedObj);
    dbspaceSchedulerObjectInitialize(factory, schedObj);
    paramListSchedulerObjectDbspace_t *paramsDbspace = (paramListSchedulerObjectDbspace_t*)perInstance;
    dbspaceSchedulerObjectStart(schedObj, pd, paramsDbspace->dbGuid);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 dbspaceSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = fact->pd;
        ocrSchedulerObjectDbspace_t* dbspaceSchedObj = (ocrSchedulerObjectDbspace_t*)self;
        ocrSchedulerObjectFactory_t *listFactory = pd->schedulerObjectFactories[dbspaceSchedObj->dbTimeList->fctId];
        listFactory->fcts.destroy(listFactory, dbspaceSchedObj->dbTimeList);
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 dbspaceSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ocrSchedulerObjectDbspace_t* dbspaceSchedObj = (ocrSchedulerObjectDbspace_t*)self;
    ASSERT(SCHEDULER_OBJECT_TYPE(element->kind) == OCR_SCHEDULER_OBJECT_DBTIME);
    ocrSchedulerObjectFactory_t *listFact = fact->pd->schedulerObjectFactories[dbspaceSchedObj->listIterator->fctId];
    listFact->fcts.insert(listFact, dbspaceSchedObj->dbTimeList, element, iterator, properties);
    return 0;
}

u8 dbspaceSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u64 dbspaceSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(properties == 0);
    ocrSchedulerObjectDbspace_t* dbspaceSchedObj = (ocrSchedulerObjectDbspace_t*)self;
    ocrSchedulerObjectFactory_t *listFactory = fact->pd->schedulerObjectFactories[dbspaceSchedObj->dbTimeList->fctId];
    return listFactory->fcts.count(listFactory, dbspaceSchedObj->dbTimeList, 0);
}

ocrSchedulerObjectIterator_t* dbspaceSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *factory, ocrSchedulerObject_t *self, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 dbspaceSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * factory, ocrSchedulerObjectIterator_t *iterator) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 dbspaceSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

ocrSchedulerObject_t* dbspaceGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 dbspaceSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* dbspaceSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 dbspaceSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 dbspaceSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                    phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 dbspaceSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_SCHED_TRANSACT);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_TRANSACT
    ocrSchedulerObject_t *schedObj = &(PD_MSG_FIELD_IO(schedArgs).schedObj);
#undef PD_MSG
#undef PD_TYPE

    ASSERT(schedObj->kind == OCR_SCHEDULER_OBJECT_DBSPACE);
    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)schedObj->guid.metaDataPtr;
    ASSERT(dbspaceObj && dbspaceObj->dbSize != 0);
    if ((dbspaceObj->dbPtr == NULL) || (schedObj->mapping == OCR_SCHEDULER_OBJECT_MAPPING_RELEASED)) {
        *marshalledSize = 0;
    } else {
        *marshalledSize = dbspaceObj->dbSize;
    }
    return 0;
}

// BUG #920 Cleanup: This is no longer required.
u8 dbspaceSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_SCHED_TRANSACT);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_TRANSACT
    ocrSchedulerObject_t *schedObj = &(PD_MSG_FIELD_IO(schedArgs).schedObj);
#undef PD_MSG
#undef PD_TYPE

    ASSERT(schedObj->kind == OCR_SCHEDULER_OBJECT_DBSPACE);
    ASSERT(schedObj->mapping == OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)schedObj->guid.metaDataPtr;

    //Dbspace critical section start
    hal_lock(&dbspaceObj->lock);

    ASSERT(dbspaceObj && dbspaceObj->dbSize != 0 && dbspaceObj->dbPtr != NULL);
    ASSERT(dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_UNMAPPED);

    hal_memCopy(buffer, dbspaceObj->dbPtr, dbspaceObj->dbSize, false); //TODO: this is overhead!

    //Also free up the DB pointer in this PD
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    pd->fcts.pdFree(pd, dbspaceObj->dbPtr);
    dbspaceObj->dbPtr = NULL;
    dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_MAPPED;

    //Dbspace critical section end
    hal_unlock(&dbspaceObj->lock);

    return 0;
}

// BUG #920 Cleanup: This is no longer required.
u8 dbspaceSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_SCHED_TRANSACT);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_TRANSACT
    ocrSchedulerObject_t *schedObj = &(PD_MSG_FIELD_IO(schedArgs).schedObj);
#undef PD_MSG
#undef PD_TYPE

    ASSERT(schedObj->kind == OCR_SCHEDULER_OBJECT_DBSPACE);
    ASSERT(!ocrGuidIsNull(schedObj->guid.guid));
    ASSERT(schedObj->guid.metaDataPtr != NULL);
    schedObj->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerObject_t *rootObj = pd->schedulers[0]->rootObj;
    ocrSchedulerObjectFactory_t *rootFact = pd->schedulerObjectFactories[rootObj->fctId];
    ocrSchedulerObject_t *dbMap = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_MAP, pd->myLocation, OCR_SCHEDULER_OBJECT_MAPPING_PINNED, 0);
    ASSERT(dbMap);
    ocrSchedulerObjectFactory_t *mapFact = pd->schedulerObjectFactories[dbMap->fctId];
    ocrSchedulerObjectIterator_t *mapIt = mapFact->fcts.createIterator(mapFact, dbMap, 0);
#if GUID_BIT_COUNT == 64
    mapIt->data = (void*)(schedObj->guid.guid.guid);
#elif GUID_BIT_COUNT == 128
    mapIt->data = (void*)(schedObj->guid.guid.lower);
#endif
    mapFact->fcts.iterate(mapFact, mapIt, SCHEDULER_OBJECT_ITERATE_SEARCH_KEY);
    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)mapIt->data;
    ASSERT(dbspaceObj);

    //Dbspace critical section start
    hal_lock(&dbspaceObj->lock);

    ASSERT(dbspaceObj->dbPtr == NULL);
    dbspaceObj->dbPtr = schedObj->guid.metaDataPtr;
    schedObj->guid.metaDataPtr = dbspaceObj;

    //Dbspace critical section end
    hal_unlock(&dbspaceObj->lock);

    mapFact->fcts.destroyIterator(mapFact, mapIt);
    return 0;
}

/******************************************************/
/* OCR-DBSPACE SCHEDULER_OBJECT FACTORY FUNCTIONS     */
/******************************************************/

void destructSchedulerObjectFactoryDbspace(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryDbspace(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t* schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryDbspace_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectDbspace_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_DBSPACE;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryDbspace;
    schedObjFact->instantiate = &newSchedulerObjectDbspace;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), dbspaceSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), dbspaceSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), dbspaceSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), dbspaceSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), dbspaceSchedulerObjectCount);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), dbspaceSchedulerObjectIterate);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), dbspaceSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), dbspaceSchedulerObjectDestroyIterator);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), dbspaceSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), dbspaceGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), dbspaceSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), dbspaceSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), dbspaceSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), dbspaceSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), dbspaceSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), dbspaceSchedulerObjectOcrPolicyMsgUnMarshallMsg);
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_DBSPACE */
