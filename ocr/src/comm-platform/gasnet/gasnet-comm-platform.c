/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_GASNET

#include "debug.h"

#include "ocr-sysboot.h"
#include "ocr-policy-domain.h"
#include "ocr-worker.h"

#include "utils/ocr-utils.h"

#include "amhandler.h"
#include "gasnet-comm-platform.h"
#include "gasnet-policy-domain.h"
#include "gasnet-am.h"
#include "gasnet-share-segment.h"

#include <gasnet.h>

#define DEBUG_TYPE COMM_PLATFORM
#define MINHEAPOFFSET (128*4096)

#define IS_MSG_TYPE(msg, msg_type) ((msg->type & PD_MSG_TYPE_ONLY) ==  msg_type)

// For upper-level platforms
#define SEND_ANY_ID 0

/*
 * @brief Message allocation
 */
static ocrPolicyMsg_t * allocateNewMessage(ocrCommPlatform_t * self, u32 size) {
    ocrPolicyDomain_t * pd = self->pd;
    ocrPolicyMsg_t * message = pd->fcts.pdMalloc(pd, size);
    ADebug(AllocDebugAllPD, "gasnet-comm-platform.c/allocateNewMessage() "
           "pdMalloc(%ld), addr=%p\n", size, message);
    initializePolicyMessage(message, size);
    return message;
}

/*
 * @brief Translate from OCR rank to GASNet rank
 */
static inline int locationToGasnetRank(ocrLocation_t target) {
    return ((int) target);
}

/*
 * @brief Translate from GASNet rank to OCR rank
 */
static inline ocrLocation_t GasnetRankToLocation(int rank) {
    return (ocrLocation_t) rank;
}

/*
 * @brief To be called by an active message from remote process
 */
static void gasnetMessageIncoming(ocrCommPlatformGasnet_t *platform , ocrPolicyMsg_t *msg, uint64_t size,
       gasnet_handlerarg_t seg_addr_hi, gasnet_handlerarg_t seg_addr_lo,
       gasnet_handlerarg_t seg_size) {
    ocrCommPlatform_t * self = (ocrCommPlatform_t*) platform;

    // -------------------------------------------------------------
    // copy the message from share segment into local segment
    // this can be slow and require more memory, but it's simpler
    // than using the share segment within OCR
    // -------------------------------------------------------------
    ocrPolicyMsg_t * msgCopy = allocateNewMessage(self, size);
    hal_memCopy(msgCopy, msg, size, false);
    msgCopy->bufferSize = size;

    // once we finished copying the message into a local buffer,
    // other nodes are free to write this share segment

    DPRINTF(DEBUG_LVL_VERB,"[GASNET] Received a message type 0x%"PRIx32" with msgId 0x%"PRIu64" size: 0x%"PRIu64"\n",
                           msg->type, msg->msgId, size);
    hal_lock(&platform->queueLock);
    platform->incoming->pushFront(platform->incoming, msgCopy);
    hal_unlock(&platform->queueLock);

    // Warning ! Do NOT touch msgCopy from now on

    // the message is to acquire a datablock. If we do receive a datablock (a response)
    // then we need to free the reserved share segment block
    if (IS_MSG_TYPE(msg, PD_MSG_DB_ACQUIRE)) {
        if (IS_MSG_TYPE(msg, PD_MSG_RESPONSE)) {
            // free this segment block for other usages
            gasnetReleaseSegmentBlock((void*)msg);
        }
    }
    void *addr = (void*) getBits64(seg_addr_hi, seg_addr_lo);

    // if the remote process provides an address to its share segment, we need to
    // store it in case we need to write a long message directly to the segment
    if (addr != NULL) {
        ocrPolicyDomain_t * pd = self->pd;
        gasnetCommBlock_t *block = (gasnetCommBlock_t*) pd->fcts.pdMalloc(pd, sizeof(gasnetCommBlock_t));
        ADebug(AllocDebugAllPD, "gasnet-comm-platform.c/gasnetMessageIncoming() "
               "pdMalloc(%ld), addr=%p\n", sizeof(gasnetCommBlock_t), block);
        block->addr = addr;
        block->size = seg_size;
        int src = locationToGasnetRank( msg->srcLocation );
        gasnetSegmentBlockPush(pd, src, block);
        DPRINTF(DEBUG_LVL_VVERB, "[GASNET] pushing %p from 0x%"PRIx32" ( 0x%"PRIx32" | 0x%"PRIx32") \n", addr, src, (u32)seg_addr_hi, (u32)seg_addr_lo);
    }
}

