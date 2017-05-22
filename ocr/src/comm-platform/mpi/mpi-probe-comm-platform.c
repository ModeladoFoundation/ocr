/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_MPI_PROBE

#include "debug.h"

#include "ocr-errors.h"
#include "ocr-sysboot.h"
#include "ocr-policy-domain.h"
#include "ocr-policy-domain-tasks.h"
#include "ocr-worker.h"

#include "utils/ocr-utils.h"

#include "mpi-probe-comm-platform.h"
#include <mpi.h>

#ifdef DEBUG_MPI_HOSTNAMES
// For gethostname
#include <unistd.h>
#endif

//BUG #609 system header: replace this with some INT_MAX from sal header
#include "limits.h"

//
// Compile-time constants
//

// DEBUG_MPI_HOSTNAMES: Dumps hostname MPI processes are started on

#define DEBUG_TYPE COMM_PLATFORM

//
// MPI communication implementation strategy
//

// Pre-post an irecv to listen to outstanding request and for every
// request that requires a response. Only supports fixed size receives.
// Warning: This mode impl is usually lagging behind the other
//          mode (i.e. less tested, may be broken).
#define STRATEGY_PRE_POST_RECV 0

// Use iprobe to scan for outstanding request (tag matches RECV_ANY_ID)
// and incoming responses for requests (using src/tag pairs)
#define STRATEGY_PROBE_RECV (!STRATEGY_PRE_POST_RECV)

// To tag outstanding send/recv
#define RECV_ANY_ID 0
#define SEND_ANY_ID 0

// Handles maintained internally to figure out what
// to listen to and what to do with the response
// This is a bit more complicated because it currently supports
// both the old style and the MT style of communication
typedef struct {
    u64 msgId;
    MPI_Request status;
#if STRATEGY_PROBE_RECV
    int src;
#endif
    u8 isMtHandle;
} mpiCommHandleBase_t;

typedef struct {
    mpiCommHandleBase_t base;
    u32 properties;
    ocrPolicyMsg_t * msg;
    u8 deleteSendMsg;
} mpiCommHandle_t;

typedef struct {
    mpiCommHandleBase_t base;
    ocrPolicyMsg_t *myMsg; /**< For ONE_WAY message, store the message buffer to free */
    pdStrand_t *myStrand;  /**< For two way messages, store the strand containing message */
} mpiCommHandleMt_t;

// Defined in mpi-comm-platform.c
extern void platformInitMPIComm(int * argc, char *** argv);
extern void platformFinalizeMPIComm();

static ocrLocation_t mpiRankToLocation(int mpiRank) {
    //BUG #605 Locations spec: identity integer cast for now
    return (ocrLocation_t) mpiRank;
}

static int locationToMpiRank(ocrLocation_t location) {
    //BUG #605 Locations spec: identity integer cast for now
    return (int) location;
}

/**
 * @brief Internal use - Returns a new message
 */
static ocrPolicyMsg_t * allocateNewMessage(ocrCommPlatform_t * self, u32 size) {
    ocrPolicyDomain_t * pd = self->pd;
    ocrPolicyMsg_t * message = pd->fcts.pdMalloc(pd, size);
    ADebug(AllocDebugAllPD, "mpi-probe-comm-platform.c/allocateNewMessage() "
           "pdMalloc(%ld), addr=%p\n", size, message);
    initializePolicyMessage(message, size);
    return message;
}

/**
 * @brief Internal use - Create a mpi handle to represent pending communications
 */
static mpiCommHandleBase_t * createMpiHandle(ocrCommPlatform_t * self, u64 id, u32 properties,
                                             ocrPolicyMsg_t * msg, u8 deleteSendMsg, u8 newMTMode) {
    mpiCommHandleBase_t *handleBase = NULL;
    if(newMTMode) {
        handleBase = self->pd->fcts.pdMalloc(self->pd, sizeof(mpiCommHandleMt_t));
        ADebug(AllocDebugAllPD, "mpi-probe-comm-platform.c/mpiCommHandleBase_t(1) "
               "pdMalloc(%ld), addr=%p\n", sizeof(mpiCommHandleMt_t), handleBase);
        handleBase->isMtHandle = true;
        mpiCommHandleMt_t *handle = (mpiCommHandleMt_t*)handleBase;
        handle->myStrand = NULL;
        handle->myMsg = NULL;
    } else {
        handleBase = self->pd->fcts.pdMalloc(self->pd, sizeof(mpiCommHandle_t));
        ADebug(AllocDebugAllPD, "mpi-probe-comm-platform.c/mpiCommHandleBase_t(2) "
               "pdMalloc(%ld), addr=%p\n", sizeof(mpiCommHandleMt_t), handleBase);
        handleBase->isMtHandle = false;
        mpiCommHandle_t *handle = (mpiCommHandle_t*)handleBase;
        handle->properties = properties;
        handle->msg = msg;
        handle->deleteSendMsg = deleteSendMsg;
    }
    handleBase->msgId = id;
    return handleBase;
}

#if STRATEGY_PRE_POST_RECV
/**
 * @brief Internal use - Asks the comm-platform to listen for incoming communication.
 */
static void postRecvAny(ocrCommPlatform_t * self) {
    ocrCommPlatformMPIProbe_t * mpiComm = (ocrCommPlatformMPIProbe_t *) self;
    ocrPolicyMsg_t * msg = allocateNewMessage(self, mpiComm->maxMsgSize);
    mpiCommHandleBase_t * handle = createMpiHandle(self, RECV_ANY_ID, PERSIST_MSG_PROP, msg, false, false);
    void * buf = msg; // Reuse request message as receive buffer
    int count = mpiComm->maxMsgSize; // don't know what to expect, upper-bound on message size
    MPI_Datatype datatype = MPI_BYTE;
    int src = MPI_ANY_SOURCE;
#if STRATEGY_PROBE_RECV
    handle->src = MPI_ANY_SOURCE;
#endif
    int tag = RECV_ANY_ID;
    MPI_Comm comm = MPI_COMM_WORLD;
    DPRINTF(DEBUG_LVL_VERB,"[MPI %"PRId32"] posting irecv ANY\n", mpiRankToLocation(self->pd->myLocation));
    int res = MPI_Irecv(buf, count, datatype, src, tag, comm, &(handle->status));
    ASSERT(res == MPI_SUCCESS);

#ifdef OCR_MONITOR_NETWORK
    buf->rcvTime = salGetTime();
#endif
    mpiComm->incoming->pushFront(mpiComm->incoming, handle);
}
#endif

/**
 * @brief Internal -- verify that outgoing messages are sent
 */
