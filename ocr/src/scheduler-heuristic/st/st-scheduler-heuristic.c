/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * - A scheduler heuristic to distribute tasks and data across space and time
 *   on a distributed system. The placement decision is done by select nodes
 *   known as scheduling nodes. At this time, the heuristic supports only one
 *   scheduling node, configured to be node 0. So, all other nodes in the
 *   system communicate to this central scheduling node to manage the scheduling
 *   of their tasks and data. This mechanism is obviously not ideal but will
 *   serve as a baseline for future improvements.
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_ST

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-hal.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "scheduler-heuristic/st/st-scheduler-heuristic.h"
#include "scheduler-object/scheduler-object-all.h"
#include "task/hc/hc-task.h"
#include "extensions/ocr-hints.h"
#include "scheduler-object/list/list-scheduler-object.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

/* These are the state machine ops used by the ST scheduler
 * To visualize the state diagram look at the ppt file here:
 * - https://xstack.exascale-tech.com/wiki/index.php/File:ST_StateMachine.pptx
 */
typedef enum {
    SM_DB_CREATE,
    SM_DB_ACQUIRE,
    SM_DB_RELEASE,
    SM_DB_FREE,
    SM_DB_AT_SCHEDULER,
    SM_DB_TIME_SHIFT_AT_SCHEDULER,
    SM_DB_DONE_AT_SCHEDULER,
    SM_DB_MOVE_DST,
    SM_DB_MOVE_SRC,
    SM_DB_AT_SPACE,
    SM_EDT_AT_SCHEDULER,
    SM_EDT_AT_SPACE,
    SM_EDT_ACQUIRE,
    SM_EDT_RELEASE,
} DbStateMachineOp;

/******************************************************/
/* OCR-ST SCHEDULER_HEURISTIC                         */
/******************************************************/

ocrSchedulerHeuristic_t* newSchedulerHeuristicSt(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicSt_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    ocrSchedulerHeuristicSt_t *derived = (ocrSchedulerHeuristicSt_t*)self;
    derived->schedulerLocation = 0;
    derived->locationPlacement = 0;
    derived->locationLock = INIT_LOCK;
    return self;
}

static void initializeContextSt(ocrSchedulerHeuristicContext_t *context, u64 contextId) {
    context->id = contextId;
    context->actionSet = NULL;
    context->cost = NULL;
    context->properties = 0;

    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    stContext->stealSchedulerObjectIndex = ((u64)-1);
    stContext->mySchedulerObject = NULL;
    return;
}

u8 stSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
    {
        ASSERT(self->scheduler);
        self->contextCount = PD->workerCount; //Shared mem heuristic
        ASSERT(self->contextCount > 0);
        break;
    }
    case RL_MEMORY_OK:
    {
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            u32 i;
            self->contexts = (ocrSchedulerHeuristicContext_t **)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*));
            ocrSchedulerHeuristicContextSt_t *contextAlloc = (ocrSchedulerHeuristicContextSt_t *)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContextSt_t));
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                initializeContextSt(context, i);
                self->contexts[i] = context;
                context->id = i;
                context->location = PD->myLocation;
                context->actionSet = NULL;
                context->cost = NULL;
                context->properties = 0;
                ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
                stContext->stealSchedulerObjectIndex = ((u64)-1);
                stContext->mySchedulerObject = NULL;
                stContext->mapIterator = NULL;
                stContext->listIterator = NULL;
            }
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
            PD->fcts.pdFree(PD, self->contexts[0]);
            PD->fcts.pdFree(PD, self->contexts);
        }
        break;
    }
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
    {
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            u32 i;
            ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
            ocrSchedulerObjectFactory_t *rootFact = PD->schedulerObjectFactories[rootObj->fctId];
            ocrSchedulerObject_t *dbMap = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_MAP, PD->myLocation, OCR_SCHEDULER_OBJECT_MAPPING_PINNED, 0);
            ocrSchedulerObjectFactory_t *mapFact = PD->schedulerObjectFactories[dbMap->fctId];
            ocrSchedulerObjectFactory_t *listFact = PD->schedulerObjectFactories[schedulerObjectList_id];
            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)self->contexts[i];
                stContext->mySchedulerObject = rootFact->fcts.getSchedulerObjectForLocation(rootFact, rootObj, OCR_SCHEDULER_OBJECT_DEQUE, i, OCR_SCHEDULER_OBJECT_MAPPING_WORKER, 0);
                ASSERT(stContext->mySchedulerObject);
                stContext->stealSchedulerObjectIndex = (i + 1) % self->contextCount;
                stContext->mapIterator = mapFact->fcts.createIterator(mapFact, dbMap, 0);
                stContext->listIterator = listFact->fcts.createIterator(listFact, NULL, 0); //Creating a list iterator for late binding to an actual list.
            }
        }
        break;
    }
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void stSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 stSchedulerHeuristicUpdate(ocrSchedulerHeuristic_t *self, u32 properties) {
    return OCR_ENOTSUP;
}

ocrSchedulerHeuristicContext_t* stSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    ocrWorker_t * worker = NULL;
    //ocrPolicyDomain_t *pd;
    //getCurrentEnv(&pd, &worker, NULL, NULL);
    //ASSERT(loc == pd->myLocation);
    getCurrentEnv(NULL, &worker, NULL, NULL);
    if (worker == NULL) return NULL;
    return self->contexts[worker->id];
}

/******************************************************/
/* STATIC FUNCTIONS                                   */
/******************************************************/

static u8 processDbSMOP(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, DbStateMachineOp op,
                        ocrGuid_t dbGuid, u64 dbSize, void *dbPtr, ocrLocation_t space, u64 time, u64 count,
                        ocrTask_t *task, ocrSchedulerObjectDbspace_t **dbspacePtr, u32 properties);

//Finds the DB time object for a DB space object's time slot (does not create one if it doesn't exist)
ocrSchedulerObjectDbtime_t *getDbTime(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerObjectDbspace_t *dbspaceObj, u64 time) {
    ASSERT(time != 0);
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObjectIterator_t *listIt = stContext->listIterator;
    ocrSchedulerObjectFactory_t *listFact = pd->schedulerObjectFactories[listIt->fctId];

    //Use a list iterator to search for an existing time object
    listIt->schedObj = dbspaceObj->dbTimeList;
    listIt->data = NULL;
    listFact->fcts.iterate(listFact, listIt, SCHEDULER_OBJECT_ITERATE_HEAD);

    ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)listIt->data;
    while (dbtimeObj && dbtimeObj->time < time) {
        listFact->fcts.iterate(listFact, listIt, SCHEDULER_OBJECT_ITERATE_NEXT);
        dbtimeObj = (ocrSchedulerObjectDbtime_t*)listIt->data;
    }

    if (dbtimeObj && dbtimeObj->time != time)
        dbtimeObj = NULL;

    listIt->schedObj = NULL;
    listIt->data = NULL;
    return dbtimeObj;
}

//Gets the DB time object for a DB space object's time slot by creating one if it doesn't exist
//Note: dbspaceObj is assumed to have been locked before entering this function
ocrSchedulerObjectDbtime_t *createDbTime(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context,
                                         ocrSchedulerObjectDbspace_t *dbspaceObj, ocrLocation_t space, u64 time)
{
    ASSERT(time != 0);
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObjectIterator_t *listIt = stContext->listIterator;
    ocrSchedulerObjectFactory_t *listFact = pd->schedulerObjectFactories[listIt->fctId];

    //Use a list iterator to search for an existing time object
    listIt->schedObj = dbspaceObj->dbTimeList;
    listIt->data = NULL;
    listFact->fcts.iterate(listFact, listIt, SCHEDULER_OBJECT_ITERATE_HEAD);

    ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)listIt->data;
    while (dbtimeObj && dbtimeObj->time < time) {
        listFact->fcts.iterate(listFact, listIt, SCHEDULER_OBJECT_ITERATE_NEXT);
        dbtimeObj = (ocrSchedulerObjectDbtime_t*)listIt->data;
    }

    ocrSchedulerObjectIterator_t *it = NULL;
    u32 properties = 0;
    bool doCreate = false;

    if (dbtimeObj == NULL) {
        doCreate = true;
        properties = SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL;
    } else if (dbtimeObj->time > time) {
        doCreate = true;
        it = listIt;
        properties = SCHEDULER_OBJECT_INSERT_BEFORE | SCHEDULER_OBJECT_INSERT_POSITION_ITERATOR;
    }
    if (doCreate) {
        paramListSchedulerObjectDbtime_t dbtimeParams;
        dbtimeParams.base.config = 0;
        dbtimeParams.base.guidRequired = 0;
        dbtimeParams.space = space;
        dbtimeParams.time = time;
        ocrSchedulerObjectFactory_t *dbtimeFact = pd->schedulerObjectFactories[schedulerObjectDbtime_id]; //TODO: Remove direct references to factory IDs
        dbtimeObj = (ocrSchedulerObjectDbtime_t*)dbtimeFact->fcts.create(dbtimeFact, (ocrParamList_t*)&dbtimeParams);
        ASSERT(dbtimeObj);
        listFact->fcts.insert(listFact, dbspaceObj->dbTimeList, (ocrSchedulerObject_t*)dbtimeObj, it, properties);
    }
    ASSERT(dbtimeObj->time == time && dbtimeObj->space == space);

    listFact->fcts.iterate(listFact, listIt, SCHEDULER_OBJECT_ITERATE_HEAD);
    ocrSchedulerObjectDbtime_t *dbtimeObjHead = (ocrSchedulerObjectDbtime_t*)listIt->data;
    if (dbtimeObjHead == dbtimeObj) {
        dbspaceObj->time = time;
    } else {
        ASSERT((dbspaceObj->time == dbtimeObjHead->time) && (dbspaceObj->time < time));
    }

    listIt->schedObj = NULL;
    listIt->data = NULL;
    return dbtimeObj;
}

