/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_API_DELEGATE

#include "debug.h"

#include "ocr-errors.h"
#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"
#include "ocr-comm-platform.h"
#include "comm-api/delegate/delegate-comm-api.h"

#define DEBUG_TYPE COMM_API

// Controls how many times we try to poll for a reply before notifying the scheduler
#define MAX_ACTIVE_WAIT 100

void delegateCommDestruct (ocrCommApi_t * base) {
    if(base->commPlatform != NULL) {
        base->commPlatform->fcts.destruct(base->commPlatform);
        base->commPlatform = NULL;
    }
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
}

u8 delegateCommSwitchRunlevel(ocrCommApi_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*,u64), u64 val) {
    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    if(properties & RL_BRING_UP) {
        toReturn |= self->commPlatform->fcts.switchRunlevel(self->commPlatform, PD, runlevel, phase,
                                                            properties, NULL, 0);
    }

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        // Nothing
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            // We can now set our PD (before this, we couldn't because
            // "our" PD might not have been started
            self->pd = PD;
        }
        break;
    case RL_MEMORY_OK:
    case RL_GUID_OK:
    case RL_COMPUTE_OK:
    case RL_USER_OK:
        // Nothing to do
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }

    if(properties & RL_TEAR_DOWN) {
        toReturn |= self->commPlatform->fcts.switchRunlevel(self->commPlatform, PD, runlevel, phase,
                                                            properties, NULL, 0);
    }
    return toReturn;
}

static ocrPolicyMsg_t * allocateNewMessage(ocrCommApi_t * self, u32 size) {
    ocrPolicyDomain_t * pd = self->pd;
    ocrPolicyMsg_t * message = pd->fcts.pdMalloc(pd, size);
    ADebug(AllocDebugAllPD, "delegate-comm-api.c/allocateNewMessage() "
           "pdMalloc(%ld), addr=%p\n", size, message);
    initializePolicyMessage(message, size);
    return message;
}

/**
 * @brief Destruct a message handler created by this comm-platform
 */
void destructMsgHandlerDelegate(ocrMsgHandle_t * handler) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    pd->fcts.pdFree(pd, handler);
}

/**
 * @brief Create a message handle for this comm-platform
 */
ocrMsgHandle_t * createMsgHandlerDelegate(ocrCommApi_t *self, ocrPolicyMsg_t * message, u32 properties) {
    ocrMsgHandle_t * handle = (ocrMsgHandle_t *) self->pd->fcts.pdMalloc(self->pd, sizeof(delegateMsgHandle_t));
    ADebug(AllocDebugAllPD, "delegate-comm-api.c/createMsgHandlerDelegate() "
           "pdMalloc(%ld), addr=%p\n", sizeof(delegateMsgHandle_t), handle);
    ASSERT(handle != NULL);
    handle->msg = message;
    handle->response = NULL;
    handle->status = HDL_NORMAL;
    handle->destruct = &destructMsgHandlerDelegate;
    handle->commApi = self;
    handle->properties = properties;
    return handle;
}

u8 delegateCommInitHandle(ocrCommApi_t *self, ocrMsgHandle_t *handle) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/**
 * @brief delegate a message send operation to the scheduler.
 */