static u8 verifyOutgoing(ocrCommPlatformMPIProbe_t *mpiComm) {
    ocrPolicyDomain_t *pd = mpiComm->base.pd;
    // Iterate over outgoing communications (mpi sends)
    iterator_t * outgoingIt = mpiComm->outgoingIt;
    outgoingIt->reset(outgoingIt);
    DPRINTF(DEBUG_LVL_VERB, "[MPI %"PRId32"] Going to check for outgoing messages\n",
            locationToMpiRank(pd->myLocation));
    while (outgoingIt->hasNext(outgoingIt)) {
        mpiCommHandleBase_t * mpiHandle = (mpiCommHandleBase_t *) outgoingIt->next(outgoingIt);
        int completed = 0;
        RESULT_ASSERT(MPI_Test(&(mpiHandle->status), &completed, MPI_STATUS_IGNORE), ==, MPI_SUCCESS);
        if(completed) {
            ocrPolicyMsg_t *msg __attribute__((unused)) = NULL;
            if(mpiHandle->isMtHandle) {
                mpiCommHandleMt_t *t = (mpiCommHandleMt_t*) mpiHandle;
                if(t->myMsg) { //TODO-MT-COMM: Need to understand why 'myMsg' discriminates 1- or 2-way
                    msg = t->myMsg;
                    DPRINTF(DEBUG_LVL_VVERB,"[MPI %"PRId32"] ONE WAY sent msg=%p src=%"PRId32", dst=%"PRId32", msgId=%"PRIu64", type=0x%"PRIx32", usefulSize=%"PRIu64"\n",
                        locationToMpiRank(pd->myLocation), msg,
                        locationToMpiRank(msg->srcLocation), locationToMpiRank(msg->destLocation),
                        msg->msgId, msg->type, msg->usefulSize);
                } else {
                    pdStrand_t* strand = t->myStrand;
                    msg = ((pdEventMsg_t*)(strand->curEvent))->msg;
                    DPRINTF(DEBUG_LVL_VVERB,"[MPI %"PRId32"] TWO WAY sent evt=%p, msg=%p src=%"PRId32", dst=%"PRId32", msgId=%"PRIu64", type=0x%"PRIx32", usefulSize=%"PRIu64"\n",
                        locationToMpiRank(pd->myLocation), strand->curEvent, msg,
                        locationToMpiRank(msg->srcLocation), locationToMpiRank(msg->destLocation),
                        msg->msgId, msg->type, msg->usefulSize);
                }
            } else {
                msg = ((mpiCommHandle_t *)mpiHandle)->msg;
                DPRINTF(DEBUG_LVL_VVERB,"[MPI %"PRId32"] sent msg=%p src=%"PRId32", dst=%"PRId32", msgId=%"PRIu64", type=0x%"PRIx32", usefulSize=%"PRIu64"\n",
                    locationToMpiRank(pd->myLocation), msg,
                    locationToMpiRank(msg->srcLocation), locationToMpiRank(msg->destLocation),
                    msg->msgId, msg->type, msg->usefulSize);
            }


            if(mpiHandle->isMtHandle) {
                mpiCommHandleMt_t *t = (mpiCommHandleMt_t*)mpiHandle;
                if(t->myMsg) {
                    DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] ONE_WAY message being freed\n",
                            locationToMpiRank(pd->myLocation));
                    ASSERT(t->myStrand == NULL);
                    // This means that a COMM_ONE_WAY message was sent, we
                    // free things
                    pd->fcts.pdFree(pd, t->myMsg);
                    pd->fcts.pdFree(pd, mpiHandle);
                } else {
                    ASSERT(t->myStrand);
                    // Don't do anything, push things on the incomming queue so
                    // we can periodically check for it
                    DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] Pushing MT handle to incoming queue\n",
                            locationToMpiRank(pd->myLocation));
#ifdef MPI_COMM_PUSH_AT_TAIL
                    mpiComm->incoming->pushTail(mpiComm->incoming, mpiHandle);
#else
                    mpiComm->incoming->pushFront(mpiComm->incoming, mpiHandle);
#endif
                }
            } else {
                u32 msgProperties = ((mpiCommHandle_t*)mpiHandle)->properties;
                // By construction, either messages are persistent in API's upper levels
                // or they've been made persistent on the send through a copy.
                ASSERT(msgProperties & PERSIST_MSG_PROP);
                // Delete the message if one-way (request or response).
                // Otherwise message might be used to store the response later.
                if (!(msgProperties & TWOWAY_MSG_PROP) || (msgProperties & ASYNC_MSG_PROP)) {
                    pd->fcts.pdFree(pd, ((mpiCommHandle_t*)mpiHandle)->msg);
                    pd->fcts.pdFree(pd, mpiHandle);
                } else {
                    // The message requires a response, put it in the incoming list
#ifdef MPI_COMM_PUSH_AT_TAIL
                    mpiComm->incoming->pushTail(mpiComm->incoming, mpiHandle);
#else
                    mpiComm->incoming->pushFront(mpiComm->incoming, mpiHandle);
#endif
                }
            }
            outgoingIt->removeCurrent(outgoingIt);
        }
    }
    DPRINTF(DEBUG_LVL_VERB, "[MPI %"PRId32"] Done checking for outgoing messages\n",
            locationToMpiRank(pd->myLocation));
    return 0;
}

#ifdef STRATEGY_PROBE_RECV
static u8 probeIncoming(ocrCommPlatform_t *self, int src, int tag, ocrPolicyMsg_t ** msg, int bufferSize);
#endif

/**
 * @brief Internal -- check for incomming responses to messages we sent (only responses)
 *
 * This function is only used by the MT functions as these responses are dealt
 * with differently than other incoming messages
 */

static u8 verifyIncomingResponsesMT(ocrCommPlatformMPIProbe_t *mpiComm, bool doUntilEmpty) {
    iterator_t * incomingIt = mpiComm->incomingIt;
    ocrPolicyDomain_t *pd = ((ocrCommPlatform_t*)mpiComm)->pd;
    incomingIt->reset(incomingIt);
    DPRINTF(DEBUG_LVL_VERB, "[MPI %"PRId32"] Going to check for incoming MT responses\n",
            locationToMpiRank(pd->myLocation));
    while(incomingIt->hasNext(incomingIt)) {
        mpiCommHandleBase_t * mpiHandle = (mpiCommHandleBase_t *) incomingIt->next(incomingIt);

        // We check if this is a MT message
        if(mpiHandle->isMtHandle) {
            DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] Found a MT MPI handle @ %p\n",
                    locationToMpiRank(pd->myLocation), mpiHandle);
            mpiCommHandleMt_t *handle = (mpiCommHandleMt_t*)mpiHandle;
            ASSERT(handle->myStrand); // If the message is in the incoming queue, it has a strand to contain the result
            pdEventMsg_t *msgEvent = (pdEventMsg_t*)(handle->myStrand->curEvent);
            ocrPolicyMsg_t **addrOfMsg = &(msgEvent->msg);
            ocrPolicyMsg_t * reqMsg = *addrOfMsg;
            DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] Handle has Strand:%p; event:%p, addrMsg:%p, reqMsg:%p\n",
                locationToMpiRank(pd->myLocation), handle->myStrand,
                handle->myStrand->curEvent, addrOfMsg, reqMsg);
            // Here we try to reuse the request message to receive the response
            u8 res = probeIncoming((ocrCommPlatform_t*)mpiComm, mpiHandle->src, (int) mpiHandle->msgId,
                                   addrOfMsg, reqMsg->bufferSize);

            // The message is properly unmarshalled at this point
            if (res == POLL_MORE_MESSAGE) {
                DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] received response on strand %p, orig msg=%p, new msg=%p\n",
                        locationToMpiRank(pd->myLocation), handle->myStrand, reqMsg, *addrOfMsg);
