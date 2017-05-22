/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * - A scheduler heuristic for WST root schedulerObjects
 *
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_HEURISTIC_HC_COMM_DELEGATE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "ocr-scheduler-object.h"
#include "scheduler-heuristic/hc/hc-comm-delegate-scheduler-heuristic.h"

// To know about the type of handlers from delegate comm api
#include "comm-api/delegate/delegate-comm-api.h"

// This is because we rely on the worker's type to understand
// what to do on a give/take
#include "worker/hc/hc-worker.h"

#define DEBUG_TYPE SCHEDULER_HEURISTIC

/******************************************************/
/* OCR-HC-COMM-DELEGATE SCHEDULER_HEURISTIC           */
/******************************************************/

static ocrSchedulerHeuristic_t* newSchedulerHeuristicHcCommDelegate(ocrSchedulerHeuristicFactory_t * factory, ocrParamList_t *perInstance) {
    ocrSchedulerHeuristic_t* self = (ocrSchedulerHeuristic_t*) runtimeChunkAlloc(sizeof(ocrSchedulerHeuristicHcCommDelegate_t), PERSISTENT_CHUNK);
    initializeSchedulerHeuristicOcr(factory, self, perInstance);
    ocrSchedulerHeuristicHcCommDelegate_t * dself = (ocrSchedulerHeuristicHcCommDelegate_t *) self;
    dself->outboxesCount = 0;
    dself->outboxes = NULL;
    dself->inboxesCount = 0;
    dself->inboxes = NULL;
    return self;
}

static void initializeContextHcCommDelegate(ocrSchedulerHeuristicContext_t *context, u64 contextId) {
    context->id = contextId;
    context->actionSet = NULL;
    context->cost = NULL;
    context->properties = 0;

    ocrSchedulerHeuristicContextHcCommDelegate_t *hcContext = (ocrSchedulerHeuristicContextHcCommDelegate_t*)context;
    hcContext->stealSchedulerObjectIndex = ((u64)-1);
    hcContext->mySchedulerObject = NULL;
    return;
}

