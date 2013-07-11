/* Copyright (c) 2012, Rice University

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1.  Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   2.  Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided
   with the distribution.
   3.  Neither the name of Intel Corporation
   nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific
   prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "ocr-macros.h"
#include "hc.h"
#include "hc_edf.h"
#include "ocr-datablock.h"
#include "ocr-utils.h"
#include "ocr-task.h"
#include "ocr-event.h"
#include "ocr-worker.h"
#include "debug.h"
#include "ocr-policy-domain.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#define SEALED_LIST ((void *) -1)
#define END_OF_LIST NULL
#define UNINITIALIZED_DATA ((ocrGuid_t) -2)


//
// Guid-kind checkers for convenience
//

static bool isDatablockGuid(ocrGuid_t guid) {
    ocrGuidKind kind;
    guidKind(getCurrentPD(),guid, &kind);
    return kind == OCR_GUID_DB;
}

static bool isEventGuid(ocrGuid_t guid) {
    ocrGuidKind kind;
    guidKind(getCurrentPD(),guid, &kind);
    return kind == OCR_GUID_EVENT;
}

static bool isEdtGuid(ocrGuid_t guid) {
    ocrGuidKind kind;
    guidKind(getCurrentPD(),guid, &kind);
    return kind == OCR_GUID_EDT;
}

static bool isEventLatchGuid(ocrGuid_t guid) {
    if(isEventGuid(guid)) {
        ocrEventHc_t * event = NULL;
        deguidify(getCurrentPD(), guid, (u64*)&event, NULL);
        return (event->kind == OCR_EVENT_LATCH_T);
    }
    return false;
}

static bool isEventSingleGuid(ocrGuid_t guid) {
    if(isEventGuid(guid)) {
        ocrEventHc_t * event = NULL;
        deguidify(getCurrentPD(), guid, (u64*)&event, NULL);
        return ((event->kind == OCR_EVENT_ONCE_T)
            || (event->kind == OCR_EVENT_IDEM_T)
            || (event->kind == OCR_EVENT_STICKY_T));
    }
    return false;
}

//
// EDT Properties Utilities
//

/**
   @brief return true if a property is part of a properties set
 **/
static bool hasProperty(u16 properties, u16 property) {
    return properties & property;
}


/******************************************************/
/* OCR-HC ELS Slots Declaration                       */
/******************************************************/

// This must be consistent with the ELS size the runtime is compiled with
#define ELS_SLOT_FINISH_LATCH 0

static ocrTask_t * getCurrentTask() {
    ocrGuid_t edtGuid = getCurrentEDT();
    // the bootstrap process launching mainEdt returns NULL_GUID for the current EDT
    if (edtGuid != NULL_GUID) {
        ocrTask_t * event = NULL;
        deguidify(getCurrentPD(), edtGuid, (u64*)&event, NULL);
        return event;
    }
    return NULL;
}

//
// forward declarations
//

void signalWaiter(ocrGuid_t waiterGuid, ocrGuid_t data, int slot);

void registerSignaler(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot);

void registerWaiter(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot);

static inline void taskSchedule(ocrGuid_t taskGuid);

// Internal signal/wait notification
// Signal a sticky event some data arrived (on its unique slot)
static void singleEventSignaled(ocrEvent_t * self, ocrGuid_t data, int slot) {
    // Single event signaled by another entity, call its satisfy method.
    self->fctPtrs->satisfy(self, data, slot);
}

// Internal signal/wait notification
// Signal a latch event some data arrived on one of its
static void latchEventSignaled(ocrEvent_t * self, ocrGuid_t data, int slot) {
    // latch event signaled by another entity, call its satisfy method.
    self->fctPtrs->satisfy(self, data, slot);
}

/******************************************************/
/* OCR-HC Finish-Latch Utilities                      */
/******************************************************/

// satisfies the incr slot of a finish latch event
static void finishLatchCheckin(ocrEvent_t * base) {
     base->fctPtrs->satisfy(base, NULL_GUID, FINISH_LATCH_INCR_SLOT);
}

// satisfies the decr slot of a finish latch event
static void finishLatchCheckout(ocrEvent_t * base) {
    base->fctPtrs->satisfy(base, NULL_GUID, FINISH_LATCH_DECR_SLOT);
}

static void setFinishLatch(ocrTask_t * edt, ocrGuid_t latchGuid) {
    edt->els[ELS_SLOT_FINISH_LATCH] = latchGuid;
}

