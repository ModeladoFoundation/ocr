
/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "experimental/ocr-placer.h"
#include "utils/hashtable.h"
#include "utils/queue.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#include "policy-domain/hc-dist/hc-dist-policy.h"

#include "worker/hc/hc-worker.h"
//BUG #204 cloning: sep-concern: need to know end type to support edt templates cloning
#include "task/hc/hc-task.h"
#include "event/hc/hc-event.h"

#define DEBUG_TYPE POLICY

#define RETRIEVE_LOCATION_FROM_MSG(pd, fname, dstLoc, DIR) \
    ocrFatGuid_t fatGuid__ = PD_MSG_FIELD_##DIR(fname); \
    u8 res = guidLocation(pd, fatGuid__, &dstLoc); \
    ASSERT(!res);

#define RETRIEVE_LOCATION_FROM_GUID_MSG(pd, dstLoc, DIR) \
    ocrFatGuid_t fatGuid__ = PD_MSG_FIELD_##DIR(guid); \
    u8 res = guidLocation(pd, fatGuid__, &dstLoc); \
    ASSERT(!res);

#define RETRIEVE_LOCATION_FROM_GUID(pd, dstLoc, guid__) \
    ocrFatGuid_t fatGuid__; \
    fatGuid__.guid = guid__; \
    fatGuid__.metaDataPtr = NULL; \
    u8 res = guidLocation(pd, fatGuid__, &dstLoc); \
    ASSERT(!res);

#define PROCESS_MESSAGE_RETURN_NOW(pd, retCode) \
    return retCode;


/****************************************************/
/* PROXY TEMPLATE MANAGEMENT                        */
/****************************************************/
//BUG #536: To be replaced by a general metadata cloning framework

extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static u8 createProcessRequestEdtDistPolicy(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid, u64 * paramv) {

    ocrGuid_t edtGuid;
    u32 paramc = 1;
    u32 depc = 0;
    u32 properties = 0;
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
    PD_MSG_FIELD_I(affinity.guid) = NULL_GUID;
    PD_MSG_FIELD_I(affinity.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(outputEvent.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(outputEvent.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(paramv) = paramv;
    PD_MSG_FIELD_IO(paramc) = paramc;
    PD_MSG_FIELD_IO(depc) = depc;
    PD_MSG_FIELD_I(depv) = NULL;
    PD_MSG_FIELD_I(properties) = properties;
    PD_MSG_FIELD_I(workType) = workType;
    // This is a "fake" EDT so it has no "parent"
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(parentLatch.guid) = NULL_GUID;
    PD_MSG_FIELD_I(parentLatch.metaDataPtr) = NULL;
    returnCode = pd->fcts.processMessage(pd, &msg, true);
    if(returnCode) {
        edtGuid = PD_MSG_FIELD_IO(guid.guid);
        DPRINTF(DEBUG_LVL_VVERB,"hc-comm-worker: Created processRequest EDT GUID 0x%lx\n", edtGuid);
        RETURN_PROFILE(returnCode);
    }

    RETURN_PROFILE(0);
#undef PD_MSG
#undef PD_TYPE
}

typedef struct _ProxyTplNode_t {
    ocrPolicyMsg_t * msg;
    struct _ProxyTplNode_t * next;
} ProxyTplNode_t;

typedef struct {
    u64 count;
    ProxyTplNode_t * queueHead;
} ProxyTpl_t;

static u8 registerRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t tplFatGuid) {
    ocrPolicyDomainHcDist_t * dself = (ocrPolicyDomainHcDist_t *) pd;
    // The lock allows to not give out reference to the proxy while we work.
    hal_lock32(&dself->lockTplLookup);
    pd->guidProviders[0]->fcts.registerGuid(pd->guidProviders[0], tplFatGuid.guid, (u64) tplFatGuid.metaDataPtr);
    ProxyTpl_t * proxyTpl = NULL;
    bool found = hashtableNonConcRemove(dself->proxyTplMap, (void *) tplFatGuid.guid, (void **) &proxyTpl);
    ASSERT(found && (proxyTpl != NULL));
    proxyTpl->count++;
    hal_unlock32(&dself->lockTplLookup);
    // At this point all calls to 'resolveRemoteMetaData' see the update pointer
    // in the guid provider and do not try to get a reference on the proxy.
    // Other workers may already own a reference to the proxy and try to enqueue themselves.
    // Hence, compete to close registration by setting the proxy's queueHead to NULL.
    u64 curValue = 0;
    u64 oldValue = 0;
    do {
        curValue = (u64) proxyTpl->queueHead;
        oldValue = hal_cmpswap64((u64*) &(proxyTpl->queueHead), curValue, 0);
    } while(curValue != oldValue);

    // Also need to compete to check out and destroy the proxy
    hal_lock32(&dself->lockTplLookup);
    proxyTpl->count--;
    if (proxyTpl->count == 0) {
        pd->fcts.pdFree(pd, proxyTpl);
    }
    hal_unlock32(&dself->lockTplLookup);

    ocrGuid_t processRequestTemplateGuid;
    ocrEdtTemplateCreate(&processRequestTemplateGuid, &processRequestEdt, 1, 0);
    ProxyTplNode_t * queueHead = (ProxyTplNode_t *) oldValue;
    DPRINTF(DEBUG_LVL_VVERB,"About to process stored clone requests for template 0x%lx queueHead=%x)\n", tplFatGuid.guid, queueHead);
    while (queueHead != ((void*) 0x1)) { // sentinel value
        DPRINTF(DEBUG_LVL_VVERB,"Processing stored clone requests for template 0x%lx)\n", tplFatGuid.guid);
        u64 paramv = (u64) queueHead->msg;
        createProcessRequestEdtDistPolicy(pd, processRequestTemplateGuid, &paramv);
        ProxyTplNode_t * currNode = queueHead;
        queueHead = currNode->next;
        pd->fcts.pdFree(pd, currNode);
    }
    ocrEdtTemplateDestroy(processRequestTemplateGuid);
    return 0;
}

static u8 resolveRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t * tplFatGuid, ocrPolicyMsg_t * msg) {
    // Check if known locally
    u64 val;
    pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], tplFatGuid->guid, &val, NULL);
    if (val == 0) {
        // If the source of the work creation is the current policy-domain,
        // it most likely means edtCreate is called from the user-code which
        // is a blocking call. In that case we fetch the metadata in a blocking
        // manner. Conversely, if the source is not the current PD, it means we
        // can fetch the metadata asynchronously and return a pending status.
        // The edt will be rescheduled at a later time once the metadata has been resolved
        bool isBlocking = (msg->srcLocation == pd->myLocation);
        ocrPolicyDomainHcDist_t * dself = (ocrPolicyDomainHcDist_t *) pd;
        // Nope, check the proxy template map
        hal_lock32(&dself->lockTplLookup);
        // Double check again if the template has been resolved.
        // Helps preserve the invariant that once a template is resolved
        // its proxy's reference count cannot increment. (lock compete in registerRemoteMetaData)
        pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], tplFatGuid->guid, &val, NULL);
        if (val != 0) {
            tplFatGuid->metaDataPtr = (void *) val;
            hal_unlock32(&dself->lockTplLookup);
            return 0;
        }
        // Check if the proxy exists or not.
        ProxyTpl_t * proxyTpl = (ProxyTpl_t *) hashtableNonConcGet(dself->proxyTplMap, (void *) tplFatGuid->guid);
        if (proxyTpl == NULL) {
            proxyTpl = (ProxyTpl_t *) pd->fcts.pdMalloc(pd, sizeof(ProxyTpl_t));
            proxyTpl->count = 1;
            proxyTpl->queueHead = (void *) 0x1; // sentinel value
            void * ret = hashtableNonConcTryPut(dself->proxyTplMap, (void *) tplFatGuid->guid, (void *) proxyTpl);
            ASSERT(ret == proxyTpl);
            hal_unlock32(&dself->lockTplLookup);

            // GUID is unknown, request a copy of the metadata
            PD_MSG_STACK(msgClone);
            getCurrentEnv(NULL, NULL, NULL, &msgClone);
            DPRINTF(DEBUG_LVL_VVERB,"Resolving metadata -> need to query remote node (using msg @ 0x%lx)\n", &msgClone);
#define PD_MSG (&msgClone)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
                msgClone.type = PD_MSG_GUID_METADATA_CLONE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = tplFatGuid->guid;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                u8 returnCode = pd->fcts.processMessage(pd, &msgClone, false);
                ASSERT(returnCode == OCR_EPEND);
#undef PD_MSG
#undef PD_TYPE
        } else {
            proxyTpl->count++;
            hal_unlock32(&dself->lockTplLookup);
        }

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
                pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], tplFatGuid->guid, &val, NULL);
            } while(val == 0);
            tplFatGuid->metaDataPtr = (void *) val;
            DPRINTF(DEBUG_LVL_VVERB,"Resolving metadata: exit busy-wait for blocking call\n");
        } else {
            // Enqueue itself, the caller will be rescheduled for execution
            ProxyTplNode_t * node = (ProxyTplNode_t *) pd->fcts.pdMalloc(pd, sizeof(ProxyTplNode_t));
            node->msg = msg;
            u64 newValue = (u64) node;
            bool notSucceed = true;
            do {
                ProxyTplNode_t * head = proxyTpl->queueHead;
                if (head == 0) { // registration is closed
                    break;
                }
                node->next = head;
                u64 curValue = (u64) head;
                u64 oldValue = hal_cmpswap64((u64*) &(proxyTpl->queueHead), curValue, newValue);
                notSucceed = (oldValue != curValue);
            } while(notSucceed);

            if (notSucceed) { // registration has closed
                pd->fcts.pdFree(pd, node);
                pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], tplFatGuid->guid, &val, NULL);
                ASSERT(val != 0);
                tplFatGuid->metaDataPtr = (void *) val;
            }
        }
        // Compete to check out of the proxy
        hal_lock32(&dself->lockTplLookup);
        proxyTpl->count--;
        if ((proxyTpl->count == 0) && (proxyTpl->queueHead == NULL)) {
            pd->fcts.pdFree(pd, proxyTpl);
        }
        hal_unlock32(&dself->lockTplLookup);
        return (val == 0) ? OCR_EPEND : 0;
    } else {
        tplFatGuid->metaDataPtr = (void *) val;
        return 0;
    }
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
    u32 lock;
    Queue_t * acquireQueue;
    u16 mode;
    u64 size;
    void * volatile ptr;
    u32 flags;
} ProxyDb_t;

/**
 * @brief Allocate a proxy DB
 */
