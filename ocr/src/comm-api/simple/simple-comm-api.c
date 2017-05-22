/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_API_SIMPLE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-worker.h"
#include "ocr-comm-platform.h"
#include "utils/ocr-utils.h"
#include "comm-api/simple/simple-comm-api.h"

#define DEBUG_TYPE COMM_API

// SIMPLE_COMM masks
#define SIMPLE_COMM_NO_PROP 0
#define SIMPLE_COMM_NO_MASK 0

//PERF: customize map size
#define HANDLE_MAP_BUCKETS 10

static void destructMsgHandler(ocrMsgHandle_t * handler) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    pd->fcts.pdFree(pd, handler);
}

/**
 * @brief Create a message handler for this comm-api
 */
static ocrMsgHandle_t * createMsgHandler(ocrCommApi_t * self, ocrPolicyMsg_t * message) {
    ocrPolicyDomain_t * pd = self->pd;
    ocrMsgHandle_t * handler = pd->fcts.pdMalloc(pd, sizeof(ocrMsgHandle_t));
    ADebug(AllocDebugAllPD, "simple-comm-api.c/createMsgHandler() "
           "pdMalloc(%ld), addr=%p\n", sizeof(ocrMsgHandle_t), handler);

    handler->msg = message;
    handler->response = NULL;
    handler->status = HDL_NORMAL;
    handler->destruct = &destructMsgHandler;
    return handler;
}

static ocrPolicyMsg_t * allocateNewMessage(ocrCommApi_t * self, u32 size) {
    ocrPolicyDomain_t * pd = self->pd;
    ocrPolicyMsg_t * message = pd->fcts.pdMalloc(pd, size);
    ADebug(AllocDebugAllPD, "simple-comm-api.c/allocateNewMessage() "
           "pdMalloc(%ld), addr=%p\n", size, message);
    initializePolicyMessage(message, size);
    return message;
}

u8 simpleCommApiInitHandle(ocrCommApi_t *self, ocrMsgHandle_t *handle) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 sendMessageSimpleCommApi(ocrCommApi_t *self, ocrLocation_t target, ocrPolicyMsg_t *message,
                        ocrMsgHandle_t **handle, u32 properties) {
    START_PROFILE(commapi_sendMessageSimpleCommApi);
    ocrCommApiSimple_t * commApiSimple = (ocrCommApiSimple_t *) self;

    // Debug and check if we should push this in the in/out patch or runlevel
    if (!(properties & PERSIST_MSG_PROP)) {
        //NOTE: In a delegation based implementation, all messages issued by comp-workers
        //are already persistant because of the asynchrony delegation allows. The comm-worker
        //may occasionally invoke send for its own messages. In that case persist is not set.
        u64 baseSize = 0, marshalledSize = 0;
        //BUG #604 Do we marshall ptr here or do we wait for the comm-platform to decide what to do ?
        // Currently avoids an extra copy because we know simple-comm fwd to mpi-comm
        ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
        u64 fullMsgSize = baseSize + marshalledSize;
        ocrPolicyMsg_t * msgCpy = allocateNewMessage(self, fullMsgSize);
        ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)msgCpy,
                                MARSHALL_FULL_COPY | MARSHALL_DBPTR | MARSHALL_NSADDR);
        message = msgCpy;
        properties |= PERSIST_MSG_PROP;

    }

    // This is weird but otherwise the compiler complains...
    u64 id = 0;

    u8 ret = self->commPlatform->fcts.sendMessage(self->commPlatform, target, message,
                                                  &id, properties, SIMPLE_COMM_NO_MASK);
    if (ret == 0) {
        if (handle != NULL) {
            if (*handle == NULL) {
                // Handle creation requested
                *handle = createMsgHandler(self, message);
            }
            //Message sent (potentially not yet received at destination)
            (*handle)->status = HDL_SEND_OK;
            // Associate id with handle
            hashtableNonConcPut(commApiSimple->handleMap, (void *) id, *handle);
        } // else: No handle requested
    } else {
        // Error occurred while sending, set handler status if any
        if ((handle != NULL) && (*handle != NULL)) {
            (*handle)->status = HDL_SEND_ERR;
        }
        // Assert for now since we don't really handle errors in upper-layers
        ASSERT(ret == 0);
    }
    RETURN_PROFILE(ret);
}

/*
 * Callers should look at the handle's status.
 *
 */
