/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_EVENT_HC

#include "ocr-hal.h"
#include "debug.h"
#include "event/hc/hc-event.h"
#include "ocr-datablock.h"
#include "ocr-edt.h"
#include "ocr-event.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"
#include "ocr-errors.h"

#if defined (ENABLE_RESILIENCY) && defined (ENABLE_CHECKPOINT_VERIFICATION)
#include "policy-domain/hc/hc-policy.h"
#endif

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#endif

#define SEALED_LIST ((void *) -1)
#define END_OF_LIST NULL

#define DEBUG_TYPE EVENT

// Custom DEBUG_LVL for debugging
#define DBG_HCEVT_LOG   DEBUG_LVL_VERB
#define DBG_HCEVT_ERR   DEBUG_LVL_WARN


/******************************************************/
/* OCR-HC Debug                                       */
/******************************************************/

#if defined(OCR_DEBUG) && !defined(OCR_TRACE_BINARY)
static char * eventTypeToString(ocrEvent_t * base) {
    ocrEventTypes_t type = base->kind;
    if(type == OCR_EVENT_ONCE_T) {
        return "once";
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    } else if (type == OCR_EVENT_COUNTED_T) {
        return "counted";
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    } else if (type == OCR_EVENT_CHANNEL_T) {
        return "channel";
#endif
    } else if (type == OCR_EVENT_IDEM_T) {
        return "idem";
    } else if (type == OCR_EVENT_STICKY_T) {
        return "sticky";
    } else if (type == OCR_EVENT_LATCH_T) {
        return "latch";
    } else {
        return "unknown";
    }
}
#endif


/***********************************************************/
/* OCR-HC Event Hint Properties                             */
/* (Add implementation specific supported properties here) */
/***********************************************************/

u64 ocrHintPropEventHc[] = {
#ifdef ENABLE_HINTS
#endif
};

//Make sure OCR_HINT_COUNT_EVT_HC in hc-task.h is equal to the length of array ocrHintPropEventHc
ocrStaticAssert((sizeof(ocrHintPropEventHc)/sizeof(u64)) == OCR_HINT_COUNT_EVT_HC);
ocrStaticAssert(OCR_HINT_COUNT_EVT_HC < OCR_RUNTIME_HINT_PROP_BITS);


/******************************************************/
/* OCR-HC Distributed Events Implementation           */
/******************************************************/

//To forge a local copy without communication.
//This ability is implementation dependent.
#ifndef ENABLE_EVENT_MDC_FORGE
#define ENABLE_EVENT_MDC_FORGE 0
#endif

// Metadata synchronization operations
#define M_CLONE 0
#define M_REG 1
#define M_SAT 2
#define M_DEL 3

typedef struct {
    ocrLocation_t location;
    ocrGuid_t guid;
} locguid_payload;

typedef struct {
    ocrLocation_t location;
} loc_payload;

typedef struct {
    ocrGuid_t guid;
} guid_payload;

#define M_SAT_payload    locguid_payload
#define M_REG_payload    loc_payload
#define M_DEL_payload    loc_payload
#define M_CLONE_payload  guid_payload

#define GET_PAYLOAD_DATA(buffer, mode, type, name)       ((type)((mode##_payload *) buffer)->name)
#define SET_PAYLOAD_DATA(buffer, mode, type, name, val)  ((((mode##_payload *) buffer)->name) = (type) val)
#define WR_PAYLOAD_DATA(buffer, type, val)               (((type*)buffer)[0] = (type) val)


static void mdPushHcDist(ocrGuid_t evtGuid, ocrLocation_t loc, ocrGuid_t dbGuid, u32 mode, u32 factoryId);


/******************************************************/
/* OCR-HC Events Implementation                       */
/******************************************************/

static u8 createDbRegNode(ocrFatGuid_t * dbFatGuid, u32 nbElems, bool doRelease, regNode_t ** node) {
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    ocrTask_t *curTask = NULL;
    u32 i;
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t curEdt = {.guid = curTask!=NULL?curTask->guid:NULL_GUID, .metaDataPtr = curTask};
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_CREATE
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.type = PD_MSG_DB_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = (*dbFatGuid);
    PD_MSG_FIELD_IO(size) = sizeof(regNode_t)*nbElems;
    PD_MSG_FIELD_IO(properties) = DB_PROP_RT_ACQUIRE;
    PD_MSG_FIELD_I(edt) = curEdt;
    PD_MSG_FIELD_I(hint) = NULL_HINT;
    PD_MSG_FIELD_I(dbType) = RUNTIME_DBTYPE;
    PD_MSG_FIELD_I(allocator) = NO_ALLOC;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
    (*dbFatGuid) = PD_MSG_FIELD_IO(guid);
    regNode_t * temp = (regNode_t*) PD_MSG_FIELD_O(ptr);
    *node = temp;
    for(i = 0; i < nbElems; ++i) {
        temp[i].guid = UNINITIALIZED_GUID;
        temp[i].slot = 0;
        temp[i].mode = -1;
    }
#undef PD_TYPE
#define PD_TYPE PD_MSG_DB_RELEASE
    if (doRelease) {
        getCurrentEnv(NULL, NULL, NULL, &msg);
        msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid) = (*dbFatGuid);
        PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
        PD_MSG_FIELD_I(edt) = curEdt;
        PD_MSG_FIELD_I(ptr) = NULL;
        PD_MSG_FIELD_I(size) = 0;
        PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
        *node = NULL;
    }
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

//
// OCR-HC Single Events Implementation
//

static void destructEventHcPeers(ocrEvent_t *base, locNode_t * curHead);

u8 destructEventHc(ocrEvent_t *base) {
    ocrEventHc_t *event = (ocrEventHc_t*)base;
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    ocrTask_t *curTask = NULL;
    getCurrentEnv(&pd, NULL, &curTask, &msg);

    DPRINTF(DEBUG_LVL_INFO, "Destroy %s: "GUIDF"\n", eventTypeToString(base), GUIDA(base->guid));
    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EVENT, OCR_ACTION_DESTROY, traceEventDestroy, base->guid);

#ifdef OCR_ENABLE_STATISTICS
    statsEVT_DESTROY(pd, getCurrentEDT(), NULL, base->guid, base);
#endif

    // Destroy datablocks linked with this event
    if (!(ocrGuidIsUninitialized(event->waitersDb.guid))) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_FREE
        msg.type = PD_MSG_DB_FREE | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(guid) = event->waitersDb;
        PD_MSG_FIELD_I(edt.guid) = curTask ? curTask->guid : NULL_GUID;
        PD_MSG_FIELD_I(edt.metaDataPtr) = curTask;
        PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
        PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE | DB_PROP_NO_RELEASE;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    }

    // Now destroy the GUID
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    // These next two statements may be not required. Just to be safe.
    PD_MSG_FIELD_I(guid.guid) = base->guid;
    PD_MSG_FIELD_I(guid.metaDataPtr) = base;
    PD_MSG_FIELD_I(properties) = 1; // Free metadata
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

#define STATE_CHECKED_IN ((u32)-1)
#define STATE_CHECKED_OUT ((u32)-2)
#define STATE_DESTROY_SEEN ((u32)-3)

// For Sticky and Idempotent
u8 destructEventHcPersist(ocrEvent_t *base) {
    ocrEventHc_t *event = (ocrEventHc_t*) base;
    // Addresses a race when the EDT that's satisfying the
    // event is still using the event's metadata but the children
    // EDT is has already invoked the destruct function.
    // BUG #537 could potentially improve on that by creating a lightweight
    // asynchronous operation to reschedule the destruction instead
    // of competing.
    u32 wc = event->waitersCount;
    // - Can be STATE_CHECKED_IN: competing for destruction with satisfy
    // - Can be STATE_CHECKED_OUT: We should win this competition
    // - Can be any other value: We are deleting the event before it is satisfied.
    //   It's either a race in the user program or an early destruction of the event.
    //
    // By contract the satisfy code should directly call destructEventHc
    // if it wins the right to invoke the destruction code
    ASSERT(wc != STATE_DESTROY_SEEN);
    if (wc == STATE_CHECKED_IN) {
        // Competing with the satisfy waiters code
        u32 oldV = hal_cmpswap32(&(event->waitersCount), wc, STATE_DESTROY_SEEN);
        if (wc == oldV) {
            // Successfully CAS from STATE_CHECKED_IN => STATE_DESTROY_SEEN
            // i.e. we lost competition: Satisfier will destroy the event
            // Return code zero as the event is 'scheduled' for deletion,
            // it just won't happen through this path.
            return 0;
        } else { // CAS failed
            // Was competing with the CAS in satisfy which only
            // does one thing: STATE_CHECKED_IN => STATE_CHECKED_OUT
            ASSERT(event->waitersCount == STATE_CHECKED_OUT);
            // fall-through and destroy the event
        }
    }
    //BUG #989: MT opportunity
    destructEventHcPeers(base, event->mdClass.peers);
    return destructEventHc(base);
}

#ifdef ENABLE_EXTENSION_CHANNEL_EVT
static u32 channelSatisfyCount(ocrEventHcChannel_t * devt) {
    return (devt->tailSat - devt->headSat);
}

static u32 channelWaiterCount(ocrEventHcChannel_t * devt) {
    return (devt->tailWaiter - devt->headWaiter);
}
#endif

ocrFatGuid_t getEventHc(ocrEvent_t *base) {
    ocrFatGuid_t res = {.guid = NULL_GUID, .metaDataPtr = NULL};
    switch(base->kind) {
    case OCR_EVENT_ONCE_T:
    case OCR_EVENT_LATCH_T:
        break;
    case OCR_EVENT_STICKY_T:
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    case OCR_EVENT_COUNTED_T:
#endif
    {
        ocrEventHcPersist_t *event = (ocrEventHcPersist_t*)base;
        res.guid = event->data;
        break;
    }
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    case OCR_EVENT_CHANNEL_T:
    {
        // Warning: Not thread safe and only used for mpi-lite legacy support to
        // let the caller know if the channel got enough deps/sat to trigger.
        // If so, it returns the first guid in the satisfy list without consuming it.
        ocrEventHcChannel_t *devt = (ocrEventHcChannel_t*)base;
        u32 satCount = channelSatisfyCount(devt);
        u32 waitCount = channelWaiterCount(devt);
        if ((satCount >= devt->nbSat) && (waitCount >= devt->nbDeps)) {
            res.guid = devt->satBuffer[devt->headSat];
        } else {
            res.guid = UNINITIALIZED_GUID;
        }
        break;
    }
#endif
    default:
        ASSERT(0);
    }
    return res;
}

static u8 commonSatisfyRegNode(ocrPolicyDomain_t * pd, ocrPolicyMsg_t * msg,
                         ocrGuid_t evtGuid,
                         ocrFatGuid_t db, ocrFatGuid_t currentEdt,
                         regNode_t * node) {
#ifdef OCR_ENABLE_STATISTICS
    //TODO the null should be the base but it's a race
    statsDEP_SATISFYFromEvt(pd, evtGuid, NULL, node->guid,
                            db.guid, node->slot);
#endif
    DPRINTF(DEBUG_LVL_INFO, "SatisfyFromEvent: src: "GUIDF" dst: "GUIDF" \n", GUIDA(evtGuid), GUIDA(node->guid));
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    getCurrentEnv(NULL, NULL, NULL, msg);
    msg->type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    // Need to refill because out may overwrite some of the in fields
    PD_MSG_FIELD_I(satisfierGuid.guid) = evtGuid;
    // Passing NULL since base may become invalid
    PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(guid.guid) = node->guid;
    PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(payload) = db;
    PD_MSG_FIELD_I(currentEdt) = currentEdt;
    PD_MSG_FIELD_I(slot) = node->slot;
#ifdef REG_ASYNC_SGL
    PD_MSG_FIELD_I(mode) = node->mode;
#endif
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, false));
#undef PD_MSG
#undef PD_TYPE

    return 0;
}