// ---------------------------------
// Active messages definition
// ---------------------------------

/**
 * @brief function to be invoked for medium-size message
 */
static void gasnetAMMessageMedium(gasnet_token_t token, void *buf, size_t nbytes,
                                  gasnet_handlerarg_t id, gasnet_handlerarg_t seg_addr_hi,
                                  gasnet_handlerarg_t seg_addr_lo, gasnet_handlerarg_t seg_size) {
  gasnet_node_t src;
  GASNET_Safe( gasnet_AMGetMsgSource(token, &src) );

  ocrCommPlatformGasnet_t *platform = getCommPlatform();

  DPRINTF(DEBUG_LVL_VVERB,"[GASNET] %s: pd=%p from %"PRId32", size: %"PRId64"\n",
                          __func__, platform->base.pd, src, nbytes);

  gasnetMessageIncoming(platform, (ocrPolicyMsg_t *)buf, nbytes, seg_addr_hi,
                        seg_addr_lo, seg_size);
}


// ---------------------------------
// declaring the function shippings
// ---------------------------------

AMHANDLER_REGISTER(gasnetAMMessageMedium);

// ---------------------------------
// amhandler_register_invoke: function to invoke function shippings
// ---------------------------------
#define AMHANDLER_ENTRY(handler)  AMH_REGISTER_FCN(handler)();

static void amhandler_register_invoke(void) {
    AMHANDLER_ENTRY(gasnetAMMessageMedium);
    registerGasnetHandler();
}

#undef AMHANDLER_ENTRY

/*
 * @brief platform initializaton
 */
void platformInitGasnetComm(int * argc, char *** argv) {

    gasnet_handlerentry_t * table;
    int count = 0;
    uintptr_t maxLocalSegSize;

    // initialize GASNet
    GASNET_Safe( gasnet_init(argc, argv) ) ;

    // initialize AM functions
    amhandler_register_invoke();
    table = amhandler_table();
    count = amhandler_count();

    maxLocalSegSize = gasnet_getMaxLocalSegmentSize();
    int segSize = GASNET_PAGESIZE * 64;
    segSize = (maxLocalSegSize<segSize ? maxLocalSegSize : segSize);
    int minheap = MINHEAPOFFSET;

    GASNET_Safe( gasnet_attach(table, count, segSize, minheap));

    gasnetBlockInit();
}

/*
 * @brief send a medium message to a remote node.
 *
 *  If a message is a take type, we should suffix it with a share segment block and its
 *  maximum size. This segment block will be used to communicate via long message
 *  by the remote node
 */
static void sendMediumMessage(ocrCommPlatform_t * self, int targetRank, u64 gasnetId,
      ocrPolicyMsg_t * message, u64 bufferSize,
      gasnet_handlerarg_t addr_hi, gasnet_handlerarg_t addr_lo,
      u32 segment_size) {
    // if the request is "asking a data", we need also to provide our shared segment
    // so that the owner of the data can send directly to the shared buffer
    if (IS_MSG_TYPE(message, PD_MSG_DB_ACQUIRE)) {
        if (IS_MSG_TYPE(message, PD_MSG_RESPONSE)) {
            // it's a response from PD_MSG_DB_ACQUIRE, but we don't need share segment
            // since the the data we requested is not big, let's free the block.
            gasnetCommBlock_t *block = gasnetSegmentBlockGet(self->pd, targetRank);
            // sometimes we don't get the gasnet share segment due to insufficient memory
            // or because the message can be small. In this case we don't need to free
            if (block != NULL) {
                self->pd->fcts.pdFree(self->pd, block);
            }
        }
    }
    // According to spec, GASNet will copy the message into its buffer
    GASNET_Safe( gasnet_AMRequestMedium4(targetRank, AMHANDLER(gasnetAMMessageMedium),
                                         message, bufferSize, gasnetId,
                                         addr_hi, addr_lo, segment_size) );
}

//-------------------------------------------------------------------------------------------
// Comm life-cycle functions
//-------------------------------------------------------------------------------------------

/*
 * @brief Sending a message through gasnet am handler
 *
 * This function will send a message to a target node using an appropriate handler.
 * If the size of the message is small or medium, we use am medium handler. Otherwise we
 * use long message or huge message for long and huge size respectively.
 */