static u8 hcCommDelegateSchedulerHeuristicSwitchRunlevel(ocrSchedulerHeuristic_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        break;
    case RL_PD_OK:
    {
        ASSERT(self->scheduler);
        self->contextCount = PD->workerCount; //Shared mem heuristic
        ASSERT(self->contextCount > 0);
        break;
    }
    case RL_MEMORY_OK:
    {
        ocrSchedulerHeuristicHcCommDelegate_t * dself = (ocrSchedulerHeuristicHcCommDelegate_t *) self;
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            u32 i;
            self->contexts = (ocrSchedulerHeuristicContext_t **)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*));
            ADebug(AllocDebugAllPD, "ce-scheduler-heuristic.c/hcCommDelegateSchedulerHeuristicSwitchRunlevel()/RL_GUID_OK[contexts] "
                   "pdMalloc(%ld), addr=%p\n",
                   (self->contextCount * sizeof(ocrSchedulerHeuristicContext_t*)), self->contexts);
            ocrSchedulerHeuristicContextHcCommDelegate_t *contextAlloc = (ocrSchedulerHeuristicContextHcCommDelegate_t *)PD->fcts.pdMalloc(PD, self->contextCount * sizeof(ocrSchedulerHeuristicContextHcCommDelegate_t));
            ADebug(AllocDebugAllPD, "ce-scheduler-heuristic.c/hcCommDelegateSchedulerHeuristicSwitchRunlevel()/RL_GUID_OK[contextAlloc] "
                   "pdMalloc(%ld), addr=%p\n",
                   (self->contextCount * sizeof(ocrSchedulerHeuristicContextHcCommDelegate_t)), contextAlloc);

            for (i = 0; i < self->contextCount; i++) {
                ocrSchedulerHeuristicContext_t *context = (ocrSchedulerHeuristicContext_t *)&(contextAlloc[i]);
                initializeContextHcCommDelegate(context, i);
                self->contexts[i] = context;
                context->id = i;
                context->location = PD->myLocation;
                context->actionSet = NULL;
                context->cost = NULL;
                context->properties = 0;
                ocrSchedulerHeuristicContextHcCommDelegate_t *hcContext = (ocrSchedulerHeuristicContextHcCommDelegate_t*)context;
                hcContext->stealSchedulerObjectIndex = ((u64)-1);
                hcContext->mySchedulerObject = NULL;
            }
            //Note: pd should have been set in base implementation
            //Create outbox queues for each worker
            u64 boxCount = PD->workerCount;
            dself->outboxesCount = boxCount;
            dself->outboxes = PD->fcts.pdMalloc(PD, sizeof(deque_t *) * boxCount);
            ADebug(AllocDebugAllPD, "ce-scheduler-heuristic.c/hcCommDelegateSchedulerHeuristicSwitchRunlevel()/RL_GUID_OK[outboxes] "
                   "pdMalloc(%ld), addr=%p\n",
                   (sizeof(deque_t *) * boxCount), dself->outboxes);
            for(i = 0; i < boxCount; ++i) {
                dself->outboxes[i] = newDeque(PD, NULL, WORK_STEALING_DEQUE);
            }
            //Create inbox queues for each worker
            dself->inboxesCount = boxCount;
            dself->inboxes = PD->fcts.pdMalloc(PD, sizeof(deque_t *) * boxCount);
            ADebug(AllocDebugAllPD, "ce-scheduler-heuristic.c/hcCommDelegateSchedulerHeuristicSwitchRunlevel()/RL_GUID_OK[inboxes] "
                   "pdMalloc(%ld), addr=%p\n",
                   (sizeof(deque_t *) * boxCount),dself->inboxes);
            for(i = 0; i < boxCount; ++i) {
                dself->inboxes[i] = newDeque(PD, NULL, SEMI_CONCURRENT_DEQUE);
            }
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
            // deallocate all pdMalloc-ed structures
            u64 i;
            for(i = 0; i < dself->outboxesCount; ++i) {
                dself->outboxes[i]->destruct(PD, dself->outboxes[i]);
            }
            for(i = 0; i < dself->inboxesCount; ++i) {
                dself->inboxes[i]->destruct(PD, dself->inboxes[i]);
            }
            PD->fcts.pdFree(PD, dself->outboxes);
            PD->fcts.pdFree(PD, dself->inboxes);
            PD->fcts.pdFree(PD, self->contexts[0]);
            PD->fcts.pdFree(PD, self->contexts);
        }
        break;
    }
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
    {
        // Nothing to do
        break;
    }
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