static ProxyDb_t * createProxyDb(ocrPolicyDomain_t * pd) {
    ProxyDb_t * proxyDb = pd->fcts.pdMalloc(pd, sizeof(ProxyDb_t));
    proxyDb->state = PROXY_DB_CREATED;
    proxyDb->nbUsers = 0;
    proxyDb->refCount = 0;
    proxyDb->lock = 0;
    proxyDb->acquireQueue = NULL;
    // Cached DB information
    proxyDb->mode = 0;
    proxyDb->size = 0;
    proxyDb->ptr = NULL;
    proxyDb->flags = 0;
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
    hal_lock32(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
    ProxyDb_t * proxyDb = NULL;
    u64 val;
    pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], dbGuid, &val, NULL);
    if (val == 0) {
        if (createIfAbsent) {
            proxyDb = createProxyDb(pd);
            pd->guidProviders[0]->fcts.registerGuid(pd->guidProviders[0], dbGuid, (u64) proxyDb);
        } else {
            hal_unlock32(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
            return NULL;
        }
    } else {
        proxyDb = (ProxyDb_t *) val;
    }
    proxyDb->refCount++;
    hal_unlock32(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
    return proxyDb;
}

/**
 * @brief Release a proxy DB, decrementing its refCount counter by one.
 * Warning: This is different from releasing a datablock.
 */
static void relProxyDb(ocrPolicyDomain_t * pd, ProxyDb_t * proxyDb) {
    hal_lock32(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
    proxyDb->refCount--;
    hal_unlock32(&((ocrPolicyDomainHcDist_t *) pd)->lockDbLookup);
}

/**
 * @brief Check if the proxy's DB mode is compatible with another DB mode
 * This call helps to determine when an acquire is susceptible to use the proxy DB
 * or if it is not and must handled at a later time.
 *
 * This implementation allows RO over RO and ITW over ITW.
 */
static bool isAcquireEligibleForProxy(ocrDbAccessMode_t proxyDbMode, ocrDbAccessMode_t acquireMode) {
    return (((proxyDbMode == DB_MODE_CONST) && (acquireMode == DB_MODE_CONST)) ||
     ((proxyDbMode == DB_MODE_RW) && (acquireMode == DB_MODE_RW)));
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
    if ((candidateQueue != NULL) && ((acquireMode == DB_MODE_CONST) || (acquireMode == DB_MODE_RW))) {
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
                                        (u64*)&fatGuid->metaDataPtr, NULL);
    ocrTaskTemplate_t * edtTemplate = (ocrTaskTemplate_t *) fatGuid->metaDataPtr;
    if(*paramc == EDT_PARAM_DEF) *paramc = edtTemplate->paramc;
    if(*depc == EDT_PARAM_DEF) *depc = edtTemplate->depc;
}

static void * acquireLocalDb(ocrPolicyDomain_t * pd, ocrGuid_t dbGuid, ocrDbAccessMode_t mode) {
    ocrTask_t *curTask = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, &curTask, &msg);
    u32 properties = mode | DB_PROP_RT_ACQUIRE;
    if (mode == DB_MODE_RO) {
        //BUG #607 DB RO mode: This is only used by the runtime for write-backs
        //         and not interfere with lockable datablocks locking protocol.
        properties |= DB_PROP_RT_OBLIVIOUS;
    }
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
    msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = dbGuid;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(edt.guid) = curTask->guid;
    PD_MSG_FIELD_IO(edt.metaDataPtr) = curTask;
    PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
    PD_MSG_FIELD_IO(properties) = properties; // Runtime acquire
    // This call may fail if the policy domain goes down
    // while we are starting to execute
    if(pd->fcts.processMessage(pd, &msg, true)) {
        ASSERT(false); // debug
        return NULL;
    }
    return PD_MSG_FIELD_O(ptr);
#undef PD_MSG
#undef PD_TYPE
}

// Might be resurrected when we finish RO DB mode implementation
// static void releaseLocalDb(ocrPolicyDomain_t * pd, ocrGuid_t dbGuid, u64 size) {
//     ocrTask_t *curTask = NULL;
//     PD_MSG_STACK(msg);
//     getCurrentEnv(NULL, NULL, &curTask, &msg);
// #define PD_MSG (&msg)
// #define PD_TYPE PD_MSG_DB_RELEASE
//     msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
//     PD_MSG_FIELD_IO(guid.guid) = dbGuid;
//     PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
//     PD_MSG_FIELD_I(edt.guid) = curTask->guid;
//     PD_MSG_FIELD_I(edt.metaDataPtr) = curTask;
//     PD_MSG_FIELD_I(size) = size;
//     PD_MSG_FIELD_I(properties) = DB_PROP_RT_ACQUIRE; // Runtime release
//     // Ignore failures at this point
//     pd->fcts.processMessage(pd, &msg, true);
// #undef PD_MSG
// #undef PD_TYPE
// }

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
            || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_GUID_METADATA_CLONE));
        // for a clone the cloning should actually be of an edt template
    }

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
    u32 properties = 0;

    // Try to automatically place datablocks and edts
    // Only support naive PD-based placement for now.
    suggestLocationPlacement(self, curLoc, (ocrLocationPlacer_t *) self->placer, msg);

    DPRINTF(DEBUG_LVL_VERB, "HC-dist processing message @ 0x%lx of type 0x%x\n", msg, msg->type);

    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_WORK_CREATE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
        // First query the guid provider to determine if we know the edtTemplate.
        u8 res = resolveRemoteMetaData(self, &PD_MSG_FIELD_I(templateGuid), msg);
        if (res == OCR_EPEND) {
            // We do not handle pending if it an edt spawned locally because
            // the edt creation is likely invoked from another local EDT.
            ASSERT(msg->srcLocation != curLoc);
            // template's metadata not available, message processing will be rescheduled.
            PROCESS_MESSAGE_RETURN_NOW(self, OCR_EPEND);
        }
        DPRINTF(DEBUG_LVL_VVERB,"WORK_CREATE: try to resolve template GUID 0x%lx\n", PD_MSG_FIELD_I(templateGuid.guid));
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
            DPRINTF(DEBUG_LVL_VVERB,"WORK_CREATE: local EDT creation for template GUID 0x%lx\n", PD_MSG_FIELD_I(templateGuid.guid));
        } else {
            // For asynchronous EDTs we check the content of depv.
            // If it contains non-persistent events the creation
            // must be synchronous and we change the message flags here.
            if (!(msg->type & PD_MSG_REQ_RESPONSE)) {
                ocrFatGuid_t * depv = PD_MSG_FIELD_I(depv);
                u32 depc = ((depv != NULL) ? PD_MSG_FIELD_IO(depc) : 0);
                u32 i;
                for(i=0; i<depc; i++) {
                    ASSERT(depv[i].guid != UNINITIALIZED_GUID);
                    ocrGuidKind kind;
                    u8 ret = self->guidProviders[0]->fcts.getKind(self->guidProviders[0], depv[i].guid, &kind);
                    ASSERT(!ret);
                    if ((kind == OCR_GUID_EVENT_ONCE) || (kind == OCR_GUID_EVENT_LATCH)) {
                        msg->type |= PD_MSG_REQ_RESPONSE;
                        DPRINTF(DEBUG_LVL_WARN,"NULL-GUID EDT creation made synchronous: depv[%d] is (ONCE|LATCH)\n", i);
                        break;
                    }
                }
            }

            // Outgoing EDT create message
            DPRINTF(DEBUG_LVL_VVERB,"WORK_CREATE: remote EDT creation at %lu for template GUID 0x%lx\n", (u64)msg->destLocation, PD_MSG_FIELD_I(templateGuid.guid));
#undef PD_MSG
#undef PD_TYPE
            /* The support for finish EDT and latch in distributed OCR
             * has the following implementation currently:
             * Whenever an EDT needs to be created on a remote node,
             * then a proxy latch is created on the remote node if
             * there is a parent latch to report to on the source node.
             * So, when an parent EDT creates a child EDT on a remote node,
             * the local latch is first incremented on the source node.
             * This latch will eventually be decremented through the proxy
             * latch on the remote node.
             */
            if (parentLatch.guid != NULL_GUID) {
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
                    PD_MSG_FIELD_I(properties) = 0;
                    RESULT_PROPAGATE(self->fcts.processMessage(self, &msg2, true));
#undef PD_MSG
#undef PD_TYPE
                } // else, will create a proxy event at current location
            }
        }

        if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
            // we're receiving a message
            DPRINTF(DEBUG_LVL_VVERB, "WORK_CREATE: received request from %d\n", msg->srcLocation);
            if (parentLatch.guid != NULL_GUID) {
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
    case PD_MSG_DB_CREATE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        // The placer may have altered msg->destLocation
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
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "WORK_DESTROY: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_SATISFY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB,"DEP_SATISFY: target is %d\n", (u32) msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_EVT_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EVT_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "EVT_DESTROY: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DB_DESTROY: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_FREE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_FREE
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DB_FREE: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_EDTTEMP_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "EDTTEMP_DESTROY: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_GUID_INFO:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_INFO
        //BUG #536: cloning: What's the meaning of guid info in distributed ?
        msg->destLocation = curLoc;
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_GUID_METADATA_CLONE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
        if (msg->type & PD_MSG_REQUEST) {
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO);
            DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: request for guid=0x%lx src=%d dest=%d\n",
                    PD_MSG_FIELD_IO(guid.guid), (u32)msg->srcLocation);
            if ((msg->destLocation != curLoc) && (msg->srcLocation == curLoc)) {
                // Outgoing request
                // NOTE: In the current implementation when we call metadata-clone
                //       it is because we've already checked the guid provider and
                //       the guid is not available
                // If it's a non-blocking processing, will set the returnDetail to busy after the request is sent out
#ifdef OCR_ASSERT
                u64 val;
                self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL);
                ASSERT(val == 0);
