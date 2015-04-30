/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_API_HANDLELESS

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-worker.h"
#include "ocr-comm-platform.h"
#include "utils/ocr-utils.h"
#include "handleless-comm-api.h"

#define DEBUG_TYPE COMM_API

void handlelessCommDestruct (ocrCommApi_t * base) {
    // call destruct on child
    if(base->commPlatform) {
        base->commPlatform->fcts.destruct(base->commPlatform);
        base->commPlatform = NULL;
    }
    runtimeChunkFree((u64)base, NULL);
}

u8 handlelessCommSwitchRunlevel(ocrCommApi_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                u32 phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    toReturn |= self->commPlatform->fcts.switchRunlevel(self->commPlatform, PD, runlevel, phase,
                                                        properties, NULL, 0);

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP)
            self->pd = PD;
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

u8 handlelessCommSendMessage(ocrCommApi_t *self, ocrLocation_t target, ocrPolicyMsg_t *message,
                             ocrMsgHandle_t **handle, u32 properties) {
    u64 id;
    // Asserts in this function are sanity checks; any violation points to memory corruption
    if (message->type & PD_MSG_REQUEST) {
        ASSERT(!(message->type & PD_MSG_RESPONSE));
        if(handle) {
            ASSERT(message->type & PD_MSG_REQ_RESPONSE);
            ocrCommApiHandleless_t * commApiHandleless = (ocrCommApiHandleless_t*)self;

            if(commApiHandleless->handle.status != 0) {
                commApiHandleless->handle.status = 0;
                return OCR_ECANCELED;
            }

            ASSERT(commApiHandleless->handle.status == 0);
            *handle = &(commApiHandleless->handle);
            (*handle)->msg = message;
            (*handle)->response = NULL;
            (*handle)->status = HDL_NORMAL;
        }
    } else {
        ASSERT(message->type & PD_MSG_RESPONSE);
        ASSERT(!handle);
    }
    return self->commPlatform->fcts.sendMessage(self->commPlatform, target, message, &id, properties, 0);
}

u8 handlelessCommPollMessage(ocrCommApi_t *self, ocrMsgHandle_t **handle) {
    u8 retval;
    ASSERT(handle);
    ocrCommApiHandleless_t * commApiHandleless = (ocrCommApiHandleless_t*)self;
    if (!(*handle)) {
        *handle = &(commApiHandleless->handle);
        (*handle)->status = HDL_NORMAL;
    } else {
        ASSERT((*handle)->msg);
    }
    // Pass a "hint" saying that the the buffer is available
    (*handle)->response = (*handle)->msg;
    retval = self->commPlatform->fcts.pollMessage(
                      self->commPlatform, &((*handle)->response), PD_CE_CE_MESSAGE,
                      NULL);
    if((*handle)->response == (*handle)->msg) {
        // This means that the comm platform did *not* allocate the buffer itself
        (*handle)->properties = 0; // Indicates "do not free"
    } else {
        // This means that the comm platform did allocate the buffer itself
        (*handle)->properties = 1; // Indicates "do free"
    }
    return retval;
}

u8 handlelessCommWaitMessage(ocrCommApi_t *self, ocrMsgHandle_t **handle) {
    ASSERT(handle);
    ocrCommApiHandleless_t * commApiHandleless = (ocrCommApiHandleless_t*)self;
    // Asserts in this function are sanity checks; any violation points to memory corruption
    if (!(*handle)) {
        *handle = &(commApiHandleless->handle);
        ASSERT((*handle)->status == 0);
        (*handle)->status = HDL_NORMAL;
    } else {
        ASSERT((*handle)->msg);
    }
    ASSERT((*handle)->status == HDL_NORMAL &&
           (*handle) == (&(commApiHandleless->handle)));
    // Pass a "hint" saying that the the buffer is available
    (*handle)->response = (*handle)->msg;
    RESULT_ASSERT(self->commPlatform->fcts.waitMessage(
                      self->commPlatform, &((*handle)->response), 0,
                      NULL), ==, 0);
    if((*handle)->response == (*handle)->msg) {
        // This means that the comm platform did *not* allocate the buffer itself
        (*handle)->properties = 0; // Indicates "do not free"
    } else {
        // This means that the comm platform did allocate the buffer itself
        (*handle)->properties = 1; // Indicates "do free"
    }
    return 0;
}

void handlelessCommDestructHandle(ocrMsgHandle_t *handle) {
    if(handle->properties == 1) {
        RESULT_ASSERT(handle->commApi->commPlatform->fcts.destructMessage(
                          handle->commApi->commPlatform, handle->response), ==, 0);
    }
    handle->msg = NULL;
    handle->response = NULL;
    handle->status = 0;
    handle->properties = 0;
}

ocrCommApi_t* newCommApiHandleless(ocrCommApiFactory_t *factory,
                                   ocrParamList_t *perInstance) {

    ocrCommApiHandleless_t * commApiHandleless = (ocrCommApiHandleless_t*)
        runtimeChunkAlloc(sizeof(ocrCommApiHandleless_t), PERSISTENT_CHUNK);
    factory->initialize(factory, (ocrCommApi_t*) commApiHandleless, perInstance);
    commApiHandleless->handle.msg = NULL;
    commApiHandleless->handle.response = NULL;
    commApiHandleless->handle.status = 0;
    commApiHandleless->handle.properties = 0;
    commApiHandleless->handle.commApi = (ocrCommApi_t*)commApiHandleless;
    commApiHandleless->handle.destruct = FUNC_ADDR(void (*)(ocrMsgHandle_t*), handlelessCommDestructHandle);

    return (ocrCommApi_t*)commApiHandleless;
}

/**
 * @brief Initialize an instance of comm-api handleless
 */
void initializeCommApiHandleless(ocrCommApiFactory_t * factory, ocrCommApi_t* self, ocrParamList_t * perInstance) {
    initializeCommApiOcr(factory, self, perInstance);
}

/******************************************************/
/* OCR COMM API HANDLELESS                            */
/******************************************************/

void destructCommApiFactoryHandleless(ocrCommApiFactory_t *factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrCommApiFactory_t *newCommApiFactoryHandleless(ocrParamList_t *perType) {
    ocrCommApiFactory_t *base = (ocrCommApiFactory_t*)
        runtimeChunkAlloc(sizeof(ocrCommApiFactoryHandleless_t), NONPERSISTENT_CHUNK);

    base->instantiate = newCommApiHandleless;
    base->initialize = initializeCommApiHandleless;
    base->destruct = destructCommApiFactoryHandleless;
    base->apiFcts.destruct = FUNC_ADDR(void (*)(ocrCommApi_t*), handlelessCommDestruct);
    base->apiFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                    u32, u32, void (*)(ocrPolicyDomain_t*, u64), u64), handlelessCommSwitchRunlevel);
    base->apiFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrLocation_t, ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                          handlelessCommSendMessage);
    base->apiFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t**),
                                          handlelessCommPollMessage);
    base->apiFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t**),
                                          handlelessCommWaitMessage);

    return base;
}
#endif /* ENABLE_COMM_API_HANDLELESS */
