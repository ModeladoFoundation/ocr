/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKER_HC

#include "debug.h"
#include "ocr-comp-platform.h"
#include "ocr-db.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-types.h"
#include "ocr-worker.h"
#include "worker/hc/hc-worker.h"
#include "policy-domain/hc/hc-policy.h"

#include "experimental/ocr-platform-model.h"
#include "extensions/ocr-affinity.h"
#include "extensions/ocr-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#endif

#ifdef OCR_RUNTIME_PROFILER
#include "utils/profiler/profiler.h"
#endif

#define DEBUG_TYPE WORKER

#if defined(UTASK_COMM) || defined(UTASK_COMM2)
#include "ocr-policy-domain-tasks.h"
#endif

/******************************************************/
/* OCR-HC WORKER                                      */
/******************************************************/

static void hcWorkShift(ocrWorker_t * worker) {

    START_PROFILE(wo_hc_workShift);
    ocrPolicyDomain_t * pd;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);

    ocrWorkerHc_t *hcWorker = (ocrWorkerHc_t *) worker;
#if defined(UTASK_COMM) || defined(UTASK_COMM2)
    RESULT_ASSERT(pdProcessStrands(pd, NP_WORK, 0), ==, 0);
#endif
    u8 retCode = 0;
    {
        START_PROFILE(wo_hc_getWork);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_GET_WORK
    msg.type = PD_MSG_SCHED_GET_WORK | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_EDT_USER;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid = NULL_GUID;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.metaDataPtr = NULL;

#ifdef OCR_MONITOR_SCHEDULER
    if(!worker->isSeeking){
        worker->isSeeking = true;
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_WORKER, OCR_ACTION_WORK_REQUEST);
    }
#endif

    retCode = pd->fcts.processMessage(pd, &msg, true);
    EXIT_PROFILE;
    }
    if(retCode == 0) {
        // We got a response
        ocrFatGuid_t taskGuid = PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt;
        if(!(ocrGuidIsNull(taskGuid.guid))){
            ocrTask_t * curTask = (ocrTask_t*)taskGuid.metaDataPtr;
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
            if (((curTask->flags & OCR_TASK_FLAG_LONG) != 0) && (((ocrWorkerHc_t *) worker)->isHelping)) {
                // Illegal to pick up a LONG EDT in that case to avoid creating a deadlock
                curTask->state = RESCHED_EDTSTATE;
                hcWorker->stealFirst = true;
            } else
#endif
            {
                START_PROFILE(wo_hc_executeWork);
                // Task sanity checks
                ASSERT(taskGuid.metaDataPtr != NULL);
                worker->curTask = curTask;
                DPRINTF(DEBUG_LVL_VERB, "Worker shifting to execute EDT GUID "GUIDF"\n", GUIDA(taskGuid.guid));
                u32 factoryId = PD_MSG_FIELD_O(factoryId);
                ((ocrTaskFactory_t*)(pd->factories[factoryId]))->fcts.execute(curTask);
                hcWorker->edtGuid = curTask->guid;
#ifdef OCR_ENABLE_EDT_NAMING
                hcWorker->name = curTask->name;
#endif
                EXIT_PROFILE;
#undef PD_TYPE
            }
            {
                START_PROFILE(wo_hc_wrapupWork);
#define PD_TYPE PD_MSG_SCHED_NOTIFY
                getCurrentEnv(NULL, NULL, NULL, &msg);
                msg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
                PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_EDT_DONE;
                PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid.guid = taskGuid.guid;
                PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_DONE).guid.metaDataPtr = taskGuid.metaDataPtr;
                RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, false), ==, 0);
                EXIT_PROFILE;
            }

            // Important for this to be the last
            worker->curTask = NULL;
        }

    } else {
        ASSERT(0); //Handle error code
    }
#undef PD_MSG
#undef PD_TYPE
#ifdef ENABLE_EXTENSION_PAUSE
    ocrPolicyDomainHc_t *self = (ocrPolicyDomainHc_t *)pd;
    if(self->pqrFlags.runtimePause == true) {
        hal_xadd32((u32*)&self->pqrFlags.pauseCounter, 1);
        //Pause called - stop workers
        while(self->pqrFlags.runtimePause == true) {
            hal_pause();
        }
        hal_xadd32((u32*)&self->pqrFlags.pauseCounter, -1);
    }