#ifdef OCR_ASSERT
                if(reqMsg != *addrOfMsg) {
                    // This means a new message was allocate as the response
                    // and the original request should be left as is)
                    ASSERT((reqMsg->srcLocation == pd->myLocation) && (reqMsg->destLocation != pd->myLocation));
                    ASSERT(((*addrOfMsg)->srcLocation != pd->myLocation) && ((*addrOfMsg)->destLocation == pd->myLocation));
                } else {
                    // Message was overwritten
                    ASSERT(((*addrOfMsg)->srcLocation != pd->myLocation) && ((*addrOfMsg)->destLocation == pd->myLocation));
                }
#endif
                if(reqMsg != *addrOfMsg) {
                    // Free the original request message
                    if(!(msgEvent->properties & COMM_STACK_MSG)) {
                        pd->fcts.pdFree(pd, reqMsg);
                    } else {
                        msgEvent->properties &= ~(COMM_STACK_MSG);
                    }
                }
                ASSERT((*addrOfMsg)->msgId == mpiHandle->msgId);

                // Mark the event as being ready so that someone can pick it up
                RESULT_ASSERT(pdMarkReadyEvent(pd, handle->myStrand->curEvent), ==, 0);

                // Free the handle and remove from incoming list
                pd->fcts.pdFree(pd, mpiHandle);
                incomingIt->removeCurrent(incomingIt);
                if(!doUntilEmpty) {
                    DPRINTF(DEBUG_LVL_VERB, "[MPI %"PRId32"] Done checking for incoming MT responses\n",
                            locationToMpiRank(pd->myLocation));
                    return POLL_MORE_MESSAGE;
                }
            }
        } else {
            DPRINTF(DEBUG_LVL_WARN, "[MPI %"PRId32"] Found a non MT handle!!!\n",
                    locationToMpiRank(pd->myLocation));
        }
    }
    DPRINTF(DEBUG_LVL_VERB, "[MPI %"PRId32"] done checking for incoming MT responses\n",
        locationToMpiRank(pd->myLocation));
    return POLL_NO_MESSAGE;
}

//
// Communication API
//