static void hcCommDelegateSchedulerHeuristicDestruct(ocrSchedulerHeuristic_t * self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

static u8 hcCommDelegateSchedulerHeuristicUpdate(ocrSchedulerHeuristic_t *self, u32 properties) {
    return OCR_ENOTSUP;
}

static ocrSchedulerHeuristicContext_t* hcCommDelegateSchedulerHeuristicGetContext(ocrSchedulerHeuristic_t *self, ocrLocation_t loc) {
    ASSERT(loc == self->scheduler->pd->myLocation);
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    return self->contexts[worker->id];
}

/**
 * @brief Take communication work
 *
 * The scheduler must identify the type of the worker doing the call to determine what to return.
 * In the case of comp-worker, a take returns a handle that has completed, while for comm-worker
 * a take returns a handle representing a message to be sent out.
 */
static inline u8 hcCommDelegateWorkEdtUserInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    ocrSchedulerHeuristicHcCommDelegate_t * commSched = (ocrSchedulerHeuristicHcCommDelegate_t *) self;
    ocrWorker_t *worker = NULL; //BUG #204: sep-concern: Do we need a way to register worker types somehow ?
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u64 wid = worker->id;

    // The fatGuid array is allocated by the caller and has space for guidCount at max.
    // The guidCount is updated with the actual number of returned values upon completion.
    ocrFatGuid_t * fatHandlers = taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guids;
    u32 count = taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount;

    //TODO use context here
    if (((ocrWorkerHc_t *) worker)->hcType == HC_WORKER_COMM) {
        // Steal from other worker's outbox
        //PERF: use real randomized iterator here
        // Try a round of stealing on other worker's outbox
        //NOTE: If we don't care about wasting a steal on self, we wouldn't need
        //      the whole worker ID business
        //NOTE: It's debatable whether we should steal a bunch from each outbox
        //      or go over all outboxes
        deque_t ** outboxes = commSched->outboxes;
        u64 outboxesCount = commSched->outboxesCount;
        u32 success = 0;

#ifdef HYBRID_COMM_COMP_WORKER // Experimental see documentation
        // Try to pop from own outbox first (support HYBRID_COMM_COMP_WORKER mode)
        ocrMsgHandle_t* handle = outboxes[wid]->popFromTail(outboxes[wid], 1);
        if (handle != NULL) {
            fatHandlers[success].metaDataPtr = handle;
            success++;
        }
#endif
        u64 i = (wid+1); // skip over 'self' outbox
        while ((i < (wid+outboxesCount)) && (success < count)) {
            deque_t * deque = outboxes[i%outboxesCount];
            ocrMsgHandle_t* handle = deque->popFromHead(deque, 1);
            if (handle != NULL) {
                fatHandlers[success].metaDataPtr = handle;
                success++;
            }
            i++;
        }
        count = success;
    } else {
        //BUG #586 Should really revisit this implementation. It sounds awfully slow.
        ASSERT(((ocrWorkerHc_t *) worker)->hcType == HC_WORKER_COMP);
        deque_t * inbox = commSched->inboxes[wid];
        u32 curIdx = 0;
        linkedlist_t * candidateList = NULL;
        while(curIdx < count) {
            ocrMsgHandle_t ** target = (ocrMsgHandle_t **) fatHandlers[curIdx].metaDataPtr;
            bool isSpecificTarget = ((target != NULL) && (*target != NULL));
            ocrMsgHandle_t * candidate = NULL;
            if (isSpecificTarget && (candidateList != NULL)) {
                //Look through previous steals
                iterator_t * iterator = candidateList->iterator(candidateList);
                while (iterator->hasNext(iterator)) {
                    ocrMsgHandle_t * handle = (ocrMsgHandle_t *) iterator->next(iterator);
                    if (handle == *target) {
                        // found a candidate in the previous steals
                        candidate = handle;
                        iterator->removeCurrent(iterator);
                        fatHandlers[curIdx].metaDataPtr = candidate;
                        curIdx++;
                        break;
                    }
                }
                iterator->destruct(iterator);
            }
            if (candidate == NULL) {
                // 'steal' from own inbox, comm-worker pushes to it
                candidate = (ocrMsgHandle_t *) inbox->popFromHead(inbox, 0);
                if (candidate == NULL) {
                    // No message available
                    break;
                }
                if (isSpecificTarget) {
                    if (candidate != *target) {
                        if (candidateList == NULL) {
                            ocrPolicyDomain_t * pd = self->scheduler->pd;
                            candidateList = newLinkedList(pd);
                        }
                        candidateList->pushFront(candidateList, candidate);
                    } else {
                        fatHandlers[curIdx].metaDataPtr = candidate;
                        curIdx++;
                    }
                } else {
                    // Found a handle and none specific was required.
                    // Don't think we go through this but double check
                    ASSERT(false && "comp-worker poll for any");
                    fatHandlers[curIdx].metaDataPtr = candidate;
                    curIdx++;
                }
            }
        }
        u32 i = curIdx;
        while (i < count) { // nullify remaining handlers
            fatHandlers[i].metaDataPtr = NULL;
            i++;
        }
        count = curIdx;
        if (candidateList != NULL) {
            // Put stolen handles for which there's no interest back
            iterator_t * iterator = candidateList->iterator(candidateList);
            while (iterator->hasNext(iterator)) {
                ocrMsgHandle_t * handle = (ocrMsgHandle_t *) iterator->next(iterator);
                // Push is concurrent with comm-worker populating the inbox
                inbox->pushAtTail(inbox, handle, 0);
                iterator->removeCurrent(iterator);
            }
            iterator->destruct(iterator);
            candidateList->destruct(candidateList);
        }
    }
    taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_COMM).guidCount = count;

    return 0;
}


