
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

// This is in place of using the general purpose 'guidLocation' implementation that relies
// on PD_MSG_GUID_INFO. Since 'guidLocationShort' is extensively called we directly call
// the guid provider here
static inline u8 guidLocationShort(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t guid,
                              ocrLocation_t* locationRes) {
    return pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], guid.guid, locationRes);
}

static inline u8 checkLocationValidity(struct _ocrPolicyDomain_t * pd, ocrLocation_t loc) {
    u64 locID = ((u64)loc);
    return ((locID >= 0) && (locID <= pd->neighborCount));
}

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

#define PROCESS_MESSAGE_WITH_PROXY_DB_AND_RETURN \
    ocrPolicyDomainHcDist_t * pdSelfDist = (ocrPolicyDomainHcDist_t *) self; \
    ProxyDb_t * proxyDb = getProxyDb(self, PD_MSG_FIELD_IO(guid.guid), false); \
    ASSERT(proxyDb && proxyDb->db); \
    PD_MSG_FIELD_IO(guid.metaDataPtr) = proxyDb->db; \
    msg->destLocation = curLoc; \
    relProxyDb(self, proxyDb); \
    return pdSelfDist->baseProcessMessage(self, msg, isBlocking);

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

/****************************************************/
/* PROXY TEMPLATE MANAGEMENT                        */
/****************************************************/
//BUG #536: To be replaced by a general metadata cloning framework

extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

u8 createProcessRequestEdtDistPolicy(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid, u64 * paramv) {

    u32 paramc = 1;
    u32 depc = 0;
    u32 properties = GUID_PROP_TORECORD;
    ocrWorkType_t workType = EDT_RT_WORKTYPE;

    START_PROFILE(api_EdtCreate);
    PD_MSG_STACK(msg);
    u8 returnCode = 0;
    ocrTask_t *curEdt = NULL;
    getCurrentEnv(NULL, NULL, &curEdt, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_CREATE
    msg.type = PD_MSG_WORK_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(templateGuid.guid) = templateGuid;
    PD_MSG_FIELD_I(templateGuid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(outputEvent.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(outputEvent.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(paramv) = paramv;
    PD_MSG_FIELD_IO(paramc) = paramc;
    PD_MSG_FIELD_IO(depc) = depc;
    PD_MSG_FIELD_I(depv) = NULL;
    PD_MSG_FIELD_I(hint) = NULL_HINT;
    PD_MSG_FIELD_I(properties) = properties;
    PD_MSG_FIELD_I(workType) = workType;
    // This is a "fake" EDT so it has no "parent"
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(parentLatch.guid) = NULL_GUID;
    PD_MSG_FIELD_I(parentLatch.metaDataPtr) = NULL;
    returnCode = pd->fcts.processMessage(pd, &msg, true);
    if(returnCode) {
        DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Created processRequest EDT GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
        RETURN_PROFILE(returnCode);
    }

    RETURN_PROFILE(0);
#undef PD_MSG
#undef PD_TYPE
}

// Utility function to enqueue a waiter when the metadata is being fetch
// Impl will most likely move to runtime events
static u64 enqueueMdProxyWaiter(ocrPolicyDomain_t * pd, MdProxy_t * mdProxy, ocrPolicyMsg_t * msg) {
    MdProxyNode_t * node = (MdProxyNode_t *) pd->fcts.pdMalloc(pd, sizeof(MdProxyNode_t));
    node->msg = msg;
    u64 newValue = (u64) node;
    bool notSucceed = true;
    do {
        MdProxyNode_t * head = mdProxy->queueHead;
        if (head == REG_CLOSED) { // registration is closed
            break;
        }
        node->next = head;
        u64 curValue = (u64) head;
        u64 oldValue = hal_cmpswap64((u64*) &(mdProxy->queueHead), curValue, newValue);
        notSucceed = (oldValue != curValue);
    } while(notSucceed);

    if (notSucceed) { // registration has closed
        pd->fcts.pdFree(pd, node);
        // There must be a fence between the head CAS and the 'ptr'
        // assignment in the code setting that resolve the metadata pointer
        u64 val = (u64) mdProxy->ptr;
        ASSERT(val != 0);
        return val;
    }
    return 0;
}

//TODO-MD: This should become part of the GP API and allow to resolve the MD at various granularities/info
u8 resolveRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t * fatGuid,
                                ocrPolicyMsg_t * msg, bool isBlocking) {
    u64 val;
    MdProxy_t * mdProxy = NULL;
    // Check if known locally
    u8 res = pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], fatGuid->guid, &val, NULL, MD_FETCH, &mdProxy);
    if (val == 0) {
        ASSERT(res == OCR_EPEND);
        if (isBlocking) {
            // Busy-wait and return only when the metadata is resolved
            DPRINTF(DEBUG_LVL_VVERB,"Resolving metadata: enter busy-wait for blocking call\n");
            do {
                // Let the scheduler know we are blocked
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
                msg.type = PD_MSG_MGT_MONITOR_PROGRESS | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_I(monitoree) = NULL;
                PD_MSG_FIELD_IO(properties) = (0 | MONITOR_PROGRESS_COMM);
                RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
#undef PD_MSG
#undef PD_TYPE
                pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], fatGuid->guid, &val, NULL, MD_LOCAL, NULL);
            } while(val == 0);
            fatGuid->metaDataPtr = (void *) val;
            DPRINTF(DEBUG_LVL_VVERB,"Resolving metadata: exit busy-wait for blocking call\n");
        } else {
            ASSERT(mdProxy != NULL);
            ASSERT(msg != NULL);
            // Enqueue itself on the MD proxy, the caller will be rescheduled for execution.
            // Remember that we're still competing with the fetch operation's completion.
            // So we may actually not succeed enqueuing ourselves and be able to read the MD pointer.
            val = enqueueMdProxyWaiter(pd, mdProxy, msg);
            // Warning: At this point we cannot access the msg pointer anymore.
            // This code becomes concurrent with the continuation being invoked, possibly destroying 'msg'.
        }
    }
    fatGuid->metaDataPtr = (void *) val;
    return ((val) ? 0 : OCR_EPEND);
}

/****************************************************/
/* PROXY DATABLOCK MANAGEMENT                       */
/****************************************************/

//BUG #536: To be replaced by a general metadata cloning framework

/**
 * @brief State of a Proxy for a DataBlock
 */
typedef enum {
    PROXY_DB_CREATED,   /**< The proxy DB has been created and is registered in GUID provider */
    PROXY_DB_FETCH,     /**< The DB ptr is being fetch */
    PROXY_DB_RUN,       /**< The DB ptr is being used */
    PROXY_DB_RELINQUISH /**< The DB ptr is being released (possibly incuring Write-Back) */
} ProxyDbState_t;

