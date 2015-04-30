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

#include "experimental/ocr-placer.h"
#include "extensions/ocr-affinity.h"

#define DEBUG_TYPE WORKER

/******************************************************/
/* OCR-HC COMMUNICATION WORKER                        */
/* Extends regular HC workers                         */
/******************************************************/

ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPolicyMsg_t * msg = (ocrPolicyMsg_t *) paramv[0];
    ocrPolicyDomain_t * pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    // This is meant to execute incoming request and asynchronously processed responses (two-way asynchronous)
    // Regular responses are routed back to requesters by the scheduler and are processed by them.
    ASSERT((msg->type & PD_MSG_REQUEST) ||
        ((msg->type & PD_MSG_RESPONSE) && ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE)));
    // Important to read this before calling processMessage. If the message requires
    // a response, the runtime reuses the request's message to post the response.
    // Hence there's a race between this code and the code posting the response.
    bool processResponse = !!(msg->type & PD_MSG_RESPONSE); // mainly for debug
    // DB_ACQUIRE are potentially asynchronous
    bool syncProcess = ((msg->type & PD_MSG_TYPE_ONLY) != PD_MSG_DB_ACQUIRE);
    bool toBeFreed = !(msg->type & PD_MSG_REQ_RESPONSE);
    DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Process incoming EDT request @ %p of type 0x%x\n", msg, msg->type);
    u8 res = pd->fcts.processMessage(pd, msg, syncProcess);
    DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: [done] Process incoming EDT @ %p request of type 0x%x\n", msg, msg->type);
    // Either flagged or was an asynchronous processing, the implementation should
    // have setup a callback and we can free the request message
    if (toBeFreed || (!syncProcess && (res == OCR_EPEND))) {
        // Makes sure the runtime doesn't try to reuse this message
        // even though it was not supposed to issue a response.
        // If that's the case, this check is racy
        ASSERT(!(msg->type & PD_MSG_RESPONSE) || processResponse);
        DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Deleted incoming EDT request @ %p of type 0x%x\n", msg, msg->type);
        // if request was an incoming one-way we can delete the message now.
        pd->fcts.pdFree(pd, msg);
    }

    return NULL_GUID;
}