static u8 commonSatisfyWaiters(ocrPolicyDomain_t *pd, ocrEvent_t *base, ocrFatGuid_t db, u32 waitersCount,
                                ocrFatGuid_t currentEdt, ocrPolicyMsg_t * msg,
                                bool isPersistentEvent) {
    ocrEventHc_t * event = (ocrEventHc_t *) base;
    // waitersDb is safe to read because non-persistent event forbids further
    // registrations and persistent event registration is closed because of
    // event->waitersCount set to STATE_CHECKED_IN.
    ocrFatGuid_t dbWaiters = event->waitersDb;
    u32 i;
#if HCEVT_WAITER_STATIC_COUNT
    u32 ub = ((waitersCount < HCEVT_WAITER_STATIC_COUNT) ? waitersCount : HCEVT_WAITER_STATIC_COUNT);
    // Do static waiters first
    for(i = 0; i < ub; ++i) {
        RESULT_PROPAGATE(commonSatisfyRegNode(pd, msg, base->guid, db, currentEdt, &event->waiters[i]));
    }
    waitersCount -= ub;
#endif

    if(waitersCount > 0) {
        ASSERT(!(ocrGuidIsUninitialized(dbWaiters.guid)));
        // First acquire the DB that contains the waiters
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
        msg->type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid) = dbWaiters;
        PD_MSG_FIELD_IO(edt) = currentEdt;
        PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
        PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
        if (isPersistentEvent) {
            // !! Warning !! RW here (and not RO) works in pair with the lock
            // being unlocked before DB_RELEASE is called in 'registerWaiterEventHcPersist'
            PD_MSG_FIELD_IO(properties) = DB_MODE_RW | DB_PROP_RT_ACQUIRE;
        } else {
            PD_MSG_FIELD_IO(properties) = DB_MODE_CONST | DB_PROP_RT_ACQUIRE;
        }
        u8 res __attribute__((unused)) = pd->fcts.processMessage(pd, msg, true);
        ASSERT(!res);
        regNode_t * waiters = (regNode_t*)PD_MSG_FIELD_O(ptr);
        //BUG #273: related to 273: we should not get an updated deguidification...
        dbWaiters = PD_MSG_FIELD_IO(guid); //Get updated deguidifcation if needed
        ASSERT(waiters);
#undef PD_TYPE

        // Second, call satisfy on all the waiters
        for(i = 0; i < waitersCount; ++i) {
            RESULT_PROPAGATE(commonSatisfyRegNode(pd, msg, base->guid, db, currentEdt, &waiters[i]));
        }

        // Release the DB
#define PD_TYPE PD_MSG_DB_RELEASE
        getCurrentEnv(NULL, NULL, NULL, msg);
        msg->type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid) = dbWaiters;
        PD_MSG_FIELD_I(edt) = currentEdt;
        PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
        PD_MSG_FIELD_I(ptr) = NULL;
        PD_MSG_FIELD_I(size) = 0;
        PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, true));
#undef PD_MSG
#undef PD_TYPE
    }

    return 0;
}

// For once events, we don't have to worry about
// concurrent registerWaiter calls (this would be a programmer error)
u8 satisfyEventHcOnce(ocrEvent_t *base, ocrFatGuid_t db, u32 slot) {
    ocrEventHc_t *event = (ocrEventHc_t*)base;
    ASSERT(slot == 0); // For non-latch events, only one slot

    DPRINTF(DEBUG_LVL_INFO, "Satisfy: "GUIDF" with "GUIDF"\n", GUIDA(base->guid), GUIDA(db.guid));

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t currentEdt;
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;
    u32 waitersCount = event->waitersCount;
    // This is only to help users find out about wrongful use of events
    event->waitersCount = STATE_CHECKED_IN; // Indicate that the event is satisfied

#ifdef OCR_ENABLE_STATISTICS
    statsDEP_SATISFYToEvt(pd, currentEdt.guid, NULL, base->guid, base, data, slot);
#endif

    if (waitersCount) {
        RESULT_PROPAGATE(commonSatisfyWaiters(pd, base, db, waitersCount, currentEdt, &msg, false));
    }

    // Since this a ONCE event, we need to destroy it as well
    // This is safe to do so at this point as all the messages have been sent
    return destructEventHc(base);
}

static u8 commonSatisfyEventHcPersist(ocrEvent_t *base, ocrFatGuid_t db, u32 slot, u32 waitersCount) {
    ASSERT(slot == 0); // Persistent-events are single slot
    DPRINTF(DEBUG_LVL_INFO, "Satisfy %s: "GUIDF" with "GUIDF"\n", eventTypeToString(base),
            GUIDA(base->guid), GUIDA(db.guid));

#ifdef OCR_ENABLE_STATISTICS
    ocrPolicyDomain_t *pd = getCurrentPD();
    ocrGuid_t edt = getCurrentEDT();
    statsDEP_SATISFYToEvt(pd, edt, NULL, base->guid, base, data, slot);
#endif
    // Process waiters to be satisfied
    if(waitersCount) {
        ocrPolicyDomain_t *pd = NULL;
        ocrTask_t *curTask = NULL;
        PD_MSG_STACK(msg);
        getCurrentEnv(&pd, NULL, &curTask, &msg);
        ocrFatGuid_t currentEdt;
        currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
        currentEdt.metaDataPtr = curTask;
        RESULT_PROPAGATE(commonSatisfyWaiters(pd, base, db, waitersCount, currentEdt, &msg, true));
    }
    u32 oldV = hal_cmpswap32(&(((ocrEventHc_t*)base)->waitersCount), STATE_CHECKED_IN, STATE_CHECKED_OUT);
    if (oldV == STATE_DESTROY_SEEN) {
        // CAS has failed because of a concurrent destroy operation, which means that we
        // won the right to destroy the event. i.e. we are logically checked out and the
        // destroy code marked the event for deletion but couldn't destroy because we were
        // checked in.
        destructEventHc(base);
    }
    // else we just checked out and there may be a concurrent deletion happening
    // => do not touch the event pointer anymore
    return 0;
}

// Notify peers we got a satisfy notification.
static void satisfyEventHcPeers(ocrEvent_t *base, ocrGuid_t dbGuid, u32 slot, locNode_t * curHead) {
    ocrLocation_t fromLoc = ((ocrEventHc_t *) base)->mdClass.satFromLoc;
    // We may get concurrent registrations but that's ok as they'll get added before curHead.
    while (curHead != NULL) {
        // NOTE-1: There's an ordering constraint between the M_SAT here and a concurrent
        // destruct operation that's on hold (because of ->waitersCount != -2).
        // => The M_DEL message that's triggered MUST be processed after the M_SAT at destination.
        // TODO: I think the M_DEL/M_SAT issue is currently a live bug because we do not have a way
        // of ordering messages processing at destination. Although all M_SAT are sent, the M_DEL may
        // outrun it.
        // NOTE-2: do not send a M_SAT to the emitter of the satisfy.
        if (curHead->loc != fromLoc) {
            mdPushHcDist(base->guid, curHead->loc, dbGuid, M_SAT, base->fctId);
        }
        curHead = curHead->next;
    }
}

// Notify peers we got a satisfy notification.
static void destructEventHcPeers(ocrEvent_t *base, locNode_t * curHead) {
    ocrLocation_t fromLoc = ((ocrEventHc_t *) base)->mdClass.delFromLoc;
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    while (curHead != NULL) {
        if (curHead->loc != fromLoc) {
            mdPushHcDist(base->guid, curHead->loc, NULL_GUID , M_DEL, base->fctId);
        }
        locNode_t * oldHead = curHead;
        curHead = curHead->next;
        pd->fcts.pdFree(pd, oldHead);
    }
}

#ifdef ENABLE_EXTENSION_COUNTED_EVT
// For counted event
u8 satisfyEventHcCounted(ocrEvent_t *base, ocrFatGuid_t db, u32 slot) {
    ocrEventHc_t * event = (ocrEventHc_t*) base;
    bool destroy = false;
    hal_lock(&(event->waitersLock));
    //BUG #809 Nanny-mode
    if ((event->waitersCount == STATE_CHECKED_IN) ||
        (event->waitersCount == STATE_CHECKED_OUT)) {
        DPRINTF(DBG_HCEVT_ERR, "User-level error detected: try to satisfy a counted event that's already satisfied: "GUIDF"\n", GUIDA(base->guid));
        ASSERT(false);
        hal_unlock(&(event->waitersLock));
        return 1; //BUG #603 error codes: Put some error code here.
    }
    ((ocrEventHcPersist_t*)event)->data = db.guid;
    u32 waitersCount = event->waitersCount;
    event->waitersCount = STATE_CHECKED_IN; // Indicate the event is satisfied
    ocrEventHcCounted_t * devt = (ocrEventHcCounted_t *) event;
    ASSERT_BLOCK_BEGIN(waitersCount <= devt->nbDeps)
    DPRINTF(DBG_HCEVT_ERR, "User-level error detected: too many registrations on counted-event "GUIDF"\n", GUIDA(base->guid));
    ASSERT_BLOCK_END

    devt->nbDeps -= waitersCount;
    destroy = ((devt->nbDeps) == 0);
    hal_unlock(&(event->waitersLock));
    u8 ret = commonSatisfyEventHcPersist(base, db, slot, waitersCount);
    if (destroy) {
        ret = destructEventHc(base);
    }
    return ret;
}
#endif


static u32 setSatisfiedEventHcPersist(ocrEvent_t *base, ocrFatGuid_t db, locNode_t ** curHead, bool checkError) {
    ocrEventHc_t * devt = (ocrEventHc_t*) base;
    hal_lock(&(devt->waitersLock));
    if ((devt->waitersCount == STATE_CHECKED_IN) ||
        (devt->waitersCount == STATE_CHECKED_OUT)) {
        if (checkError) {
            // Sticky needs to check for error, idem just ignores by definition.
            //BUG #809 Nanny-mode
            DPRINTF(DBG_HCEVT_ERR, "User-level error detected: try to satisfy a sticky event that's already satisfied: "GUIDF"\n", GUIDA(base->guid));
            ASSERT(false);
        }
        hal_unlock(&(devt->waitersLock));
        return STATE_CHECKED_IN;
    }
    ((ocrEventHcPersist_t*)devt)->data = db.guid;
    u32 waitersCount = devt->waitersCount;
    devt->waitersCount = STATE_CHECKED_IN; // Indicate the event is satisfied
    //RACE-1: Get the current head for the peer list. Note that once we release the lock
    // there may be new registrations on the peer list. It's ok though, they will be
    // getting the GUID the event is satisfied with as part of the serialization protocol.
    *curHead = devt->mdClass.peers;
    //Note that we do not close registrations here because we still want to record
    //subsequent peers for destruction purpose
    hal_unlock(&(devt->waitersLock));
    return waitersCount;
}

// For idempotent events, accessed through the fct pointers interface
u8 satisfyEventHcPersistIdem(ocrEvent_t *base, ocrFatGuid_t db, u32 slot) {
    // Register the satisfy
    locNode_t * curHead;
    u32 waitersCount = setSatisfiedEventHcPersist(base, db, &curHead, /*checkError*/ false);
    if (waitersCount != STATE_CHECKED_IN) {
        //BUG #989: MT opportunity with two following calls micro-tasks and
        //          have a 'join' event to set the destruction flag.
        // Notify peers
        satisfyEventHcPeers(base, db.guid, slot, curHead);
        // Notify waiters
        u8 res = commonSatisfyEventHcPersist(base, db, slot, waitersCount);
        ASSERT(!res);
        // Set destruction flag
        hal_fence();// make sure all operations are done
        // This is to signal a concurrent destruct currently
        // waiting on the satisfaction being done it can now
        // proceed.
        ((ocrEventHc_t*)base)->waitersCount = STATE_CHECKED_OUT;
    }
    return 0;
}

// For sticky events, accessed through the fct pointers interface
u8 satisfyEventHcPersistSticky(ocrEvent_t *base, ocrFatGuid_t db, u32 slot) {
    // Register the satisfy
    locNode_t * curHead;
    u32 waitersCount = setSatisfiedEventHcPersist(base, db, &curHead, /*checkError*/ true);
    ASSERT(waitersCount != STATE_CHECKED_IN); // i.e. no two satisfy on stickies
    //BUG #989: MT opportunity with two following calls micro-tasks and
    //          have a 'join' event to set the destruction flag.
    // Notify peers
    satisfyEventHcPeers(base, db.guid, slot, curHead);
    // Notify waiters
    u8 res = commonSatisfyEventHcPersist(base, db, slot, waitersCount);
    ASSERT(!res);
    // Set destruction flag
    hal_fence();// make sure all operations are done
    // This is to signal a concurrent destruct currently
    // waiting on the satisfaction being done it can now
    // proceed.
    ((ocrEventHc_t*)base)->waitersCount = STATE_CHECKED_OUT;
    return 0;
}

