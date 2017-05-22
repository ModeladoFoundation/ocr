/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST

#include "debug.h"
#include "ocr-comm-platform.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-policy-domain-tasks.h"
#include "ocr-sysboot.h"
#include "experimental/ocr-placer.h"
#include "experimental/ocr-platform-model.h"
#include "utils/hashtable.h"
#include "utils/queue.h"
#include "extensions/ocr-hints.h"

#ifdef ENABLE_EXTENSION_LABELING
#include "experimental/ocr-labeling-runtime.h"
#endif

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#include "policy-domain/hc-dist/hc-dist-policy.h"

#include "worker/hc/hc-worker.h"
//BUG #204 cloning: sep-concern: need to know end type to support edt templates cloning
#include "task/hc/hc-task.h"
#include "event/hc/hc-event.h"

#define DEBUG_TYPE POLICY

#ifdef ENABLE_EXTENSION_PERF
extern void addPerfEntry(ocrPolicyDomain_t *pd, void *executePtr,
                         ocrTaskTemplate_t *taskT);
#endif

// This is in place of using the general purpose 'guidLocation' implementation that relies
// on PD_MSG_GUID_INFO. Since 'guidLocationShort' is extensively called we directly call
// the guid provider here
static inline u8 guidLocationShort(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t guid,
                              ocrLocation_t* locationRes) {
    return pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], guid.guid, locationRes);
}


extern u8 resolveRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t * fatGuid,
                                ocrPolicyMsg_t * msg, bool isBlocking);

#define RETRIEVE_LOCATION_FROM_MSG(pd, fname, dstLoc, DIR) \
    ocrFatGuid_t fatGuid__ = PD_MSG_FIELD_##DIR(fname); \
    RESULT_ASSERT(guidLocationShort(pd, fatGuid__, &dstLoc), ==, 0);

#define RETRIEVE_LOCATION_FROM_GUID_MSG(pd, dstLoc, DIR) \
    ocrFatGuid_t fatGuid__ = PD_MSG_FIELD_##DIR(guid); \
    RESULT_ASSERT(guidLocationShort(pd, fatGuid__, &dstLoc), ==, 0);

#define RETRIEVE_LOCATION_FROM_GUID(pd, dstLoc, guid__) \
    ocrFatGuid_t fatGuid__; \
    fatGuid__.guid = guid__; \
    fatGuid__.metaDataPtr = NULL; \
    RESULT_ASSERT(guidLocationShort(pd, fatGuid__, &dstLoc), ==, 0);

#define PROCESS_MESSAGE_RETURN_NOW(pd, retCode) \
    return retCode;

#define CHECK_PROCESS_MESSAGE_LOCALLY_AND_RETURN \
    if (msg->type & PD_MSG_LOCAL_PROCESS) { \
        ocrPolicyDomainHcDist_t * pdSelfDist = (ocrPolicyDomainHcDist_t *) self; \
        return pdSelfDist->baseProcessMessage(self, msg, isBlocking); \
    }

static void setReturnDetail(ocrPolicyMsg_t * msg, u8 returnDetail) {
    // This is open for debate here #932
    ASSERT(returnDetail == 0);
    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_EVT_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EVT_DESTROY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DEP_SATISFY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_EDTTEMP_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_WORK_CREATE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_WORK_CREATE
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_WORK_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DEP_ADD:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_ADD
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DEP_DYNADD:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_DYNADD
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DB_FREE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_FREE
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    default:
    ASSERT("Unhandled message type in setReturnDetail");
    break;
    }
}

extern u8 processIncomingMsg(ocrPolicyDomain_t * pd, ocrPolicyMsg_t * msg);

u8 processCommEvent(ocrPolicyDomain_t *self, pdEvent_t** evt, u32 idx) {
    ASSERT(((*evt)->properties & PDEVT_TYPE_MASK) == PDEVT_TYPE_MSG);
    DPRINTF(DEBUG_LVL_VERB, "processCommEvent invoked\n");
    processIncomingMsg(self, ((pdEventMsg_t *) *evt)->msg);
    // Ok to systematically free for now because msg events is a separate allocation from the pd msg itself
    // In future revision of the implementation we may have to checkt the return code of processIncomingMsg
    // to make a decision.
    // NOTE: the event is actually auto garbage collected. We check to make sure this
    // is the case
    ASSERT((*evt)->properties & PDEVT_GC);
    // It should also not free the message itself (see above)
    ASSERT(!((*evt)->properties & PDEVT_DESTROY_DEEP));
    *evt = NULL;
    return 0;
}

//TODO should be part of the PD interface (currently in hc-policy.c)
extern ocrObjectFactory_t * resolveObjectFactory(ocrPolicyDomain_t *pd, ocrGuidKind kind);

void getTemplateParamcDepc(ocrPolicyDomain_t * self, ocrFatGuid_t * fatGuid, u32 * paramc, u32 * depc) {
    // Need to deguidify the edtTemplate to know how many elements we're really expecting
    self->guidProviders[0]->fcts.getVal(self->guidProviders[0], fatGuid->guid,
                                        (u64*)&fatGuid->metaDataPtr, NULL, MD_LOCAL, NULL);
    ocrTaskTemplate_t * edtTemplate = (ocrTaskTemplate_t *) fatGuid->metaDataPtr;
    if(*paramc == EDT_PARAM_DEF) *paramc = edtTemplate->paramc;
    if(*depc == EDT_PARAM_DEF) *depc = edtTemplate->depc;
}

//Notify scheduler of policy message before it is processed
static inline void hcDistSchedNotifyPreProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg) {
    //Hard-coded for now, ideally scheduler should register interests
    bool eligible = ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) ||
                  ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_CREATE);
    if ((msg->type & PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER) || !eligible)
        return;
    ocrSchedulerOpNotifyArgs_t notifyArgs;
    notifyArgs.base.location = msg->srcLocation;
    notifyArgs.kind = OCR_SCHED_NOTIFY_PRE_PROCESS_MSG;
    notifyArgs.OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG).msg = msg;
    //Ignore the return code here
    self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
            self->schedulers[0], (ocrSchedulerOpArgs_t*) &notifyArgs, NULL);
    msg->type |= PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER;
}

//Notify scheduler of policy message after it is processed
static inline void hcDistSchedNotifyPostProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg) {
    //Hard-coded for now, ideally scheduler should register interests
    bool eligible = ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) ||
                  ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_CREATE);
    if (!(msg->type & PD_MSG_REQ_POST_PROCESS_SCHEDULER) || !eligible)
        return;
    ocrSchedulerOpNotifyArgs_t notifyArgs;
    notifyArgs.base.location = msg->srcLocation;
    notifyArgs.kind = OCR_SCHED_NOTIFY_POST_PROCESS_MSG;
    notifyArgs.OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_POST_PROCESS_MSG).msg = msg;
    RESULT_ASSERT(self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
                    self->schedulers[0], (ocrSchedulerOpArgs_t*) &notifyArgs, NULL), ==, 0);
    msg->type &= ~PD_MSG_REQ_POST_PROCESS_SCHEDULER;
}


#ifdef ENABLE_OCR_API_DEFERRABLE
static u8 hcDistDeferredProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {
    // Systematically delegate to the base PD
    ocrPolicyDomainHcDist_t * pdSelfDist = (ocrPolicyDomainHcDist_t *) self;
    return pdSelfDist->baseProcessMessage(self, msg, isBlocking);
}
#endif

/*
 * Handle messages requiring remote communications, delegate locals to shared memory implementation.
 */
u8 hcDistProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {
    // When isBlocking is false, it means the message processing is FULLY asynchronous.
    // Hence, when processMessage returns it is not guaranteed 'msg' contains the response,
    // even though PD_MSG_REQ_RESPONSE is set.
    // Conversely, when the response is received, the calling context that created 'msg' may
    // not exist anymore. The response policy message must carry all the relevant information
    // so that the PD can process it.

    // This check is only meant to prevent erroneous uses of non-blocking processing for messages
    // that require a response. For now, only PD_MSG_DEP_REGWAITER message is using this feature.
    ASSERT(msg->bufferSize != 0);
    if ((isBlocking == false) && (msg->type & PD_MSG_REQ_RESPONSE)) {
        ASSERT(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE)
            || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_GUID_METADATA_CLONE)
            // Some scenario we want the satisfy to be blocking
            // (see EDT's finish latch and MD_MOVE)
            || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DEP_SATISFY)
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
            || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_EVT_CREATE)
#endif
            );

        // for a clone the cloning should actually be of an edt template
    }

    bool postProcess = true; // Temporary workaround: See Bug #936

    //BUG #604 msg setup: how to double check that: msg->srcLocation has been filled by getCurrentEnv(..., &msg) ?

    // Determine message's recipient and properties:
    // If destination is not set, check if it is a message with an affinity.
    //  If there's an affinity specified:
    //  - Determine an associated location
    //  - Set the msg destination to that location
    //  - Nullify the affinity guid
    // Else assume destination is the current location

    u8 ret = 0;
    // Pointer we keep around in case we create a copy original message
    // and need to get back to it
    ocrPolicyMsg_t * originalMsg = msg;

    //BUG #605: Locations/affinity: would help to have a NO_LOC default value
    //The current assumption is that a locally generated message will have
    //src and dest set to the 'current' location. If the message has an affinity
    //hint, it is then used to potentially decide on a different destination.
    ocrLocation_t curLoc = self->myLocation;
#ifndef UTASK_COMM2
    u32 properties = 0;
#endif


