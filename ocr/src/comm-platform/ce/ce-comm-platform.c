/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_CE

#include "debug.h"

#include "ocr-comp-platform.h"
#include "ocr-policy-domain.h"
#include "policy-domain/ce/ce-policy.h"

#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"

#include "ce-comm-platform.h"

#include "mmio-table.h"
#include "rmd-arch.h"
#include "rmd-map.h"
#include "rmd-msg-queue.h"
#include "rmd-mmio.h"

#define DEBUG_TYPE COMM_PLATFORM

//
// Hgh-Level Theory of Operation / Design
//
// Communication will always involve one local->remote copy of
// information. Whether it is a source initiated bulk DMA or a series
// of remote initiated loads, it *will* happen. What does *not* need
// to happen are any additional local copies between the caller and
// callee on the sending end.
//
// Hence:
//
// (a) Every XE has a local receive stage. Every CE has per-XE receive
//     stage.  All receive stages are at MSG_QUEUE_OFFT in the agent
//     scratchpad and are MSG_QUEUE_SIZE bytes.
//
// (b) Every receive stage starts with an F/E word, followed by
//     content.
//
// (c) ceCommSendMessage():
//
//     CEs do not initiate communication, they only respond. Hence,
//     Send() ops do not expect a reply (they *are* replies
//     themselves) and so they will always be synchronous and once
//     data has been shipped out the buffer passed into the Send() is
//     free to be reused.
//
//        - Atomically test & set remote stage to F. Error if already F.
//        - DMA to remote stage
//        - Send() returns.
//
//     NOTE: XE software needs to consume a response from its stage
//           before injecting another request to CE. Otherwise, there
//           is the possibility of a race in the likely case that the
//           netowrk & CE are much faster than an XE...
//
// (d) ceCommPollMessage() -- non-blocking receive
//
//     Check local stage's F/E word. If E, return empty. If F, return content.
//
// (e) ceCommWaitMessage() -- blocking receive
//
//     While local stage E, keep looping. (FIXME: rate-limit w/ timer?)
//     Once it is F, return content.
//
// (f) ceCommDestructMessage() -- callback to notify received message was consumed
//
//     Atomically test & set local stage to E. Error if already E.
//

// Ugly globals below, but would go away once FSim has QMA support trac #232
#define OUTSTANDING_CE_MSGS 4
u64 msgAddresses[OUTSTANDING_CE_MSGS] = {0xbadf00d, 0xbadf00d, 0xbadf00d, 0xbadf00d};
ocrPolicyMsg_t sendBuf, recvBuf; // Currently size of 1 msg each.
// TODO: The above need to be placed in the CE's scratchpad, but once QMAs are ready, should
// be no problem. trac #232

void ceCommsInit(ocrCommPlatform_t * commPlatform, ocrPolicyDomain_t * PD);
u8 ceCommDestructMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg);
u8 ceCommDestructCEMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg);
u8 ceCommCheckSeqIdRecv(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg);

u64 parentOf(u64 location) {
    // XE's parent is its CE
    if ((location & ID_AGENT_CE) != ID_AGENT_CE)
        return ((location & ~ID_AGENT_MASK ) | ID_AGENT_CE);
    else if (BLOCK_FROM_ID(location)) // Non-zero block has zero block as parent
        return (location & ~ID_BLOCK_MASK);
    else if (UNIT_FROM_ID(location)) // Non-zero unit has zero unit & zero block as parent
        return (location & ~ID_UNIT_MASK);
    return location;
}

void ceCommDestruct (ocrCommPlatform_t * base) {

    runtimeChunkFree((u64)base, NULL);
}