static ocrEvent_t * getFinishLatch(ocrTask_t * edt) {
    if (edt != NULL) { //  NULL happens in main when there's no edt yet
        ocrGuid_t latchGuid = edt->els[ELS_SLOT_FINISH_LATCH];
        if (latchGuid != NULL_GUID) {
            ocrEvent_t * event = NULL;
            deguidify(getCurrentPD(), latchGuid, (u64*)&event, NULL);
            return event;
        }
    }
    return NULL;
}

static bool isFinishLatchOwner(ocrEvent_t * finishLatch, ocrGuid_t edtGuid) {
    return (finishLatch != NULL) && (((ocrEventHcFinishLatch_t *)finishLatch)->ownerGuid == edtGuid);
}


/******************************************************/
/* OCR-HC Task Implementation                         */
/******************************************************/

static void newTaskHcInternalCommon (ocrPolicyDomain_t * pd, ocrTaskHc_t* derived,
                                     ocrTaskTemplate_t * taskTemplate, u32 paramc,
                                     u64* paramv, u32 depc, ocrGuid_t outputEvent) {
    if (depc == 0) {
        derived->signalers = END_OF_LIST;
    } else {
        // Since we know how many dependences we have, preallocate signalers
        derived->signalers = checkedMalloc(derived->signalers, sizeof(regNode_t)*depc);
    }
    derived->waiters = END_OF_LIST;
    // Initialize base
    ocrTask_t* base = (ocrTask_t*) derived;
    base->guid = UNINITIALIZED_GUID;
    guidify(pd, (u64)base, &(base->guid), OCR_GUID_EDT);
    base->templateGuid = taskTemplate->guid;
    base->paramc = paramc;
    base->paramv = paramv;
    base->outputEvent = outputEvent;
    base->depc = depc;
    base->addedDepCounter = pd->getAtomic64(pd, NULL /*Context*/);
    // Initialize ELS
    int i = 0;
    while (i < ELS_SIZE) {
        base->els[i++] = NULL_GUID;
    }
}

static ocrTaskHc_t* newTaskHcInternal (ocrTaskFactory_t* factory, ocrPolicyDomain_t * pd,
                                       ocrTaskTemplate_t * taskTemplate, u32 paramc,
                                       u64* paramv, u32 depc, u16 properties,
                                       ocrGuid_t affinity, ocrGuid_t outputEvent) {
    ocrTaskHc_t* newEdt = (ocrTaskHc_t*)checkedMalloc(newEdt, sizeof(ocrTaskHc_t));
    newTaskHcInternalCommon(pd, newEdt, taskTemplate, paramc, paramv, depc, outputEvent);
    ocrTask_t * newEdtBase = (ocrTask_t *) newEdt;
    // If we are creating a finish-edt
    if (hasProperty(properties, EDT_PROP_FINISH)) {
        ocrEventFactory_t * eventFactory = getEventFactoryFromPd(pd);
        ocrEvent_t * latch = eventFactory->instantiate(eventFactory, OCR_EVENT_FINISH_LATCH_T,
                                                       false, NULL);
        ocrEventHcFinishLatch_t * hcLatch = (ocrEventHcFinishLatch_t *) latch;
        // Set the owner of the latch
        hcLatch->ownerGuid = newEdtBase->guid;
        ocrEvent_t * parentLatch = getFinishLatch(getCurrentTask());
        if (parentLatch != NULL) {
            // Check in current finish latch
            finishLatchCheckin(parentLatch);
            // Link the new latch to the parent's decrement slot
            // When this finish scope is done, the parent scope is signaled.
            // DESIGN can modify the finish latch to have a standard list of regnode and just addDependence
            hcLatch->parentLatchWaiter.guid = parentLatch->guid;
            hcLatch->parentLatchWaiter.slot = FINISH_LATCH_DECR_SLOT;
        } else {
            hcLatch->parentLatchWaiter.guid = NULL_GUID;
            hcLatch->parentLatchWaiter.slot = -1;
        }
        // Check in the new finish scope
        finishLatchCheckin(latch);
        // Set edt's ELS to the new latch
        setFinishLatch(newEdtBase, latch->guid);
        // If there's an output event for this finish edt, add a dependence
        // from the finish latch to it.
        // DESIGN can modify flatch to have a standard list of regnode and just addDependence in a if !NULL_GUID
        hcLatch->outputEventWaiter.guid = outputEvent;
        hcLatch->outputEventWaiter.slot = 0;
    } else {
        // If the currently executing edt is in a finish scope,
        // but is not a finish-edt itself, just register to the scope
        ocrEvent_t * curLatch = getFinishLatch(getCurrentTask());
        if (curLatch != NULL) {
            // Check in current finish latch
            finishLatchCheckin(curLatch);
            // Transmit finish-latch to newly created edt
            setFinishLatch(newEdtBase, curLatch->guid);
        }
    }
    return newEdt;
}

