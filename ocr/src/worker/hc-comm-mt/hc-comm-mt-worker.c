/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKER_HC_COMM_MT

#include "debug.h"
#include "ocr-db.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-worker.h"
#include "worker/hc/hc-worker.h"
#include "worker/hc-comm-mt/hc-comm-mt-worker.h"
#include "ocr-errors.h"
#include "ocr-policy-domain-tasks.h"

// Load the affinities
#include "experimental/ocr-platform-model.h"
#include "extensions/ocr-affinity.h"
#include "extensions/ocr-hints.h"

#define DEBUG_TYPE WORKER

// Phase numbering is ad-hoc. We just know the last phase of
// RL_USER_OK going down is zero.
#define PHASE_RUN ((u8) 3)
#define PHASE_COMP_QUIESCE ((u8) 2)
#define PHASE_COMM_QUIESCE ((u8) 1)
#define PHASE_DONE ((u8) 0)


/******************************************************/
/* OCR-HC COMMUNICATION WORKER                        */
/* Extends regular HC workers                         */
/******************************************************/
static u8 createUTask(ocrPolicyDomain_t * pd, pdEvent_t * evt) {
    pdStrand_t * msgStrand;
    RESULT_ASSERT(pdGetNewStrand(pd, &msgStrand, pd->strandTables[PDSTT_COMM-1], evt, 0 /*unused*/), ==, 0);
    pdAction_t * processAction = pdGetProcessMessageAction(NP_WORK);
    RESULT_ASSERT(pdEnqueueActions(pd, msgStrand, 1, &processAction, true/*clear hold*/), ==, 0);
    RESULT_ASSERT(pdUnlockStrand(msgStrand), ==, 0);
    return 0;
}

extern u8 processIncomingMsg(ocrPolicyDomain_t * pd, ocrPolicyMsg_t * msg);

static void workerLoopHcCommMTInternal(ocrWorker_t * worker, ocrPolicyDomain_t *pd, bool flushOutgoingComm) {
    // In outgoing flush mode:
    // - Send all messages that we need to send
    // - Loop until there are not more outgoing messages to be processed
    //   by the underlying comm-platform
    // In regular mode:
    // - Check if there is work to be done (to send outgoing messages
    //   from the workers)
    // - Poll for any incoming communication (either a response or an unsolicited message)
    //TODO-MT-COMM: I don't think this is guaranteeing all the sends are completed
    u32 count = 0;
    do {
        count = 1;
        RESULT_ASSERT(pdProcessNStrands(pd, NP_COMM, &count, 0), ==, 0);
    } while (flushOutgoingComm && count > 0);

    u8 ret = 0;
    do {
        pdEvent_t * evt = NULL;
        ret = pd->fcts.pollMessageMT(pd, &evt, 0);
        if (ret == POLL_MORE_MESSAGE) {
            ASSERT(evt->properties & PDEVT_TYPE_MSG);
            pdEventMsg_t *evtMsg = (pdEventMsg_t*)evt;
            ocrPolicyMsg_t * message __attribute__((unused)) = evtMsg->msg;
            //To catch misuses, assert src is not self and dst is self
            ASSERT((message->srcLocation != pd->myLocation) && (message->destLocation == pd->myLocation));
            // In this case, we only get unsolicited messages because the other ones are not returned at all
            DPRINTF(DEBUG_LVL_VVERB, "Creating a microtask from event %p with msg %p\n", evt, message);
            createUTask(pd, evt);
        }
    } while (flushOutgoingComm && !(ret & POLL_NO_OUTGOING_MESSAGE));
}

static void workShiftHcCommMT(ocrWorker_t * worker) {
    ocrWorkerHcCommMT_t * rworker = (ocrWorkerHcCommMT_t *) worker;
    workerLoopHcCommMTInternal(worker, worker->pd, rworker->flushOutgoingComm);
}