u8 ceCommSwitchRunlevel(ocrCommPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                      phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

#ifndef ENABLE_BUILDER_ONLY
    u64 i, tmp;
    s8 j;

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
        ceCommsInit(self, PD);
        break;
    case RL_PD_OK:
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        {
            for(i = 0; i < OUTSTANDING_CE_MSGS; i++)
                msgAddresses[i] = 0;
            // If I have any CE children, wait for all of them to be ready
            for(i = 0; i<PD->neighborCount; i++) {
                u64 target = PD->neighbors[i] & 0xFFFFFFFF;
                if(PD->myLocation == parentOf(target)) {
                    u64 *rmbox = (u64 *) (DR_CE_BASE(CHIP_FROM_ID(target),
                                      UNIT_FROM_ID(target), BLOCK_FROM_ID(target))
                                      + (u64)(msgAddresses));
                    while((*(volatile u64 *)(&rmbox[3])) != 0xfeedf00d) hal_pause();
                    ASSERT(rmbox[3] == 0xfeedf00d);
                    *(volatile u64 *)&rmbox[3] = 0;
                }
            }

            // Signal to my parent that I'm ready after all my children CE are ready
            if(PD->myLocation != PD->parentLocation) {
                msgAddresses[3] = 0xfeedf00d;
                while((*(volatile u64 *)(&msgAddresses[3])) != 0) hal_pause();
                ASSERT(*(volatile u64 *)(&msgAddresses[3]) == 0);
            }

            // Send a dummy mesg to get XEs to exit their barrier
            for(j = ((ocrPolicyDomainCe_t *)PD)->xeCount-1; j>=0; j--) {
                u64 * rq = ((ocrCommPlatformCe_t *)self)->rq[((u64)j) & ID_AGENT_MASK];

                (((ocrCommPlatformCe_t *)self)->lq[j])[0] = 0;
                (((ocrCommPlatformCe_t *)self)->lq[j])[1] = 0;
                // Wait till XE is in the barrier
                do {
                    tmp = rmd_ld64((u64)rq);
                } while(tmp != 0xfeedf00d);
                ASSERT(tmp == 0xfeedf00d);
                // Exit the XE out of the barrier
                tmp = rmd_mmio_xchg64((u64)rq, (u64)2);
                ASSERT(tmp == 0xfeedf00d);
            }
        }
        break;
    case RL_USER_OK:
        {
            for(i=0; i< OUTSTANDING_CE_MSGS; i++)
                msgAddresses[i] = 0xdead;

            recvBuf.type = 0xdead;
        }
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
#endif

    return toReturn;
}

void ceCommsInit(ocrCommPlatform_t * commPlatform, ocrPolicyDomain_t * PD) {

    u64 i;

    ASSERT(commPlatform != NULL && PD != NULL);

    ocrCommPlatformCe_t * cp = (ocrCommPlatformCe_t *)commPlatform;
    commPlatform->pd = PD;

    // Initialize the bufferSize properly for recvBuf and sendBuf
    initializePolicyMessage(&recvBuf, sizeof(ocrPolicyMsg_t));
    initializePolicyMessage(&sendBuf, sizeof(ocrPolicyMsg_t));

    // FIXME: HACK!!! HACK!!! HACK!!!
    // Because currently PD->Start() never returns, the CE cannot
    // Start() before booting its XEs. So, it boots the XEs and
    // Start()s only then. Which leads to a race between XEs Send()ing
    // to the CE and the CE initializing its comm-platform. The comm
    // buffers need to be cleared before use (otherwise you get
    // Full/Empty bit issues), but if we clear them here, we may clear
    // the first message sent by a fast XE that was started before us.
    // The HACK FIX is to clear the CE message buffers in RMDKRNL
    // before OCR on both CEs and XEs is started, so we shouldn't
    // clear it here now (commented out below.) Eventually, when the
    // Begin()/Start() issues are resolved and we can init properly
    // this HACK needs to be reversed...
    //
    // Zero-out our stage for receiving messages
    //for(i=MSG_QUEUE_OFFT; i<(MAX_NUM_XE * MSG_QUEUE_SIZE); i += sizeof(u64))
    //    *(volatile u64 *)i = 0;

    // Fill-in location tuples: ours and our parent's (the CE in FSIM)
    PD->myLocation = (ocrLocation_t)rmd_ld64(CE_MSR_BASE + CORE_LOCATION * sizeof(u64));
    commPlatform->location = PD->myLocation;
    hal_fence();
    // My parent is my unit's block 0 CE
    PD->parentLocation = (PD->myLocation & ~(ID_BLOCK_MASK|ID_AGENT_MASK)) | ID_AGENT_CE; // My parent is my unit's block 0 CE
    // If I'm a block 0 CE, my parent is unit 0 block 0 CE
    if ((PD->myLocation & ID_BLOCK_MASK) == 0)
        PD->parentLocation = (PD->myLocation & ~(ID_UNIT_MASK|ID_BLOCK_MASK|ID_AGENT_MASK))
                             | ID_AGENT_CE;
    // TODO: Generalize this to cover higher levels of hierarchy too. trac #231

    // Remember our PD in case we need to call through it later
    cp->pdPtr = PD;

    // Pre-compute pointer to our block's XEs' remote stages (where we send to)
    // Pre-compute pointer to our block's XEs' local stages (where they send to us)
    for(i=0; i<MAX_NUM_XE ; i++) {
        cp->rq[i] = (u64 *)(BR_XE_BASE(i) + MSG_QUEUE_OFFT);
        cp->lq[i] = (u64 *)((u64)MSG_QUEUE_OFFT + i * MSG_QUEUE_SIZE);
    }

    // Arbitrary first choice
    cp->pollq = 0;

    // Statically check stage area is big enough for 1 policy message + F/E word
    COMPILE_TIME_ASSERT(MSG_QUEUE_SIZE >= (sizeof(u64) + sizeof(ocrPolicyMsg_t)));
}