#endif

    RETURN_PROFILE();
}

static void workerLoop(ocrWorker_t * worker) {
    u8 continueLoop = true;
    // At this stage, we are in the USER_OK runlevel
    ASSERT(worker->curState == GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK))));
    ocrPolicyDomain_t *pd = worker->pd;
    if (worker->amBlessed) {
        ocrGuid_t affinityMasterPD;
        u64 count = 0;
        // There should be a single master PD
        ASSERT(!ocrAffinityCount(AFFINITY_PD_MASTER, &count) && (count == 1));
        ocrAffinityGet(AFFINITY_PD_MASTER, &count, &affinityMasterPD);

        // This is all part of the mainEdt setup
        // and should be executed by the "blessed" worker.
        void * packedUserArgv = userArgsGet();
        ocrEdt_t mainEdt = mainEdtGet();
        u64 totalLength = ((u64*) packedUserArgv)[0]; // already exclude this first arg
        // strip off the 'totalLength first argument'
        packedUserArgv = (void *) (((u64)packedUserArgv) + sizeof(u64)); // skip first totalLength argument
        ocrGuid_t dbGuid;
        void* dbPtr;

        ocrHint_t dbHint;
        ocrHintInit( &dbHint, OCR_HINT_DB_T );
#if GUID_BIT_COUNT == 64
            ocrSetHintValue( & dbHint, OCR_HINT_DB_AFFINITY, affinityMasterPD.guid );
#elif GUID_BIT_COUNT == 128
            ocrSetHintValue( & dbHint, OCR_HINT_DB_AFFINITY, affinityMasterPD.lower );
#else
#error Unknown GUID type
#endif
        ocrDbCreate(&dbGuid, &dbPtr, totalLength,
                    DB_PROP_IGNORE_WARN, &dbHint, NO_ALLOC);
        // copy packed args to DB
        hal_memCopy(dbPtr, packedUserArgv, totalLength, 0);
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
        msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = dbGuid;
        PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(edt.guid) = NULL_GUID;
        PD_MSG_FIELD_I(edt.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
        PD_MSG_FIELD_I(ptr) = NULL;
        PD_MSG_FIELD_I(size) = 0;
        PD_MSG_FIELD_I(properties) = 0;
        RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, true), ==, 0);
#undef PD_MSG
#undef PD_TYPE

        // Prepare the mainEdt for scheduling
        ocrGuid_t edtTemplateGuid = NULL_GUID, edtGuid = NULL_GUID;
        ocrEdtTemplateCreate(&edtTemplateGuid, mainEdt, 0, 1);

        ocrHint_t edtHint;
        ocrHintInit( &edtHint, OCR_HINT_EDT_T );
#if GUID_BIT_COUNT == 64
            ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, affinityMasterPD.guid );
#elif GUID_BIT_COUNT == 128
            ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, affinityMasterPD.lower );
#else
#error Unknown GUID type
#endif
        ocrEdtCreate(&edtGuid, edtTemplateGuid, EDT_PARAM_DEF, /* paramv = */ NULL,
                     /* depc = */ EDT_PARAM_DEF, /* depv = */ &dbGuid,
                     GUID_PROP_TORECORD, &edtHint, NULL);
        // Once mainEdt is created, its template is no longer needed
        ocrEdtTemplateDestroy(edtTemplateGuid);
    }
    // Actual loop
    do {
        while(worker->curState == worker->desiredState) {
            START_PROFILE(wo_hc_workerLoop);
            worker->fcts.workShift(worker);
            EXIT_PROFILE;
        }
        DPRINTF(DEBUG_LVL_VERB, "Worker %"PRIu64" dropped out of curState(%"PRIu32",%"PRIu32") going to desiredState(%"PRIu32",%"PRIu32")\n", worker->id,
                                GET_STATE_RL(worker->curState), GET_STATE_PHASE(worker->curState),
                                GET_STATE_RL(worker->desiredState), GET_STATE_PHASE(worker->desiredState));

        // Here we are shifting to another runlevel or phase
        switch(GET_STATE_RL(worker->desiredState)) {
        case RL_USER_OK: {
            u8 desiredPhase = GET_STATE_PHASE(worker->desiredState);
            // Should never fall-through here if there has been no transition
            ASSERT(desiredPhase != RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK));
            ASSERT(worker->callback != NULL);
            worker->curState = GET_STATE(RL_USER_OK, desiredPhase);
            // Callback the PD, but keep working
            worker->callback(worker->pd, worker->callbackArg);
            // Warning: Code potentially concurrent with switchRunlevel
            break;
        }
        case RL_COMPUTE_OK: {
            u8 phase = GET_STATE_PHASE(worker->desiredState);
            if(RL_IS_FIRST_PHASE_DOWN(worker->pd, RL_COMPUTE_OK, phase)) {
                DPRINTF(DEBUG_LVL_VERB, "Noticed transition to RL_COMPUTE_OK\n");
                // We first change our state prior to the callback
                // because we may end up doing some of the callback processing
                worker->curState = worker->desiredState;
                if(worker->callback != NULL) {
                    worker->callback(worker->pd, worker->callbackArg);
                }
                // There is no need to do anything else except quit
                continueLoop = false;
            } else {
                ASSERT(0);
            }
            break;
        }
        default:
            // Only these two RL should occur
            ASSERT(0);
        }
    } while(continueLoop);
    DPRINTF(DEBUG_LVL_VERB, "Finished worker loop ... waiting to be reapped\n");
}