//Default proxy DB internal queue size
#define PROXY_DB_QUEUE_SIZE_DEFAULT 4

/**
 * @brief Data-structure to store foreign DB information
 */
typedef struct {
    ProxyDbState_t state;
    u32 nbUsers;
    u32 refCount;
    lock_t lock;
    Queue_t * acquireQueue;
    u16 mode;
    u64 size;
    void * volatile ptr;
    u32 flags;
    ocrDataBlock_t *db;
} ProxyDb_t;

/**
 * @brief Allocate a proxy DB
 */
static ProxyDb_t * createProxyDb(ocrPolicyDomain_t * pd) {
    ProxyDb_t * proxyDb = pd->fcts.pdMalloc(pd, sizeof(ProxyDb_t));
    proxyDb->state = PROXY_DB_CREATED;
    proxyDb->nbUsers = 0;
    proxyDb->refCount = 0;
    proxyDb->lock = INIT_LOCK;
    proxyDb->acquireQueue = NULL;
    // Cached DB information
    proxyDb->mode = 0;
    proxyDb->size = 0;
    proxyDb->ptr = NULL;
    proxyDb->flags = 0;
    proxyDb->db = NULL;
    return proxyDb;
}

/**
 * @brief Reset a proxy DB.
 * Warning: This call does NOT reinitialize all of the proxy members !
 */
static void resetProxyDb(ProxyDb_t * proxyDb) {
    proxyDb->state = PROXY_DB_CREATED;
    proxyDb->mode = 0;
    proxyDb->flags = 0;
    proxyDb->nbUsers = 0;
    // DBs are not supposed to be resizable hence, do NOT reset
    // size and ptr so they can be reused in the subsequent fetch.
}

/**
 * @brief Lookup a proxy DB in the GUID provider.
 *        Increments the proxy's refCount by one.
 * @param dbGuid            The GUID of the datablock to look for
 * @param createIfAbsent    Create the proxy DB if not found.
 */