static u8 MPICommSendMessage(ocrCommPlatform_t * self,
                      ocrLocation_t target, ocrPolicyMsg_t * message,
                      u64 *id, u32 properties, u32 mask) {

    u64 bufferSize = message->bufferSize;
    ocrCommPlatformMPIProbe_t * mpiComm = ((ocrCommPlatformMPIProbe_t *) self);

    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
    u64 fullMsgSize = baseSize + marshalledSize;

    //BUG #602 multi-comm-worker: msgId incr only works if a single comm-worker per rank,
    //do we want OCR to provide PD, system level counters ?
    // Always generate an identifier for a new communication to give back to upper-layer
    u64 mpiId = mpiComm->msgId++;

    // If we're sending a request, set the message's msgId to this communication id
    if (message->type & PD_MSG_REQUEST) {
        message->msgId = mpiId;
    } else {
        // For response in ASYNC set the message ID as any.
        ASSERT(message->type & PD_MSG_RESPONSE);
        if (properties & ASYNC_MSG_PROP) {
            message->msgId = SEND_ANY_ID;
        }
        // else, for regular responses, just keep the original
        // message's msgId the calling PD is waiting on.
    }

    ocrPolicyMsg_t * messageBuffer = message;

    // Check if we need to allocate a new message buffer:
    //  - Does the serialized message fit in the current message ?
    //  - Is the message persistent (then need a copy anyway) ?
    bool deleteSendMsg = false;
    if ((fullMsgSize > bufferSize) || !(properties & PERSIST_MSG_PROP)) {
        // Allocate message and marshall a copy
        messageBuffer = allocateNewMessage(self, fullMsgSize);
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)messageBuffer,
            MARSHALL_FULL_COPY | MARSHALL_DBPTR | MARSHALL_NSADDR);
        if (properties & PERSIST_MSG_PROP) {
            // Message was persistent, two cases:
            if ((properties & TWOWAY_MSG_PROP) && (!(properties & ASYNC_MSG_PROP))) {
                //  - The message is two-way and is not asynchronous: do not touch the
                //    message parameter, but record that we indeed made a new copy that
                //    we will have to deallocate when the communication is completed.
                deleteSendMsg = true;
            } else {
                //  - The message is one-way: By design, all one-way are heap-allocated copies.
                //    It is the comm-platform responsibility to free them, do it now since we've
                //    made our own copy.
                self->pd->fcts.pdFree(self->pd, message);
                message = NULL; // to catch misuses later in this function call
            }
        } else {
            // Message wasn't persistent, hence the caller is responsible for deallocation.
            // It doesn't matter whether the communication is one-way or two-way.
            properties |= PERSIST_MSG_PROP;
            ASSERT(false && "not used in current implementation (hence not tested)");
        }
    } else {
        ocrMarshallMode_t marshallMode = (ocrMarshallMode_t) GET_PROP_U8_MARSHALL(properties);
        if (marshallMode == 0) {
            // Marshall the message. We made sure we had enough space.
            ocrPolicyMsgMarshallMsg(messageBuffer, baseSize, (u8*)messageBuffer,
                                    MARSHALL_APPEND | MARSHALL_DBPTR | MARSHALL_NSADDR);
        } else {
            ASSERT(marshallMode == MARSHALL_FULL_COPY);
            //BUG #604 Communication API extensions
            // They are needed in a comm-platform such as mpi or gasnet
            // but it feels off that the calling context already set those
            // because it shouldn't know beforehand if the communication is
            // crossing address space
            // | MARSHALL_DBPTR :  only for acquire/release message
            // | MARSHALL_NSADDR : only used when unmarshalling so far
            ASSERT ((((messageBuffer->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) ||
                    ((messageBuffer->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_RELEASE))
                    ? (marshallMode & (MARSHALL_DBPTR | MARSHALL_NSADDR)) : 1);
        }
    }

    // Warning: From now on, exclusively use 'messageBuffer' instead of 'message'
    ASSERT(fullMsgSize == messageBuffer->usefulSize);
    // Prepare MPI call arguments
    MPI_Datatype datatype = MPI_BYTE;
    int targetRank = locationToMpiRank(target);
    ASSERT(targetRank > -1);
    MPI_Comm comm = MPI_COMM_WORLD;

    // Setup request's MPI send
    mpiCommHandleBase_t * handle = createMpiHandle(self, mpiId, properties, messageBuffer, deleteSendMsg, false);

    // Setup request's response
    if ((messageBuffer->type & PD_MSG_REQ_RESPONSE) && !(properties & ASYNC_MSG_PROP)) {
#if STRATEGY_PRE_POST_RECV
        // Reuse request message as receive buffer unless indicated otherwise
        ocrPolicyMsg_t * respMsg = messageBuffer;
        int respTag = mpiId;
        // Prepare a handle for the incoming response
        mpiCommHandleBase_t * respHandle = createMpiHandle(self, respTag, properties, respMsg, false, false);
        //PERF: (STRATEGY_PRE_POST_RECV) could do better if the response for this message's type is of fixed-length.
        int respCount = mpiComm->maxMsgSize;
        MPI_Request * status = &(respHandle->status);
        //Post a receive matching the request's msgId.
        //The other end will post a send using msgId as tag
        DPRINTF(DEBUG_LVL_VERB,"[MPI %"PRId32"] posting irecv for msgId %"PRIu64"\n", mpiRankToLocation(self->pd->myLocation), respTag);
        int res = MPI_Irecv(respMsg, respCount, datatype, targetRank, respTag, comm, status);
        if (res != MPI_SUCCESS) {
            //BUG #603 define error for comm-api
            ASSERT(false);
            return res;
        }

#ifdef OCR_MONITOR_NETWORK
        respMsg->rcvTime = salGetTime();
#endif
#ifdef MPI_COMM_PUSH_AT_TAIL
        mpiComm->incoming->pushTail(mpiComm->incoming, respHandle);
#else
        mpiComm->incoming->pushFront(mpiComm->incoming, respHandle);
#endif
#endif
#if STRATEGY_PROBE_RECV
        // In probe mode just record the recipient id to be checked later
        handle->src = targetRank;
#endif
    }

    // If this send is for a response, use message's msgId as tag to
    // match the source recv operation that had been posted on the request send.
    // Note that msgId is set to SEND_ANY_ID a little earlier in the case of asynchronous
    // message like DB_ACQUIRE. It allows to handle the response as a one-way message that
    // is not tied to any particular request at destination
    int tag = (messageBuffer->type & PD_MSG_RESPONSE) ? messageBuffer->msgId : SEND_ANY_ID;

    MPI_Request * status = &(handle->status);

    DPRINTF(DEBUG_LVL_VVERB,"[MPI %"PRId32"] posting isend for msgId=%"PRIu64" msg=%p type=%"PRIx32" "
            "fullMsgSize=%"PRIu64" marshalledSize=%"PRIu64" to MPI rank %"PRId32"\n",
            locationToMpiRank(self->pd->myLocation), messageBuffer->msgId,
            messageBuffer, messageBuffer->type, fullMsgSize, marshalledSize, targetRank);

    //If this assert bombs, we need to implement message chunking
    //or use a larger MPI datatype to send the message.
    ASSERT((fullMsgSize < INT_MAX) && "Outgoing message is too large");
    ASSERT((messageBuffer->srcLocation == self->pd->myLocation) &&
        (messageBuffer->destLocation != self->pd->myLocation) &&
        (targetRank == messageBuffer->destLocation));

#ifdef OCR_MONITOR_NETWORK
    messageBuffer->sendTime = salGetTime();
#endif

    int res = MPI_Isend(messageBuffer, (int) fullMsgSize, datatype, targetRank, tag, comm, status);

    if (res == MPI_SUCCESS) {
#ifdef MPI_COMM_PUSH_AT_TAIL
        mpiComm->outgoing->pushTail(mpiComm->outgoing, handle);
#else
        mpiComm->outgoing->pushFront(mpiComm->outgoing, handle);
#endif
        *id = mpiId;
    } else {
        //BUG #603 define error for comm-api
        ASSERT(false);
    }

    return res;
}

static u8 MPICommSendMessageMT(ocrCommPlatform_t * self,
                        pdEvent_t **inOutMsg,
                        pdEvent_t *statusEvent, u32 idx) {

    // Make sure we at least have something to send
    ASSERT(*inOutMsg != NULL);
    u64 evtValue = (u64)(*inOutMsg);

    DPRINTF(DEBUG_LVL_VERB, "[MPI %"PRId32"] MTSend of event 0x%"PRIx64"\n",
            locationToMpiRank(self->pd->myLocation), evtValue);
    if(idx == 0) {
        // This is a direct call to the function (no strand processing)
        // We only deal with cases where the message is ready at this time
        u8 ret = pdResolveEvent(self->pd, &evtValue, 0);
        ASSERT(ret == 0 || ret == OCR_ENOP);
        *inOutMsg = (pdEvent_t*)evtValue;
    } else {
        // We do not deal with continuations from inside this function yet
        // Runtime error, see OCR developers
        ASSERT(0);
    }

    DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] MTSend resolved event to %p\n",
            locationToMpiRank(self->pd->myLocation), *inOutMsg);
    // Make sure the event contains a message
    ASSERT((*inOutMsg)->properties & PDEVT_TYPE_MSG);

    // Extract the message from the event
    pdEventMsg_t *msgEvent = (pdEventMsg_t*)(*inOutMsg);
    ocrPolicyMsg_t *message = msgEvent->msg;

    u64 bufferSize = message->bufferSize;
    ocrCommPlatformMPIProbe_t * mpiComm = ((ocrCommPlatformMPIProbe_t *) self);

    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
    u64 fullMsgSize = baseSize + marshalledSize;

    //BUG #602 multi-comm-worker: msgId incr only works if a single comm-worker per rank,
    //do we want OCR to provide PD, system level counters ?
    // Always generate an identifier for a new communication to give back to upper-layer
    u64 mpiId = mpiComm->msgId++;

    if(!(message->type & PD_MSG_RESPONSE)) {
        // If we're sending an actual two way message, set the msgId
        if (!(msgEvent->properties & COMM_ONE_WAY)) {
            message->msgId = mpiId;
        } else {
            // In other case, we use the SEND_ANY_ID so that
            // when the other side responds, it send it using this tag
            // (which is where we will be "listening")
            message->msgId = SEND_ANY_ID;
        }
    } else { // a response
        // TODO-MT-COMM: This is only needed to accommodate the TWO_WAY/ASYNC paradigm
        //               Can get rid of it when we go full on with MT
        if((msgEvent->properties & COMM_ONE_WAY) &&
           (((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) ||
            ((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_GUID_METADATA_CLONE))) {
            message->msgId = SEND_ANY_ID;
        }
    }

    // Check if we need to allocate a new message buffer:
    //  - Does the serialized message fit in the current message ?
    if ((fullMsgSize > bufferSize)) {
        // Allocate message and marshall a copy
        ocrPolicyMsg_t *messageBuffer = allocateNewMessage(self, fullMsgSize);
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)messageBuffer,
                                MARSHALL_FULL_COPY | MARSHALL_DBPTR | MARSHALL_NSADDR);
        // Replace the message in the event with the larger one and destroy the old one
        msgEvent->msg = messageBuffer;
        if(!(msgEvent->properties & COMM_STACK_MSG)) {
            self->pd->fcts.pdFree(self->pd, message);
        }
        msgEvent->properties &= ~COMM_STACK_MSG;
        message = msgEvent->msg;
    } else {
        // TODO: I will have to revisit this...
        ocrMarshallMode_t marshallMode = (ocrMarshallMode_t) GET_PROP_U8_MARSHALL(msgEvent->properties);
        if (marshallMode == 0) {
            // Marshall the message. We made sure we had enough space.
            ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)message,
                                    MARSHALL_APPEND | MARSHALL_DBPTR | MARSHALL_NSADDR);
        } else {
            ASSERT(marshallMode == MARSHALL_FULL_COPY);
            //BUG #604 Communication API extensions
            // They are needed in a comm-platform such as mpi or gasnet
            // but it feels off that the calling context already set those
            // because it shouldn't know beforehand if the communication is
            // crossing address space
            // | MARSHALL_DBPTR :  only for acquire/release message
            // | MARSHALL_NSADDR : only used when unmarshalling so far
            ASSERT ((((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) ||
                    ((message->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_RELEASE))
                    ? (marshallMode & (MARSHALL_DBPTR | MARSHALL_NSADDR)) : 1);
        }
    }

    ASSERT(fullMsgSize == message->usefulSize);
    // Prepare MPI call arguments
    MPI_Datatype datatype = MPI_BYTE;
    int targetRank = locationToMpiRank(message->destLocation);
    ASSERT(targetRank > -1);
    MPI_Comm comm = MPI_COMM_WORLD;

    // Setup request's MPI send
    mpiCommHandleBase_t * handle = createMpiHandle(self, mpiId, msgEvent->properties, message, false, true);

    // If this is not a ONE_WAY message, we need to figure out who to
    // probe later on to get the response from
    // NOTE: This precludes forwarding requests at this point
    if (!(msgEvent->properties & COMM_ONE_WAY)) {
        // In probe mode just record the recipient id to be checked later
        handle->src = targetRank;
    }

    // Here, if this is a response:
    //   - to a COMM_ONE_WAY: we use msgId which will have been set to SEND_ANY_ID
    //   - to a two-way message: we use msgId which will have been properly set
    int tag = (message->type & PD_MSG_RESPONSE) ? message->msgId : SEND_ANY_ID;

    MPI_Request * status = &(handle->status);

    DPRINTF(DEBUG_LVL_VVERB,"[MPI %"PRId32"] posting isend for msgId=%"PRIu64" msg=%p type=%"PRIx32" "
            "fullMsgSize=%"PRIu64" marshalledSize=%"PRIu64" to MPI rank %"PRId32" with tag %"PRId32"\n",
            locationToMpiRank(self->pd->myLocation), message->msgId,
            message, message->type, fullMsgSize, marshalledSize, targetRank, tag);

    //If this assert bombs, we need to implement message chunking
    //or use a larger MPI datatype to send the message.
    ASSERT((fullMsgSize < INT_MAX) && "Outgoing message is too large");
    ASSERT((message->srcLocation == self->pd->myLocation) &&
        (message->destLocation != self->pd->myLocation) &&
        (targetRank == message->destLocation));
    int res = MPI_Isend(message, (int) fullMsgSize, datatype, targetRank, tag, comm, status);

    if(res == MPI_SUCCESS) {
        if(msgEvent->properties & COMM_ONE_WAY) {
            DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] send is COMM_ONE_WAY -- no strand created\n",
                    locationToMpiRank(self->pd->myLocation));
            // If this is a one-way message, we destroy the current event, store the message
            // (to destroy later) and return a NULL event
            ((mpiCommHandleMt_t*)handle)->myMsg = message;
            msgEvent->msg = NULL; // Set to NULL because msgEvent may free things deeply
                                  // and we actually want to keep around the message to free
                                  // it later ourself

            // A ONE_WAY message should be auto garbage collected
            ASSERT((*inOutMsg)->properties & PDEVT_GC);
            *inOutMsg = NULL;
        } else {
            DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] send expects a response\n",
                    locationToMpiRank(self->pd->myLocation));
            // If there is a response expected, we need to keep track of that
            // We either mark the event as non-ready if it is already in a strand
            // or create a new strand for it. In all cases, we clear the properties flag
            msgEvent->properties = 0;

            // First, mark the event as not ready (we are waiting for a response)
            RESULT_ASSERT(pdMarkWaitEvent(self->pd, *inOutMsg), ==, 0);

            if(idx == 0) {
                if((*inOutMsg)->strand == NULL) {
                    // If we don't have a strand, we create a new one and put this
                    // event in it.
                    pdStrand_t *resultStrand = NULL;
                    RESULT_ASSERT(pdGetNewStrand(self->pd, &resultStrand,
                                                 self->pd->strandTables[PDSTT_COMM-1],
                                                 *inOutMsg, 0), ==, 0);
                    ((mpiCommHandleMt_t *)handle)->myStrand = resultStrand;
                    // Return the "fake" event pointer
                    *inOutMsg = (void*)(PDST_EVENT_ENCODE(resultStrand, PDSTT_COMM));
                    RESULT_ASSERT(pdUnlockStrand(resultStrand), ==, 0);
                } else {
                    ((mpiCommHandleMt_t*)handle)->myStrand = (*inOutMsg)->strand;
                    // No need to change the event; it is not ready so will not
                    // continue processing stuff
                }
            } else {
                // We don't currently support continuations
                ASSERT(0);
            }

            DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] send expects response -- in strand %p; returned %p\n",
                    locationToMpiRank(self->pd->myLocation), ((mpiCommHandleMt_t*)handle)->myStrand, *inOutMsg);
        }
        // We push this to the outgoing queue because we need to check if we
        // can free the buffer at some point