static u8 GasnetCommSendMessage(ocrCommPlatform_t * self,
                                ocrLocation_t target, ocrPolicyMsg_t * message,
                                u64 *id, u32 properties, u32 mask) {

    u64 bufferSize = message->bufferSize;
    ocrCommPlatformGasnet_t * gasnetComm = ((ocrCommPlatformGasnet_t *) self);

    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
    u64 fullMsgSize = baseSize + marshalledSize;

    //BUG #602 multi-comm-worker: msgId incr only works if a single comm-worker per rank,
    // Do we want OCR to provide PD, system level counters ?
    // Always generate an identifier for a new communication to give back to upper-layer
    u64 gasnetId = gasnetComm->msgId++;

    // If we're sending a request, set the message's msgId to this communication id
    if (message->type & PD_MSG_REQUEST) {
        message->msgId = gasnetId;
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
    if ((fullMsgSize > bufferSize) || !(properties & PERSIST_MSG_PROP)) {
        // Allocate message and marshall a copy
        messageBuffer = allocateNewMessage(self, fullMsgSize);
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)messageBuffer,
            MARSHALL_FULL_COPY | MARSHALL_DBPTR | MARSHALL_NSADDR);
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

    // Prepare GASNET call arguments
    int targetRank = locationToGasnetRank(target);
    ASSERT(targetRank > -1);
    DPRINTF(DEBUG_LVL_VVERB,"GasnetCommSendMessage self=%p, to %"PRId64" (%"PRId32") %"PRId64" bytes\n",
                            self, target, targetRank, fullMsgSize);

    void * segment_addr = NULL;
    u32 segment_size = 0;

    if (IS_MSG_TYPE(messageBuffer, PD_MSG_DB_ACQUIRE))
    {
      // for PD_MSG_RESPONSE:
      //   we need to reserve a share segment so that the remote node can write it there
      //   for releasing the data (PD_MSG_RELEASE_DB)
      // for msg request:
      //   this message is to request a datablock, we need to prepare a share
      //   segment in case the data we request is big
      //
      // get any available segment memory, needed for both response and request
      gasnetCommBlockSender_t *block = gasnetReserveSegmentBlock();
      if (block != NULL) {
          segment_addr = block->block.addr;
          segment_size = block->block.size;
      }
      DPRINTF(DEBUG_LVL_VVERB,"[%"PRId32"] msg type: %"PRIx32"   s: %p\n", gasnet_mynode(),
                              message->type, segment_addr);
    }
    gasnet_handlerarg_t addr_hi = (gasnet_handlerarg_t) BITS64_HIGH(segment_addr);
    gasnet_handlerarg_t addr_lo = (gasnet_handlerarg_t) BITS64_LOW(segment_addr);

    // ------------------------------------------------------------
    // if the length of the message is less than the maximum of medium gasnet,
    // we'll use gasnet_AMRequestMedium1.
    // if the length is bigger then medium size and less than long size,
    //  we'll use gasnet_AMRequestLong1 (which use address in segment)
    // Otherwise, we send an error message.
    // ------------------------------------------------------------
    const int max_medium_size = gasnet_AMMaxMedium();

    if (max_medium_size > fullMsgSize) {
        sendMediumMessage( self, targetRank, gasnetId, messageBuffer, fullMsgSize,
        addr_hi, addr_lo, segment_size);
    } else {
        // sending large messages require access to the remote share segment
        // (only when am long enabled)
        gasnetCommBlock_t *blockRemote = gasnetSegmentBlockGet(self->pd, targetRank);
        if (gasnet_AMMaxLongRequest() > fullMsgSize) {
            gasnetSendLongMessage(targetRank, messageBuffer, fullMsgSize, gasnetId, blockRemote,
                                  addr_hi, addr_lo, segment_size);
        } else {
        // at the moment we couldn't afford to have a huge message due to Gasnet limitation.
        // we need to deal with this by chunking the message.
        gasnetSendHugeMessage( targetRank, messageBuffer, fullMsgSize, gasnetId, blockRemote,
                              addr_hi, addr_lo, segment_size );
        }
        // sometimes we don't get the gasnet share segment due to insufficient memory
        // or because the message can be small. In this case we don't need to free
        if (blockRemote != NULL) {
            self->pd->fcts.pdFree(self->pd, blockRemote);
        }
    }

    // Whatever happened up there, the copy can always be deleted
    if (message != messageBuffer) {
        self->pd->fcts.pdFree(self->pd, messageBuffer);
    }

    // Regarding the original message
    // An outgoing request for a two-way should be on the user stack
    // An outgoing request for a one-way should have been copied (hence to be deleted)
    // An outgoing response for a two-way should be on the heap
    //    => What about if the response was larger than the request who deallocates ?
    if (!(message->type & PD_MSG_REQ_RESPONSE)) {
        // A one-way (req or resp) should have been made persistent in upper-layers
        ASSERT(properties & PERSIST_MSG_PROP);
        // Hence, free the original since we've just made a copy
        self->pd->fcts.pdFree(self->pd, message);
    }

    DPRINTF(DEBUG_LVL_VERB,"[GASNET] AM for msgId %"PRId64" type %"PRIx32" to rank %"PRId32"\n",
                           message->msgId, message->type, targetRank);
    *id = gasnetId;
    return 0;
}