#ifdef PLACER_LEGACY //BUG #476 - This code is being deprecated
    // Try to automatically place datablocks and edts. Only support naive PD-based placement for now.
    suggestLocationPlacement(self, curLoc, (ocrPlatformModelAffinity_t *) self->platformModel,
                             (ocrLocationPlacer_t *) self->placer, msg);
#else
        hcDistSchedNotifyPreProcessMessage(self, msg);
#endif

#ifdef ENABLE_OCR_API_DEFERRABLE
    //TODO-DEFERRED:
    // There's a tradeoff for enqueuing before or after the scheduler notify. Do we want the scheduler
    // to see operations ahead of time now, or a more "global" analysis at the end of the EDT when
    // all the operations are deferred.
    //
    //TODO-DEFERRED: The management of PD_MSG_IGNORE is okay-ish here. Notify is invoked once now
    //so that we make a placement decision that gives a target location and we use that to generate
    //a remote GUID. Ideally we would do a local GUID+MD to actual location so that the placement
    //decision can be made after the facts.
    if ((msg->type & PD_MSG_DEFERRABLE) && !hcDistDeferredProcessMessage(self, msg, isBlocking)) {
        return 0;
    }
#endif

    DPRINTF(DEBUG_LVL_VERB, "HC-dist processing message @ %p of type 0x%"PRIx32"\n", msg, msg->type);


    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_WORK_CREATE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
        // First query the guid provider to determine if we know the edtTemplate.
#ifdef OCR_ASSERT
        ocrLocation_t srcLocation = msg->srcLocation;