#ifdef MPI_COMM_PUSH_AT_TAIL
        mpiComm->outgoing->pushTail(mpiComm->outgoing, handle);
#else
        mpiComm->outgoing->pushFront(mpiComm->outgoing, handle);
#endif
    } else {
        //BUG #603 define error for comm-api
        ASSERT(false);
    }

    // No support for statusEvent for now
    if(statusEvent != NULL) {
        DPRINTF(DEBUG_LVL_WARN, "Ignoring statusEvent for now\n");
        statusEvent = NULL;
    }

    return res;
}

#if STRATEGY_PROBE_RECV
static u8 probeIncoming(ocrCommPlatform_t *self, int src, int tag, ocrPolicyMsg_t ** msg, int bufferSize) {
    //PERF: Would it be better to always probe and allocate messages for responses on the fly
    //rather than having all this book-keeping for receiving and reusing requests space ?
    //Sound we should get a pool of small messages (let say sizeof(ocrPolicyMsg_t) and allocate
    //variable size message on the fly).
    MPI_Status status;

#ifdef MPI_MSG
    MPI_Message mpiMsg;
#endif

    int available = 0;
#ifdef MPI_MSG
    RESULT_ASSERT(MPI_Improbe(src, tag, MPI_COMM_WORLD, &available, &mpiMsg, &status), ==, MPI_SUCCESS);
#else
    DPRINTF(DEBUG_LVL_VERB, "Probing message from src: %"PRId32" with tag: %"PRId32"\n",
        src, tag);
    RESULT_ASSERT(MPI_Iprobe(src, tag, MPI_COMM_WORLD, &available, &status), ==, MPI_SUCCESS);
#endif
    if (available) {
        ASSERT(msg != NULL);
        ASSERT((bufferSize == 0) ? ((tag == RECV_ANY_ID) && (*msg == NULL)) : 1);
        src = status.MPI_SOURCE; // Using MPI_ANY_SOURCE for the receive might get a different message
        // Look at the size of incoming message
        MPI_Datatype datatype = MPI_BYTE;
        int count;
        RESULT_ASSERT(MPI_Get_count(&status, datatype, &count), ==, MPI_SUCCESS);
        ASSERT(count != 0);
        // Reuse request's or allocate a new message if incoming size is greater.
        if (count > bufferSize) {
            *msg = allocateNewMessage(self, count);
        }
        ASSERT(*msg != NULL);
        MPI_Comm comm = MPI_COMM_WORLD;
#ifdef MPI_MSG
        RESULT_ASSERT(MPI_Mrecv(*msg, count, datatype, &mpiMsg, MPI_STATUS_IGNORE), ==, MPI_SUCCESS);
#else
        RESULT_ASSERT(MPI_Recv(*msg, count, datatype, src, tag, comm, MPI_STATUS_IGNORE), ==, MPI_SUCCESS);
#endif
        // After recv, the message size must be updated since it has just been overwritten.
        (*msg)->usefulSize = count;
        (*msg)->bufferSize = count;

        // This check usually fails in the 'ocrPolicyMsgGetMsgSize' when there
        // has been an issue in MPI. It manifest as a received buffer being complete
        // garbage whereas the sender doesn't detect any corruption of the message when
        // it is recycled. Tinkering with multiple MPI implementation it sounds the issue
        // is with the MPI library not being able to register a hook for malloc calls.
        ASSERT((((*msg)->type & (PD_MSG_REQUEST | PD_MSG_RESPONSE)) != (PD_MSG_REQUEST | PD_MSG_RESPONSE)) &&
           (((*msg)->type & PD_MSG_REQUEST) || ((*msg)->type & PD_MSG_RESPONSE)) &&
           "error: Try to link the MPI library first when compiling your OCR program");

#ifdef OCR_MONITOR_NETWORK
        (*msg)->rcvTime = salGetTime();
#endif

        // Unmarshall the message. We check to make sure the size is OK
        // This should be true since MPI seems to make sure to send the whole message
        u64 baseSize = 0, marshalledSize = 0;
        ocrPolicyMsgGetMsgSize(*msg, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
        ASSERT((baseSize+marshalledSize) == count);
        // The unmarshalling is just fixing up fields to point to the correct
        // payload address trailing after the base message.
        //BUG #604 Communication API extensions
        //1)     I'm thinking we can further customize un/marshalling for MPI. Because we use
        //       mpi tags, we actually don't need to send the header part of response message.
        //       We can directly recv the message at msg + header, update the msg header
        //       to be a response + flip src/dst.
        //2)     See if we can improve unmarshalling by keeping around pointers for the various
        //       payload to be unmarshalled
        //3)     We also need to deguidify all the fatGuids that are 'local' and decide
        //       where it is appropriate to do it.
        //       - REC: I think the right place would be in the user code (ie: not the comm layer)
        ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg,
                                  MARSHALL_APPEND | MARSHALL_NSADDR | MARSHALL_DBPTR);
        DPRINTF(DEBUG_LVL_VVERB, "Returning a message in %p\n", msg);
        return POLL_MORE_MESSAGE;
    }
    return POLL_NO_MESSAGE;
}
#endif