/*
 * @brief Gasnet general polling message
 */
u8 GasnetCommPollMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                         u32 properties, u32 *mask) {
    ocrCommPlatformGasnet_t *gasnetComm = (ocrCommPlatformGasnet_t*) self;

    ASSERT(msg != NULL);
    ASSERT(*msg == NULL && "GASNet cannot poll for a specific message");

    // remove all incoming tasks in the check the queue
    GASNET_Safe(gasnet_AMPoll());


    hal_lock(&gasnetComm->queueLock);
    iterator_t * incomingIt = gasnetComm->incomingIt;
    incomingIt->reset(incomingIt);
    if (incomingIt->hasNext(incomingIt)) {
        *msg = (ocrPolicyMsg_t *) incomingIt->next(incomingIt);
        incomingIt->removeCurrent(incomingIt);
        hal_unlock(&gasnetComm->queueLock);

        u64 baseSize = 0, marshalledSize = 0;
        ocrPolicyMsgGetMsgSize(*msg, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);

        ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg,
                                  MARSHALL_APPEND | MARSHALL_NSADDR | MARSHALL_DBPTR);

        return POLL_MORE_MESSAGE;
    }

    pdLookingForWork(gasnetComm);
    u8 retCode = POLL_NO_MESSAGE;

    if (retCode == POLL_NO_MESSAGE) {
        retCode |= POLL_NO_OUTGOING_MESSAGE; // Gasnet immediately sends so there's no queue to check
        // This is protected by the lock so it may show up empty but doesn't mean
        // there are no outstanding puts blocked on the lock
        retCode |= (gasnetComm->incoming->isEmpty(gasnetComm->incoming)) ? POLL_NO_INCOMING_MESSAGE : 0;
    }
    hal_unlock(&gasnetComm->queueLock);

    // Message is properly un-marshalled at this point
    return retCode;
}

/*
 * @brief Blocking-wait until we receive a message
 */
u8 GasnetCommWaitMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                         u32 properties, u32 *mask) {
    u8 ret = 0;
    do {
        ret = self->fcts.pollMessage(self, msg, properties, mask);
    } while(ret != POLL_MORE_MESSAGE);

    return ret;
}