u8 pollMessageSimpleCommApi(ocrCommApi_t *self, ocrMsgHandle_t **handle) {
    START_PROFILE(commapi_pollMessageSimpleCommApi);
    //BUG #130 Is there a use case to poll for a one-way to complete ?
    //This implementation does not support one-way with ack
    //However it's a little hard to detect here. A handle message
    //pointer may have been de-allocated and it's the only place
    //where we can have a look.

    //IMPL: Alternative would be that the comm-platform always return
    //out/in comms that are done and we can update the status here.
    //That would require to be able to tell the platform to post new recv.
    ocrCommApiSimple_t * commApiSimple = (ocrCommApiSimple_t *) self;
    ocrPolicyMsg_t * msg = NULL;
    //IMPL: by contract commPlatform only poll and return recvs.
    //They can be incoming request or response. (but not outgoing req/resp ack)
    u8 ret = self->commPlatform->fcts.pollMessage(self->commPlatform, &msg, SIMPLE_COMM_NO_PROP, SIMPLE_COMM_NO_MASK);
    if (ret == POLL_MORE_MESSAGE) {
        ASSERT((handle != NULL) && (*handle == NULL));
        if (msg->type & PD_MSG_REQUEST) {
            // This an outstanding request, we need to create a handle for
            // the caller to manipulate the message.
            *handle = createMsgHandler(self, msg);
        } else {
            // Response for a request
            //NOTE: If the outgoing communication requires a response, the communication layer
            //      must set the handler->response pointer to the response's communication handler.
            ASSERT(msg->type & PD_MSG_RESPONSE);
            if (msg->msgId == 0) {
                // Contractual with comm-platform when msgId is zero.
                // It is an asynchronous response callback, not registered in the hashtable.
                *handle = createMsgHandler(self, msg);
                (*handle)->properties = ASYNC_MSG_PROP;
            } else {
                RESULT_ASSERT(hashtableNonConcRemove(commApiSimple->handleMap, (void *) msg->msgId, (void **)handle), !=, 0);
                ASSERT(*handle != NULL);
            }
            (*handle)->response = msg;
            (*handle)->status = HDL_RESPONSE_OK;
        }
    }
    RETURN_PROFILE(ret);
}

u8 waitMessageSimpleCommApi(ocrCommApi_t *self, ocrMsgHandle_t **handle) {
    START_PROFILE(commapi_waitMessageSimpleCommApi);
    u8 ret = 0;
    do {
        ret = self->fcts.pollMessage(self, handle);
    } while(ret != POLL_MORE_MESSAGE);

    RETURN_PROFILE(ret);
}

void simpleCommApiDestruct (ocrCommApi_t * base) {
    if(base->commPlatform != NULL) {
        base->commPlatform->fcts.destruct(base->commPlatform);
        base->commPlatform = NULL;
    }
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
}

u8 simpleCommApiSwitchRunlevel(ocrCommApi_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                        phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    if(properties & RL_BRING_UP) {
        toReturn |= self->commPlatform->fcts.switchRunlevel(
            self->commPlatform, PD, runlevel, phase, properties, NULL, 0);
    }

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            // We can now set our PD (before this, we couldn't because
            // "our" PD might not have been started
            self->pd = PD;
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        // We can allocate our map here because the memory is up
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
            ocrCommApiSimple_t * commApiSimple = (ocrCommApiSimple_t *) self;
            commApiSimple->handleMap = newHashtableModulo(self->pd, HANDLE_MAP_BUCKETS);
        }
        if ((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_GUID_OK, phase)) {
            //BUG #527: memory reclaim: would like to make sure this is empty, otherwise it probably
            //means there are pending communication so we shouldn't be in tear down.
            ocrCommApiSimple_t * commApiSimple = (ocrCommApiSimple_t *) self;
            destructHashtable(commApiSimple->handleMap, NULL, NULL);
        }
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }

    if(properties & RL_TEAR_DOWN) {
        toReturn |= self->commPlatform->fcts.switchRunlevel(
            self->commPlatform, PD, runlevel, phase, properties, NULL, 0);
    }
    return toReturn;
}

ocrCommApi_t* newCommApiSimple(ocrCommApiFactory_t *factory,
                           ocrParamList_t *perInstance) {
    ocrCommApiSimple_t * commApiSimple = (ocrCommApiSimple_t*)
        runtimeChunkAlloc(sizeof(ocrCommApiSimple_t), PERSISTENT_CHUNK);
    factory->initialize(factory, (ocrCommApi_t*) commApiSimple, perInstance);
    return (ocrCommApi_t*)commApiSimple;
}

/**
 * @brief Initialize an instance of comm-api simple
 */
void initializeCommApiSimple(ocrCommApiFactory_t * factory, ocrCommApi_t* self, ocrParamList_t * perInstance) {
    initializeCommApiOcr(factory, self, perInstance);
    ocrCommApiSimple_t * commApiSimple = (ocrCommApiSimple_t*) self;
    commApiSimple->handleMap = NULL;
}

/******************************************************/
/* OCR COMM API Simple                                */
/******************************************************/

void destructCommApiFactorySimple(ocrCommApiFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrCommApiFactory_t *newCommApiFactorySimple(ocrParamList_t *perType) {
    ocrCommApiFactory_t *base = (ocrCommApiFactory_t*)
        runtimeChunkAlloc(sizeof(ocrCommApiFactorySimple_t), NONPERSISTENT_CHUNK);

    base->instantiate = newCommApiSimple;
    base->initialize = initializeCommApiSimple;
    base->destruct = destructCommApiFactorySimple;

    base->apiFcts.destruct = FUNC_ADDR(void (*)(ocrCommApi_t*), simpleCommApiDestruct);
    base->apiFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                      phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), simpleCommApiSwitchRunlevel);
    base->apiFcts.initHandle = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t*), simpleCommApiInitHandle);
    base->apiFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrLocation_t, ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                          sendMessageSimpleCommApi);
    base->apiFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t**),
                                          pollMessageSimpleCommApi);
    base->apiFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommApi_t*, ocrMsgHandle_t**),
                                          waitMessageSimpleCommApi);
    return base;
}

#endif /* ENABLE_COMM_API_SIMPLE */