static void destructTaskHc ( ocrTask_t* base ) {
    ocrTaskHc_t* derived = (ocrTaskHc_t*)base;
#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcessDestruct(&(base->statProcess));
#endif
    ocrPolicyDomain_t *pd = getCurrentPD();
    ocrPolicyCtx_t * orgCtx = getCurrentWorkerContext();
    ocrPolicyCtx_t * ctx = orgCtx->clone(orgCtx);
    ctx->type = PD_MSG_GUID_REL;
    pd->inform(pd, base->guid, ctx);
    base->addedDepCounter->fctPtrs->destruct(base->addedDepCounter);
    free(derived);
}

// Signals an edt one of its dependence slot is satisfied
static void taskSignaled(ocrTask_t * base, ocrGuid_t data, u32 slot) {
    // An EDT has a list of signalers, but only register
    // incrementally as signals arrive.
    // Assumption: signal frontier is initialized at slot zero
    // Whenever we receive a signal, it can only be from the
    // current signal frontier, since it is the only signaler
    // the edt is registered with at that time.
    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    // Replace the signaler's guid by the data guid, this to avoid
    // further references to the event's guid, which is good in general
    // and crucial for once-event since they are being destroyed on satisfy.
    self->signalers[slot].guid = data;
    if (slot == (base->depc-1)) {
        // All dependencies have been satisfied, schedule the edt
        taskSchedule(base->guid);
    } else {
        // else register the edt on the next event to wait on
        slot++;
        //TODO, could we directly pass the node to avoid a new alloc ?
        registerWaiter((self->signalers[slot]).guid, base->guid, slot);
    }
}

//Registers an entity that will signal on one of the edt's slot.
static void edtRegisterSignaler(ocrTask_t * base, ocrGuid_t signalerGuid, int slot) {
    // Only support event signals
    ASSERT(isEventGuid(signalerGuid) || isDatablockGuid(signalerGuid));
    //DESIGN would be nice to pre-allocate all of this since we know the edt
    //       dep slot size at edt's creation, then what type should we expose
    //       for the signalers member in the task's data-structure ?
    //No need to CAS anything as all dependencies are provided at creation.
    ocrTaskHc_t * self = (ocrTaskHc_t *) base;
    regNode_t * node = &(self->signalers[slot]);
    node->guid = signalerGuid;
    node->slot = slot;
    // No need to chain nodes here, will use index
    node->next = NULL;
}


//
// OCR TASK SCHEDULING
//

/**
 * @brief Schedules a task when we know its guid.
 * Warning: The caller must ensure all dependencies have been satisfied
 * Note: static function only meant to factorize code.
 */
static inline void taskSchedule( ocrGuid_t taskGuid ) {
    // Setting up the context
    ocrPolicyCtx_t * orgCtx = getCurrentWorkerContext();
    // Current worker schedulers to current policy domain
    // TODO this can be cached somewhere if target/dest are static
    ocrPolicyCtx_t * ctx = orgCtx->clone(orgCtx);
    ctx->destPD = orgCtx->sourcePD;
    ctx->destObj = NULL_GUID;
    ctx->type = PD_MSG_EDT_READY;
    // give the edt to the policy domain
    orgCtx->PD->giveEdt(orgCtx->PD, 1, &taskGuid, ctx);
}

/**
 * @brief Tries to schedules a task by registering to its first dependence.
 * If no dependence, schedule the task right-away
 * Warning: This method is to be called ONCE per task and there's no safeguard !
 */
static void tryScheduleTask( ocrTask_t* base ) {
    //DESIGN This is very much specific to the way we've chosen
    //       to handle event-to-task dependence, which needs some
    //       kind of bootstrapping. This could be done when the last
    //       dependence slot is registered with a signaler, but then
    //       the semantic of the schedule function is weaker.
    ocrTaskHc_t* self = (ocrTaskHc_t*)base;
    if (base->depc != 0) {
        // Register the task on the first dependence. If all
        // dependences are here, the task is scheduled by
        // the dependence management code.
        registerWaiter(self->signalers[0].guid, base->guid, 0);
    } else {
        // If there's no dependence, the task must be scheduled now.
        taskSchedule(base->guid);
    }
}