static u8 takeFromSchedulerAndSend(ocrPolicyDomain_t * pd) {
    // When the communication-worker is not stopping only a single iteration is
    // executed. Otherwise it is executed until the scheduler's 'take' do not
    // return any more work.
    ocrMsgHandle_t * outgoingHandle = NULL;
    PD_MSG_STACK(msgCommTake);
    u8 ret = 0;
    getCurrentEnv(NULL, NULL, NULL, &msgCommTake);
    ocrFatGuid_t handlerGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
    //IMPL: MSG_COMM_TAKE implementation must be consistent across PD, Scheduler and Worker.
    // We expect the PD to fill-in the guids pointer with an ocrMsgHandle_t pointer
    // to be processed by the communication worker or NULL.
    //PERF: could request 'n' for internal comm load balancing (outgoing vs pending vs incoming).
    #define PD_MSG (&msgCommTake)
    #define PD_TYPE PD_MSG_COMM_TAKE
    msgCommTake.type = PD_MSG_COMM_TAKE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guids) = &handlerGuid;
    PD_MSG_FIELD_IO(extra) = 0; /*unused*/
    PD_MSG_FIELD_IO(guidCount) = 1;
    PD_MSG_FIELD_I(properties) = 0;
    PD_MSG_FIELD_IO(type) = OCR_GUID_COMM;
    ret = pd->fcts.processMessage(pd, &msgCommTake, true);
    if (!ret && (PD_MSG_FIELD_IO(guidCount) != 0)) {
        ASSERT(PD_MSG_FIELD_IO(guidCount) == 1); //LIMITATION: single guid returned by comm take
        ocrFatGuid_t handlerGuid = PD_MSG_FIELD_IO(guids[0]);
        ASSERT(handlerGuid.metaDataPtr != NULL);
        outgoingHandle = (ocrMsgHandle_t *) handlerGuid.metaDataPtr;
    #undef PD_MSG
    #undef PD_TYPE
        if (outgoingHandle != NULL) {
            // This code handles the pd's outgoing messages. They can be requests or responses.
            DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: outgoing handle comm take successful handle=%p, msg=%p type=0x%x\n",
                    outgoingHandle, outgoingHandle->msg, outgoingHandle->msg->type);
            //We can never have an outgoing handle with the response ptr set because
            //when we process an incoming request, we lose the handle by calling the
            //pd's process message. Hence, a new handle is created for one-way response.
            ASSERT(outgoingHandle->response == NULL);
            u32 properties = outgoingHandle->properties;
            ASSERT(properties & PERSIST_MSG_PROP);
            //DIST-TODO design: Not sure where to draw the line between one-way with/out ack implementation
            //If the worker was not aware of the no-ack policy, is it ok to always give a handle
            //and the comm-api contract is to at least set the HDL_SEND_OK flag ?
            ocrMsgHandle_t ** sendHandle = ((properties & TWOWAY_MSG_PROP) && !(properties & ASYNC_MSG_PROP))
                ? &outgoingHandle : NULL;
            //DIST-TODO design: who's responsible for deallocating the handle ?
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

static u8 createProcessRequestEdt(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid, u64 * paramv) {

    ocrGuid_t edtGuid;
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
    PD_MSG_FIELD_I(templateGuid.guid) = templateGuid;
    PD_MSG_FIELD_I(templateGuid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(affinity.guid) = NULL_GUID;
    PD_MSG_FIELD_I(affinity.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(outputEvent.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(outputEvent.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(paramv) = paramv;
    PD_MSG_FIELD_IO(paramc) = paramc;
    PD_MSG_FIELD_IO(depc) = depc;
    PD_MSG_FIELD_I(depv) = NULL;
    PD_MSG_FIELD_I(properties) = properties;
    PD_MSG_FIELD_I(workType) = workType;
    // This is a "fake" EDT so it has no "parent"
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(parentLatch.guid) = NULL_GUID;
    PD_MSG_FIELD_I(parentLatch.metaDataPtr) = NULL;
    returnCode = pd->fcts.processMessage(pd, &msg, true);
    if(returnCode) {
        edtGuid = PD_MSG_FIELD_IO(guid.guid);
        DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Created processRequest EDT GUID 0x%lx\n", edtGuid);
        RETURN_PROFILE(returnCode);
    }

    RETURN_PROFILE(0);
#undef PD_MSG
#undef PD_TYPE
}

//TODO-RL to make the workShift pointer happy
static void workerLoopHcComm_DUMMY() {
    ASSERT(false);
}

static void workerLoopHcCommInternal(ocrWorker_t * worker, ocrGuid_t processRequestTemplate, bool checkEmptyOutgoing) {
    ocrPolicyDomain_t *pd = worker->pd;

    // First, Ask the scheduler if there are any communication to be scheduled and sent them if any.
    takeFromSchedulerAndSend(pd);

    ocrMsgHandle_t * handle = NULL;
    u8 ret = pd->fcts.pollMessage(pd, &handle);
    if (ret == POLL_MORE_MESSAGE) {
        //IMPL: for now only support successful polls on incoming request and responses
        ASSERT((handle->status == HDL_RESPONSE_OK)||(handle->status == HDL_NORMAL));
        ocrPolicyMsg_t * message = (handle->status == HDL_RESPONSE_OK) ? handle->response : handle->msg;
        //To catch misuses, assert src is not self and dst is self
        ASSERT((message->srcLocation != pd->myLocation) && (message->destLocation == pd->myLocation));
        // Poll a response to a message we had sent.
        if ((message->type & PD_MSG_RESPONSE) && !(handle->properties & ASYNC_MSG_PROP)) {
            DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Received message response for msgId: %ld\n",  message->msgId); // debug
            // Someone is expecting this response, give it back to the PD
            ocrFatGuid_t fatGuid;
            fatGuid.metaDataPtr = handle;
            PD_MSG_STACK(giveMsg);
            getCurrentEnv(NULL, NULL, NULL, &giveMsg);
        #define PD_MSG (&giveMsg)
        #define PD_TYPE PD_MSG_COMM_GIVE
            giveMsg.type = PD_MSG_COMM_GIVE | PD_MSG_REQUEST;
            PD_MSG_FIELD_IO(guids) = &fatGuid;
            PD_MSG_FIELD_IO(guidCount) = 1;
            PD_MSG_FIELD_I(properties) = 0;
            PD_MSG_FIELD_I(type) = OCR_GUID_COMM;
            ret = pd->fcts.processMessage(pd, &giveMsg, false);
            ASSERT(ret == 0);
        #undef PD_MSG
        #undef PD_TYPE
            //For now, assumes all the responses are for workers that are
            //waiting on the response handler provided by sendMessage, reusing
            //the request msg as an input buffer for the response.
        } else {
            ASSERT((message->type & PD_MSG_REQUEST) || ((message->type & PD_MSG_RESPONSE) && (handle->properties & ASYNC_MSG_PROP)));
            // else it's a request or a response with ASYNC_MSG_PROP set (i.e. two-way but asynchronous handling of response).
            DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Received message, msgId: %ld type:0x%x prop:0x%x\n",
                                    message->msgId, message->type, handle->properties);
            // This is an outstanding request, delegate to PD for processing
            u64 msgParamv = (u64) message;
        #ifdef HYBRID_COMM_COMP_WORKER // Experimental see documentation
            // Execute selected 'sterile' messages on the spot
            if ((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) {
                DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Execute message, msgId: %ld\n", pd->myLocation, message->msgId);
                processRequestEdt(1, &msgParamv, 0, NULL);
            } else {
                createProcessRequestEdt(pd, processRequestTemplate, &msgParamv);
            }
        #else
            createProcessRequestEdt(pd, processRequestTemplate, &msgParamv);
        #endif
            // We do not need the handle anymore
            handle->destruct(handle);
            //DIST-TODO-3: depending on comm-worker implementation, the received message could
            //then be 'wrapped' in an EDT and pushed to the deque for load-balancing purpose.
        }

    } else {
        //DIST-TODO No messages ready for processing, ask PD for EDT work.
        if (checkEmptyOutgoing && (ret & POLL_NO_OUTGOING_MESSAGE)) {
            return;
        }
    }
}

static void workerLoopHcComm(ocrWorker_t * worker) {
    u8 continueLoop = true;
    // At this stage, we are in the USER_OK runlevel
    ASSERT(worker->curState == ((RL_USER_OK << 16) | (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK))));

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
        ocrDbCreate(&dbGuid, &dbPtr, totalLength,
                    DB_PROP_IGNORE_WARN, affinityMasterPD, NO_ALLOC);
        // copy packed args to DB
        hal_memCopy(dbPtr, packedUserArgv, totalLength, 0);
        // Prepare the mainEdt for scheduling
        ocrGuid_t edtTemplateGuid = NULL_GUID, edtGuid = NULL_GUID;
        ocrEdtTemplateCreate(&edtTemplateGuid, mainEdt, 0, 1);
        ocrEdtCreate(&edtGuid, edtTemplateGuid, EDT_PARAM_DEF, /* paramv = */ NULL,
                     /* depc = */ EDT_PARAM_DEF, /* depv = */ &dbGuid,
                     EDT_PROP_NONE, affinityMasterPD, NULL);
    }

    ocrGuid_t processRequestTemplate = NULL_GUID;
    ocrEdtTemplateCreate(&processRequestTemplate, &processRequestEdt, 1, 0);
    do {
        // 'communication' loop: take, send / poll, dispatch, execute
        u32 PHASE_RUN = 3;
        u32 PHASE_COMP_QUIESCE = 2;
        u32 PHASE_COMM_QUIESCE = 1;
        u32 PHASE_DONE = 0;
        // Double check the setup
        ASSERT(RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK) == PHASE_RUN);
        u32 phase = worker->desiredState & 0xFFFF;
        if ((phase == PHASE_RUN) ||
            (phase == PHASE_COMP_QUIESCE)) {
            while(worker->curState == worker->desiredState) {
                workerLoopHcCommInternal(worker, processRequestTemplate, false);
            }
        } else if (phase == PHASE_COMM_QUIESCE) {
            // All workers in this PD are not executing user EDTs anymore.
            // However, there may still be communication in flight.
            // Two reasons for that:
            // 1- EDTs execution generated asynchronous one-way communications
            //    that need to be flushed out.
            // 2- Other PDs are still communication with the current PD.
            //    This happens mainly because some runtime work is done in
            //    EDT's epilogue. So the sink EDT has executed but some EDTs
            //    are still wrapping up.

            // Goal of this phase is to make sure this PD is done processing
            // the communication it has generated.

            // First: Flush all outgoing communications out of the PD
            while(takeFromSchedulerAndSend(worker->pd) == POLL_MORE_MESSAGE);

            // Second: Loop until pollMessage says there's no more outgoing
            // messages to be processed by the underlying comm-platform.
            // This call returns when outgoing messages are all sent ...
            workerLoopHcCommInternal(worker, processRequestTemplate, true/*check empty queue*/);
            // Done with the quiesce comm action, callback the PD
            worker->callback(worker->pd, worker->callbackArg);
            // Warning: Code potentially concurrent with switchRunlevel now on

            // The callback triggers the distributed shutdown protocol.
            // The PD intercepts the callback and sends shutdown message to other PDs.
            // When all PDs have answered to the shutdown message, the phase change is enacted.

            // Until that happens, keep working to process communications shutdown generates.
            while(worker->curState == worker->desiredState) {
                workerLoopHcCommInternal(worker, processRequestTemplate, false);
            }
            // The comm-worker has been transitioned
            ASSERT((worker->desiredState & 0xFFFF) == PHASE_DONE);
        } else {
            ASSERT(phase == PHASE_DONE);
            //TODO-RL: this needs to happen wrt to the generation of shutdown messages
            // When the comm-worker quiesce and it already had all its neighbors PD's shutdown msg
            // we need to make sure there's no outgoing messages pending (i.e. a one-way shutdown)
            // for other PDs before wrapping up the user runlevel.
            //TODO-RL: Need to revisit that and think about what messages could be present here.
            // Empty outgoing messages from the scheduler
            while(takeFromSchedulerAndSend(worker->pd) == POLL_MORE_MESSAGE);
            // Loop until pollMessage says there's no more outgoing messages to be sent
            workerLoopHcCommInternal(worker, processRequestTemplate, true/*check empty queue*/);
            // Phase shouldn't have changed since we haven't done callback yet
            ASSERT((worker->desiredState & 0xFFFF) == PHASE_DONE);
            worker->callback(worker->pd, worker->callbackArg);
            // Warning: Code potentially concurrent with switchRunlevel now on
            // Need to busy wait until the PD makes workers to transition to
            // the next runlevel. The switch hereafter can then take the correct case.
            //TODO-RL Should/Can callback implement some form of barrier instead of me doing that here
            while((worker->curState) == (worker->desiredState));
            ASSERT((worker->desiredState >> 16) == RL_COMPUTE_OK);
        }

        // Here we are shifting to another RL or Phase
        switch((worker->desiredState) >> 16) {
        case RL_USER_OK: {
            u32 desiredPhase = worker->desiredState & 0xFFFF;
            ASSERT(desiredPhase != PHASE_RUN);
            ASSERT((desiredPhase == PHASE_COMP_QUIESCE) ||
                    (desiredPhase == PHASE_COMM_QUIESCE) ||
                    (desiredPhase == PHASE_DONE));
            if (desiredPhase == PHASE_COMP_QUIESCE) {
                // No actions to take in this phase.
                // Callback the PD and fall-through to keep working.
                worker->callback(worker->pd, worker->callbackArg);
                //Warning: The moment this callback is invoked, This code
                //is potentially running concurrently with the last worker
                //going out if PHASE_COMP_QUIESCE. That also means this code
                //is potentially concurrently with 'switchRunlevel' being
                //invoked on this worker by another worker.
            }
            // - Intentionally fall-through here for PHASE_COMM_QUIESCE.
            //   The comm-worker leads that phase transition.
            // - Keep worker loop alive: MUST use 'desiredPhase' instead of
            //   'worker->desiredState' to avoid races.
            worker->curState = desiredPhase;
            break;
        }
        // BEGIN copy-paste original code
        // TODO-RL not sure we still need that
        case RL_COMPUTE_OK: {
            u32 phase = worker->desiredState & 0xFFFF;
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
        // END copy-paste original code
        default:
            // Only these two RL should occur
            ASSERT(0);
        }
    } while(continueLoop);
}

u8 hcCommWorkerSwitchRunlevel(ocrWorker_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                              u32 phase, u32 properties, void (*callback)(ocrPolicyDomain_t *, u64), u64 val) {
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
                    self->desiredState = (RL_USER_OK << 16) | RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK); // We put ourself one past
                    // so that we can then come back down when
                    // shutting down
                } else {
                    // At this point, the original capable thread goes to work
                    self->curState = self->desiredState = ((RL_USER_OK << 16) | RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK));
                    workerLoopHcComm(self);
                }
            }
        }

        u32 PHASE_RUN = 3;
        u32 PHASE_COMP_QUIESCE = 2;
        u32 PHASE_COMM_QUIESCE = 1;
        u32 PHASE_DONE = 0;
        if((properties & RL_TEAR_DOWN) && phase == PHASE_COMP_QUIESCE) {
            // Transitions from RUN to PHASE_COMP_QUIESCE
            // Not sure how that would happen but leave it for now
            ASSERT((self->curState == self->desiredState));
            ASSERT((self->curState >> 16) == RL_USER_OK);
            ASSERT((self->curState & 0xFFFF) == (PHASE_COMP_QUIESCE+1));
            ASSERT(callback != NULL);
            self->callback = callback;
            self->callbackArg = val;
            hal_fence();
            self->desiredState = RL_USER_OK << 16 | PHASE_COMP_QUIESCE;
        }

        if((properties & RL_TEAR_DOWN) && phase == PHASE_COMM_QUIESCE) {
            //Warning: At this point it is not 100% sure the worker has
            //already transitioned to PHASE_COMM_QUIESCE.
            ASSERT(((self->curState & 0xFFFF) == PHASE_COMP_QUIESCE) ||
                   ((self->curState & 0xFFFF) == PHASE_RUN));
            // This is set for sure
            ASSERT((self->desiredState & 0xFFFF) == PHASE_COMP_QUIESCE);
            ASSERT(callback != NULL);
            self->callback = callback;
            self->callbackArg = val;
            hal_fence();
            // Either breaks the worker's loop from the PHASE_COMP_QUIESCE
            // or is set even before that loop is reached and skip the
            // PHASE_COMP_QUIESCE altogeher, which is fine
            self->desiredState = RL_USER_OK << 16 | PHASE_COMM_QUIESCE;
        }

        //TODO-RL Last phase that transitions to another runlevel
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_USER_OK, phase)) {
            ASSERT(phase == PHASE_DONE);
            // We need to break out of the compute loop
            // We need to have a callback for all workers here
            ASSERT(callback != NULL);
            // We make sure that we actually fully booted before shutting down
            //RL why is this possible ?
            while(self->curState != (RL_USER_OK << 16)) ;

            ASSERT(self->curState == RL_USER_OK);
            self->callback = callback;
            self->callbackArg = val;
            hal_fence();
            // Breaks the worker's compute loop
            self->desiredState = RL_COMPUTE_OK << 16;
        }
        break;
    }
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void* runWorkerHcComm(ocrWorker_t * worker) {
    // TODO: This probably needs to go away and be set directly
    // by the PD during one of the RLs
    //Register this worker and get a context id
    ocrPolicyDomain_t *pd = worker->pd;
    PD_MSG_STACK(msg);
    msg.srcLocation = pd->myLocation;
    msg.destLocation = pd->myLocation;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_REGISTER
    msg.type = PD_MSG_MGT_REGISTER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(loc) = (ocrLocation_t) ((ocrWorkerHc_t *) worker)->id;
    PD_MSG_FIELD_I(properties) = 0;
    pd->fcts.processMessage(pd, &msg, true);
    worker->seqId = PD_MSG_FIELD_O(seqId);
#undef PD_MSG
#undef PD_TYPE

    // At this point, we should have a callback to inform the PD
    // that we have successfully achieved the RL_COMPUTE_OK RL
    ASSERT(worker->callback != NULL);
    worker->callback(worker->pd, worker->callbackArg);

    // Set the current environment
    worker->computes[0]->fcts.setCurrentEnv(worker->computes[0], worker->pd, worker);
    worker->curState = RL_COMPUTE_OK << 16;

    // We wait until we transition to the next RL
    while(worker->curState == worker->desiredState) ;

    // At this point, we should be going to RL_USER_OK
    ASSERT(worker->desiredState == ((RL_USER_OK << 16) | (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK))));

    // Start the worker loop
    worker->curState = worker->desiredState;
    workerLoopHcComm(worker);
    // Worker loop will transition back down to RL_COMPUTE_OK last phase

    ASSERT((worker->curState == worker->desiredState) && (worker->curState == ((RL_COMPUTE_OK << 16 ) | (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_COMPUTE_OK) - 1))));
    return NULL;
}