ocrSchedulerObjectDbspace_t *createDbSpace(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context,
                                           ocrGuid_t dbGuid, u64 dbSize, void *dbPtr, ocrLocation_t space, u64 time,
                                           ocrDbState state, ocrSchedulerObjectDbspace_t *dbspaceObj, u64 count)
{
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerHeuristicSt_t *derived = (ocrSchedulerHeuristicSt_t*)self;
    ASSERT((space == pd->myLocation) || (pd->myLocation == derived->schedulerLocation));
    bool doCreateTime = (time != 0);

    if (dbspaceObj == NULL) {
        ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
        ocrSchedulerObjectFactory_t *mapFact = pd->schedulerObjectFactories[stContext->mapIterator->fctId];
        ocrSchedulerObjectIterator_t *mapIt = stContext->mapIterator;
        ocrSchedulerObjectPdspace_t *pdspaceObj = (ocrSchedulerObjectPdspace_t*)self->scheduler->rootObj;

        //PD critical section to handle concurrent DB space object creates
        hal_lock(&pdspaceObj->lock);
#if GUID_BIT_COUNT == 64
        mapIt->data = (void*) dbGuid.guid;
#elif GUID_BIT_COUNT == 128
        mapIt->data = (void*) dbGuid.lower;
#endif
        mapFact->fcts.iterate(mapFact, mapIt, SCHEDULER_OBJECT_ITERATE_SEARCH_KEY);
        dbspaceObj = (ocrSchedulerObjectDbspace_t*)mapIt->data;
        if (dbspaceObj == NULL) {
            paramListSchedulerObjectDbspace_t dbspaceParams;
            dbspaceParams.base.config = 0;
            dbspaceParams.base.guidRequired = 0;
            dbspaceParams.dbGuid = dbGuid;
            ocrSchedulerObjectFactory_t *dbspaceFact = pd->schedulerObjectFactories[schedulerObjectDbspace_id]; //TODO: Remove direct references to factory IDs
            dbspaceObj = (ocrSchedulerObjectDbspace_t*)dbspaceFact->fcts.create(dbspaceFact, (ocrParamList_t*)&dbspaceParams);
            ASSERT(dbspaceObj);
            DPRINTF(DEBUG_LVL_VVERB, "DbSpace Map: Key: "GUIDF" Value: %p\n", GUIDA(dbGuid), dbspaceObj);
            dbspaceObj->dbSize = dbSize;
            dbspaceObj->dbPtr = dbPtr;
            dbspaceObj->time = time;
            dbspaceObj->state = state;
            if (doCreateTime) createDbTime(self, context, dbspaceObj, space, time);
            doCreateTime = false;
            switch(state) {
            case DB_STATE_PROXY:
                dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
                break;
            case DB_STATE_INFO:
                {
                    ASSERT(time != 0);
                    dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_MAPPED;
                    if (count != 0) {
                        ASSERT(pd->myLocation == derived->schedulerLocation && count == 1);
                        ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
                        dbtimeObj->schedulerCount = count;
                    }
                }
                break;
            case DB_STATE_LOCAL_ACTIVE:
                {
                    ASSERT(time != 0 && count == 1);
                    dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
                    dbspaceObj->activeCount = count;
                    dbspaceObj->mode = DB_ACQUIRE_SHARED;
                    ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
                    dbtimeObj->edtScheduledCount = count;
                    if (pd->myLocation == derived->schedulerLocation) {
                        dbtimeObj->schedulerCount = count;
                    }
                    DPRINTF(DEBUG_LVL_VVERB, "ST-SCHEDULER: createDbSpace: (db: "GUIDF" state: %"PRIu32" time: %"PRIu64" active: %"PRIu64" [%p] edtScheduled: %"PRIu32" edtDone: %"PRIu32")\n",
                                                                 GUIDA(dbGuid), dbspaceObj->state, dbtimeObj->time, dbspaceObj->activeCount,
                                                                 dbtimeObj, dbtimeObj->edtScheduledCount, dbtimeObj->edtDoneCount);
                }
                break;
            default:
                DPRINTF(DEBUG_LVL_INFO, "INVALID DB state %"PRId32"\n", (u32)(state));
                ASSERT(0);
                return NULL;
            }
            time = 0;
            mapFact->fcts.insert(mapFact, pdspaceObj->dbMap, (ocrSchedulerObject_t*)dbspaceObj,
                                 mapIt, (SCHEDULER_OBJECT_INSERT_INPLACE | SCHEDULER_OBJECT_INSERT_POSITION_ITERATOR));
        }

        //PD critical section end
        hal_unlock(&pdspaceObj->lock);
    }

    //Dbspace critical section start
    hal_lock(&dbspaceObj->lock);

    if (doCreateTime) createDbTime(self, context, dbspaceObj, space, time);
    if (state == DB_STATE_INFO) {
        ASSERT(dbSize != 0 && dbPtr == NULL);
        if (dbspaceObj->state == DB_STATE_PROXY) {
            ASSERT(dbspaceObj->dbSize == 0 && dbspaceObj->dbPtr == NULL);
            dbspaceObj->dbSize = dbSize;
            dbspaceObj->state = DB_STATE_INFO;
            dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_MAPPED;
            if (count != 0) {
                ASSERT(pd->myLocation == derived->schedulerLocation && count == 1);
                ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
                ASSERT(dbtimeObj->time == time && dbtimeObj->schedulerCount == 0);
                dbtimeObj->schedulerCount = count;
            }
        } else {
            ASSERT(dbspaceObj->dbSize == dbSize);
        }
    }

    //Dbspace critical section end
    hal_unlock(&dbspaceObj->lock);

    return dbspaceObj;
}

static void destructDbSpace(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerObjectDbspace_t *dbspaceObj) {
    ASSERT(dbspaceObj->state == DB_STATE_LOCAL_INACTIVE);
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    //Remove the DB space object from the map
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObjectFactory_t *mapFact = pd->schedulerObjectFactories[stContext->mapIterator->fctId];
    ocrSchedulerObjectIterator_t *mapIt = stContext->mapIterator;
#if GUID_BIT_COUNT == 64
        mapIt->data = (void*) dbspaceObj->dbGuid.guid;
#elif GUID_BIT_COUNT == 128
        mapIt->data = (void*) dbspaceObj->dbGuid.lower;
#endif
    mapFact->fcts.iterate(mapFact, mapIt, SCHEDULER_OBJECT_ITERATE_SEARCH_KEY);
    ASSERT(dbspaceObj == (ocrSchedulerObjectDbspace_t*)mapIt->data);
    mapFact->fcts.remove(mapFact, NULL, OCR_SCHEDULER_OBJECT_VOIDPTR, 1, NULL, mapIt, SCHEDULER_OBJECT_REMOVE_ITERATOR);
    ASSERT(dbspaceObj == (ocrSchedulerObjectDbspace_t*)mapIt->data);

    //Destroy the DB space object
    ocrSchedulerObjectFactory_t *dbspaceFact = pd->schedulerObjectFactories[dbspaceObj->base.fctId];
    dbspaceFact->fcts.destroy(dbspaceFact, (ocrSchedulerObject_t*)dbspaceObj);
}

//Now that all the depv DBs are local, try to acquire the DBs so that the EDT can be scheduled.
//If EDT cannot acquire the DB due to a mode conflict, stash the EDT into the readyList to try later again.
static void acquireEdtDeps(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrTask_t *task) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    //Get the EDT's dependence vector
    ASSERT(task);
#if VERIFY_TASK_TIME
    ocrHintVal_t hintVal;
    ocrHint_t edtHint;
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    RESULT_ASSERT((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx])->fcts.getHint(task, &edtHint), ==, 0);
    RESULT_ASSERT(ocrHintGetValue(&edtHint, OCR_HINT_EDT_TIME, &hintVal), ==, 0);
#endif

    RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[task->fctId]))->fcts.dependenceResolved(task, NULL_GUID, NULL, EDT_SLOT_NONE), ==, 0);
}

//The EDT's depv is scanned to see if all the DBs are local and current
//(i.e the DB's current time slot matches EDT's). If a depv DB has not
//yet moved to the current space and time, the EDT will be inserted into
//that DB's waitList. Once, the DB arrives then the EDT will
//proceed to check the next DB.
static void scheduleEdtDeps(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrTask_t *task, u32 startIdx) {
    u32 i, j;
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    //Get the schedule time of this EDT
    ocrHint_t edtHint;
    ocrHintVal_t hintVal = {};
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.getHint(task, &edtHint), ==, 0);
    RESULT_ASSERT(ocrHintGetValue(&edtHint, OCR_HINT_EDT_TIME, &hintVal), ==, 0);
    u64 time = hintVal.u64Val;

    //Get the EDT's dependence vector
    ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //BUG #926:This is temporary until we get proper introspection support
    u32 depc = task->depc;
    ocrEdtDep_t *depv = hcTask->resolvedDeps;
    for (i = 0; i < depc; i++) ASSERT(depv[i].ptr == NULL);

    bool asad = true; // :-) We start off assuming all DBs are local and current (all-singing-all-dancing)

    for (i = startIdx; i < depc && asad; i++) {
        if (!ocrGuidIsNull(depv[i].guid) && depv[i].mode != DB_MODE_NULL) {
            bool uniq = true;
            for (j = 0; j < i; j++) {
                if (ocrGuidIsEq(depv[j].guid, depv[i].guid)) {
                    uniq = false;
                    break;
                }
            }
            if (uniq) {
                if (processDbSMOP(self, context, SM_EDT_AT_SPACE, depv[i].guid, 0, NULL, pd->myLocation, time, 1, task, NULL, 0) != 0) {
                    asad = false; // :-(
                }
            }
        }
    }

    //If all DBs are found to be local, then start the acquiring process
    if (asad) acquireEdtDeps(self, context, task);
}