#endif
        //TODO-MD-MT could create a continuation for that
        u8 res = resolveRemoteMetaData(self, &PD_MSG_FIELD_I(templateGuid), msg, (msg->srcLocation == self->myLocation));
        if (res == OCR_EPEND) {
            // We do not handle pending if it is an edt spawned locally as there's
            // context on the call stack we can't just return from.
            ASSERT(srcLocation != curLoc);
            // template's metadata not available, message processing will be rescheduled.
            PROCESS_MESSAGE_RETURN_NOW(self, OCR_EPEND);
        }
        DPRINTF(DEBUG_LVL_VVERB,"WORK_CREATE: try to resolve template GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(templateGuid.guid)));
        // Now that we have the template, we can set paramc and depc correctly
        // This needs to be done because the marshalling of messages relies on paramc and
        // depc being correctly set (so no negative values)
        if((PD_MSG_FIELD_IO(paramc) == EDT_PARAM_DEF) || (PD_MSG_FIELD_IO(depc) == EDT_PARAM_DEF)) {
            getTemplateParamcDepc(self, &PD_MSG_FIELD_I(templateGuid), &PD_MSG_FIELD_IO(paramc), &PD_MSG_FIELD_IO(depc));
        }
        ASSERT(PD_MSG_FIELD_IO(paramc) != EDT_PARAM_UNK && PD_MSG_FIELD_IO(depc) != EDT_PARAM_UNK);
        ASSERT(PD_MSG_FIELD_IO(paramc) != EDT_PARAM_DEF && PD_MSG_FIELD_IO(depc) != EDT_PARAM_DEF);
        if((PD_MSG_FIELD_I(paramv) == NULL) && (PD_MSG_FIELD_IO(paramc) != 0)) {
            // User error, paramc non zero but no parameters
            DPRINTF(DEBUG_LVL_WARN, "error: paramc is non-zero but paramv is NULL\n");
            ASSERT(false);
            PROCESS_MESSAGE_RETURN_NOW(self, OCR_EINVAL);
        }
        ocrFatGuid_t currentEdt = PD_MSG_FIELD_I(currentEdt);
        ocrFatGuid_t parentLatch = PD_MSG_FIELD_I(parentLatch);

        // The placer may have altered msg->destLocation
        // We override if it is labeled
        //TODO shouldn't this be handled by the placer ITSELF somehow ?
        if(PD_MSG_FIELD_I(properties) & GUID_PROP_IS_LABELED) {
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO);
        }

        if (msg->destLocation == curLoc) {
            DPRINTF(DEBUG_LVL_VVERB,"WORK_CREATE: local EDT creation for template GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(templateGuid.guid)));
        } else {
            // For asynchronous EDTs we check the content of depv.
            // If it contains non-persistent events the creation
            // must be synchronous and we change the message flags here.
            if (!(msg->type & PD_MSG_REQ_RESPONSE)) {
                ocrFatGuid_t * depv = PD_MSG_FIELD_I(depv);
                u32 depc = ((depv != NULL) ? PD_MSG_FIELD_IO(depc) : 0);
                u32 i;
                for(i=0; i<depc; i++) {
                    ASSERT(!(ocrGuidIsUninitialized(depv[i].guid)));
                    ocrGuidKind kind;
                    RESULT_ASSERT(self->guidProviders[0]->fcts.getKind(self->guidProviders[0], depv[i].guid, &kind), ==, 0);
                    if ((kind == OCR_GUID_EVENT_ONCE) || (kind == OCR_GUID_EVENT_LATCH)) {
                        msg->type |= PD_MSG_REQ_RESPONSE;
                        DPRINTF(DEBUG_LVL_WARN,"NULL-GUID EDT creation made synchronous: depv[%"PRId32"] is (ONCE|LATCH)\n", i);
                        break;
                    }
                }
            }

            // Outgoing EDT create message
            DPRINTF(DEBUG_LVL_VVERB,"WORK_CREATE: remote EDT creation at %"PRIu64" for template GUID "GUIDF"\n", (u64)msg->destLocation, GUIDA(PD_MSG_FIELD_I(templateGuid.guid)));
#undef PD_MSG
#undef PD_TYPE
            // Before remotely creating the EDT, increment the parent finish scope.
            // On the remote end, a local finish scope is then created and tied to this one.
            if (!(ocrGuidIsNull(parentLatch.guid))) {
                ocrLocation_t parentLatchLoc;
                RETRIEVE_LOCATION_FROM_GUID(self, parentLatchLoc, parentLatch.guid);
                //By construction the parent latch is always local
                ASSERT(parentLatchLoc == curLoc);
                //Check in to parent latch
                PD_MSG_STACK(msg2);
                getCurrentEnv(NULL, NULL, NULL, &msg2);
#define PD_MSG (&msg2)
#define PD_TYPE PD_MSG_DEP_SATISFY
                // This message MUST be fully processed (i.e. parentLatch satisfied)
                // before we return. Otherwise there's a race between this registration
                // and the current EDT finishing.
                msg2.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_I(satisfierGuid.guid) = NULL_GUID; // BUG #587: what to set these as?
                PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(guid) = parentLatch;
                PD_MSG_FIELD_I(payload.guid) = NULL_GUID;
                PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(currentEdt) = currentEdt;
                PD_MSG_FIELD_I(slot) = OCR_EVENT_LATCH_INCR_SLOT;
#ifdef REG_ASYNC_SGL
                PD_MSG_FIELD_I(mode) = -1; //Doesn't matter for latch
#endif
                PD_MSG_FIELD_I(properties) = 0;
                RESULT_PROPAGATE(self->fcts.processMessage(self, &msg2, true));
#undef PD_MSG
#undef PD_TYPE
            }
        }
        break;
    }
    case PD_MSG_METADATA_COMM:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_METADATA_COMM
        ASSERT(msg->destLocation != msg->srcLocation);
        //Notes about the previous assert:
        //- Basically says I know we're pushing and pulling so it makes
        //no sense that a PD asks itself something. For now this is useful
        //to catch errors.
        //- This may be sound though if we think of this message as low
        //level enough that its src/dest must have been set. This is in opposition
        //to a message generated by the user interface for which the PD must decide
        //who is the right recipient.
        //- This is also related to who decides what to do of a message. For instance
        //in the case of an add dependence, is it the src, dest or none that's in "charge"
        //of the message. That's an interesting discussion to have because maybe the PD
        //should be dumb and let "something" related to the MD decide what to do.
        if (msg->destLocation != self->myLocation) {
            DPRINTF(DEBUG_LVL_VERB, "Sending MD_COMM to %"PRIu64" mode=%"PRIu64" for "GUIDF"\n", msg->destLocation, PD_MSG_FIELD_I(mode), GUIDA(PD_MSG_FIELD_I(guid)));
        } else {
            DPRINTF(DEBUG_LVL_VERB, "Receiving MD_COMM from %"PRIu64" mode=%"PRIu64" for "GUIDF"\n", msg->srcLocation, PD_MSG_FIELD_I(mode), GUIDA(PD_MSG_FIELD_I(guid)));
        }
        // Fall-through:
        // - Outgoing: just forward as a one-way
        // - Incoming: local processing and answer is set in response's field
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_CREATE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        // The placer may have altered msg->destLocation
        // We override in case the GUID is labeled
        if (PD_MSG_FIELD_IO(properties) & GUID_PROP_IS_LABELED) {
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO);
            ASSERT(false); //TODO need to think about how labeled DB creation plays out with MD
        }

        //TODO-MD: Pas cool. The DB carries a hint to a location that's used
        //as a destination location for the message and now we have to overwrite
        //as the MD factory wants to process it first.
        if (!(PD_MSG_FIELD_IO(properties) & DB_PROP_NO_ACQUIRE)) {
            msg->destLocation = curLoc;
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_EVT_CREATE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_CREATE
        if (PD_MSG_FIELD_I(properties) & GUID_PROP_IS_LABELED) {
            // We need to resolve location because of labeled GUIDs.
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO);
        } else {
            // for all local messages, fall-through and let local PD to process
            msg->destLocation = curLoc;
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    // Need to determine the destination of the message based
    // on the operation and guids it involves.
    case PD_MSG_WORK_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
        //TODO-MD-EDT: This needs better support in the GUID/MD to decide if it is a local or fwd op
        //+ there's probably a leak at the origin PD
        u64 val;
        self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_I(guid.guid), &val, NULL, MD_LOCAL, NULL);
        if (val == 0) { // No local representent
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        } // else, we do have a local representent, destroy locally
        //TODO-MD-EDT this should also destroy the proxy left at the origin
        DPRINTF(DEBUG_LVL_VVERB, "WORK_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_SATISFY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB,"DEP_SATISFY: target is %"PRId32"\n", (u32) msg->destLocation);
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
#ifndef XP_CHANNEL_EVT_NONFIFO
        if (msg->destLocation != curLoc) {
            ocrGuidKind kind;
            // Check if it's a channel event that needs a blocking satisfy
            RESULT_ASSERT(self->guidProviders[0]->fcts.getKind(
                              self->guidProviders[0], PD_MSG_FIELD_I(guid.guid), &kind), ==, 0);
#ifdef ALLOW_EAGER_DB
            // If dest is remote and the target is a channel event, we check if the DB is EAGER
            ocrFatGuid_t dbFat = PD_MSG_FIELD_I(payload);
            bool isNullGuid = ocrGuidIsNull(dbFat.guid);
            if ((kind == OCR_GUID_EVENT_CHANNEL) && (!isNullGuid)) {
                ocrLocation_t dbLoc;
                RESULT_ASSERT(guidLocationShort(self, dbFat, &dbLoc), ==, 0);
                if (dbLoc == curLoc) {
                    self->guidProviders[0]->fcts.getVal(self->guidProviders[0], dbFat.guid,
                                    (u64*)&(dbFat.metaDataPtr), NULL, MD_LOCAL, NULL);
                    ASSERT(dbFat.metaDataPtr != NULL);
                    // Check for eager
                    ocrHint_t hint;
                    RESULT_ASSERT(ocrHintInit(&hint, OCR_HINT_DB_T), ==, 0);
                    ocrDataBlock_t * dbSelf = (ocrDataBlock_t *) dbFat.metaDataPtr;
                    RESULT_ASSERT(((ocrDataBlockFactory_t *)self->factories[dbSelf->fctId])->fcts.getHint(dbSelf, &hint), ==, 0);
                    u64 hintValue = 0ULL;
                    if ((ocrGetHintValue(&hint, OCR_HINT_DB_EAGER, &hintValue) == 0) && (hintValue != 0)) {
                        DPRINTF(DEBUG_LVL_VVERB,"Eager: DETECTED Eager hint on DB "GUIDF" for remote channel\n", GUIDA(dbSelf->guid));
                        ocrPolicyMsg_t * msgClone;
                        regNode_t node;
                        node.guid = PD_MSG_FIELD_I(guid.guid);
                        node.slot = 0;
                        node.mode = DB_MODE_RO;
                        PD_MSG_STACK(msgStack);
                        u64 msgSize = (_PD_MSG_SIZE_IN(PD_MSG_GUID_METADATA_CLONE)) + sizeof(u32) + sizeof(regNode_t) - sizeof(char*);
                        if (msgSize > sizeof(ocrPolicyMsg_t)) {
                            //TODO-MD-SLAB
                            msgClone = (ocrPolicyMsg_t *) self->fcts.pdMalloc(self, msgSize);
                            ADebug(AllocDebugAllPD, "hc-dist-policy.c/hcDistProcessMessage()/PD_MSG_DEP_SATISFY "
                                   "pdMalloc(%ld), addr=%p\n", msgSize, msgClone);
                            initializePolicyMessage(msgClone, msgSize);
                        } else {
                            msgClone = &msgStack;
                        }
                        getCurrentEnv(NULL, NULL, NULL, msgClone);
#undef PD_MSG
#undef PD_TYPE
#define PD_MSG (msgClone)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
                        msgClone->type = PD_MSG_GUID_METADATA_CLONE | PD_MSG_REQUEST;
                        PD_MSG_FIELD_IO(guid) = dbFat;
                        PD_MSG_FIELD_I(type) = MD_CLONE | MD_NON_COHERENT;
                        PD_MSG_FIELD_I(dstLocation) = msg->destLocation;
                        //TODO-EAGER Need to support multiple dependences to be satisfied
                        char *  writePtr = (char *) &PD_MSG_FIELD_I(addPayload);
                        ((u32*)writePtr)[0] = ((u32)1);
                        writePtr+=sizeof(u32);
                        ((regNode_t *)writePtr)[0] = node;
                        DPRINTF(DEBUG_LVL_VVERB,"Eager: PUSH DB Eager for DB "GUIDF" on remote channel\n", GUIDA(dbSelf->guid));
                        RESULT_PROPAGATE(self->fcts.processMessage(self, msgClone, false));
#undef PD_MSG
#undef PD_TYPE
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
                        if (msg->type & PD_MSG_REQ_RESPONSE) {
                            msg->type |= PD_MSG_RESPONSE;
                            msg->type &= ~PD_MSG_REQ_RESPONSE;
                            PD_MSG_FIELD_O(returnDetail) = 0;
                        }
                        msg->type &= ~PD_MSG_REQUEST;
                        PROCESS_MESSAGE_RETURN_NOW(self, 0);
                    } // has Hint
                } // db is remote
            } //channel and db is not nullGuid
#endif /* ALLOW_EAGER_DB */

#ifndef COMMWRK_PROCESS_SATISFY_CHANNEL_ONLY
            // Turn the call into a blocking call, if we're not ordering them on the receiving end
            if (kind == OCR_GUID_EVENT_CHANNEL) {
                msg->type |= PD_MSG_REQ_RESPONSE;
                isBlocking = true;
            }
#endif /*!COMMWRK_PROCESS_SATISFY_CHANNEL_ONLY*/
        }
#endif /*!XP_CHANNEL_EVT_NONFIFO*/
#endif /*ENABLE_EXTENSION_CHANNEL_EVT*/
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_EVT_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EVT_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "EVT_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
        // For mpilite long running EDTs to handle blocking destroy of labeled events
        ocrTask_t *curEdt = NULL;
        getCurrentEnv(NULL, NULL, &curEdt, NULL);
        if ((curEdt != NULL) && (curEdt->flags & OCR_TASK_FLAG_LONG)) {
            msg->type |= PD_MSG_REQ_RESPONSE;
            isBlocking = true;
        }
#endif
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_DESTROY:
    {
        ASSERT(false);
        //TODO this is where the PD should ask the factory what to do with the call
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_DESTROY
        u64 val;
        self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_I(guid.guid), &val, NULL, MD_LOCAL, NULL);
        if (!val) { // Have no local representent for the DB, forward.
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        } // else fall-through for local MD to process
        DPRINTF(DEBUG_LVL_VVERB, "DB_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_FREE:
    {
        //TODO this is where the PD should ask the factory what to do with the call
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_FREE
        u64 val;
        self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_I(guid.guid), &val, NULL, MD_LOCAL, NULL);
        if (!val) { // Have no local representent for the DB, forward.
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        } // else fall-through for local MD to process
        DPRINTF(DEBUG_LVL_VVERB, "DB_FREE: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_EDTTEMP_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "EDTTEMP_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_GUID_INFO:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_INFO
        u64 val;
        ocrGuidKind kind;
        // So we're trying to resolve guid info and if it's a guidmap we need to pull it here
        self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, &kind, MD_LOCAL, NULL);
        if ((val == 0) && (kind == OCR_GUID_GUIDMAP)) {
            //BUG #536: cloning - piggy back on the mecanism that fetches templates
            RESULT_ASSERT(resolveRemoteMetaData(self, &PD_MSG_FIELD_IO(guid), msg, true), ==, 0);
        } else {
            //BUG #536: cloning: What's the meaning of guid info in distributed ?
            msg->destLocation = curLoc;
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_GUID_METADATA_CLONE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
        if (msg->type & PD_MSG_REQUEST) {
            if (HAS_MD_CLONE(PD_MSG_FIELD_I(type))) {
                // Do not call the macro because it relies on GUID_INFO
                // and we go into a deadlock when cloning GUID maps
                // RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO);
                self->guidProviders[0]->fcts.getLocation(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(msg->destLocation));
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: request for guid="GUIDF" src=%"PRId32" dest=%"PRId32"\n",
                        GUIDA(PD_MSG_FIELD_IO(guid.guid)), (u32)msg->srcLocation, (u32)msg->destLocation);
                if ((msg->destLocation != curLoc) && (msg->srcLocation == curLoc)) {
                    // Outgoing request
                    // NOTE: In the current implementation when we call metadata-clone
                    //       it is because we've already checked the guid provider and
                    //       the guid is not available.
                    // We'll still query the factory associated with the metadata in case it can forge a local copy
                    // The caller must ensure this code is executed by a single worker thread per OCR object instance.
                    ocrGuidKind guidKind;
                    u8 ret = self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &guidKind);
                    ASSERT(!ret);
                    // TODO: Extend to ocrObjectFactory_t when md cloning is fully implemented
                    // TODO: forge support: Only sticky events supported so far
                    if ((guidKind == OCR_GUID_DB) || MDC_SUPPORT_EVT(guidKind)) {
                        // Clients are:
                        // - Event: PD_MSG_DEP_ADD (Blocking)
                        // - Db: PD_MSG_DB_ACQUIRE (Non-Blocking)
                        //
                        ocrObjectFactory_t * factory = resolveObjectFactory(self, guidKind);
                        DPRINTF(DEBUG_LVL_VVERB, "Requesting clone operation on local factory for remote GUID "GUIDF"\n",  GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                        ocrObject_t * mdPtr;
                        factory->clone(factory, PD_MSG_FIELD_IO(guid.guid), &mdPtr, self->myLocation, PD_MSG_FIELD_I(type));
                        //TODO: because the underlying messages in clone are asynchronous one-way, the call here
                        //is almost certain not to return a valid mdPtr. In the current implementation this is ok
                        //because the return code for METADATA_CLONE would be EPEND. That raises questions on the
                        //semantic of clone though. Is this an asynchronous call or what ?
                        PD_MSG_FIELD_IO(guid.metaDataPtr) = mdPtr;
                        // Note the current message never actually leaves the PD here. It is up to the factory
                        // clone operation to issue the right calls to eventually trigger PD_MSG_METADATA_COMM
                        // messages that carry serialized metadata across PDs.
                        msg->type &= ~PD_MSG_REQUEST;
                        msg->type |= PD_MSG_RESPONSE;
                        if (mdPtr != NULL) {
                            PROCESS_MESSAGE_RETURN_NOW(self, 0);
                        } else {
                            PROCESS_MESSAGE_RETURN_NOW(self, OCR_EPEND);
                        }
                    }
                    // else fall-through and request the cloning
                    // If it's a non-blocking processing, will set the returnDetail to busy after the request is sent out
                }
            } else { // MD_MOVE request
                // In current implementation this type of message never leaves the current PD
                ASSERT(msg->srcLocation == msg->destLocation);
                ASSERT(msg->srcLocation == curLoc);
                // Local PD needs to inspect the MD_MOVE operation and take appropriate actions
                // which include notifying relevant PD modules and invoke lower-level MD_COMM APIs
            }
        } // end outgoing request

        if ((msg->destLocation == curLoc) && (msg->srcLocation != curLoc) && (msg->type & PD_MSG_RESPONSE)) {
            // Incoming response to a MD_CLONE request posted earlier
            ocrGuidKind tkind;
            self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &tkind);
            if (tkind == OCR_GUID_EDT_TEMPLATE) {
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for template="GUIDF")\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                // Incoming response to an asynchronous metadata clone
                u64 metaDataSize = sizeof(ocrTaskTemplateHc_t) + (sizeof(u64) * OCR_HINT_COUNT_EDT_HC);
                void * metaDataPtr = self->fcts.pdMalloc(self, metaDataSize);
                ADebug(AllocDebugAllPD, "hc-dist-policy.c/hcDistProcessMessage()/PD_MSG_GUID_METADATA_CLONE[1] "
                       "pdMalloc(%ld), addr=%p\n", metaDataSize, metaDataPtr);
                ASSERT(PD_MSG_FIELD_IO(guid.metaDataPtr) != NULL); // Currently points to message's payload
                ASSERT(PD_MSG_FIELD_O(size) == metaDataSize);
                hal_memCopy(metaDataPtr, PD_MSG_FIELD_IO(guid.metaDataPtr), metaDataSize, false);
                void * base = PD_MSG_FIELD_IO(guid.metaDataPtr);
                ocrTaskTemplateHc_t * tpl = (ocrTaskTemplateHc_t *) metaDataPtr;
                if (tpl->hint.hintVal != NULL) {
                    tpl->hint.hintVal  = (u64*)((u64)base + sizeof(ocrTaskTemplateHc_t));
                }
#ifdef ENABLE_RESILIENCY
                tpl->base.base.kind = OCR_GUID_EDT_TEMPLATE;
                tpl->base.base.size = metaDataSize;
#endif

#ifdef ENABLE_EXTENSION_PERF
                tpl->base.taskPerfsEntry = NULL;
                addPerfEntry(self, tpl->base.executePtr, &tpl->base);
#endif
                PD_MSG_FIELD_IO(guid.metaDataPtr) = metaDataPtr;
                self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), (u64) metaDataPtr);
                PROCESS_MESSAGE_RETURN_NOW(self, 0);
            }
