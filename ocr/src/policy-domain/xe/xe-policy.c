/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_XE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-runtime-types.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#include "policy-domain/xe/xe-policy.h"

#ifdef ENABLE_SYSBOOT_FSIM
#include "rmd-bin-files.h"
#endif

#include "utils/profiler/profiler.h"

#define DEBUG_TYPE POLICY


extern void ocrShutdown(void);

// Function to cause run-level switches in this PD
u8 xePdSwitchRunlevel(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, u32 properties) {
#ifdef ENABLE_SYSBOOT_FSIM
    if (XE_PDARGS_OFFSET != offsetof(ocrPolicyDomainXe_t, packedArgsLocation)) {
        DPRINTF(DEBUG_LVL_WARN, "XE_PDARGS_OFFSET (in .../ss/common/include/rmd-bin-files.h) is 0x%lx.  Should be 0x%lx\n",
            (u64) XE_PDARGS_OFFSET, (u64) offsetof(ocrPolicyDomainXe_t, packedArgsLocation));
        ASSERT (0);
    }
#endif
    // The PD should have been brought up by now and everything instantiated

    u64 i = 0;
    u64 maxCount = 0;

    ((ocrPolicyDomainXe_t*)policy)->shutdownInitiated = 0;


    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        policy->guidProviders[i]->fcts.switchRunlevel(policy->guidProviders[i], policy, RL_PD_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        policy->schedulers[i]->fcts.switchRunlevel(policy->schedulers[i], policy, RL_PD_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
        policy->schedulers[i]->fcts.switchRunlevel(policy->schedulers[i], policy, RL_MEMORY_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }

    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; i++) {
        policy->commApis[i]->fcts.switchRunlevel(policy->commApis[i], policy, RL_PD_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }
    // REC: Moved all workers to start here.
    // Note: it's important to first logically start all workers.
    // Once they are all up, start the runtime.
    // Workers should start the underlying target and platforms
    maxCount = policy->workerCount;
    for(i = 0; i < maxCount; i++) {
        policy->workers[i]->fcts.switchRunlevel(policy->workers[i], policy, RL_PD_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        policy->allocators[i]->fcts.switchRunlevel(policy->allocators[i], policy, RL_MEMORY_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        policy->schedulers[i]->fcts.switchRunlevel(policy->schedulers[i], policy, RL_COMPUTE_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }

    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; i++) {
        policy->commApis[i]->fcts.switchRunlevel(policy->commApis[i], policy, RL_MEMORY_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }
    policy->placer = NULL;
    guidify(policy, (u64)policy, &(policy->fguid), OCR_GUID_POLICY);

    // REC: Moved all workers to start here.
    // Note: it's important to first logically start all workers.
    // Once they are all up, start the runtime.
    // Workers should start the underlying target and platforms
    maxCount = policy->workerCount;
    for(i = 0; i < maxCount; i++) {
        policy->workers[i]->fcts.switchRunlevel(policy->workers[i], policy, RL_COMPUTE_OK,
                                                 0, RL_REQUEST | RL_BRING_UP | RL_BARRIER, NULL, 0);
    }

#if defined(SAL_FSIM_XE) // If running on FSim XE
    PRINTF("XE shutting down\n");
    hal_exit(0);
#endif

    return 0;
}


void xePolicyDomainDestruct(ocrPolicyDomain_t * policy) {
    // Destroying instances
    u64 i = 0;
    u64 maxCount = 0;

    // Note: As soon as worker '0' is stopped; its thread is
    // free to fall-through and continue shutting down the
    // policy domain
    maxCount = policy->workerCount;
    for(i = 0; i < maxCount; i++) {
        policy->workers[i]->fcts.destruct(policy->workers[i]);
    }

    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; i++) {
        policy->commApis[i]->fcts.destruct(policy->commApis[i]);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        policy->schedulers[i]->fcts.destruct(policy->schedulers[i]);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        policy->allocators[i]->fcts.destruct(policy->allocators[i]);
    }


    // Destruct factories
    maxCount = policy->taskFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        policy->taskFactories[i]->destruct(policy->taskFactories[i]);
    }

    maxCount = policy->taskTemplateFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        policy->taskTemplateFactories[i]->destruct(policy->taskTemplateFactories[i]);
    }

    maxCount = policy->dbFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        policy->dbFactories[i]->destruct(policy->dbFactories[i]);
    }

    maxCount = policy->eventFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        policy->eventFactories[i]->destruct(policy->eventFactories[i]);
    }

    //Anticipate those to be null-impl for some time
    ASSERT(policy->costFunction == NULL);

    // Destroy these last in case some of the other destructs make use of them
    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        policy->guidProviders[i]->fcts.destruct(policy->guidProviders[i]);
    }

    // Destroy self
    runtimeChunkFree((u64)policy->workers, NULL);
    runtimeChunkFree((u64)policy->schedulers, NULL);
    runtimeChunkFree((u64)policy->allocators, NULL);
    runtimeChunkFree((u64)policy->taskFactories, NULL);
    runtimeChunkFree((u64)policy->taskTemplateFactories, NULL);
    runtimeChunkFree((u64)policy->dbFactories, NULL);
    runtimeChunkFree((u64)policy->eventFactories, NULL);
    runtimeChunkFree((u64)policy->guidProviders, NULL);
    runtimeChunkFree((u64)policy, NULL);
}