u8 delegateCommSendMessage(ocrCommApi_t *self, ocrLocation_t target,
                            ocrPolicyMsg_t *message,
                            ocrMsgHandle_t **handle, u32 properties) {
    START_PROFILE(commapi_delegateCommSendMessage);
    ocrPolicyDomain_t * pd = self->pd;
    // Message source/destination is corrupted
    ASSERT((pd->myLocation == message->srcLocation) && (target == message->destLocation));
    ASSERT(pd->myLocation != target); // Do not support sending to 'itself' (current PD).
    // If the message is not persistent and the marshall mode is set, we do the specified
    // copy. Otherwise it is just the mode the buffer has been copied in the first place.
    // Modified this to experiment with asynchronous remote edt creation
    if (!(properties & PERSIST_MSG_PROP)) {
        // Need to make a copy of the message. Recall that send is returning
        // as soon as the handle is handed over to the scheduler.
        ocrMarshallMode_t marshallMode = (ocrMarshallMode_t) GET_PROP_U8_MARSHALL(properties);
        if (marshallMode == 0) {
            marshallMode = MARSHALL_DUPLICATE; // impl choice
        }
        // NOTE: here we could support _APPEND or _ADDL although we would still
        //       have to create a new message anyway because of PERSIST_MSG_PROP
        ASSERT((marshallMode & MARSHALL_DUPLICATE) || (marshallMode & MARSHALL_FULL_COPY));
        u64 baseSize = 0, marshalledSize = 0;
        ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, marshallMode);
        u64 fullSize = baseSize + marshalledSize;
        ocrPolicyMsg_t * msgCpy = allocateNewMessage(self, fullSize);
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)msgCpy, marshallMode);
        message = msgCpy;
        // Indicates to entities down the line the message is now persistent
        // The rationale is that one-way calls are always deallocated by the
        // comm-platform and two-way calls are always poll/wait at some point
        // by a caller that must free the message.
        properties |= PERSIST_MSG_PROP;
    }

    ocrMsgHandle_t * handlerDelegate = createMsgHandlerDelegate(self, message, properties);
    DPRINTF(DEBUG_LVL_VVERB,"Delegate API: end message handle=%p, msg=%p, type=0x%"PRIx32"\n",
            handlerDelegate, message, message->type);
    // Give comm handle to policy-domain
    ocrFatGuid_t fatGuid;
    fatGuid.guid = NULL_GUID;
    fatGuid.metaDataPtr = handlerDelegate;
    PD_MSG_STACK(giveMsg);
    getCurrentEnv(NULL, NULL, NULL, &giveMsg);
#define PD_MSG (&giveMsg)
#define PD_TYPE PD_MSG_SCHED_NOTIFY
    giveMsg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_COMM_READY;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_COMM_READY).guid = fatGuid;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &giveMsg, false));
#undef PD_MSG
#undef PD_TYPE
    if (handle != NULL) {
        // Set the handle for the caller
        *handle = handlerDelegate;
    }
    RETURN_PROFILE(0);
}

/**
 * @brief poll message is a no-op in this delegate implementation
 *
 * Depending on the calling context, the handle can either be:
 *  - handle == NULL  (Impl specific, likely poll for "progress" to be made)
 *  - *handle == NULL (Poll any and return message)
 *  - *handle != NULL (Poll for a specific handle completion)
 */
u8 delegateCommPollMessage(ocrCommApi_t *self, ocrMsgHandle_t **handle) {
    START_PROFILE(commapi_delegateCommPollMessage);
    ocrPolicyDomain_t * pd = self->pd;
    // Try to take a message from the scheduler and pass the handle as a hint.
    ocrFatGuid_t fatGuid;
    fatGuid.metaDataPtr = handle;
    PD_MSG_STACK(takeMsg);
    getCurrentEnv(NULL, NULL, NULL, &takeMsg);
#define PD_MSG (&takeMsg)
#define PD_TYPE PD_MSG_SCHED_GET_WORK
    takeMsg.type = PD_MSG_SCHED_GET_WORK | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_WORK_COMM;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guids = &fatGuid;
    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount = 1;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &takeMsg, true));
#undef PD_MSG
#undef PD_TYPE
    delegateMsgHandle_t * delHandle = (delegateMsgHandle_t *) fatGuid.metaDataPtr;
    if (delHandle != NULL) {
        if (handle != NULL) {
            #ifdef OCR_ASSERT
            // was polling for a specific handle, check if that's what we got
            if (*handle != NULL)
                ASSERT(*handle == ((ocrMsgHandle_t*)delHandle));
            #endif
            // Set the handle for the caller
            *handle = (ocrMsgHandle_t *) delHandle;
        }
        RETURN_PROFILE(POLL_MORE_MESSAGE);
    }
    RETURN_PROFILE(POLL_NO_MESSAGE);
}