#ifdef ENABLE_EXTENSION_LABELING
            else if (tkind == OCR_GUID_GUIDMAP) {

                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for guidMap="GUIDF")\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                ocrGuidMap_t * mapOrg = (ocrGuidMap_t *) PD_MSG_FIELD_IO(guid.metaDataPtr);
                u64 metaDataSize = ((sizeof(ocrGuidMap_t) + sizeof(s64) - 1) & ~(sizeof(s64)-1)) + mapOrg->numParams*sizeof(s64);
                void * metaDataPtr = self->fcts.pdMalloc(self, metaDataSize);
                ADebug(AllocDebugAllPD, "hc-dist-policy.c/hcDistProcessMessage()/PD_MSG_GUID_METADATA_CLONE[2] "
                       "pdMalloc(%ld), addr=%p\n", metaDataSize, metaDataPtr);
                ASSERT(PD_MSG_FIELD_IO(guid.metaDataPtr) != NULL);
                hal_memCopy(metaDataPtr, mapOrg, metaDataSize, false);
                ocrGuidMap_t * map = (ocrGuidMap_t *) metaDataPtr;
                if (mapOrg->numParams) { // Fix-up params
                    map->params = (s64*)((char*)map + ((sizeof(ocrGuidMap_t) + sizeof(s64) - 1) & ~(sizeof(s64)-1)));
                }
                PD_MSG_FIELD_IO(guid.metaDataPtr) = metaDataPtr;
                self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), (u64) metaDataPtr);
                PROCESS_MESSAGE_RETURN_NOW(self, 0);
            }
#endif
            else {
                ASSERT(tkind == OCR_GUID_AFFINITY);
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for affinity "GUIDF")\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                // Intercept that and make a copy of the affinity
                u64 metaDataSize = sizeof(ocrAffinity_t);
                void * metaDataPtr = self->fcts.pdMalloc(self, metaDataSize);
                ADebug(AllocDebugAllPD, "hc-dist-policy.c/hcDistProcessMessage()/PD_MSG_GUID_METADATA_CLONE[3] "
                       "pdMalloc(%ld), addr=%p\n", metaDataSize, metaDataPtr);
                hal_memCopy(metaDataPtr, PD_MSG_FIELD_IO(guid.metaDataPtr), metaDataSize, false);
                PD_MSG_FIELD_IO(guid.metaDataPtr) = metaDataPtr;
                self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), (u64) metaDataPtr);
                PROCESS_MESSAGE_RETURN_NOW(self, 0);
            }
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_GUID_DESTROY:
    {
        // This is always a local call and there should be something to destroy in the map
#ifdef OCR_ASSERT
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
        u64 val;
        self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_I(guid).guid, &val, NULL, MD_LOCAL, NULL);
        ASSERT(val != 0);