static void localDeguidify(ocrPolicyDomain_t *self, ocrFatGuid_t *guid) {
    ASSERT(self->guidProviderCount == 1);
    if(guid->guid != NULL_GUID && guid->guid != UNINITIALIZED_GUID) {
        if(guid->metaDataPtr == NULL) {
            self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid->guid,
                                                (u64*)(&(guid->metaDataPtr)), NULL);
        }
    }
}

static u8 xeProcessCeRequest(ocrPolicyDomain_t *self, ocrPolicyMsg_t **msg) {
    u8 returnCode = 0;
    u32 type = ((*msg)->type & PD_MSG_TYPE_ONLY);
    ocrPolicyDomainXe_t *rself = (ocrPolicyDomainXe_t*)self;
    ASSERT((*msg)->type & PD_MSG_REQUEST);
    if ((*msg)->type & PD_MSG_REQ_RESPONSE) {
        ocrMsgHandle_t *handle = NULL;
        returnCode = self->fcts.sendMessage(self, self->parentLocation, (*msg),
                                            &handle, (TWOWAY_MSG_PROP | PERSIST_MSG_PROP));
        if (returnCode == 0) {
            ASSERT(handle && handle->msg);
            ASSERT(handle->msg == *msg); // This is what we passed in
            RESULT_ASSERT(self->fcts.waitMessage(self, &handle), ==, 0);
            ASSERT(handle->response);
            // Check if the message was a proper response and came from the right place
            //ASSERT(handle->response->srcLocation == self->parentLocation);
            ASSERT(handle->response->destLocation == self->myLocation);
            if((handle->response->type & PD_MSG_TYPE_ONLY) != type) {
                // Special case: shutdown in progress, cancel this message
                // The handle is destroyed by the caller for this case
                if(!rself->shutdownInitiated)
                    ocrShutdown();
                else {
                    // We destroy the handle here (which frees the buffer)
                    handle->destruct(handle);
                }
                return OCR_ECANCELED;
            }
            ASSERT((handle->response->type & PD_MSG_TYPE_ONLY) == type);
            if(handle->response != *msg) {
                // We need to copy things back into *msg
                // TODO: FIXME when issue #68 is fully implemented by checking
                // sizes
                // We use the marshalling function to "copy" this message
                u64 baseSize = 0, marshalledSize = 0;
                ocrPolicyMsgGetMsgSize(handle->response, &baseSize, &marshalledSize, 0);
                // For now, it must fit in a single message
                ASSERT(baseSize + marshalledSize <= sizeof(ocrPolicyMsg_t));
                ocrPolicyMsgMarshallMsg(handle->response, baseSize, (u8*)*msg, MARSHALL_DUPLICATE);
            }
            handle->destruct(handle);
        }
    } else {
        returnCode = self->fcts.sendMessage(self, self->parentLocation, (*msg), NULL, 0);
    }
    return returnCode;
}