u8 ceCommSendMessageToCE(ocrCommPlatform_t *self, ocrLocation_t target,
                     ocrPolicyMsg_t *message, u64 *id,
                     u32 properties, u32 mask) {

    u32 i;
    u64 retval;
    u64 msgAbsAddr;
    static u64 msgId = 0xace00000000;         // Known signature to aid grepping
    if(msgId==0xace00000000)
        msgId |= (self->location & 0xFF)<<16; // Embed the src location onto msgId
    ASSERT(self->location != target);

    if(sendBuf.type) {
        // Check if remote target is already dead
        u64 *rmbox = (u64 *) (DR_CE_BASE(CHIP_FROM_ID(sendBuf.destLocation),
                              UNIT_FROM_ID(sendBuf.destLocation), BLOCK_FROM_ID(sendBuf.destLocation))
                              + (u64)(msgAddresses));
        if(rmbox[0] == 0xdead) sendBuf.type = 0;
        return 1;
    }

    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, 0);
    if(baseSize + marshalledSize > sendBuf.bufferSize) {

        DPRINTF(DEBUG_LVL_WARN, "Comm platform only handles messages up to size %lu (0x%lx)\n",
                sendBuf.bufferSize, &sendBuf);
        ASSERT(0);
    }

    message->msgId = msgId++;
    message->type |= PD_CE_CE_MESSAGE;
    ocrPolicyMsgMarshallMsg(message, baseSize, (u8 *)&sendBuf, MARSHALL_FULL_COPY);

    msgAbsAddr = DR_CE_BASE(CHIP_FROM_ID(self->location),
                            UNIT_FROM_ID(self->location), BLOCK_FROM_ID(self->location))
                            + (u64)(&sendBuf);

    u64 *rmbox = (u64 *) (DR_CE_BASE(CHIP_FROM_ID(target),
                          UNIT_FROM_ID(target), BLOCK_FROM_ID(target))
                          + (u64)(msgAddresses));
    u32 k = 0;
    do {
        for(i = 0; i<OUTSTANDING_CE_MSGS; i++, k++) {
            retval = hal_cmpswap64(&rmbox[i], 0, msgAbsAddr);
            if(retval == 0) break;    // Send successful
            else if(retval == 0xdead) {
                     sendBuf.type = 0;
                     return 2;        // Target dead, give up
            } else if(retval == 0xbadf00d) {
                     sendBuf.type = 0;
                     return 1;        // Target busy, retry later
            }
        }
    } while(retval && ((k<100)));
    // TODO: This value is arbitrary.
    // What is a reasonable number of tries before giving up?

    if(retval) {
        sendBuf.type = 0;
        DPRINTF(DEBUG_LVL_INFO, "Cancel send msg %lx type %lx, properties %x; (%lx->%lx)\n",
                                 message->msgId, message->type, properties, self->location,
                                 target);
        if(rmbox[0] == 0xdead) return 2; // Shutdown in progress - no retry
        return 1;                        // otherwise, retry send
    }
    DPRINTF(DEBUG_LVL_VVERB, "Sent msg %lx type %lx; (%lx->%lx)\n", message->msgId, message->type, self->location, target);
    return 0;
}