static ProxyDb_t * getProxyDb(ocrPolicyDomain_t * pd, ocrGuid_t dbGuid, bool createIfAbsent) {
    hal_lock(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
    ProxyDb_t * proxyDb = NULL;
    u64 val;
    pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dbGuid, &val, NULL, MD_LOCAL, NULL);
    if (val == 0) {
        if (createIfAbsent) {
            proxyDb = createProxyDb(pd);
            pd->guidProviders[0]->fcts.registerGuid(pd->guidProviders[0], dbGuid, (u64) proxyDb);
        } else {
            hal_unlock(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
            return NULL;
        }
    } else {
        proxyDb = (ProxyDb_t *) val;
    }
    proxyDb->refCount++;
    hal_unlock(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
    return proxyDb;
}

/**
 * @brief Release a proxy DB, decrementing its refCount counter by one.
 * Warning: This is different from releasing a datablock.
 */
static void relProxyDb(ocrPolicyDomain_t * pd, ProxyDb_t * proxyDb) {
    hal_lock(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
    proxyDb->refCount--;
    hal_unlock(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
}

/**
 * @brief Check if the proxy's DB mode is compatible with another DB mode
 * This call helps to determine when an acquire is susceptible to use the proxy DB
 * or if it is not and must handled at a later time.
 *
 * This implementation allows all reuse (RO/CONST/RW) but EW
 */
static bool isAcquireEligibleForProxy(ocrDbAccessMode_t proxyDbMode, ocrDbAccessMode_t acquireMode) {
    return ((proxyDbMode != DB_MODE_EW) && (acquireMode == proxyDbMode));
}

/**
 * @brief Enqueue an acquire message into the proxy DB for later processing.
 *
 * Warning: The caller must own the proxy DB internal's lock.
 */
static void enqueueAcquireMessageInProxy(ocrPolicyDomain_t * pd, ProxyDb_t * proxyDb, ocrPolicyMsg_t * msg) {
    // Ensure there's sufficient space in the queue
    if (proxyDb->acquireQueue == NULL) {
        proxyDb->acquireQueue = newBoundedQueue(pd, PROXY_DB_QUEUE_SIZE_DEFAULT);
    }
    if (queueIsFull(proxyDb->acquireQueue)) {
        proxyDb->acquireQueue = queueDoubleResize(proxyDb->acquireQueue, /*freeOld=*/true);
    }
    // Enqueue: Need to make a copy of the message since the acquires are two-way
    // asynchronous and the calling context may disappear later on.
    //BUG #587 Auto-serialization
    ocrPolicyMsg_t * msgCopy = (ocrPolicyMsg_t *) pd->fcts.pdMalloc(pd, sizeof(ocrPolicyMsg_t));
    initializePolicyMessage(msgCopy, sizeof(ocrPolicyMsg_t));
    hal_memCopy(msgCopy, msg, sizeof(ocrPolicyMsg_t), false);
    queueAddLast(proxyDb->acquireQueue, (void *) msgCopy);
}

/**
 * @brief Dequeue an acquire message from a message queue compatible with a DB access node.
 *
 * @param candidateQueue      The queue to dequeue from
 * @param acquireMode         The DB access mode the message should be compatible with
 *
 * Warning: The caller must own the proxy DB internal's lock.
 */
static Queue_t * dequeueCompatibleAcquireMessageInProxy(ocrPolicyDomain_t * pd, Queue_t * candidateQueue, ocrDbAccessMode_t acquireMode) {
    if ((candidateQueue != NULL) && (acquireMode != DB_MODE_EW)) {
        u32 idx = 0;
        Queue_t * eligibleQueue = NULL;
        // Iterate the candidate queue
        while(idx < candidateQueue->tail) {
            ocrPolicyMsg_t * msg = queueGet(candidateQueue, idx);
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
            if ((PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK) == acquireMode) {
                // Found a match
                if (eligibleQueue == NULL) {
                    eligibleQueue = newBoundedQueue(pd, PROXY_DB_QUEUE_SIZE_DEFAULT);
                }
                if (queueIsFull(eligibleQueue)) {
                    eligibleQueue = queueDoubleResize(eligibleQueue, /*freeOld=*/true);
                }
                // Add to eligible queue, remove from candidate queue
                queueAddLast(eligibleQueue, (void *) msg);
                queueRemove(candidateQueue, idx);
            } else {
                idx++;
            }
    #undef PD_MSG
    #undef PD_TYPE
        }
        return eligibleQueue;
    }
    return NULL;
}

/**
 * @brief Update an acquire message with information from a proxy DB
 */
static void updateAcquireMessage(ocrPolicyMsg_t * msg, ProxyDb_t * proxyDb) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
    PD_MSG_FIELD_O(ptr)  = proxyDb->ptr;
    PD_MSG_FIELD_O(size) = proxyDb->size;
    PD_MSG_FIELD_IO(properties) = proxyDb->flags;
#undef PD_MSG
#undef PD_TYPE
}

void getTemplateParamcDepc(ocrPolicyDomain_t * self, ocrFatGuid_t * fatGuid, u32 * paramc, u32 * depc) {
    // Need to deguidify the edtTemplate to know how many elements we're really expecting
    self->guidProviders[0]->fcts.getVal(self->guidProviders[0], fatGuid->guid,
                                        (u64*)&fatGuid->metaDataPtr, NULL, MD_LOCAL, NULL);
    ocrTaskTemplate_t * edtTemplate = (ocrTaskTemplate_t *) fatGuid->metaDataPtr;
    if(*paramc == EDT_PARAM_DEF) *paramc = edtTemplate->paramc;
    if(*depc == EDT_PARAM_DEF) *depc = edtTemplate->depc;
}

static void * acquireLocalDbOblivious(ocrPolicyDomain_t * pd, ocrGuid_t dbGuid) {
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, &curTask, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
    msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = dbGuid;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(edt.guid) = (curTask == NULL) ? NULL_GUID : curTask->guid;
    PD_MSG_FIELD_IO(edt.metaDataPtr) = curTask;
    PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
    PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
    PD_MSG_FIELD_IO(properties) = DB_PROP_RT_OBLIVIOUS; // Runtime acquire
    if(pd->fcts.processMessage(pd, &msg, true)) {
        ASSERT(false); // debug
        return NULL;
    }
    return PD_MSG_FIELD_O(ptr);
#undef PD_MSG
#undef PD_TYPE
}

//Notify scheduler of policy message before it is processed
static inline void hcDistSchedNotifyPreProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg) {
    if (msg->type & PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER)
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
    if (!(msg->type & PD_MSG_REQ_POST_PROCESS_SCHEDULER))
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
    if ((isBlocking == false) && (msg->type & PD_MSG_REQ_RESPONSE)) {
        ASSERT(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE)
            || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_GUID_METADATA_CLONE)
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
        if((PD_MSG_FIELD_I(paramv) == NULL) && (PD_MSG_FIELD_IO(paramc) != 0)) {
            // User error, paramc non zero but no parameters
            DPRINTF(DEBUG_LVL_WARN, "error: paramc is non-zero but paramv is NULL\n");
            ASSERT(false);
            PROCESS_MESSAGE_RETURN_NOW(self, OCR_EINVAL);
        }
        ocrFatGuid_t currentEdt = PD_MSG_FIELD_I(currentEdt);
        ocrFatGuid_t parentLatch = PD_MSG_FIELD_I(parentLatch);

        // The placer may have altered msg->destLocation
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
            /* The support for finish EDT and latch in distributed OCR
             * has the following implementation currently:
             * Whenever an EDT needs to be created on a remote node,
             * then a proxy latch is created on the remote node if
             * there is a parent latch to report to on the source node.
             * So, when a parent EDT creates a child EDT on a remote node,
             * the local latch is first incremented on the source node.
             * This latch will eventually be decremented through the proxy
             * latch on the remote node.
             */
            if (!(ocrGuidIsNull(parentLatch.guid))) {
                ocrLocation_t parentLatchLoc;
                RETRIEVE_LOCATION_FROM_GUID(self, parentLatchLoc, parentLatch.guid);
                if (parentLatchLoc == curLoc) {
                    //Check in to parent latch
                    PD_MSG_STACK(msg2);
                    getCurrentEnv(NULL, NULL, NULL, &msg2);
#define PD_MSG (&msg2)
#define PD_TYPE PD_MSG_DEP_SATISFY
                    // This message MUST be fully processed (i.e. parentLatch satisfied)
                    // before we return. Otherwise there's a race between this registration
                    // and the current EDT finishing.
                    msg2.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
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
                } // else, will create a proxy event at current location
            }
        }

        if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
            // we're receiving a message
            DPRINTF(DEBUG_LVL_VVERB, "WORK_CREATE: received request from %"PRIu64"\n", msg->srcLocation);
            if (!(ocrGuidIsNull(parentLatch.guid))) {
                //Create a proxy latch in current node
                /* On the remote side, a proxy latch is first created and a dep
                 * is added to the source latch. Within the remote node, this
                 * proxy latch becomes the new parent latch for the EDT to be
                 * created inside the remote node.
                 */
                PD_MSG_STACK(msg2);
                getCurrentEnv(NULL, NULL, NULL, &msg2);
#define PD_MSG (&msg2)
#define PD_TYPE PD_MSG_EVT_CREATE
                msg2.type = PD_MSG_EVT_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(currentEdt) = currentEdt;
#ifdef ENABLE_EXTENSION_PARAMS_EVT
                PD_MSG_FIELD_I(params) = NULL;
#endif
                PD_MSG_FIELD_I(properties) = 0;
                PD_MSG_FIELD_I(type) = OCR_EVENT_LATCH_T;
                RESULT_PROPAGATE(self->fcts.processMessage(self, &msg2, true));

                ocrFatGuid_t latchFGuid = PD_MSG_FIELD_IO(guid);
#undef PD_TYPE
#define PD_TYPE PD_MSG_DEP_ADD
                msg2.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
                PD_MSG_FIELD_IO(properties) = DB_MODE_CONST; // not called from add-dependence
                PD_MSG_FIELD_I(source) = latchFGuid;
                PD_MSG_FIELD_I(dest) = parentLatch;
                PD_MSG_FIELD_I(currentEdt) = currentEdt;
                PD_MSG_FIELD_I(slot) = OCR_EVENT_LATCH_DECR_SLOT;
                RESULT_PROPAGATE(self->fcts.processMessage(self, &msg2, true));
#undef PD_MSG
#undef PD_TYPE

#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                PD_MSG_FIELD_I(parentLatch) = latchFGuid;
#undef PD_MSG
#undef PD_TYPE
            }
        }
        break;
    }
    case PD_MSG_METADATA_COMM:
    {
        //Fall-through to send or base PD process
        break;
    }
    case PD_MSG_DB_CREATE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        // The placer may have altered msg->destLocation
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
            // Turn the call into a blocking call
            if (kind == OCR_GUID_EVENT_CHANNEL) {
                msg->type |= PD_MSG_REQ_RESPONSE;
                isBlocking = true;
            }
        }
#endif
#endif
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
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DB_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_FREE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_FREE
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
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
            if (PD_MSG_FIELD_I(type) == MD_CLONE) {
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
                    //       the guid is not available
                    // If it's a non-blocking processing, will set the returnDetail to busy after the request is sent out
#ifdef OCR_ASSERT
                    u64 val;
                    self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL, MD_LOCAL, NULL);
                    ASSERT(val == 0);