static u8 MPICommPollMessageInternal(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                              u32 properties, u32 *mask) {
    ocrPolicyDomain_t * pd = self->pd;
    ocrCommPlatformMPIProbe_t * mpiComm = ((ocrCommPlatformMPIProbe_t *) self);

    ASSERT(msg != NULL);
    ASSERT((*msg == NULL) && "MPI comm-layer cannot poll for a specific message");

    verifyOutgoing(mpiComm);

    // Iterate over incoming communications (mpi recvs)
    iterator_t * incomingIt = mpiComm->incomingIt;
    incomingIt->reset(incomingIt);
#if STRATEGY_PRE_POST_RECV
    bool debugIts = false;
#endif
    while (incomingIt->hasNext(incomingIt)) {
        mpiCommHandleBase_t * mpiHandleBase = (mpiCommHandleBase_t *) incomingIt->next(incomingIt);
        // Ignore anything that has to do with MT
        if(mpiHandleBase->isMtHandle) {
            continue;
        }
        mpiCommHandle_t* mpiHandle = (mpiCommHandle_t*)mpiHandleBase;

        //PERF: Would it be better to always probe and allocate messages for responses on the fly
        //rather than having all this book-keeping for receiving and reusing requests space ?
    #if STRATEGY_PROBE_RECV
        // Probe a specific incoming message. Response message overwrites the request one
        // if it fits. Otherwise, a new message is allocated. Upper-layers are responsible
        // for deallocating the request/response buffers.
        ocrPolicyMsg_t * reqMsg = mpiHandle->msg;
        u8 res = probeIncoming(self, mpiHandle->base.src, (int) mpiHandle->base.msgId, &mpiHandle->msg, mpiHandle->msg->bufferSize);
        // The message is properly unmarshalled at this point
        if (res == POLL_MORE_MESSAGE) {
#ifdef OCR_ASSERT
            if (reqMsg != mpiHandle->msg) {
                // Original request hasn't changed
                ASSERT((reqMsg->srcLocation == pd->myLocation) && (reqMsg->destLocation != pd->myLocation));
                // Newly received response
                ASSERT((mpiHandle->msg->srcLocation != pd->myLocation) && (mpiHandle->msg->destLocation == pd->myLocation));
            } else {
                // Reused, so it is the response
                ASSERT((reqMsg->srcLocation != pd->myLocation) && (reqMsg->destLocation == pd->myLocation));
            }
#endif
            if ((reqMsg != mpiHandle->msg) && mpiHandle->deleteSendMsg) {
                // we did allocate a new message to store the response
                // and the request message was already an internal copy
                // made by the comm-platform, hence the pointer is only
                // known here and must be deallocated. The sendMessage
                // caller still has a pointer to the original message.
                pd->fcts.pdFree(pd, reqMsg);
            }
            ASSERT(mpiHandle->msg->msgId == mpiHandle->base.msgId);
            *msg = mpiHandle->msg;
            pd->fcts.pdFree(pd, mpiHandle);
            incomingIt->removeCurrent(incomingIt);
            return res;
        }
    #endif
    #if STRATEGY_PRE_POST_RECV
        debugIts = true;
        int completed = 0;
        int ret = MPI_Test(&(mpiHandle->base.status), &completed, MPI_STATUS_IGNORE);
        ASSERT(ret == MPI_SUCCESS);
        if (completed) {
            ocrPolicyMsg_t * receivedMsg = mpiHandle->msg;
            u32 needRecvAny = (receivedMsg->type & PD_MSG_REQUEST);
            DPRINTF(DEBUG_LVL_VERB,"[MPI %"PRId32"] Received a message of type %"PRIx32" with msgId %"PRId32" \n",
                    locationToMpiRank(self->pd->myLocation), receivedMsg->type, (int) receivedMsg->msgId);
            // if request : msg may be reused for the response
            // if response: upper-layer must process and deallocate
            //BUG #604 Communication API extensions: There's no convenient way to let upper-layers know if msg can be reused
            *msg = receivedMsg;
            // We need to unmarshall the message here
            // Check the size for sanity (I think it should be OK but not sure in this case)
            u64 baseSize, marshalledSize;
            ocrPolicyMsgGetMsgSize(*msg, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
            ASSERT(baseSize + marshalledSize <= mpiComm->maxMsgSize);
            ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg,
                                      MARSHALL_APPEND | MARSHALL_DBPTR | MARSHALL_NSADDR);
            pd->fcts.pdFree(pd, mpiHandle);
            incomingIt->removeCurrent(incomingIt);
            if (needRecvAny) {
                // Receiving a request indicates a mpi recv any
                // has completed. Post a new one.
                postRecvAny(self);
            }
            return POLL_MORE_MESSAGE;
        }
    #endif
    }
#if STRATEGY_PRE_POST_RECV
    ASSERT(debugIts != false); // There should always be an irecv any posted
#endif
    u8 retCode = POLL_NO_MESSAGE;

#if STRATEGY_PROBE_RECV
    // Check for outstanding incoming. If any, a message is allocated
    // and returned through 'msg'.
    retCode = probeIncoming(self, MPI_ANY_SOURCE, RECV_ANY_ID, msg, 0);
    // Message is properly un-marshalled at this point
#endif
    if (retCode == POLL_NO_MESSAGE) {
        retCode |= (mpiComm->outgoing->isEmpty(mpiComm->outgoing)) ? POLL_NO_OUTGOING_MESSAGE : 0;
        retCode |= (mpiComm->incoming->isEmpty(mpiComm->incoming)) ? POLL_NO_INCOMING_MESSAGE : 0;
    }
    return retCode;
}