static void processDbWaitlist(ocrPolicyDomain_t *pd, ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context,
                              ocrSchedulerObjectDbspace_t *dbspaceObj, ocrSchedulerObjectDbtime_t *dbtimeObj, u64 waiterCount)
{
    u32 i;
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObjectFactory_t *listFact = pd->schedulerObjectFactories[stContext->listIterator->fctId];
    while (waiterCount > 0) {
        ocrTask_t *task = (ocrTask_t*)ocrSchedulerObjectListHead(dbtimeObj->waitList);
        ASSERT(task);
        listFact->fcts.remove(listFact, dbtimeObj->waitList, OCR_SCHEDULER_OBJECT_EDT, 1, NULL, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
        //Get the EDT's dependence vector
        ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //BUG #926:This is temporary until we get proper introspection support
        u32 depc = task->depc;
        ocrEdtDep_t *depv = hcTask->resolvedDeps;
        for (i = 0; i < depc; i++) {
            if (ocrGuidIsEq(depv[i].guid, dbspaceObj->dbGuid)) break;
        }
        ASSERT(i < depc);
        scheduleEdtDeps(self, context, task, i+1);
        waiterCount--;
    }
    RESULT_ASSERT(listFact->fcts.count(listFact, dbtimeObj->waitList, 0), ==, 0);
}

//Send a message to DB's new dest location to start DB movement from src location
static u8 initiateDbMove(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *pd, ocrGuid_t dbGuid, u32 dbSize,
                           ocrLocation_t srcLocation, ocrLocation_t dstLocation, u64 srcTime, u64 dstTime)
{
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
    msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
    msg.destLocation = dstLocation;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).guid = dbGuid;
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_UPDATE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_SPACETIME_DB;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.dbSize = dbSize;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.srcLoc = srcLocation;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.srcTime = srcTime;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.dstTime = dstTime;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

//DB Move operation: src location sends the DB to dest location
static u8 dbMoveSend(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *pd, ocrGuid_t dbGuid, ocrLocation_t space, ocrSchedulerObjectDbspace_t *dbspaceObj, bool doRelease)
{
    //If we are using PD-level acquire/release,
    //then release the DB from the PD here.
    //TODO: This will usually write-back the DB to the
    //home location which is an unnecessary overhead.
    //We have to fix this!
    if (doRelease) {
        PD_MSG_STACK(releaseMsg);
        getCurrentEnv(NULL, NULL, NULL, &releaseMsg);
#define PD_MSG (&releaseMsg)
#define PD_TYPE PD_MSG_DB_RELEASE
        releaseMsg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = dbGuid;
        PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(edt.guid) = NULL_GUID;
        PD_MSG_FIELD_I(edt.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(ptr) = NULL;
        PD_MSG_FIELD_I(size) = 0;
        PD_MSG_FIELD_I(properties) = DB_PROP_RT_PD_ACQUIRE;
        RESULT_ASSERT(pd->fcts.processMessage(pd, &releaseMsg, true), ==, 0); //Blocking call to release
#undef PD_MSG
#undef PD_TYPE
    }

    //Send a transact message to the destination.
    //If DB acquire/release is used, then we send a no-data dbspace object.
    //Else, we marshall the Dbspace object and transfer to the destination.
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_TRANSACT
    msg.type = PD_MSG_SCHED_TRANSACT | PD_MSG_REQUEST;
    msg.destLocation = space;
    PD_MSG_FIELD_IO(size) = 0;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_TRANSACT_PROP_TRANSFER;
    PD_MSG_FIELD_IO(schedArgs).schedObj.guid.guid = dbGuid;
    PD_MSG_FIELD_IO(schedArgs).schedObj.guid.metaDataPtr = dbspaceObj;
    PD_MSG_FIELD_IO(schedArgs).schedObj.kind = OCR_SCHEDULER_OBJECT_DBSPACE;
    PD_MSG_FIELD_IO(schedArgs).schedObj.fctId = dbspaceObj->base.fctId;
    PD_MSG_FIELD_IO(schedArgs).schedObj.loc = space;
    // The transact should be aware of the current scheduler object mapping
    // state to estimate the marshall size. If the object has already been
    // released, then the transact should not marshall anything more.
    PD_MSG_FIELD_IO(schedArgs).schedObj.mapping = OCR_SCHEDULER_OBJECT_MAPPING_RELEASED;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 dbTimeDone(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *pd, ocrGuid_t dbGuid, u64 dbSize, u64 time, u64 edtDoneCount, bool doFree)
{
    ocrSchedulerHeuristicSt_t *derived = (ocrSchedulerHeuristicSt_t*)self;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
    msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
    msg.destLocation = derived->schedulerLocation;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).guid = dbGuid;
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_DONE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_SPACETIME_DB;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.dbSize = dbSize;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.time = time;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.edtDoneCount = edtDoneCount;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.free = doFree;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 processDbSMOP(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, DbStateMachineOp op,
                        ocrGuid_t dbGuid, u64 dbSize, void *dbPtr, ocrLocation_t space, u64 time, u64 count,
                        ocrTask_t *task, ocrSchedulerObjectDbspace_t **dbspacePtr, u32 properties)
{
    u8 retVal = 0;
    bool localDestruct = false;
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerHeuristicSt_t *derived __attribute__((unused)) = (ocrSchedulerHeuristicSt_t*)self;
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObjectFactory_t *listFact = pd->schedulerObjectFactories[stContext->listIterator->fctId];

    ocrSchedulerObjectDbspace_t *dbspaceObj = NULL;
    if (!ocrGuidIsNull(dbGuid)) {
        //Search for an existing Dbspace object with DB guid
        ocrSchedulerObjectFactory_t *mapFact = pd->schedulerObjectFactories[stContext->mapIterator->fctId];
        ocrSchedulerObjectIterator_t *mapIt = stContext->mapIterator;
#if GUID_BIT_COUNT == 64
        mapIt->data = (void*) dbGuid.guid;
#elif GUID_BIT_COUNT == 128
        mapIt->data = (void*) dbGuid.lower;
#endif
        mapFact->fcts.iterate(mapFact, mapIt, SCHEDULER_OBJECT_ITERATE_SEARCH_KEY);
        dbspaceObj = (ocrSchedulerObjectDbspace_t*)mapIt->data;
    } else {
        ASSERT(dbspacePtr && *dbspacePtr);
        dbspaceObj = *dbspacePtr;
    }

    switch(op) {
    case SM_DB_CREATE: //SM is notified that a DB has been created
        {
            ASSERT(dbSize != 0 && space == pd->myLocation && time != 0);
            ASSERT(dbspaceObj == NULL);
            ocrDbState state = dbPtr ? DB_STATE_LOCAL_ACTIVE : DB_STATE_INFO;
            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_CREATE: (db: "GUIDF" size: %"PRIu64" ptr: %p space: %"PRIu64" time: %"PRIu64" state: %"PRIu32")\n", GUIDA(dbGuid), dbSize, dbPtr, space, time, state);
            dbspaceObj = createDbSpace(self, context, dbGuid, dbSize, dbPtr, space, time, state, NULL, count);
            ASSERT(dbspaceObj);
        }
        break;
    case SM_DB_ACQUIRE: //SM is notified that the DB has been acquired by an EDT in this PD
        {
            ASSERT(dbSize == 0 && dbPtr == NULL && space == pd->myLocation && time == 0);
            ASSERT(dbspaceObj);

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_ACQUIRE: (db: "GUIDF" state: %"PRIu32")\n", GUIDA(dbGuid), dbspaceObj->state);
            ocrSchedulerObjectDbtime_t *dbtimeObj __attribute__((unused)) = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ASSERT(dbspaceObj->time == dbtimeObj->time);

            dbspaceObj->activeCount += 1;

            if (dbspaceObj->state == DB_STATE_LOCAL_INACTIVE)
                dbspaceObj->state = DB_STATE_LOCAL_ACTIVE;
            ASSERT(dbspaceObj->state == DB_STATE_LOCAL_ACTIVE);
            DPRINTF(DEBUG_LVL_VVERB, "ST-SCHEDULER: SM_DB_ACQUIRE: (db: "GUIDF" state: %"PRIu32" time: %"PRIu64" active: %"PRIu64" [%p] edtScheduled: %"PRIu32" edtDone: %"PRIu32")\n",
                                                                 GUIDA(dbGuid), dbspaceObj->state, dbtimeObj->time, dbspaceObj->activeCount,
                                                                 dbtimeObj, dbtimeObj->edtScheduledCount, dbtimeObj->edtDoneCount);

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);
        }
        break;
    case SM_DB_RELEASE: //SM is notified that the DB has been released by an EDT in this PD
        {
            ASSERT(dbSize == 0 && dbPtr == NULL && space == pd->myLocation && time == 0);
            ASSERT(dbspaceObj);

            bool isTimeDone = false;
            u64 edtDoneCountLocal = 0;

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_RELEASE: (db: "GUIDF" state: %"PRIu32")\n", GUIDA(dbGuid), dbspaceObj->state);
            ASSERT(dbspaceObj->state == DB_STATE_LOCAL_ACTIVE && dbspaceObj->activeCount > 0);
            ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ASSERT(dbspaceObj->time == dbtimeObj->time);
            time = dbspaceObj->time;
            dbSize = dbspaceObj->dbSize;

            dbspaceObj->activeCount -= 1;
            dbtimeObj->edtDoneCount += 1;

            if (dbtimeObj->edtScheduledCount == dbtimeObj->edtDoneCount) {
                ASSERT(dbspaceObj->activeCount == 0);
                dbspaceObj->state = DB_STATE_LOCAL_INACTIVE;
                edtDoneCountLocal = dbtimeObj->edtDoneCount;
                localDestruct = dbspaceObj->free;
                isTimeDone = true;
            }
            DPRINTF(DEBUG_LVL_VVERB, "ST-SCHEDULER: SM_DB_RELEASE: (db: "GUIDF" state: %"PRIu32" active: %"PRIu64" [%p] edtScheduled: %"PRIu32" edtDone: %"PRIu32" done: %"PRIu32" free: %"PRIu32")\n",
                                                                    GUIDA(dbGuid), dbspaceObj->state, dbspaceObj->activeCount,
                                                                    dbtimeObj, dbtimeObj->edtScheduledCount, dbtimeObj->edtDoneCount,
                                                                    (u8)isTimeDone, (u8)localDestruct);

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);

            if (isTimeDone) {
                RESULT_ASSERT(dbTimeDone(self, pd, dbGuid, dbSize, time, edtDoneCountLocal, localDestruct), ==, 0);
            }
        }
        break;
    case SM_DB_FREE: //SM is notified that this DB will be freed
        {
            ASSERT(dbSize == 0 && dbPtr == NULL && space == pd->myLocation && time == 0);
            if(dbspaceObj == NULL) {
                //PD does not know this DB. So just send out a done message to the scheduler node.
                ASSERT(pd->myLocation != derived->schedulerLocation);
                RESULT_ASSERT(dbTimeDone(self, pd, dbGuid, 0, 0, 0, true), ==, 0);
            } else {
                bool isTimeDone = false;
                u64 edtDoneCountLocal = 0;

                //Dbspace critical section start
                hal_lock(&dbspaceObj->lock);

                DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_FREE: (db: "GUIDF" state: %"PRIu32")\n", GUIDA(dbGuid), dbspaceObj->state);
                ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
                ASSERT(dbspaceObj->time == dbtimeObj->time);
                dbspaceObj->free = true;
                dbSize = dbspaceObj->dbSize;

                if ((dbtimeObj->edtScheduledCount != dbtimeObj->edtDoneCount) && ((properties & DB_PROP_NO_RELEASE) == 0)) {
                    ASSERT(dbtimeObj->edtScheduledCount > dbtimeObj->edtDoneCount && dbspaceObj->activeCount > 0);
                    ASSERT(dbspaceObj->state == DB_STATE_LOCAL_ACTIVE);
                    dbspaceObj->activeCount -= 1;
                    dbtimeObj->edtDoneCount += 1;
                }

                if (dbtimeObj->edtScheduledCount == dbtimeObj->edtDoneCount) {
                    ASSERT(dbspaceObj->activeCount == 0);
                    dbspaceObj->state = DB_STATE_LOCAL_INACTIVE;
                    time = dbtimeObj->time;
                    edtDoneCountLocal = dbtimeObj->edtDoneCount;
                    isTimeDone = true;
                    localDestruct = true;
                }
                DPRINTF(DEBUG_LVL_VVERB, "ST-SCHEDULER: SM_DB_FREE: (db: "GUIDF" state: %"PRIu32" time: %"PRIu64" active: %"PRIu64" [%p] edtScheduled: %"PRIu32" edtDone: %"PRIu32" done: %"PRIu32" free: %"PRIu32")\n",
                                                                     GUIDA(dbGuid), dbspaceObj->state, dbtimeObj->time, dbspaceObj->activeCount,
                                                                     dbtimeObj, dbtimeObj->edtScheduledCount, dbtimeObj->edtDoneCount,
                                                                     (u8)isTimeDone, (u8)localDestruct);

                //Dbspace critical section end
                hal_unlock(&dbspaceObj->lock);

                if (isTimeDone) {
                    RESULT_ASSERT(dbTimeDone(self, pd, dbGuid, dbSize, time, edtDoneCountLocal, true), ==, 0);
                }
            }
        }
        break;
    case SM_DB_AT_SCHEDULER: //SM at scheduler node is notified of a DB created at another PD
        {
            ASSERT(dbSize != 0 && dbPtr == NULL && space != pd->myLocation && time != 0);
            dbspaceObj = createDbSpace(self, context, dbGuid, dbSize, NULL, space, time, DB_STATE_INFO, dbspaceObj, count);
            ASSERT(dbspaceObj);
            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_AT_SCHEDULER: (db: "GUIDF" size: %"PRIu64" time: %"PRIu64" state: %"PRIu32")\n", GUIDA(dbGuid), dbSize, time, dbspaceObj->state);
            if (dbspaceObj->free)
                retVal = 1;
        }
        break;
    case SM_DB_TIME_SHIFT_AT_SCHEDULER: //process
        {
            bool update = false;
            ASSERT(dbSize == 0 && dbPtr == NULL && space == pd->myLocation && time == 0 && task == NULL);
            ASSERT(dbspaceObj);

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_TIME_SHIFT_AT_SCHEDULER: (db: "GUIDF" state: %"PRIu32")\n", GUIDA(dbGuid), dbspaceObj->state);
            ASSERT(dbspaceObj->state != DB_STATE_PROXY);
            ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ocrSchedulerObjectDbtime_t *dbtimeObjNext = NULL;

            if (dbtimeObj && dbtimeObj->schedulerDone) {
                ASSERT((dbtimeObj->time == dbspaceObj->time) && (dbtimeObj->schedulerCount == dbtimeObj->edtDoneCount));
                dbtimeObjNext = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHeadNext(dbspaceObj->dbTimeList);
                ASSERT(dbtimeObjNext && (dbtimeObjNext->time > dbtimeObj->time) && (dbtimeObjNext->schedulerCount > 0) && (dbtimeObjNext->edtDoneCount == 0));
                if (dbtimeObj->space == pd->myLocation) {
                    //If current location is scheduler node, don't remove the head here.
                    //It will be removed as part of DB move transaction.
                    ASSERT(dbspaceObj->state == DB_STATE_LOCAL_INACTIVE);
                    ASSERT(dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_PINNED);
                } else {
                    listFact->fcts.remove(listFact, dbspaceObj->dbTimeList, OCR_SCHEDULER_OBJECT_DBTIME, 1, NULL, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                    dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_MAPPED;
                }
                update = true;
            }

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);

            if (update) {
                RESULT_ASSERT(initiateDbMove(self, pd, dbGuid, dbspaceObj->dbSize, dbtimeObj->space, dbtimeObjNext->space, dbtimeObj->time, dbtimeObjNext->time), ==, 0);
            }
        }
        break;
    case SM_DB_DONE_AT_SCHEDULER: //SM at scheduler node is notified that DB's current time slot is done
        {
            ASSERT(dbSize != 0 && dbPtr == NULL && count != 0 && task == NULL);
            dbspaceObj = createDbSpace(self, context, dbGuid, dbSize, NULL, space, time, DB_STATE_INFO, dbspaceObj, count);
            ASSERT(dbspaceObj);
            u64 edtDoneCount = count;

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_DONE_AT_SCHEDULER: (db: "GUIDF" space: %"PRIu64" time: %"PRIu64" count: %"PRIu64" free: %"PRIu32" state: %"PRIu32")\n",
                                                                             GUIDA(dbGuid), space, time, edtDoneCount, properties, dbspaceObj->state);
            ASSERT(dbspaceObj->state != DB_STATE_PROXY);
            if (time == 0) {
                ASSERT(edtDoneCount == 0 && properties != 0);
            } else {
                ASSERT(dbspaceObj->time == time);
            }
            ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ASSERT(dbtimeObj && dbtimeObj->space == space && dbtimeObj->time == dbspaceObj->time && dbtimeObj->schedulerCount >= edtDoneCount);

            if (properties != 0) {
                ASSERT(dbspaceObj->free == false);
                dbspaceObj->free = true;
            }

            if (dbspaceObj->free)
                retVal = 1;

            if (space != pd->myLocation) {
                ASSERT(dbspaceObj->state == DB_STATE_INFO && dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
                ASSERT(dbtimeObj->edtDoneCount < edtDoneCount);
                dbtimeObj->edtDoneCount = edtDoneCount;
            }
            ASSERT(dbtimeObj->schedulerCount >= edtDoneCount);

            if (dbtimeObj->schedulerCount == edtDoneCount) {
                ocrSchedulerObjectDbtime_t *dbtimeObjNext = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHeadNext(dbspaceObj->dbTimeList);
                if (dbtimeObjNext) {
                    ASSERT(dbtimeObjNext->schedulerCount > 0 && dbspaceObj->free == false);
                    dbtimeObj->schedulerDone = true; //time-shift possible
                } else {
                    retVal = 1;
                    if (dbspaceObj->free && space != pd->myLocation)
                        localDestruct = true;
                }
            } else {
                retVal = 1;
            }
            DPRINTF(DEBUG_LVL_VVERB, "ST-SCHEDULER: SM_DB_DONE_AT_SCHEDULER: (db: "GUIDF" space: %"PRIu64" time: %"PRIu64" schedCount: %"PRIu32" free: %"PRIu32" state: %"PRIu32")\n",
                                                                             GUIDA(dbGuid), space, time, dbtimeObj->schedulerCount, (u8)localDestruct,
                                                                             dbspaceObj->state);

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);
        }
        break;
    case SM_DB_MOVE_DST: //SM is notified that DB is now scheduled at this PD
        {
            ASSERT(dbSize != 0 && dbPtr == NULL && space == pd->myLocation && time != 0 && count != 0);
            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_MOVE_DST: (db: "GUIDF" time: %"PRIu64")\n", GUIDA(dbGuid), time);
            u64 srcTime  __attribute__((unused)) = (u64)count; //Use count to pass srcTime //TODO: clean this up
            ocrLocation_t srcLoc __attribute__((unused)) = (ocrLocation_t)task; //Use task to pass srcLoc //TODO: clean this up
            dbPtr = NULL;
            task = NULL;
            dbspaceObj = createDbSpace(self, context, dbGuid, dbSize, NULL, space, time, DB_STATE_INFO, dbspaceObj, 0); //Create if absent
            ASSERT(dbspaceObj);

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            ocrSchedulerObjectDbtime_t *dbtimeObj = getDbTime(self, context, dbspaceObj, time);
            ASSERT(dbtimeObj && dbtimeObj->time == time && dbtimeObj->space == pd->myLocation);
            ocrSchedulerObjectDbtime_t *dbtimeObjHead __attribute__((unused)) = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            u64 waiterCount = 0;
            switch(dbspaceObj->state) {
            case DB_STATE_INFO:
                {   //DB mov src != dest
                    ASSERT(dbtimeObjHead == dbtimeObj && dbspaceObj->time == time);
                    ASSERT(dbspaceObj->dbPtr == NULL && dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
                    if (dbtimeObj->edtScheduledCount > 0) dbspaceObj->state = DB_STATE_REMOTE_INACTIVE;
                }
                break;
            case DB_STATE_LOCAL_INACTIVE:
                {   //DB mov src == dest
                    ASSERT(srcLoc == pd->myLocation && dbtimeObjHead->time == srcTime && dbspaceObj->time == srcTime);
                    ASSERT(dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_PINNED);
                    listFact->fcts.remove(listFact, dbspaceObj->dbTimeList, OCR_SCHEDULER_OBJECT_DBTIME, 1, NULL, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
                    ASSERT((ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList) == dbtimeObj);
                    dbspaceObj->time = time;
                    ASSERT(listFact->fcts.count(listFact, dbtimeObj->readyList, 0) == 0);
                    waiterCount = listFact->fcts.count(listFact, dbtimeObj->waitList, 0);
                    if (waiterCount > 0)
                        dbspaceObj->state = DB_STATE_LOCAL_ACTIVE;
                }
                break;
            default:
                DPRINTF(DEBUG_LVL_INFO, "INVALID DB state %"PRId32" for op %"PRId32"\n", (u32)(dbspaceObj->state), (u32)op);
                ASSERT(0);
                return OCR_EINVAL;
            }

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);

            //Note: we do this outside DB space critical section
            if (waiterCount > 0) processDbWaitlist(pd, self, context, dbspaceObj, dbtimeObj, waiterCount);
        }
        break;
    case SM_DB_MOVE_SRC: //SM is notified that DB is now no longer scheduled at this PD and needs to be released
        {
            ASSERT(dbSize == 0 && dbPtr == NULL && space != pd->myLocation && time != 0);
            ASSERT(dbspaceObj && dbspaceObj->dbSize != 0);
            bool doRelease = false;

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_MOVE_SRC: (db: "GUIDF" time: %"PRIu64" state: %"PRIu32")\n", GUIDA(dbGuid), time, dbspaceObj->state);
            if (dbspaceObj->dbPtr == NULL) {
                ASSERT(dbspaceObj->state == DB_STATE_INFO && dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
            } else {
                ASSERT(dbspaceObj->state == DB_STATE_LOCAL_INACTIVE && dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_PINNED);
                dbspaceObj->state = DB_STATE_INFO;
                dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_MAPPED;
                dbspaceObj->dbPtr = NULL;
                doRelease =  true;
            }

            ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ASSERT(dbspaceObj->time == time && dbtimeObj->time == time);
            listFact->fcts.remove(listFact, dbspaceObj->dbTimeList, OCR_SCHEDULER_OBJECT_DBTIME, 1, NULL, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
            dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            dbspaceObj->time = (dbtimeObj != NULL) ? dbtimeObj->time : 0;

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);

            //DB transfer to destination
            RESULT_ASSERT(dbMoveSend(self, pd, dbGuid, space, dbspaceObj, doRelease), ==, 0);

        }
        break;
    case SM_DB_AT_SPACE: //SM is notified that DB has arrived at PD
        {
            ASSERT(dbspaceObj && dbSize == dbspaceObj->dbSize && dbPtr != NULL && space == pd->myLocation && time == 0 && task == NULL);

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_DB_AT_SPACE: (db: "GUIDF" ptr: %p state: %"PRIu32")\n", GUIDA(dbGuid), dbPtr, dbspaceObj->state);
            dbspaceObj->dbPtr = dbPtr;
            dbspaceObj->base.mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
            ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ASSERT(dbspaceObj->time == dbtimeObj->time);
            ASSERT(listFact->fcts.count(listFact, dbtimeObj->readyList, 0) == 0);
            u64 waiterCount = listFact->fcts.count(listFact, dbtimeObj->waitList, 0);
            switch(dbspaceObj->state) {
            case DB_STATE_INFO:
                ASSERT(waiterCount == 0);
                dbspaceObj->state = DB_STATE_LOCAL_INACTIVE;
                break;
            case DB_STATE_REMOTE_INACTIVE:
                ASSERT(waiterCount != 0);
                dbspaceObj->state = DB_STATE_LOCAL_ACTIVE;
                break;
            default:
                DPRINTF(DEBUG_LVL_INFO, "INVALID DB state %"PRId32" for op %"PRId32"\n", (u32)(dbspaceObj->state), (u32)op);
                ASSERT(0);
                return OCR_EINVAL;
            }

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);

            //Note: we do this outside DB space critical section
            if (waiterCount > 0) processDbWaitlist(pd, self, context, dbspaceObj, dbtimeObj, waiterCount);
        }
        break;
    case SM_EDT_AT_SCHEDULER:
        {
            ASSERT(dbSize == 0 && dbPtr == NULL && time == 0 && task == NULL);
            dbspaceObj = createDbSpace(self, context, dbGuid, 0, NULL, pd->myLocation, 0, DB_STATE_PROXY, dbspaceObj, 0); //create if absent
            ASSERT(dbspaceObj);
            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_EDT_AT_SCHEDULER: (db: "GUIDF")\n", GUIDA(dbGuid));
        }
        break;
    case SM_EDT_AT_SPACE: //EDT has arrived at scheduled space. Check if depv DB is local. If not, add EDT to DB waitlist.
        {
            ASSERT(dbSize == 0 && dbPtr == NULL && space == pd->myLocation && time != 0 && task != NULL);
            dbspaceObj = createDbSpace(self, context, dbGuid, 0, NULL, pd->myLocation, time, DB_STATE_PROXY, dbspaceObj, 0); //create if absent
            ASSERT(dbspaceObj && time >= dbspaceObj->time);

            //Dbspace critical section start
            hal_lock(&dbspaceObj->lock);

            DPRINTF(DEBUG_LVL_VERB, "ST-SCHEDULER: SM_EDT_AT_SPACE: (edt: "GUIDF" db: "GUIDF" time: %"PRIu64" state: %"PRIu32")\n", GUIDA(task->guid), GUIDA(dbGuid), time, dbspaceObj->state);
            //Find time slot for EDT
            ocrSchedulerObjectDbtime_t *dbtimeObj = getDbTime(self, context, dbspaceObj, time);
            ASSERT(dbtimeObj && dbtimeObj->time == time && dbtimeObj->space == pd->myLocation);

            dbtimeObj->edtScheduledCount += 1;

            bool local = false;
            if (dbspaceObj->time == time) {
                switch(dbspaceObj->state) {
                case DB_STATE_INFO:
                    break;
                case DB_STATE_LOCAL_INACTIVE:
                    dbspaceObj->state = DB_STATE_LOCAL_ACTIVE;
                    break;
                default:
                    break;
                }
                if (dbspaceObj->state == DB_STATE_LOCAL_ACTIVE)
                    local = true;
            }

            if (!local) {
                listFact->fcts.insert(listFact, dbtimeObj->waitList, (ocrSchedulerObject_t*)task, NULL,
                                         (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL));
                retVal = 1;
            }
            DPRINTF(DEBUG_LVL_VVERB, "ST-SCHEDULER: SM_EDT_AT_SPACE: (edt: "GUIDF" db: "GUIDF" state: %"PRIu32" time: %"PRIu64" active: %"PRIu64" [%p] edtScheduled: %"PRIu32" edtDone: %"PRIu32" local: %"PRIu32")\n",
                                                                      GUIDA(task->guid), GUIDA(dbGuid), dbspaceObj->state, time, dbspaceObj->activeCount,
                                                                      dbtimeObj, dbtimeObj->edtScheduledCount, dbtimeObj->edtDoneCount, (u8)local);

            //Dbspace critical section end
            hal_unlock(&dbspaceObj->lock);
        }
        break;
    default:
        ASSERT(0);
        break;
    }

    if (localDestruct) {
        destructDbSpace(self, context, dbspaceObj);
        dbspaceObj = NULL;
    }

    if (dbspaceObj && dbspacePtr) {
        if (*dbspacePtr) {
            ASSERT(*dbspacePtr == dbspaceObj);
        } else {
            *dbspacePtr = dbspaceObj;
        }
    }

    return retVal;
}

//Sends the scheduler decision of the scheduled time and space of the EDT to the requester
static u8 respondSchedulerDecision(ocrSchedulerHeuristic_t *self, ocrGuid_t edtGuid, ocrLocation_t edtLocation, ocrLocation_t scheduleSpace, u64 scheduleTime) {
    ocrPolicyDomain_t *pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    DPRINTF(DEBUG_LVL_INFO, "Responding Space-Time decision for EDT "GUIDF": Space: %"PRIu64" Time: %"PRIu64"\n", GUIDA(edtGuid), scheduleSpace, scheduleTime);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
    msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
    msg.destLocation = edtLocation;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).guid = edtGuid;
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_RESPONSE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_SPACETIME_EDT;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).resp.space = scheduleSpace;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).resp.time = scheduleTime;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