//         RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
//         DPRINTF(DEBUG_LVL_VVERB, "GUID_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
#endif
        break;
    }
    case PD_MSG_MGT_RL_NOTIFY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
        if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
            ASSERT(PD_MSG_FIELD_I(runlevel) == RL_COMPUTE_OK);
            ASSERT(PD_MSG_FIELD_I(properties) == (RL_REQUEST | RL_BARRIER | RL_TEAR_DOWN));
            // Incoming rl notify message from another PD
            ocrPolicyDomainHcDist_t * rself = ((ocrPolicyDomainHcDist_t*)self);
            // incr the shutdown counter (compete with hcDistPdSwitchRunlevel)
            u32 oldAckValue = hal_xadd32(&rself->shutdownAckCount, 1);
            ocrPolicyDomainHc_t * bself = (ocrPolicyDomainHc_t *) self;
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: incoming: old value for shutdownAckCount=%"PRIu32"\n", oldAckValue);
            if (oldAckValue == (self->neighborCount)) {
                // Got messages from all PDs and self.
                // Done with distributed shutdown and can continue with the local shutdown.
                PD_MSG_STACK(msgNotifyRl);
                getCurrentEnv(NULL, NULL, NULL, &msgNotifyRl);
                DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: distributed shutdown is done. Resume local shutdown\n");
                RESULT_ASSERT(rself->baseSwitchRunlevel(self, bself->rlSwitch.runlevel, bself->rlSwitch.properties), ==, 0);
            }
            //Note: Per current implementation, even if PDs are not in the same runlevel,
            //      the first time a PD receives a ack it has to be in the last phase up
            //      otherwise it couldn't have received the message
            bool doLocalShutdown = ((oldAckValue == 0) && (RL_GET_PHASE_COUNT_UP(self, RL_USER_OK) == bself->rlSwitch.nextPhase));
            if (!doLocalShutdown) {
                DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: got notification RL=%"PRId32" PH=%"PRId32"\n", bself->rlSwitch.runlevel, bself->rlSwitch.nextPhase);
                PD_MSG_FIELD_O(returnDetail) = 0;
                return 0;
            } else {
                DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: fall-through\n");
            }
            // else
            // We are receiving a shutdown message from another PD and both
            // the ack counter is '0' and the runlevel RL_USER_OK is at its
            // highest phase. It means ocrShutdown() did not originate from
            // this PD, hence must initiate the local shutdown process.
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_DYNADD:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_DYNADD
        // CHECK_PROCESS_MESSAGE_LOCALLY_AND_RETURN;
        // RETRIEVE_LOCATION_FROM_MSG(self, edt, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNADD: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_DYNREMOVE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        //TODO-MD-EDT: Shouldn't be able to dynremove from remote.
        // RETRIEVE_LOCATION_FROM_MSG(self, edt, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNREMOVE: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_ACQUIRE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
        ASSERT((msg->srcLocation == msg->destLocation) && "All DB_ACQUIRE messages should be local");
        if (msg->type & PD_MSG_REQUEST) {
            // Whether the DB is local or not try to resolve it.
            // - Rationale is that even if the MD is already available in this PD,
            //   the current worker executing the acquire may have to broker a MD clone.
            // - First time, this message may be queued up. When MD is resolved, the message
            //   is processed again, comes here and will fall-through
            ocrFatGuid_t fguid = PD_MSG_FIELD_IO(guid);
            // TODO: Do we want blocking to be driven by the blocking parameter of processMessage ?
            // TODO: Ideally disambiguation here based on LowLevel MD if operation should pull the MD or delegate to where GUID lives
            u8 ret = resolveRemoteMetaData(self, &fguid, msg, /*isBlocking=*/false);

            // NOTE: PD_MSG_DB_ACQUIRE messages never leaves the policy domain.
            // Acquisition protocol is implemented by the metadata.
            if (ret == OCR_EPEND) {
                // Whoever is calling processMessage MUST be able to handle asynchrony for ACQUIRE
                PROCESS_MESSAGE_RETURN_NOW(self, OCR_EPEND);
            }
            // else we have metadata available and want to fall-through to invoke acquire on the metadata.
            // The call may work or return OCR_EBUSY when the acquire request cannot be accomodated.
            // Then, the MD implementation is responsible for recording the request and grant the waiter
            // access at some point in the future.
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_RELEASE:
    {
        // The DB is foreign but if we're releasing it, we must have a MD for it.
        // This message processing is always local but the underlying metadata
        // implementation for the DB may trigger metadata coherence operations.
        #ifdef OCR_ASSERT
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_RELEASE
        ASSERT((msg->srcLocation == msg->destLocation) && "All DB_RELEASE messages should be local");
        if ((msg->srcLocation == curLoc) && (msg->destLocation != curLoc)) {
            // If there are multiple concurrent acquire on the same DB's metadata, only one cloning operation is issued and others are enqueued.
            // Note this differs from the actual acquire operation where concurrency is handle in the datablock implementation.
            // This is just for assertion purpose and ok if not executed when asserts are off
            u64 val;
            ASSERT(self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL, MD_LOCAL, NULL) == 0);
        }
#undef PD_MSG
#undef PD_TYPE
        #endif/*OCR_ASSERT*/
        break;
    }
    case PD_MSG_DEP_REGSIGNALER:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_REGSIGNALER
        RETRIEVE_LOCATION_FROM_MSG(self, dest, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_REGSIGNALER: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_REGWAITER:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_REGWAITER
        RETRIEVE_LOCATION_FROM_MSG(self, dest, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_REGWAITER: destGuid is "GUIDF" target is %"PRId32"\n",
                                GUIDA(PD_MSG_FIELD_I(dest.guid)), (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_SCHED_GET_WORK:
    {
        // fall-through and do regular take
        break;
    }
    case PD_MSG_SCHED_TRANSACT:
    {
        // Scheduler sets dest location
        DPRINTF(DEBUG_LVL_VVERB, "SCHED_TRANSACT: target is %"PRId32"\n", (u32)msg->destLocation);
        break;
    }
    case PD_MSG_SCHED_ANALYZE:
    {
        // Scheduler sets dest location
        DPRINTF(DEBUG_LVL_VVERB, "SCHED_ANALYZE: target is %"PRId32"\n", (u32)msg->destLocation);
        break;
    }
    case PD_MSG_MGT_MONITOR_PROGRESS:
    {
        msg->destLocation = curLoc;
        break;
    }
    case PD_MSG_EVT_GET: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EVT_GET
        // HACK for BUG #865 Remote lookup for event completion
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_HINT_GET: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_HINT_GET
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_HINT_SET: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_HINT_SET
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_RESILIENCY_CHECKPOINT: {
        // Resiliency manager sets dest location
        DPRINTF(DEBUG_LVL_VVERB, "RESILIENCY_CHECKPOINT: target is %"PRId32"\n", (u32)msg->destLocation);
        break;
    }
    case PD_MSG_DEP_UNREGSIGNALER: {
        //Not implemented: see #521, #522
        ASSERT(false && "Not implemented PD_MSG_DEP_UNREGSIGNALER");
        break;
    }
    case PD_MSG_DEP_UNREGWAITER: {
        //Not implemented: see #521, #522
        ASSERT(false && "Not implemented PD_MSG_DEP_UNREGWAITER");
        break;
    }
    // filter out local messages
    case PD_MSG_DEP_ADD:
    case PD_MSG_MEM_OP:
    case PD_MSG_MEM_ALLOC:
    case PD_MSG_MEM_UNALLOC:
    case PD_MSG_WORK_EXECUTE:
    case PD_MSG_EDTTEMP_CREATE:
    case PD_MSG_GUID_CREATE:
    case PD_MSG_SCHED_NOTIFY:
    case PD_MSG_SAL_OP:
    case PD_MSG_SAL_PRINT:
    case PD_MSG_SAL_READ:
    case PD_MSG_SAL_WRITE:
    case PD_MSG_SAL_TERMINATE:
    case PD_MSG_MGT_OP: //BUG #587 not-supported: PD_MSG_MGT_OP is probably not always local
    case PD_MSG_MGT_REGISTER:
    case PD_MSG_MGT_UNREGISTER:
    case PD_MSG_GUID_RESERVE: {
        // Set beforehand by the caller depending on where it want the guid reserved from
        break;
    }
    case PD_MSG_GUID_UNRESERVE:
    case PD_MSG_RESILIENCY_NOTIFY:
    case PD_MSG_RESILIENCY_MONITOR:
    // case PD_MSG_EVT_CREATE:
    {
        msg->destLocation = curLoc;
        // for all local messages, fall-through and let local PD to process
        break;
    }
    default:
        //BUG #587 not-supported: not sure what to do with those.
        // ocrDbReleaseocrDbMalloc, ocrDbMallocOffset, ocrDbFree, ocrDbFreeOffset

        // This is just a fail-safe to make sure the
        // PD impl accounts for all type of messages.
        ASSERT(false && "Unsupported message type");
    }

    // By now, we must have decided what's the actual destination of the message

    // Delegate msg to another PD
    if(msg->destLocation != curLoc) {
        //NOTE: Some of the messages logically require a response, but the PD can
        // already know what to return or can generate a response on behalf
        // of another PD and let it know after the fact. In that case, the PD may
        // void the PD_MSG_REQ_RESPONSE msg's type and treat the call as a one-way
        //TODO cleanup
        DPRINTF(DEBUG_LVL_VVERB,"0x%"PRIx32" (msg->type & PD_MSG_REQ_RESPONSE)=%"PRIu32" \n", msg->type, ((msg->type & PD_MSG_REQ_RESPONSE) && isBlocking));
        fflush(stdin);
        // Message requires a response, send request and wait for response.
        if ((msg->type & PD_MSG_REQ_RESPONSE) && isBlocking) {
            DPRINTF(DEBUG_LVL_VVERB,"Can't process message locally sending and "
                    "processing a two-way message @ (orig: %p, now: %p) to %"PRIu64"\n", originalMsg, msg,
                    msg->destLocation);

#ifdef UTASK_COMM2
            // Get a strand and note that we want to send the message
            // We will note that the next action should be to send the message.
            // In an ideal world, we would package our continuation after that but
            // for now we'll just have to wait on the response
            pdEvent_t *event;
            RESULT_ASSERT(pdCreateEvent(self, &event, PDEVT_TYPE_MSG, 0), ==, 0);
            ((pdEventMsg_t *) event)->msg = msg;
            ((pdEventMsg_t*)event)->properties |= COMM_STACK_MSG;
            pdMarkReadyEvent(self, event);
            pdStrand_t * msgStrand;
            RESULT_ASSERT(
                pdGetNewStrand(self, &msgStrand, self->strandTables[PDSTT_COMM-1], event, 0 /*unused*/),
                ==, 0);
            pdAction_t * sendAction = pdGetProcessMessageAction(NP_COMM);
            // Do NOT clear the hold since we are waiting on the event next
            RESULT_ASSERT(
                pdEnqueueActions(self, msgStrand, 1, &sendAction, false/*NO clear hold*/),
                ==, 0);
            RESULT_ASSERT(pdUnlockStrand(msgStrand), ==, 0);

            // Process strands until we get our message back
            RESULT_ASSERT(pdProcessResolveEvents(self, NP_WORK, 1, &event, PDSTT_CLEARHOLD), ==, 0);

            // At this point, we can resolve the event and proceed
            // We also clear the fwdHold to let the runtime know that we no longer
            // need this strand
            {
                u8 ret __attribute__((unused)) = pdResolveEvent(self, (u64*)&event, true);
                ASSERT(ret == 0 || ret == OCR_ENOP);
            }
            ocrPolicyMsg_t *response = ((pdEventMsg_t*)event)->msg;
            // We have the response and won't do anything with the event anymore; we destroy it
            // We could destroy it with the handle but clean enough here
            RESULT_ASSERT(pdDestroyEvent(self, event), ==, 0);
#else
            // Since it's a two-way, we'll be waiting for the response and set PERSIST.
            // NOTE: underlying comm-layer may or may not make a copy of msg.
            properties |= TWOWAY_MSG_PROP;
            properties |= PERSIST_MSG_PROP;
            ocrMsgHandle_t * handle = NULL;
            self->fcts.sendMessage(self, msg->destLocation, msg, &handle, properties);
            // Wait on the response handle for the communication to complete.
            DPRINTF(DEBUG_LVL_VVERB,"Waiting for reply from %"PRId32"\n", (u32)msg->destLocation);
            self->fcts.waitMessage(self, &handle);
            DPRINTF(DEBUG_LVL_VVERB,"Received reply from %"PRId32" for original message @ %p\n",
                    (u32)msg->destLocation, originalMsg);
            ASSERT(handle->response != NULL);

            // Check if we need to copy the response header over to the request msg.
            // Happens when the message includes some additional variable size payload
            // and request message cannot be reused. Or the underlying communication
            // platform was not able to reuse the request message buffer.

            //
            // Warning: From now on EXCLUSIVELY work on the response message
            //

            // Warning: Do NOT try to access the response IN fields !

            ocrPolicyMsg_t * response = handle->response;
            DPRINTF(DEBUG_LVL_VERB, "Processing response @ %p to original message @ %p\n", response, originalMsg);
#endif
            switch (response->type & PD_MSG_TYPE_ONLY) {
            case PD_MSG_GUID_METADATA_CLONE:
            {
                // Do not need to perform a copy here as the template proxy mecanism
                // is systematically making a copy on write in the guid provider.
            break;
            }
            default: {
                break;
            }
            } //end switch

            //
            // At this point the response message is ready to be returned
            //

            // Since the caller only has access to the original message we need
            // to make sure it's up-to-date.

            //BUG #587: even if original contains the response that has been
            // unmarshalled there, how safe it is to let the message's payload
            // pointers escape into the wild ? They become invalid when the message
            // is deleted.

            if (originalMsg != response) {
                //BUG #587: Here there are a few issues:
                // - The response message may include marshalled pointers, hence
                //   the original message may be too small to accomodate the payload part.
                //   In that case, I don't see how to avoid malloc-ing new memory for each
                //   pointer and update the originalMsg's members, since the use of that
                //   pointers may outlive the message lifespan. Then there's the question
                //   of when those are freed.
                u64 baseSize = 0, marshalledSize = 0;
                ocrPolicyMsgGetMsgSize(response, &baseSize, &marshalledSize, 0);

                // That should only happen for cloning for which we've already
                // extracted payload as a separated heap-allocated pointer
                ASSERT(baseSize <= originalMsg->bufferSize);

                // Marshall 'response' into 'originalMsg': DOES NOT duplicate the payload

                //BUG #587: need to double check exactly what kind of messages we can get here and
                //how the payload would have been serialized.
                // DEPRECATED comment
                // Each current pointer is copied at the end of the message as payload
                // and the pointers then points to that data.
                // Note: originalMsg's usefulSize (request) is going to be updated to response's one.
                // Here we just need something that does a shallow copy
                u32 bufBSize = originalMsg->bufferSize;
                // Copy msg into the buffer for the common part
                hal_memCopy(originalMsg, response, baseSize, false);
                originalMsg->bufferSize = bufBSize;
                // ocrPolicyMsgUnMarshallMsg((u8*)handle->response, NULL, originalMsg, MARSHALL_ADDL);
                // ocrPolicyMsgMarshallMsg(handle->response, baseSize, (u8*)originalMsg, MARSHALL_DUPLICATE);
                self->fcts.pdFree(self, response);
            }

            if ((originalMsg != msg) && (msg != response)) {
                // Just double check if a copy had been made for the request and free it.
                self->fcts.pdFree(self, msg);
            }
#ifndef UTASK_COMM2
            handle->destruct(handle);
#endif
        } else {
            // Either a one-way request or an asynchronous two-way
            DPRINTF(DEBUG_LVL_VVERB,"Sending a one-way request or response to asynchronous two-way msg @ %p to %"PRIu64"\n",
                    msg, msg->destLocation);

            if (msg->type & PD_MSG_REQ_RESPONSE) {
                ret = OCR_EPEND; // return to upper layer the two-way is pending
            }

            //LIMITATION: For one-way we cannot use PERSIST and thus must request
            // a copy to be done because current implementation doesn't support
            // "waiting" on a one-way.
            u32 sendProp = (msg->type & PD_MSG_REQ_RESPONSE) ? ASYNC_MSG_PROP : 0; // indicates copy required

            if (msg->type & PD_MSG_RESPONSE) {
                sendProp = ASYNC_MSG_PROP;
                // Outgoing asynchronous response for a two-way communication
                // Should be the only asynchronous two-way msg kind for now
                ASSERT((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_GUID_METADATA_CLONE);
            } else {
                ASSERT(msg->type & PD_MSG_REQUEST);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                if (((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) && !(msg->type & PD_MSG_REQ_RESPONSE)) {
                    ASSERT(ocrGuidIsNull(PD_MSG_FIELD_IO(guid.guid)));
                    // Do a full marshalling to make sure we capture paramv/depv
                    ocrMarshallMode_t marshallMode = MARSHALL_FULL_COPY;
                    sendProp |= (((u32)marshallMode) << COMM_PROP_BEHAVIOR_OFFSET);
                }
#undef PD_MSG
#undef PD_TYPE
            }
            u8 res = 0;
#ifdef UTASK_COMM2
            // one-way request:
            // We don't care as much about sendProp as computed above; what we
            // do care about is whether or not we need to copy the message
            // since it will be encapsulated in a micro-task. This should also
            // go away when everything is MT friendly.

            //TODO-MT-COMM: Here we may actually have a notion of whether or not the message
            //is persistent. For instance, in deferred the message lives somewhere on the heap
            //hence it may not be necessary to make a new copy if the marshalled version fits there.
            //Addl note: the deferred mode could pre-allocate larger buffers to avoid a re-alloc here too.

            // This behavior is taken from the delegate comm-api:
            //   - always make a DUPLICATE copy
            //   - EXCEPT if we are doing a remote EDT creation (PD_MSG_WORK_CREATE and
            //     no response requirement
            ocrMarshallMode_t marshallMode = MARSHALL_DUPLICATE; // Default
            if(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) && !(msg->type & PD_MSG_REQ_RESPONSE)) {
                marshallMode = MARSHALL_FULL_COPY;
            }

            u64 baseSize = 0, marshalledSize = 0;
            ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, marshallMode);
            u64 fullSize = baseSize + marshalledSize;

            ocrPolicyMsg_t * msgCpy = self->fcts.pdMalloc(self, fullSize);
            ADebug(AllocDebugAllPD, "hc-dist-policy.c/hcDistProcessMessage(msgCpy) "
                   "pdMalloc(%ld), addr=%p\n", fullSize, msgCpy);
            initializePolicyMessage(msgCpy, fullSize);
            ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)msgCpy, marshallMode);

            // Package msgCpy in an event
            pdEvent_t *event;
            RESULT_ASSERT(pdCreateEvent(self, &event, PDEVT_TYPE_MSG, 0), ==, 0);
            event->properties |= PDEVT_GC; // We need to garbage collect this event
                                           // when the strand is over
            event->properties |= PDEVT_DESTROY_DEEP; // We copied the message as well so
                                                     // it needs to be freed with the event
            ((pdEventMsg_t*)event)->msg = msgCpy;
            ((pdEventMsg_t*)event)->properties = COMM_ONE_WAY; // This is a "one-way" message
                                                               // as no response is going to
                                                               // be contained in this event
            pdMarkReadyEvent(self, event);
            pdStrand_t * msgStrand;
            RESULT_ASSERT(pdGetNewStrand(self, &msgStrand, self->strandTables[PDSTT_COMM-1], event, 0 /*unused*/), ==, 0);
            pdAction_t * processAction = pdGetProcessMessageAction(NP_COMM);
            // Clear the hold here because we are not going to be waiting on anything
            // The created event will be destroyed by the communication layer
            RESULT_ASSERT(pdEnqueueActions(self, msgStrand, 1, &processAction, true/*clear hold*/), ==, 0);
            RESULT_ASSERT(pdUnlockStrand(msgStrand), ==, 0);
#else
            // one-way request, several options:
            // - make a copy in sendMessage (current strategy)
            // - submit the message to be sent and wait for delivery
            res = self->fcts.sendMessage(self, msg->destLocation, msg, NULL, sendProp);
#endif
            // msg has been copied so we can update its returnDetail regardless
            // This is open for debate here #932
            if (sendProp == 0) {
                setReturnDetail(msg, res);
            }

            //NOTE: For PD_MSG_GUID_METADATA_CLONE we do not need to set OCR_EBUSY in the
            //      message's returnDetail field as being the PD issuing the call we can
            //      rely on the PEND return status.
        }
    } else {
        // Local PD handles the message. msg's destination is curLoc
        //NOTE: 'msg' may be coming from 'self' or from a remote PD. It can
        // either be a request (that may need a response) or a response.

        bool reqResponse = !!(msg->type & PD_MSG_REQ_RESPONSE); // for correctness check
        ocrLocation_t orgSrcLocation __attribute__((unused)) = msg->srcLocation; // for correctness check
        ocrPolicyDomainHcDist_t * pdSelfDist = (ocrPolicyDomainHcDist_t *) self;

        //BUG #587: check if buffer is too small, can try to arrange something so that
        //disambiguation is done at compile time (we already know message sizes)
        ocrPolicyMsg_t * msgInCopy = NULL;
        if (reqResponse && (msg->srcLocation != self->myLocation)) {
            u64 baseSizeIn = ocrPolicyMsgGetMsgBaseSize(msg, true);
            u64 baseSizeOut = ocrPolicyMsgGetMsgBaseSize(msg, false);
            bool resizeNeeded = ((baseSizeIn < baseSizeOut) && (msg->bufferSize < baseSizeOut));
            if (resizeNeeded) {
                msgInCopy = msg;
                DPRINTF(DEBUG_LVL_VVERB,"Buffer resize for response of message type 0x%"PRIx64"\n",
                                        (msgInCopy->type & PD_MSG_TYPE_ONLY));
                msg = self->fcts.pdMalloc(self, baseSizeOut);
                ADebug(AllocDebugAllPD, "hc-dist-policy.c/hcDistProcessMessage(msg) "
                       "pdMalloc(%ld), addr=%p\n", baseSizeOut, msg);
                initializePolicyMessage(msg, baseSizeOut);
                ocrPolicyMsgMarshallMsg(msgInCopy, baseSizeIn, (u8*)msg, MARSHALL_DUPLICATE);
            }
        }
        u32 msgType = msg->type & PD_MSG_TYPE_ONLY;
        // NOTE: It is important to ensure the base processMessage call doesn't
        // store any pointers read from the request message
        ret = pdSelfDist->baseProcessMessage(self, msg, isBlocking);

        if (msgInCopy && (ret != OCR_EPEND) && ((msg->type & PD_MSG_TYPE_ONLY) != PD_MSG_DB_ACQUIRE)) {
            // The original message is now contained in msgInCopy. Since we use the new
            // message to fulfil the communication we need to do extra work to clean up
            // the original message. Hence, deallocate copy here unless the calling context
            // is responsible for it.
            self->fcts.pdFree(self, msgInCopy);
        }

        // Here, 'msg' content has potentially changed if a response was required
        // If msg's destination is not the current location anymore, it means we were
        // processing an incoming request from another PD. Send the response now.

        // Special case until we process handles instead of messages
        if ((msgType & PD_MSG_TYPE_ONLY) == PD_MSG_METADATA_COMM) {
            if (ret == OCR_EPEND) {
                // When pending, the MD takes the responsibility of freeing the message
                // which leads to race conditions from now on so avoid touching the msg again.
                postProcess = false;
            } else {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
                if (PD_MSG_FIELD_I(response) != NULL) {
                    // This is reading from the request not the response but it's ok for debugging
                    DPRINTF(DEBUG_LVL_WARN, "Sending MD_COMM to %"PRIu64" mode=%"PRIu64" for "GUIDF"\n", msg->srcLocation, PD_MSG_FIELD_I(mode), GUIDA(PD_MSG_FIELD_I(guid)));
                    msg = PD_MSG_FIELD_I(response);
                    // The request is deallocated in processRequestEdt as we're processing a one-way message
                    reqResponse = true; //TODO this is part of the hack for the response field of PD_MSG_METADATA_COMM
                }
#undef PD_MSG
#undef PD_TYPE
            }
        }

        if ((ret != OCR_EPEND) && (msg->destLocation != curLoc)) {
            // For now a two-way is always between the same pair of src/dst.
            // Cannot answer to someone else on behalf of the original sender.
            ASSERT(msg->destLocation == orgSrcLocation);
            ASSERT(reqResponse); // double check not trying to answer when we shouldn't

            //IMPL: Because we are processing a two-way originating from another PD,
            // the message buffer is necessarily managed by the runtime (as opposed
            // to be on the user call stack calling in its PD).
            // Hence, we post the response as a one-way, persistent and no handle.
            // The message will be deallocated on one-way call completion.
            u32 sendProp = PERSIST_MSG_PROP;
            DPRINTF(DEBUG_LVL_VVERB, "Send response to %"PRIu64" type=%"PRIx64" after local processing of msg\n", msg->destLocation, msg->type & PD_MSG_TYPE_ONLY);
            ASSERT(msg->type & PD_MSG_RESPONSE);
            ASSERT((msg->type & PD_MSG_TYPE_ONLY) != PD_MSG_MGT_MONITOR_PROGRESS);
            switch(msg->type & PD_MSG_TYPE_ONLY) {
            case PD_MSG_WORK_CREATE:
            {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_WORK_CREATE
                // Here we have to extract the output event's GUID from the EDT's metadata pointer
                // if the response is OCR_EGUIDEXISTS
                if(PD_MSG_FIELD_O(returnDetail) == OCR_EGUIDEXISTS) {
                    PD_MSG_FIELD_IO(outputEvent.guid) = ((ocrTask_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr)))->outputEvent;
                }
#undef PD_MSG
#undef PD_TYPE
                break;
            }
            case PD_MSG_GUID_METADATA_CLONE:
            {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
                sendProp |= ASYNC_MSG_PROP;
#undef PD_MSG
#undef PD_TYPE
                break;
            }
            case PD_MSG_METADATA_COMM:
            {
                //BUG #190
                ASSERT(msg->type & PD_MSG_RESPONSE);
                // This is necessary to tag the response as a one-way
                sendProp |= ASYNC_MSG_PROP;
                break;
            }
            default: {
                ASSERT(msg->type & PD_MSG_RESPONSE);
            }
            }
            // Do the post processing BEFORE sending otherwise the msg destruction is concurrent
            hcDistSchedNotifyPostProcessMessage(self, msg);
            postProcess = false;
#ifdef UTASK_COMM2
            // Ideally, we would still have the event/strand that called this
            // but for now, we create another event and say that the next action on it
            // is to send the message. This will be a COMM_ONE_WAY message because
            // the event can then be destroyed since we are not going to do anything
            // with it.
            {
                pdEvent_t *event;
                RESULT_ASSERT(pdCreateEvent(self, &event, PDEVT_TYPE_MSG, 0), ==, 0);
                event->properties |= PDEVT_GC;
                event->properties |= PDEVT_DESTROY_DEEP;
                ((pdEventMsg_t*)event)->msg = msg;
                // This is a "one-way" message because this is a response and
                // we don't do anything with the response to the response
                ((pdEventMsg_t*)event)->properties = COMM_ONE_WAY;
                pdMarkReadyEvent(self, event);
                pdStrand_t * msgStrand;
                RESULT_ASSERT(
                    pdGetNewStrand(self, &msgStrand, self->strandTables[PDSTT_COMM-1], event, 0 /*unused*/),
                    ==, 0);
                pdAction_t * processAction = pdGetProcessMessageAction(NP_COMM);
                // Clear the hold here because we are not going to be waiting on anything
                RESULT_ASSERT(
                    pdEnqueueActions(self, msgStrand, 1, &processAction, true/*clear hold*/),
                    ==, 0);
                RESULT_ASSERT(pdUnlockStrand(msgStrand), ==, 0);
            }
#else
            // Send the response message
            self->fcts.sendMessage(self, msg->destLocation, msg, NULL, sendProp);
#endif
        }
    }

    if (postProcess) { // Temporary workaround: See Bug #936
        hcDistSchedNotifyPostProcessMessage(self, msg);
    }

    return ret;
}