u8 xePolicyDomainProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {

    u8 returnCode = 0;

    ASSERT((msg->type & PD_MSG_REQUEST) && (!(msg->type & PD_MSG_RESPONSE)));

    DPRINTF(DEBUG_LVL_VVERB, "Going to process message of type 0x%lx\n",
            (msg->type & PD_MSG_TYPE_ONLY));
    switch(msg->type & PD_MSG_TYPE_ONLY) {
    // First type of messages: things that we offload completely to the CE
    case PD_MSG_DB_CREATE: case PD_MSG_DB_DESTROY:
    case PD_MSG_DB_ACQUIRE: case PD_MSG_DB_RELEASE: case PD_MSG_DB_FREE:
    case PD_MSG_MEM_ALLOC: case PD_MSG_MEM_UNALLOC:
    case PD_MSG_WORK_CREATE: case PD_MSG_WORK_DESTROY:
    case PD_MSG_EDTTEMP_CREATE: case PD_MSG_EDTTEMP_DESTROY:
    case PD_MSG_EVT_CREATE: case PD_MSG_EVT_DESTROY: case PD_MSG_EVT_GET:
    case PD_MSG_GUID_CREATE: case PD_MSG_GUID_INFO: case PD_MSG_GUID_DESTROY:
    case PD_MSG_COMM_TAKE:
    case PD_MSG_DEP_ADD: case PD_MSG_DEP_REGSIGNALER: case PD_MSG_DEP_REGWAITER:
    case PD_MSG_DEP_SATISFY: {
        START_PROFILE(pd_xe_OffloadtoCE);
        if((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) {
            START_PROFILE(pd_xe_resolveTemp);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
            if((s32)(PD_MSG_FIELD_IO(paramc)) < 0) {
                localDeguidify(self, &(PD_MSG_FIELD_I(templateGuid)));
                ocrTaskTemplate_t *template = PD_MSG_FIELD_I(templateGuid).metaDataPtr;
                PD_MSG_FIELD_IO(paramc) = template->paramc;
            }
#undef PD_MSG
#undef PD_TYPE
            EXIT_PROFILE;
        }

        DPRINTF(DEBUG_LVL_VVERB, "Offloading message of type 0x%x to CE\n",
                msg->type & PD_MSG_TYPE_ONLY);
        returnCode = xeProcessCeRequest(self, &msg);

        if(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_COMM_TAKE) && (returnCode == 0)) {
            START_PROFILE(pd_xe_Take);
#define PD_MSG msg
#define PD_TYPE PD_MSG_COMM_TAKE
            if (PD_MSG_FIELD_IO(guidCount) > 0) {
                DPRINTF(DEBUG_LVL_VVERB, "Received EDT with GUID 0x%lx (@ 0x%lx)\n",
                        PD_MSG_FIELD_IO(guids[0].guid), &(PD_MSG_FIELD_IO(guids[0].guid)));
                localDeguidify(self, (PD_MSG_FIELD_IO(guids)));
                DPRINTF(DEBUG_LVL_VVERB, "Received EDT (0x%lx; 0x%lx)\n",
                        (u64)self->myLocation, (PD_MSG_FIELD_IO(guids))->guid,
                        (PD_MSG_FIELD_IO(guids))->metaDataPtr);
                // For now, we return the execute function for EDTs
                PD_MSG_FIELD_IO(extra) = (u64)(self->taskFactories[0]->fcts.execute);
            }
#undef PD_MSG
#undef PD_TYPE
            EXIT_PROFILE;
        }
        EXIT_PROFILE;
        break;
    }

    // Messages are not handled at all
    case PD_MSG_COMM_GIVE: // Give should only be triggered from the CE
    case PD_MSG_WORK_EXECUTE: case PD_MSG_DEP_UNREGSIGNALER:
    case PD_MSG_DEP_UNREGWAITER: case PD_MSG_SAL_PRINT:
    case PD_MSG_SAL_READ: case PD_MSG_SAL_WRITE:
    case PD_MSG_MGT_REGISTER: case PD_MSG_MGT_UNREGISTER:
    case PD_MSG_SAL_TERMINATE:
    case PD_MSG_GUID_METADATA_CLONE: case PD_MSG_GUID_RESERVE:
    case PD_MSG_GUID_UNRESERVE: case PD_MSG_MGT_MONITOR_PROGRESS:
    {
        DPRINTF(DEBUG_LVL_WARN, "XE PD does not handle call of type 0x%x\n",
                (u32)(msg->type & PD_MSG_TYPE_ONLY));
        ASSERT(0);
        returnCode = OCR_ENOTSUP;
        break;
    }

    // Messages handled locally
    case PD_MSG_DEP_DYNADD: {
        START_PROFILE(pd_xe_DepDynAdd);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNADD
        // Check to make sure that the EDT is only doing this to
        // itself
        // Also, this should only happen when there is an actual EDT
        ASSERT(curTask &&
               curTask->guid == PD_MSG_FIELD_I(edt.guid));

        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNADD req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_I(db.guid));
        ASSERT(curTask->fctId == self->taskFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.notifyDbAcquire(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_DYNREMOVE: {
        START_PROFILE(pd_xe_DepDynRemove);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        // Check to make sure that the EDT is only doing this to
        // itself
        // Also, this should only happen when there is an actual EDT
        ASSERT(curTask &&
            curTask->guid == PD_MSG_FIELD_I(edt.guid));
        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNREMOVE req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_I(db.guid));
        ASSERT(curTask->fctId == self->taskFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.notifyDbRelease(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_MGT_RL_NOTIFY: {
        START_PROFILE(pd_xe_Shutdown);
        ocrPolicyDomainXe_t *rself = (ocrPolicyDomainXe_t*)self;
        rself->shutdownInitiated = 1;
        // Shutdown here
        if(msg->srcLocation == self->myLocation) {
            DPRINTF(DEBUG_LVL_INFO, "SHUTDOWN initiation from 0x%lx %lx\n",
                    msg->srcLocation, msg->destLocation);
            returnCode = xeProcessCeRequest(self, &msg);
            self->workers[0]->fcts.switchRunlevel(self->workers[0], self, RL_USER_OK,
                                                  0, RL_REQUEST, NULL, 0);
            // HACK: We force the return code to be 0 here
            // because otherwise something will complain
            returnCode = 0;
        } else {
            //FIXME: We never exercise this code path.
            //Keeping it for now until we fix the shutdown protocol
            //Bug #134
            ASSERT(0);
            DPRINTF(DEBUG_LVL_INFO, "SHUTDOWN(slave) from 0x%lx\n",
                    msg->srcLocation);
            // Send the message back saying that
            // we did the shutdown
            msg->destLocation = msg->srcLocation;
            msg->srcLocation = self->myLocation;
            msg->type &= ~PD_MSG_REQUEST;
            msg->type |= PD_MSG_RESPONSE;
            RESULT_ASSERT(self->fcts.sendMessage(self, msg->destLocation, msg, NULL, 0),
                          ==, 0);
            returnCode = 0;
        }
        EXIT_PROFILE;
        break;
    }
    default: {
        DPRINTF(DEBUG_LVL_WARN, "Unknown message type 0x%x\n", (u32)(msg->type & PD_MSG_TYPE_ONLY));
        ASSERT(0);
    }
    }; // End of giant switch

    return returnCode;
}

u8 xePdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {
    ocrPolicyDomainXe_t *rself = (ocrPolicyDomainXe_t*)self;
    ASSERT(target == self->parentLocation);
    // Set the header of this message
    message->srcLocation  = self->myLocation;
    message->destLocation = self->parentLocation;
    u8 returnCode = self->commApis[0]->fcts.sendMessage(self->commApis[0], target, message, handle, properties);
    if (returnCode == 0) return 0;
    switch (returnCode) {
    case OCR_ECANCELED:
        // Our outgoing message was cancelled and shutdown is assumed
        // TODO: later this will be expanded to include failures
        // We destruct the handle for the first message in case it was partially used
        if(*handle)
            (*handle)->destruct(*handle);
        //FIXME: OCR_ECANCELED shouldn't be inferred generally as shutdown
        //but rather handled on a per message basis. Once we have a proper
        //shutdown protocol this should go away.
        //Bug #134
        // We do not do a shutdown if we already have a shutdown initiated
        if(returnCode==OCR_ECANCELED && !rself->shutdownInitiated)
            ocrShutdown();
        break;
    case OCR_EBUSY:
        if(*handle)
            (*handle)->destruct(*handle);
        ocrMsgHandle_t *tempHandle = NULL;
        RESULT_ASSERT(self->fcts.waitMessage(self, &tempHandle), ==, 0);
        ASSERT(tempHandle && tempHandle->response);
        ocrPolicyMsg_t * newMsg = tempHandle->response;
        ASSERT(newMsg->srcLocation == self->parentLocation);
        ASSERT(newMsg->destLocation == self->myLocation);
        RESULT_ASSERT(self->fcts.processMessage(self, newMsg, true), ==, 0);
        tempHandle->destruct(tempHandle);
        break;
    default:
        ASSERT(0);
    }
    return returnCode;
}

u8 xePdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
    return self->commApis[0]->fcts.pollMessage(self->commApis[0], handle);
}

u8 xePdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
    return self->commApis[0]->fcts.waitMessage(self->commApis[0], handle);
}

void* xePdMalloc(ocrPolicyDomain_t *self, u64 size) {
    START_PROFILE(pd_xe_pdMalloc);
    void *ptr;
    PD_MSG_STACK(msg);
    ocrPolicyMsg_t* pmsg = &msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_ALLOC
    msg.type = PD_MSG_MEM_ALLOC  | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(type) = DB_MEMTYPE;
    PD_MSG_FIELD_I(size) = size;
    ASSERT(self->workerCount == 1);              // Assert this XE has exactly one worker.
    u8 msgResult = xeProcessCeRequest(self, &pmsg);
    ASSERT (msgResult == 0);   // TODO: Are there error cases I need to handle?  How?
    ptr = PD_MSG_FIELD_O(ptr);
#undef PD_TYPE
#undef PD_MSG
    RETURN_PROFILE(ptr);
}

void xePdFree(ocrPolicyDomain_t *self, void* addr) {
    START_PROFILE(pd_xe_pdFree);
    PD_MSG_STACK(msg);
    ocrPolicyMsg_t *pmsg = &msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_UNALLOC
    msg.type = PD_MSG_MEM_UNALLOC | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(ptr) = addr;
    PD_MSG_FIELD_I(type) = DB_MEMTYPE;
    // TODO: Things are missing. Brian's new way to free things should fix this!
    PD_MSG_FIELD_I(properties) = 0;
    u8 msgResult = xeProcessCeRequest(self, &pmsg);
    ASSERT(msgResult == 0);
#undef PD_MSG
#undef PD_TYPE
    RETURN_PROFILE();
}

ocrPolicyDomain_t * newPolicyDomainXe(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrCost_t *costFunction, ocrParamList_t *perInstance) {

    ocrPolicyDomainXe_t * derived = (ocrPolicyDomainXe_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainXe_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;

    ASSERT(base);
#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, base, statsObject, costFunction, perInstance);
#else
    factory->initialize(factory, base, costFunction, perInstance);
#endif
    derived->packedArgsLocation = NULL;

    return base;
}

void initializePolicyDomainXe(ocrPolicyDomainFactory_t * factory, ocrPolicyDomain_t* self,
#ifdef OCR_ENABLE_STATISTICS
                              ocrStats_t *statsObject,
#endif
                              ocrCost_t *costFunction, ocrParamList_t *perInstance) {
#ifdef OCR_ENABLE_STATISTICS
    self->statsObject = statsObject;
#endif

    initializePolicyDomainOcr(factory, self, perInstance);
}

static void destructPolicyDomainFactoryXe(ocrPolicyDomainFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrPolicyDomainFactory_t * newPolicyDomainFactoryXe(ocrParamList_t *perType) {
    ocrPolicyDomainFactory_t* base = (ocrPolicyDomainFactory_t*) runtimeChunkAlloc(sizeof(ocrPolicyDomainFactoryXe_t), NONPERSISTENT_CHUNK);

    ASSERT(base); // Check allocation

#ifdef OCR_ENABLE_STATISTICS
    base->instantiate = FUNC_ADDR(ocrPolicyDomain_t*(*)(ocrPolicyDomainFactory_t*,ocrCost_t*,
                                  ocrParamList_t*), newPolicyDomainXe);
#endif

    base->instantiate = &newPolicyDomainXe;
    base->initialize = &initializePolicyDomainXe;
    base->destruct = &destructPolicyDomainFactoryXe;

    base->policyDomainFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrRunlevel_t, u32), xePdSwitchRunlevel);
    base->policyDomainFcts.destruct = FUNC_ADDR(void(*)(ocrPolicyDomain_t*), xePolicyDomainDestruct);
    base->policyDomainFcts.processMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*,ocrPolicyMsg_t*,u8), xePolicyDomainProcessMessage);
    base->policyDomainFcts.sendMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*, ocrLocation_t, ocrPolicyMsg_t*, ocrMsgHandle_t**, u32),
                                         xePdSendMessage);
    base->policyDomainFcts.pollMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), xePdPollMessage);
    base->policyDomainFcts.waitMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), xePdWaitMessage);

    base->policyDomainFcts.pdMalloc = FUNC_ADDR(void*(*)(ocrPolicyDomain_t*,u64), xePdMalloc);
    base->policyDomainFcts.pdFree = FUNC_ADDR(void(*)(ocrPolicyDomain_t*,void*), xePdFree);
    return base;
}

#endif /* ENABLE_POLICY_DOMAIN_XE */