//This is the main heuristic that is invoked by the scheduler node to determine an EDT's space and time of execution
static u8 analyzeEdtSpaceTime(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context,
                              ocrGuid_t edtGuid, ocrLocation_t edtLocation, u32 depc, ocrEdtDep_t *depv,
                              ocrEdtProxy_t *edtProxy)
{
    u32 i, j, k, shiftCount;
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    DPRINTF(DEBUG_LVL_VVERB, "Analyzing Space-Time for EDT "GUIDF"\n", GUIDA(edtGuid));

    ocrSchedulerHeuristicSt_t *derived = (ocrSchedulerHeuristicSt_t*)self;
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObjectFactory_t *listFact = pd->schedulerObjectFactories[stContext->listIterator->fctId];

    u32 startIdx = 0;
    u32 deps = 0;

    if (edtProxy) { //Resuming from a suspension point
        ocrSchedulerObjectDbspace_t *dbspaceObj __attribute__((unused)) = (ocrSchedulerObjectDbspace_t*)edtProxy->depv[edtProxy->frontierIdx].ptr;
        ASSERT(dbspaceObj && dbspaceObj->state != DB_STATE_PROXY);
        startIdx = edtProxy->frontierIdx + 1;
        deps = edtProxy->deps;
        edtProxy->frontierIdx = depc;
        edtProxy->deps = depc + 1;
    }

    //Get the dbspace objects for the depv guids
    //We reuse the depv[i].ptr field to hold the db objects
    for (i = startIdx; i < depc; i++) {
        if (!ocrGuidIsNull(depv[i].guid) && (depv[i].mode != DB_MODE_NULL)) {
            ASSERT(depv[i].ptr == NULL);
            /* Ensure uniqueness to avoid double-locking */
            //TODO: optimize!
            bool uniq = true;
            for (j = 0; j < i; j++) {
                if (ocrGuidIsEq(depv[j].guid, depv[i].guid)) {
                    uniq = false;
                    break;
                }
            }
            if (uniq) {
                deps++;
                ocrSchedulerObjectDbspace_t *dbspaceObj = NULL;
                RESULT_ASSERT(processDbSMOP(self, context, SM_EDT_AT_SCHEDULER, depv[i].guid, 0, NULL, edtLocation, 0, 1, NULL, &dbspaceObj, 0), ==, 0);
                if (dbspaceObj->state == DB_STATE_PROXY) { //DB info has not yet reached scheduler node. EDT needs to wait.
                    ASSERT(pd->neighborCount != 0);
                    bool proxy = false;
                    hal_lock(&dbspaceObj->lock);
                    if (dbspaceObj->state == DB_STATE_PROXY) { //Double check to ensure
                        if (edtProxy == NULL) {
                            edtProxy = (ocrEdtProxy_t*)pd->fcts.pdMalloc(pd, sizeof(ocrEdtProxy_t));
                            edtProxy->edtGuid = edtGuid;
                            edtProxy->edtLocation = edtLocation;
                            edtProxy->depc = depc;
                            edtProxy->depv = (ocrEdtDep_t*)pd->fcts.pdMalloc(pd, sizeof(ocrEdtDep_t)*depc);
                            for (k = 0; k < depc; k++) edtProxy->depv[k] = depv[k];
                        }
                        edtProxy->frontierIdx = i;
                        edtProxy->deps = deps;
                        listFact->fcts.insert(listFact, dbspaceObj->schedList, (ocrSchedulerObject_t*)edtProxy, NULL, (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL));
                        proxy = true;
                    }
                    hal_unlock(&dbspaceObj->lock);
                    if (proxy)
                        return 0; //We suspend until DB reaches scheduler node
                }
                depv[i].ptr = dbspaceObj;
            }
        }
    }
    ASSERT(deps <= depc);
    DPRINTF(DEBUG_LVL_VVERB, "Analyzing Space-Time for EDT "GUIDF": Useful deps: %"PRIu32"\n", GUIDA(edtGuid), deps);

    //If we are running only one node, there can only be one time slot available.
    //Respond immediately with default time slot of 1.
    if (pd->neighborCount == 0) {
        for (i = 0; i < depc; i++) {
            if (depv[i].ptr != NULL) {
                ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
                hal_lock(&dbspaceObj->lock);
                ASSERT(dbspaceObj->time == 1);
                ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
                ASSERT(dbtimeObj->time == 1);
                dbtimeObj->schedulerCount += 1;
                hal_unlock(&dbspaceObj->lock);
                depv[i].ptr = NULL;
            }
        }
        return respondSchedulerDecision(self, edtGuid, edtLocation, edtLocation, 1);
    }

    if (deps == 0) { //If EDT has no useful deps, then spread it for load-balancing
        //TODO: Bad placement (round-robin); needs better placement with updated load information.
        //TODO: Read affinity hint
        u64 scheduleTime = 1; //use the default time slot
        hal_lock(&derived->locationLock);
        ocrLocation_t scheduleSpace = derived->locationPlacement;
        derived->locationPlacement = (derived->locationPlacement + 1) % (pd->neighborCount + 1); //TODO: Works for MPI ranks only. Use platform model in future.
        hal_unlock(&derived->locationLock);
        DPRINTF(DEBUG_LVL_VERB, "Scheduler decision (load balance) for EDT "GUIDF": Space: %"PRIu64" Time: %"PRIu64"\n", GUIDA(edtGuid), scheduleSpace, scheduleTime);
        return respondSchedulerDecision(self, edtGuid, edtLocation, scheduleSpace, scheduleTime);
    }

    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////     FULL DEPV LOCKING ALGO    /////////////////////
    ///////////////////////////////////////////////////////////////////////////

    //Take locks on all the dbspace objects corresponding to DBs in dependence slots
    //Note: This is a finer locking mechanism than sorted locking because
    //it avoids the transitive locked chain blocking problem.
    //Specifically, two independent sets of DBs can make progress if a
    //third set which overlaps with both is blocked. This is not possible
    //in sorted locking schemes.

    DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF": Depv Lock: START (deps: %"PRIu32")\n", GUIDA(edtGuid), deps);

    ocrSchedulerObjectPdspace_t *pdspaceObj = (ocrSchedulerObjectPdspace_t*)self->scheduler->rootObj;
    while (1) {
        bool contention = false;
        hal_lock(&pdspaceObj->lock);
        for (i = 0; i < depc; i++) {
            if(depv[i].ptr != NULL) {
                ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
                if (hal_trylock(&dbspaceObj->lock) != 0) {
                    contention = true;
                    for (j = 0; j < i; j++) {
                        if(depv[j].ptr != NULL) {
                            ocrSchedulerObjectDbspace_t *dbspaceObjPtr = (ocrSchedulerObjectDbspace_t*)depv[j].ptr;
                            hal_unlock(&dbspaceObjPtr->lock);
                        }
                    }
                    break;
                }
            }
        }
        hal_unlock(&pdspaceObj->lock);

        if (!contention) break;

        //Maximize probability of grabbing all the dbspace locks next time before trying to lock pdspace
        do {
            for (i = 0; i < depc; i++) {
                if(depv[i].ptr != NULL) {
                    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
                    if(hal_islocked(&(dbspaceObj->lock))) {
                        while(hal_islocked(&(dbspaceObj->lock)))
                            ;
                        break;
                    }
                }
            }
        } while(i < depc);
    }

    DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF": Depv Lock: LOCKED\n", GUIDA(edtGuid));

    ///////////////////////////////////////////////////////////////////////////
    /////////////////////////     FULL DEPV LOCKED    /////////////////////////
    ///////////////////////////////////////////////////////////////////////////

    //This heuristic uses the maximum sized DB in the depv as the reference DB
    //and its timeline is used as the reference timeline.
    //The heuristic will iterate over the time slots in the reference timeline.
    //For each time slot found on the reference timeline, we check if the other DBs
    //in the depv also have the same time slot in their timelines.
    //If they do, then for every other DB in the depv, we check if that DB
    //is scheduled on the same space as the reference DB in that particular time slot.
    //If the space matches, then we have found a "matching" timeslot for the other DB.
    //If the space does not match, then we have found a conflict and with that
    //the current reference timeslot becomes infeasible for scheduling. Subsequently,
    //the heuristic will iterate onto the next time slot in the reference timeline.
    //If the reference timeslot is missing from the other DB timeline, then
    //we have a feasible timeslot but one that will require data movement (conservative estimate).
    //A missing but feasible time slot can be inserted/appended to a DB timeline.
    //So, the goal of the heuristic is to find a timeslot on the reference timeline
    //that is feasible for scheduling other depv DB's as well as incurring the least
    //data movement costs.
    //Finally, if no feasible timeslot exists, the heuristic will shift to the DB with the
    //max timeline as the reference DB and continue the search for a feasible time slot.
    //If even after that no feasible time slot is found, the reference DB (now with the max timeline)
    //will extend the reference timeline by one time tick but maintain the same space
    //as the last timeslot. All other DB's add the new time slot and space to their timelines.

    //Initialize schedule vars
    u64 scheduleTime = 0;
    ocrLocation_t scheduleSpace = edtLocation; //use EDT's current location as default. TODO: Read the affinity hint.
    ocrSchedulerObjectDbspace_t *dbspaceObjRef = NULL;
    ocrSchedulerObjectDbtime_t *dbtimeObjRef = NULL;
    ocrLocation_t refSpace = edtLocation;
    u64 refTime = 0;
    u32 totalDbSize = 0;
    u32 maxDbSize = 0;
    u32 refDbIndex __attribute__((unused)) = depc;

    //Reset the time iterators in the depv DB space objects to HEAD
    //Use max size DB as the reference timeline
    for (i = 0; i < depc; i++) {
        if (depv[i].ptr != NULL) {
            ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
            ASSERT(dbspaceObj->base.mapping != OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED);
            ASSERT(dbspaceObj->state != DB_STATE_PROXY && dbspaceObj->dbSize != 0 && dbspaceObj->time != 0);
            listFact->fcts.iterate(listFact, dbspaceObj->listIterator, SCHEDULER_OBJECT_ITERATE_HEAD);
            ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)dbspaceObj->listIterator->data;
            ASSERT(dbtimeObj); //DB must have a current time slot
            if (dbtimeObj->schedulerDone) { //If time slot is done, shift to the next one
                listFact->fcts.iterate(listFact, dbspaceObj->listIterator, SCHEDULER_OBJECT_ITERATE_NEXT);
                dbtimeObj = (ocrSchedulerObjectDbtime_t*)dbspaceObj->listIterator->data;
                ASSERT(dbtimeObj);
            }
            totalDbSize += dbspaceObj->dbSize;
            if (dbspaceObj->dbSize > maxDbSize) {
                dbspaceObjRef = dbspaceObj;
                dbtimeObjRef = dbtimeObj;
                refDbIndex = i;
            }
        }
    }

    ASSERT(refDbIndex < depc);
    u32 minDataMovement = totalDbSize; //data movement initialization

    do {
        DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF": RefDb "GUIDF"\n", GUIDA(edtGuid), GUIDA(depv[refDbIndex].guid));
        while (dbtimeObjRef != NULL) {
            refSpace = dbtimeObjRef->space;
            refTime = dbtimeObjRef->time;
            DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF" RefDb "GUIDF" Time Scan: Ref space %"PRIu64" and time %"PRIu64"\n", GUIDA(edtGuid), GUIDA(depv[refDbIndex].guid), refSpace, refTime);
            u32 curDataMovement = 0;
            bool feasible = true;
            for (i = 0; i < depc && feasible; i++) {
                if (depv[i].ptr != NULL && depv[i].ptr != (void*)dbspaceObjRef) {
                    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
                    ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)dbspaceObj->listIterator->data;

                    if (dbtimeObj) {
                        //Progress the time iterator of this DB to be at least the reference schedule time slot
                        while (dbtimeObj && dbtimeObj->time < refTime) {
                            listFact->fcts.iterate(listFact, dbspaceObj->listIterator, SCHEDULER_OBJECT_ITERATE_NEXT);
                            dbtimeObj = (ocrSchedulerObjectDbtime_t*)dbspaceObj->listIterator->data;
                        }

                        //If a time slot is found that matches the reference schedule time slot and space, then we don't count data movement costs
                        //If no more time slot is available, that is also a feasible solution since a new time slot can be added at the end.
                        //But for conservative reasons, we will count data movement costs
                        if (dbtimeObj) {
                            DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF" RefDb "GUIDF" Time Scan: DB "GUIDF" space %"PRIu64" and time %"PRIu64"\n", GUIDA(edtGuid), GUIDA(depv[refDbIndex].guid), GUIDA(depv[i].guid), dbtimeObj->space, dbtimeObj->time);
                            if (dbtimeObj->time == refTime) {
                                if (dbtimeObj->space == refSpace) {
                                    DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF" RefDb "GUIDF" Time Scan: DB "GUIDF" Match found!\n", GUIDA(edtGuid), GUIDA(depv[refDbIndex].guid), GUIDA(depv[i].guid));
                                } else {
                                    //We have a scheduling conflict.
                                    feasible = false;
                                    curDataMovement = totalDbSize;
                                }
                            } else {
                                curDataMovement += dbspaceObj->dbSize; //Reference time slot does not exist in DB. Need to insert time slot. Count data movement cost.
                            }
                        } else {
                            curDataMovement += dbspaceObj->dbSize; //Reference time slot exceeds DB timeline. Need to append time slot. Count data movement cost.
                        }
                    } else {
                        curDataMovement += dbspaceObj->dbSize; //Reference time slot exceeds DB timeline. Need to append time slot. Count data movement cost.
                    }
                }
            }

            ASSERT(minDataMovement > 0);
            if (curDataMovement < minDataMovement)
                minDataMovement = curDataMovement;

            //If a goal schedule time and space has been found, we declare victory and break.
            if (minDataMovement == 0)
                break;

            //Increment the reference time slot
            listFact->fcts.iterate(listFact, dbspaceObjRef->listIterator, SCHEDULER_OBJECT_ITERATE_NEXT);
            dbtimeObjRef = (ocrSchedulerObjectDbtime_t*)dbspaceObjRef->listIterator->data;

        } //end inner while

        ASSERT(refTime != 0);

        if (minDataMovement == totalDbSize) { //No feasible time slot found on reference DB
            ocrSchedulerObjectDbspace_t *dbspaceObjRefPrev __attribute__((unused)) = dbspaceObjRef;
            dbspaceObjRef = NULL;
            dbtimeObjRef = NULL;
            maxDbSize = 0;
            //Find a new reference DB with a longer timeline than the previous one
            for (i = 0; i < depc; i++) {
                if (depv[i].ptr != NULL) {
                    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
                    ocrSchedulerObjectDbtime_t *dbtimeObj = (ocrSchedulerObjectDbtime_t*)dbspaceObj->listIterator->data;
                    if (dbtimeObj && dbspaceObj->dbSize > maxDbSize) {
                        ASSERT(dbspaceObj != dbspaceObjRefPrev);
                        dbspaceObjRef = dbspaceObj;
                        dbtimeObjRef = dbtimeObj;
                        refDbIndex = i;
                    }
                }
            }

            if (dbspaceObjRef == NULL) { //No feasible time slot found on any DB
                scheduleSpace = refSpace;
                scheduleTime = refTime + 1;
                break;
            }
        } else {
            scheduleSpace = refSpace;
            scheduleTime = refTime;
            break;
        }
    } while (dbtimeObjRef != NULL);

    DPRINTF(DEBUG_LVL_VERB, "Scheduler decision (data movement) for EDT "GUIDF": Space: %"PRIu64" Time: %"PRIu64"\n", GUIDA(edtGuid), scheduleSpace, scheduleTime);

    //Update the DB space objects in this scheduler node to account for the newly scheduled EDT
    shiftCount = 0;
    for (i = 0; i < depc; i++) {
        if (depv[i].ptr != NULL) {
            //Get the schedule time slot and increment the scheduler count
            ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
            ocrSchedulerObjectDbtime_t *dbtimeObj = createDbTime(self, context, dbspaceObj, scheduleSpace, scheduleTime);
            ASSERT(dbtimeObj && dbtimeObj->time == scheduleTime && dbtimeObj->space == scheduleSpace);
            dbtimeObj->schedulerCount += 1;

            //Check if DB needs to move
            ocrSchedulerObjectDbtime_t *dbtimeObjHead = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
            ASSERT(dbtimeObjHead && dbtimeObjHead->time == dbspaceObj->time);
            if (dbtimeObjHead->schedulerCount == dbtimeObjHead->edtDoneCount) {
                ASSERT(dbtimeObjHead != dbtimeObj);
                if (dbtimeObjHead->schedulerDone == false) {
                    ocrSchedulerObjectDbtime_t *dbtimeObjNext __attribute__((unused)) = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHeadNext(dbspaceObj->dbTimeList);
                    ASSERT(dbtimeObjNext && (dbtimeObjNext->time > dbtimeObjHead->time) && (dbtimeObjNext->schedulerCount > 0) && (dbtimeObjNext->edtDoneCount == 0));
                    dbtimeObjHead->schedulerDone = true; //time-shift possible
                    shiftCount++;
                }
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    ////////////////////////////     UNLOCK DEPV    ///////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    for (i = 0; i < depc; i++) {
        if (depv[i].ptr != NULL) {
            ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
            hal_unlock(&dbspaceObj->lock);
        }
    }

    DPRINTF(DEBUG_LVL_VVERB, "EDT "GUIDF": Depv Lock: UNLOCKED\n", GUIDA(edtGuid));

    ///////////////////////////////////////////////////////////////////////////
    ////////////////////////     FULL DEPV UNLOCKED    ////////////////////////
    ///////////////////////////////////////////////////////////////////////////

    //Do the DB time shifts
    if (shiftCount > 0) {
        for (i = 0; i < depc; i++) {
            if (depv[i].ptr != NULL) {
                ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)depv[i].ptr;
                RESULT_ASSERT(processDbSMOP(self, context, SM_DB_TIME_SHIFT_AT_SCHEDULER, dbspaceObj->dbGuid, 0, NULL, pd->myLocation, 0, 0, NULL, &dbspaceObj, 0), ==, 0);
            }
        }
    }

    for (i = 0; i < depc; i++)
        depv[i].ptr = NULL;

    //Finally send the results of scheduler analysis of the edt's scheduled space and time to the requester
    RESULT_ASSERT(respondSchedulerDecision(self, edtGuid, edtLocation, scheduleSpace, scheduleTime), ==, 0);

    //Delete the edtProxy if created
    if (edtProxy) {
        pd->fcts.pdFree(pd, edtProxy->depv);
        pd->fcts.pdFree(pd, edtProxy);
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/******************************************************************************/
/* WORK FUNCTIONS                                                             */
/******************************************************************************/

/* Find EDT for the worker to execute - This uses random workstealing to find work if no work is found owned deque */
static u8 stSchedulerHeuristicWorkEdtUserInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrSchedulerObject_t edtObj;
    edtObj.guid.guid = NULL_GUID;
    edtObj.guid.metaDataPtr = NULL;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;

    //First try to pop from own deque
    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObject_t *schedObj = stContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
    u8 retVal = fact->fcts.remove(fact, schedObj, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_TAIL);

    //If pop fails, then try to steal from other deques
    if (ocrGuidIsNull(edtObj.guid.guid)) {
        //First try to steal from the last deque that was visited (probably had a successful steal)
        ocrSchedulerObject_t *stealSchedulerObject = ((ocrSchedulerHeuristicContextSt_t*)self->contexts[stContext->stealSchedulerObjectIndex])->mySchedulerObject;
        ASSERT(stealSchedulerObject);
        retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD); //try cached deque first

        //If cached steal failed, then restart steal loop from starting index
        ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
        ocrSchedulerObjectFactory_t *sFact = self->scheduler->pd->schedulerObjectFactories[rootObj->fctId];
        while (ocrGuidIsNull(edtObj.guid.guid) && sFact->fcts.count(sFact, rootObj, (SCHEDULER_OBJECT_COUNT_EDT | SCHEDULER_OBJECT_COUNT_RECURSIVE) ) != 0) {
            u32 i;
            for (i = 1; ocrGuidIsNull(edtObj.guid.guid) && i < self->contextCount; i++) {
                stContext->stealSchedulerObjectIndex = (context->id + i) % self->contextCount; //simple round robin stealing
                stealSchedulerObject = ((ocrSchedulerHeuristicContextSt_t*)self->contexts[stContext->stealSchedulerObjectIndex])->mySchedulerObject;
                if (stealSchedulerObject)
                    retVal = fact->fcts.remove(fact, stealSchedulerObject, OCR_SCHEDULER_OBJECT_EDT, 1, &edtObj, NULL, SCHEDULER_OBJECT_REMOVE_HEAD);
            }
        }
    }

    if(!(ocrGuidIsNull(edtObj.guid.guid))){
        taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt = edtObj.guid;
        //Add same schedObj determine flag
#ifdef OCR_MONITOR_SCHEDULER
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_WORKER, OCR_ACTION_WORK_TAKEN, edtObj.guid.guid, schedObj);
        ocrWorker_t *wrkr;
        getCurrentEnv(NULL, &wrkr, NULL, NULL);
        wrkr->isSeeking = false;
#endif
    }
    return retVal;
}