// This is for latch events
u8 satisfyEventHcLatch(ocrEvent_t *base, ocrFatGuid_t db, u32 slot) {
    ocrEventHcLatch_t *event = (ocrEventHcLatch_t*)base;
    ASSERT(slot == OCR_EVENT_LATCH_DECR_SLOT ||
           slot == OCR_EVENT_LATCH_INCR_SLOT);

    s32 incr = (slot == OCR_EVENT_LATCH_DECR_SLOT)?-1:1;
    s32 count;
    do {
        count = event->counter;
        // FIXME: the (u32 *) cast on the line below is because event->counter is an (s32 *)
    } while(hal_cmpswap32((u32 *)&(event->counter), count, count+incr) != count);

    DPRINTF(DEBUG_LVL_INFO, "Satisfy %s: "GUIDF" %s\n", eventTypeToString(base),
            GUIDA(base->guid), ((slot == OCR_EVENT_LATCH_DECR_SLOT) ? "decr":"incr"));

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t currentEdt;
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;

#ifdef OCR_ENABLE_STATISTICS
    statsDEP_SATISFYToEvt(pd, currentEdt.guid, NULL, base->guid, base, data, slot);
#endif
    if(count + incr != 0) {
        return 0;
    }
    // Here the event is satisfied
    DPRINTF(DEBUG_LVL_INFO, "Satisfy %s: "GUIDF" reached zero\n", eventTypeToString(base), GUIDA(base->guid));

    u32 waitersCount = event->base.waitersCount;
    // This is only to help users find out about wrongful use of events
    event->base.waitersCount = STATE_CHECKED_IN; // Indicate that the event is satisfied

    if (waitersCount) {
        RESULT_PROPAGATE(commonSatisfyWaiters(pd, base, db, waitersCount, currentEdt, &msg, false));
    }

    // The latch is satisfied so we destroy it
    return destructEventHc(base);
}

u8 registerSignalerHc(ocrEvent_t *self, ocrFatGuid_t signaler, u32 slot,
                      ocrDbAccessMode_t mode, bool isDepAdd) {
    return 0; // We do not do anything for signalers
}

u8 unregisterSignalerHc(ocrEvent_t *self, ocrFatGuid_t signaler, u32 slot,
                        bool isDepRem) {
    return 0; // We do not do anything for signalers
}

#ifdef REG_ASYNC_SGL
static u8 commonEnqueueWaiter(ocrPolicyDomain_t *pd, ocrEvent_t *base, ocrFatGuid_t waiter,
                              u32 slot, ocrDbAccessMode_t mode, ocrFatGuid_t currentEdt, ocrPolicyMsg_t * msg) {
#else
static u8 commonEnqueueWaiter(ocrPolicyDomain_t *pd, ocrEvent_t *base, ocrFatGuid_t waiter,
                              u32 slot, ocrFatGuid_t currentEdt, ocrPolicyMsg_t * msg) {
#endif
    // Warn: Caller must have acquired the waitersLock
    ocrEventHc_t *event = (ocrEventHc_t*)base;
    u32 waitersCount = event->waitersCount;

#if HCEVT_WAITER_STATIC_COUNT
    if (waitersCount < HCEVT_WAITER_STATIC_COUNT) {
        event->waiters[waitersCount].guid = waiter.guid;
        event->waiters[waitersCount].slot = slot;
#ifdef REG_ASYNC_SGL
        event->waiters[waitersCount].mode = mode;
#endif
        ++event->waitersCount;
        // We can release the lock now
        hal_unlock(&(event->waitersLock));
    } else {
#endif
        ocrFatGuid_t oldDbGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
        ocrFatGuid_t dbGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
        regNode_t *waiters = NULL;
        regNode_t *waitersNew = NULL;
        u8 toReturn = 0;
        // We're working with the dynamically created waiter list
        if (waitersCount == HCEVT_WAITER_STATIC_COUNT) {
            // Initial setup
            u8 toReturn = createDbRegNode(&(event->waitersDb), HCEVT_WAITER_DYNAMIC_COUNT, false, &waiters);
            if (toReturn) {
                ASSERT(false && "Failed allocating db waiter");
                hal_unlock(&(event->waitersLock));
                return toReturn;
            }
            dbGuid = event->waitersDb; // for release
            event->waitersMax += HCEVT_WAITER_DYNAMIC_COUNT;
            waitersCount=0;
        } else {
            // Acquire the DB that contains the waiters
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
            getCurrentEnv(NULL, NULL, NULL, msg);
            msg->type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_IO(guid) = event->waitersDb;
            PD_MSG_FIELD_IO(edt) = currentEdt;
            PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
            PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
            PD_MSG_FIELD_IO(properties) = DB_MODE_RW | DB_PROP_RT_ACQUIRE;
            //Should be a local DB
            if((toReturn = pd->fcts.processMessage(pd, msg, true))) {
                // should be the only writer active on the waiter DB since we have the lock
                ASSERT(false); // debug
                ASSERT(toReturn != OCR_EBUSY);
                hal_unlock(&(event->waitersLock));
                return toReturn; //BUG #603 error codes
            }
            waiters = (regNode_t*)PD_MSG_FIELD_O(ptr);
            //BUG #273
            event->waitersDb = PD_MSG_FIELD_IO(guid);
            ASSERT(waiters);
#undef PD_TYPE
            if(waitersCount + 1 == event->waitersMax) {
                // We need to create a new DB and copy things over
#define PD_TYPE PD_MSG_DB_CREATE
                getCurrentEnv(NULL, NULL, NULL, msg);
                msg->type = PD_MSG_DB_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid) = dbGuid;
                PD_MSG_FIELD_IO(properties) = DB_PROP_RT_ACQUIRE;
                PD_MSG_FIELD_IO(size) = sizeof(regNode_t)*event->waitersMax*2;
                PD_MSG_FIELD_I(edt) = currentEdt;
                PD_MSG_FIELD_I(hint) = NULL_HINT;
                PD_MSG_FIELD_I(dbType) = RUNTIME_DBTYPE;
                PD_MSG_FIELD_I(allocator) = NO_ALLOC;
                if((toReturn = pd->fcts.processMessage(pd, msg, true))) {
                    ASSERT(false); // debug
                    hal_unlock(&(event->waitersLock));
                    return toReturn; //BUG #603 error codes
                }
                waitersNew = (regNode_t*)PD_MSG_FIELD_O(ptr);
                oldDbGuid = event->waitersDb;
                dbGuid = PD_MSG_FIELD_IO(guid);
                event->waitersDb = dbGuid;
#undef PD_TYPE
                // -HCEVT_WAITER_STATIC_COUNT because part of the waiters are in the statically allocated waiter array
                u32 nbNodes = waitersCount-HCEVT_WAITER_STATIC_COUNT;
                hal_memCopy(waitersNew, waiters, sizeof(regNode_t)*(nbNodes), false);
                event->waitersMax *= 2;
                u32 i;
                u32 maxNbNodes = event->waitersMax-HCEVT_WAITER_STATIC_COUNT;
                for(i = nbNodes; i < maxNbNodes; ++i) {
                    waitersNew[i].guid = NULL_GUID;
                    waitersNew[i].slot = 0;
                    waitersNew[i].mode = -1;
                }
                waiters = waitersNew;
            } else {
                dbGuid = event->waitersDb; // for release
            }
            waitersCount=event->waitersCount-HCEVT_WAITER_STATIC_COUNT;
        }

        waiters[waitersCount].guid = waiter.guid;
        waiters[waitersCount].slot = slot;
#ifdef REG_ASYNC_SGL
        waiters[waitersCount].mode = mode;
#endif
        ++event->waitersCount;

        // We can release the lock now
        hal_unlock(&(event->waitersLock));

        // Release the waiter datablock / free old waiter DB when necessary
        //
        // In both cases it is important to release the GUID read from the cached
        // DB value and not from the event data-structure since we're operating
        // outside the lock there can be a new db created and assigned before getting here
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_RELEASE
        getCurrentEnv(NULL, NULL, NULL, msg);
        msg->type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid) = dbGuid;
        PD_MSG_FIELD_I(edt) = currentEdt;
        PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
        PD_MSG_FIELD_I(ptr) = NULL;
        PD_MSG_FIELD_I(size) = 0;
        PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, msg, true));
#undef PD_MSG
#undef PD_TYPE

        if(waitersNew) {
            // We need to free the old DB (implicitely released as DB_PROP_NO_RELEASE
            // is not provided here) and release the new one.
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_FREE
            getCurrentEnv(NULL, NULL, NULL, msg);
            msg->type = PD_MSG_DB_FREE | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(guid) = oldDbGuid;
            PD_MSG_FIELD_I(edt) = currentEdt;
            PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
            PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE;
            if((toReturn = pd->fcts.processMessage(pd, msg, false))) {
                ASSERT(false); // debug
                return toReturn; //BUG #603 error codes
            }
#undef PD_MSG
#undef PD_TYPE
        }
#if HCEVT_WAITER_STATIC_COUNT
    }
#endif
    return 0; //Require registerSignaler invocation
}



/**
 * In this call, we do not contend with the satisfy (once and latch events) however,
 * we do contend with multiple registration.
 * By construction, users must ensure a ONCE event is registered before satisfy is called.
 */