static void workerLoopHcCommMT(ocrWorker_t * worker) {
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
                    DB_PROP_IGNORE_WARN, &dbHint, 1234);

        // copy packed args to DB
        hal_memCopy(dbPtr, packedUserArgv, totalLength, 0);
        // Release the DB so that mainEdt can acquire it.
        // Do not invoke ocrDbRelease to avoid the warning there.
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
        msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = dbGuid;
        PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(edt.guid) = NULL_GUID;
        PD_MSG_FIELD_I(edt.metaDataPtr) = NULL;
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
                     EDT_PROP_NONE, &edtHint, NULL);
    }

    ASSERT(worker->curState == GET_STATE(RL_USER_OK, PHASE_RUN));
    ocrWorkerHcCommMT_t * rworker = (ocrWorkerHcCommMT_t *) worker;
    rworker->flushOutgoingComm = false;
    do {
        // 'communication' loop: take, send / poll, dispatch, execute
        // Double check the setup
        ASSERT(RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK) == PHASE_RUN);
        u8 phase = GET_STATE_PHASE(worker->desiredState);
        if ((phase == PHASE_RUN) ||
            (phase == PHASE_COMP_QUIESCE)) {
            while(worker->curState == worker->desiredState) {
                worker->fcts.workShift(worker);
            }
        } else if (phase == PHASE_COMM_QUIESCE) {
            // All workers in this PD are not executing user EDTs anymore.
            // However, there may still be communication in flight.
            // Two reasons for that:
            // 1- EDTs execution generated asynchronous one-way communications
            //    that need to be flushed out.
            // 2- Other PDs are still communicating with the current PD.
            //    This happens mainly because some runtime work is done in
            //    EDT's epilogue. So the sink EDT has executed but some EDTs
            //    are still wrapping up.

            // Goal of this phase is to make sure this PD is done processing
            // the communication it has generated.
            rworker->flushOutgoingComm = true;
            // This call returns when all outgoing messages are sent.
            worker->fcts.workShift(worker);
            // Done with the quiesce comm action, callback the PD
            worker->curState = GET_STATE(RL_USER_OK, PHASE_COMM_QUIESCE);
            worker->callback(worker->pd, worker->callbackArg);
            // Warning: Code potentially concurrent with switchRunlevel now on

            // The callback triggers the distributed shutdown protocol.
            // The PD intercepts the callback and sends shutdown message to other PDs.
            // When all PDs have answered to the shutdown message, the phase change is enacted.

            // Until that happens, keep working to process communications shutdown generates.
            rworker->flushOutgoingComm = false;
            while(worker->curState == worker->desiredState) {
                worker->fcts.workShift(worker);
            }
            // The comm-worker has been transitioned
            ASSERT(GET_STATE_PHASE(worker->desiredState) == PHASE_DONE);
        } else {
            ASSERT(phase == PHASE_DONE);
            // When the comm-worker quiesce and it already had all its neighbors PD's shutdown msg
            // we need to make sure there's no outgoing messages pending (i.e. a one-way shutdown)
            // for other PDs before wrapping up the user runlevel.
            rworker->flushOutgoingComm = true;
            // This call returns when all outgoing messages are sent.
            worker->fcts.workShift(worker);
            // Phase shouldn't have changed since we haven't done callback yet
            ASSERT(GET_STATE_PHASE(worker->desiredState) == PHASE_DONE);
            worker->curState = GET_STATE(RL_USER_OK, PHASE_DONE);
            worker->callback(worker->pd, worker->callbackArg);
            // Warning: Code potentially concurrent with switchRunlevel now on
            // Need to busy wait until the PD makes workers to transition to
            // the next runlevel. The switch hereafter can then take the correct case.
            // Wait for the comm-worker to transition to the desired state.
            // NOTE: that's a bug in waiting because it will deadlock if the
            //       currently executing worker is the comm-worker (which can't
            //       happen in the current design because they do not process
            //       messages)
            while((worker->curState) == (worker->desiredState))
                ;
            ASSERT(GET_STATE_RL(worker->desiredState) == RL_COMPUTE_OK);
        }

        // Here we are shifting to another RL or Phase
        switch(GET_STATE_RL(worker->desiredState)) {
        case RL_USER_OK: {
            u8 desiredState = worker->desiredState;
            u8 desiredPhase = GET_STATE_PHASE(desiredState);
            ASSERT(desiredPhase != PHASE_RUN);
            ASSERT((desiredPhase == PHASE_COMP_QUIESCE) ||
                    (desiredPhase == PHASE_COMM_QUIESCE) ||
                    (desiredPhase == PHASE_DONE));
            if (desiredPhase == PHASE_COMP_QUIESCE) {
                // No actions to take in this phase.
                // Callback the PD and fall-through to keep working.
                worker->curState = GET_STATE(RL_USER_OK, PHASE_COMP_QUIESCE);
                worker->callback(worker->pd, worker->callbackArg);
                //Warning: The moment this callback is invoked, This code
                //is potentially running concurrently with the last worker
                //going out of PHASE_COMP_QUIESCE. That also means this code
                //is potentially concurrently with 'switchRunlevel' being
                //invoked on this worker, by itself or another worker.
            }
            // - Intentionally fall-through here for PHASE_COMM_QUIESCE.
            //   The comm-worker leads that phase transition.
            // - Keep worker loop alive: MUST use 'desiredState' instead of
            //   'worker->desiredState' to avoid races.
            worker->curState = desiredState;
            break;
        }
        // BEGIN copy-paste from hc-worker code
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
        // END copy-paste from hc-worker code
        default:
            // Only these two RL should occur
            ASSERT(0);
        }
    } while(continueLoop);
    DPRINTF(DEBUG_LVL_VERB, "Finished comm worker loop ... waiting to be reapped\n");
}