#endif
                }
            } else { // MD_MOVE request
                // In current implementation this type of message never leaves the current PD
                ASSERT(msg->srcLocation == msg->destLocation);
                ASSERT(msg->srcLocation == curLoc);
                // Local PD needs to inspect the MD_MOVE operation and take appropriate actions
                // which include notifying relevant PD modules and invoke lower-level MD_COMM APIs
            }
        }

        if ((msg->destLocation == curLoc) && (msg->srcLocation != curLoc) && (msg->type & PD_MSG_RESPONSE)) {
            // Incoming response to a MD_CLONE request posted earlier
            ocrGuidKind tkind;
            self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &tkind);
            if (tkind == OCR_GUID_EDT_TEMPLATE) {
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for template="GUIDF")\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                // Incoming response to an asynchronous metadata clone
                u64 metaDataSize = sizeof(ocrTaskTemplateHc_t) + (sizeof(u64) * OCR_HINT_COUNT_EDT_HC);
                void * metaDataPtr = self->fcts.pdMalloc(self, metaDataSize);
                ASSERT(PD_MSG_FIELD_IO(guid.metaDataPtr) != NULL);
                ASSERT(PD_MSG_FIELD_O(size) == metaDataSize);
                hal_memCopy(metaDataPtr, PD_MSG_FIELD_IO(guid.metaDataPtr), metaDataSize, false);
                void * base = PD_MSG_FIELD_IO(guid.metaDataPtr);
                ocrTaskTemplateHc_t * tpl = (ocrTaskTemplateHc_t *) metaDataPtr;
                if (tpl->hint.hintVal != NULL) {
                    tpl->hint.hintVal  = (u64*)((u64)base + sizeof(ocrTaskTemplateHc_t));
                }
                self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), (u64) metaDataPtr);
                PROCESS_MESSAGE_RETURN_NOW(self, 0);
            }