#ifdef REG_ASYNC_SGL
u8 registerWaiterEventHc(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd, ocrDbAccessMode_t mode) {
#else
u8 registerWaiterEventHc(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd) {
#endif
    // Here we always add the waiter to our list so we ignore isDepAdd
    ocrEventHc_t *event = (ocrEventHc_t*)base;

    DPRINTF(DEBUG_LVL_INFO, "Register waiter %s: "GUIDF" with waiter "GUIDF" on slot %"PRId32"\n",
            eventTypeToString(base), GUIDA(base->guid), GUIDA(waiter.guid), slot);

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    //BUG #809 this should be part of the n
    if (event->waitersCount == STATE_CHECKED_IN) {
         // This is best effort race check
         DPRINTF(DBG_HCEVT_ERR, "User-level error detected: adding dependence to a non-persistent event that's already satisfied: "GUIDF"\n", GUIDA(base->guid));
         ASSERT(false);
         return 1; //BUG #603 error codes: Put some error code here.
    }
    ocrFatGuid_t currentEdt = {.guid = curTask!=NULL?curTask->guid:NULL_GUID, .metaDataPtr = curTask};
    hal_lock(&(event->waitersLock)); // Lock is released by commonEnqueueWaiter
#ifdef REG_ASYNC_SGL
    return commonEnqueueWaiter(pd, base, waiter, slot, mode, currentEdt, &msg);
#else
    return commonEnqueueWaiter(pd, base, waiter, slot, currentEdt, &msg);
#endif
}


/**
 * @brief Registers waiters on persistent events such as sticky or idempotent.
 *
 * This code contends with a satisfy call and with concurrent add-dependences that try
 * to register their waiter.
 * The event waiterLock is grabbed, if the event is already satisfied, directly satisfy
 * the waiter. Otherwise add the waiter's guid to the waiters db list. If db is too small
 * reallocate and copy over to a new one.
 *
 * Returns non-zero if the registerWaiter requires registerSignaler to be called there-after
 */
#ifdef REG_ASYNC_SGL
u8 registerWaiterEventHcPersist(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd, ocrDbAccessMode_t mode) {
#else
u8 registerWaiterEventHcPersist(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd) {
#endif
    ocrEventHcPersist_t *event = (ocrEventHcPersist_t*)base;

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t currentEdt;
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;

    // EDTs incrementally register on their dependences as elements
    // get satisfied (Guarantees O(n) traversal of dependence list).
    // Other events register when the dependence is added
    ocrGuidKind waiterKind = OCR_GUID_NONE;
    RESULT_ASSERT(guidKind(pd, waiter, &waiterKind), ==, 0);

#ifndef REG_ASYNC
#ifndef REG_ASYNC_SGL
    if(isDepAdd && waiterKind == OCR_GUID_EDT) {
        ASSERT(false && "Should never happen anymore");
        // If we're adding a dependence and the waiter is an EDT we
        // skip this part. The event is registered on the EDT and
        // the EDT will register on the event only when its dependence
        // frontier reaches this event.
        return 0; //Require registerSignaler invocation
    }
#endif
#endif
    ASSERT(waiterKind == OCR_GUID_EDT || (waiterKind & OCR_GUID_EVENT));

    DPRINTF(DEBUG_LVL_INFO, "Register waiter %s: "GUIDF" with waiter "GUIDF" on slot %"PRId32"\n",
            eventTypeToString(base), GUIDA(base->guid), GUIDA(waiter.guid), slot);
    // Lock to read the event->data
    hal_lock(&(event->base.waitersLock));
    if (!(ocrGuidIsUninitialized(event->data))) {
        ocrFatGuid_t dataGuid = {.guid = event->data, .metaDataPtr = NULL};
        hal_unlock(&(event->base.waitersLock));

#ifdef REG_ASYNC_SGL
        regNode_t node = {.guid = waiter.guid, .slot = slot, .mode = mode};
#else
        regNode_t node = {.guid = waiter.guid, .slot = slot};
#endif
        // We send a message saying that we satisfy whatever tried to wait on us
        return commonSatisfyRegNode(pd, &msg, base->guid, dataGuid, currentEdt, &node);
    }

    // Lock is released by commonEnqueueWaiter
#ifdef REG_ASYNC_SGL
    return commonEnqueueWaiter(pd, base, waiter, slot, mode, currentEdt, &msg);
#else
    return commonEnqueueWaiter(pd, base, waiter, slot, currentEdt, &msg);
#endif
}

/**
 * @brief Registers waiters on a counted-event.
 *
 * Pretty much the same code as for persistent events, see comments there
 *
 * Returns non-zero if the registerWaiter requires registerSignaler to be called there-after
 */
#ifdef ENABLE_EXTENSION_COUNTED_EVT
#ifdef REG_ASYNC_SGL
u8 registerWaiterEventHcCounted(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd, ocrDbAccessMode_t mode) {
#else
u8 registerWaiterEventHcCounted(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd) {
#endif
    ocrEventHcPersist_t *event = (ocrEventHcPersist_t*)base;

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t currentEdt;
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;

    // EDTs incrementally register on their dependences as elements
    // get satisfied (Guarantees O(n) traversal of dependence list).
    // Other events register when the dependence is added
    ocrGuidKind waiterKind = OCR_GUID_NONE;
    RESULT_ASSERT(guidKind(pd, waiter, &waiterKind), ==, 0);

#ifndef REG_ASYNC
#ifndef REG_ASYNC_SGL
    if(isDepAdd && waiterKind == OCR_GUID_EDT) {
        ASSERT(false && "Should never happen anymore");
        // If we're adding a dependence and the waiter is an EDT we
        // skip this part. The event is registered on the EDT and
        // the EDT will register on the event only when its dependence
        // frontier reaches this event.
        return 0; //Require registerSignaler invocation
    }
#endif
#endif
    ASSERT(waiterKind == OCR_GUID_EDT || (waiterKind & OCR_GUID_EVENT));

    DPRINTF(DEBUG_LVL_INFO, "Register waiter %s: "GUIDF" with waiter "GUIDF" on slot %"PRId32"\n",
            eventTypeToString(base), GUIDA(base->guid), GUIDA(waiter.guid), slot);
    // Lock to read the data field
    hal_lock(&(event->base.waitersLock));
    if(!(ocrGuidIsUninitialized(event->data))) {
        ocrFatGuid_t dataGuid = {.guid = event->data, .metaDataPtr = NULL};
        hal_unlock(&(event->base.waitersLock));
#ifdef REG_ASYNC_SGL
        regNode_t node = {.guid = waiter.guid, .slot = slot, .mode = mode};
#else
        regNode_t node = {.guid = waiter.guid, .slot = slot};
#endif
        // We send a message saying that we satisfy whatever tried to wait on us
        RESULT_PROPAGATE(commonSatisfyRegNode(pd, &msg, base->guid, dataGuid, currentEdt, &node));
        // Here it is still safe to use the base pointer because the satisfy
        // call cannot trigger the destruction of the event. For counted-events
        // the runtime takes care of it
        hal_lock(&(event->base.waitersLock));
        ocrEventHcCounted_t * devt = (ocrEventHcCounted_t *) event;
        // Account for this registration. When it reaches zero the event
        // can be deallocated since it is already satisfied and this call
        // was the last ocrAddDependence.
        ASSERT(devt->nbDeps > 0);
        devt->nbDeps--;
        u64 nbDeps = devt->nbDeps;
        hal_unlock(&(event->base.waitersLock));
        // Check if we'll need to destroy the event
        if (nbDeps == 0) {
            // Can move that after satisfy to reduce CPL
            destructEventHc(base);
        }
        return 0; //Require registerSignaler invocation
    }

    // Lock is released by commonEnqueueWaiter
#ifdef REG_ASYNC_SGL
    return commonEnqueueWaiter(pd, base, waiter, slot, mode, currentEdt, &msg);
#else
    return commonEnqueueWaiter(pd, base, waiter, slot, currentEdt, &msg);
#endif
}
#endif


// In this call, we do not contend with satisfy
u8 unregisterWaiterEventHc(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepRem) {
    // Always search for the waiter because we don't know if it registered or not so
    // ignore isDepRem
    ocrEventHc_t *event = (ocrEventHc_t*)base;


    DPRINTF(DEBUG_LVL_INFO, "UnRegister waiter %s: "GUIDF" with waiter "GUIDF" on slot %"PRId32"\n",
            eventTypeToString(base), GUIDA(base->guid), GUIDA(waiter.guid), slot);

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    regNode_t *waiters = NULL;
    u32 i;

    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t curEdt = {.guid = curTask!=NULL?curTask->guid:NULL_GUID, .metaDataPtr = curTask};

    // Acquire the DB that contains the waiters
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
    msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = event->waitersDb;
    PD_MSG_FIELD_IO(edt) = curEdt;
    PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
    PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
    PD_MSG_FIELD_IO(properties) = DB_MODE_RW | DB_PROP_RT_ACQUIRE;
    //Should be a local DB
    u8 res __attribute__((unused)) = pd->fcts.processMessage(pd, &msg, true);
    ASSERT(!res); // Possible corruption of waitersDb

    waiters = (regNode_t*)PD_MSG_FIELD_O(ptr);
    //BUG #273
    event->waitersDb = PD_MSG_FIELD_IO(guid);
    ASSERT(waiters);
#undef PD_TYPE
    // We search for the waiter that we need and remove it
    for(i = 0; i < event->waitersCount; ++i) {
        if(ocrGuidIsEq(waiters[i].guid, waiter.guid) && waiters[i].slot == slot) {
            // We will copy all the other ones
            hal_memCopy((void*)&waiters[i], (void*)&waiters[i+1],
                        sizeof(regNode_t)*(event->waitersCount - i - 1), false);
            --event->waitersCount;
            break;
        }
    }

    // We always release waitersDb
#define PD_TYPE PD_MSG_DB_RELEASE
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = event->waitersDb;
    PD_MSG_FIELD_I(edt) = curEdt;
    PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
    PD_MSG_FIELD_I(ptr) = NULL;
    PD_MSG_FIELD_I(size) = 0;
    PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}


// In this call, we can have concurrent satisfy
u8 unregisterWaiterEventHcPersist(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot) {
    ocrEventHcPersist_t *event = (ocrEventHcPersist_t*)base;


    DPRINTF(DEBUG_LVL_INFO, "Unregister waiter %s: "GUIDF" with waiter "GUIDF" on slot %"PRId32"\n",
            eventTypeToString(base), GUIDA(base->guid), GUIDA(waiter.guid), slot);

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    regNode_t *waiters = NULL;
    u32 i;
    u8 toReturn = 0;

    getCurrentEnv(&pd, NULL, &curTask, &msg);
    ocrFatGuid_t curEdt = {.guid = curTask!=NULL?curTask->guid:NULL_GUID, .metaDataPtr = curTask};
    hal_lock(&(event->base.waitersLock));
    if(!(ocrGuidIsUninitialized(event->data))) {
        // We don't really care at this point so we don't do anything
        hal_unlock(&(event->base.waitersLock));
        return 0;
    }

    // Here we need to actually update our waiter list. We still hold the lock
    // Acquire the DB that contains the waiters
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
    msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = event->base.waitersDb;
    PD_MSG_FIELD_IO(edt) = curEdt;
    PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
    PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
    PD_MSG_FIELD_IO(properties) = DB_MODE_RW | DB_PROP_RT_ACQUIRE;
    //Should be a local DB
    if((toReturn = pd->fcts.processMessage(pd, &msg, true))) {
        ASSERT(!toReturn); // Possible corruption of waitersDb
        hal_unlock(&(event->base.waitersLock));
        return toReturn;
    }
    //BUG #273: Guid reading
    waiters = (regNode_t*)PD_MSG_FIELD_O(ptr);
    event->base.waitersDb = PD_MSG_FIELD_IO(guid);
    ASSERT(waiters);
#undef PD_TYPE
    // We search for the waiter that we need and remove it
    for(i = 0; i < event->base.waitersCount; ++i) {
        if(ocrGuidIsEq(waiters[i].guid, waiter.guid) && waiters[i].slot == slot) {
            // We will copy all the other ones
            hal_memCopy((void*)&waiters[i], (void*)&waiters[i+1],
                        sizeof(regNode_t)*(event->base.waitersCount - i - 1), false);
            --event->base.waitersCount;
            break;
        }
    }

    // We can release the lock now
    hal_unlock(&(event->base.waitersLock));

    // We always release waitersDb
#define PD_TYPE PD_MSG_DB_RELEASE
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = event->base.waitersDb;
    PD_MSG_FIELD_I(edt) = curEdt;
    PD_MSG_FIELD_I(srcLoc) = pd->myLocation;
    PD_MSG_FIELD_I(ptr) = NULL;
    PD_MSG_FIELD_I(size) = 0;
    PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

u8 setHintEventHc(ocrEvent_t* self, ocrHint_t *hint) {
    ocrEventHc_t *derived = (ocrEventHc_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_SET(hint, rHint, OCR_HINT_COUNT_EVT_HC, ocrHintPropEventHc, OCR_HINT_EVT_PROP_START);
    return 0;
}

u8 getHintEventHc(ocrEvent_t* self, ocrHint_t *hint) {
    ocrEventHc_t *derived = (ocrEventHc_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_GET(hint, rHint, OCR_HINT_COUNT_EVT_HC, ocrHintPropEventHc, OCR_HINT_EVT_PROP_START);
    return 0;
}

ocrRuntimeHint_t* getRuntimeHintEventHc(ocrEvent_t* self) {
    ocrEventHc_t *derived = (ocrEventHc_t*)self;
    return &(derived->hint);
}


/******************************************************/
/* OCR-HC Events Factory                              */
/******************************************************/

ocrGuidKind eventTypeToGuidKind(ocrEventTypes_t eventType) {
    switch(eventType) {
        case OCR_EVENT_ONCE_T:
            return OCR_GUID_EVENT_ONCE;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
        case OCR_EVENT_COUNTED_T:
            return OCR_GUID_EVENT_COUNTED;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
        case OCR_EVENT_CHANNEL_T:
            return OCR_GUID_EVENT_CHANNEL;
#endif
        case OCR_EVENT_IDEM_T:
            return OCR_GUID_EVENT_IDEM;
        case OCR_EVENT_STICKY_T:
            return OCR_GUID_EVENT_STICKY;
        case OCR_EVENT_LATCH_T:
            return OCR_GUID_EVENT_LATCH;
        default:
            ASSERT(false && "Unknown type of event");
        return OCR_GUID_NONE;
    }
}

static ocrEventTypes_t guidKindToEventType(ocrGuidKind kind) {
    switch(kind) {
        case OCR_GUID_EVENT_ONCE:
            return OCR_EVENT_ONCE_T;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
        case OCR_GUID_EVENT_COUNTED:
            return OCR_EVENT_COUNTED_T;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
        case OCR_GUID_EVENT_CHANNEL:
            return OCR_EVENT_CHANNEL_T;
#endif
        case OCR_GUID_EVENT_IDEM:
            return OCR_EVENT_IDEM_T;
        case OCR_GUID_EVENT_STICKY:
            return OCR_EVENT_STICKY_T;
        case OCR_GUID_EVENT_LATCH:
            return OCR_EVENT_LATCH_T;
        default:
            ASSERT(false && "Unknown kind of event");
        return OCR_EVENT_T_MAX;
    }
}

void destructEventFactoryHc(ocrObjectFactory_t * factory) {
    runtimeChunkFree((u64)((ocrEventFactory_t*)factory)->hintPropMap, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

// Internal utility function to send a 'push' message to update a remote copy.
//
// - The content of the message is implementation specific. It is dependent on how the
//   implementation decides to maintain coherence across multiple distributed copies of metadata.
// - Only supports M_SAT messages
//BUG #989: MT opportunity in certain circumstances. Check comments in deserializeEventFactoryHc
static void mdPushHcDist(ocrGuid_t evtGuid, ocrLocation_t loc, ocrGuid_t dbGuid, u32 mode, u32 factoryId) {
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    msg.destLocation = loc;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg.type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid) = evtGuid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/ //TODO-MD-OP not clearly defined yet
    PD_MSG_FIELD_I(mode) = mode;
    PD_MSG_FIELD_I(factoryId) = factoryId;
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    DPRINTF (DBG_HCEVT_LOG, "event-md: push "GUIDF" in mode=%d\n", GUIDA(evtGuid), mode);
    PD_MSG_FIELD_I(sizePayload) = 0;
    ASSERT((ocrPolicyMsgGetMsgBaseSize(&msg, true) + sizeof(ocrLocation_t) + sizeof(ocrGuid_t)) < sizeof(ocrPolicyMsg_t));
    // Always specify where the push comes from
    // TODO: This is redundant with the message header but the header doesn't make it all
    // the way to the recipient OCR object. Would that change if we collapse object's
    // functions into a big processMessage ?
    // Location is enough for M_REG
    PD_MSG_FIELD_I(sizePayload) = sizeof(ocrLocation_t);
    // Serialization
    ASSERT((mode == M_REG) || (mode == M_SAT) || (mode == M_DEL));
    char * ptr = &(PD_MSG_FIELD_I(payload));
    WR_PAYLOAD_DATA(ptr, ocrLocation_t, pd->myLocation);
    // For M_SAT, add the guid the event is satisfied with
    if (mode == M_SAT) {
        SET_PAYLOAD_DATA(ptr, M_SAT, ocrGuid_t, guid, dbGuid);
        // Check alignment issues
        ASSERT(GET_PAYLOAD_DATA(ptr, M_SAT, ocrLocation_t, location) == pd->myLocation);
        PD_MSG_FIELD_I(sizePayload) += sizeof(ocrGuid_t);
    }
    pd->fcts.processMessage(pd, &msg, true);
#undef PD_MSG
#undef PD_TYPE
}

// Only used in no-forge mode
static void mdPullHcDist(ocrGuid_t guid, u32 mode, u32 factoryId) {
    // This implementation only pulls in clone mode
    ASSERT(mode == M_CLONE);
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    // Since we just pull to clone the destination of this message is
    // always the location that owns the GUID.
    ocrLocation_t destLocation;
    u8 returnValue = pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], guid, &destLocation);
    ASSERT(!returnValue);
    msg.destLocation = destLocation;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg.type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid) = guid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PULL;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
    PD_MSG_FIELD_I(mode) = mode;
    PD_MSG_FIELD_I(factoryId) = factoryId;
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    DPRINTF (DBG_HCEVT_LOG, "event-md: pull "GUIDF" in mode=%d\n", GUIDA(guid), mode);
    PD_MSG_FIELD_I(sizePayload) = 0;
    ASSERT((ocrPolicyMsgGetMsgBaseSize(&msg, true) + sizeof(ocrLocation_t)) < sizeof(ocrPolicyMsg_t));
    // Always specify where the push comes from
    // TODO: This is redundant with the message header but the header doesn't make it all
    // the way to the recipient OCR object. Would that change if we collapse object's
    // functions into a big processMessage ?
    PD_MSG_FIELD_I(sizePayload) = sizeof(ocrLocation_t);
    char * ptr = &(PD_MSG_FIELD_I(payload));
    WR_PAYLOAD_DATA(ptr, ocrLocation_t, pd->myLocation);
    pd->fcts.processMessage(pd, &msg, true);
#undef PD_MSG
#undef PD_TYPE
}

/******************************************************/
/* OCR-HC Events Master/Slave                         */
/******************************************************/


static u8 initNewEventHc(ocrEventHc_t * event, ocrEventTypes_t eventType, ocrGuid_t data, ocrEventFactory_t * factory, u32 sizeOfGuid, ocrParamList_t *perInstance) {
    ocrEvent_t * base = (ocrEvent_t*) event;
    base->kind = eventType;
    u32 factoryId = factory->factoryId;
    base->base.fctId = factoryId;
    base->fctId = factoryId;

    // Set-up HC specific structures
    event->waitersCount = 0;
    event->waitersMax = HCEVT_WAITER_STATIC_COUNT;
    event->waitersLock = INIT_LOCK;

    int jj = 0;
    while (jj < HCEVT_WAITER_STATIC_COUNT) {
        event->waiters[jj].guid = NULL_GUID;
        event->waiters[jj].slot = 0;
        event->waiters[jj].mode = -1;
        jj++;
    }

    if(eventType == OCR_EVENT_LATCH_T) {
        // Initialize the counter
        if (perInstance != NULL) {
#ifdef ENABLE_EXTENSION_PARAMS_EVT
            // Expecting ocrEventParams_t as the paramlist
            ocrEventParams_t * params = (ocrEventParams_t *) perInstance;
            ((ocrEventHcLatch_t*)event)->counter = params->EVENT_LATCH.counter;
#endif
        } else {
            ((ocrEventHcLatch_t*)event)->counter = 0;
        }
    }
    event->mdClass.peers = NULL;
    event->mdClass.satFromLoc = INVALID_LOCATION;
    event->mdClass.delFromLoc = INVALID_LOCATION;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    if(eventType == OCR_EVENT_IDEM_T || eventType == OCR_EVENT_STICKY_T || eventType == OCR_EVENT_COUNTED_T) {
#else
    if(eventType == OCR_EVENT_IDEM_T || eventType == OCR_EVENT_STICKY_T) {
#endif
        ((ocrEventHcPersist_t*)event)->data = data;
        if (!ocrGuidIsUninitialized(data)) {
            // For master-slave impl, we did a clone and the event was already satisfied
            event->waitersCount = STATE_CHECKED_OUT;
        }
    }

#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    if(eventType == OCR_EVENT_CHANNEL_T) {
        // Check current extension implementation restrictions
        ocrEventHcChannel_t * devt = ((ocrEventHcChannel_t*)event);
        // Expecting ocrEventParams_t as the paramlist
        ocrEventParams_t * params = (ocrEventParams_t *) perInstance;
        ASSERT((params->EVENT_CHANNEL.nbSat == 1) && "Channel-event limitation nbSat must be set to 1");
        ASSERT((params->EVENT_CHANNEL.nbDeps == 1) && "Channel-event limitation nbDeps must be set to 1");
        ASSERT((params->EVENT_CHANNEL.maxGen != 0) && "Channel-event maxGen=0 invalid value");
        u32 maxGen = (params->EVENT_CHANNEL.maxGen == EVENT_CHANNEL_UNBOUNDED) ? 1 : params->EVENT_CHANNEL.maxGen;
        devt->maxGen = params->EVENT_CHANNEL.maxGen;
        devt->nbSat = params->EVENT_CHANNEL.nbSat;
        devt->satBufSz = maxGen * devt->nbSat;
        devt->nbDeps = params->EVENT_CHANNEL.nbDeps;
	devt->waitBufSz = maxGen * devt->nbDeps;
        if (devt->maxGen == EVENT_CHANNEL_UNBOUNDED) {
            ocrPolicyDomain_t * pd;
	    getCurrentEnv(&pd, NULL, NULL, NULL);
            // Setup backing data-structure pointers
            devt->satBuffer = (ocrGuid_t *) pd->fcts.pdMalloc(pd, sizeof(ocrGuid_t) * devt->satBufSz);
            devt->waiters = (regNode_t *) pd->fcts.pdMalloc(pd, sizeof(regNode_t) * devt->waitBufSz);
        } else {
            // Setup backing data-structure pointers
            u32 baseSize = sizeof(ocrEventHcChannel_t);
            u32 sizeSat = (sizeof(ocrGuid_t) * devt->satBufSz);
            devt->satBuffer = (ocrGuid_t *)((u64)base + baseSize);
            devt->waiters = (regNode_t *)((u64)base + baseSize + sizeSat);
        }
        devt->headSat = 0;
        devt->tailSat = 0;
        devt->headWaiter = 0;
        devt->tailWaiter = 0;
        u32 i;
        for (i=0; i<devt->satBufSz; i++) {
            devt->satBuffer[i] = UNINITIALIZED_GUID;
        }
#ifdef OCR_ASSERT
        regNode_t regnode;
        regnode.guid = NULL_GUID;
        regnode.slot = 0;
        regnode.mode = -1;
        // This is not really necessary outside of debug mode
        for (i=0; i<devt->waitBufSz; i++) {
            devt->waiters[i] = regnode;
        }
#endif
    }
#endif

    u32 hintc = OCR_HINT_COUNT_EVT_HC;
    if (hintc == 0) {
        event->hint.hintMask = 0;
        event->hint.hintVal = NULL;
    } else {
        OCR_RUNTIME_HINT_MASK_INIT(event->hint.hintMask, OCR_HINT_EVT_T, factoryId);
        event->hint.hintVal = (u64*)((u64)base + sizeOfGuid);
    }

    // Initialize GUIDs for the waiters data-blocks
    event->waitersDb.guid = UNINITIALIZED_GUID;
    event->waitersDb.metaDataPtr = NULL;

#ifdef ENABLE_EXTENSION_COUNTED_EVT
    if(eventType == OCR_EVENT_COUNTED_T) {
        // Initialize the counter for dependencies tracking
        ocrEventParams_t * params = (ocrEventParams_t *) perInstance;
        if ((params != NULL) && (((ocrEventParams_t *) perInstance)->EVENT_COUNTED.nbDeps != 0)) {
            ((ocrEventHcCounted_t*)event)->nbDeps = (perInstance == NULL) ? 0 : params->EVENT_COUNTED.nbDeps;
        } else {
            DPRINTF(DBG_HCEVT_ERR, "error: Illegal nbDeps value (zero) for OCR_EVENT_COUNTED_T 0x"GUIDF"\n", GUIDA(base->guid));
            factory->fcts[OCR_EVENT_COUNTED_T].destruct(base);
            ASSERT(false);
            return OCR_EINVAL; // what ?
        }
    }
#endif
    return 0;
}

static u8 allocateNewEventHc(ocrGuidKind guidKind, ocrFatGuid_t * resultGuid, u32 * sizeofMd, u32 properties, ocrParamList_t *perInstance) {
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    // Create the event itself by getting a GUID
    *sizeofMd = sizeof(ocrEventHc_t);
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    if(guidKind == OCR_GUID_EVENT_COUNTED) {
        *sizeofMd = sizeof(ocrEventHcCounted_t);
    }
#endif
    if(guidKind == OCR_GUID_EVENT_LATCH) {
        *sizeofMd = sizeof(ocrEventHcLatch_t);
    }
    if((guidKind == OCR_GUID_EVENT_IDEM) || (guidKind == OCR_GUID_EVENT_STICKY)) {
        *sizeofMd = sizeof(ocrEventHcPersist_t);
    }
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    if(guidKind == OCR_GUID_EVENT_CHANNEL) {
#ifndef ENABLE_EXTENSION_PARAMS_EVT
        ASSERT(false && "ENABLE_EXTENSION_PARAMS_EVT must be defined to use Channel-events");
#endif
        ASSERT((perInstance != NULL) && "error: No parameters specified at Channel-event creation");
        // Expecting ocrEventParams_t as the paramlist
        ocrEventParams_t * params = (ocrEventParams_t *) perInstance;
        // Allocate extra space to store backing data-structures that are parameter-dependent
        u32 xtraSpace = 0;
        if (params->EVENT_CHANNEL.maxGen != EVENT_CHANNEL_UNBOUNDED) {
            // Allocate extra space to store backing data-structures that are parameter-dependent
            u32 sizeSat = (sizeof(ocrGuid_t) * params->EVENT_CHANNEL.nbSat * params->EVENT_CHANNEL.maxGen);
            u32 sizeWaiters = (sizeof(regNode_t) * params->EVENT_CHANNEL.nbDeps * params->EVENT_CHANNEL.maxGen);
            xtraSpace = (sizeSat + sizeWaiters);
        }
        *sizeofMd = sizeof(ocrEventHcChannel_t) + xtraSpace;
    }
#endif
    u32 hintc = OCR_HINT_COUNT_EVT_HC;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = *resultGuid;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD_I(size) = (*sizeofMd) + hintc*sizeof(u64);
    PD_MSG_FIELD_I(kind) = guidKind;
    PD_MSG_FIELD_I(targetLoc) = pd->myLocation;
    PD_MSG_FIELD_I(properties) = properties;
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
    u8 returnValue = PD_MSG_FIELD_O(returnDetail);
    if (returnValue && (returnValue != OCR_EGUIDEXISTS)) {
        ASSERT(false);
        return returnValue;
    }
    // Set-up base structures
    resultGuid->guid = PD_MSG_FIELD_IO(guid.guid);
    resultGuid->metaDataPtr = PD_MSG_FIELD_IO(guid.metaDataPtr);
#ifdef ENABLE_RESILIENCY
    ocrEvent_t *evt = (ocrEvent_t*)resultGuid->metaDataPtr;
    evt->base.kind = guidKind;
    evt->base.size = (*sizeofMd) + hintc*sizeof(u64);
#endif
#undef PD_MSG
#undef PD_TYPE
    return returnValue;
}

// Created a distributed event that has additional metadata
// REQ: fguid must be a valid GUID so that the event has either already been allocated
//      (hence we have a guid) or it's a labeled guid and the guid is well-formed.
// NOTE: This was originally intended to be used for forging event but it also works
//       to allocate an event from the M_CLONE path.
static u8 newEventHcDist(ocrFatGuid_t * fguid, ocrGuid_t data, ocrEventFactory_t * factory) {
    ASSERT(!ocrGuidIsNull(fguid->guid));
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrGuidKind guidKind;
    u8 returnValue = pd->guidProviders[0]->fcts.getKind(pd->guidProviders[0], fguid->guid, &guidKind);
    ocrEventTypes_t eventType = guidKindToEventType(guidKind);
    ASSERT(!returnValue);
    ocrLocation_t guidLoc;
    returnValue = pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], fguid->guid, &guidLoc);
    ASSERT(!returnValue);
    u32 sizeOfMd;
    returnValue = allocateNewEventHc(guidKind, fguid, &sizeOfMd, GUID_PROP_IS_LABELED/*prop*/, NULL);
    ocrEventHc_t *event = (ocrEventHc_t*) fguid->metaDataPtr;
    u8 ret = initNewEventHc(event, eventType, data, factory, sizeOfMd, NULL);
    if (ret) { return ret; }
    if (pd->myLocation != guidLoc) {
        event->mdClass.peers = NULL;
    } else { // automatically register the master location as a peer
        locNode_t * locNode = (locNode_t *) pd->fcts.pdMalloc(pd, sizeof(locNode_t));
        locNode->loc = guidLoc;
        locNode->next = NULL;
        event->mdClass.peers = locNode;
    }
    // Do this at the very end; it indicates that the object of the GUID is actually valid
    hal_fence(); // Make sure sure this really happens last
    ((ocrEvent_t*) event)->guid = fguid->guid;

    DPRINTF(DEBUG_LVL_INFO, "Create %s: "GUIDF"\n", eventTypeToString((ocrEvent_t *)event), GUIDA(fguid->guid));
#ifdef OCR_ENABLE_STATISTICS
    statsEVT_CREATE(getCurrentPD(), getCurrentEDT(), NULL, fguid->guid, ((ocrEvent_t*) event));
#endif
    ASSERT(!returnValue);
    return returnValue;
}

u8 cloneEventFactoryHc(ocrObjectFactory_t * pfactory, ocrGuid_t guid, ocrObject_t ** mdPtr) {
    ocrEventFactory_t * factory = (ocrEventFactory_t *) pfactory;
    if (ENABLE_EVENT_MDC_FORGE) { // Allow forging
        ocrPolicyDomain_t * pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        ocrGuidKind guidKind;
        RESULT_ASSERT(pd->guidProviders[0]->fcts.getKind(pd->guidProviders[0], guid, &guidKind), ==, 0 );
        if (MDC_SUPPORT_EVT(guidKind)) { // And is for a supported GUID kind
            // Create a new instance
            ocrFatGuid_t fguid = {.guid = guid, .metaDataPtr = NULL};
            newEventHcDist(&fguid, UNINITIALIZED_GUID, factory);
            *mdPtr = fguid.metaDataPtr;
            ocrPolicyDomain_t * pd;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            ocrLocation_t destLocation;
            pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], guid, &destLocation);
            // Generate a registration message for the owner of that guid
            mdPushHcDist(guid, destLocation, NULL_GUID, M_REG, factory->factoryId);
            return 0;
        }
    }
    // Otherwise fall-through to regular cloning
    *mdPtr = NULL;
    mdPullHcDist(guid, M_CLONE, factory->factoryId);
    return OCR_EPEND;
}