u8 hcDistProcessEvent(ocrPolicyDomain_t* self, pdEvent_t **evt, u32 idx) {
    // Simple version to test out micro tasks for now. This just executes a blocking
    // call to the regular process message and returns NULL
    ASSERT(idx == 0);
    ASSERT((evt != NULL) && (*evt != NULL));
    ASSERT(((*evt)->properties & PDEVT_TYPE_MASK) == PDEVT_TYPE_MSG);
    pdEventMsg_t *evtMsg = (pdEventMsg_t*)(*evt);
    ocrPolicyMsg_t * msg = evtMsg->msg;

    // This is called in two cases:
    //   - if we actually need to process a message (for example, we had a response or something)
    //   - if we need to send a message (the worker is now the COMM worker) and we therefore
    //     need to directly send the message (we skip the comm-API as that will probably go away)
    if(msg->destLocation != self->myLocation) {
        DPRINTF(DEBUG_LVL_VERB, "Found a message to be sent to 0x%"PRIx64" type=0x%"PRIx32"\n", msg->destLocation, msg->type);
        ocrWorker_t *worker;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        u32 id = worker->id;
        RESULT_ASSERT(self->commApis[id]->commPlatform[0].fcts.sendMessageMT(
            &(self->commApis[id]->commPlatform[0]), evt, /*status evt*/NULL, 0), ==, 0);
    } else if (msg->srcLocation != self->myLocation) {
        DPRINTF(DEBUG_LVL_VERB, "Process message from 0x%"PRIx64" type=0x%"PRIx32"\n", msg->srcLocation, msg->type);
        processCommEvent(self, evt, idx);
        *evt = NULL;
    } else {
        // HACK: For now, this path should not be exercised
#ifndef ENABLE_OCR_API_DEFERRABLE
        ASSERT(0);
#endif
        //TODO-DEFERRED Copy paste from hc-policy.c
        ocrWorker_t * worker;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        // Check if we need to restore a context in which the MT is supposed to execute.
        // Can typically happen in deferred execution where the EDT user code is done
        // but there still is pending OCR operations in the form of MT to execute.
        ocrTask_t * curTask = worker->curTask;
        if (evtMsg->ctx) {
            worker->curTask = evtMsg->ctx;
        }
        DPRINTF(DEBUG_LVL_WARN, "hcDistProcessEvent executing msg of type 0x%"PRIx64"\n", msg->type & PD_MSG_TYPE_ONLY);
        hcDistProcessMessage(self, evtMsg->msg, true);
        worker->curTask = curTask;
        *evt = NULL;
    }
    return 0;
}