static u8 MPICommPollMessageInternalMT(ocrCommPlatform_t *self, pdEvent_t **outEvent,
                                u32 idx) {
    // We should not, at this point, be calling this as any continuation or back-processing
    ASSERT(idx == 0);
    ocrPolicyDomain_t * pd = self->pd;
    ocrCommPlatformMPIProbe_t * mpiComm = ((ocrCommPlatformMPIProbe_t *) self);

    // First, verify if outgoing messages went out OK
    RESULT_ASSERT(verifyOutgoing(mpiComm), ==, 0);

    // Next, check for each message we are expecting a response. Whenever there is a
    // matched receive, the event for that communication is marked ready, which
    // allows a blocked caller to proceed or a continuation on the communication
    // to become eligible for scheduling.
    RESULT_ASSERT(verifyIncomingResponsesMT(mpiComm, true), ==, POLL_NO_MESSAGE);

    // Here is where we actually check for messages that we are not
    // expecting and return them in outEvent
    ocrPolicyMsg_t *outMsg = NULL;
    u8 retCode = probeIncoming(self, MPI_ANY_SOURCE, RECV_ANY_ID, &outMsg, 0);

    // If we actually have a message, we create an event for it and mark
    // it as ready
    if(retCode == POLL_MORE_MESSAGE) {
        RESULT_ASSERT(pdCreateEvent(pd, outEvent, PDEVT_TYPE_MSG, 0), ==, 0);
        // We don't destroy deep for now because of compatibility with the
        // processIncomingMsg in the PD call that does the free of the message
        (*outEvent)->properties |= PDEVT_GC /* | PDEVT_DESTROY_DEEP */;
        ((pdEventMsg_t*)(*outEvent))->msg = outMsg;
        DPRINTF(DEBUG_LVL_VVERB, "[MPI %"PRId32"] found incoming unsolicited message, evt=%p, msg=%p\n",
                locationToMpiRank(pd->myLocation), *outEvent, outMsg);
        RESULT_ASSERT(pdMarkReadyEvent(pd, *outEvent), ==, 0);
    }

    // Message is properly un-marshalled at this point
    if (retCode == POLL_NO_MESSAGE) {
        retCode |= (mpiComm->outgoing->isEmpty(mpiComm->outgoing)) ? POLL_NO_OUTGOING_MESSAGE : 0;
        retCode |= (mpiComm->incoming->isEmpty(mpiComm->incoming)) ? POLL_NO_INCOMING_MESSAGE : 0;
    }
    return retCode;
}

static u8 MPICommPollMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                      u32 properties, u32 *mask) {
    ocrCommPlatformMPIProbe_t * mpiComm __attribute__((unused)) = ((ocrCommPlatformMPIProbe_t *) self);
    // Not supposed to be polled outside RL_USER_OK
    ASSERT_BLOCK_BEGIN(((mpiComm->curState >> 4) == RL_USER_OK))
    DPRINTF(DEBUG_LVL_WARN,"[MPI %"PRIu64"] Illegal runlevel[%"PRId32"] reached in MPI-comm-platform pollMessage\n",
            mpiRankToLocation(self->pd->myLocation), (mpiComm->curState >> 4));
    ASSERT_BLOCK_END
    return MPICommPollMessageInternal(self, msg, properties, mask);
}

static u8 MPICommPollMessageMT(ocrCommPlatform_t *self, pdEvent_t **outEvent, u32 index) {
    ocrCommPlatformMPIProbe_t * mpiComm __attribute__((unused)) = ((ocrCommPlatformMPIProbe_t *) self);
    // Not supposed to be polled outside RL_USER_OK
    ASSERT_BLOCK_BEGIN(((mpiComm->curState >> 4) == RL_USER_OK))
        DPRINTF(DEBUG_LVL_WARN,"[MPI %"PRIu64"] Illegal runlevel[%"PRId32"] reached in MPI-comm-platform pollMessage\n",
                mpiRankToLocation(self->pd->myLocation), (mpiComm->curState >> 4));
    ASSERT_BLOCK_END
    return MPICommPollMessageInternalMT(self, outEvent, index);
}

static u8 MPICommWaitMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                      u32 properties, u32 *mask) {
    u8 ret = 0;
    do {
        ret = self->fcts.pollMessage(self, msg, properties, mask);
    } while(ret != POLL_MORE_MESSAGE);

    return ret;
}

static u8 MPICommWaitMessageMT(ocrCommPlatform_t *self, pdEvent_t **outEvent, u32 index) {
    u8 ret = 0;
    do {
        ret = self->fcts.pollMessageMT(self, outEvent, index);
        // TODO: Do we want to process things that are ready in the strand table
        // This loop will block until we have an incomming message that
        // is either an initial request or a COMM_ONE_WAY.
    } while(ret != POLL_MORE_MESSAGE);

    return ret;
}