#ifdef ENABLE_EXTENSION_LABELING
            else if (tkind == OCR_GUID_GUIDMAP) {
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for guidMap="GUIDF")\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                ocrGuidMap_t * mapOrg = (ocrGuidMap_t *) PD_MSG_FIELD_IO(guid.metaDataPtr);
                u64 metaDataSize = ((sizeof(ocrGuidMap_t) + sizeof(s64) - 1) & ~(sizeof(s64)-1)) + mapOrg->numParams*sizeof(s64);
                void * metaDataPtr = self->fcts.pdMalloc(self, metaDataSize);
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
                hal_memCopy(metaDataPtr, PD_MSG_FIELD_IO(guid.metaDataPtr), metaDataSize, false);
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
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "GUID_DESTROY: target is %"PRId32"\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
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
        if (msg->type & PD_MSG_REQUEST) {
            ASSERT(checkLocationValidity(self, PD_MSG_FIELD_IO(destLoc)));
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO)
            // Send/Receive to/from remote or local processing, all fall-through
            if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
                // Incoming acquire request
                // The DB MUST be local to this node (that's the acquire is sent to this PD)
                // Need to resolve the DB metadata before handing the message over to the base PD.
                // The sender didn't know about the metadataPtr, receiver does.
                u64 val;
                self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL, MD_LOCAL, NULL);
                ASSERT_BLOCK_BEGIN(val != 0)
                DPRINTF(DEBUG_LVL_WARN, "User-level error detected: DB acquire failed for DB "GUIDF". It most likely has already been destroyed\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                ASSERT_BLOCK_END
                PD_MSG_FIELD_IO(guid.metaDataPtr) = (void *) val;
                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Incoming request for DB GUID "GUIDF" with properties=0x%"PRIx32"\n",
                        GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties));
                // Fall-through to local processing
            }
            if ((msg->srcLocation == curLoc) && (msg->destLocation != curLoc)) {
                if (msg->type & PD_MSG_LOCAL_PROCESS) { //BUG #162 - This is a workaround until metadata cloning
                    DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE local processing: DB GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                    PROCESS_MESSAGE_WITH_PROXY_DB_AND_RETURN
                }
                // Outgoing acquire request
                ProxyDb_t * proxyDb = getProxyDb(self, PD_MSG_FIELD_IO(guid.guid), true);
                hal_lock(&(proxyDb->lock)); // lock the db
                switch(proxyDb->state) {
                    case PROXY_DB_CREATED:
                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing request for DB GUID "GUIDF" with properties=0x%"PRIx32", creation fetch\n",
                                GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties));
                        // The proxy has just been created, need to fetch the DataBlock
                        PD_MSG_FIELD_IO(properties) |= DB_FLAG_RT_FETCH;
                        proxyDb->state = PROXY_DB_FETCH;
                    break;
                    case PROXY_DB_RUN:
                        // The DB is already in use locally
                        // Check if the acquire is compatible with the current usage
                        if (isAcquireEligibleForProxy(proxyDb->mode, (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK))) {
                            DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing request for DB GUID "GUIDF" with properties=0x%"PRIx32", intercepted for local proxy DB\n",
                                    GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties));
                            //Use the local cached version of the DB
                            updateAcquireMessage(msg, proxyDb);
                            proxyDb->nbUsers++;
                            //Granted access to the DB through the proxy. In that sense the request
                            //has been processed and is now a response to be returned to the caller.
                            msg->type = PD_MSG_RESPONSE;
                            msg->destLocation = curLoc; // optional, doing it to be consistent
                            PD_MSG_FIELD_O(returnDetail) = 0;
                            // No need to fall-through for local processing, proxy DB is used.
                            hal_unlock(&(proxyDb->lock));
                            relProxyDb(self, proxyDb);
                            PROCESS_MESSAGE_RETURN_NOW(self, 0);
                        } // else, not eligible to use the proxy, must enqueue the message
                        //WARN: fall-through is intentional
                    case PROXY_DB_FETCH:
                    case PROXY_DB_RELINQUISH:
                        //WARN: Do NOT move implementation: 'PROXY_DB_RUN' falls-through here
                        // The proxy is in a state requiring stalling outgoing acquires.
                        // The acquire 'msg' is copied and enqueued.
                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing request for DB GUID "GUIDF" with properties=0x%"PRIx32", enqueued\n",
                                GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties));
                        enqueueAcquireMessageInProxy(self, proxyDb, msg);
                        // Inform caller the acquire is pending.
                        hal_unlock(&(proxyDb->lock));
                        relProxyDb(self, proxyDb);
                        //NOTE: Important to set return values correctly since the acquire is not done yet !
                        // Here we set the returnDetail of the original message (not the enqueued copy)
                        PD_MSG_FIELD_O(returnDetail) = OCR_EBUSY;
                        PROCESS_MESSAGE_RETURN_NOW(self, OCR_EPEND);
                    break;
                    default:
                        ASSERT(false && "Unsupported proxy DB state");
                    // Fall-through to send the outgoing message
                }
                hal_unlock(&(proxyDb->lock));
                relProxyDb(self, proxyDb);
            }
        } else { // DB_ACQUIRE response
            ASSERT(msg->type & PD_MSG_RESPONSE);
            ASSERT(checkLocationValidity(self, PD_MSG_FIELD_IO(destLoc)));
            msg->destLocation = PD_MSG_FIELD_IO(destLoc);
            if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
                // Incoming acquire response
                ocrGuid_t dbGuid = PD_MSG_FIELD_IO(guid.guid);
                ProxyDb_t * proxyDb = getProxyDb(self, dbGuid, false);
                // Cannot receive a response to an acquire if we don't have a proxy
                ASSERT(proxyDb != NULL);
                hal_lock(&(proxyDb->lock)); // lock the db
                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Incoming response for DB GUID "GUIDF" with properties=0x%"PRIx32"\n",
                        GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties));
                switch(proxyDb->state) {
                    case PROXY_DB_FETCH:
                    {
                        // Processing an acquire response issued in the fetch state
                        // Update message properties
                        PD_MSG_FIELD_IO(properties) &= ~DB_FLAG_RT_FETCH;
                        //BUG #587 double check but I think we don't need the WB flag anymore since we have the mode
                        bool doWriteBack = !((PD_MSG_FIELD_IO(properties) & DB_MODE_RO) || (PD_MSG_FIELD_IO(properties) & DB_MODE_CONST) ||
                                             (PD_MSG_FIELD_IO(properties) & DB_PROP_SINGLE_ASSIGNMENT));
                        if (doWriteBack) {
                            PD_MSG_FIELD_IO(properties) |= DB_FLAG_RT_WRITE_BACK;
                        }
                        // Try to double check that across acquires the DB size do not change
                        ASSERT((proxyDb->size != 0) ? (proxyDb->size == PD_MSG_FIELD_O(size)) : true);

                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: caching data copy for DB GUID "GUIDF" size=%"PRIu64" \n",
                            GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_O(size));

                        // update the proxy DB
                        ASSERT(proxyDb->nbUsers == 0);
                        proxyDb->nbUsers++; // checks in as a proxy user
                        proxyDb->state = PROXY_DB_RUN;
                        proxyDb->mode = (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK);
                        proxyDb->size = PD_MSG_FIELD_O(size);
                        proxyDb->flags = PD_MSG_FIELD_IO(properties);
                        // Deserialize the data pointer from the message
                        // The message ptr is set to the message payload but we need
                        // to make a copy since the message will be deallocated later on.
                        void * newPtr = proxyDb->ptr; // See if we can reuse the old pointer
                        if (newPtr == NULL) {
                            newPtr = self->fcts.pdMalloc(self, proxyDb->size);
                        }
                        void * msgPayloadPtr = PD_MSG_FIELD_O(ptr);
                        hal_memCopy(newPtr, msgPayloadPtr, proxyDb->size, false);
                        proxyDb->ptr = newPtr;
                        // Update message to be consistent, but no calling context should need to read it.
                        PD_MSG_FIELD_O(ptr) = proxyDb->ptr;
                        if (proxyDb->db != NULL && !ocrGuidIsEq(proxyDb->db->guid, dbGuid)) {
                            ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.destruct(proxyDb->db);
                            proxyDb->db = NULL;
                        }
                        if (proxyDb->db == NULL) {
                            ocrFatGuid_t tGuid;
                            RESULT_ASSERT(((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->instantiate(
                                              ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx])), &tGuid, self->allocators[0]->fguid, self->fguid,
                                              proxyDb->size, proxyDb->ptr, NULL_HINT, DB_PROP_RT_PROXY, NULL), ==, 0);
                            proxyDb->db = (ocrDataBlock_t*)tGuid.metaDataPtr;
                            proxyDb->db->guid = dbGuid;
                        }

                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: caching data copy for DB GUID "GUIDF" ptr=%p size=%"PRIu64" flags=0x%"PRIx32"\n",
                            GUIDA(PD_MSG_FIELD_IO(guid.guid)), proxyDb->ptr, proxyDb->size, proxyDb->flags);
                        // Scan queue for compatible acquire that could use this cached proxy DB
                        Queue_t * eligibleQueue = dequeueCompatibleAcquireMessageInProxy(self, proxyDb->acquireQueue, proxyDb->mode);

                        // Iterate the queue and process pending acquire messages:
                        // Now the proxy state is RUN. All calls to process messages for
                        // eligible acquires will succeed in getting and using the cached data.
                        // Also note that the current acquire being checked in (proxy's count is one)
                        // ensures the proxy stays in RUN state when the current worker will
                        // fall-through to local processing.

                        if (eligibleQueue != NULL) {
                            u32 idx = 0;
                            // Update the proxy DB counter once (all subsequent acquire MUST be successful or there's a bug)
                            proxyDb->nbUsers += queueGetSize(eligibleQueue);
                            while(idx < queueGetSize(eligibleQueue)) {
                                // Consider msg is a response now
                                ocrPolicyMsg_t * msg = (ocrPolicyMsg_t *) queueGet(eligibleQueue, idx);
                                //NOTE: if we were to cache the proxyDb info we could release the proxy
                                //lock before this loop and allow for more concurrency. Although we would
                                //not have a pointer to the proxy, we would have one to the DB ptr data.
                                //I'm not sure if that means we're breaking the refCount abstraction.
                                updateAcquireMessage(msg, proxyDb);
                                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: dequeued eligible acquire for DB GUID "GUIDF" with properties=0x%"PRIx32"\n",
                                    GUIDA(PD_MSG_FIELD_IO(guid.guid)), proxyDb->flags);
                                // The acquire message had been processed once and was enqueued.
                                // Now it is processed 'again' but immediately succeeds in acquiring
                                // the cached data from the proxy and potentially iterates the acquire
                                // frontier of the EDT that originally called acquire.

                                // For the frontier to be iterated we need to directly call the base implementation
                                // and treat this request as a response.
                                msg->type &= ~PD_MSG_REQUEST;
                                msg->type &= ~PD_MSG_REQ_RESPONSE;
                                msg->type |= PD_MSG_RESPONSE;
                                msg->srcLocation = curLoc;
                                msg->destLocation = curLoc;
                                ocrPolicyDomainHcDist_t * pdSelfDist = (ocrPolicyDomainHcDist_t *) self;

                                // This call MUST succeed or there's a bug in the implementation.
                                RESULT_ASSERT(pdSelfDist->baseProcessMessage(self, msg, false), ==, 0);
                                ASSERT(PD_MSG_FIELD_O(returnDetail) == 0); // Message's processing return code
                                // Free the message (had been copied when enqueued)
                                self->fcts.pdFree(self, msg);
                                idx++;
                            }
                            queueDestroy(eligibleQueue);
                        }
                        hal_unlock(&(proxyDb->lock));
                        relProxyDb(self, proxyDb);
                        if (PD_MSG_FIELD_IO(edtSlot) == EDT_SLOT_NONE) {
                            //BUG #190
                            PROCESS_MESSAGE_RETURN_NOW(self, 0);
                        }
                        // Fall-through to local processing:
                        // This acquire may be part of an acquire frontier that needs to be iterated over
                    }
                    break;
                    // Handle all the invalid cases
                    case PROXY_DB_CREATED:
                        // Error in created state: By design cannot receive an acquire response in this state
                        ASSERT(false && "Invalid proxy DB state: PROXY_DB_CREATED processing an acquire response");
                    break;
                    case PROXY_DB_RUN:
                        // Error in run state: an acquire is either local and use the cache copy or is enqueued
                        ASSERT(false && "Invalid proxy DB state: PROXY_DB_RUN processing an acquire response");
                    break;
                    case PROXY_DB_RELINQUISH:
                        // Error in relinquish state: all should have been enqueued
                        ASSERT(false && "Invalid proxy DB state: PROXY_DB_RELINQUISH processing an acquire response");
                    break;
                    default:
                        ASSERT(false && "Unsupported proxy DB state");
                    // Fall-through to process the incoming acquire response
                }
                // processDbAcquireResponse(self, msg);
            } // else outgoing acquire response to be sent out, fall-through
        }
        if ((msg->srcLocation == curLoc) && (msg->destLocation == curLoc)) {
            DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: local request for DB GUID "GUIDF" with properties 0x%"PRIx32"\n",
                    GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties));
        }
        // Let the base policy's processMessage acquire the DB on behalf of the remote EDT
        // and then append the db data to the message.
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_RELEASE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_RELEASE
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO)
        if ((msg->srcLocation == curLoc) && (msg->destLocation != curLoc)) {
            if (msg->type & PD_MSG_LOCAL_PROCESS) { //BUG #162 - This is a workaround until metadata cloning
                DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE local processing: DB GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                PROCESS_MESSAGE_WITH_PROXY_DB_AND_RETURN
            }
            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE outgoing request send for DB GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
            // Outgoing release request
            ProxyDb_t * proxyDb = getProxyDb(self, PD_MSG_FIELD_IO(guid.guid), false);
            if (proxyDb == NULL) {
                // This is VERY likely an error in the user-code where the DB is released twice by the same EDT.
                DPRINTF(DEBUG_LVL_WARN,"Detected multiple release for DB "GUIDF" by EDT "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)), GUIDA(PD_MSG_FIELD_I(edt.guid)));
                msg->type &= ~PD_MSG_REQUEST;
                msg->type &= ~PD_MSG_REQ_RESPONSE;
                msg->type |= PD_MSG_RESPONSE;
                msg->srcLocation = curLoc;
                msg->destLocation = curLoc;
                PD_MSG_FIELD_O(returnDetail) = 0;
                PROCESS_MESSAGE_RETURN_NOW(self, OCR_EACCES);
            }
            hal_lock(&(proxyDb->lock)); // lock the db
            switch(proxyDb->state) {
                case PROXY_DB_RUN:
                    if (proxyDb->nbUsers == 1) {
                        // Last checked-in user of the proxy DB in this PD
                        proxyDb->state = PROXY_DB_RELINQUISH;
                        DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE outgoing request send for DB GUID "GUIDF" with WB=%"PRId32"\n",
                            GUIDA(PD_MSG_FIELD_IO(guid.guid)), !!(proxyDb->flags & DB_FLAG_RT_WRITE_BACK));
                        if (proxyDb->flags & DB_FLAG_RT_WRITE_BACK) {
                            // Serialize the cached DB ptr for write back
                            u64 dbSize = proxyDb->size;
                            void * dbPtr = proxyDb->ptr;
                            // Update the message's properties
                            PD_MSG_FIELD_I(properties) |= DB_FLAG_RT_WRITE_BACK;
                            PD_MSG_FIELD_I(ptr) = dbPtr;
                            PD_MSG_FIELD_I(size) = dbSize;
                            //ptr is updated on the other end when deserializing
                        } else {
                            // Just to double check if we missed callsites
                            ASSERT(PD_MSG_FIELD_I(ptr) == NULL);
                            ASSERT(PD_MSG_FIELD_I(size) == 0);
                            // no WB, make sure these are set to avoid erroneous serialization
                            PD_MSG_FIELD_I(ptr) = NULL;
                            PD_MSG_FIELD_I(size) = 0;
                        }
                        // Fall-through and send the release request
                        // The count is decremented when the release response is received
                    } else {
                        DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE outgoing request send for DB GUID "GUIDF" intercepted for local proxy DB=%"PRId32"\n",
                            GUIDA(PD_MSG_FIELD_IO(guid.guid)), !!(proxyDb->flags & DB_FLAG_RT_WRITE_BACK));
                        // The proxy DB is still in use locally, no need to notify the original DB.
                        proxyDb->nbUsers--;
                        // fill in response message
                        msg->type &= ~PD_MSG_REQUEST;
                        msg->type &= ~PD_MSG_REQ_RESPONSE;
                        msg->type |= PD_MSG_RESPONSE;
                        msg->srcLocation = curLoc;
                        msg->destLocation = curLoc;
                        PD_MSG_FIELD_O(returnDetail) = 0;
                        hal_unlock(&(proxyDb->lock));
                        relProxyDb(self, proxyDb);
                        PROCESS_MESSAGE_RETURN_NOW(self, 0); // bypass local processing
                    }
                break;
                // Handle all the invalid cases
                case PROXY_DB_CREATED:
                    // Error in created state: By design cannot release before acquire
                    ASSERT(false && "Invalid proxy DB state: PROXY_DB_CREATED processing a release request");
                break;
                case PROXY_DB_FETCH:
                    // Error in run state: Cannot release before initial acquire has completed
                    ASSERT(false && "Invalid proxy DB state: PROXY_DB_RUN processing a release request");
                break;
                case PROXY_DB_RELINQUISH:
                    // Error in relinquish state: By design the last release transitions the proxy from run to
                    // relinquish. An outgoing release request while in relinquish state breaks this invariant.
                    ASSERT(false && "Invalid proxy DB state: PROXY_DB_RELINQUISH processing a release request");
                break;
                default:
                    ASSERT(false && "Unsupported proxy DB state");
                // Fall-through to send the outgoing message
            }
            hal_unlock(&(proxyDb->lock));
            relProxyDb(self, proxyDb);
        }

        if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
            // Incoming DB_RELEASE pre-processing
            // Need to resolve the DB metadata locally before handing the message over
            u64 val;
            self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL, MD_LOCAL, NULL);
            ASSERT(val != 0);
            PD_MSG_FIELD_IO(guid.metaDataPtr) = (void *) val;
            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE incoming request received for DB GUID "GUIDF" WB=%"PRId32"\n",
                    GUIDA(PD_MSG_FIELD_IO(guid.guid)), !!(PD_MSG_FIELD_I(properties) & DB_FLAG_RT_WRITE_BACK));
            //BUG #587 db: We may want to double check this writeback (first one) is legal wrt single assignment
            if (PD_MSG_FIELD_I(properties) & DB_FLAG_RT_WRITE_BACK) {
                // Unmarshall and write back
                //WARN: MUST read from the RELEASE size u64 field instead of the msg size (u32)
                u64 size = PD_MSG_FIELD_I(size);
                void * data = PD_MSG_FIELD_I(ptr);
                // Acquire local DB with oblivious property on behalf of remote release to do the writeback.
                void * localData = acquireLocalDbOblivious(self, PD_MSG_FIELD_IO(guid.guid));
                ASSERT(localData != NULL);
                hal_memCopy(localData, data, size, false);
                //BUG #607 DB RO mode: We do not release here because we've been using this
                // special mode to do the write back. the release happens in the fall-through
            } // else fall-through and do the regular release
        }
        if ((msg->srcLocation == curLoc) && (msg->destLocation == curLoc)) {
            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE local processing: DB GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
        }