u8 stSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_EDT_USER:
        return stSchedulerHeuristicWorkEdtUserInvoke(self, context, opArgs, hints);
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 stSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************************************/
/* NOTIFY FUNCTIONS                                                           */
/******************************************************************************/

static u8 stDbCreate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrDataBlock_t *db) {
    u64 time;
    ocrPolicyDomain_t *pd;
    ocrTask_t *task;
    getCurrentEnv(&pd, NULL, &task, NULL);
    if (task) {
        ocrHintVal_t hintVal = {};
        ocrHint_t edtHint;
        ocrHintInit(&edtHint, OCR_HINT_EDT_T);
        RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.getHint(task, &edtHint), ==, 0);
        RESULT_ASSERT(ocrHintGetValue(&edtHint, OCR_HINT_EDT_TIME, &hintVal), ==, 0);
        time = hintVal.u64Val;
    } else {
        time = 1;
    }
    u64 count = db->ptr ? 1 : 0;

    //Create a DB space object in current PD
    RESULT_ASSERT(processDbSMOP(self, context, SM_DB_CREATE, db->guid, db->size, db->ptr, pd->myLocation, time, count, NULL, NULL, 0), ==, 0);

    //Notify scheduler node of new DB
    ocrSchedulerHeuristicSt_t *derived = (ocrSchedulerHeuristicSt_t*)self;
    if (pd->myLocation != derived->schedulerLocation) {
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
        msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
        msg.destLocation = derived->schedulerLocation;
        PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
        PD_MSG_FIELD_IO(schedArgs).guid = db->guid;
        PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_CREATE;
        PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_SPACETIME_DB;
        PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).create.dbSize = db->size;
        PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).create.time = time;
        PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).create.count = count;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    }
    return 0;
}