u8 serializeEventFactoryHc(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t * src, u64 * mode, ocrLocation_t destLocation, void ** destBuffer, u64 * destSize) {
    ASSERT(destBuffer != NULL);
    ASSERT(!ocrGuidIsNull(guid));
    ASSERT(*destSize != 0);
#if ENABLE_EVENT_MDC_FORGE
    ASSERT(false); // Never do a pull when we forge events
#else
    // More of a proof of concept since we can easily forge events in this implementation
    ocrEvent_t * evt = (ocrEvent_t *) src;
    *mode = 0; // clear bits
    // Specialized implementation serialization:
    // - Ignore most of the field and initialize them on deserialization
    // - Must handle concurrency with competing operations being invoked on the event
    //   - By design, there should not be a concurrent destruct
    ocrEventHc_t * devt = (ocrEventHc_t *) src;
    switch(evt->kind) {
        case OCR_EVENT_STICKY_T:
        case OCR_EVENT_IDEM_T:
        {
            // - There's not much information to serialize beside the
            //   GUID the event is currently satisfied with (or not).
            // - We always register the peer so that we can reclaim
            //   all them on destruction
            ocrPolicyDomain_t *pd = NULL;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            ASSERT(*destSize >= sizeof(ocrGuid_t));
            // Note: this is concurrent with the event being satisfied so it's best effort.
            locNode_t * locNode = (locNode_t *) pd->fcts.pdMalloc(pd, sizeof(locNode_t));
            hal_lock(&(devt->waitersLock));
            ocrGuid_t data = ((ocrEventHcPersist_t *)evt)->data;
            locNode->loc = destLocation;
            locNode->next = devt->mdClass.peers;
            // Enqueuing at the head relies on the fact the slave location doesn't issue
            // multiple simultaneous pull requests for the same OCR object. Otherwise we
            // would have to enforce unicity when enqueuing.
            devt->mdClass.peers = locNode;
            hal_unlock(&(devt->waitersLock));
            // Just send the current GUID the event is satisfied (or not) with.
            SET_PAYLOAD_DATA((*destBuffer), M_CLONE, ocrGuid_t, guid, data);
        break;
        }
    default:
        ASSERT(false && "Metadata-cloning not supported for this event type");
    }
    return 0;
#endif
}

