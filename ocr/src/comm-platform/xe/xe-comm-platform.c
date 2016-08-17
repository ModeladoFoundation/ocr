/*
 * This file is subject to the lixense agreement located in the file LICENSE
 * and cannot be distributed without it. This notixe cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_XE

#include "debug.h"

#include "ocr-comm-api.h"
#include "ocr-policy-domain.h"

#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"

#include "xe-comm-platform.h"

#include "mmio-table.h"
#include "xstg-map.h"
#include "xstg-msg-queue.h"

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
//     scratchpad and are 2*sizeof(u64) bytes in size.
//
// (b) Every receive stage starts with an F/E word, followed by
//     the address of the content.
//
// (c) xeCommSendMessage() has 2 cases:
//
//    (1) Send() of a persistent buffer -- the caller guarantees that
//        the buffer will hang around at least until a response for it
//        has been received. In this implementation, if a response is required,
//        the buffer will stick around so really the only time when we are not
//        in this case is if we have a message that does not require a response
//        and that is in an ephemeral buffer.
//
//    (2) Send() of an ephemeral buffer -- the caller can reclaim the
//        buffer as soon as Send() returns to it.
//
//    In both cases we:
//        - Atomically test&set the remote state to F (and error if already F)
//        - Write the address of the buffer. The message will be copied over
//          directly by the CE into its own local buffer.
//        - Alarm remote, freezing
//        - CE IRQ restarts XE clock immediately; Send() returns.
//
// (d) xeCommPollMessage() -- non-blocking receive
//
//     Check local stage's F/E word. If E, return empty. If F, return content.
//
// (e) xeCommWaitMessage() -- blocking receive
//
//     While local stage E, keep looping. (BUG #515: allow XEs to sleep)
//     Once it is F, return content.
//
// (f) xeCommDestructMessage() -- callback to notify received message was consumed
//
//     Atomically test & set local stage to E. Error if already E.
//

void xeCommDestruct (ocrCommPlatform_t * base) {
    runtimeChunkFree((u64)base, NULL);
}

u8 xeCommSwitchRunlevel(ocrCommPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        if((properties & RL_BRING_UP) && (RL_IS_FIRST_PHASE_UP(PD, RL_NETWORK_OK, phase))) {
#ifndef ENABLE_BUILDER_ONLY
            u64 i;
            ocrCommPlatformXe_t * cp = (ocrCommPlatformXe_t *)self;


            // Zero-out our stage for receiving messages
            for(i=AR_L1_BASE + MSG_QUEUE_OFFT; i<AR_L1_BASE + MSG_QUEUE_SIZE; i += sizeof(u64))
                *(volatile u64 *)i = 0ULL;
            DPRINTF(DEBUG_LVL_VVERB, "Zeroed out local addresses for incoming buffer @ 0x%"PRIx64" for size %"PRIu32"\n", AR_L1_BASE + MSG_QUEUE_OFFT, MSG_QUEUE_SIZE);

            // Remember which XE number we are
            cp->N = AGENT_FROM_ID(PD->myLocation) - ID_AGENT_XE0;

            // Pre-compute pointer to our stage at the CE
            cp->rq = (fsimCommSlot_t *)(BR_L1_BASE(ID_AGENT_CE) + MSG_QUEUE_OFFT + cp->N * MSG_QUEUE_SIZE);
            // Initialize it to empty
            cp->rq->status = FSIM_COMM_FREE_BUFFER;
            DPRINTF(DEBUG_LVL_VVERB, "Initializing receive queue for agent %"PRIu64" @ CE @ %p\n", cp->N, cp->rq);
#endif
        }
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            self->pd = PD;
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

u8 xeCommSendMessage(ocrCommPlatform_t *self, ocrLocation_t target,
                     ocrPolicyMsg_t *message, u64 *id,
                     u32 properties, u32 mask) {

#ifndef ENABLE_BUILDER_ONLY
    ASSERT(self != NULL);
    ASSERT(message != NULL && message->bufferSize != 0);

    ocrCommPlatformXe_t * cp = (ocrCommPlatformXe_t *)self;

    // For now, XEs only sent to their CE; make sure!
    if(target != self->pd->parentLocation)
        DPRINTF(DEBUG_LVL_WARN, "XE trying to send to %"PRIx64" not parent %"PRIx64"\n", target, self->pd->parentLocation);
    ASSERT(target == self->pd->parentLocation);

    // - Atomically test & set remote stage to Busy. Error if already non-Empty.
    DPRINTF(DEBUG_LVL_VVERB, "XE trying to grab its remote slot @ %p\n", cp->rq);
    u64 tmp = FSIM_COMM_RSVRD_BUFFER;
    do {
        tmp = hal_cmpswap64(&(cp->rq->status), FSIM_COMM_FREE_BUFFER, FSIM_COMM_RSVRD_BUFFER);
    } while(tmp != FSIM_COMM_FREE_BUFFER);
    DPRINTF(DEBUG_LVL_VVERB, "XE successful at grabbing remote slot @ %p\n", cp->rq);

    // We calculate the full-size of the message. We do this here because it is
    // local and therefore faster than doing it from the CE
    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, 0);
    DPRINTF(DEBUG_LVL_VERB, "Got size of message %p: base:%"PRIu64" addl:%"PRIu64"\n",
        message, baseSize, marshalledSize);
    cp->rq->size = baseSize + marshalledSize;

    // We are also going to marshall things locally so that the CE only
    // needs to copy things over once
    ocrPolicyMsg_t *tmsg = NULL;
    if(message->bufferSize >= baseSize + marshalledSize) {
        DPRINTF(DEBUG_LVL_VVERB, "Message @ %p is large enough -- marshalling in it\n", message);
        // We marshall the message in-place
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)message, MARSHALL_APPEND);
        // All addresses returned by allocators are socket relative so I should be good
        // except if the message was on the stack in which case we need to make it block relative
        u64 maddr = (u64)message;
        if(!(maddr & _SR_LEAD_ONE)) {
            DPRINTF(DEBUG_LVL_VVERB, "Address %p is not socket relative -- normalizing\n");
            // Not socket relative. Check if AR. All other cases are OK
            if(maddr & _AR_LEAD_ONE) {
                maddr = BR_L1_BASE(cp->N + ID_AGENT_XE0) + maddr - AR_L1_BASE;
                DPRINTF(DEBUG_LVL_VVERB, "Making address block-relative to 0x%"PRIx64"\n", maddr);
            }
            cp->rq->addr = (ocrPolicyMsg_t*)maddr;
        } else {
            cp->rq->addr = message;
        }
    } else {
        // Here, we create a buffer that is large enough and we marshall everything in it
        tmsg = (ocrPolicyMsg_t*)self->pd->fcts.pdMalloc(self->pd, baseSize + marshalledSize);
        getCurrentEnv(NULL, NULL, NULL, tmsg);
        tmsg->bufferSize = baseSize + marshalledSize;
        DPRINTF(DEBUG_LVL_VVERB, "Message @ %p is too small -- creating additional buffer @ %p of size 0x%"PRIu64"\n",
            message, tmsg, baseSize + marshalledSize);
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)tmsg, MARSHALL_FULL_COPY);
        cp->rq->addr = tmsg;
    }
    hal_fence(); // Let's be safe.
    // Tell the CE that it can go and read it
    DPRINTF(DEBUG_LVL_VERB, "Informing the CE with addr: %p, size: %"PRIu64"\n", cp->rq->addr, cp->rq->size);
    RESULT_ASSERT(hal_swap64(&(cp->rq->status), FSIM_COMM_FULL_BUFFER), ==, FSIM_COMM_RSVRD_BUFFER);

    // - Alarm remote to tell the CE it has something from us. The message will
    // not be processed until this happens (the swap above is just used as an additional
    // check but does not trigger the CE)
    __asm__ __volatile__("alarm %0\n\t" : : "L" (XE_MSG_READY));
    DPRINTF(DEBUG_LVL_VERB, "RELEASED\n");
    if(tmsg) {
        DPRINTF(DEBUG_LVL_VERB, "Freeing temporary buffer @ %p\n", tmsg);
        self->pd->fcts.pdFree(self->pd, tmsg);
    }
#endif

    return 0;
}

u8 xeCommPollMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                     u32 properties, u32 *mask) {

    ASSERT(self != NULL);
    ASSERT(msg != NULL);

    // Local stage is at well-known address
    fsimCommSlot_t * lq = (fsimCommSlot_t*)(AR_L1_BASE + MSG_QUEUE_OFFT);

    // Check local stage's Empty/Busy/Full word. If non-Full, return; else, return content.
    if(lq->status != FSIM_COMM_FULL_BUFFER) {
        return POLL_NO_MESSAGE;
    }

    // We check the size of the message and create an appropriate sized one here
    u64 totalSize = lq->size;
    *msg = (ocrPolicyMsg_t*)self->pd->fcts.pdMalloc(self->pd, totalSize);
    DPRINTF(DEBUG_LVL_VERB, "Poll: Created local receive buffer of size %"PRIu64" @ %p to receive %p\n",
        totalSize, *msg, lq->addr);

    // Copy the message fully into our buffer
    hal_memCopy(*msg, lq->addr, totalSize, false);
    DPRINTF(DEBUG_LVL_VVERB, "Poll: Copied message from %p to %p\n", lq->addr, *msg);

    // At this point, we fix-up all the pointers.
    ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg, MARSHALL_APPEND);
    DPRINTF(DEBUG_LVL_VERB, "Poll: Found full message @ %p of type 0x%"PRIx32"\n", *msg, (*msg)->type);

    // We set the local address to our newly created buffer so we can properly destroy it later
    lq->laddr = *msg;
    return 0;
}

u8 xeCommWaitMessage(ocrCommPlatform_t *self,  ocrPolicyMsg_t **msg,
                     u32 properties, u32 *mask) {

    DPRINTF(DEBUG_LVL_VERB, "Waiting for message\n");
    ASSERT(self != NULL);
    ASSERT(msg != NULL);

    // Local stage is at well-known address
    fsimCommSlot_t* lq = (fsimCommSlot_t*)(AR_L1_BASE + MSG_QUEUE_OFFT);

    // While local stage non-Full, keep looping.
    // BUG #515: Sleep
    while(lq->status != FSIM_COMM_FULL_BUFFER)
        ;

    // We check the size of the message and create an appropriate sized one here
    u64 totalSize = lq->size;
    *msg = (ocrPolicyMsg_t*)self->pd->fcts.pdMalloc(self->pd, totalSize);
    DPRINTF(DEBUG_LVL_VERB, "Wait: Created local receive buffer of size %"PRIu64" @ %p to receive %p\n",
        totalSize, *msg, lq->addr);

    // Copy the message fully into our buffer
    hal_memCopy(*msg, lq->addr, totalSize, false);
    DPRINTF(DEBUG_LVL_VVERB, "Wait: Copied message from %p to %p\n", lq->addr, *msg);

    // Marshall message from the CE's memory back into the buffer we allocated
    ocrPolicyMsgUnMarshallMsg((u8*)*msg, NULL, *msg, MARSHALL_APPEND);
    DPRINTF(DEBUG_LVL_VERB, "Wait: Found full message @ %p of type 0x%"PRIx32"\n", *msg, (*msg)->type);

    // We set the local address to our newly created buffer so we can properly destroy it later
    lq->laddr = *msg;
    return 0;
}

u8 xeCommSetMaxExpectedMessageSize(ocrCommPlatform_t *self, u64 size, u32 mask) {
    ASSERT(0);
    return 0;
}

u8 xeCommDestructMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg) {

    ASSERT(self != NULL);
    ASSERT(msg != NULL);
    DPRINTF(DEBUG_LVL_VERB, "Resetting incomming message buffer, freeing %p\n", msg);
#ifndef ENABLE_BUILDER_ONLY
    // Local stage is at well-known address
    fsimCommSlot_t *lq = (fsimCommSlot_t*)(AR_L1_BASE + MSG_QUEUE_OFFT);
    ASSERT(msg == lq->laddr); // We should only be destroying the message we received
    self->pd->fcts.pdFree(self->pd, msg);
    // Atomically test & set local stage to "clean-up". Error if prev not Full.
    // This will inform the CE that it needs to potentially free its local copy of the message
    {
        RESULT_ASSERT(hal_swap64(&(lq->status), FSIM_COMM_CLEANUP_BUFFER), ==, FSIM_COMM_FULL_BUFFER);
    }
#endif

    return 0;
}

ocrCommPlatform_t* newCommPlatformXe(ocrCommPlatformFactory_t *factory,
                                     ocrParamList_t *perInstance) {

    ocrCommPlatformXe_t * commPlatformXe = (ocrCommPlatformXe_t*)
                                           runtimeChunkAlloc(sizeof(ocrCommPlatformXe_t), PERSISTENT_CHUNK);
    ocrCommPlatform_t * base = (ocrCommPlatform_t *) commPlatformXe;
    factory->initialize(factory, base, perInstance);
    return base;
}

void initializeCommPlatformXe(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * base, ocrParamList_t * perInstance) {
    initializeCommPlatformOcr(factory, base, perInstance);
}

/******************************************************/
/* OCR COMP PLATFORM PTHREAD FACTORY                  */
/******************************************************/

void destructCommPlatformFactoryXe(ocrCommPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrCommPlatformFactory_t *newCommPlatformFactoryXe(ocrParamList_t *perType) {
    ocrCommPlatformFactory_t *base = (ocrCommPlatformFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrCommPlatformFactoryXe_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newCommPlatformXe;
    base->initialize = &initializeCommPlatformXe;
    base->destruct = &destructCommPlatformFactoryXe;

    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCommPlatform_t*), xeCommDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), xeCommSwitchRunlevel);
    base->platformFcts.setMaxExpectedMessageSize = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, u64, u32),
                                                             xeCommSetMaxExpectedMessageSize);
    base->platformFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrLocation_t,
                                                      ocrPolicyMsg_t *, u64*, u32, u32), xeCommSendMessage);
    base->platformFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t**, u32, u32*),
                                               xeCommPollMessage);
    base->platformFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t**, u32, u32*),
                                               xeCommWaitMessage);
    base->platformFcts.destructMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t*),
                                                   xeCommDestructMessage);

    return base;
}
#endif /* ENABLE_COMM_PLATFORM_XE */