static u8 stSchedulerHeuristicNotifyEdtSatisfiedInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    u32 i;
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerHeuristicSt_t *derived = (ocrSchedulerHeuristicSt_t*)self;
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrTask_t *task __attribute__((unused)) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_SATISFIED).guid.metaDataPtr;
    ASSERT(task);

    //This scheduler does not support scheduling of runtime EDTs
    if ((task->flags & OCR_TASK_FLAG_RUNTIME_EDT) != 0)
        return OCR_ENOTSUP;

    //If a task is created using an affinity hint in this PD
    //and it does not have any deps, then the scheduler will
    //obey the hints and execute the task in this PD.
    //Task will get default time which is 1.
    if ((task->flags & OCR_TASK_FLAG_USES_AFFINITY) != 0 && task->depc == 0) {
        ocrHint_t edtHint;
        ocrHintInit(&edtHint, OCR_HINT_EDT_T);
        ocrHintVal_t hintVal = {};
        hintVal.u64Val = pd->myLocation;
        RESULT_ASSERT(ocrHintSetValue(&edtHint, OCR_HINT_EDT_SPACE, hintVal), ==, 0);
        hintVal.u64Val = 1;
        RESULT_ASSERT(ocrHintSetValue(&edtHint, OCR_HINT_EDT_TIME, hintVal), ==, 0);
        RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.setHint(task, &edtHint), ==, 0);
        return OCR_ENOP;
    }

    ocrTaskHc_t *hcTask = (ocrTaskHc_t*)task; //BUG #926:This is temporary until we get proper introspection support
    ocrEdtDep_t *depv = hcTask->resolvedDeps;

    DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Edt Satisfied: EDT: "GUIDF" Depc: %"PRId32"\n", GUIDA(task->guid), task->depc);
    for (i = 0; i < task->depc; i++) {
        DPRINTF(DEBUG_LVL_VERB, "SCHED_NOTIFY: Edt Satisfied: EDT: "GUIDF" Dep[%"PRId32"]: "GUIDF"\n", GUIDA(task->guid), i, GUIDA(depv[i].guid));
    }

    //Clear the data pointer values
    for (i = 0; i < task->depc; i++)
        depv[i].ptr = NULL;

    //Send task deps and associated modes to scheduler node for space/time analysis
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
    msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
    msg.destLocation = derived->schedulerLocation;
    PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
    PD_MSG_FIELD_IO(schedArgs).guid = task->guid;
    PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_SPACETIME_EDT;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).req.depc = task->depc;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).req.depv = hcTask->resolvedDeps;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