u8 ceCommSendMessage(ocrCommPlatform_t *self, ocrLocation_t target,
                     ocrPolicyMsg_t *message, u64 *id,
                     u32 properties, u32 mask) {
    ASSERT(self != NULL);
    ASSERT(message != NULL);
    ASSERT(target != self->location);

    ocrCommPlatformCe_t * cp = (ocrCommPlatformCe_t *)self;
    // If target is not in the same block, use a different function
    // FIXME: do the same for chip/unit/board as well, or better yet, a new macro
    if((self->location & ~ID_AGENT_MASK) != (target & ~ID_AGENT_MASK)) {
        message->seqId = self->fcts.getSeqIdAtNeighbor(self, target, 0);
        return ceCommSendMessageToCE(self, target, message, id, properties, mask);
    } else {
        message->seqId = 0; //For XE's the neighbor ID of the CE is always 0.
        // TODO: compute all-but-agent & compare between us & target
        // Target XE's stage (note this is in remote XE memory!)
        u64 * rq = cp->rq[((u64)target) & ID_AGENT_MASK];

        // - Check remote stage Empty/Busy/Full is Empty.
        {
            u64 tmp = rmd_ld64((u64)rq);
            if(tmp) return 1; // Temporary workaround till bug #134 is fixed
            ASSERT(tmp == 0);
        }

#ifndef ENABLE_BUILDER_ONLY
        // - DMA to remote stage
        // We marshall things properly
        u64 baseSize = 0, marshalledSize = 0;
        ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, 0);
        // We can only deal with the case where everything fits in the message
        if(baseSize + marshalledSize > message->bufferSize) {
            DPRINTF(DEBUG_LVL_WARN, "Comm platform only handles messages up to size %ld\n",
                    message->bufferSize);
            ASSERT(0);
        }
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)message, MARSHALL_APPEND);
        if(message->usefulSize > MSG_QUEUE_SIZE - sizeof(u64))
            DPRINTF(DEBUG_LVL_WARN, "Message of type %x is too large (%lx) for buffer (%lx)\n",
                                    message->type, message->usefulSize, MSG_QUEUE_SIZE-sizeof(u64));
        ASSERT(message->usefulSize <= MSG_QUEUE_SIZE - sizeof(u64));
        // - DMA to remote stage, with fence
        DPRINTF(DEBUG_LVL_VVERB, "DMA-ing out message to 0x%lx of size %d\n",
                (u64)&rq[1], message->usefulSize);
        rmd_mmio_dma_copyregion_async((u64)message, (u64)&rq[1], message->usefulSize);

        // - Fence DMA
        rmd_fence_fbm();
        // - Atomically test & set remote stage to Full. Error if already non-Empty.
        {
            u64 tmp = rmd_mmio_xchg64((u64)rq, (u64)2);
            ASSERT(tmp == 0);
        }
#endif
    }
    return 0;
}

u8 ceCommCheckSeqIdRecv(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg) {
    u64 mask = msg->srcLocation & self->location;
    msg->seqId = (mask >> fls64(mask)) & 0xF; // Assumption: that there are always <= 0xF neighbors
    return 0;
}

u8 ceCommCheckCEMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg) {
    u32 j;

    // Go through our mbox to check for valid messages
    for(j=0; j<OUTSTANDING_CE_MSGS; j++)
        if(msgAddresses[j]) {
            // We fixup pointers
            u64 baseSize = 0, marshalledSize = 0;
            ocrPolicyMsgGetMsgSize((ocrPolicyMsg_t*)msgAddresses[j], &baseSize, &marshalledSize, 0);
            if(baseSize + marshalledSize > recvBuf.bufferSize) {
                DPRINTF(DEBUG_LVL_WARN, "Comm platform only handles messages up to size %ld\n",
                                         recvBuf.bufferSize);
                ASSERT(0);
            }
            ocrPolicyMsgUnMarshallMsg((u8*)msgAddresses[j], NULL, &recvBuf, MARSHALL_FULL_COPY);
            ceCommCheckSeqIdRecv(self, (ocrPolicyMsg_t*)msgAddresses[j]);
            ceCommDestructCEMessage(self, (ocrPolicyMsg_t*)msgAddresses[j]);
            *msg = &recvBuf;

            return 0;
        }
    *msg = NULL;
    return 1;
}