/**
 * @brief Called by comm and comp workers to give communication work.
 *
 * The scheduler must identify the type of the worker doing the call to
 * determine what to do.
 */
static u8 hcCommDelegateSchedulerHeuristicGetWorkInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpWorkArgs_t *taskArgs = (ocrSchedulerOpWorkArgs_t*)opArgs;
    switch(taskArgs->kind) {
    case OCR_SCHED_WORK_COMM:
        // Taking comm tasks
        return hcCommDelegateWorkEdtUserInvoke(self, context, opArgs, hints);
    // Unknown ops
    default:
        ASSERT(0);
        return OCR_ENOTSUP;
    }
    return 0;
}

static u8 hcCommDelegateSchedulerHeuristicGetWorkSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static inline u8 hcCommDelegateNotifyCommReadyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    //TODO limitation: previous implementation was able to coalesce multiple give into a single call.
    ocrFatGuid_t * fatHandlers = &(notifyArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_COMM_READY).guid);
    u32 count = 1;

    ocrPolicyDomain_t * pd __attribute__((unused)) = self->scheduler->pd;
    ocrSchedulerHeuristicHcCommDelegate_t * commSched = (ocrSchedulerHeuristicHcCommDelegate_t *) self;
    ocrWorker_t *worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    //BUG #204 sep-concern: Do we need a way to register worker types somehow ?
    if (((ocrWorkerHc_t *) worker)->hcType == HC_WORKER_COMM) {
        u32 i=0;
        while (i < count) {
            delegateMsgHandle_t* handle = (delegateMsgHandle_t *) fatHandlers[i].metaDataPtr;
        #ifdef HYBRID_COMM_COMP_WORKER // Experimental see documentation
            ocrPolicyMsg_t * message = (handle->handle.status == HDL_RESPONSE_OK) ? handle->handle.response : handle->handle.msg;
            bool outgoingComm = (message->destLocation != pd->myLocation);
            if (outgoingComm) {
                // (support HYBRID_COMM_COMP_WORKER mode)
                // The communication worker can process simple messages that
                // are known to be short lived and are 'sterile' (i.e. do not generate
                // new communication) beside responding to the message.
                // In that case, the comm-worker should only be able to give outgoing responses to the scheduler
                ASSERT(message->type & PD_MSG_RESPONSE);
                // Push to the comm worker outbox
                DPRINTF(DEBUG_LVL_VVERB,"[%"PRId32"] hc-comm-delegate-scheduler:: Comm-worker pushes outgoing to own outbox %"PRId32"\n",
                    (int) pd->myLocation, worker->id);
                deque_t * outbox = commSched->outboxes[worker->id];
                outbox->pushAtTail(outbox, handle, 0);
            } else {
        #endif
                // Comm-worker giving back to a worker's inbox
                DPRINTF(DEBUG_LVL_VVERB,"[%"PRIu64"] hc-comm-delegate-scheduler:: Comm-worker pushes at tail of box %"PRIu64"\n",
                    pd->myLocation, handle->boxId);
                // Push is concurrent because the comp-worker may pushing/poping from inbox in parallel
                deque_t * inbox = commSched->inboxes[handle->boxId];
                inbox->pushAtTail(inbox, (ocrMsgHandle_t *) handle, 0);
        #ifdef HYBRID_COMM_COMP_WORKER
            }
        #endif
            i++;
        }
        count = i;
    } else {
        // Comp-worker giving a message to send
        u32 i=0;
        while (i < count) {
            // Set delegate handle's box id.
            delegateMsgHandle_t* delHandle = (delegateMsgHandle_t *) fatHandlers[i].metaDataPtr;
#ifdef OCR_ASSERT
            ocrPolicyMsg_t * message = (delHandle->handle.status == HDL_RESPONSE_OK) ? delHandle->handle.response : delHandle->handle.msg;
            ASSERT((message->srcLocation == pd->myLocation) && (message->destLocation != pd->myLocation));
#endif
            //BUG #587: boxId is defined in del-handle however only the scheduler is using it
            delHandle->boxId = worker->id;
            DPRINTF(DEBUG_LVL_VVERB,"[%"PRIu64"] hc-comm-delegate-scheduler:: Comp-worker pushes at tail of box %"PRIu64"\n",
                pd->myLocation, delHandle->boxId);
            ASSERT((delHandle->boxId >= 0) && (delHandle->boxId < pd->workerCount));
            // Put handle to worker's outbox
            deque_t * outbox = commSched->outboxes[delHandle->boxId];
            outbox->pushAtTail(outbox, (ocrMsgHandle_t *) delHandle, 0);
            i++;
        }
        count = i;
    }

    return 0;
}