/**
 * @brief Wait for a reply for the last message that has been sent out.
 *
 * Depending on the calling context, the handle can either be:
 *  - handle == NULL  (Impl specific, likely wait for "progress" to be made)
 *  - *handle == NULL (Wait any and return message)
 *  - *handle != NULL (Wait for a specific handle completion)
 */
u8 delegateCommWaitMessage(ocrCommApi_t *self, ocrMsgHandle_t **handle) {
    START_PROFILE(commapi_delegateCommWaitMessage);
    u8 ret = POLL_NO_MESSAGE;
    //BUG #130 Is there a use case to wait for a one-way to complete ?
    while(ret == POLL_NO_MESSAGE) {
        // Try to poll a little
        u64 i = 0;
        while ((i < MAX_ACTIVE_WAIT) && (ret == POLL_NO_MESSAGE)) {
            ret = self->fcts.pollMessage(self,handle);
            i++;
        }
        if (ret == POLL_NO_MESSAGE) {
            // If nothing shows up, transfer control to the scheduler for monitoring progress
            ocrPolicyDomain_t * pd;
            PD_MSG_STACK(msg);
            getCurrentEnv(&pd, NULL, NULL, &msg);
        #define PD_MSG (&msg)
        #define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
            msg.type = PD_MSG_MGT_MONITOR_PROGRESS | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            //BUG #131 helper-mode: not sure if the caller should register a progress function or if the
            //scheduler should know what to do for each type of monitor progress
            PD_MSG_FIELD_IO(properties) = (0 | MONITOR_PROGRESS_COMM);
            PD_MSG_FIELD_I(monitoree) = &handle;
            RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
        #undef PD_MSG
        #undef PD_TYPE
        }
    }
    RETURN_PROFILE(ret);
}

ocrCommApi_t* newCommApiDelegate(ocrCommApiFactory_t *factory,
                                       ocrParamList_t *perInstance) {
    ocrCommApiDelegate_t * commPlatformDelegate = (ocrCommApiDelegate_t*)
        runtimeChunkAlloc(sizeof(ocrCommApiDelegate_t), PERSISTENT_CHUNK);
    factory->initialize(factory, (ocrCommApi_t*) commPlatformDelegate, perInstance);
    return (ocrCommApi_t*) commPlatformDelegate;
}

/**
 * @brief Initialize an instance of comm-api delegate
 */
void initializeCommApiDelegate(ocrCommApiFactory_t * factory, ocrCommApi_t* self, ocrParamList_t * perInstance) {
    initializeCommApiOcr(factory, self, perInstance);
}

/******************************************************/
/* OCR COMM API DELEGATE FACTORY                      */
/******************************************************/

void destructCommApiFactoryDelegate(ocrCommApiFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrCommApiFactory_t *newCommApiFactoryDelegate(ocrParamList_t *perType) {
    ocrCommApiFactory_t *base = (ocrCommApiFactory_t*)
        runtimeChunkAlloc(sizeof(ocrCommApiFactoryDelegate_t), NONPERSISTENT_CHUNK);

    base->instantiate = newCommApiDelegate;
    base->initialize = initializeCommApiDelegate;
    base->destruct = destructCommApiFactoryDelegate;
    base->apiFcts.destruct = FUNC_ADDR(void (*)(ocrCommApi_t*), delegateCommDestruct);
    base->apiFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                    phase_t, u32, void (*)(ocrPolicyDomain_t*,u64), u64), delegateCommSwitchRunlevel);
    base->apiFcts.initHandle = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t*), delegateCommInitHandle);
    base->apiFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrLocation_t,
                                                      ocrPolicyMsg_t *, ocrMsgHandle_t **, u32), delegateCommSendMessage);
    base->apiFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t**),
                                               delegateCommPollMessage);
    base->apiFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t**),
                                               delegateCommWaitMessage);
    return base;
}

#endif /* ENABLE_COMM_API_DELEGATE */