u8 newEventHc(ocrEventFactory_t * factory, ocrFatGuid_t *fguid,
              ocrEventTypes_t eventType, u32 properties,
              ocrParamList_t *perInstance) {
    u32 sizeOfMd;
    ocrGuidKind guidKind = eventTypeToGuidKind(eventType);
    u8 returnValue = allocateNewEventHc(guidKind, fguid, &sizeOfMd, properties, perInstance);
    if (returnValue) { ASSERT(returnValue == OCR_EGUIDEXISTS); return returnValue; }

    ocrEventHc_t *event = (ocrEventHc_t*) fguid->metaDataPtr;
    returnValue = initNewEventHc(event, eventType, UNINITIALIZED_GUID, factory, sizeOfMd, perInstance);
    if (returnValue) { return returnValue; }

    // Do this at the very end; it indicates that the object
    // of the GUID is actually valid
    hal_fence(); // Make sure sure this really happens last
    ((ocrEvent_t*) event)->guid = fguid->guid;

    DPRINTF(DEBUG_LVL_INFO, "Create %s: "GUIDF"\n", eventTypeToString(((ocrEvent_t*) event)), GUIDA(fguid->guid));
#ifdef OCR_ENABLE_STATISTICS
    statsEVT_CREATE(getCurrentPD(), getCurrentEDT(), NULL, fguid->guid, ((ocrEvent_t*) event));
#endif
    ASSERT(!returnValue);
    return returnValue;
}

u8 deserializeEventFactoryHc(ocrObjectFactory_t * pfactory, ocrGuid_t evtGuid, ocrObject_t ** dest, u64 mode, void * srcBuffer, u64 srcSize) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrGuidKind guidKind;
    u8 ret = pd->guidProviders[0]->fcts.getKind(pd->guidProviders[0], evtGuid, &guidKind);
    ASSERT(!ret);
    // TODO this code needs to be generalized for other types of events
    ASSERT((guidKindToEventType(guidKind) == OCR_EVENT_STICKY_T) || (guidKindToEventType(guidKind) == OCR_EVENT_IDEM_T))
    ocrEventFactory_t * factory = (ocrEventFactory_t *) pfactory;
    switch(mode) {
        case M_CLONE: {
            // The payload should carry the GUID for the data the event is satisfied with if any.
            // We create the event anyhow: would only make sense for persistent events that
            // may still have ocrAddDependence coming in and would benefit from having the MD local.
            DPRINTF(DBG_HCEVT_LOG, "event-md: deserialize M_CLONE "GUIDF"\n", GUIDA(evtGuid));
            ocrGuid_t dataGuid = GET_PAYLOAD_DATA(srcBuffer, M_CLONE, ocrGuid_t, guid);
            ocrFatGuid_t fguid;
            fguid.guid = evtGuid;
            fguid.metaDataPtr = NULL;
            newEventHcDist(&fguid, dataGuid, factory);
            ASSERT(fguid.metaDataPtr != NULL);
            *dest = fguid.metaDataPtr;
        break;
        }
        // Register another peer to our peerlist (different from an ocrAddDependence)
        // TODO: We can probably have few slot pre-allocated and extend that dynamically
        // passed the fixed size like we do for events waiters => Actually might be a nice
        // typedef struct to add.
        case M_REG: {
            DPRINTF(DBG_HCEVT_LOG, "event-md: deserialize M_REG "GUIDF"\n", GUIDA(evtGuid));
            ocrEventHc_t * devt = (ocrEventHc_t *) (*dest);
            locNode_t * locNode = (locNode_t *) pd->fcts.pdMalloc(pd, sizeof(locNode_t));
            ocrLocation_t loc = GET_PAYLOAD_DATA(srcBuffer, M_REG, ocrLocation_t, location);
            locNode->loc = loc;
            hal_lock(&(devt->waitersLock));
            //RACE-1: Check inside the lock to avoid race with satisfier. Allows
            //to determine this context is responsible for sending the M_SAT.
            bool doSatisfy = (devt->waitersCount == STATE_CHECKED_IN);
            // Registering while the event is being destroyed: Something is wrong in user code or runtime code
            ASSERT(devt->waitersCount  != STATE_CHECKED_OUT);
            // Whether the event is already satisfied or not, we need
            // to register so that the peer is notified on 'destruct'
            locNode->next = devt->mdClass.peers;
            devt->mdClass.peers = locNode;
            hal_unlock(&(devt->waitersLock));
            if (doSatisfy) {
                // The event is already satisfied, need to notify that back.
                mdPushHcDist(evtGuid, loc, ((ocrEventHcPersist_t *)devt)->data, M_SAT, ((ocrEvent_t*)devt)->fctId);
            }
        break;
        }
        // Processing a satisfy notification from a peer
        case M_SAT: {
            DPRINTF(DBG_HCEVT_LOG, "event-md: deserialize M_SAT "GUIDF"\n", GUIDA(evtGuid));
            ocrEvent_t * base = (ocrEvent_t *) (*dest);
            ((ocrEventHc_t *) base)->mdClass.satFromLoc = GET_PAYLOAD_DATA(srcBuffer, M_SAT, ocrLocation_t, location);
            ocrFatGuid_t fdata;
            fdata.guid = GET_PAYLOAD_DATA(srcBuffer, M_SAT, ocrGuid_t, guid);
            //TODO need the slot for latch events too
            u32 slot = 0;
            factory->fcts[guidKindToEventType(guidKind)].satisfy(base, fdata, slot);
        break;
        }
        case M_DEL: {
            DPRINTF(DBG_HCEVT_LOG, "event-md: deserialize M_DEL "GUIDF"\n", GUIDA(evtGuid));
            ocrEvent_t * base = (ocrEvent_t *) (*dest);
            ((ocrEventHc_t *) base)->mdClass.delFromLoc = GET_PAYLOAD_DATA(srcBuffer, M_DEL, ocrLocation_t, location);
            factory->fcts[guidKindToEventType(guidKind)].destruct(base);
        break;
        }
    }
    return 0;
}

//
// Simple Channel
//

#ifdef ENABLE_EXTENSION_CHANNEL_EVT

#ifndef DEBUG_LVL_CHANNEL
#define DEBUG_LVL_CHANNEL DEBUG_LVL_INFO
#endif

static void pushDependence(ocrEventHcChannel_t * devt, regNode_t * node) {
    //TODO rollover u32/u64
    // tail - head cannot go be more that the bound
    ASSERT(devt->tailWaiter < (devt->headWaiter + devt->waitBufSz));
    u32 idx = devt->tailWaiter % devt->waitBufSz;
    devt->tailWaiter++;
    devt->waiters[idx] = *node;
}