static u8 stSchedulerHeuristicNotifyEdtReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ocrFatGuid_t *fguid = &(notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid);

    DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Edt Ready: EDT: "GUIDF" \n", GUIDA(fguid->guid));

    ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
    ocrSchedulerObject_t *schedObj = stContext->mySchedulerObject;
    ASSERT(schedObj);
    ocrSchedulerObject_t edtObj;
    edtObj.guid = *fguid;
    edtObj.kind = OCR_SCHEDULER_OBJECT_EDT;
    ocrSchedulerObjectFactory_t *fact = self->scheduler->pd->schedulerObjectFactories[schedObj->fctId];
#ifdef OCR_MONITOR_SCHEDULER
    ocrGuid_t taskGuid = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid;
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_SCHEDULED, taskGuid, schedObj);
#endif
    return fact->fcts.insert(fact, schedObj, &edtObj, NULL, (SCHEDULER_OBJECT_INSERT_AFTER | SCHEDULER_OBJECT_INSERT_POSITION_TAIL));
}

static u8 stSchedHeuristicNotifyPreProcessMsgInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ASSERT(notifyArgs->kind == OCR_SCHED_NOTIFY_PRE_PROCESS_MSG);
    ocrPolicyMsg_t * msg = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG).msg;
    ASSERT((msg->type & PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER) == 0);
    u64 msgType = (msg->type & PD_MSG_TYPE_ONLY);
    switch(msgType) {
    case PD_MSG_DB_CREATE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
            if (PD_MSG_FIELD_I(dbType) == USER_DBTYPE) {
                msg->type |= (PD_MSG_LOCAL_PROCESS | PD_MSG_REQ_POST_PROCESS_SCHEDULER);
                msg->destLocation = pd->myLocation;
            }
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    case PD_MSG_DB_ACQUIRE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_ACQUIRE
            if ((PD_MSG_FIELD_IO(properties) & (DB_PROP_RT_ACQUIRE | DB_PROP_RT_PD_ACQUIRE)) == 0) {
                msg->type |= (PD_MSG_LOCAL_PROCESS | PD_MSG_REQ_POST_PROCESS_SCHEDULER);
                msg->destLocation = pd->myLocation;
            }
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    case PD_MSG_DB_RELEASE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_RELEASE
            if ((PD_MSG_FIELD_I(properties) & (DB_PROP_RT_ACQUIRE | DB_PROP_RT_PD_ACQUIRE)) == 0) {
                ocrGuid_t dbGuid = PD_MSG_FIELD_IO(guid.guid);
                DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Msg Pre Process: DB_RELEASE: db: "GUIDF" edt: "GUIDF"\n", GUIDA(dbGuid), GUIDA(PD_MSG_FIELD_I(edt.guid)));
                RESULT_ASSERT(processDbSMOP(self, context, SM_DB_RELEASE, dbGuid, 0, 0, pd->myLocation, 0, 1, NULL, NULL, 0), ==, 0);
                msg->type |= PD_MSG_LOCAL_PROCESS;
                msg->destLocation = pd->myLocation;
            }
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    case PD_MSG_DB_FREE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_FREE
            if ((PD_MSG_FIELD_I(properties) & DB_PROP_RT_ACQUIRE) == 0) {
                ocrGuid_t dbGuid = PD_MSG_FIELD_I(guid.guid);
                DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Msg Pre Process: DB_FREE: db: "GUIDF"\n", GUIDA(dbGuid));
                RESULT_ASSERT(processDbSMOP(self, context, SM_DB_FREE, dbGuid, 0, 0, pd->myLocation, 0, 0, NULL, NULL, PD_MSG_FIELD_I(properties)), ==, 0);
            }
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    //All msgs that will require local processing
    case PD_MSG_DEP_DYNADD:
    {
        msg->type |= PD_MSG_LOCAL_PROCESS;
        msg->destLocation = pd->myLocation;
        break;
    }
    default:
        return OCR_ENOP;
    }
    return 0;
}

static u8 stSchedHeuristicNotifyPostProcessMsgInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    ASSERT(notifyArgs->kind == OCR_SCHED_NOTIFY_POST_PROCESS_MSG);
    ocrPolicyMsg_t * msg = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_POST_PROCESS_MSG).msg;
    ASSERT(msg->type & PD_MSG_REQ_POST_PROCESS_SCHEDULER);
    u64 msgType = (msg->type & PD_MSG_TYPE_ONLY);
    switch(msgType) {
    case PD_MSG_DB_CREATE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
            ocrDataBlock_t *db = PD_MSG_FIELD_IO(guid.metaDataPtr);
            ASSERT(db && db->size != 0);
            ASSERT(PD_MSG_FIELD_IO(size) == db->size);
            ASSERT(PD_MSG_FIELD_O(ptr) == db->ptr);
            DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Msg Post Process: DB_CREATE: db: "GUIDF" (size: %"PRIu64" ptr: %p)\n", GUIDA(db->guid), db->size, db->ptr);
            RESULT_ASSERT(stDbCreate(self, context, db), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    case PD_MSG_DB_ACQUIRE:
        {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_ACQUIRE
            if (PD_MSG_FIELD_O(returnDetail) == 0) {
                ocrGuid_t dbGuid = PD_MSG_FIELD_IO(guid.guid);
                DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Msg Post Process: DB_ACQUIRE: db: "GUIDF" edt: "GUIDF"\n", GUIDA(dbGuid), GUIDA(PD_MSG_FIELD_IO(edt.guid)));
                RESULT_ASSERT(processDbSMOP(self, context, SM_DB_ACQUIRE, dbGuid, 0, 0, pd->myLocation, 0, 1, NULL, NULL, 0), ==, 0);
            }
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    default:
        return OCR_ENOP;
    }
    return 0;
}

u8 stSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_PRE_PROCESS_MSG:
        return stSchedHeuristicNotifyPreProcessMsgInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_POST_PROCESS_MSG:
        return stSchedHeuristicNotifyPostProcessMsgInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_SATISFIED:
        return stSchedulerHeuristicNotifyEdtSatisfiedInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_READY:
        return stSchedulerHeuristicNotifyEdtReadyInvoke(self, context, opArgs, hints);
    case OCR_SCHED_NOTIFY_EDT_DONE:
        {
            // Destroy the work
            ocrPolicyDomain_t *pd;
            PD_MSG_STACK(msg);
            getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
            getCurrentEnv(NULL, NULL, NULL, &msg);
            msg.type = PD_MSG_WORK_DESTROY | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(guid) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid;
            PD_MSG_FIELD_I(currentEdt) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid;
            PD_MSG_FIELD_I(properties) = 0;
#ifdef OCR_ASSERT
            ocrTask_t *task __attribute__((unused)) = notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid.metaDataPtr;
            ASSERT(task);
            if ((task->flags & OCR_TASK_FLAG_RUNTIME_EDT) == 0) {
                DPRINTF(DEBUG_LVL_INFO, "SCHED_NOTIFY: Edt Done: EDT: "GUIDF" \n", GUIDA(task->guid));
            }
            //TODO: Handle potential leak of task metadata on remote node when scheduled node != create node
#endif
            RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        }
        break;
    // Notifies ignored by this heuristic
    case OCR_SCHED_NOTIFY_EDT_CREATE:
        return OCR_ENOP;
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 stSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************************************/
/* TRANSACT FUNCTIONS                                                         */
/******************************************************************************/

static u8 stSchedulerHeuristicTransactEdt(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerOpTransactArgs_t *transactArgs = (ocrSchedulerOpTransactArgs_t*)opArgs;
    ocrFatGuid_t *fguid = &(transactArgs->schedObj.guid);
    ASSERT(fguid->metaDataPtr);
    ocrTask_t *task = (ocrTask_t*)fguid->metaDataPtr;
    ASSERT(task);

    //Check the scheduled space of the EDT
    ocrHintVal_t hintVal = {};
    ocrHint_t edtHint;
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.getHint(task, &edtHint), ==, 0);
    RESULT_ASSERT(ocrHintGetValue(&edtHint, OCR_HINT_EDT_SPACE, &hintVal), ==, 0);
    //space hint value should be the current location
    ASSERT(hintVal.u64Val == pd->myLocation);

    DPRINTF(DEBUG_LVL_INFO, "SCHED_TRANSACT: Received EDT: "GUIDF" from %"PRIu64"\n", GUIDA(task->guid), transactArgs->base.location);

    //Register the guid in this PD
    u64 val;
    pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], task->guid, &val, NULL, MD_LOCAL, NULL);
    ASSERT(val == 0);
    pd->guidProviders[0]->fcts.registerGuid(pd->guidProviders[0], task->guid, (u64) task);

    scheduleEdtDeps(self, context, task, 0);
    return 0;
}

static u8 stSchedulerHeuristicTransactDb(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerOpTransactArgs_t *transactArgs = (ocrSchedulerOpTransactArgs_t*)opArgs;
    ocrGuid_t dbGuid = transactArgs->schedObj.guid.guid;
    ocrSchedulerObjectDbspace_t *dbspaceObj = (ocrSchedulerObjectDbspace_t*)transactArgs->schedObj.guid.metaDataPtr;
    if (dbspaceObj == NULL) {
        ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
        ocrSchedulerObjectFactory_t *mapFact = pd->schedulerObjectFactories[stContext->mapIterator->fctId];
        //Search for an existing Dbspace object with DB guid
        ocrSchedulerObjectIterator_t *mapIt = stContext->mapIterator;
#if GUID_BIT_COUNT == 64
        mapIt->data = (void*) dbGuid.guid;
#elif GUID_BIT_COUNT == 128
        mapIt->data = (void*) dbGuid.lower;
#endif
        mapFact->fcts.iterate(mapFact, mapIt, SCHEDULER_OBJECT_ITERATE_SEARCH_KEY);
        dbspaceObj = (ocrSchedulerObjectDbspace_t*)mapIt->data;
        ASSERT(dbspaceObj && dbspaceObj->dbPtr == NULL && dbspaceObj->base.mapping == OCR_SCHEDULER_OBJECT_MAPPING_MAPPED);
    }

    DPRINTF(DEBUG_LVL_INFO, "SCHED_TRANSACT: Received DB: "GUIDF" from %"PRIu64"\n", GUIDA(dbspaceObj->dbGuid), transactArgs->base.location);

    void *dbPtr = dbspaceObj->dbPtr;
    u64 dbSize = dbspaceObj->dbSize;

    //If we are using the acquire/release API,
    //we acquire the DB here before proceeding.
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
    msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = dbGuid; // DB guid
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(edt.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(edt.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
    PD_MSG_FIELD_IO(properties) = DB_MODE_RW | DB_PROP_RT_PD_ACQUIRE;
    RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, true), ==, 0); //blocking call to acquire
    ASSERT(PD_MSG_FIELD_O(ptr) && PD_MSG_FIELD_O(size) == dbSize);
    ASSERT(dbPtr == NULL);
    dbPtr = PD_MSG_FIELD_O(ptr);
#undef PD_MSG
#undef PD_TYPE

    RESULT_ASSERT(processDbSMOP(self, context, SM_DB_AT_SPACE, NULL_GUID, dbSize, dbPtr, pd->myLocation, 0, 0, NULL, &dbspaceObj, 0), ==, 0);
    return 0;
}

u8 stSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpTransactArgs_t *transactArgs = (ocrSchedulerOpTransactArgs_t*)opArgs;
    ASSERT(transactArgs->properties == OCR_TRANSACT_PROP_TRANSFER);
    ocrSchedulerObjectKind kind = transactArgs->schedObj.kind;
    switch(kind) {
    case OCR_SCHEDULER_OBJECT_EDT:
        return stSchedulerHeuristicTransactEdt(self, context, opArgs, hints);
    case OCR_SCHEDULER_OBJECT_DB:
        return stSchedulerHeuristicTransactDb(self, context, opArgs, hints);
    default:
        ASSERT(0);
        break;
    }
    return 0;
}

u8 stSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************************************/
/* ANALYZE FUNCTIONS                                                          */
/******************************************************************************/