/**
 * Builds an instance of a HC Communication worker
 */
ocrWorker_t* newWorkerHcComm(ocrWorkerFactory_t * factory, ocrParamList_t * perInstance) {
    ocrWorker_t * worker = (ocrWorker_t*)
            runtimeChunkAlloc(sizeof(ocrWorkerHcComm_t), NULL);
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
}

/******************************************************/
/* OCR-HC COMMUNICATION WORKER FACTORY                */
/******************************************************/

void destructWorkerFactoryHcComm(ocrWorkerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrWorkerFactory_t * newOcrWorkerFactoryHcComm(ocrParamList_t * perType) {
    ocrWorkerFactory_t * baseFactory = newOcrWorkerFactoryHc(perType);
    ocrWorkerFcts_t baseFcts = baseFactory->workerFcts;

    ocrWorkerFactoryHcComm_t* derived = (ocrWorkerFactoryHcComm_t*)runtimeChunkAlloc(sizeof(ocrWorkerFactoryHcComm_t), (void *)1);
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
    //TODO This doesn't really work out for communication-workers
    base->workerFcts.workShift = FUNC_ADDR(void* (*) (ocrWorker_t *), workerLoopHcComm_DUMMY);
    base->workerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                       u32, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcCommWorkerSwitchRunlevel);
    baseFactory->destruct(baseFactory);
    return base;
}

#endif /* ENABLE_WORKER_HC_COMM */