void destructWorkerHc(ocrWorker_t * base) {
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
}

u8 hcWorkerSwitchRunlevel(ocrWorker_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                          phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t *, u64), u64 val) {

    u8 toReturn = 0;

    // Verify properties
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    // Call the runlevel change on the underlying platform
    if(runlevel == RL_CONFIG_PARSE && (properties & RL_BRING_UP) && phase == 0) {
        // Set the worker properly the first time
        ASSERT(self->computeCount == 1);
        self->computes[0]->worker = self;
    }
    // Even if we have a callback, we make things synchronous for the computes
    if(runlevel != RL_COMPUTE_OK) {
        // For compute OK, we need to do things BEFORE calling this
        toReturn |= self->computes[0]->fcts.switchRunlevel(self->computes[0], PD, runlevel, phase, properties,
                                                           NULL, 0);
    }
    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_CONFIG_PARSE, phase)) {
                // This worker implementation supports an arbitrary number
                // of down phases in RL_USER_OK. Each phase goes into the work loop.
                // We need at least two phases for the RL_COMPUTE_OK TEAR_DOWN
                RL_ENSURE_PHASE_DOWN(PD, RL_COMPUTE_OK, RL_PHASE_WORKER, 2);
            } else if(RL_IS_LAST_PHASE_UP(PD, RL_CONFIG_PARSE, phase)) {
                // We check that the compute and user phases have the right
                // count. We currently only support one user phase and two
                // compute phase. If this changes, the workerLoop code and hcWorkerRun
                // code will have to be modified (as well as this code of course)
                if(RL_GET_PHASE_COUNT_UP(PD, RL_COMPUTE_OK) != 1 ||
                   RL_GET_PHASE_COUNT_DOWN(PD, RL_COMPUTE_OK) != 2 ||
                   RL_GET_PHASE_COUNT_UP(PD, RL_USER_OK) != 1 ||
                   RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK) != 1) {
                    DPRINTF(DEBUG_LVL_WARN, "Worker does not support compute and user counts\n");
                    ASSERT(0);
                }
            }
        }
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP)
            self->pd = PD;
        break;
    case RL_MEMORY_OK:
        //Check that OCR has been configured to utilize system worker.
        //worker[n-1] by convention. If so initialize deques
        if(PD->workers[(PD->workerCount)-1]->type == SYSTEM_WORKERTYPE){
            if(self->type == MASTER_WORKERTYPE || self->type == SLAVE_WORKERTYPE) {
                if(((ocrWorkerHc_t *)self)->sysDeque == NULL){
                    ((ocrWorkerHc_t*)self)->sysDeque = newDeque(self->pd, NULL, NON_CONCURRENT_DEQUE);
                }
            }
        }
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            // Guidify ourself
            guidify(self->pd, (u64)self, &(self->fguid), OCR_GUID_WORKER);
            // We need a way to inform the PD
            ASSERT(callback != NULL);
            self->curState = GET_STATE(RL_MEMORY_OK, 0); // Technically last phase of memory OK but doesn't really matter
            self->desiredState = GET_STATE(RL_COMPUTE_OK, phase);
            self->location = (u64) self; // Currently used only by visualizer, value is not important as long as it's unique

            // See if we are blessed
            self->amBlessed = (properties & RL_BLESSED) != 0;
            if(!(properties & RL_PD_MASTER)) {
                self->callback = callback;
                self->callbackArg = val;
                hal_fence();
                toReturn |= self->computes[0]->fcts.switchRunlevel(self->computes[0], PD, runlevel, phase, properties,
                                                                   NULL, 0);
            } else {
                // First set our current environment (this is usually done by the new thread's startup code)
                self->computes[0]->fcts.setCurrentEnv(self->computes[0], self->pd, self);
                // We just directly call the callback after switching our underlying target
                toReturn |= self->computes[0]->fcts.switchRunlevel(self->computes[0], PD, runlevel, phase, properties,
                                                                   NULL, 0);
                callback(self->pd, val);
                self->curState = GET_STATE(RL_COMPUTE_OK, 0);
            }
        }
        if((properties & RL_TEAR_DOWN)) {
            toReturn |= self->computes[0]->fcts.switchRunlevel(self->computes[0], PD, runlevel, phase, properties,
                                                               NULL, 0);
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
                // Destroy GUID
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
                msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(guid) = self->fguid;
                PD_MSG_FIELD_I(properties) = 0;
                toReturn |= self->pd->fcts.processMessage(self->pd, &msg, false);
                self->fguid.guid = NULL_GUID;
#undef PD_MSG
#undef PD_TYPE
                // At this stage, only the RL_PD_MASTER should be actually
                // capable
                DPRINTF(DEBUG_LVL_VERB, "Last phase in RL_COMPUTE_OK DOWN for %p (am PD master: %"PRId32")\n",
                    self, properties & RL_PD_MASTER);
                self->desiredState = self->curState = GET_STATE(RL_COMPUTE_OK, phase);
            } else if(RL_IS_FIRST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
                ASSERT(self->curState == GET_STATE(RL_USER_OK, 0));
                ASSERT(callback != NULL);
                self->callback = callback;
                self->callbackArg = val;
                hal_fence();
                self->desiredState = GET_STATE(RL_COMPUTE_OK, phase);
            } else {
                ASSERT(false && "Unexpected phase on runlevel RL_COMPUTE_OK teardown");
            }
        }
        break;
    case RL_USER_OK:
        if((properties & RL_BRING_UP)) {
            if(RL_IS_LAST_PHASE_UP(PD, RL_USER_OK, phase)) {
                if(!(properties & RL_PD_MASTER)) {
                    // No callback required on the bring-up
                    self->callback = NULL;
                    self->callbackArg = 0ULL;
                    hal_fence();
                    self->desiredState = GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK))); // We put ourself one past
                    // so that we can then come back down when shutting down
                } else {
                    // At this point, the original capable thread goes to work
                    self->curState = GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK)));
                    if (!((ocrWorkerHc_t*) self)->legacySecondStart) {
                        self->desiredState = self->curState;
                        if (properties & RL_LEGACY) {
                            // amBlessed was set to true when the runtime is brought up in COMPUTE_OK
                            // but it is not known whether we are in legacy mode or not at that point.
                            // There's no blessed worker in legacy mode, flip to false so that
                            // the master thread legacy's second start does not try to execute a mainEdt.
                            self->amBlessed = false;
                        }
                        ((ocrWorkerHc_t*) self)->legacySecondStart = true;
                    }
                    if (!(properties & RL_LEGACY)) {
                        workerLoop(self);
                    }
                }
            }
        }
        if (properties & RL_TEAR_DOWN) {
            if (RL_IS_FIRST_PHASE_DOWN(PD, RL_USER_OK, phase)) {
                // We make sure that we actually fully booted before shutting down.
                // Addresses a race where a worker still hasn't started but
                // another worker has started and executes the shutdown protocol
                //while(self->curState != GET_STATE(RL_USER_OK, (phase+1))){
                while(self->curState != GET_STATE(RL_USER_OK, (phase+1)));
                ASSERT(self->curState == GET_STATE(RL_USER_OK, (phase+1)));
            }

            // Transition to the next phase
            ASSERT(GET_STATE_PHASE(self->curState) == (phase+1));
            ASSERT(callback != NULL);
            self->callback = callback;
            self->callbackArg = val;
            hal_fence();
            // Breaks the worker's compute loop
            self->desiredState = GET_STATE(RL_USER_OK, phase);
        }
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void* hcRunWorker(ocrWorker_t * worker) {
    // At this point, we should have a callback to inform the PD
    // that we have successfully achieved the RL_COMPUTE_OK RL
    ASSERT(worker->callback != NULL);
    worker->callback(worker->pd, worker->callbackArg);
    // Set the current environment
    worker->computes[0]->fcts.setCurrentEnv(worker->computes[0], worker->pd, worker);
    worker->curState = GET_STATE(RL_COMPUTE_OK, 0);

    // We wait until we transition to the next RL
    while(worker->curState == worker->desiredState) ;

    // At this point, we should be going to RL_USER_OK
    ASSERT(worker->desiredState == GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK))));

    // Start the worker loop
    worker->curState = worker->desiredState;
    workerLoop(worker);
    // Worker loop will transition back down to RL_COMPUTE_OK last phase

    ASSERT((worker->curState == worker->desiredState) &&
            (worker->curState == GET_STATE(RL_COMPUTE_OK, (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_COMPUTE_OK) - 1))));
    return NULL;
}