#undef PD_MSG
#undef PD_TYPE
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
        DPRINTF(DEBUG_LVL_VVERB, "DEP_REGWAITER: target is %"PRId32"\n", (u32)msg->destLocation);
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
    case PD_MSG_GUID_RESERVE:
    case PD_MSG_GUID_UNRESERVE:
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
            case PD_MSG_DB_ACQUIRE:
            {
                //BUG #190
                // When edtSlot equals EDT_SLOT_NONE and the message is remote we're *likely*
                // dealing with an acquire made from a blocking calling context (such as legacy)
                // so we need to do things differently. There are various checks across the impl
                // that deals with this scenario and will be much better addressed when we have
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                ASSERT(((PD_MSG_FIELD_IO(edtSlot) == EDT_SLOT_NONE) || false) && "Unhandled blocking acquire message");
#undef PD_MSG
#undef PD_TYPE
                // We need to process the response to complete the acquire
                // HACK for BUG #865
                self->fcts.processMessage(self, response, true);
            break;
            }
            case PD_MSG_DB_CREATE:
            {
                // Receiving the reply for a DB_CREATE
                // Because the current DB creation implementation does not issue a full-fleshed
                // acquire message but rather only do a local acquire at destination, we need
                // to create and populate the proxy here and replicate what a new acquire on a
                // not proxied DB would have been doing
#define PD_MSG (response)
#define PD_TYPE PD_MSG_DB_CREATE
                ocrGuid_t dbGuid = PD_MSG_FIELD_IO(guid.guid);
                ProxyDb_t * proxyDb = createProxyDb(self);
                proxyDb->state = PROXY_DB_RUN;
                proxyDb->nbUsers = 1; // self
                proxyDb->refCount = 0; // ref hasn't been shared yet so '0' is fine.
                proxyDb->mode = (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK); //BUG #273
                if (proxyDb->mode == 0) {
                    DPRINTF(DEBUG_LVL_WARN,"DB create mode not found !!!, default to RW\n");
                    proxyDb->mode = DB_MODE_RW;
                }
                //BUG #273: The easy patch is to make 'size' an IO, otherwise we need to make sure
                //the request message is copied on send, so that we can keep it around and when
                //we have the response we can still poke into the request for these info
                proxyDb->size = PD_MSG_FIELD_IO(size); //BUG #273
                proxyDb->ptr =  self->fcts.pdMalloc(self, PD_MSG_FIELD_IO(size)); //BUG #273
                // Preset the writeback flag: even single assignment needs to be written back the first time.
                proxyDb->flags = (PD_MSG_FIELD_IO(properties) | DB_FLAG_RT_WRITE_BACK); //BUG #273
                // double check there's no proxy registered for the same DB
                u64 val;
                self->guidProviders[0]->fcts.getVal(self->guidProviders[0], dbGuid, &val, NULL, MD_LOCAL, NULL);
                ASSERT(val == 0);
                // Do the actual registration
                self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], dbGuid, (u64) proxyDb);
                // Update message with proxy DB ptr
                PD_MSG_FIELD_O(ptr) = proxyDb->ptr;
                ASSERT(proxyDb->db == NULL);
                ocrFatGuid_t tGuid;
                RESULT_ASSERT(((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->instantiate(
                                  ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx])), &tGuid, self->allocators[0]->fguid, self->fguid,
                                  proxyDb->size, proxyDb->ptr, NULL_HINT, DB_PROP_RT_PROXY, NULL), ==, 0);
                proxyDb->db = (ocrDataBlock_t*)tGuid.metaDataPtr;
                proxyDb->db->guid = dbGuid;
