/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKER_HC_COMM

#include "debug.h"
#include "ocr-db.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-worker.h"
#include "worker/hc/hc-worker.h"
#include "worker/hc-comm/hc-comm-worker.h"
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

//TODO-MD-MT These should become micro-tasks
extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

/******************************************************/
/* OCR-HC COMMUNICATION WORKER                        */
/* Extends regular HC workers                         */
/******************************************************/
#ifdef UTASK_COMM
static u8 createUTask(ocrPolicyDomain_t * pd, ocrPolicyMsg_t * msg) {
    pdEvent_t * pdEvent;
    RESULT_ASSERT(pdCreateEvent(pd, &pdEvent, PDEVT_TYPE_MSG, 0), ==, 0);
    // We don't destroy deep for now because of compatibility with the
    // processIncomingMsg call that does the free of the message
    pdEvent->properties |= PDEVT_GC /*| PDEVT_DESTROY_DEEP*/;
    ((pdEventMsg_t *) pdEvent)->msg = msg;
    DPRINTF(DEBUG_LVL_VERB, "Created micro-task from incoming comm: %p\n", pdEvent);
    RESULT_ASSERT(pdMarkReadyEvent(pd, pdEvent), ==, 0);
    pdStrand_t * msgStrand;
    RESULT_ASSERT(pdGetNewStrand(pd, &msgStrand, pd->strandTables[PDSTT_COMM-1], pdEvent, 0 /*unused*/), ==, 0);
    pdAction_t * processAction = pdGetProcessMessageAction(NP_WORK);
    RESULT_ASSERT(pdEnqueueActions(pd, msgStrand, 1, &processAction, true/*clear hold*/), ==, 0);
    RESULT_ASSERT(pdUnlockStrand(msgStrand), ==, 0);
    return 0;
}

#else