u8 hcCommMTWorkerSwitchRunlevel(ocrWorker_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t *, u64), u64 val) {
    u8 toReturn = 0;
    // Verify properties
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    ocrWorkerHcCommMT_t * commWorker = (ocrWorkerHcCommMT_t *) self;

    switch(runlevel) {
    case RL_CONFIG_PARSE:
    case RL_NETWORK_OK:
    case RL_PD_OK:
    case RL_MEMORY_OK:
    case RL_GUID_OK:
    case RL_COMPUTE_OK: {
        commWorker->baseSwitchRunlevel(self, PD, runlevel, phase, properties, callback, val);
        break;
    }
    case RL_USER_OK: {
        // Even if we have a callback, we make things synchronous for the computes
        if(runlevel != RL_COMPUTE_OK) {
            toReturn |= self->computes[0]->fcts.switchRunlevel(self->computes[0], PD, runlevel, phase, properties,
                                                               NULL, 0);
        }
        if((properties & RL_BRING_UP)) {
            if(RL_IS_LAST_PHASE_UP(PD, RL_USER_OK, phase)) {
                if(!(properties & RL_PD_MASTER)) {
                    // No callback required on the bring-up
                    self->callback = NULL;
                    self->callbackArg = 0ULL;
                    hal_fence();
                    self->desiredState = GET_STATE(RL_USER_OK, RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK)); // We put ourself one past
                    // so that we can then come back down when shutting down
                } else {
                    // At this point, the original capable thread goes to work
                    self->curState = self->desiredState = GET_STATE(RL_USER_OK, RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK));
                    workerLoopHcCommMT(self);
                }
            }
        }

        if (properties & RL_TEAR_DOWN) {
            if(phase == PHASE_COMP_QUIESCE) {
                // Transitions from RUN to PHASE_COMP_QUIESCE
                // We make sure that we actually fully booted before shutting down.
                // Addresses a race where a worker still hasn't started but
                // another worker has started and executes the shutdown protocol
                while(self->curState != GET_STATE(RL_USER_OK, (phase + 1)))
                    ;
                ASSERT(self->curState == GET_STATE(RL_USER_OK, (phase + 1)));
                ASSERT((self->curState == self->desiredState));
                ASSERT(callback != NULL);
                self->callback = callback;
                self->callbackArg = val;
                hal_fence();
                self->desiredState = GET_STATE(RL_USER_OK, PHASE_COMP_QUIESCE);
            }

            if(phase == PHASE_COMM_QUIESCE) {
                //Warning: At this point it is not 100% sure the worker has
                //already transitioned to PHASE_COMM_QUIESCE.
                ASSERT((GET_STATE_PHASE(self->curState) == PHASE_COMP_QUIESCE) ||
                       (GET_STATE_PHASE(self->curState) == PHASE_RUN));
                // This is set for sure
                ASSERT(GET_STATE_PHASE(self->desiredState) == PHASE_COMP_QUIESCE);
                ASSERT(callback != NULL);
                self->callback = callback;
                self->callbackArg = val;
                hal_fence();
                // Either breaks the worker's loop from the PHASE_COMP_QUIESCE
                // or is set even before that loop is reached and skip the
                // PHASE_COMP_QUIESCE altogeher, which is fine
                self->desiredState = GET_STATE(RL_USER_OK, PHASE_COMM_QUIESCE);
            }

            //BUG #583: RL Last phase that transitions to another runlevel
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_USER_OK, phase)) {
                ASSERT(phase == PHASE_DONE);
                // We need to break out of the compute loop
                // We need to have a callback for all workers here
                ASSERT(callback != NULL);
                // We make sure that we actually fully booted before shutting down.
                // Addresses a race where a worker still hasn't started but
                // another worker has started and executes the shutdown protocol
                while(self->curState != GET_STATE(RL_USER_OK, (phase+1)))
                    ;
                ASSERT(self->curState == GET_STATE(RL_USER_OK, (phase+1)));

                ASSERT(GET_STATE_RL(self->curState) == RL_USER_OK);
                self->callback = callback;
                self->callbackArg = val;
                hal_fence();
                // Breaks the worker's compute loop
                self->desiredState = GET_STATE(RL_USER_OK, PHASE_DONE);
            }
        }
        break;
    }
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