#endif
            }
        }
        if ((msg->destLocation == curLoc) && (msg->srcLocation != curLoc) && (msg->type & PD_MSG_RESPONSE)) {
            // Incoming response to a clone request posted earlier
            ocrGuidKind tkind;
            self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &tkind);
            if (tkind == OCR_GUID_EDT_TEMPLATE) {
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for template=0x%lx)\n", PD_MSG_FIELD_IO(guid.guid));
                // Incoming response to an asynchronous metadata clone
                u64 metaDataSize = sizeof(ocrTaskTemplateHc_t) + (sizeof(u64) * OCR_HINT_COUNT_EDT_HC);
                void * metaDataPtr = self->fcts.pdMalloc(self, metaDataSize);
                ASSERT(PD_MSG_FIELD_IO(guid.metaDataPtr) != NULL);
                ASSERT(PD_MSG_FIELD_O(size) == metaDataSize);
                // Register the metadata, process the waiter queue and checks out from the proxy.
                hal_memCopy(metaDataPtr, PD_MSG_FIELD_IO(guid.metaDataPtr), metaDataSize, false);
                ocrFatGuid_t tplFatGuid;
                tplFatGuid.guid = PD_MSG_FIELD_IO(guid.guid);
                tplFatGuid.metaDataPtr = metaDataPtr;
                registerRemoteMetaData(self, tplFatGuid);
                PROCESS_MESSAGE_RETURN_NOW(self, 0);
            } else {
                ASSERT(tkind == OCR_GUID_AFFINITY);
                DPRINTF(DEBUG_LVL_VVERB,"METADATA_CLONE: Incoming response for affinity 0x%lx)\n", PD_MSG_FIELD_IO(guid.guid));
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
        DPRINTF(DEBUG_LVL_VVERB, "GUID_DESTROY: target is %d\n", (u32)msg->destLocation);
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
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: incoming: old value for shutdownAckCount=%lu\n", oldAckValue);
            if (oldAckValue == (self->neighborCount)) {
                // Got messages from all PDs and self.
                // Done with distributed shutdown and can continue with the local shutdown.
                PD_MSG_STACK(msgNotifyRl);
                getCurrentEnv(NULL, NULL, NULL, &msgNotifyRl);
                DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: distributed shutdown is done. Resume local shutdown\n");
                u8 returnCode = rself->baseSwitchRunlevel(self, bself->rlSwitch.runlevel, bself->rlSwitch.properties);
                ASSERT(returnCode == 0);
            }
            //Note: Per current implementation, even if PDs are not in the same runlevel,
            //      the first time a PD receives a ack it has to be in the last phase up
            //      otherwise it couldn't have received the message
            bool doLocalShutdown = ((oldAckValue == 0) && (RL_GET_PHASE_COUNT_UP(self, RL_USER_OK) == bself->rlSwitch.nextPhase));
            if (!doLocalShutdown) {
                DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: got notification RL=%d PH=%d\n", bself->rlSwitch.runlevel, bself->rlSwitch.nextPhase);
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
        RETRIEVE_LOCATION_FROM_MSG(self, edt, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNADD: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_DYNREMOVE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        RETRIEVE_LOCATION_FROM_MSG(self, edt, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNREMOVE: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DB_ACQUIRE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
        if (msg->type & PD_MSG_REQUEST) {
            RETRIEVE_LOCATION_FROM_GUID_MSG(self, msg->destLocation, IO)
            // Send/Receive to/from remote or local processing, all fall-through
            if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
                // Incoming acquire request
                // The DB MUST be local to this node (that's the acquire is sent to this PD)
                // Need to resolve the DB metadata before handing the message over to the base PD.
                // The sender didn't know about the metadataPtr, receiver does.
                u64 val;
                self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL);
                ASSERT(val != 0);
                PD_MSG_FIELD_IO(guid.metaDataPtr) = (void *) val;
                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Incoming request for DB GUID 0x%lx with properties=0x%x\n",
                        PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties));
                // Fall-through to local processing
            }
            if ((msg->srcLocation == curLoc) && (msg->destLocation != curLoc)) {
                // Outgoing acquire request
                ProxyDb_t * proxyDb = getProxyDb(self, PD_MSG_FIELD_IO(guid.guid), true);
                hal_lock32(&(proxyDb->lock)); // lock the db
                switch(proxyDb->state) {
                    case PROXY_DB_CREATED:
                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing request for DB GUID 0x%lx with properties=0x%x, creation fetch\n",
                                PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties));
                        // The proxy has just been created, need to fetch the DataBlock
                        PD_MSG_FIELD_IO(properties) |= DB_FLAG_RT_FETCH;
                        proxyDb->state = PROXY_DB_FETCH;
                    break;
                    case PROXY_DB_RUN:
                        // The DB is already in use locally
                        // Check if the acquire is compatible with the current usage
                        if (isAcquireEligibleForProxy(proxyDb->mode, (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK))) {
                            DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing request for DB GUID 0x%lx with properties=0x%x, intercepted for local proxy DB\n",
                                    PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties));
                            //Use the local cached version of the DB
                            updateAcquireMessage(msg, proxyDb);
                            proxyDb->nbUsers++;
                            //Granted access to the DB through the proxy. In that sense the request
                            //has been processed and is now a response to be returned to the caller.
                            msg->type = PD_MSG_RESPONSE;
                            msg->destLocation = curLoc; // optional, doing it to be consistent
                            PD_MSG_FIELD_O(returnDetail) = 0;
                            // No need to fall-through for local processing, proxy DB is used.
                            hal_unlock32(&(proxyDb->lock));
                            relProxyDb(self, proxyDb);
                            PROCESS_MESSAGE_RETURN_NOW(self, 0);
                        } // else, not eligible to use the proxy, must enqueue the message
                        //WARN: fall-through is intentional
                    case PROXY_DB_FETCH:
                    case PROXY_DB_RELINQUISH:
                        //WARN: Do NOT move implementation: 'PROXY_DB_RUN' falls-through here
                        // The proxy is in a state requiring stalling outgoing acquires.
                        // The acquire 'msg' is copied and enqueued.
                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing request for DB GUID 0x%lx with properties=0x%x, enqueued\n",
                                PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties));
                        enqueueAcquireMessageInProxy(self, proxyDb, msg);
                        // Inform caller the acquire is pending.
                        hal_unlock32(&(proxyDb->lock));
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
                hal_unlock32(&(proxyDb->lock));
                relProxyDb(self, proxyDb);
            }
        } else { // DB_ACQUIRE response
            ASSERT(msg->type & PD_MSG_RESPONSE);
            RETRIEVE_LOCATION_FROM_MSG(self, edt, msg->destLocation, IO)
            if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
                // Incoming acquire response
                ProxyDb_t * proxyDb = getProxyDb(self, PD_MSG_FIELD_IO(guid.guid), false);
                // Cannot receive a response to an acquire if we don't have a proxy
                ASSERT(proxyDb != NULL);
                hal_lock32(&(proxyDb->lock)); // lock the db
                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Incoming response for DB GUID 0x%lx with properties=0x%x\n",
                        PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties));
                switch(proxyDb->state) {
                    case PROXY_DB_FETCH:
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

                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: caching data copy for DB GUID 0x%lx size=%lu \n",
                            PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_O(size));

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
                        DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: caching data copy for DB GUID 0x%lx ptr=%p size=%lu flags=0x%x\n",
                            PD_MSG_FIELD_IO(guid.guid), proxyDb->ptr, proxyDb->size, proxyDb->flags);
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
                                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: dequeued eligible acquire for DB GUID 0x%lx with properties=0x%x\n",
                                    PD_MSG_FIELD_IO(guid.guid), proxyDb->flags);
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
                                u8 returnCode = pdSelfDist->baseProcessMessage(self, msg, false);
                                ASSERT(PD_MSG_FIELD_O(returnDetail) == 0); // Message's processing return code
                                ASSERT(returnCode == 0); // processMessage return code
                                // Free the message (had been copied when enqueued)
                                self->fcts.pdFree(self, msg);
                                idx++;
                            }
                            queueDestroy(eligibleQueue);
                        }
                        hal_unlock32(&(proxyDb->lock));
                        relProxyDb(self, proxyDb);
                        // Fall-through to local processing:
                        // This acquire may be part of an acquire frontier that needs to be iterated over
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
            DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: local request for DB GUID 0x%lx with properties 0x%x\n",
                    PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties));
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
            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN outgoing request send for DB GUID 0x%lx\n", PD_MSG_FIELD_IO(guid.guid));
            // Outgoing release request
            ProxyDb_t * proxyDb = getProxyDb(self, PD_MSG_FIELD_IO(guid.guid), false);
            hal_lock32(&(proxyDb->lock)); // lock the db
            switch(proxyDb->state) {
                case PROXY_DB_RUN:
                    if (proxyDb->nbUsers == 1) {
                        // Last checked-in user of the proxy DB in this PD
                        proxyDb->state = PROXY_DB_RELINQUISH;
                        DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN outgoing request send for DB GUID 0x%lx with WB=%d\n",
                            PD_MSG_FIELD_IO(guid.guid), !!(proxyDb->flags & DB_FLAG_RT_WRITE_BACK));
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
                        DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN outgoing request send for DB GUID 0x%lx intercepted for local proxy DB\n",
                            PD_MSG_FIELD_IO(guid.guid), !!(proxyDb->flags & DB_FLAG_RT_WRITE_BACK));
                        // The proxy DB is still in use locally, no need to notify the original DB.
                        proxyDb->nbUsers--;
                        // fill in response message
                        msg->type &= ~PD_MSG_REQUEST;
                        msg->type &= ~PD_MSG_REQ_RESPONSE;
                        msg->type |= PD_MSG_RESPONSE;
                        msg->srcLocation = curLoc;
                        msg->destLocation = curLoc;
                        PD_MSG_FIELD_O(returnDetail) = 0;
                        hal_unlock32(&(proxyDb->lock));
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
            hal_unlock32(&(proxyDb->lock));
            relProxyDb(self, proxyDb);
        }

        if ((msg->srcLocation != curLoc) && (msg->destLocation == curLoc)) {
            // Incoming DB_RELEASE pre-processing
            // Need to resolve the DB metadata locally before handing the message over
            u64 val;
            self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &val, NULL);
            ASSERT(val != 0);
            PD_MSG_FIELD_IO(guid.metaDataPtr) = (void *) val;
            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN incoming request received for DB GUID 0x%lx WB=%d\n",
                    PD_MSG_FIELD_IO(guid.guid), !!(PD_MSG_FIELD_I(properties) & DB_FLAG_RT_WRITE_BACK));
            //BUG #587 db: We may want to double check this writeback (first one) is legal wrt single assignment
            if (PD_MSG_FIELD_I(properties) & DB_FLAG_RT_WRITE_BACK) {
                // Unmarshall and write back
                //WARN: MUST read from the RELEASE size u64 field instead of the msg size (u32)
                u64 size = PD_MSG_FIELD_I(size);
                void * data = PD_MSG_FIELD_I(ptr);
                // Acquire local DB on behalf of remote release to do the writeback.
                // Do it in OB mode so as to make sure the acquire goes through
                // We perform the write-back and then fall-through for the actual db release.
                //BUG #607 DB RO mode: Not implemented, we're converting the call to a special
                void * localData = acquireLocalDb(self, PD_MSG_FIELD_IO(guid.guid), DB_MODE_RO);
                ASSERT(localData != NULL);
                hal_memCopy(localData, data, size, false);
                //BUG #607 DB RO mode: We do not release here because we've been using this special mode to do the write back
                // releaseLocalDb(self, PD_MSG_FIELD_IO(guid.guid), size);
            } // else fall-through and do the regular release
        }
        if ((msg->srcLocation == curLoc) && (msg->destLocation == curLoc)) {
            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN local processing: DB GUID 0x%lx\n", PD_MSG_FIELD_IO(guid.guid));
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
        DPRINTF(DEBUG_LVL_VVERB, "DEP_REGSIGNALER: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_DEP_REGWAITER:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_REGWAITER
        RETRIEVE_LOCATION_FROM_MSG(self, dest, msg->destLocation, I);
        DPRINTF(DEBUG_LVL_VVERB, "DEP_REGWAITER: target is %d\n", (u32)msg->destLocation);
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_COMM_TAKE: {
        // fall-through and do regular take
        break;
    }
    case PD_MSG_MGT_MONITOR_PROGRESS:
    {
        msg->destLocation = curLoc;
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
    case PD_MSG_EVT_CREATE:
    case PD_MSG_EVT_GET:
    case PD_MSG_GUID_CREATE:
    case PD_MSG_COMM_GIVE:
    case PD_MSG_SAL_OP:
    case PD_MSG_SAL_PRINT:
    case PD_MSG_SAL_READ:
    case PD_MSG_SAL_WRITE:
    case PD_MSG_SAL_TERMINATE:
    case PD_MSG_MGT_OP: //BUG #587 not-supported: PD_MSG_MGT_OP is probably not always local
    case PD_MSG_MGT_REGISTER:
    case PD_MSG_MGT_UNREGISTER:
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
                    "processing a two-way message @ (orig: 0x%lx, now: 0x%lx) to %d\n", originalMsg, msg,
                    (u32)msg->destLocation);
            // Since it's a two-way, we'll be waiting for the response and set PERSIST.
            // NOTE: underlying comm-layer may or may not make a copy of msg.
            properties |= TWOWAY_MSG_PROP;
            properties |= PERSIST_MSG_PROP;
            ocrMsgHandle_t * handle = NULL;

            self->fcts.sendMessage(self, msg->destLocation, msg, &handle, properties);
            // Wait on the response handle for the communication to complete.
            DPRINTF(DEBUG_LVL_VVERB,"Waiting for reply from %d\n", (u32)msg->destLocation);
            self->fcts.waitMessage(self, &handle);
            DPRINTF(DEBUG_LVL_VVERB,"Received reply from %d for original message @ 0x%lx\n",
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
            DPRINTF(DEBUG_LVL_VERB, "Processing response @ 0x%lx to original message @ 0x%lx\n", response, originalMsg);
            switch (response->type & PD_MSG_TYPE_ONLY) {
            case PD_MSG_DB_ACQUIRE:
            {
                // Shouldn't happen in this implementation.
                ASSERT(false && "Unhandled blocking acquire message");
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
                    printf("WARNING: DB create mode not found !!!, default to ITW\n");
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
                self->guidProviders[0]->fcts.getVal(self->guidProviders[0], dbGuid, &val, NULL);
                ASSERT(val == 0);
                // Do the actual registration
                self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], dbGuid, (u64) proxyDb);
                // Update message with proxy DB ptr
                PD_MSG_FIELD_O(ptr) = proxyDb->ptr;
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
                hal_lock32(&(proxyDb->lock)); // lock the db
                switch(proxyDb->state) {
                case PROXY_DB_RELINQUISH:
                    // Should have a single user since relinquish state
                    // forces concurrent acquires to be queued.
                    ASSERT(proxyDb->nbUsers == 1);
                    // The release having occurred, the proxy's metadata is invalid.
                    if (queueIsEmpty(proxyDb->acquireQueue)) {
                        // There are no pending acquire for this DB, try to deallocate the proxy.
                        hal_lock32(&((ocrPolicyDomainHcDist_t *) self)->lockDbLookup);
                        // Here nobody else can acquire a reference on the proxy
                        if (proxyDb->refCount == 1) {
                            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN response received for DB GUID 0x%lx, destroy proxy\n", dbGuid);
                            // Nullify the entry for the proxy DB in the GUID provider
                            self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], dbGuid, (u64) 0);
                            // Nobody else can get a reference on the proxy's lock now
                            hal_unlock32(&((ocrPolicyDomainHcDist_t *) self)->lockDbLookup);
                            // Deallocate the proxy DB and the cached ptr
                            // NOTE: we do not unlock proxyDb->lock not call relProxyDb
                            // since we're destroying the whole proxy and we're the last user.
                            self->fcts.pdFree(self, proxyDb->ptr);
                            if (proxyDb->acquireQueue != NULL) {
                                self->fcts.pdFree(self, proxyDb->acquireQueue);
                            }
                            self->fcts.pdFree(self, proxyDb);
                        } else {
                            // Not deallocating the proxy then allow others to grab a reference
                            hal_unlock32(&((ocrPolicyDomainHcDist_t *) self)->lockDbLookup);
                            // Else no pending acquire enqueued but someone already got a reference
                            // to the proxyDb, repurpose the proxy for a new fetch
                            // Resetting the state to created means the any concurrent acquire
                            // to the currently executing call will try to fetch the DB.
                            resetProxyDb(proxyDb);
                            // Allow others to use the proxy
                            hal_unlock32(&(proxyDb->lock));
                            DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN response received for DB GUID 0x%lx, proxy is referenced\n", dbGuid);
                            relProxyDb(self, proxyDb);
                        }
                    } else {
                        // else there are pending acquire, repurpose the proxy for a new fetch
                        // Resetting the state to created means the popped acquire or any concurrent
                        // acquire to the currently executing call will try to fetch the DB.
                        resetProxyDb(proxyDb);
                        DPRINTF(DEBUG_LVL_VVERB,"DB_RELEASE_WARN response received for DB GUID 0x%lx, processing queued acquire\n", dbGuid);
                        // DBs are not supposed to be resizable hence, do NOT reset
                        // size and ptr so they can be reused in the subsequent fetch.
                        // NOTE: There's a size check when an acquire fetch completes and we reuse the proxy.
                        // Pop one of the enqueued acquire and process it 'again'.
                        ocrPolicyMsg_t * pendingAcquireMsg = (ocrPolicyMsg_t *) queueRemoveLast(proxyDb->acquireQueue);
                        hal_unlock32(&(proxyDb->lock));
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

            if (originalMsg != handle->response) {
                //BUG #587: Here there are a few issues:
                // - The response message may include marshalled pointers, hence
                //   the original message may be too small to accomodate the payload part.
                //   In that case, I don't see how to avoid malloc-ing new memory for each
                //   pointer and update the originalMsg's members, since the use of that
                //   pointers may outlive the message lifespan. Then there's the question
                //   of when those are freed.
                u64 baseSize = 0, marshalledSize = 0;
                ocrPolicyMsgGetMsgSize(handle->response, &baseSize, &marshalledSize, 0);

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
                hal_memCopy(originalMsg, handle->response, baseSize, false);
                originalMsg->bufferSize = bufBSize;
                // ocrPolicyMsgUnMarshallMsg((u8*)handle->response, NULL, originalMsg, MARSHALL_ADDL);
                // ocrPolicyMsgMarshallMsg(handle->response, baseSize, (u8*)originalMsg, MARSHALL_DUPLICATE);
                self->fcts.pdFree(self, handle->response);
            }

            if ((originalMsg != msg) && (msg != handle->response)) {
                // Just double check if a copy had been made for the request and free it.
                self->fcts.pdFree(self, msg);
            }
            handle->destruct(handle);
        } else {
            // Either a one-way request or an asynchronous two-way
            DPRINTF(DEBUG_LVL_VVERB,"Sending a one-way request or response to asynchronous two-way msg @ 0x%lx to %d\n",
                    msg, (u32) msg->destLocation);

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
                    DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: Outgoing response for DB GUID 0x%lx with properties=0x%x and dest=%d\n",
                            PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(properties), (u32) msg->destLocation);
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
                    ASSERT(PD_MSG_FIELD_IO(guid.guid) == NULL_GUID);
                    // Do a full marshalling to make sure we capture paramv/depv
                    ocrMarshallMode_t marshallMode = MARSHALL_FULL_COPY;
                    sendProp |= (((u32)marshallMode) << COMM_PROP_BEHAVIOR_OFFSET);
                }
#undef PD_MSG
#undef PD_TYPE
            }

            // one-way request, several options:
            // - make a copy in sendMessage (current strategy)
            // - submit the message to be sent and wait for delivery
            self->fcts.sendMessage(self, msg->destLocation, msg, NULL, sendProp);

            //NOTE: For PD_MSG_GUID_METADATA_CLONE we do not need to set OCR_EBUSY in the
            //      message's returnDetail field as being the PD issuing the call we can
            //      rely on the PEND return status.
        }
    } else {
        // Local PD handles the message. msg's destination is curLoc
        //NOTE: 'msg' may be coming from 'self' or from a remote PD. It can
        // either be a request (that may need a response) or a response.

        bool reqResponse = !!(msg->type & PD_MSG_REQ_RESPONSE); // for correctness check
        ocrLocation_t orgSrcLocation = msg->srcLocation; // for correctness check
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
                DPRINTF(DEBUG_LVL_VVERB,"Buffer resize for response of message type 0x%lx\n",
                                        (msgInCopy->type & PD_MSG_TYPE_ONLY));
                msg = self->fcts.pdMalloc(self, baseSizeOut);
                initializePolicyMessage(msg, baseSizeOut);
                ocrPolicyMsgMarshallMsg(msgInCopy, baseSizeIn, (u8*)msg, MARSHALL_DUPLICATE);
            }
        }

        // NOTE: It is important to ensure the base processMessage call doesn't
        // store any pointers read from the request message
        ret = pdSelfDist->baseProcessMessage(self, msg, isBlocking);

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
            DPRINTF(DEBUG_LVL_VVERB,"Send response to %d after local processing of msg\n", msg->destLocation);
            ASSERT(msg->type & PD_MSG_RESPONSE);
            ASSERT((msg->type & PD_MSG_TYPE_ONLY) != PD_MSG_MGT_MONITOR_PROGRESS);
            switch(msg->type & PD_MSG_TYPE_ONLY) {
            case PD_MSG_GUID_METADATA_CLONE:
            {
                sendProp |= ASYNC_MSG_PROP;
                break;
            }
            case PD_MSG_DB_ACQUIRE:
            {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
                DPRINTF(DEBUG_LVL_VVERB,"DB_ACQUIRE: post-process response, GUID=0x%lx serialize DB's ptr, dest is %d\n",
                        PD_MSG_FIELD_IO(guid.guid), (u32) msg->destLocation);

                sendProp |= ASYNC_MSG_PROP;
#undef PD_MSG
#undef PD_TYPE
            break;
            }
            default: { }
            }
            // Send the response message
            self->fcts.sendMessage(self, msg->destLocation, msg, NULL, sendProp);

            if ((msgInCopy != NULL) && (msgInCopy != msg)) {
                // A copy of the original message had been made to accomodate
                // the response that was larger. Free the request message.
                self->fcts.pdFree(self, msgInCopy);
            }
        }
    }
    return ret;
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
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: loop shutdown neighbors[%d] is %d\n", i, (u32) self->neighbors[i]);
            PD_MSG_STACK(msgShutdown);
            getCurrentEnv(NULL, NULL, NULL, &msgShutdown);
        #define PD_MSG (&msgShutdown)
        #define PD_TYPE PD_MSG_MGT_RL_NOTIFY
            msgShutdown.destLocation = self->neighbors[i];
            msgShutdown.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(runlevel) = RL_COMPUTE_OK;
            PD_MSG_FIELD_I(properties) = RL_REQUEST | RL_BARRIER | RL_TEAR_DOWN;
            PD_MSG_FIELD_I(errorCode) = self->shutdownCode;
            DPRINTF(DEBUG_LVL_VVERB,"PD_MSG_MGT_RL_NOTIFY: send shutdown msg to %d\n", (u32) msgShutdown.destLocation);
            u8 returnCode = self->fcts.processMessage(self, &msgShutdown, true);
            ASSERT(returnCode == 0);
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
            if (runlevel == RL_GUID_OK) {
                dself->proxyTplMap = newHashtableModulo(self, 10);
            }
            if (runlevel == RL_CONFIG_PARSE) {
                // In distributed the shutdown protocol requires three phases
                // for the RL_USER_OK TEAR_DOWN. The communication worker must be
                // aware of those while computation workers can be generic and rely
                // on the switchRunlevel/callback mecanism.
                // Because we want to keep the computation worker implementation more generic
                // we request phases directly from here through the coalesced number of phases at slot 0.
                RL_ENSURE_PHASE_DOWN(self, RL_USER_OK, 0, 3);
            }
        }
        return res;
    }
}