static void taskExecute ( ocrTask_t* base ) {
    ocrTaskHc_t* derived = (ocrTaskHc_t*)base;
    // In this implementation each time a signaler has been satisfied, its guid
    // has been replaced by the db guid it has been satisfied with.
    u32 paramc = base->paramc;
    u64 * paramv = base->paramv;
    u64 depc = base->depc;
    
    ocrEdtDep_t * depv = NULL;
    // If any dependencies, acquire their data-blocks
    if (depc != 0) {
        u64 i = 0;
        //TODO would be nice to resolve regNode into guids before
        depv = (ocrEdtDep_t *) checkedMalloc(depv, sizeof(ocrEdtDep_t) * depc);
        // Double-check we're not rescheduling an already executed edt
        ASSERT(derived->signalers != END_OF_LIST);
        while ( i < depc ) {
            //TODO would be nice to standardize that on satisfy
            regNode_t * regNode = &(derived->signalers[i]);
            ocrGuid_t dbGuid = regNode->guid;
            depv[i].guid = dbGuid;
            if(dbGuid != NULL_GUID) {
                ASSERT(isDatablockGuid(dbGuid));
                ocrDataBlock_t * db = NULL;
                deguidify(getCurrentPD(), dbGuid, (u64*)&db, NULL);
                depv[i].ptr = db->fctPtrs->acquire(db, base->guid, true);
            } else {
                depv[i].ptr = NULL;
            }
            i++;
        }
        free(derived->signalers);
        derived->signalers = END_OF_LIST;
    }

    ocrTaskTemplate_t * taskTemplate;
    deguidify(getCurrentPD(), base->templateGuid, (u64*)&taskTemplate, NULL);
    //TODO: define when task template is resolved from its guid
    ocrGuid_t retGuid = taskTemplate->executePtr(paramc, paramv,
                                                 depc, depv);

    // edt user code is done, if any deps, release data-blocks
    if (depc != 0) {
        u64 i = 0;
        for(i=0; i<depc; ++i) {
            if(depv[i].guid != NULL_GUID) {
                ocrDataBlock_t * db = NULL;
                deguidify(getCurrentPD(), depv[i].guid, (u64*)&db, NULL);
                RESULT_ASSERT(db->fctPtrs->release(db, base->guid, true), ==, 0);
            }
        }
    }
    bool satisfyOutputEvent = (base->outputEvent != NULL_GUID);
    // check out from current finish scope
    ocrEvent_t * curLatch = getFinishLatch(base);
    if (curLatch != NULL) {
        // if own the latch, then set the retGuid
        if (isFinishLatchOwner(curLatch, base->guid)) {
            ((ocrEventHcFinishLatch_t *) curLatch)->returnGuid = retGuid;
            satisfyOutputEvent = false;
        }
        // If the edt is the last to checkout from the current finish scope,
        // the latch event automatically satisfies the parent latch (if any) 
        // and the output event associated with the current finish-edt (if any)
        finishLatchCheckout(curLatch);
    }

    // !! Warning: curLatch might have been deallocated at that point !!

    if (satisfyOutputEvent) {
        ocrEvent_t * outputEvent;
        deguidify(getCurrentPD(), base->outputEvent, (u64*)&outputEvent, NULL);
        // We know the output event must be of type single sticky since it is
        // internally allocated by the runtime
        ASSERT(isEventSingleGuid(base->outputEvent));
        outputEvent->fctPtrs->satisfy(outputEvent, retGuid, 0);
    }
}

/******************************************************/
/* OCR-HC Task Template Factory                       */
/******************************************************/

ocrTaskTemplate_t * newTaskTemplateHc(ocrTaskTemplateFactory_t* factory, ocrEdt_t executePtr, u32 paramc, u32 depc) {
    ocrTaskTemplateHc_t* template = (ocrTaskTemplateHc_t*) checkedMalloc(template, sizeof(ocrTaskTemplateHc_t));
    ocrTaskTemplate_t * base = (ocrTaskTemplate_t *) template;
    base->paramc = paramc;
    base->depc = depc;
    base->executePtr = executePtr;
    base->guid = UNINITIALIZED_GUID;
    guidify(getCurrentPD(), (u64)base, &(base->guid), OCR_GUID_EDT_TEMPLATE);
    return base;
}