// NOTE: This is exactly the same as the runWorkerHc beside the call to the different work loop
void* runWorkerHcCommMT(ocrWorker_t * worker) {
    // At this point, we should have a callback to inform the PD
    // that we have successfully achieved the RL_COMPUTE_OK RL
    ASSERT(worker->callback != NULL);
    worker->callback(worker->pd, worker->callbackArg);

    // Set the current environment
    worker->computes[0]->fcts.setCurrentEnv(worker->computes[0], worker->pd, worker);
    worker->curState = GET_STATE(RL_COMPUTE_OK, 0);

    // We wait until we transition to the next RL
    while(worker->curState == worker->desiredState)
        ;

    // At this point, we should be going to RL_USER_OK
    ASSERT(worker->desiredState == GET_STATE(RL_USER_OK, RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK)));

    // Start the worker loop
    worker->curState = worker->desiredState;
    workerLoopHcCommMT(worker);
    // Worker loop will transition back down to RL_COMPUTE_OK last phase
    ASSERT((worker->curState == worker->desiredState) &&
           (worker->curState == GET_STATE(RL_COMPUTE_OK, RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_COMPUTE_OK) - 1)));
    return NULL;
}

/**
 * Builds an instance of a HC Communication worker
 */
ocrWorker_t* newWorkerHcCommMT(ocrWorkerFactory_t * factory, ocrParamList_t * perInstance) {
    ocrWorker_t * worker = (ocrWorker_t*)
            runtimeChunkAlloc(sizeof(ocrWorkerHcCommMT_t), PERSISTENT_CHUNK);
    factory->initialize(factory, worker, perInstance);
    return (ocrWorker_t *) worker;
}

void initializeWorkerHcCommMT(ocrWorkerFactory_t * factory, ocrWorker_t *self, ocrParamList_t *perInstance) {
    ocrWorkerFactoryHcCommMT_t * derivedFactory = (ocrWorkerFactoryHcCommMT_t *) factory;
    derivedFactory->baseInitialize(factory, self, perInstance);
    // Override base's default value
    ocrWorkerHc_t * workerHc = (ocrWorkerHc_t *) self;
    workerHc->hcType = HC_WORKER_COMM;
    // Initialize comm worker's members
    ocrWorkerHcCommMT_t * workerHcComm = (ocrWorkerHcCommMT_t *) self;
    workerHcComm->baseSwitchRunlevel = derivedFactory->baseSwitchRunlevel;
    workerHcComm->flushOutgoingComm = false;
}

/******************************************************/
/* OCR-HC COMMUNICATION WORKER FACTORY                */
/******************************************************/

void destructWorkerFactoryHcCommMT(ocrWorkerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrWorkerFactory_t * newOcrWorkerFactoryHcCommMT(ocrParamList_t * perType) {
    ocrWorkerFactory_t * baseFactory = newOcrWorkerFactoryHc(perType);
    ocrWorkerFcts_t baseFcts = baseFactory->workerFcts;
    ocrWorkerFactoryHcCommMT_t* derived = (ocrWorkerFactoryHcCommMT_t*)runtimeChunkAlloc(sizeof(ocrWorkerFactoryHcCommMT_t), NONPERSISTENT_CHUNK);
    ocrWorkerFactory_t * base = (ocrWorkerFactory_t *) derived;
    base->instantiate = FUNC_ADDR(ocrWorker_t* (*)(ocrWorkerFactory_t*, ocrParamList_t*), newWorkerHcCommMT);
    base->initialize  = FUNC_ADDR(void (*)(ocrWorkerFactory_t*, ocrWorker_t*, ocrParamList_t*), initializeWorkerHcCommMT);
    base->destruct    = FUNC_ADDR(void (*)(ocrWorkerFactory_t*), destructWorkerFactoryHcCommMT);

    // Store function pointers we need from the base implementation
    derived->baseInitialize = baseFactory->initialize;
    derived->baseSwitchRunlevel = baseFcts.switchRunlevel;

    // Copy base's function pointers
    base->workerFcts = baseFcts;
    // Specialize comm functions
    base->workerFcts.run = FUNC_ADDR(void* (*)(ocrWorker_t*), runWorkerHcCommMT);
    base->workerFcts.workShift = FUNC_ADDR(void* (*) (ocrWorker_t *), workShiftHcCommMT);
    base->workerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                       phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcCommMTWorkerSwitchRunlevel);
    baseFactory->destruct(baseFactory);
    return base;
}

#endif /* ENABLE_WORKER_HC_COMM_MT */