u8 ceCommPollMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                     u32 properties, u32 *mask) {

    ASSERT(self != NULL);
    ASSERT(msg != NULL);

    ocrCommPlatformCe_t * cp = (ocrCommPlatformCe_t *)self;

    if(properties & PD_CE_CE_MESSAGE) { // Poll only for CE messages
        return ceCommCheckCEMessage(self, msg);
    } else ASSERT(0); // Nothing else supported

    // Local stage is at well-known 0x0
    u64 i = cp->pollq;

    // Check local stage's F/E word. If E, return empty. If F, return content.

    // Loop once over the local stages searching for a message
    do {
        // Check this stage for Empty/Busy/Full
        if((cp->lq[i])[0] == 2) {
            // Have one, break out
            break;
        } else {
            // Advance to next queue
            i = (i+1) % MAX_NUM_XE;
        }
    } while(i != cp->pollq);

    // Either we got a message in the current queue, or we ran through all of them and they're all empty
    if((cp->lq[i])[0] == 2) {
        // We have a message
        // Provide a ptr to the local stage's contents
        *msg = (ocrPolicyMsg_t *)&((cp->lq[i])[1]);
        ASSERT((*msg)->bufferSize <= MSG_QUEUE_SIZE - sizeof(u64));
        // We fixup pointers
        u64 baseSize = 0, marshalledSize = 0;
        ocrPolicyMsgGetMsgSize(*msg, &baseSize, &marshalledSize, 0);
        if(baseSize + marshalledSize > (*msg)->bufferSize) {
            DPRINTF(DEBUG_LVL_WARN, "Comm platform only handles messages up to size %ld\n",
                    (*msg)->bufferSize);
            ASSERT(0);
        }
        ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg, MARSHALL_APPEND);
        ceCommCheckSeqIdRecv(self, *msg);

        // Advance queue for next check
        cp->pollq = (i + 1) % MAX_NUM_XE;

        // One message being returned
        return 0;
    } else {
        // We ran through all of them and they're all empty
        return POLL_NO_MESSAGE;
    }
}

u8 ceCommWaitMessage(ocrCommPlatform_t *self,  ocrPolicyMsg_t **msg,
                     u32 properties, u32 *mask) {

    ASSERT(self != NULL);
    ASSERT(msg != NULL);

    ocrCommPlatformCe_t * cp = (ocrCommPlatformCe_t *)self;

    // Give priority to CE messages
    if(!ceCommCheckCEMessage(self, msg)) return 0;

    // Local stage is at well-known 0x0
    u32 i, j;

    DPRINTF(DEBUG_LVL_VVERB, "Going to wait for message (starting at %d)\n",
            cp->pollq);
    // Loop through the stages till we receive something
    for(i = cp->pollq, j=(cp->pollq - 1 + MAX_NUM_XE) % MAX_NUM_XE;
           (cp->lq[i])[0] != 2; i = (i+1) % MAX_NUM_XE) {
        // Halt the CPU, instead of burning rubber
        // An alarm would wake us, so no delay will result
        // TODO: REC: Loop around at least once if we get
        // an alarm to check all slots.
        // Note that a timer alarm wakes us up periodically
        if(i==j) {
             // Before going to sleep, check for CE messages
             if(!ceCommCheckCEMessage(self, msg)) return 0;
             __asm__ __volatile__("hlt\n\t");
        }
    }

#if 1
    // We have a message
    DPRINTF(DEBUG_LVL_VVERB, "Waited for message and got message from %d at 0x%lx\n",
            i, &((cp->lq[i])[1]));
    *msg = (ocrPolicyMsg_t *)&((cp->lq[i])[1]);
    // We fixup pointers
    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(*msg, &baseSize, &marshalledSize, 0);
    if(baseSize + marshalledSize > (*msg)->bufferSize) {
        DPRINTF(DEBUG_LVL_WARN, "Comm platform only handles messages up to size %ld\n",
                (*msg)->bufferSize);
        ASSERT(0);
    }
    ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg, MARSHALL_APPEND);
    ceCommCheckSeqIdRecv(self, *msg);
#else
    // We have a message
    // NOTE: For now we copy it into the buffer provided by the caller
    //       eventually when QMA arrives we'll move to a posted-buffer
    //       scheme and eliminate the copies.
    hal_memCopy(*msg, &((cp->lq[i])[1]), sizeof(ocrPolicyMsg_t), 0);
    hal_fence();
#endif

    // Set queue index to this queue, so we'll know what to "destruct" later...
    cp->pollq = i;

    // One message being returned
    return 0;
}