void destructTaskTemplateFactoryHc(ocrTaskTemplateFactory_t* base) {
    ocrTaskTemplateFactoryHc_t* derived = (ocrTaskTemplateFactoryHc_t*) base;
    free(derived);
}

ocrTaskTemplateFactory_t * newTaskTemplateFactoryHc(ocrParamList_t* perType) {
    ocrTaskTemplateFactoryHc_t* derived = (ocrTaskTemplateFactoryHc_t*) checkedMalloc(derived, sizeof(ocrTaskTemplateFactoryHc_t));
    ocrTaskTemplateFactory_t* base = (ocrTaskTemplateFactory_t*) derived;
    base->instantiate = newTaskTemplateHc;
    base->destruct =  destructTaskTemplateFactoryHc;
    //TODO What taskTemplateFcts is supposed to do ?
    return base;
}

/******************************************************/
/* OCR-HC Task Factory                                */
/******************************************************/

ocrTask_t * newTaskHc(ocrTaskFactory_t* factory, ocrTaskTemplate_t * taskTemplate,
                      u32 paramc, u64* paramv, u32 depc, u16 properties,
                      ocrGuid_t affinity, ocrGuid_t * outputEventPtr,
                      ocrParamList_t *perInstance) {

    // Initialize a sticky outputEvent if requested (ptr not NULL)
    // i.e. the user gave a place-holder for the runtime to initialize and set an output event.
    ocrGuid_t outputEvent = (ocrGuid_t) outputEventPtr;
    ocrPolicyDomain_t* pd = getCurrentPD();
    if (outputEvent != NULL_GUID) {
        ocrEventFactory_t * eventFactory = getEventFactoryFromPd(pd);
        ocrEvent_t * event = eventFactory->instantiate(eventFactory, OCR_EVENT_STICKY_T, false, NULL);
        *outputEventPtr = event->guid;
        outputEvent = event->guid;
    }
    ocrTaskHc_t* edt = newTaskHcInternal(factory, pd, taskTemplate, paramc, paramv,
                                         depc, properties, affinity, outputEvent);
    ocrTask_t* base = (ocrTask_t*) edt;
    base->fctPtrs = &(((ocrTaskFactoryHc_t *) factory)->taskFctPtrs);
    return base;
}

void destructTaskFactoryHc(ocrTaskFactory_t* base) {
    ocrTaskFactoryHc_t* derived = (ocrTaskFactoryHc_t*) base;
    free(derived);
}

ocrTaskFactory_t * newTaskFactoryHc(ocrParamList_t* perInstance) {
    ocrTaskFactoryHc_t* derived = (ocrTaskFactoryHc_t*) checkedMalloc(derived, sizeof(ocrTaskFactoryHc_t));
    ocrTaskFactory_t* base = (ocrTaskFactory_t*) derived;
    base->instantiate = newTaskHc;
    base->destruct =  destructTaskFactoryHc;
    // initialize singleton instance that carries hc implementation
    // function pointers. Every instantiated task template will use
    // this pointer to resolve functions implementations.
    derived->taskFctPtrs.destruct = destructTaskHc;
    derived->taskFctPtrs.execute = taskExecute;
    derived->taskFctPtrs.schedule = tryScheduleTask;
    return base;
}


/******************************************************/
/* Signal/Wait interface implementation               */
/******************************************************/

// Registers a 'waiter' that should be signaled by 'self' on a particular slot.
// If 'self' has already been satisfied, it signals 'waiter' right away.
// Warning: Concurrent with  event's put.
static void awaitableEventRegisterWaiter(ocrEventHcAwaitable_t * self, ocrGuid_t waiter, int slot) {
    // Try to insert 'waiter' at the beginning of the list
    regNode_t * curHead = (regNode_t *) self->waiters;
    if(curHead != SEALED_LIST) {
        regNode_t * newHead = checkedMalloc(newHead, sizeof(regNode_t));
        newHead->guid = waiter;
        newHead->slot = slot;
        newHead->next = (regNode_t *) curHead;
        while(curHead != SEALED_LIST && !__sync_bool_compare_and_swap(&(self->waiters), curHead, newHead)) {
            curHead = (regNode_t *) self->waiters;
            newHead->next = (regNode_t *) curHead;
        }
        if (curHead != SEALED_LIST) {
            // Insertion successful, we're done
            return;
        }
        //else list has been sealed by a concurrent satisfy
        //need to reclaim non-inserted node
        free(newHead);
    }
    // Either the event was satisfied to begin with
    // or while we were trying to insert the waiter,
    // the event has been satisfied.
    signalWaiter(waiter, self->data, slot);
}