static u8 popDependence(ocrEventHcChannel_t * devt, regNode_t * node) {
    if (devt->headWaiter == devt->tailWaiter) {
        return 1;
    } else {
        u32 idx = devt->headWaiter % devt->waitBufSz;
        devt->headWaiter++;
        *node = devt->waiters[idx];
        return 0;
    }
}

static void pushSatisfy(ocrEventHcChannel_t * devt, ocrGuid_t data) {
    ASSERT(devt->tailSat < (devt->headSat + devt->satBufSz));
    u32 idx = devt->tailSat % devt->satBufSz;
    devt->tailSat++;
    devt->satBuffer[idx] = data;
}

static ocrGuid_t popSatisfy(ocrEventHcChannel_t * devt) {
    if (devt->headSat == devt->tailSat) {
        return UNINITIALIZED_GUID;
    } else {
        u32 idx = devt->headSat % devt->satBufSz;
        devt->headSat++;
        ocrGuid_t res = devt->satBuffer[idx];
        ASSERT(!ocrGuidIsUninitialized(res));
        return res;
    }
}

#define CHANNEL_BUFFER_RESIZE(cntFct, bufName, bufSz, headName, tailName, type) \
    s32 nbElems = cntFct(devt); \
    u32 newMaxNbElems = nbElems * 2; \
    type * oldData = devt->bufName; \
    devt->bufName = (type *) pd->fcts.pdMalloc(pd, sizeof(type)*newMaxNbElems); \
    s32 headOffset = devt->headName%nbElems; \
    s32 tailOffset = devt->tailName%nbElems; \
    if ((headOffset > tailOffset) || ((headOffset == tailOffset) && (headOffset != 0))) { \
        s32 nbElemRight = (devt->bufSz - headOffset);  \
        hal_memCopy(devt->bufName, &oldData[headOffset], sizeof(type)*nbElemRight, false);  \
        hal_memCopy(&(devt->bufName[nbElemRight]), oldData, sizeof(type)*tailOffset, false);  \
    } else {  \
        hal_memCopy(devt->bufName, &oldData[headOffset], sizeof(type)*nbElems, false);  \
    }  \
    devt->headName = 0;  \
    devt->tailName = nbElems;  \
    pd->fcts.pdFree(pd, oldData);  \
    devt->bufSz = newMaxNbElems;

static bool isChannelSatisfyFull(ocrEventHcChannel_t * devt) {
    return (channelSatisfyCount(devt)  == devt->satBufSz);
}

static bool isChannelWaiterFull(ocrEventHcChannel_t * devt) {
    return (channelWaiterCount(devt) == devt->waitBufSz);
}

static void channelWaiterResize(ocrEventHcChannel_t * devt) {
    ocrPolicyDomain_t * pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    CHANNEL_BUFFER_RESIZE(channelWaiterCount, waiters, waitBufSz, headWaiter, tailWaiter, regNode_t);
}

static void channelSatisfyResize(ocrEventHcChannel_t * devt) {
    ocrPolicyDomain_t * pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    CHANNEL_BUFFER_RESIZE(channelSatisfyCount, satBuffer, satBufSz, headSat, tailSat, ocrGuid_t);
}

#ifdef REG_ASYNC_SGL
u8 registerWaiterEventHcChannel(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd, ocrDbAccessMode_t mode) {
#else
u8 registerWaiterEventHcChannel(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepAdd) {
#endif
    ocrEventHc_t * evt = ((ocrEventHc_t*)base);
    ocrEventHcChannel_t * devt = ((ocrEventHcChannel_t*)base);
    hal_lock(&evt->waitersLock);
    ocrGuid_t data = popSatisfy(devt);
    regNode_t regnode;
    regnode.guid = waiter.guid;
    regnode.slot = slot;
#ifdef REG_ASYNC_SGL
    regnode.mode = mode;
#endif
    if (!ocrGuidIsUninitialized(data)) {
        DPRINTF(DEBUG_LVL_CHANNEL, "registerWaiterEventHcChannel "GUIDF" push dep and deque satisfy\n",
                GUIDA(base->guid));
        hal_unlock(&evt->waitersLock);
        // We can fire the event
        ocrPolicyDomain_t *pd = NULL;
        ocrTask_t *curTask = NULL;
        PD_MSG_STACK(msg);
        getCurrentEnv(&pd, NULL, &curTask, &msg);
        ocrFatGuid_t currentEdt;
        currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
        currentEdt.metaDataPtr = curTask;
        ocrFatGuid_t db;
        db.guid = data;
        db.metaDataPtr = NULL;
        DPRINTF(DEBUG_LVL_CHANNEL, "registerWaiterEventHcChannel satisfy edt with DB="GUIDF"\n",
                GUIDA(data));
        return commonSatisfyRegNode(pd, &msg, base->guid, db, currentEdt, &regnode);
    } else {
        DPRINTF(DEBUG_LVL_CHANNEL, "registerWaiterEventHcChannel "GUIDF" push dependence curSize=%"PRIu32"\n",
                GUIDA(base->guid), channelWaiterCount(devt));
        if ((devt->maxGen == EVENT_CHANNEL_UNBOUNDED) && isChannelWaiterFull(devt)) {
            channelWaiterResize(devt);
        }
        pushDependence(devt, &regnode);
        hal_unlock(&evt->waitersLock);
    }
    return 0;
}

u8 satisfyEventHcChannel(ocrEvent_t *base, ocrFatGuid_t db, u32 slot) {
    ocrEventHc_t * evt = ((ocrEventHc_t*)base);
    ocrEventHcChannel_t * devt = ((ocrEventHcChannel_t*)base);
    hal_lock(&evt->waitersLock);
    regNode_t regnode;
    u8 res = popDependence(devt, &regnode);
    if (res == 0) {
        DPRINTF(DEBUG_LVL_CHANNEL, "satisfyEventHcChannel "GUIDF" satisfy go through\n",
                GUIDA(base->guid));
        hal_unlock(&evt->waitersLock);
        // We can fire the event
        ocrPolicyDomain_t *pd = NULL;
        ocrTask_t *curTask = NULL;
        PD_MSG_STACK(msg);
        getCurrentEnv(&pd, NULL, &curTask, &msg);
        ocrFatGuid_t currentEdt;
        currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
        currentEdt.metaDataPtr = curTask;
        DPRINTF(DEBUG_LVL_CHANNEL, "satisfyEventHcChannel satisfy edt with DB="GUIDF"\n",
                GUIDA(db.guid));
        return commonSatisfyRegNode(pd, &msg, base->guid, db, currentEdt, &regnode);
    } else {
        DPRINTF(DEBUG_LVL_CHANNEL, "satisfyEventHcChannel "GUIDF" satisfy enqueued curSize=%"PRIu32"\n",
                GUIDA(base->guid), channelSatisfyCount(devt));
        if ((devt->maxGen == EVENT_CHANNEL_UNBOUNDED) && isChannelSatisfyFull(devt)) {
            channelSatisfyResize(devt);
        }
        pushSatisfy(devt, db.guid);
        hal_unlock(&evt->waitersLock);
    }
    return 0;
}

u8 unregisterWaiterEventHcChannel(ocrEvent_t *base, ocrFatGuid_t waiter, u32 slot, bool isDepRem) {
    ASSERT(false && "Not supported");
    return 0;
}

#endif /*Channel implementation*/

u8 mdSizeEventFactoryHc(ocrObject_t *dest, u64 mode, u64 * size) {
#if ENABLE_EVENT_MDC_FORGE
#else
    if (mode == M_CLONE) {
        ocrEventTypes_t eventType = ((ocrEvent_t *) dest)->kind;
        *size = 0;
        if((eventType == OCR_EVENT_IDEM_T) || (eventType == OCR_EVENT_STICKY_T)) {
            *size = sizeof(ocrGuid_t); // The 'data' field
        } else if(eventType == OCR_EVENT_LATCH_T) {
            *size = 0;
        } else { // OCR_EVENT_ONCE_T
            *size = 0;
        }
    }
#endif
    return 0;
}

#ifdef ENABLE_RESILIENCY
u8 getSerializationSizeEventHc(ocrEvent_t* self, u64* size) {
    ocrEventHc_t *evtHc = (ocrEventHc_t*)self;
    u32 numPeers = 0;
    locNode_t * curHead;
    for (curHead = evtHc->mdClass.peers; curHead != NULL; curHead = curHead->next)
        numPeers++;

    u64 evtSize = (evtHc->hint.hintVal ? OCR_HINT_COUNT_EVT_HC * sizeof(u64) : 0) +
                  (numPeers * sizeof(locNode_t));
    //NOTE: Waiters DB should be serialized as part of guid provider

    switch(self->kind) {
    case OCR_EVENT_ONCE_T:
        evtSize += sizeof(ocrEventHc_t);
        break;
    case OCR_EVENT_IDEM_T:
    case OCR_EVENT_STICKY_T:
        evtSize += sizeof(ocrEventHcPersist_t);
        break;
    case OCR_EVENT_LATCH_T:
        evtSize += sizeof(ocrEventHcLatch_t);
        break;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    case OCR_EVENT_COUNTED_T:
        evtSize += sizeof(ocrEventHcCounted_t);
        break;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    case OCR_EVENT_CHANNEL_T: {
        ocrEventHcChannel_t * evtHcChannel = (ocrEventHcChannel_t*)self;
        evtSize += sizeof(ocrEventHcChannel_t) +
                   sizeof(ocrGuid_t) * evtHcChannel->satBufSz +
                   sizeof(regNode_t) * evtHcChannel->waitBufSz;
        break;
        }
#endif
    default:
        ASSERT(0);
        break;
    }
    self->base.size = evtSize;
    *size = evtSize;
    return 0;
}

u8 serializeEventHc(ocrEvent_t* self, u8* buffer) {
    ASSERT(buffer);
    u8* bufferHead = buffer;
    ocrEventHc_t *evtHc = (ocrEventHc_t*)self;
    ocrEventHc_t *evtHcBuf = (ocrEventHc_t*)buffer;

    //First serialize the base
    u64 len = 0;
    switch(self->kind) {
    case OCR_EVENT_ONCE_T:
        len = sizeof(ocrEventHc_t);
        break;
    case OCR_EVENT_IDEM_T:
    case OCR_EVENT_STICKY_T:
        len = sizeof(ocrEventHcPersist_t);
        break;
    case OCR_EVENT_LATCH_T:
        len = sizeof(ocrEventHcLatch_t);
        break;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    case OCR_EVENT_COUNTED_T:
        len = sizeof(ocrEventHcCounted_t);
        break;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    case OCR_EVENT_CHANNEL_T:
        len = sizeof(ocrEventHcChannel_t);
        break;
#endif
    default:
        ASSERT(0);
        break;
    }
    ASSERT(len > 0);
    hal_memCopy(buffer, self, len, false);
    buffer += len;

    //Next serialize the HC event extras
    if (evtHc->hint.hintVal && OCR_HINT_COUNT_EVT_HC) {
        evtHcBuf->hint.hintVal = (u64*)buffer;
        len = OCR_HINT_COUNT_EVT_HC * sizeof(u64);
        hal_memCopy(buffer, evtHc->hint.hintVal, len, false);
        buffer += len;
    }

    if (evtHc->mdClass.peers != NULL) {
        evtHcBuf->mdClass.peers = (locNode_t*)buffer;
        locNode_t * curHead;
        len = sizeof(locNode_t);
        for (curHead = evtHc->mdClass.peers; curHead != NULL; curHead = curHead->next) {
            hal_memCopy(buffer, curHead, len, false);
            locNode_t *peerBuf = (locNode_t*)buffer;
            peerBuf->next = (curHead->next != NULL) ? (locNode_t*)(buffer + len) : NULL;
            buffer += len;
        }
    }

    //Finally serialize the derived event extras
    switch(self->kind) {
    case OCR_EVENT_ONCE_T:
    case OCR_EVENT_IDEM_T:
    case OCR_EVENT_STICKY_T:
    case OCR_EVENT_LATCH_T:
        break;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    case OCR_EVENT_COUNTED_T:
        break;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    case OCR_EVENT_CHANNEL_T:
        {
            ocrEventHcChannel_t * evtHcChannel = (ocrEventHcChannel_t*)self;
            ocrEventHcChannel_t * evtHcChannelBuf = (ocrEventHcChannel_t*)evtHcBuf;
            if (evtHcChannel->satBuffer) {
                evtHcChannelBuf->satBuffer = (ocrGuid_t*)buffer;
                len = sizeof(ocrGuid_t) * evtHcChannel->satBufSz;
                hal_memCopy(buffer, evtHcChannel->satBuffer, len, false);
                buffer += len;
            }

            if (evtHcChannel->waiters) {
                evtHcChannelBuf->waiters = (regNode_t*)buffer;
                len = sizeof(regNode_t) * evtHcChannel->waitBufSz;
                hal_memCopy(buffer, evtHcChannel->waiters, len, false);
                buffer += len;
            }
        }
        break;
#endif
    default:
        ASSERT(0);
        break;
    }
    ASSERT((buffer - bufferHead) == self->base.size);
    return 0;
}

//TODO: Need to handle waitersDb ptr
u8 deserializeEventHc(u8* buffer, ocrEvent_t** self) {
    ASSERT(self);
    ASSERT(buffer);
    u8* bufferHead = buffer;
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    ocrEvent_t *evtBuf = (ocrEvent_t*)buffer;
    ocrEventHc_t *evtHcBuf = (ocrEventHc_t*)buffer;
    u64 len = 0;
    switch(evtBuf->kind) {
    case OCR_EVENT_ONCE_T:
        len = sizeof(ocrEventHc_t);
        break;
    case OCR_EVENT_IDEM_T:
    case OCR_EVENT_STICKY_T:
        len = sizeof(ocrEventHcPersist_t);
        break;
    case OCR_EVENT_LATCH_T:
        len = sizeof(ocrEventHcLatch_t);
        break;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    case OCR_EVENT_COUNTED_T:
        len = sizeof(ocrEventHcCounted_t);
        break;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    case OCR_EVENT_CHANNEL_T:
        len = sizeof(ocrEventHcChannel_t);
        break;
#endif
    default:
        ASSERT(0);
        break;
    }
    ASSERT(len > 0);
    u64 extra = (evtHcBuf->hint.hintVal ? OCR_HINT_COUNT_EVT_HC * sizeof(u64) : 0);
    ocrEvent_t *evt = (ocrEvent_t*)pd->fcts.pdMalloc(pd, (len + extra));

    u64 offset = 0;
    hal_memCopy(evt, buffer, len, false);
    buffer += len;
    offset += len;

    ocrEventHc_t *evtHc = (ocrEventHc_t*)evt;
    if (evtHc->hint.hintVal && OCR_HINT_COUNT_EVT_HC) {
        len = OCR_HINT_COUNT_EVT_HC * sizeof(u64);
        evtHc->hint.hintVal = (u64*)((u8*)evtHc + offset);
        hal_memCopy(evtHc->hint.hintVal, buffer, len, false);
        buffer += len;
        offset += len;
    }

    if ((s32)(evtHcBuf->waitersCount) >= 0 && evtHcBuf->mdClass.peers != NULL) {
        len = sizeof(locNode_t);
        locNode_t * prevNode = NULL;
        bool doContinue = true;
        while (doContinue) {
            locNode_t * curNode = (locNode_t*)pd->fcts.pdMalloc(pd, len);
            hal_memCopy(curNode, buffer, len, false);
            curNode->next = NULL;
            if (prevNode == NULL) {
                evtHc->mdClass.peers = curNode;
            } else {
                prevNode->next = curNode;
            }
            prevNode = curNode;
            doContinue = (((locNode_t*)buffer)->next != NULL);
            buffer += len;
        }
    }

    switch(evt->kind) {
    case OCR_EVENT_ONCE_T:
    case OCR_EVENT_IDEM_T:
    case OCR_EVENT_STICKY_T:
    case OCR_EVENT_LATCH_T:
        break;
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    case OCR_EVENT_COUNTED_T:
        break;
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    case OCR_EVENT_CHANNEL_T:
        {
            ocrEventHcChannel_t * evtHcChannel = (ocrEventHcChannel_t*)evtHc;
            if (evtHcChannel->satBuffer) {
                len = sizeof(ocrGuid_t) * evtHcChannel->satBufSz;
                evtHcChannel->satBuffer = (ocrGuid_t*)pd->fcts.pdMalloc(pd, len);
                hal_memCopy(evtHcChannel->satBuffer, buffer, len, false);
                buffer += len;
            }
            if (evtHcChannel->waiters) {
                len = sizeof(regNode_t) * evtHcChannel->waitBufSz;
                evtHcChannel->waiters = (regNode_t*)pd->fcts.pdMalloc(pd, len);
                hal_memCopy(evtHcChannel->waiters, buffer, len, false);
                buffer += len;
            }
        }
        break;
#endif
    default:
        ASSERT(0);
        break;
    }

    *self = evt;
    ASSERT((buffer - bufferHead) == (*self)->base.size);
    return 0;
}

u8 fixupEventHc(ocrEvent_t *base) {
    ocrEventHc_t *hcEvent = (ocrEventHc_t*)base;
    if (hcEvent->waitersDb.metaDataPtr != NULL) {
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        //Fixup the DB pointer
        ASSERT(!ocrGuidIsNull(hcEvent->waitersDb.guid));
        ocrGuid_t dbGuid = hcEvent->waitersDb.guid;
        ocrObject_t * ocrObj = NULL;
        pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dbGuid, (u64*)&ocrObj, NULL, MD_LOCAL, NULL);
        ASSERT(ocrObj != NULL && ocrObj->kind == OCR_GUID_DB);
        ocrDataBlock_t *db = (ocrDataBlock_t*)ocrObj;
        ASSERT(ocrGuidIsEq(dbGuid, db->guid));
        hcEvent->waitersDb.metaDataPtr = db;
    }
    return 0;
}