u8 GasnetCommSwitchRunlevel(ocrCommPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ocrCommPlatformGasnet_t * gasnetComm = ((ocrCommPlatformGasnet_t *) self);
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
            int rank=gasnet_mynode();
            PD->myLocation = GasnetRankToLocation(rank);
            registerMessage(gasnetMessageIncoming);
        }
        break;
    case RL_MEMORY_OK:
        // Nothing to do
        break;
    case RL_GUID_OK:
        ASSERT(self->pd == PD);
        if((properties & RL_BRING_UP) && RL_IS_LAST_PHASE_UP(self->pd, RL_GUID_OK, phase)) {
            gasnetComm->incoming = newLinkedList(PD);
            gasnetComm->incomingIt = gasnetComm->incoming->iterator(gasnetComm->incoming);

            addCommPlatform(self);

            // All-to-all neighbor knowledge
            int nbRanks = gasnet_nodes();
            PD->neighborCount = nbRanks - 1;
            PD->neighbors = PD->fcts.pdMalloc(PD, sizeof(ocrLocation_t) * PD->neighborCount);
            ADebug(AllocDebugAllPD, "gasnet-comm-platform.c/GasnetCommSwitchRunlevel() "
                   "pdMalloc(%ld), addr=%p\n",
                   (sizeof(ocrLocation_t) * PD->neighborCount), PD->neighbors);
            int myRank = (int) locationToGasnetRank(PD->myLocation);
            int i = 0;
            while(i < (nbRanks-1)) {
                PD->neighbors[i] = (((myRank+i+1)%nbRanks));
                DPRINTF(DEBUG_LVL_VERB,"[%"PRId32"] neighbors[%"PRId32"] is %"PRId64"\n", myRank, i, PD->neighbors[i]);
                i++;
            }
            // Runlevel barrier across policy-domains
            gasnet_barrier_notify(0,GASNET_BARRIERFLAG_ANONYMOUS);
            gasnet_barrier_wait(0,GASNET_BARRIERFLAG_ANONYMOUS);

            GASNET_Safe(gasnet_AMPoll());
        }
        if ((properties & RL_TEAR_DOWN) && RL_IS_FIRST_PHASE_DOWN(self->pd, RL_GUID_OK, phase)) {
            gasnetComm->incomingIt->destruct(gasnetComm->incomingIt);
            ASSERT(gasnetComm->incoming->isEmpty(gasnetComm->incoming));
            gasnetComm->incoming->destruct(gasnetComm->incoming);
        }
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        // Note: This PD may reach this runlevel after other PDs. It is not
        // an issue for gasnet since the library is already up and will buffer
        // the messages. The communication worker wll pick that up whenever
        // it has started.
        if((properties & RL_BRING_UP) && RL_IS_LAST_PHASE_UP(self->pd, RL_USER_OK, phase)) {
            gasnet_barrier_notify(0,GASNET_BARRIERFLAG_ANONYMOUS);
            gasnet_barrier_wait(0,GASNET_BARRIERFLAG_ANONYMOUS);
        }
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void GasnetCommDestruct(ocrCommPlatform_t * base) {
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
    // free resources allocated in am-handlers
    amhandler_free_table();
}

/*
 * @brief create a new comm-platform for gasnet
 */
ocrCommPlatform_t* newCommPlatformGasnet(ocrCommPlatformFactory_t *factory,
                                       ocrParamList_t *perInstance) {
    ocrCommPlatformGasnet_t * commPlatformGasnet = (ocrCommPlatformGasnet_t*)
    runtimeChunkAlloc(sizeof(ocrCommPlatformGasnet_t), PERSISTENT_CHUNK);
    commPlatformGasnet->base.location = ((paramListCommPlatformInst_t *)perInstance)->location;
    commPlatformGasnet->base.fcts = factory->platformFcts;
    factory->initialize(factory, (ocrCommPlatform_t *) commPlatformGasnet, perInstance);
    return (ocrCommPlatform_t*) commPlatformGasnet;
}


/******************************************************/
/*  GASNET COMM-PLATFORM FACTORY                      */
/******************************************************/

void destructCommPlatformFactoryGasnet(ocrCommPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

/*
 * @brief Callback to initialize the platform
 */
void initializeCommPlatformGasnet(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * base,
          ocrParamList_t * perInstance) {
    initializeCommPlatformOcr(factory, base, perInstance);
    ocrCommPlatformGasnet_t * gasnetComm = (ocrCommPlatformGasnet_t*) base;
    gasnetComm->msgId = 1;
    gasnetComm->incoming = NULL;
    gasnetComm->incomingIt = NULL;
    gasnetComm->queueLock = INIT_LOCK;
}

/*
 * @brief Function to define communication callbacks
 */
ocrCommPlatformFactory_t *newCommPlatformFactoryGasnet(ocrParamList_t *perType) {

    ocrCommPlatformFactory_t *base = (ocrCommPlatformFactory_t*)
    runtimeChunkAlloc(sizeof(ocrCommPlatformFactoryGasnet_t), NONPERSISTENT_CHUNK);
    base->instantiate = &newCommPlatformGasnet;
    base->initialize = &initializeCommPlatformGasnet;
    base->destruct = FUNC_ADDR(void (*)(ocrCommPlatformFactory_t*), destructCommPlatformFactoryGasnet);

    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCommPlatform_t*), GasnetCommDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                  phase_t, u32, void (*)(ocrPolicyDomain_t*,u64), u64), GasnetCommSwitchRunlevel);
    base->platformFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,ocrLocation_t,
                                               ocrPolicyMsg_t*,u64*,u32,u32), GasnetCommSendMessage);
    base->platformFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,ocrPolicyMsg_t**,u32,u32*),
                                               GasnetCommPollMessage);
    base->platformFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*,ocrPolicyMsg_t**,u32,u32*),
                                               GasnetCommWaitMessage);

    return base;
}

#endif //ENABLE_COMM_PLATFORM_GASNET