bool hcIsRunningWorker(ocrWorker_t * base) {
    // BUG #583: This states that we are in USER mode. Do we want to include RL_COMPUTE_OK?
    return (base->curState == GET_STATE(RL_USER_OK, 0));
}

void hcPrintLocation(ocrWorker_t *base, char* location) {
    SNPRINTF(location, 32, "Worker 0x%"PRIx64"", base->location);
}

/**
 * @brief Builds an instance of a HC worker
 */
ocrWorker_t* newWorkerHc(ocrWorkerFactory_t * factory, ocrParamList_t * perInstance) {
    ocrWorker_t * worker = (ocrWorker_t*) runtimeChunkAlloc( sizeof(ocrWorkerHc_t), PERSISTENT_CHUNK);
    factory->initialize(factory, worker, perInstance);
    return worker;
}

/**
 * @brief Initialize an instance of a HC worker
 */
void initializeWorkerHc(ocrWorkerFactory_t * factory, ocrWorker_t* self, ocrParamList_t * perInstance) {
    initializeWorkerOcr(factory, self, perInstance);
    self->type = ((paramListWorkerHcInst_t*)perInstance)->workerType;
#ifdef OCR_ASSERT
    u64 workerId = ((paramListWorkerInst_t*)perInstance)->workerId;
    //TODO: try to get away from SYSTEM_WORKERTYPE and remove this check.
    if (self->type !=  SYSTEM_WORKERTYPE)
        ASSERT((workerId && self->type == SLAVE_WORKERTYPE) ||
           (workerId == 0 && self->type == MASTER_WORKERTYPE));
#endif
    ocrWorkerHc_t * workerHc = (ocrWorkerHc_t*) self;

    if(self->type == SYSTEM_WORKERTYPE){
        workerHc->hcType = HC_WORKER_SYSTEM;
    }else{
        workerHc->hcType = HC_WORKER_COMP;
    }
    workerHc->legacySecondStart = false;
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    workerHc->isHelping = 0;
    workerHc->stealFirst = 0;
#endif
}

/******************************************************/
/* OCR-HC WORKER FACTORY                              */
/******************************************************/

void destructWorkerFactoryHc(ocrWorkerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrWorkerFactory_t * newOcrWorkerFactoryHc(ocrParamList_t * perType) {
    ocrWorkerFactory_t* base = (ocrWorkerFactory_t*)runtimeChunkAlloc(sizeof(ocrWorkerFactoryHc_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newWorkerHc;
    base->initialize = &initializeWorkerHc;
    base->destruct = &destructWorkerFactoryHc;

    base->workerFcts.destruct = FUNC_ADDR(void (*) (ocrWorker_t *), destructWorkerHc);
    base->workerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                       phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcWorkerSwitchRunlevel);
    base->workerFcts.run = FUNC_ADDR(void* (*) (ocrWorker_t *), hcRunWorker);
    base->workerFcts.workShift = FUNC_ADDR(void* (*) (ocrWorker_t *), hcWorkShift);
    base->workerFcts.isRunning = FUNC_ADDR(bool (*) (ocrWorker_t *), hcIsRunningWorker);
    base->workerFcts.printLocation = FUNC_ADDR(void (*)(ocrWorker_t*, char*), hcPrintLocation);
    return base;
}

#endif /* ENABLE_WORKER_HC */