static u8 hcCommDelegateSchedulerHeuristicNotifyInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ocrSchedulerHeuristicContext_t *context = self->fcts.getContext(self, opArgs->location);
    ocrSchedulerOpNotifyArgs_t *notifyArgs = (ocrSchedulerOpNotifyArgs_t*)opArgs;
    switch(notifyArgs->kind) {
    case OCR_SCHED_NOTIFY_COMM_READY:
        return hcCommDelegateNotifyCommReadyInvoke(self, context, opArgs, hints);
    // Unknown ops
    default:
        ASSERT(false && "error: hcCommDelegate doesn't support notify message");
        return OCR_ENOTSUP;
    }
    return 0;
}

static u8 hcCommDelegateSchedulerHeuristicNotifySimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 hcCommDelegateSchedulerHeuristicTransactInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 hcCommDelegateSchedulerHeuristicTransactSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 hcCommDelegateSchedulerHeuristicAnalyzeInvoke(ocrSchedulerHeuristic_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

static u8 hcCommDelegateSchedulerHeuristicAnalyzeSimulate(ocrSchedulerHeuristic_t *self, ocrSchedulerHeuristicContext_t *context, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-HC SCHEDULER_HEURISTIC FACTORY                 */
/******************************************************/

static void destructSchedulerHeuristicFactoryHcCommDelegate(ocrSchedulerHeuristicFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrSchedulerHeuristicFactory_t * newOcrSchedulerHeuristicFactoryHcCommDelegate(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerHeuristicFactory_t* base = (ocrSchedulerHeuristicFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerHeuristicFactoryHcCommDelegate_t), NONPERSISTENT_CHUNK);
    base->factoryId = factoryId;
    base->instantiate = &newSchedulerHeuristicHcCommDelegate;
    base->destruct = &destructSchedulerHeuristicFactoryHcCommDelegate;;
    base->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                 phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcCommDelegateSchedulerHeuristicSwitchRunlevel);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrSchedulerHeuristic_t*), hcCommDelegateSchedulerHeuristicDestruct);

    base->fcts.update = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, u32), hcCommDelegateSchedulerHeuristicUpdate);
    base->fcts.getContext = FUNC_ADDR(ocrSchedulerHeuristicContext_t* (*)(ocrSchedulerHeuristic_t*, ocrLocation_t), hcCommDelegateSchedulerHeuristicGetContext);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicGetWorkInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_GET_WORK].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicGetWorkSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicNotifyInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_NOTIFY].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicNotifySimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicTransactInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_TRANSACT].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicTransactSimulate);

    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].invoke = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicAnalyzeInvoke);
    base->fcts.op[OCR_SCHEDULER_HEURISTIC_OP_ANALYZE].simulate = FUNC_ADDR(u8 (*)(ocrSchedulerHeuristic_t*, ocrSchedulerHeuristicContext_t*, ocrSchedulerOpArgs_t*, ocrRuntimeHint_t*), hcCommDelegateSchedulerHeuristicAnalyzeSimulate);

    return base;
}

#endif /* ENABLE_SCHEDULER_HEURISTIC_HC_COMM_DELEGATE */