static u8 createProcessRequestEdt(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid, u64 * paramv) {

    u32 paramc = 1;
    u32 depc = 0;
    u32 properties = 0;
    ocrWorkType_t workType = EDT_RT_WORKTYPE;

    START_PROFILE(api_EdtCreate);
    PD_MSG_STACK(msg);
    u8 returnCode = 0;
    ocrTask_t *curEdt = NULL;
    getCurrentEnv(NULL, NULL, &curEdt, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_CREATE
    msg.type = PD_MSG_WORK_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(outputEvent.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(outputEvent.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(paramc) = paramc;
    PD_MSG_FIELD_IO(depc) = depc;
    PD_MSG_FIELD_I(templateGuid.guid) = templateGuid;
    PD_MSG_FIELD_I(templateGuid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(hint) = NULL_HINT;
    // This is a "fake" EDT so it has no "parent"
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(parentLatch.guid) = NULL_GUID;
    PD_MSG_FIELD_I(parentLatch.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(paramv) = paramv;
    PD_MSG_FIELD_I(depv) = NULL;
    PD_MSG_FIELD_I(workType) = workType;
    PD_MSG_FIELD_I(properties) = properties;
    returnCode = pd->fcts.processMessage(pd, &msg, true);
    if(returnCode) {
        DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Created processRequest EDT GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
        RETURN_PROFILE(returnCode);
    }

    RETURN_PROFILE(0);
#undef PD_MSG
#undef PD_TYPE
}

#endif /* UTASK_COMM */

static u8 takeFromSchedulerAndSend(ocrWorker_t * worker, ocrPolicyDomain_t * pd) {
    // When the communication-worker is not stopping only a single iteration is
    // executed. Otherwise it is executed until the scheduler's 'take' do not
    // return any more work.
    ocrMsgHandle_t * outgoingHandle = NULL;
    PD_MSG_STACK(msgCommTake);
    u8 ret = 0;
    getCurrentEnv(NULL, NULL, NULL, &msgCommTake);
    ocrFatGuid_t handlerGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
    //IMPL: MSG_SCHED_GET_WORK implementation must be consistent across PD, Scheduler and Worker.
    // We expect the PD to fill-in the guids pointer with an ocrMsgHandle_t pointer
    // to be processed by the communication worker or NULL.
    //PERF: could request 'n' for internal comm load balancing (outgoing vs pending vs incoming).
#define PD_MSG (&msgCommTake)
#define PD_TYPE PD_MSG_SCHED_GET_WORK
    msgCommTake.type = PD_MSG_SCHED_GET_WORK | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_COMM;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guids = &handlerGuid;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount = 1;
    ret = pd->fcts.processMessage(pd, &msgCommTake, true);
    if (!ret && (PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount != 0)) {
        ASSERT(PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount == 1); //LIMITATION: single guid returned by comm take
        ocrFatGuid_t handlerGuid = PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guids[0];
        ASSERT(handlerGuid.metaDataPtr != NULL);
        outgoingHandle = (ocrMsgHandle_t *) handlerGuid.metaDataPtr;
#undef PD_MSG
#undef PD_TYPE
        if (outgoingHandle != NULL) {
            // This code handles the pd's outgoing messages. They can be requests or responses.
            DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: outgoing handle comm take successful handle=%p, msg=%p type=0x%"PRIx32"\n",
                    outgoingHandle, outgoingHandle->msg, outgoingHandle->msg->type);
            //We can never have an outgoing handle with the response ptr set because
            //when we process an incoming request, we lose the handle by calling the
            //pd's process message. Hence, a new handle is created for one-way response.
            ASSERT(outgoingHandle->response == NULL);
            u32 properties = outgoingHandle->properties;
            ASSERT(properties & PERSIST_MSG_PROP);
            //BUG #587 design: Not sure where to draw the line between one-way with/out ack implementation
            //If the worker was not aware of the no-ack policy, is it ok to always give a handle
            //and the comm-api contract is to at least set the HDL_SEND_OK flag ?
            ocrMsgHandle_t ** sendHandle = ((properties & TWOWAY_MSG_PROP) && !(properties & ASYNC_MSG_PROP))
                ? &outgoingHandle : NULL;
            ASSERT((outgoingHandle->msg->srcLocation == pd->myLocation) &&
                   (outgoingHandle->msg->destLocation != pd->myLocation));
            //BUG #587 design: who's responsible for deallocating the handle ?
            //If the message is two-way, the creator of the handle is responsible for deallocation
            //If one-way, the comm-layer disposes of the handle when it is not needed anymore
            //=> Sounds like if an ack is expected, caller is responsible for dealloc, else callee
            pd->fcts.sendMessage(pd, outgoingHandle->msg->destLocation, outgoingHandle->msg, sendHandle, properties);

            // This is contractual for now. It recycles the handler allocated in the delegate-comm-api:
            // - Sending a request one-way or a response (always non-blocking): The delegate-comm-api
            //   creates the handle merely to be able to give it to the scheduler. There's no use of the
            //   handle beyond this point.
            // - The runtime does not implement blocking one-way. Hence, the callsite of the original
            //   send message did not ask for a handler to be returned.
            if (sendHandle == NULL) {
                outgoingHandle->destruct(outgoingHandle);
            }

            //Communication is posted. If TWOWAY, subsequent calls to poll may return the response
            //to be processed
            return POLL_MORE_MESSAGE;
        }
    }
    return POLL_NO_MESSAGE;
}

static void workerLoopHcCommInternal(ocrWorker_t * worker, ocrPolicyDomain_t *pd, ocrGuid_t processRequestTemplate, bool flushOutgoingComm) {
    // In outgoing flush mode:
    // - Send all outgoing communications
    // - Loop until pollMessage says there's no more outgoing
    //   messages to be processed by the underlying comm-platform.
    // In regular mode:
    // - Try to take from the scheduler and send outgoing communication
    // - Poll to receive an incoming communication
    u8 ret;
    do {
        ret = takeFromSchedulerAndSend(worker, pd);
    } while (flushOutgoingComm && (ret == POLL_MORE_MESSAGE));

    do {
        ocrMsgHandle_t * handle = NULL;
        ret = pd->fcts.pollMessage(pd, &handle);
        if (ret == POLL_MORE_MESSAGE) {
            //IMPL: for now only support successful polls on incoming request and responses
            ASSERT((handle->status == HDL_RESPONSE_OK)||(handle->status == HDL_NORMAL));
            ocrPolicyMsg_t * message = (handle->status == HDL_RESPONSE_OK) ? handle->response : handle->msg;
            //To catch misuses, assert src is not self and dst is self
            ASSERT((message->srcLocation != pd->myLocation) && (message->destLocation == pd->myLocation));
            // Poll a response to a message we had sent.
            if ((message->type & PD_MSG_RESPONSE) && !(handle->properties & ASYNC_MSG_PROP)) {
                DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Received message response for msgId: %"PRId64"\n",  message->msgId); // debug
                // Someone is expecting this response, give it back to the PD
                ocrFatGuid_t fatGuid;
                fatGuid.guid = NULL_GUID;
                fatGuid.metaDataPtr = handle;
                PD_MSG_STACK(giveMsg);
                getCurrentEnv(NULL, NULL, NULL, &giveMsg);
            #define PD_MSG (&giveMsg)
            #define PD_TYPE PD_MSG_SCHED_NOTIFY
                giveMsg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
                PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_COMM_READY;
                PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_COMM_READY).guid = fatGuid;
                RESULT_ASSERT(pd->fcts.processMessage(pd, &giveMsg, false), ==, 0);
            #undef PD_MSG
            #undef PD_TYPE
                //For now, assumes all the responses are for workers that are
                //waiting on the response handler provided by sendMessage, reusing
                //the request msg as an input buffer for the response.
            } else {
                ASSERT((message->type & PD_MSG_REQUEST) || ((message->type & PD_MSG_RESPONSE) && (handle->properties & ASYNC_MSG_PROP)));
                // else it's a request or a response with ASYNC_MSG_PROP set (i.e. two-way but asynchronous handling of response).
                DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Received message, msgId: %"PRId64" type:0x%"PRIx32" prop:0x%"PRIx64"\n",
                                        message->msgId, message->type, handle->properties);
                // This is an outstanding request, delegate to PD for processing
                u64 msgParamv = (u64) message;
            #ifdef HYBRID_COMM_COMP_WORKER // Experimental see documentation
                // Execute selected 'sterile' messages on the spot
                if ((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) {
                    DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Execute message, msgId: %"PRId64"\n", pd->myLocation, message->msgId);
                    processRequestEdt(1, &msgParamv, 0, NULL);
                } else {
                    #ifdef UTASK_COMM
                    createUTask(pd, message);
                    #else
                    createProcessRequestEdt(pd, processRequestTemplate, &msgParamv);
                    #endif
                }
            #else
                if ((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) {
                    //BUG #190
#define PD_MSG (message)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                    bool blockingAcquire = ((message->type & PD_MSG_RESPONSE) && (PD_MSG_FIELD_IO(edtSlot) == EDT_SLOT_NONE));
#undef PD_MSG
#undef PD_TYPE
                    if (blockingAcquire) {
                        // This going through the PD mecanism to deal with the incoming acquire response
                        // and dequeue EDTs that may be waiting on the acquire.
                        // The PD will not call the acquire callback because there's none in that case.
                        processRequestEdt(1, &msgParamv, 0, NULL);
                        // This is to unblock the calling blocked on the acquire
                        ocrFatGuid_t fatGuid;
                        fatGuid.guid = NULL_GUID;
                        fatGuid.metaDataPtr = handle;
                        PD_MSG_STACK(giveMsg);
                        getCurrentEnv(NULL, NULL, NULL, &giveMsg);
                    #define PD_MSG (&giveMsg)
                    #define PD_TYPE PD_MSG_SCHED_NOTIFY
                        giveMsg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
                        PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_COMM_READY;
                        PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_COMM_READY).guid = fatGuid;
                        RESULT_ASSERT(pd->fcts.processMessage(pd, &giveMsg, false), ==, 0);
                    #undef PD_MSG
                    #undef PD_TYPE
                    } else {
                        #ifdef UTASK_COMM
                        createUTask(pd, message);
                        #else
                        createProcessRequestEdt(pd, processRequestTemplate, &msgParamv);
                        #endif
                        // We do not need the handle anymore
                        handle->destruct(handle);
                    }
                } else {
#ifdef COMMWRK_PROCESS_SATISFY // This is for benchmarking purpose to measure overhead of delegating processing
                    if ((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DEP_SATISFY) {
                        DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Process PD_MSG_DEP_SATISFY message, type=0x%"PRIx64" msgId: %"PRIu64"\n",  message->type, message->msgId);
                        processRequestEdt(1, &msgParamv, 0, NULL);
                    } else {
#endif
                        #ifdef UTASK_COMM
                        createUTask(pd, message);
                        #else
                        createProcessRequestEdt(pd, processRequestTemplate, &msgParamv);
                        #endif
#ifdef COMMWRK_PROCESS_SATISFY
                    }
#endif
                    // We do not need the handle anymore
                    handle->destruct(handle);
                }
            #endif
                //BUG #587: depending on comm-worker implementation, the received message could
                //then be 'wrapped' in an EDT and pushed to the deque for load-balancing purpose.
            }
        }
    } while (flushOutgoingComm && !(ret & POLL_NO_OUTGOING_MESSAGE));
}

static void workShiftHcComm(ocrWorker_t * worker) {
    ocrWorkerHcComm_t * rworker = (ocrWorkerHcComm_t *) worker;
    workerLoopHcCommInternal(worker, worker->pd, rworker->processRequestTemplate, rworker->flushOutgoingComm);
}

static void workerLoopHcComm(ocrWorker_t * worker) {
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
            ocrHintSetValue( & dbHint, OCR_HINT_DB_AFFINITY, (ocrHintVal_t){ .u64Val = affinityMasterPD.guid });
#elif GUID_BIT_COUNT == 128
            ocrHintSetValue( & dbHint, OCR_HINT_DB_AFFINITY, (ocrHintVal_t){ .u64Val = affinityMasterPD.lower });
#else
#error Unknown GUID type
#endif

        ocrDbCreate(&dbGuid, &dbPtr, totalLength,
                    DB_PROP_IGNORE_WARN, &dbHint, NO_ALLOC);

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
            ocrHintSetValue( & edtHint, OCR_HINT_EDT_AFFINITY, (ocrHintVal_t){ .u64Val = affinityMasterPD.guid });
#elif GUID_BIT_COUNT == 128
            ocrHintSetValue( & edtHint, OCR_HINT_EDT_AFFINITY, (ocrHintVal_t){ .u64Val = affinityMasterPD.lower });
#else
#error Unknown GUID type
#endif
        ocrEdtCreate(&edtGuid, edtTemplateGuid, EDT_PARAM_DEF, /* paramv = */ NULL,
                     /* depc = */ EDT_PARAM_DEF, /* depv = */ &dbGuid,
                     EDT_PROP_NONE, &edtHint, NULL);
    }

    ASSERT(worker->curState == GET_STATE(RL_USER_OK, PHASE_RUN));
    // Setup the template EDT for asynchronous processing of incoming communications
    ocrWorkerHcComm_t * rworker = (ocrWorkerHcComm_t *) worker;
    ocrEdtTemplateCreate(&(rworker->processRequestTemplate), &processRequestEdt, 1, 0);
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

u8 hcCommWorkerSwitchRunlevel(ocrWorker_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                              phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t *, u64), u64 val) {
    u8 toReturn = 0;
    // Verify properties
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    ocrWorkerHcComm_t * commWorker = (ocrWorkerHcComm_t *) self;

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
                    workerLoopHcComm(self);
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
void* runWorkerHcComm(ocrWorker_t * worker) {
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
    workerLoopHcComm(worker);
    // Worker loop will transition back down to RL_COMPUTE_OK last phase
    ASSERT((worker->curState == worker->desiredState) &&
           (worker->curState == GET_STATE(RL_COMPUTE_OK, RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_COMPUTE_OK) - 1)));
    return NULL;
}

/**
 * Builds an instance of a HC Communication worker
 */
ocrWorker_t* newWorkerHcComm(ocrWorkerFactory_t * factory, ocrParamList_t * perInstance) {
    ocrWorker_t * worker = (ocrWorker_t*)
            runtimeChunkAlloc(sizeof(ocrWorkerHcComm_t), PERSISTENT_CHUNK);
    factory->initialize(factory, worker, perInstance);
    return (ocrWorker_t *) worker;
}

void initializeWorkerHcComm(ocrWorkerFactory_t * factory, ocrWorker_t *self, ocrParamList_t *perInstance) {
    ocrWorkerFactoryHcComm_t * derivedFactory = (ocrWorkerFactoryHcComm_t *) factory;
    derivedFactory->baseInitialize(factory, self, perInstance);
    // Override base's default value
    ocrWorkerHc_t * workerHc = (ocrWorkerHc_t *) self;
    workerHc->hcType = HC_WORKER_COMM;
    // Initialize comm worker's members
    ocrWorkerHcComm_t * workerHcComm = (ocrWorkerHcComm_t *) self;
    workerHcComm->baseSwitchRunlevel = derivedFactory->baseSwitchRunlevel;
    workerHcComm->processRequestTemplate = NULL_GUID;
    workerHcComm->flushOutgoingComm = false;
}

/******************************************************/
/* OCR-HC COMMUNICATION WORKER FACTORY                */
/******************************************************/

void destructWorkerFactoryHcComm(ocrWorkerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrWorkerFactory_t * newOcrWorkerFactoryHcComm(ocrParamList_t * perType) {
    ocrWorkerFactory_t * baseFactory = newOcrWorkerFactoryHc(perType);
    ocrWorkerFcts_t baseFcts = baseFactory->workerFcts;

    ocrWorkerFactoryHcComm_t* derived = (ocrWorkerFactoryHcComm_t*)runtimeChunkAlloc(sizeof(ocrWorkerFactoryHcComm_t), NONPERSISTENT_CHUNK);
    ocrWorkerFactory_t * base = (ocrWorkerFactory_t *) derived;
    base->instantiate = FUNC_ADDR(ocrWorker_t* (*)(ocrWorkerFactory_t*, ocrParamList_t*), newWorkerHcComm);
    base->initialize  = FUNC_ADDR(void (*)(ocrWorkerFactory_t*, ocrWorker_t*, ocrParamList_t*), initializeWorkerHcComm);
    base->destruct    = FUNC_ADDR(void (*)(ocrWorkerFactory_t*), destructWorkerFactoryHcComm);

    // Store function pointers we need from the base implementation
    derived->baseInitialize = baseFactory->initialize;
    derived->baseSwitchRunlevel = baseFcts.switchRunlevel;

    // Copy base's function pointers
    base->workerFcts = baseFcts;
    // Specialize comm functions
    base->workerFcts.run = FUNC_ADDR(void* (*)(ocrWorker_t*), runWorkerHcComm);
    base->workerFcts.workShift = FUNC_ADDR(void* (*) (ocrWorker_t *), workShiftHcComm);
    base->workerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                       phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcCommWorkerSwitchRunlevel);
    baseFactory->destruct(baseFactory);
    return base;
}

#endif /* ENABLE_WORKER_HC_COMM */