u8 ceCommDestructCEMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg) {
   u32 i;

   for(i = 0; i<OUTSTANDING_CE_MSGS; i++)
       if(msgAddresses[i] == (u64)msg) {
           DPRINTF(DEBUG_LVL_VVERB, "Destructing msg %lx\n", msg->msgId);
           msgAddresses[i] = 0;
           msg->type = 0;
           msg->msgId = 0xdead;
       }

   return 0;
}

u8 ceCommDestructMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg) {

    ASSERT(self != NULL);
    ASSERT(msg != NULL);

    ocrCommPlatformCe_t * cp = (ocrCommPlatformCe_t *)self;

    if(msg->type == 0xdead) return 0; // FIXME: This is needed to ignore shutdown
                                 // messages. To go away once #134 is fixed
    if(msg->type & PD_CE_CE_MESSAGE) {
        if (msg->destLocation == self->location)
            return ceCommDestructCEMessage(self, msg);
        else
            return 0;
    }

    // Remember XE number
    u64 n = cp->pollq;

    DPRINTF(DEBUG_LVL_VVERB, "Destructing message received from %d (un-clock gate)\n", n);

    // Sanity check we're "destruct"ing the right thing...
    ASSERT(msg == (ocrPolicyMsg_t *)&((cp->lq[cp->pollq])[1]));

    // "Free" stage
    (cp->lq[cp->pollq])[0] = 0;

    // Advance queue for next time
    cp->pollq = (cp->pollq + 1) % MAX_NUM_XE;

    {
        // Clear the XE pipeline clock gate while preserving other bits.
        u64 state = rmd_ld64(XE_MSR_BASE(n) + (FUB_CLOCK_CTL * sizeof(u64)));
        rmd_st64_async( XE_MSR_BASE(n) + (FUB_CLOCK_CTL * sizeof(u64)), state & ~0x10000000ULL );
    }

    return 0;
}

u64 ceGetSeqIdAtNeighbor(ocrCommPlatform_t *self, ocrLocation_t neighborLoc, u64 neighborId) {
    return UNINITIALIZED_NEIGHBOR_INDEX;
}

ocrCommPlatform_t* newCommPlatformCe(ocrCommPlatformFactory_t *factory,
                                     ocrParamList_t *perInstance) {

    ocrCommPlatformCe_t * commPlatformCe = (ocrCommPlatformCe_t*)
                                           runtimeChunkAlloc(sizeof(ocrCommPlatformCe_t), PERSISTENT_CHUNK);
    ocrCommPlatform_t * derived = (ocrCommPlatform_t *) commPlatformCe;
    factory->initialize(factory, derived, perInstance);
    return derived;
}

u8 ceCommSetMaxExpectedMessageSize(ocrCommPlatform_t *self, u64 size, u32 mask) {
    ASSERT(0);
    return 0;
}

void initializeCommPlatformCe(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * derived, ocrParamList_t * perInstance) {
    initializeCommPlatformOcr(factory, derived, perInstance);
}

/******************************************************/
/* OCR COMP PLATFORM PTHREAD FACTORY                  */
/******************************************************/

void destructCommPlatformFactoryCe(ocrCommPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrCommPlatformFactory_t *newCommPlatformFactoryCe(ocrParamList_t *perType) {
    ocrCommPlatformFactory_t *base = (ocrCommPlatformFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrCommPlatformFactoryCe_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newCommPlatformCe;
    base->initialize = &initializeCommPlatformCe;
    base->destruct = &destructCommPlatformFactoryCe;

    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCommPlatform_t*), ceCommDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), ceCommSwitchRunlevel);
    base->platformFcts.setMaxExpectedMessageSize = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, u64, u32),
                                                        ceCommSetMaxExpectedMessageSize);
    base->platformFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrLocation_t,
                                     ocrPolicyMsg_t *, u64*, u32, u32), ceCommSendMessage);
    base->platformFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t**, u32, u32*),
                                     ceCommPollMessage);
    base->platformFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t**, u32, u32*),
                                     ceCommWaitMessage);
    base->platformFcts.destructMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t*),
                                         ceCommDestructMessage);
    base->platformFcts.getSeqIdAtNeighbor = FUNC_ADDR(u64 (*)(ocrCommPlatform_t*, ocrLocation_t, u64),
                                                   ceGetSeqIdAtNeighbor);

    return base;
}
#endif /* ENABLE_COMM_PLATFORM_CE */