u8 resetEventHc(ocrEvent_t *base) {
#ifdef ENABLE_CHECKPOINT_VERIFICATION
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomainHc_t *hcPolicy = (ocrPolicyDomainHc_t *)pd;
    if (hcPolicy->checkpointInProgress) {
        pd->fcts.pdFree(pd, base);
    }
#endif
    return 0;
}
#endif

ocrEventFactory_t * newEventFactoryHc(ocrParamList_t *perType, u32 factoryId) {
    ocrObjectFactory_t * bbase = (ocrObjectFactory_t *)
                                  runtimeChunkAlloc(sizeof(ocrEventFactoryHc_t), PERSISTENT_CHUNK);
    bbase->clone = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**), cloneEventFactoryHc);
    bbase->mdSize = FUNC_ADDR(u8 (*)(ocrObject_t * dest, u64, u64*), mdSizeEventFactoryHc);
    bbase->serialize = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t*, u64*, ocrLocation_t, void**, u64*),
        serializeEventFactoryHc);
    bbase->deserialize = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**, u64, void*, u64),
        deserializeEventFactoryHc);

    ocrEventFactory_t *base = (ocrEventFactory_t*) bbase;
    base->instantiate = FUNC_ADDR(u8 (*)(ocrEventFactory_t*, ocrFatGuid_t*,
                                  ocrEventTypes_t, u32, ocrParamList_t*), newEventHc);
    base->base.destruct =  FUNC_ADDR(void (*)(ocrObjectFactory_t*), destructEventFactoryHc);

    // Initialize the base's base
    // For now, we keep it NULL. This is just a placeholder
    base->base.fcts.processEvent = NULL;

    // Initialize the function pointers

    // Setup common functions
    base->commonFcts.setHint = FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrHint_t*), setHintEventHc);
    base->commonFcts.getHint = FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrHint_t*), getHintEventHc);
    base->commonFcts.getRuntimeHint = FUNC_ADDR(ocrRuntimeHint_t* (*)(ocrEvent_t*), getRuntimeHintEventHc);
#ifdef ENABLE_RESILIENCY
    base->commonFcts.getSerializationSize = FUNC_ADDR(u8 (*)(ocrEvent_t*, u64*), getSerializationSizeEventHc);
    base->commonFcts.serialize = FUNC_ADDR(u8 (*)(ocrEvent_t*, u8*), serializeEventHc);
    base->commonFcts.deserialize = FUNC_ADDR(u8 (*)(u8*, ocrEvent_t**), deserializeEventHc);
    base->commonFcts.fixup = FUNC_ADDR(u8 (*)(ocrEvent_t*), fixupEventHc);
    base->commonFcts.reset = FUNC_ADDR(u8 (*)(ocrEvent_t*), resetEventHc);
#endif

    // Setup functions properly
    u32 i;
    for(i = 0; i < (u32)OCR_EVENT_T_MAX; ++i) {
        base->fcts[i].destruct = FUNC_ADDR(u8 (*)(ocrEvent_t*), destructEventHc);
        base->fcts[i].get = FUNC_ADDR(ocrFatGuid_t (*)(ocrEvent_t*), getEventHc);
        base->fcts[i].registerSignaler = FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, ocrDbAccessMode_t, bool),
            registerSignalerHc);
        base->fcts[i].unregisterSignaler = FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), unregisterSignalerHc);
    }
    base->fcts[OCR_EVENT_STICKY_T].destruct =
    base->fcts[OCR_EVENT_IDEM_T].destruct = FUNC_ADDR(u8 (*)(ocrEvent_t*), destructEventHcPersist);

    // Setup satisfy function pointers
    base->fcts[OCR_EVENT_ONCE_T].satisfy =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32), satisfyEventHcOnce);
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    base->fcts[OCR_EVENT_COUNTED_T].satisfy =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32), satisfyEventHcCounted);
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    base->fcts[OCR_EVENT_CHANNEL_T].satisfy =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32), satisfyEventHcChannel);
#endif
    base->fcts[OCR_EVENT_LATCH_T].satisfy =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32), satisfyEventHcLatch);
    base->fcts[OCR_EVENT_IDEM_T].satisfy =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32), satisfyEventHcPersistIdem);
    base->fcts[OCR_EVENT_STICKY_T].satisfy =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32), satisfyEventHcPersistSticky);
#ifdef REG_ASYNC_SGL
    // Setup registration function pointers
    base->fcts[OCR_EVENT_ONCE_T].registerWaiter =
    base->fcts[OCR_EVENT_LATCH_T].registerWaiter =
         FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool, ocrDbAccessMode_t), registerWaiterEventHc);
    base->fcts[OCR_EVENT_IDEM_T].registerWaiter =
    base->fcts[OCR_EVENT_STICKY_T].registerWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool, ocrDbAccessMode_t), registerWaiterEventHcPersist);
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    base->fcts[OCR_EVENT_COUNTED_T].registerWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool, ocrDbAccessMode_t), registerWaiterEventHcCounted);
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    base->fcts[OCR_EVENT_CHANNEL_T].registerWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool, ocrDbAccessMode_t), registerWaiterEventHcChannel);
#endif
#else
    base->fcts[OCR_EVENT_ONCE_T].registerWaiter =
    base->fcts[OCR_EVENT_LATCH_T].registerWaiter =
         FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), registerWaiterEventHc);
    base->fcts[OCR_EVENT_IDEM_T].registerWaiter =
    base->fcts[OCR_EVENT_STICKY_T].registerWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), registerWaiterEventHcPersist);
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    base->fcts[OCR_EVENT_COUNTED_T].registerWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), registerWaiterEventHcCounted);
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    base->fcts[OCR_EVENT_CHANNEL_T].registerWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), registerWaiterEventHcChannel);
#endif
#endif
    base->fcts[OCR_EVENT_ONCE_T].unregisterWaiter =
    base->fcts[OCR_EVENT_LATCH_T].unregisterWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), unregisterWaiterEventHc);
    base->fcts[OCR_EVENT_IDEM_T].unregisterWaiter =
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    base->fcts[OCR_EVENT_COUNTED_T].unregisterWaiter =
#endif
    base->fcts[OCR_EVENT_STICKY_T].unregisterWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), unregisterWaiterEventHcPersist);
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    base->fcts[OCR_EVENT_CHANNEL_T].unregisterWaiter =
        FUNC_ADDR(u8 (*)(ocrEvent_t*, ocrFatGuid_t, u32, bool), unregisterWaiterEventHcChannel);
#endif

    base->factoryId = factoryId;

    //Setup hint framework
    base->hintPropMap = (u64*)runtimeChunkAlloc(sizeof(u64)*(OCR_HINT_EVT_PROP_END - OCR_HINT_EVT_PROP_START - 1), PERSISTENT_CHUNK);
    OCR_HINT_SETUP(base->hintPropMap, ocrHintPropEventHc, OCR_HINT_COUNT_EVT_HC, OCR_HINT_EVT_PROP_START, OCR_HINT_EVT_PROP_END);
    return base;
}
#endif /* ENABLE_EVENT_HC */