u8 stSchedulerHeuristicAnalyzeSpaceTimeEdtRequest(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    u32 i;
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ASSERT(((ocrSchedulerHeuristicSt_t*)self)->schedulerLocation == pd->myLocation) // This must be a scheduler node;

    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    ocrGuid_t edtGuid = analyzeArgs->guid;
    ocrLocation_t edtLocation = analyzeArgs->base.location;
    u32 depc = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).req.depc;
    ocrEdtDep_t *depv = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).req.depv;

    DPRINTF(DEBUG_LVL_INFO, "SCHED_ANALYZE: Space-Time Request from %"PRIu64": EDT: "GUIDF" Depc: %"PRId32"\n", edtLocation, GUIDA(edtGuid), depc);
    for (i = 0; i < depc; i++) {
        ASSERT(depv[i].ptr == NULL);
        DPRINTF(DEBUG_LVL_VERB, "SCHED_ANALYZE: Space-Time Request from %"PRIu64": EDT: "GUIDF" Dep[%"PRId32"]: "GUIDF"\n", edtLocation, GUIDA(edtGuid), i, GUIDA(depv[i].guid));
    }
    return analyzeEdtSpaceTime(self, context, edtGuid, edtLocation, depc, depv, NULL);
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeEdtResponse(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    ocrFatGuid_t fguid;
    fguid.guid = analyzeArgs->guid;
    fguid.metaDataPtr = NULL;
    pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], fguid.guid, (u64*)(&(fguid.metaDataPtr)), NULL, MD_LOCAL, NULL);
    ASSERT(fguid.metaDataPtr);
    ocrTask_t *task = (ocrTask_t*)fguid.metaDataPtr;

    //Get space and time response from scheduler node
    ocrLocation_t space = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).resp.space;
    u64 time = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_EDT).resp.time;

    DPRINTF(DEBUG_LVL_INFO, "SCHED_ANALYZE: Space-Time Response from %"PRIu64": EDT: "GUIDF" Scheduled SPACE: %"PRIu64" TIME: %"PRIu64"\n", analyzeArgs->base.location, GUIDA(analyzeArgs->guid), space, time);

    //Set space and time as runtime hints on task
    ocrHint_t edtHint;
    ocrHintVal_t hintVal = {};
    ocrHintInit(&edtHint, OCR_HINT_EDT_T);
    hintVal.u64Val = space;
    RESULT_ASSERT(ocrHintSetValue(&edtHint, OCR_HINT_EDT_SPACE, hintVal), ==, 0);
    hintVal.u64Val = time;
    RESULT_ASSERT(ocrHintSetValue(&edtHint, OCR_HINT_EDT_TIME, hintVal), ==, 0);
    RESULT_ASSERT(((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.setHint(task, &edtHint), ==, 0);

    //Schedule EDT onto the space determined by the heuristic
    if (space == pd->myLocation) {
        //If EDT is already in scheduled location,
        //then temporally schedule EDT within that location
        scheduleEdtDeps(self, context, task, 0);
    } else {
        DPRINTF(DEBUG_LVL_INFO, "ST-SCHEDULER: Transacting EDT: "GUIDF" from %"PRIu64" to %"PRIu64"\n", GUIDA(task->guid), pd->myLocation, space);

        //If EDT is not in scheduled location,
        //transact the EDT over to scheduled location
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
        ocrSchedulerObject_t *rootObj = self->scheduler->rootObj;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_TRANSACT
        msg.type = PD_MSG_SCHED_TRANSACT | PD_MSG_REQUEST;
        msg.destLocation = space;
        PD_MSG_FIELD_IO(size) = 0;
        PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
        PD_MSG_FIELD_IO(schedArgs).properties = OCR_TRANSACT_PROP_TRANSFER;
        PD_MSG_FIELD_IO(schedArgs).schedObj.guid.guid = task->guid;
        PD_MSG_FIELD_IO(schedArgs).schedObj.guid.metaDataPtr = task;
        PD_MSG_FIELD_IO(schedArgs).schedObj.kind = OCR_SCHEDULER_OBJECT_EDT;
        PD_MSG_FIELD_IO(schedArgs).schedObj.fctId = rootObj->fctId;
        PD_MSG_FIELD_IO(schedArgs).schedObj.loc = space;
        PD_MSG_FIELD_IO(schedArgs).schedObj.mapping = OCR_SCHEDULER_OBJECT_MAPPING_MAPPED;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
        task->state = REAPING_EDTSTATE; //Transition the original task state to reaping so that it can handle the future destroy.
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeEdtInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    switch(analyzeArgs->properties) {
    case OCR_SCHED_ANALYZE_REQUEST:
        return stSchedulerHeuristicAnalyzeSpaceTimeEdtRequest(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_RESPONSE:
        return stSchedulerHeuristicAnalyzeSpaceTimeEdtResponse(self, context, opArgs, hints);
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeDbCreate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ASSERT(pd->myLocation == ((ocrSchedulerHeuristicSt_t*)self)->schedulerLocation); //This must be a scheduler node

    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    ocrGuid_t dbGuid = analyzeArgs->guid;
    u64 dbSize = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).create.dbSize;
    u64 time = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).create.time;
    u64 schedulerCount = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).create.count;
    ocrLocation_t space = analyzeArgs->base.location;
    ASSERT(dbSize != 0 && time != 0);

    DPRINTF(DEBUG_LVL_INFO, "SCHED_ANALYZE: Space-Time Create from %"PRIu64": DB: "GUIDF" (size: %"PRIu64" time: %"PRIu64")\n", analyzeArgs->base.location, GUIDA(dbGuid), dbSize, time);

    ocrSchedulerObjectDbspace_t *dbspaceObj = NULL;
    u8 retVal = processDbSMOP(self, context, SM_DB_AT_SCHEDULER, dbGuid, dbSize, NULL, space, time, schedulerCount, NULL, &dbspaceObj, 0);

    //If EDTs are waiting for this DB to be scheduled, start scheduling them.
    if (retVal == 0) {
        ASSERT(dbspaceObj);
        ocrSchedulerHeuristicContextSt_t *stContext = (ocrSchedulerHeuristicContextSt_t*)context;
        ocrSchedulerObjectFactory_t *listFact = pd->schedulerObjectFactories[stContext->listIterator->fctId];
        u32 count = listFact->fcts.count(listFact, dbspaceObj->schedList, 0);
        while (count-- > 0) {
            ocrEdtProxy_t *edtProxy = (ocrEdtProxy_t*)ocrSchedulerObjectListHead(dbspaceObj->schedList);
            ASSERT(edtProxy);
            RESULT_ASSERT(listFact->fcts.remove(listFact, dbspaceObj->schedList, OCR_SCHEDULER_OBJECT_VOIDPTR, 1, NULL, NULL, SCHEDULER_OBJECT_REMOVE_HEAD), ==, 0);
            RESULT_ASSERT(analyzeEdtSpaceTime(self, context, edtProxy->edtGuid, edtProxy->edtLocation, edtProxy->depc, edtProxy->depv, edtProxy), ==, 0);
        }
        RESULT_ASSERT(listFact->fcts.count(listFact, dbspaceObj->schedList, 0), ==, 0);
    } else {
        ASSERT(dbspaceObj == NULL);
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeDbDestroy(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeDbRequest(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    ocrGuid_t dbGuid = analyzeArgs->guid;
    ocrLocation_t dstLoc = analyzeArgs->base.location;
    u64 srcTime = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).req.srcTime;

    DPRINTF(DEBUG_LVL_INFO, "SCHED_ANALYZE: Space-Time Request from %"PRIu64": DB: "GUIDF" (time: %"PRIu64")\n", analyzeArgs->base.location, GUIDA(dbGuid), srcTime);

    RESULT_ASSERT(processDbSMOP(self, context, SM_DB_MOVE_SRC, dbGuid, 0, NULL, dstLoc, srcTime, 0, NULL, NULL, 0), ==, 0);
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeDbUpdate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    ocrGuid_t dbGuid = analyzeArgs->guid;
    u64 dbSize = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.dbSize;
    ocrLocation_t srcLoc = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.srcLoc;
    u64 srcTime = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.srcTime;
    u64 dstTime = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).update.dstTime;
    ASSERT(dbSize != 0 && dstTime > srcTime);

    DPRINTF(DEBUG_LVL_INFO, "SCHED_ANALYZE: Space-Time Update from %"PRIu64": DB: "GUIDF" (srcLoc: %"PRIu64" srcTime: %"PRIu64" dstTime: %"PRIu64")\n", analyzeArgs->base.location, GUIDA(dbGuid), srcLoc, srcTime, dstTime);

    ocrSchedulerObjectDbspace_t *dbspaceObj = NULL;
    RESULT_ASSERT(processDbSMOP(self, context, SM_DB_MOVE_DST, dbGuid, dbSize, NULL, pd->myLocation, dstTime, srcTime, (void*)srcLoc, &dbspaceObj, 0), ==, 0);

    if (srcLoc != pd->myLocation) {
        ocrSchedulerObjectDbtime_t *dbtimeObj __attribute__((unused)) = (ocrSchedulerObjectDbtime_t*)ocrSchedulerObjectListHead(dbspaceObj->dbTimeList);
        ASSERT(dbtimeObj->time == dstTime);
        ASSERT(dbspaceObj->time == dstTime);

        //Send a message to source to start the transfer
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_ANALYZE
        msg.type = PD_MSG_SCHED_ANALYZE | PD_MSG_REQUEST;
        msg.destLocation = srcLoc;
        PD_MSG_FIELD_IO(schedArgs).base.heuristicId = self->factoryId;
        PD_MSG_FIELD_IO(schedArgs).guid = dbGuid;
        PD_MSG_FIELD_IO(schedArgs).properties = OCR_SCHED_ANALYZE_REQUEST;
        PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_ANALYZE_SPACETIME_DB;
        PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).req.srcTime = srcTime;
        PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).req.dstTime = dstTime;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeDbDone(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ASSERT(pd->myLocation == ((ocrSchedulerHeuristicSt_t*)self)->schedulerLocation); //This must be a scheduler node

    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    ocrGuid_t dbGuid = analyzeArgs->guid;
    ocrLocation_t space = analyzeArgs->base.location;
    u64 dbSize = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.dbSize;
    u64 time = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.time;
    u64 edtDoneCount = analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.edtDoneCount;
    u32 doFree = (space != pd->myLocation && analyzeArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_ANALYZE_SPACETIME_DB).done.free) ? 1 : 0;

    DPRINTF(DEBUG_LVL_INFO, "SCHED_ANALYZE: Space-Time Done from %"PRIu64": DB: "GUIDF" (time: %"PRIu64" count: %"PRIu64")\n", analyzeArgs->base.location, GUIDA(dbGuid), time, edtDoneCount);

    u8 retVal = processDbSMOP(self, context, SM_DB_DONE_AT_SCHEDULER, dbGuid, dbSize, NULL, space, time, edtDoneCount, NULL, NULL, doFree);
    if (retVal == 0) {
        RESULT_ASSERT(processDbSMOP(self, context, SM_DB_TIME_SHIFT_AT_SCHEDULER, dbGuid, 0, NULL, pd->myLocation, 0, 0, NULL, NULL, 0), ==, 0);
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSpaceTimeDbInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    switch(analyzeArgs->properties) {
    case OCR_SCHED_ANALYZE_CREATE:
        return stSchedulerHeuristicAnalyzeSpaceTimeDbCreate(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_REQUEST:
        return stSchedulerHeuristicAnalyzeSpaceTimeDbRequest(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_UPDATE:
        return stSchedulerHeuristicAnalyzeSpaceTimeDbUpdate(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_DONE:
        return stSchedulerHeuristicAnalyzeSpaceTimeDbDone(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_DESTROY:
        return stSchedulerHeuristicAnalyzeSpaceTimeDbDestroy(self, context, opArgs, hints);
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = (ocrSchedulerOpAnalyzeArgs_t*)opArgs;
    switch(analyzeArgs->kind) {
    case OCR_SCHED_ANALYZE_SPACETIME_EDT:
        return stSchedulerHeuristicAnalyzeSpaceTimeEdtInvoke(self, context, opArgs, hints);
    case OCR_SCHED_ANALYZE_SPACETIME_DB:
        return stSchedulerHeuristicAnalyzeSpaceTimeDbInvoke(self, context, opArgs, hints);
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

u8 stSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-ST SCHEDULER_HEURISTIC FACTORY                 */
/******************************************************/

void destructSchedulerHeuristicFactorySt(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactorySt(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactorySt_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicSt;
    base->destruct = &destructSchedulerHeuristicFactorySt;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), stSchedulerHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), stSchedulerHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), stSchedulerHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), stSchedulerHeuristicGetContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), stSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_ST */