#undef PD_MSG
#undef PD_TYPE
            break;
            }
            case PD_MSG_DB_RELEASE:
            {
#define PD_MSG (response)
#define PD_TYPE PD_MSG_DB_RELEASE
                //BUG #273: made guid an IO for now
                ocrGuid_t dbGuid = PD_MSG_FIELD_IO(guid.guid);
                ProxyDb_t * proxyDb = getProxyDb(self, dbGuid, false);
                ASSERT(proxyDb != NULL);
                hal_lock(&(proxyDb->lock)); // lock the db
                switch(proxyDb->state) {
                case PROXY_DB_RELINQUISH:
                    // Should have a single user since relinquish state
                    // forces concurrent acquires to be queued.
                    ASSERT(proxyDb->nbUsers == 1);
                    // The release having occurred, the proxy's metadata is invalid.
                    if (queueIsEmpty(proxyDb->acquireQueue)) {
                        // There are no pending acquire for this DB, try to deallocate the proxy.
                        hal_lock(&((ocrPolicyDomainHcDist_t *) self)->lockDbLookup);
                        // Here nobody else can acquire a reference on the proxy
                        if (proxyDb->refCount == 1) {
                            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE response received for DB GUID "GUIDF", destroy proxy\n", GUIDA(dbGuid));
                            // Removes the entry for the proxy DB in the GUID provider
                            self->guidProviders[0]->fcts.unregisterGuid(self->guidProviders[0], dbGuid, (u64**) 0);
                            // Nobody else can get a reference on the proxy's lock now
                            hal_unlock(&((ocrPolicyDomainHcDist_t *) self)->lockDbLookup);
                            // Deallocate the proxy DB and the cached ptr
                            // NOTE: we do not unlock proxyDb->lock not call relProxyDb
                            // since we're destroying the whole proxy and we're the last user.
                            ASSERT(proxyDb->db != NULL);
                            ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.destruct(proxyDb->db);
                            self->fcts.pdFree(self, proxyDb->ptr);
                            if (proxyDb->acquireQueue != NULL) {
                                self->fcts.pdFree(self, proxyDb->acquireQueue);
                            }
                            self->fcts.pdFree(self, proxyDb);
                        } else {
                            // Not deallocating the proxy then allow others to grab a reference
                            hal_unlock(&((ocrPolicyDomainHcDist_t *) self)->lockDbLookup);
                            // Else no pending acquire enqueued but someone already got a reference
                            // to the proxyDb, repurpose the proxy for a new fetch
                            // Resetting the state to created means the any concurrent acquire
                            // to the currently executing call will try to fetch the DB.
                            resetProxyDb(proxyDb);
                            // Allow others to use the proxy
                            hal_unlock(&(proxyDb->lock));
                            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE response received for DB GUID "GUIDF", proxy is referenced\n", GUIDA(dbGuid));
                            relProxyDb(self, proxyDb);
                        }
                    } else {
                        // else there are pending acquire, repurpose the proxy for a new fetch
                        // Resetting the state to created means the popped acquire or any concurrent
                        // acquire to the currently executing call will try to fetch the DB.
                        resetProxyDb(proxyDb);
                        DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE response received for DB GUID "GUIDF", processing queued acquire\n", GUIDA(dbGuid));
                        // DBs are not supposed to be resizable hence, do NOT reset
                        // size and ptr so they can be reused in the subsequent fetch.
                        // NOTE: There's a size check when an acquire fetch completes and we reuse the proxy.
                        // Pop one of the enqueued acquire and process it 'again'.
                        ocrPolicyMsg_t * pendingAcquireMsg = (ocrPolicyMsg_t *) queueRemoveLast(proxyDb->acquireQueue);
                        hal_unlock(&(proxyDb->lock));
                        relProxyDb(self, proxyDb);
                        // Now this processMessage call is potentially concurrent with new acquire being issued
                        // by other EDTs on this node. It's ok, either this call succeeds or the acquire is enqueued again.
                        u8 returnCode = self->fcts.processMessage(self, pendingAcquireMsg, false);
                        ASSERT((returnCode == 0) || (returnCode == OCR_EPEND));
                        if (returnCode == OCR_EPEND) {
                            // If the acquire didn't succeed, the message has been copied and enqueued
                            self->fcts.pdFree(self, pendingAcquireMsg);
                        }
                    }
                break;
                // Handle all the invalid cases
                case PROXY_DB_CREATED:
                    // Error in created state: By design cannot release before acquire
                    ASSERT(false && "Invalid proxy DB state: PROXY_DB_CREATED processing a release response");
                break;
                case PROXY_DB_FETCH:
                    // Error in run state: Cannot release before initial acquire has completed
                    ASSERT(false && "Invalid proxy DB state: PROXY_DB_RUN processing a release response");
                break;
                case PROXY_DB_RUN:
                    // Error the acquire request should have transitioned the proxy from the run state to the
                    // relinquish state
                    ASSERT(false && "Invalid proxy DB state: PROXY_DB_RELINQUISH processing a release response");
                break;
                default:
                    ASSERT(false && "Unsupported proxy DB state");
                }
#undef PD_MSG
#undef PD_TYPE
            break;
            }
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
                ASSERT(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE)
                        || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_GUID_METADATA_CLONE));
                // Marshall outgoing DB_ACQUIRE Response
                switch(msg->type & PD_MSG_TYPE_ONLY) {
                case PD_MSG_DB_ACQUIRE:
                {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                    DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing response for DB GUID "GUIDF" with properties=0x%"PRIx32" and dest=%"PRId32"\n",
                            GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_IO(properties), (u32) msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
                break;
                }
                default: { }
                }
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
                initializePolicyMessage(msg, baseSizeOut);
                ocrPolicyMsgMarshallMsg(msgInCopy, baseSizeIn, (u8*)msg, MARSHALL_DUPLICATE);
            }
        }

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

        if (msg->destLocation != curLoc) {
            ASSERT(reqResponse); // double check not trying to answer when we shouldn't
            // For now a two-way is always between the same pair of src/dst.
            // Cannot answer to someone else on behalf of the original sender.
            ASSERT(msg->destLocation == orgSrcLocation);

            //IMPL: Because we are processing a two-way originating from another PD,
            // the message buffer is necessarily managed by the runtime (as opposed
            // to be on the user call stack calling in its PD).
            // Hence, we post the response as a one-way, persistent and no handle.
            // The message will be deallocated on one-way call completion.
            u32 sendProp = PERSIST_MSG_PROP;
            DPRINTF(DEBUG_LVL_VVERB,"Send response to %"PRIu64" after local processing of msg\n", msg->destLocation);
            ASSERT(msg->type & PD_MSG_RESPONSE);
            ASSERT((msg->type & PD_MSG_TYPE_ONLY) != PD_MSG_MGT_MONITOR_PROGRESS);
            switch(msg->type & PD_MSG_TYPE_ONLY) {
            case PD_MSG_GUID_METADATA_CLONE:
            {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
                ocrGuidKind kind;
                self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_IO(guid).guid, &kind);
                sendProp |= ASYNC_MSG_PROP;
#undef PD_MSG
#undef PD_TYPE
                break;
            }
            case PD_MSG_DB_ACQUIRE:
            {
                //BUG #190
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: post-process response, GUID="GUIDF" serialize DB's ptr, dest is %"PRIu64"\n",
                        GUIDA(PD_MSG_FIELD_IO(guid.guid)), msg->destLocation);
                if (PD_MSG_FIELD_IO(edtSlot) != EDT_SLOT_NONE) {
                    sendProp |= ASYNC_MSG_PROP;
                }
#undef PD_MSG
#undef PD_TYPE
            break;
            }
            default: { }
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
        hcDistProcessMessage(self, evtMsg->msg, true);
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
    hcDistPd->lockDbLookup = INIT_LOCK;
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