//These are essentially switches to dispatch call to the correct implementation

void registerDependence(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot) {
    // WAIT MODE: event-to-event registration
    // Note: do not call 'registerWaiter' here as it triggers event-to-edt
    // registration, which should only be done on edtSchedule.
    if (isEventGuid(signalerGuid) && isEventGuid(waiterGuid)) {
        ocrEventHcAwaitable_t * target;
        deguidify(getCurrentPD(), signalerGuid, (u64*)&target, NULL);
        awaitableEventRegisterWaiter(target, waiterGuid, slot);
        return;
    }

    // SIGNAL MODE:
    //  - anything-to-edt registration
    //  - db-to-event registration
    registerSignaler(signalerGuid, waiterGuid, slot);
}

// Registers a waiter on a signaler
void registerWaiter(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot) {
    if (isEventGuid(signalerGuid)) {
        ASSERT(isEdtGuid(waiterGuid) || isEventGuid(waiterGuid));
        ocrEventHcAwaitable_t * target;
        deguidify(getCurrentPD(), signalerGuid, (u64*)&target, NULL);
        awaitableEventRegisterWaiter(target, waiterGuid, slot);
    } else if(isDatablockGuid(signalerGuid) && isEdtGuid(waiterGuid)) {
            signalWaiter(waiterGuid, signalerGuid, slot);
    } else {
        // Everything else is an error
        ASSERT("error: Unsupported guid kind in registerWaiter" );
    }
}

// register a signaler on a waiter
void registerSignaler(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot) {
    // anything to edt registration
    if (isEdtGuid(waiterGuid)) {
        // edt waiting for a signal from an event or a datablock
        ASSERT(isEventGuid(signalerGuid) || isDatablockGuid(signalerGuid));
        ocrTask_t * target = NULL;
        deguidify(getCurrentPD(), waiterGuid, (u64*)&target, NULL);
        edtRegisterSignaler(target, signalerGuid, slot);
        ocrTaskTemplate_t * template = NULL;
        deguidify(getCurrentPD(), target->templateGuid, (u64*)&template, NULL);
        if ( template->depc == target->addedDepCounter->fctPtrs->xadd(target->addedDepCounter,1) ) {
            target->fctPtrs->schedule(target);
        }
        return;
    // datablock to event registration => satisfy on the spot
    } else if (isDatablockGuid(signalerGuid)) {
        ASSERT(isEventGuid(waiterGuid));
        ocrEvent_t * target = NULL;
        deguidify(getCurrentPD(), waiterGuid, (u64*)&target, NULL);
        // This looks a duplicate of signalWaiter, however there hasn't
        // been any signal strictly speaking, hence calling satisfy directly
        ASSERT(isEventSingleGuid(waiterGuid) || isEventLatchGuid(waiterGuid));
        target->fctPtrs->satisfy(target, signalerGuid, slot);
        return;
    }
    // Remaining legal registrations is event-to-event, but this is
    // handled in 'registerWaiter'
    ASSERT(isEventGuid(waiterGuid) && isEventGuid(signalerGuid) && "error: Unsupported guid kind in registerDependence");
}

// signal a 'waiter' data has arrived on a particular slot
void signalWaiter(ocrGuid_t waiterGuid, ocrGuid_t data, int slot) {
    // TODO do we need to know who's signaling ?
    if (isEventSingleGuid(waiterGuid)) {
        ocrEvent_t * target = NULL;
        deguidify(getCurrentPD(), waiterGuid, (u64*)&target, NULL);
        singleEventSignaled(target, data, slot);
    } else if (isEventLatchGuid(waiterGuid)) {
        ocrEvent_t * target = NULL;
        deguidify(getCurrentPD(), waiterGuid, (u64*)&target, NULL);
        latchEventSignaled(target, data, slot);
    } else if(isEdtGuid(waiterGuid)) {
        ocrTask_t * target = NULL;
        deguidify(getCurrentPD(), waiterGuid, (u64*)&target, NULL);
        taskSignaled(target, data, slot);
    } else {
        // ERROR
        ASSERT(0 && "error: Unsupported guid kind in signal");
    }
}