u8 hcDistPdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ASSERT(((s32)target) > -1);
    ASSERT(message->srcLocation == self->myLocation);
    ASSERT(message->destLocation != self->myLocation);
    u32 id = worker->seqId;
    u8 ret = self->commApis[id]->fcts.sendMessage(self->commApis[id], target, message, handle, properties);
    return ret;
}

u8 hcDistPdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u32 id = worker->seqId;
    u8 ret = self->commApis[id]->fcts.pollMessage(self->commApis[id], handle);
    return ret;
}

u8 hcDistPdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    u32 id = worker->seqId;
    u8 ret = self->commApis[id]->fcts.waitMessage(self->commApis[id], handle);
    return ret;
}

ocrPolicyDomain_t * newPolicyDomainHcDist(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrCost_t *costFunction, ocrParamList_t *perInstance) {

    ocrPolicyDomainHcDist_t * derived = (ocrPolicyDomainHcDist_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainHcDist_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;

#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, statsObject, base, costFunction, perInstance);
#else
    factory->initialize(factory, base, costFunction, perInstance);
#endif
    return base;
}

void initializePolicyDomainHcDist(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                  ocrStats_t *statsObject,
#endif
                                  ocrPolicyDomain_t *self, ocrCost_t *costFunction, ocrParamList_t *perInstance) {
    ocrPolicyDomainFactoryHcDist_t * derivedFactory = (ocrPolicyDomainFactoryHcDist_t *) factory;
    // Initialize the base policy-domain
#ifdef OCR_ENABLE_STATISTICS
    derivedFactory->baseInitialize(factory, statsObject, self, costFunction, perInstance);
#else
    derivedFactory->baseInitialize(factory, self, costFunction, perInstance);
#endif
    ocrPolicyDomainHcDist_t * hcDistPd = (ocrPolicyDomainHcDist_t *) self;
    hcDistPd->baseProcessMessage = derivedFactory->baseProcessMessage;
    hcDistPd->baseSwitchRunlevel = derivedFactory->baseSwitchRunlevel;
    hcDistPd->lockDbLookup = 0;
    hcDistPd->lockTplLookup = 0;
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
    derivedBase->policyDomainFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrLocation_t, ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                                   hcDistPdSendMessage);
    derivedBase->policyDomainFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcDistPdPollMessage);
    derivedBase->policyDomainFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcDistPdWaitMessage);

    baseFactory->destruct(baseFactory);
    return derivedBase;
}

#endif /* ENABLE_POLICY_DOMAIN_HC_DIST */