u8 hcDistPdSwitchRunlevel(ocrPolicyDomain_t *self, ocrRunlevel_t runlevel, u32 properties) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*) self;

    if((runlevel == RL_USER_OK) && RL_IS_LAST_PHASE_DOWN(self, RL_USER_OK, rself->rlSwitch.nextPhase)) {
        ASSERT(rself->rlSwitch.runlevel == runlevel);
        // The local shutdown is completed.
        // Notify neighbors PDs and stall the phase change
        // until we got acknoledgements from all of them.
        // Notify other PDs the user runlevel has completed here
        getCurrentEnv(&self, NULL, NULL, NULL);
        u32 i = 0;
        while(i < self->neighborCount) {
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: loop shutdown neighbors[%"PRId32"] is %"PRId32"\n", i, (u32) self->neighbors[i]);
            PD_MSG_STACK(msgShutdown);
            getCurrentEnv(NULL, NULL, NULL, &msgShutdown);
        #define PD_MSG (&msgShutdown)
        #define PD_TYPE PD_MSG_MGT_RL_NOTIFY
            msgShutdown.destLocation = self->neighbors[i];
            msgShutdown.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(runlevel) = RL_COMPUTE_OK;
            PD_MSG_FIELD_I(properties) = RL_REQUEST | RL_BARRIER | RL_TEAR_DOWN;
            PD_MSG_FIELD_I(errorCode) = self->shutdownCode;
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: send shutdown msg to %"PRId32"\n", (u32) msgShutdown.destLocation);
            RESULT_ASSERT(self->fcts.processMessage(self, &msgShutdown, true), ==, 0);
        #undef PD_MSG
        #undef PD_TYPE
            i++;
        }
        // Consider the PD to have reached its local quiescence.
        // This code is concurrent with receiving notifications
        // from other PDs and must detect if it is the last
        ocrPolicyDomainHcDist_t * dself = (ocrPolicyDomainHcDist_t *) self;
        // incr the shutdown counter (compete with processMessage PD_MSG_MGT_RL_NOTIFY)
        u32 oldAckValue = hal_xadd32(&dself->shutdownAckCount, 1);
        if (oldAckValue != (self->neighborCount)) {
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: reached local quiescence. To be resumed when distributed shutdown is done\n");
            // If it is not the last one to increment do not fall-through
            // The switch runlevel will be called whenever we get the last
            // shutdown ack.
            return 0;
        } else {
            // Last shutdown acknowledgement, resume the runlevel switch
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: distributed shutdown is done. Process with local shutdown\n");
            return dself->baseSwitchRunlevel(self, rself->rlSwitch.runlevel, rself->rlSwitch.properties);
        }
    } else { // other runlevels than RL_USER_OK
        ocrPolicyDomainHcDist_t * dself = (ocrPolicyDomainHcDist_t *) self;
        u8 res = dself->baseSwitchRunlevel(self, runlevel, properties);
        if (properties & RL_BRING_UP) {
            if (runlevel == RL_CONFIG_PARSE) {
                // In distributed the shutdown protocol requires three phases
                // for the RL_USER_OK TEAR_DOWN. The communication worker must be
                // aware of those while computation workers can be generic and rely
                // on the switchRunlevel/callback mecanism.
                // Because we want to keep the computation worker implementation more generic
                // we request phases directly from here through the coalesced number of phases at slot 0.
                RL_ENSURE_PHASE_DOWN(self, RL_USER_OK, 0, 3);
            }
        } else {
            ASSERT(properties & RL_TEAR_DOWN);
        }
        return res;
    }
}