u8 MPICommSwitchRunlevel(ocrCommPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ocrCommPlatformMPIProbe_t * mpiComm = ((ocrCommPlatformMPIProbe_t *) self);
    u8 toReturn = 0;
    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    switch(runlevel) {
    case RL_CONFIG_PARSE:
    case RL_NETWORK_OK:
        // Nothing
        break;
    case RL_PD_OK:
        if ((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
            //Initialize base
            self->pd = PD;
            //BUG #605 Locations spec: commPlatform and worker have a location, are the supposed to be the same ?
            int rank=0;
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            DPRINTF(DEBUG_LVL_VERB,"[MPI %"PRId32"] comm-platform starts\n", rank);
            PD->myLocation = locationToMpiRank(rank);
        }
        break;
    case RL_MEMORY_OK:
        // Nothing to do
        break;
    case RL_GUID_OK:
        ASSERT(self->pd == PD);
        if((properties & RL_BRING_UP) && RL_IS_LAST_PHASE_UP(self->pd, RL_GUID_OK, phase)) {
            //BUG #602 multi-comm-worker: multi-initialization if multiple comm-worker
            //Initialize mpi comm internal queues
            mpiComm->msgId = 1;
            mpiComm->incoming = newLinkedList(PD);
            mpiComm->outgoing = newLinkedList(PD);
            mpiComm->incomingIt = mpiComm->incoming->iterator(mpiComm->incoming);
            mpiComm->outgoingIt = mpiComm->outgoing->iterator(mpiComm->outgoing);

            // Default max size is customizable through setMaxExpectedMessageSize()
#if STRATEGY_PRE_POST_RECV
            //Limitation: STRATEGY_PRE_POST_RECV doesn't support arbitrary message size
            mpiComm->maxMsgSize = sizeof(ocrPolicyMsg_t)*2;
#endif
#if STRATEGY_PROBE_RECV
            // Do not need that with probe
            ASSERT(mpiComm->maxMsgSize == 0);
#endif
            // Generate the list of known neighbors (All-to-all)
            //BUG #606 Neighbor registration: neighbor information should come from discovery or topology description
            int nbRanks;
            MPI_Comm_size(MPI_COMM_WORLD, &nbRanks);
            PD->neighborCount = nbRanks - 1;
            PD->neighbors = PD->fcts.pdMalloc(PD, sizeof(ocrLocation_t) * PD->neighborCount);
            ADebug(AllocDebugAllPD, "mpi-probe-comm-platform.c/MPICommSwitchRunlevel() "
                   "pdMalloc(%ld), addr=%p\n",
                (sizeof(ocrLocation_t) * PD->neighborCount), PD->neighbors);
            int myRank = (int) locationToMpiRank(PD->myLocation);
            int i = 0;
            while(i < (nbRanks-1)) {
                PD->neighbors[i] = mpiRankToLocation((myRank+i+1)%nbRanks);
                DPRINTF(DEBUG_LVL_VERB,"[MPI %"PRId32"] Neighbors[%"PRId32"] is %"PRIu64"\n", myRank, i, PD->neighbors[i]);
                i++;
            }
#ifdef DEBUG_MPI_HOSTNAMES
            char hostname[256];
            gethostname(hostname,255);
            PRINTF("MPI rank %"PRId32" on host %s\n", myRank, hostname);
#endif
            // Runlevel barrier across policy-domains
            MPI_Barrier(MPI_COMM_WORLD);

#if STRATEGY_PRE_POST_RECV
            // Post a recv any to start listening to incoming communications
            postRecvAny(self);
#endif
        }
        if ((properties & RL_TEAR_DOWN) && RL_IS_FIRST_PHASE_DOWN(self->pd, RL_GUID_OK, phase)) {
#if STRATEGY_PROBE_RECV
            iterator_t * incomingIt = mpiComm->incomingIt;
            incomingIt->reset(incomingIt);
            if (incomingIt->hasNext(incomingIt)) {
                mpiCommHandleBase_t * mpiHandle = (mpiCommHandleBase_t *) incomingIt->next(incomingIt);
                ocrPolicyMsg_t *msg = NULL;
                if(mpiHandle->isMtHandle) {
                    mpiCommHandleMt_t *t = (mpiCommHandleMt_t*)mpiHandle;
                    msg = t->myMsg?t->myMsg:((pdEventMsg_t*)(t->myStrand->curEvent))->msg;
                } else {
                    msg = ((mpiCommHandle_t*)mpiHandle)->msg;
                }
                self->pd->fcts.pdFree(self->pd, msg);
                self->pd->fcts.pdFree(self->pd, mpiHandle);
                incomingIt->removeCurrent(incomingIt);
            }
#endif
            ASSERT(mpiComm->incoming->isEmpty(mpiComm->incoming));
            mpiComm->incoming->destruct(mpiComm->incoming);
            iterator_t * outgoingIt = mpiComm->outgoingIt;
            outgoingIt->reset(outgoingIt);
            if (outgoingIt->hasNext(outgoingIt)) {
                mpiCommHandle_t * mpiHandle __attribute__((unused)) = (mpiCommHandle_t *) outgoingIt->next(outgoingIt);
                DPRINTF(DEBUG_LVL_WARN, "Shutdown: message of type %"PRIx32" has not been drained\n", (u32) (mpiHandle->msg->type & PD_MSG_TYPE_ONLY));
            }
            ASSERT(mpiComm->outgoing->isEmpty(mpiComm->outgoing));
            mpiComm->outgoing->destruct(mpiComm->outgoing);
            mpiComm->incomingIt->destruct(mpiComm->incomingIt);
            mpiComm->outgoingIt->destruct(mpiComm->outgoingIt);
            PD->fcts.pdFree(PD, PD->neighbors);
            PD->neighbors = NULL;
        }
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        // Note: This PD may reach this runlevel after other PDs. It is not
        // an issue for MPI since the library is already up and will buffer
        // the messages. The communication worker wll pick that up whenever
        // it has started
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    // Store the runlevel/phase in curState for debugging purpose
    mpiComm->curState = ((runlevel<<4) | phase);
    return toReturn;
}

//
// Init and destruct
//

static void MPICommDestruct (ocrCommPlatform_t * self) {
    //This should be called only once per rank and by the same thread that did MPI_Init.
    platformFinalizeMPIComm();
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

static ocrCommPlatform_t* newCommPlatformMPI(ocrCommPlatformFactory_t *factory,
                                       ocrParamList_t *perInstance) {
    ocrCommPlatformMPIProbe_t * commPlatformMPI = (ocrCommPlatformMPIProbe_t*)
    runtimeChunkAlloc(sizeof(ocrCommPlatformMPIProbe_t), PERSISTENT_CHUNK);
    //BUG #605 Locations spec: what is a comm-platform location ? is it the same as the PD ?
    commPlatformMPI->base.location = ((paramListCommPlatformInst_t *)perInstance)->location;
    commPlatformMPI->base.fcts = factory->platformFcts;
    factory->initialize(factory, (ocrCommPlatform_t *) commPlatformMPI, perInstance);
    return (ocrCommPlatform_t*) commPlatformMPI;
}


/******************************************************/
/* MPI COMM-PLATFORM FACTORY                          */
/******************************************************/

static void destructCommPlatformFactoryMPI(ocrCommPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

static void initializeCommPlatformMPI(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * base, ocrParamList_t * perInstance) {
    initializeCommPlatformOcr(factory, base, perInstance);
    ocrCommPlatformMPIProbe_t * mpiComm = (ocrCommPlatformMPIProbe_t*) base;
    mpiComm->msgId = 1; // all recv ANY use id '0'
    mpiComm->incoming = NULL;
    mpiComm->outgoing = NULL;
    mpiComm->incomingIt = NULL;
    mpiComm->outgoingIt = NULL;
    mpiComm->maxMsgSize = 0;
    mpiComm->curState = 0;
}

ocrCommPlatformFactory_t *newCommPlatformFactoryMPIProbe(ocrParamList_t *perType) {
    ocrCommPlatformFactory_t *base = (ocrCommPlatformFactory_t*)
        runtimeChunkAlloc(sizeof(ocrCommPlatformFactoryMPIProbe_t), NONPERSISTENT_CHUNK);
    base->instantiate = &newCommPlatformMPI;
    base->initialize = &initializeCommPlatformMPI;
    base->destruct = FUNC_ADDR(void (*)(ocrCommPlatformFactory_t*), destructCommPlatformFactoryMPI);

    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCommPlatform_t*), MPICommDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                  phase_t, u32, void (*)(ocrPolicyDomain_t*,u64), u64), MPICommSwitchRunlevel);
    base->platformFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,ocrLocation_t,
                                               ocrPolicyMsg_t*,u64*,u32,u32), MPICommSendMessage);
    base->platformFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,ocrPolicyMsg_t**,u32,u32*),
                                               MPICommPollMessage);
    base->platformFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,ocrPolicyMsg_t**,u32,u32*),
                                               MPICommWaitMessage);
    base->platformFcts.sendMessageMT = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,
                                                        pdEvent_t**,pdEvent_t*,u32), MPICommSendMessageMT);
    base->platformFcts.pollMessageMT = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,pdEvent_t**,u32),
                                                 MPICommPollMessageMT);
    base->platformFcts.waitMessageMT = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,pdEvent_t**,u32),
                                                 MPICommWaitMessageMT);
    return base;
}

#endif /* ENABLE_COMM_PLATFORM_MPI_PROBE */