u8 hcDistPdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {
    ocrWorker_t * worker;
#ifdef UTASK_COMM2
    ASSERT(0); // Should use micro-tasks to communicate
#endif
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ASSERT(((s32)target) > -1);
    ASSERT(message->srcLocation == self->myLocation);
    ASSERT(message->destLocation != self->myLocation);
    u32 id = worker->id;
    u8 ret = self->commApis[id]->fcts.sendMessage(self->commApis[id], target, message, handle, properties);
    return ret;
}

u8 hcDistPdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
#ifdef UTASK_COMM2
    ASSERT(0); // Should use micro-tasks to communicate
#endif
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u32 id = worker->id;
    u8 ret = self->commApis[id]->fcts.pollMessage(self->commApis[id], handle);
    return ret;
}

u8 hcDistPdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
#ifdef UTASK_COMM2
    ASSERT(0); // Should use micro-tasks to communicate
#endif
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u32 id = worker->id;
    u8 ret = self->commApis[id]->fcts.waitMessage(self->commApis[id], handle);
    return ret;
}

u8 hcDistPdSendMessageMT(ocrPolicyDomain_t* self, pdEvent_t **inOutEvent,
                         pdEvent_t **statusEvent, u32 idx) {
#ifdef UTASK_COMM2
    ASSERT(0); // Should use micro-tasks to communicate
#endif
    return OCR_ENOTSUP;
}

u8 hcDistPdPollMessageMT(ocrPolicyDomain_t *self, pdEvent_t **outEvent, u32 idx) {
    // This is used by the comm worker to look for work. We just forward
    // directly to the comm platform
    ocrWorker_t *worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u32 id = worker->id;
    return self->commApis[id]->commPlatform[0].fcts.pollMessageMT(
        &(self->commApis[id]->commPlatform[0]), outEvent, idx);
}

u8 hcDistPdWaitMessageMT(ocrPolicyDomain_t *self,  pdEvent_t **outEvent, u32 idx) {
    // This is used by the comm worker to look for work. We just forward
    // directly to the comm platform
    ocrWorker_t *worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u32 id = worker->id;
    return self->commApis[id]->commPlatform[0].fcts.waitMessageMT(
        &(self->commApis[id]->commPlatform[0]), outEvent, idx);
}

ocrPolicyDomain_t * newPolicyDomainHcDist(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrParamList_t *perInstance) {

    ocrPolicyDomainHcDist_t * derived = (ocrPolicyDomainHcDist_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainHcDist_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;

#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, statsObject, base, perInstance);
#else
    factory->initialize(factory, base, perInstance);
#endif
    return base;
}

void initializePolicyDomainHcDist(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                  ocrStats_t *statsObject,
#endif
                                  ocrPolicyDomain_t *self, ocrParamList_t *perInstance) {
    ocrPolicyDomainFactoryHcDist_t * derivedFactory = (ocrPolicyDomainFactoryHcDist_t *) factory;
    // Initialize the base policy-domain
#ifdef OCR_ENABLE_STATISTICS
    derivedFactory->baseInitialize(factory, statsObject, self, perInstance);
#else
    derivedFactory->baseInitialize(factory, self, perInstance);
#endif
    ocrPolicyDomainHcDist_t * hcDistPd = (ocrPolicyDomainHcDist_t *) self;
    hcDistPd->baseProcessMessage = derivedFactory->baseProcessMessage;
    hcDistPd->baseSwitchRunlevel = derivedFactory->baseSwitchRunlevel;
    hcDistPd->shutdownAckCount = 0;
}

static void destructPolicyDomainFactoryHcDist(ocrPolicyDomainFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrPolicyDomainFactory_t * newPolicyDomainFactoryHcDist(ocrParamList_t *perType) {
    ocrPolicyDomainFactory_t * baseFactory = newPolicyDomainFactoryHc(perType);
    ocrPolicyDomainFcts_t baseFcts = baseFactory->policyDomainFcts;

    ocrPolicyDomainFactoryHcDist_t* derived = (ocrPolicyDomainFactoryHcDist_t*) runtimeChunkAlloc(sizeof(ocrPolicyDomainFactoryHcDist_t), NONPERSISTENT_CHUNK);
    ocrPolicyDomainFactory_t* derivedBase = (ocrPolicyDomainFactory_t*) derived;
    // Set up factory function pointers and members
    derivedBase->instantiate = newPolicyDomainHcDist;
    derivedBase->initialize = initializePolicyDomainHcDist;
    derivedBase->destruct =  destructPolicyDomainFactoryHcDist;
    derivedBase->policyDomainFcts = baseFcts;
    derived->baseInitialize = baseFactory->initialize;
    derived->baseProcessMessage = baseFcts.processMessage;
    derived->baseSwitchRunlevel = baseFcts.switchRunlevel;

    // specialize some of the function pointers
    derivedBase->policyDomainFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrRunlevel_t, u32), hcDistPdSwitchRunlevel);
    derivedBase->policyDomainFcts.processMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*,ocrPolicyMsg_t*,u8), hcDistProcessMessage);
    derivedBase->policyDomainFcts.processEvent = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, pdEvent_t**, u32), hcDistProcessEvent);
    derivedBase->policyDomainFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrLocation_t, ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                                   hcDistPdSendMessage);
    derivedBase->policyDomainFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcDistPdPollMessage);
    derivedBase->policyDomainFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcDistPdWaitMessage);
    derivedBase->policyDomainFcts.sendMessageMT = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, pdEvent_t **, pdEvent_t*, u32),
                                                            hcDistPdSendMessageMT);
    derivedBase->policyDomainFcts.pollMessageMT = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, pdEvent_t **, u32), hcDistPdPollMessageMT);
    derivedBase->policyDomainFcts.waitMessageMT = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, pdEvent_t **, u32), hcDistPdWaitMessageMT);

    baseFactory->destruct(baseFactory);
    return derivedBase;
}

#endif /* ENABLE_POLICY_DOMAIN_HC_DIST */
