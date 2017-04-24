/**
 * @brief Simple implementation of a malloc wrapper
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_DATABLOCK_LOCKABLE

#include "ocr-hal.h"
#include "datablock/lockable/lockable-datablock.h"
#include "debug.h"
#include "ocr-comp-platform.h"
#include "ocr-datablock.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"
#include "extensions/ocr-hints.h"
#include "experimental/ocr-platform-model.h"
#include "hc/hc.h" // For the regNode_t data-structure


#if defined (ENABLE_RESILIENCY) && defined (ENABLE_CHECKPOINT_VERIFICATION)
#include "policy-domain/hc/hc-policy.h"
#endif

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#endif

#define DEBUG_TYPE DATABLOCK

#define DEBUG_LVL_BUG DEBUG_LVL_INFO

// Distributed implementation of lockable datablock. On creation the DB
// is bound to a PD. Either the current PD or the one declared through
// the affinity hint. Other PDs must require a clone of the DB metadata
// and go through the acquire/release process to gain access to data.
// Even in EW there can be multiple users since it's always valid to
// acquire the DB in RO.

// Macros that can be defined:
// - LOCKABLE_RELEASE_ASYNC: Experimental for asynchronous release

#define DFLT_PEND_MSG_Q_SIZE 4

// DB modes masks
#define EXC_MASK      2    //10
#define WR_MASK       1    //01

// DB modes declarations
#define DB_EW    3    //11
#define DB_CONST 2    //10
#define DB_RW    1    //01
#define DB_RO    0    //00

// DB internal state
// - isPrime: true if the MD is the current authority (R/W modes)
// - isActive: true if the MD is not prime but has a pointer to live DB ptr (R mode)
#define STATE_PRIME   2
#define STATE_SHARED  1
#define STATE_IDLE    0


// Type for metadata actions
#define MD_TYPE_MASTER
#define MD_TYPE_SLAVE

#define mdAction_t u64

// Actions carried out on the DB metadata
#define M_CLONE           0x1
#define M_ACQUIRE         0x2
#define M_RELEASE         0x4
#define M_DATA            0x8
#define M_DEL             0x10
#define M_SATISFY         0x20

// 'IN' size of PD_MSG_METADATA_COMM
#define MSG_MDCOMM_SZ       (_PD_MSG_SIZE_IN(PD_MSG_METADATA_COMM))

// To tweak the debug level for DB metadata
#ifndef DBG_LVL_DB_MD
#define DBG_LVL_DB_MD DEBUG_LVL_VERB
#endif

// DB State:
// - isPrime: true if the MD is the current authority (R/W modes)
// - isActive: true if the MD is not prime but has a1250 pointer to live DB ptr (R mode)

typedef struct _storage_t {
    u64 directory;
    u64 offset; // offsets * nb of bits set in directory
} storage_t;

#define STORAGE_ID_HINT (0x1)
#define STORAGE_ID_DATA (0x2)

#define HAS_STORED(ptr, name) (ptr->directory & STORAGE_ID_##name)

//TODO this hasn't been finalized and we're cheating for now because only use this for hints
//Would need to get the mask for everything from id to 0 to know where to look in the offset array
// popcnt64(ptr->directory & ((STORAGE_ID_##name)-1))
#define GET_STORAGE_PTR(ptr, name) ((char *)((ptr->directory == (STORAGE_ID_##name)) ? (((storage_t*)ptr)+1) : NULL))

/***********************************************************/
/* OCR-Lockable Datablock Hint Properties                  */
/* (Add implementation specific supported properties here) */
/***********************************************************/

u64 ocrHintPropDbLockable[] = {
#ifdef ENABLE_HINTS
    OCR_HINT_DB_AFFINITY,
    OCR_HINT_DB_EAGER
#endif
};

//Make sure OCR_HINT_COUNT_DB_LOCKABLE in lockable-datablock.h is equal to the length of array ocrHintPropDbLockable
ocrStaticAssert((sizeof(ocrHintPropDbLockable)/sizeof(u64)) == OCR_HINT_COUNT_DB_LOCKABLE);
ocrStaticAssert(OCR_HINT_COUNT_DB_LOCKABLE < OCR_RUNTIME_HINT_PROP_BITS);

typedef struct _md_pull_acquire_t {
    // Could have src location here but it is carried by the msg too
    u8 dbMode;
} md_pull_acquire_t;

typedef struct _md_push_acquire_t {
    u8 dbMode;
    bool writeBack;
    char * dbPtr; //TODO-MD-PACK
} md_push_acquire_t;

// Piggyback on the push acquire so that we can reuse the same message buffer
// Warning, these must be exactly the same because we recast the acquire into a release struct
#define md_push_release_t md_push_acquire_t

typedef struct _md_push_clone_t {
    u64 srcLocation;
    u64 size;
    u32 flags;
    bool isEager;
    storage_t storage;
} md_push_clone_t;

typedef struct _md_push_data_t {
    char * dbPtr;
} md_push_data_t;

typedef struct _md_push_satisfy_t {
    u32 waitersCount;
    char * regNodesPtr;
} md_push_satisfy_t;

//TODO-MD-RESOLVE
static u8 lockableSerialize(ocrObjectFactory_t * factory, ocrGuid_t guid,
                     ocrObject_t * src, u64 * mode, ocrLocation_t destLocation,
                     void ** destBuffer, u64 * destSize);

/******************************************************/
/* OCR-Lockable Datablock                             */
/******************************************************/

static u8 getDbMode(ocrDbAccessMode_t accessMode) {
    switch(accessMode) {
        case DB_MODE_CONST:
            return DB_CONST;
        case DB_MODE_RW:
            return DB_RW;
        case DB_MODE_EW:
            return DB_EW;
        case DB_MODE_RO:
            return DB_RO;
        default:
            ASSERT(false && "LockableDB: Unsupported accessMode");
            return (u8) -1;
    }
}

// Data-structure to store EDT waiting to be granted access to the DB.
//MD: Thing this would be deprecated. A 'waiter' would be a continuation
//and the continuation's would know what was blocked on acquire.
typedef struct _dbWaiter_t {
    //TODO-MD-EDT: Also need the location of the requester since the EDT can move
    ocrGuid_t guid;
    ocrFatGuid_t fguid;
    ocrLocation_t dstLoc;
    u32 slot;
    u32 properties; // properties specified with the acquire request
    bool isInternal;
    struct _dbWaiter_t * next;
} dbWaiter_t;

static void enqueueLocalAcquire(ocrPolicyDomain_t * pd, ocrFatGuid_t dstGuid, ocrLocation_t dstLoc,
                                u32 dstSlot, bool isInternal, u32 properties, dbWaiter_t ** queue) {
    ASSERT(queue != NULL);
    dbWaiter_t * waiterEntry = (dbWaiter_t *) pd->fcts.pdMalloc(pd, sizeof(dbWaiter_t));
    waiterEntry->fguid = dstGuid;
    waiterEntry->dstLoc = dstLoc;
    waiterEntry->slot = dstSlot;
    waiterEntry->isInternal = isInternal;
    waiterEntry->properties = properties;
    waiterEntry->next = *queue;
    *queue = waiterEntry;
}

static void enqueueRemoteAcquire(ocrPolicyDomain_t * pd, ocrPolicyMsg_t * msg, Queue_t ** queuePtr) {
    Queue_t * queue = *queuePtr;
    if (queue == NULL) {
        queue = newBoundedQueue(pd, DFLT_PEND_MSG_Q_SIZE);
        *queuePtr = queue;
    }
    if (queueIsFull(queue)) {
        queue = queueDoubleResize(queue, /*freeOld=*/true);
        *queuePtr = queue;
    }
    queueAddLast(queue, msg);
}

// Low level acquire, all the work regarding the legality
// of the acquire must have been done upfront.
static void lowLevelAcquire(ocrDataBlock_t *self, void** ptr, ocrFatGuid_t edt, u32 edtSlot,
                  u8 dbMode, bool isInternal, u32 properties) {
    ocrDataBlockLockable_t *rself = (ocrDataBlockLockable_t*) self;
    rself->attributes.numUsers += 1;
    DPRINTF(DEBUG_MSK_EDTSTATS, "Acquiring DB @ 0x%"PRIx64" (GUID: "GUIDF") size %"PRId64" from EDT (GUID: "GUIDF") (runtime acquire: %"PRId32") (mode: %"PRId32") (numUsers: %"PRId32") (dbMode: %"PRId32")\n",
            (u64)self->ptr, GUIDA(rself->base.guid), rself->base.size, GUIDA(edt.guid), (u32)isInternal, (int) dbMode,
            rself->attributes.numUsers, rself->attributes.dbMode);
#ifdef OCR_ENABLE_STATISTICS
    {
        statsDB_ACQ(pd, edt.guid, (ocrTask_t*)edt.metaDataPtr, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */
    *ptr = self->ptr;
}

// Low level release, all the work regarding the legality
// of the release must have been done upfront.
static void lowLevelRelease(ocrDataBlock_t *self, ocrDataBlockLockableAttr_t * attr) {
#ifdef OCR_ASSERT
    if (attr->numUsers == 0) {
        DPRINTF(DEBUG_LVL_WARN, "Illegal release detected on DB "GUIDF": "
                "Either it has not been acquired or too many releases have been called\n",
                GUIDA(self->guid));
        ASSERT(false);
    }
#endif
    attr->numUsers -= 1;
    ocrDataBlockLockable_t *rself  __attribute__((unused)) = (ocrDataBlockLockable_t*) self;
    DPRINTF(DEBUG_LVL_VERB, "Release DB @ 0x%"PRIx64" (GUID: "GUIDF") (numUsers: %"PRId32") (dbMode: %"PRId32")\n",
            (u64)self->ptr, GUIDA(rself->base.guid),
            rself->attributes.numUsers, rself->attributes.dbMode);
}

static u8 lockableMdSize(ocrObject_t * dest, u64 mode, u64 * size);


// Send a release message to mdPeer
static void issueReleaseRequest(ocrDataBlock_t * self) {
    ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t*) self;
    ASSERT(rself->attributes.hasPeers);
    ocrPolicyDomain_t *pd;
    ocrPolicyMsg_t * msg;
    self->ptr = NULL; // Important to nullify for the destruct call
    ASSERT(rself->backingPtrMsg != NULL);
    msg = rself->backingPtrMsg;
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    rself->backingPtrMsg = NULL;
    u64 mode = PD_MSG_FIELD_I(mode);
    // The mode would have been pre-setup at the DB creation
    // when we knew the context in which we were operating
    if (mode & (M_CLONE | M_DATA)) {
        // PD_MSG_FIELD_I(sizePayload) has been set at creation's time
        DPRINTF(DBG_LVL_DB_MD, "db-md: push local clone "GUIDF"\n", GUIDA(self->guid));
        DPRINTF(DBG_LVL_DB_MD, "issueReleaseRequest msgSize=%"PRIu64" sizePayload=%"PRIu32"\n", msg->usefulSize, PD_MSG_FIELD_I(sizePayload));
    } else {
        ASSERT(mode & M_ACQUIRE); //Don't really like this. We piggy-back on
        //the message that brought the DB in. Ut was an acquire and now we release.
        mode = M_RELEASE;
        u64 sizePayload; // accounts for whether or not data is written-back
        lockableMdSize((ocrObject_t *) self, mode, &sizePayload);
        PD_MSG_FIELD_I(sizePayload) = sizePayload;
        md_push_release_t * payload = (md_push_release_t *) &PD_MSG_FIELD_I(payload);
        ASSERT(((payload->dbMode & WR_MASK) & !(rself->attributes.flags & DB_PROP_SINGLE_ASSIGNMENT)) ? rself->attributes.writeBack : !rself->attributes.writeBack);
        DPRINTF (DBG_LVL_DB_MD, "db-md: push release "GUIDF" in dbMode=%d\n", GUIDA(self->guid), payload->dbMode);
    }
    getCurrentEnv(&pd, NULL, NULL, NULL);

    // Fill in this call specific arguments
    ocrLocation_t destLocation = rself->mdPeers;
    ASSERT(destLocation != INVALID_LOCATION);
    msg->destLocation = destLocation;
    msg->srcLocation = pd->myLocation;
#ifdef LOCKABLE_RELEASE_ASYNC
    msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
#else
    msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
#endif
    PD_MSG_FIELD_I(guid) = self->guid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
    PD_MSG_FIELD_I(mode) = mode;
    PD_MSG_FIELD_I(factoryId) = self->fctId;
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    // Note: here we do not serialize because we're actually reusing the original message
    // to send the release. 'lockableMdSize' takes into account the fact we're writing back
    // or not to set the appropriate message payload size.
    // We also kept the writeback flag on so that the recipient knows we're writing back.

    //TODO-MD-SENDCPY: this is an example of how bad the current interfaces are.
    //We can't convey to process message we do not want the message
    //to be copied since it's already persistent, so use send message
#ifdef LOCKABLE_RELEASE_ASYNC
    pd->fcts.sendMessage(pd, msg->destLocation, msg, NULL, PERSIST_MSG_PROP);
#else
    rself->attributes.isReleasing = true;
    DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] doing issueReleaseRequest\n", GUIDA(self->guid));
    ocrWorker_t * curWorker = rself->worker;
    rself->worker = NULL;
    hal_unlock(&rself->lock);
    pd->fcts.processMessage(pd, msg, true);
    // The message was the original message that brought the data in
    // Because we used processMessage to get a blocking call, a copy has
    // been made and we must now free the message pointer.
    pd->fcts.pdFree(pd, msg);
    hal_lock(&rself->lock);
    rself->worker = curWorker;
    rself->attributes.isReleasing = false;
#endif

#undef PD_MSG
#undef PD_TYPE
}

// Sends a message to mdPeers requesting acquisition of the DB in the specified mode.
// - Flips the isFetching flag.
// - The DB lock must be held by the caller
static void issueFetchRequest(ocrDataBlock_t * self, u8 othMode) {
    ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t*) self;
    ASSERT(!rself->attributes.isFetching);
    rself->attributes.isFetching = true;
    // Create a policy-domain message
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    // Fill in this call specific arguments
    ocrLocation_t destLocation = rself->mdPeers;
    ASSERT(destLocation != INVALID_LOCATION);
    msg.destLocation = destLocation;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg.type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(guid) = self->guid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PULL;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
    PD_MSG_FIELD_I(mode) = M_ACQUIRE;
    PD_MSG_FIELD_I(factoryId) = self->fctId;
    PD_MSG_FIELD_I(sizePayload) = sizeof(md_pull_acquire_t);
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    DPRINTF(DBG_LVL_DB_MD, "db-md: pull acquire "GUIDF" in mode=%d isEager=%d\n", GUIDA(self->guid), othMode, rself->attributes.isEager);
    // Create a M_ACQUIRE PULL payload
    md_pull_acquire_t * payload = (md_pull_acquire_t *) &PD_MSG_FIELD_I(payload);
    payload->dbMode = othMode;
    //TODO-MD-SLAB we try and use the stack-allocated message because we kind of know it's large enough.
    // This should be replaced either by a dynamic check here or systematically call a fast runtime allocator
    ASSERT((ocrPolicyMsgGetMsgBaseSize(&msg, true) + sizeof(md_pull_acquire_t)) < sizeof(ocrPolicyMsg_t));
    // Send the request
    //TODO-MD-SENDCPY could we just send the message here instead of going through the PD ?
    pd->fcts.processMessage(pd, &msg, true);
#undef PD_MSG
#undef PD_TYPE
}

static void answerFetchRequest(ocrDataBlock_t * self, ocrLocation_t destLocation, u8 othMode) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    u64 mdMode = M_ACQUIRE;
    u64 mdSize;
    lockableMdSize((ocrObject_t *) self, mdMode, &mdSize);
    u64 msgSize = MSG_MDCOMM_SZ + mdSize;
    ocrPolicyMsg_t * msg;
    PD_MSG_STACK(msgStack);
    u32 msgProp = 0;
    if (msgSize > sizeof(ocrPolicyMsg_t)) {
        //TODO-MD-SLAB
        msg = (ocrPolicyMsg_t *) allocPolicyMsg(pd, &msgSize);
        initializePolicyMessage(msg, msgSize);
        getCurrentEnv(NULL, NULL, NULL, msg);
        msgProp = PERSIST_MSG_PROP;
    } else {
        msg = &msgStack;
        getCurrentEnv(NULL, NULL, NULL, &msgStack);
    }

    // Fill in this call specific arguments
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    msg->destLocation = destLocation;
    PD_MSG_FIELD_I(guid) = self->guid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
    PD_MSG_FIELD_I(mode) = mdMode;
    PD_MSG_FIELD_I(factoryId) = self->fctId;
    PD_MSG_FIELD_I(sizePayload) = mdSize;
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    // Create a M_ACQUIRE PUSH payload
    md_push_acquire_t * payload = (md_push_acquire_t *) &PD_MSG_FIELD_I(payload);
    payload->dbMode = othMode;
    // This is a little artificial for now. It depends who's in charge of deciding when/where to do the writeback
    // For now write back when acquiring in one of the write mode and not a single assignment DB.
    // Technically, a SA DB should not be acquired in write mode, just depends on how much slack the runtime allows.
    payload->writeBack = !!(othMode & WR_MASK) && !(((ocrDataBlockLockable_t *)self)->attributes.flags & DB_PROP_SINGLE_ASSIGNMENT);
    DPRINTF (DBG_LVL_DB_MD, "db-md: push acquire "GUIDF" wb=%d dbMode=%d msgSize=%"PRIu64" dbSize=%"PRIu64"\n", GUIDA(self->guid), payload->writeBack, othMode, msgSize, self->size);
#ifdef DB_STATS_LOCKABLE
    ((ocrDataBlockLockable_t *)self)->stats.counters[CNT_REMOTE_ACQUIRE]++;
#endif
    void *dataPtr;
    ocrFatGuid_t fguid = {.guid = NULL_GUID, .metaDataPtr = NULL};
    lowLevelAcquire(self, &dataPtr, fguid, EDT_SLOT_NONE, othMode, false, 0);

    ocrObjectFactory_t * factory = pd->factories[self->fctId];
    lockableSerialize(factory, self->guid, (ocrObject_t *) self, &mdMode, destLocation, (void **) &payload, &mdSize);

    // Send the request
    pd->fcts.sendMessage(pd, msg->destLocation, msg, NULL, msgProp);
#undef PD_MSG
#undef PD_TYPE
}

// Sends a message containing basic information regarding the DB
// Note that clone request and first acquire are decoupled. Should try to factorize
static void answerCloneRequest(ocrDataBlock_t * self, ocrLocation_t srcLocation) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    // Get the serialized size
    u64 mdMode = M_CLONE;
    u64 mdSize;
    lockableMdSize((ocrObject_t *) self, mdMode, &mdSize);
    u64 msgSize = MSG_MDCOMM_SZ + mdSize;
    // Check if we use stack or heap allocated message
    ocrPolicyMsg_t * msg;
    PD_MSG_STACK(msgStack);
    u32 msgProp = 0;
    if (msgSize > sizeof(ocrPolicyMsg_t)) {
        //TODO-MD-SLAB
        msg = (ocrPolicyMsg_t *) allocPolicyMsg(pd, &msgSize);
        initializePolicyMessage(msg, msgSize);
        getCurrentEnv(NULL, NULL, NULL, msg);
        msgProp = PERSIST_MSG_PROP;
    } else {
        msg = &msgStack;
        getCurrentEnv(NULL, NULL, NULL, &msgStack);
    }
    DPRINTF(DBG_LVL_DB_MD,"answerCloneRequest "GUIDF" to PD=0x%d\n", GUIDA(self->guid), (int)srcLocation);
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    msg->destLocation = srcLocation;
    PD_MSG_FIELD_I(guid) = self->guid;
    PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
    PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
    PD_MSG_FIELD_I(mode) = mdMode;
    PD_MSG_FIELD_I(factoryId) = self->fctId;
    PD_MSG_FIELD_I(sizePayload) = mdSize;
    PD_MSG_FIELD_I(response) = NULL;
    PD_MSG_FIELD_I(mdPtr) = NULL;
    // Create a M_CLONE PUSH payload
    md_push_clone_t * payload = (md_push_clone_t *) &PD_MSG_FIELD_I(payload);
    ocrObjectFactory_t * factory = pd->factories[self->fctId];
    lockableSerialize(factory, self->guid, (ocrObject_t *) self, &mdMode, srcLocation, (void **) &payload, &mdSize);
    pd->fcts.sendMessage(pd, srcLocation, msg, NULL, msgProp);
#undef PD_MSG
#undef PD_TYPE
}

// Grant access to remote acquire requests queued up on a given mode
static void processRemoteAcquireCallbacks(ocrDataBlock_t *self, Queue_t * queue, u8 dbMode, bool processAll) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyMsg_t * msg = (ocrPolicyMsg_t *) queueRemoveLast(queue);
    ocrLocation_t destLocation = msg->srcLocation;
    // Generate a push acquire message to grand access
    answerFetchRequest(self, destLocation, dbMode);
    pd->fcts.pdFree(pd, msg);
#ifdef OCR_ASSERT
    if (dbMode & WR_MASK) {
        // Current impl only allows a single MD to get write access
        // and ensures a single write request per MD is sent out.
        u32 sz = queueGetSize(queue);
        u32 i=0;
        while ((i < sz) && processAll) {
            ocrPolicyMsg_t * msg = queueGet(queue, i);
            ASSERT(msg->srcLocation != destLocation);
            i++;
        }
    }
#endif
    // For read-only we can share across multiple MDs
    while (processAll && !queueIsEmpty(queue)) {
        ocrPolicyMsg_t * msg = (ocrPolicyMsg_t *) queueRemoveLast(queue);
        ASSERT(msg != NULL);
        //TODO-MD-COLLECTIVE this is too inefficient in the case of giving out a RD copy to multiple recipients
        answerFetchRequest(self, msg->srcLocation, dbMode);
        pd->fcts.pdFree(pd, msg);
    }
}

// Grant access to local acquire requests queued up on a given mode
static dbWaiter_t * processLocalAcquireCallbacks(ocrDataBlock_t *self, dbWaiter_t * waiter, bool processAll) {
    ocrFatGuid_t dbGuid = {.guid = self->guid, .metaDataPtr = self};
    u64 dbSize = self->size;
    ocrPolicyDomain_t * pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, NULL);
    u8 dbMode = ((ocrDataBlockLockable_t *)self)->attributes.dbMode;
    do {
        ASSERT(waiter->slot != EDT_SLOT_NONE);
        //MD: Different approaches here:
        // 1- The acquire's continuation is handled as: call processMessage on incoming
        //    response answer which the PD dispatches here.
        //    Also, we need to trigger any other acquire that were blocked on pulling the md.
        // 2- When the ACQUIRE_MSG was transformed into calling acquire on this DB,
        //    we did create a PULL_MSG and did a WAIT_FOR on the PULL_MSG.
        //    It means the acquire didn't leave the PD but PULL did and its
        //    continuation is jumping back there. Over there we can make sure
        //    the db ptr is setup, transition the state of the DB and resume
        //    the other blocked acquire (how since they are internal ?).
        getCurrentEnv(NULL, NULL, NULL, &msg);
        ASSERT(waiter->dstLoc == pd->myLocation);
        //BUG #273: The In/Out nature of certain parameters is exposed here
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
        msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_RESPONSE;
        PD_MSG_FIELD_IO(guid) = dbGuid;
        PD_MSG_FIELD_IO(edt) = waiter->fguid;
        PD_MSG_FIELD_IO(destLoc) = waiter->dstLoc;
        PD_MSG_FIELD_IO(edtSlot) = waiter->slot;
        // In this implementation properties encodes the MODE + isInternal +
        // any additional flags set by the PD (such as the FETCH flag)
        PD_MSG_FIELD_IO(properties) = waiter->properties;
        // A response msg is being built, must set all the OUT fields
        PD_MSG_FIELD_O(size) = dbSize;
        PD_MSG_FIELD_O(returnDetail) = 0;
        //NOTE: we still have the lock, finalize the acquire
        lowLevelAcquire(self, &PD_MSG_FIELD_O(ptr), PD_MSG_FIELD_IO(edt),
                        PD_MSG_FIELD_IO(edtSlot), dbMode, waiter->isInternal,
                        PD_MSG_FIELD_IO(properties));
#undef PD_MSG
#undef PD_TYPE
        dbWaiter_t * next = waiter->next;
        pd->fcts.pdFree(pd, waiter);
        waiter = next;
        //TODO-MD-MT this could become a MT since we have already accounted for the acquire, no races here
        RESULT_ASSERT(pd->fcts.processMessage(pd, &msg, true), ==, 0);
    } while ((waiter != NULL) && processAll);
    return waiter;
}

// Check if there's any pending acquire request to fulfill.
// This impl favors writers over readers.
static bool scheduleLocalPendingAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, dbWaiter_t ** waitQueues) {
    //MD: Here we need the continuation of the acquire that
    //went off-PD, to walk the right waiter queue when resolved.
    //Also, at that point it's not very clear how we're going
    //to dispatch an acquire call that we want to block to the right queue.
    //Either we maintain a bunch of queues or we would have a selector
    //continuation. i.e. we have a bunch of continuations registered on the
    //original acquire event, but we only want to enable selected one.
    //What to do with the others then ?
    // Piggy-back on the rules for acquiring
    ASSERT((attr->state == STATE_PRIME) || (attr->state == STATE_SHARED));
    bool notSharedRead = !((attr->state == STATE_SHARED) && !(attr->dbMode & WR_MASK));
    u8 dbMode;
    if ((waitQueues[DB_RW] != NULL) && notSharedRead) {
        dbMode = DB_RW;
    } else if ((waitQueues[DB_EW] != NULL) && notSharedRead) {
        dbMode = DB_EW;
    } else if (waitQueues[DB_CONST] != NULL) {
        dbMode = DB_CONST;
    } else if (waitQueues[DB_RO] != NULL) {
        dbMode = DB_RO;
    } else {
        // There was no writes nor reads available
        return false;
    }
    dbWaiter_t * waiters = waitQueues[dbMode];
    attr->dbMode = dbMode;

    // Process all pending callback for that mode, except for EW
    waitQueues[dbMode] = processLocalAcquireCallbacks(self, waiters, (dbMode != DB_EW));
    ASSERT((dbMode != DB_EW) ? (waitQueues[dbMode] == NULL) : 1);
    if (waitQueues[DB_RO] != NULL) {
        // RO just piggybacks on the other state/mode
        waitQueues[DB_RO] = processLocalAcquireCallbacks(self, waiters, /*all=*/true);
        ASSERT(waitQueues[DB_RO] == NULL);
    }
    return true;
}

// Returns true if any acquire was eligible
static bool scheduleRemotePendingAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, Queue_t ** waitQueues) {
    // Try to process some of the remote
    ASSERT(!attr->hasPeers);
    // - Go over the remote pending acquire messages
    ASSERT(attr->state == STATE_PRIME);
    u8 dbMode;
    if (!queueIsEmpty(waitQueues[DB_RW])) {
        dbMode = DB_RW;
    } else if (!queueIsEmpty(waitQueues[DB_EW])) {
        dbMode = DB_EW;
    } else if (!queueIsEmpty(waitQueues[DB_CONST])) {
        dbMode = DB_CONST;
    } else if (!queueIsEmpty(waitQueues[DB_RO])) {
        dbMode = DB_RO;
    } else {
        // There was no writes nor reads available
        return false;
    }
    // - Transition the current MD to the corresponding state
    attr->dbMode = dbMode;
    attr->state = STATE_SHARED;

    // Process all pending callback for that mode, except for RW and EW since
    // only a single node can get the DB in write at a time.
    Queue_t * waiters = waitQueues[dbMode];
    processRemoteAcquireCallbacks(self, waiters, dbMode, !(dbMode & WR_MASK));
    ASSERT((!(dbMode & WR_MASK)) ? queueIsEmpty(waitQueues[dbMode]) : 1);
    if (!(dbMode & WR_MASK)) {
        // If we scheduled one of the read only modes, the other one is eligible too.
        u8 othMode = ((dbMode == DB_RO) ? DB_CONST : DB_RO);
        if (!queueIsEmpty(waitQueues[othMode])) {
            processRemoteAcquireCallbacks(self, waitQueues[othMode], othMode, /*all=*/true);
        }
        ASSERT(queueIsEmpty(waitQueues[othMode]));
    }
    return true;
}

// Re-evaluates scheduling eligibility for pending acquires
// - whenever release reaches zero users
// - When the MD becomes idle and has privileges
// - When the MD gains privileges back,
static bool schedulePendingAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t *) self;
    ASSERT(attr->state != STATE_IDLE); // Caller should have transitioned state
    ASSERT(attr->numUsers == 0);
    dbWaiter_t ** localWaitQueues = (dbWaiter_t **) rself->localWaitQueues;
    bool hadAcquire = scheduleLocalPendingAcquire(self, attr, localWaitQueues);
    if (!hadAcquire && !attr->hasPeers) {
        hadAcquire = scheduleRemotePendingAcquire(self, attr, rself->remoteWaitQueues);
        // If nothing found, keep the MD idle
    }
    return hadAcquire;
}

// Returns boolean indicating if acquire is granted
static bool localAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode);
static void localRelease(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr);
static bool remoteAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode);
static void remoteRelease(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr);

//
// Prime rules for local & remote acquire/release.
//

// Returns if granted or not
static bool localAcquirePrime(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    ASSERT((!attr->isEager || !(othMode & WR_MASK)) && "Limitation db-eager only support read modes");

    if (attr->dbMode == DB_RO) {
        // Transition to anything asked
        attr->dbMode = othMode;
        return true;
    }

    if ((attr->dbMode == DB_CONST) && !(othMode & WR_MASK)) {
        return true;
    }

    if ((attr->dbMode == DB_RW) && (othMode == DB_RW)) {
        return true;
    }
    ASSERT(!attr->isEager);

    // All others must be deferred
    return false;
}

// This is always received by the master
static bool remoteAcquirePrime(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    if (attr->dbMode == DB_RO) {
        // We do not check the number of users since RO doesn't guarantee anything
        // Transitions to share state in given mode
        attr->state = STATE_SHARED;
        attr->dbMode = othMode;
        return true;
    }
    if ((attr->dbMode == DB_CONST) && !(othMode & WR_MASK)) {
        attr->state = STATE_SHARED;
        return true;
    }
    // All other are deferred
    return false;
}

static void localReleasePrime(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    lowLevelRelease(self, attr);
    if (attr->numUsers == 0) { // No more users
        if (attr->hasPeers) { // slave
            // Try to schedule local eligible acquire
            attr->dbMode = DB_RO;
            if (!(attr->isEager) && !schedulePendingAcquire(self, attr)) {
                // else report to MD peer the DB is released
                attr->state = STATE_IDLE;
                issueReleaseRequest(self);
            }
        } else {
            attr->dbMode = DB_RO;
            // Invoke the transition method to check for queues
            schedulePendingAcquire(self, attr);
        }
    }
}

static void remoteReleasePrime(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    ASSERT(false && "Inconsistent remote release in remoteReleasePv");
}


//
// Shared rules for local & remote acquire/release.
//

static bool localAcquireShared(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    if (othMode & WR_MASK) {
        // Shared is granted when in prime and transitioning to shared
        return false;
    }
    // Read modes left
    if ((attr->dbMode == DB_RO) && (othMode == DB_CONST)) {
        // Transition RO => CONST
        attr->dbMode = DB_CONST;
    }
    return true;
}

static void localReleaseShared(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    lowLevelRelease(self, attr);
    if (attr->numUsers == 0) {
        if (attr->hasPeers) { // slaves
            // Transition to idle state
            // Keep the current mode for schedule to poke at.
            if (!(attr->isEager) && !schedulePendingAcquire(self, attr)) {
                // Couldn't schedule any pending work, release back to master
                attr->state = STATE_IDLE;
                attr->dbMode = DB_RO;
                issueReleaseRequest(self);
            }
        } else {
            // Transition to prime state
            attr->state = STATE_PRIME;
            attr->dbMode = DB_RO;
            schedulePendingAcquire(self, attr);
        }
    }
}

// This is always received by the master
static bool remoteAcquireShared(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    if (!(attr->dbMode & WR_MASK)) {
        if ((attr->dbMode == DB_RO) && (othMode == DB_CONST)) {
            attr->dbMode = DB_CONST;
        }
        return true;
    }
    return false;
}

// In this impl, executing a remote release in VALID state means we are the home PD
static void remoteReleaseShared(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    lowLevelRelease(self, attr);
    if (attr->numUsers == 0) {
        // No more users
        ASSERT(!attr->hasPeers);
        // home PD, transitions back to PV
        attr->state = STATE_PRIME;
        attr->dbMode = DB_RO;
        schedulePendingAcquire(self, attr);
    }
}

//
// Idle rules for local & remote acquire/release.
//

static bool localAcquireIdle(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    ASSERT(attr->hasPeers); // Only invoked on slaves
    // If home PD, wait to get our privileges back when
    // other MDs are done else, request privileges to peer
    if ((attr->hasPeers) && (!attr->isFetching)) {
        // See note about race in lockableAcquire()
        issueFetchRequest(self, othMode);
    }
    return false; // Always queue
}

void localReleaseIdle(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    ASSERT(false && "Invalid local release operation in none state");
}

// Invoked both for master and slave MDs
static bool remoteAcquireIdle(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    // Remote acquire when we are in idle mode.
    // Can't happen since a slave never receives remote acquries
    ASSERT(false);
    return false;
}

// Only for master
static void remoteReleaseIdle(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    ASSERT(false);
}

// Returns boolean indicating if acquire is granted
static bool localAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    switch (attr->state) {
        case STATE_IDLE: {
            return localAcquireIdle(self, attr, othMode);
        }
        case STATE_PRIME: {
            return localAcquirePrime(self, attr, othMode);
        }
        case STATE_SHARED: {
            return localAcquireShared(self, attr, othMode);
        }
        default:
            ASSERT(false);
    }
    return false;
}

static void localRelease(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    switch (attr->state) {
        case STATE_IDLE: {
            localReleaseIdle(self, attr);
        break;
        }
        case STATE_PRIME: {
            localReleasePrime(self, attr);
        break;
        }
        case STATE_SHARED: {
            localReleaseShared(self, attr);
        break;
        }
        default:
            ASSERT(false);
    }
}

static bool remoteAcquire(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr, u8 othMode) {
    switch (attr->state) {
        case STATE_IDLE: {
            return remoteAcquireIdle(self, attr, othMode);
        }
        case STATE_PRIME: {
            return remoteAcquirePrime(self, attr, othMode);
        }
        case STATE_SHARED: {
            return remoteAcquireShared(self, attr, othMode);
        }
        default:
            ASSERT(false);
    }
    return false;
}

static void remoteRelease(ocrDataBlock_t * self, ocrDataBlockLockableAttr_t * attr) {
    switch (attr->state) {
        case STATE_IDLE: {
            remoteReleaseIdle(self, attr);
        break;
        }
        case STATE_PRIME: {
            remoteReleasePrime(self, attr);
        break;
        }
        case STATE_SHARED: {
            remoteReleaseShared(self, attr);
        break;
        }
        default:
            ASSERT(false);
    }
}


// Forward declaration
u8 lockableDestruct(ocrDataBlock_t *self);

// This is needed because we may be in helper mode ?
static bool lockButSelf(ocrDataBlockLockable_t *rself) {
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    bool unlock = true;
    if (hal_islocked(&(rself->lock))) {
        if (worker == rself->worker) {
            // fall-through
            unlock = false;
        } else {
            hal_lock(&rself->lock);
        }
    } else {
        hal_lock(&rself->lock);
        rself->worker = worker;
    }
    return unlock;
}

// Can only be called locally to the current PD. However, the MD may or may not be able to
// accomodate the call immediately.
// Remote acquire are going through the MD cloning infrastructure and the 'process' call
u8 lockableAcquire(ocrDataBlock_t *self, void** ptr, ocrFatGuid_t edt, ocrLocation_t dstLoc, u32 edtSlot,
                  ocrDbAccessMode_t accessMode, bool isInternal, u32 properties) {
    u8 res = 0;
    ocrDataBlockLockable_t *rself = (ocrDataBlockLockable_t*)self;
    bool unlock = lockButSelf(rself);
    u8 othMode = getDbMode(accessMode);
    // When we're a clone MD it's easy to use isFetching to shortcut whether or not to grant.
    // It doesn't cover all of them but it's cheap enough to do it here.
    bool granted = (!rself->attributes.isFetching) && (!rself->attributes.isReleasing) &&
                    localAcquire(self, &rself->attributes, othMode);
    if (granted) { // Enqueue acquire request
        // Do not touch the state here. For local MD the state doesn't change and in
        // remote the state is set before executing callbacks
#ifdef DB_STATS_LOCKABLE
        rself->stats.counters[CNT_LOCAL_ACQUIRE]++;
#endif
        // Registers first intent to acquire a SA block in writable mode
        if (othMode & WR_MASK) {
            if (rself->attributes.singleAssign) {
                DPRINTF(DEBUG_LVL_WARN, "Cannot re-acquire SA DB (GUID "GUIDF") for EDT "GUIDF" in writable mode %"PRIu32"\n", GUIDA(self->guid), GUIDA(edt.guid), (u32)accessMode);
                ASSERT(false && "OCR_EACCES");
                if (unlock) {
                    rself->worker = NULL;
                    hal_unlock(&rself->lock);
                }
                return OCR_EACCES;
            } else if ((self->flags & DB_PROP_SINGLE_ASSIGNMENT) != 0) {
                rself->attributes.singleAssign = 1;
#ifdef ENABLE_RESILIENCY
                ASSERT(self->bkPtr == NULL);
                self->singleAssigner = edt.guid;
                DPRINTF(DEBUG_LVL_VERB, "DB (GUID "GUIDF") single assign from EDT "GUIDF"\n", GUIDA(rself->base.guid), GUIDA(edt.guid));
#endif
            }
        }
        lowLevelAcquire(self, ptr, edt, edtSlot, othMode, isInternal, properties);
        //TODO-MD-DBRTACQ
        //Came from a gated acquire waiting on MD, generate a response.
        if (properties & DB_PROP_ASYNC_ACQ) {
            ocrPolicyDomain_t *pd = NULL;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            dbWaiter_t * dbWaiter = (dbWaiter_t *) pd->fcts.pdMalloc(pd, sizeof(dbWaiter_t));
            dbWaiter->fguid = edt;
            dbWaiter->dstLoc = dstLoc;
            dbWaiter->slot = edtSlot;
            dbWaiter->properties = properties;
            dbWaiter->isInternal = isInternal;
            dbWaiter->next = NULL;
            processLocalAcquireCallbacks(self, dbWaiter, false);
        }
    } else { // Not deferred
        // Note there's a race here between asking to update the MD
        // and enqueuing the acquire for further processing. This is
        // currently addressed by owning the lock on the datablock.
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        enqueueLocalAcquire(pd, edt, dstLoc, edtSlot, isInternal, properties, &(rself->localWaitQueues[othMode]));
        res = OCR_EBUSY;
    }
    if (unlock) {
        rself->worker = NULL;
        hal_unlock(&rself->lock);
    }
    return res;
}

// Always called by release local to the current PD
// 'edt' may be NULL_GUID here if we are doing a PD-level release
u8 lockableRelease(ocrDataBlock_t *self, ocrFatGuid_t edt, ocrLocation_t srcLoc, bool isInternal) {
    ocrDataBlockLockable_t *rself = (ocrDataBlockLockable_t*)self;
    DPRINTF(DEBUG_LVL_VERB, "Releasing DB @ 0x%"PRIx64" (GUID "GUIDF") from EDT "GUIDF" (runtime release: %"PRId32")\n",
            (u64)self->ptr, GUIDA(rself->base.guid), GUIDA(edt.guid), (u32)isInternal);
    // Start critical section
    hal_lock(&(rself->lock));
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    rself->worker = worker;
#ifdef DB_STATS_LOCKABLE
    rself->stats.counters[CNT_LOCAL_RELEASE]++;
#endif
#ifdef ENABLE_RESILIENCY
    // u8 curMode = rself->attributes.dbMode;
#endif
#ifdef ENABLE_RESILIENCY
    //TODO-resiliency: We don't need to have self->singleAssigner because we
    // can detect the transition from write to read for the SA datablock.
    //TODO-resiliency: For shared-memory OCR we can take the snapshot after localRelease.
    // For distributed first it depends what the resilience implementation requires for a
    // remote release and if it want to keep a local backup or not. If it needs a local snapshot
    // some refactoring is necessary here.

    // Take backup for resiliency. Handle only single assignment DBs for now.
    // Detect the transition from write to read to trigger the backup.
    // if (((self->flags & DB_PROP_SINGLE_ASSIGNMENT) != 0) && ((curMode & WR_MASK) && !(rself->attributes.dbMode & WR_MASK))) {
    if (((self->flags & DB_PROP_SINGLE_ASSIGNMENT) != 0) && ocrGuidIsEq(self->singleAssigner, edt.guid)) {
        ASSERT(rself->attributes.singleAssign == 1 && self->bkPtr == NULL);
        ocrPolicyDomain_t * pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        self->bkPtr = pd->fcts.pdMalloc(pd, self->size);
        hal_memCopy(self->bkPtr, self->ptr, self->size, 0);
        DPRINTF(DEBUG_LVL_VERB, "DB (GUID "GUIDF") backed up from EDT "GUIDF"\n", GUIDA(rself->base.guid), GUIDA(edt.guid));
    }
#endif
    localRelease(self, &rself->attributes);
    DPRINTF(DEBUG_LVL_VVERB, "DB (GUID: "GUIDF") attributes: numUsers %"PRId32" freeRequested %"PRId32"\n",
            GUIDA(self->guid), rself->attributes.numUsers, rself->attributes.freeRequested);
#ifdef OCR_ENABLE_STATISTICS
    {
        statsDB_REL(getCurrentPD(), edt.guid, (ocrTask_t*)edt.metaDataPtr, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */
    //Cannot be here and releasing since self would have put
    //the flag to false and concurrent release from other workers
    // would have lost the remote release competition
    ASSERT(!rself->attributes.isReleasing);
    if (rself->attributes.numUsers == 0) {
        if (rself->attributes.hasPeers) { // slave
            ASSERT(rself->attributes.state == STATE_IDLE);
            // If we still have users, then whoever checkout last will resume.
            // While we were doing the release and potentially block through the master-helper,
            // the current worker sat the isReleasing flag and released the lock . Other workers,
            // including the current worker, could have executed acquire call. In either case, they
            // would have acquired the lock and be gated on the isReleasing flag.
            // If we've reached zero users, then we must look at the queues to resume one of
            // the acquire.
            dbWaiter_t ** waitQueues = (dbWaiter_t **) rself->localWaitQueues;
            u8 dbMode = ((u8)-1);
            if ((waitQueues[DB_RW] != NULL)) {
                dbMode = DB_RW;
            } else if ((waitQueues[DB_EW] != NULL)) {
                dbMode = DB_EW;
            } else if (waitQueues[DB_CONST] != NULL) {
                dbMode = DB_CONST;
            } else if (waitQueues[DB_RO] != NULL) {
                dbMode = DB_RO;
            }
            if (dbMode != ((u8)-1)) {
                ASSERT((!rself->attributes.freeRequested) && "Datablock user-level error: concurrent acquire and deletion detected");
                ASSERT((!rself->attributes.isFetching) && "Datablock internal error: concurrent fetch and acquire");
                // This will send out an acquire request to the master MD for that dbMode
                // hence return code being false as no successful local acquire
                DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] trigger fetch\n", GUIDA(self->guid));
                RESULT_ASSERT(localAcquire(self, &rself->attributes, dbMode), ==, false);
            } // else stay idle
        }
        // Check if we need to free the block
        if (rself->attributes.freeRequested == 1) {
            rself->worker = NULL;
            hal_unlock(&(rself->lock));
            DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] %p destruct from release\n", GUIDA(self->guid), self);
            return lockableDestruct(self);
        }

    }
    rself->worker = NULL;
    hal_unlock(&(rself->lock));
    return 0;
}

static void setTrackID(u64 * tracker, ocrLocation_t locId) {
    u64 id = (u64) locId;
    tracker[id/64] |=  (1ULL << (id % 64));
}

static void clearTrackID(u64 * tracker, ocrLocation_t locId) {
    u64 id = ((u64) locId);
    tracker[id/64] &= ~(1ULL << (id % 64));
}

// Distributed destruction of metadata peers
static void issueDelMessage(ocrDataBlockLockable_t * rself, ocrLocation_t srcToAvoid) {
    ocrDataBlock_t * self = (ocrDataBlock_t *) rself;
    if (rself->attributes.hasPeers) {
        // Slave has received the free call, notify master
        ocrPolicyDomain_t * pd;
        ocrPolicyMsg_t msg;
        getCurrentEnv(&pd, NULL, NULL, &msg);
        ocrLocation_t destLocation;
        pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], self->guid, &destLocation);
        msg.destLocation = destLocation;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_METADATA_COMM
        msg.type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(guid) = self->guid;
        PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
        PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
        PD_MSG_FIELD_I(mode) = M_DEL;
        PD_MSG_FIELD_I(factoryId) = self->fctId;
        PD_MSG_FIELD_I(sizePayload) = 0;
        PD_MSG_FIELD_I(response) = NULL;
        PD_MSG_FIELD_I(mdPtr) = NULL;
#undef PD_MSG
#undef PD_TYPE
        pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
    } else {
        // Master has received the free call
        // We own the lock so technically freeRequest has to be seen before we
        // can process eventual deletion acknowledgements
        // Remove the source from the tracker
        if (srcToAvoid != INVALID_LOCATION) {
            clearTrackID(rself->mdLocTracker, srcToAvoid);
        }

        // Send M_DEL to all remaining slaves
        u64 cur = rself->mdLocTracker[0];
        if (cur) {
            ocrPolicyDomain_t * pd;
            ocrPolicyMsg_t msg;
            getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_METADATA_COMM
            msg.type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(guid) = self->guid;
            PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
            PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
            PD_MSG_FIELD_I(mode) = M_DEL;
            PD_MSG_FIELD_I(factoryId) = self->fctId;
            PD_MSG_FIELD_I(sizePayload) = 0;
            PD_MSG_FIELD_I(response) = NULL;
            PD_MSG_FIELD_I(mdPtr) = NULL;
#undef PD_MSG
#undef PD_TYPE
            u32 i=0;
            do {
                u8 nbShift = 0;
                while(cur != 0) {
                    if (cur & 1ULL) {
                        ocrLocation_t destLoc = (ocrLocation_t) ((i*64)+nbShift);
                        ASSERT(destLoc != srcToAvoid);
                        DPRINTF(DBG_LVL_DB_MD, "(GUID: "GUIDF") Notify location %d for M_DEL at %d\n", GUIDA(self->guid), (int) destLoc, (int) i);
                        getCurrentEnv(NULL, NULL, NULL, &msg);
                        msg.destLocation = destLoc;
                        pd->fcts.sendMessage(pd, msg.destLocation, &msg, NULL, 0);
                    }
                    cur >>= 1;
                    nbShift++;
                }
                i++;
                if (i < DB_MAX_LOC_ARRAY) {
                    cur = rself->mdLocTracker[i];
                }
            } while (cur);
        }
    }
}

u8 lockableDestruct(ocrDataBlock_t *self) {
    DPRINTF(DEBUG_LVL_VERB, "Freeing DB (GUID: "GUIDF")\n", GUIDA(self->guid));
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);

    ocrDataBlockLockable_t *rself = (ocrDataBlockLockable_t*)self;
    // Any of these wrong would indicate a race between free and DB's consumers
    ASSERT(rself->attributes.numUsers == 0);
    ASSERT(rself->attributes.isFetching == 0);
    ASSERT(rself->attributes.freeRequested == 1);
#ifdef OCR_ASSERT // For simpler debuggging
    if (rself->attributes.hasPeers) {
        rself->attributes.state = STATE_IDLE;
        rself->attributes.dbMode = DB_RO;
    } else {
        rself->attributes.state = STATE_PRIME;
        rself->attributes.dbMode = DB_RO;
    }
#endif
    u32 i=0;
    for(;i < DB_MODE_COUNT; i++) {
        ASSERT(rself->localWaitQueues[i] == NULL); // Linked-list so should empty
        if (rself->remoteWaitQueues[i] != NULL) {
#ifdef OCR_ASSERT
            while (!queueIsEmpty(rself->remoteWaitQueues[i])) {
                ocrPolicyMsg_t * msg __attribute__((unused)) = queueRemoveLast(rself->remoteWaitQueues[i]);
                DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] error: remote DB operation pending while DB is "
                                        "being destroyed msg src=%"PRIu64" dst=%"PRIu64" type=0x%"PRIx32"\n",
                                        GUIDA(self->guid), msg->srcLocation, msg->destLocation, msg->type);
            }
#endif
            ASSERT(queueIsEmpty(rself->remoteWaitQueues[i]));
            queueDestroy(rself->remoteWaitQueues[i]);
            rself->remoteWaitQueues[i] = NULL;
        }
    }
#ifdef DB_STATS_LOCKABLE
    DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] Remote Acquire = %"PRIu64"\n", GUIDA(self->guid), rself->stats.counters[CNT_REMOTE_ACQUIRE]);
    DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] Remote Release = %"PRIu64"\n", GUIDA(self->guid), rself->stats.counters[CNT_REMOTE_RELEASE]);
    DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] Local Acquire  = %"PRIu64"\n", GUIDA(self->guid), rself->stats.counters[CNT_LOCAL_ACQUIRE]);
    DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] Local Release  = %"PRIu64"\n", GUIDA(self->guid), rself->stats.counters[CNT_LOCAL_RELEASE]);
    DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] Eager Clone    = %"PRIu64"\n", GUIDA(self->guid), rself->stats.counters[CNT_EAGER_CLONE]);
    DPRINTF(DEBUG_LVL_WARN, "["GUIDF"] Eager Push     = %"PRIu64"\n", GUIDA(self->guid), rself->stats.counters[CNT_EAGER_PULL]);
#endif
    ASSERT(rself->lock == 0);

#ifdef ENABLE_RESILIENCY
    if(self->bkPtr) {
        pd->fcts.pdFree(pd, self->bkPtr);
        self->bkPtr = NULL;
    }
#endif

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_UNALLOC
    // Two cases here:
    // 1) If this is a clone then msg and data ptr must be null
    // 2) This is the master, it can msg/data being either
    //    !null/!null, data points to msg's payload
    //    null/!null, has never been cloned
    if (rself->backingPtrMsg != NULL) {
        pd->fcts.pdFree(pd, rself->backingPtrMsg);
    } else {
        if (self->ptr != NULL) {
            msg.type = PD_MSG_MEM_UNALLOC | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(allocatingPD.guid) = self->allocatingPD;
            PD_MSG_FIELD_I(allocatingPD.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(allocator.guid) = self->allocator;
            PD_MSG_FIELD_I(allocator.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(ptr) = self->ptr;
            PD_MSG_FIELD_I(type) = DB_MEMTYPE;
            PD_MSG_FIELD_I(properties) = 0;
            RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
        }
    }

#ifdef OCR_ENABLE_STATISTICS
    // This needs to be done before GUID is freed.
    {
        ocrTask_t *task = NULL;
        getCurrentEnv(NULL, NULL, &task, NULL);
        statsDB_DESTROY(pd, task->guid, task, self->allocator, NULL, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */
#undef PD_TYPE
#define PD_TYPE PD_MSG_GUID_DESTROY
    getCurrentEnv(NULL, NULL, NULL, &msg);
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    // These next two statements may be not required. Just to be safe
    PD_MSG_FIELD_I(guid.guid) = self->guid;
    PD_MSG_FIELD_I(guid.metaDataPtr) = self;
    PD_MSG_FIELD_I(properties) = 1; // Free metadata
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

// This is the runtime implementation for ocrDbDestroy and is invoked once per DB instance.
u8 lockableFree(ocrDataBlock_t *self, ocrFatGuid_t edt, ocrLocation_t srcLoc, u32 properties) {
    bool isInternal = ((properties & DB_PROP_RT_ACQUIRE) != 0);
    bool reqRelease = ((properties & DB_PROP_NO_RELEASE) == 0);
    ocrDataBlockLockable_t *rself = (ocrDataBlockLockable_t*)self;
    DPRINTF(DEBUG_LVL_VERB, "Requesting a free for DB @ 0x%"PRIx64" (GUID: "GUIDF"); props: 0x%"PRIx32"\n",
            (u64)self->ptr, GUIDA(rself->base.guid), properties);

#ifdef ENABLE_EXTENSION_PERF
    ocrTask_t * curEdt = NULL;
    getCurrentEnv(NULL, NULL, &curEdt, NULL);
    if(curEdt) curEdt->swPerfCtrs[PERF_DB_DESTROYS - PERF_HW_MAX] += self->size;
#endif

    hal_lock(&(rself->lock));
    if(rself->attributes.freeRequested) {
        ASSERT(false && "Internal DB free invoked multiple times");
        hal_unlock(&(rself->lock));
        return OCR_EPERM;
    }
    DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] setting freeRequest from lockableFree reqRelease=%d\n", GUIDA(self->guid), (int) reqRelease);
    rself->attributes.freeRequested = 1;
    ASSERT((rself->attributes.isFetching == 0) && "error: DB Destroy seems to be concurrent with other DB operations");
    issueDelMessage(rself, INVALID_LOCATION);
    // This is to work out the issue where an EDT is post-releasing the DB
    // and there's synchronization happening with the DB master MD. However,
    // a child EDT is trying to destroy the DB.
    if (rself->attributes.numUsers == 0) {
        if (!rself->attributes.isReleasing) {
            DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] destruct from lockableFree\n", GUIDA(self->guid));
            hal_unlock(&(rself->lock));
            return lockableDestruct(self);
        } // else do nothing, the release code will call destruct
        hal_unlock(&(rself->lock));
    } else {
        hal_unlock(&(rself->lock));
        // The datablock may not have been acquired by the current EDT hence
        // we do not need to account for a release.
        if (reqRelease) {
            DPRINTF(DEBUG_LVL_VVERB, "Free triggering release for DB @ 0x%"PRIx64" (GUID: "GUIDF")\n",
                    (u64)self->ptr, GUIDA(rself->base.guid));
            lockableRelease(self, edt, srcLoc, isInternal);
        }
    }
    return 0;
}

u8 lockableRegisterWaiter(ocrDataBlock_t *self, ocrFatGuid_t waiter, u32 slot,
                         bool isDepAdd) {
    ASSERT(0);
    return OCR_ENOSYS;
}

u8 lockableUnregisterWaiter(ocrDataBlock_t *self, ocrFatGuid_t waiter, u32 slot,
                           bool isDepRem) {
    ASSERT(0);
    return OCR_ENOSYS;
}

// Called in the following contexts:
// - Local creation of a local DB
// - Local creation of a proxy for a remote DB
//     - a) on ocrDbCreate specifying a remote affinity hint
//     - b) as a consequence of MD cloning steming from acquiring a remote DB
// - Local creation as a 'writeback' for use case 'a'
static u8 newDataBlockLockableInternal(ocrDataBlockFactory_t *factory, ocrFatGuid_t *guid, ocrFatGuid_t allocator,
                        ocrFatGuid_t allocPD, u64 size, void** ptr, ocrHint_t * hint, u32 flags,
                        ocrParamList_t *perInstance, bool isEager, bool isClone, bool firstCreate, ocrLocation_t loc) {
    ocrPolicyDomain_t *pd = NULL;
    u8 returnValue = 0;
    ocrGuid_t resultGuid = NULL_GUID;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    ocrDataBlockLockable_t *result = NULL;
    u32 hintc = (flags & DB_PROP_NO_HINT) ? 0 : OCR_HINT_COUNT_DB_LOCKABLE;
    u32 mSize = sizeof(ocrDataBlockLockable_t) + hintc*sizeof(u64);
    ocrLocation_t targetLoc = pd->myLocation;
    if (hint != NULL_HINT) {
        u64 hintValue = 0ULL;
        if ((ocrGetHintValue(hint, OCR_HINT_DB_AFFINITY, &hintValue) == 0) && (hintValue != 0)) {
            ocrGuid_t affGuid;
#if GUID_BIT_COUNT == 64
            affGuid.guid = hintValue;
#elif GUID_BIT_COUNT == 128
            affGuid.upper = 0ULL;
            affGuid.lower = hintValue;
#endif
            ASSERT(!ocrGuidIsNull(affGuid));
            affinityToLocation(&targetLoc, affGuid);
        }
    }
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = *guid;
    PD_MSG_FIELD_I(size) = mSize;
    PD_MSG_FIELD_I(targetLoc) = targetLoc;
    PD_MSG_FIELD_I(kind) = OCR_GUID_DB;
    // Note we delay recording the GUID to after the DB is fully initialized
    PD_MSG_FIELD_I(properties) = ((flags & (GUID_RT_PROP_ALL|GUID_PROP_ALL)) & ~GUID_PROP_TORECORD);
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
    result = (ocrDataBlockLockable_t*)PD_MSG_FIELD_IO(guid.metaDataPtr);
    resultGuid = PD_MSG_FIELD_IO(guid.guid);
    returnValue = PD_MSG_FIELD_O(returnDetail);
#undef PD_MSG
#undef PD_TYPE
    if(returnValue != 0) {
        return returnValue;
    }
    ASSERT(result);

    // Initialize the base's base
    result->base.base.fctId = factory->factoryId;
#ifdef ENABLE_RESILIENCY
    result->base.base.kind = OCR_GUID_DB;
    result->base.base.size = mSize;
#endif
    result->base.allocator = allocator.guid;
    result->base.allocatingPD = allocPD.guid;
    result->base.size = size;
    result->base.fctId = factory->factoryId;
    // Only keep flags that represent the nature of
    // the DB as opposed to one-time usage creation flags
    result->base.flags = (flags & DB_PROP_SINGLE_ASSIGNMENT);
    result->lock = INIT_LOCK;
    result->attributes.flags = result->base.flags;
    result->attributes.numUsers = 0;
    result->attributes.freeRequested = 0;
    result->attributes.singleAssign = 0;
#ifdef ENABLE_RESILIENCY
    result->base.bkPtr = NULL;
    result->base.singleAssigner = NULL_GUID;
#endif
    result->attributes.isFetching = false;
    result->attributes.isReleasing = false;
    result->attributes.isEager = isEager;
    result->backingPtrMsg = NULL;
    u8 i;
    for(i=0; i < DB_MODE_COUNT; i++) {
        result->localWaitQueues[i] = NULL;
    }
    for(i=0; i < DB_MODE_COUNT; i++) {
        result->remoteWaitQueues[i] = NULL;
    }
    result->worker = NULL;
    result->attributes.dbMode = DB_RO;
    result->mdPeers = loc;
    if (isClone && !isEager) {
        // Two scenario for a clone creation:
        // 1) Acquiring a remote DB the current PD do not know about yet
        // 2) Remote creation of a DB, which is staged by a local creation
        //    and a lazy remote creation on release.
        // So when we do a local on behalf of a remote create,
        // we need to allocate enough space so that we can use the backingPtr
        // as a message to be sent out on release !
        result->attributes.state = (firstCreate) ? STATE_PRIME : STATE_IDLE;
        result->attributes.hasPeers = 1;
        // This is for when the clone MD is the first instantiation of the DB.
        // In that case we need to do write back to the remote node that owns the DB.
        result->attributes.writeBack = firstCreate;
    } else {
        // For local DB creation
        result->attributes.state = STATE_PRIME;
        result->attributes.hasPeers = 0;
    }
    if (hintc == 0) {
        result->hint.hintMask = 0;
        result->hint.hintVal = NULL;
    } else {
        OCR_RUNTIME_HINT_MASK_INIT(result->hint.hintMask, OCR_HINT_DB_T, factory->factoryId);
        result->hint.hintVal = (u64*)((u64)result + sizeof(ocrDataBlockLockable_t));
        if (hint != NULL_HINT) {
            factory->fcts.setHint(((ocrDataBlock_t *) result), hint);
        }
    }
    for(i = 0; i < DB_MAX_LOC_ARRAY; ++i) {
        result->mdLocTracker[i] = 0ULL;
    }
#ifdef DB_STATS_LOCKABLE
    for(i = 0; i < CNT_MAX; ++i) {
        result->stats.counters[i] = 0;
    }
#endif
    result->nonCoherentLoc = INVALID_LOCATION;

#ifdef OCR_ENABLE_STATISTICS
    ocrTask_t *task = NULL;
    getCurrentEnv(NULL, NULL, &task, NULL);
    statsDB_CREATE(pd, task->guid, task, allocator.guid,
                   (ocrAllocator_t*)allocator.metaDataPtr, result->base.guid,
                   &(result->base));
#endif /* OCR_ENABLE_STATISTICS */

    DPRINTF(DEBUG_LVL_VVERB, "Creating a datablock of size %"PRIu64", @ 0x%"PRIx64" (GUID: "GUIDF") isEager=%d isClone=%d firstCreate=%d\n",
            size, (u64)result->base.ptr, GUIDA(result->base.guid), isEager, isClone, firstCreate);

    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_DATABLOCK, OCR_ACTION_CREATE, traceDataCreate, result->base.guid, size);
    // If the caller wants a pointer back, we are setting up a local DB.
    // In some cases, the DB has a hint to a remote location and we create
    // a local version of it before pushing it back on release.
    if ((ptr != NULL) && (*ptr == NULL)) {
        if (!isClone) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_MEM_ALLOC
            msg.type = PD_MSG_MEM_ALLOC | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_I(size) = size;
            PD_MSG_FIELD_I(properties) = 0;
            PD_MSG_FIELD_I(type) = DB_MEMTYPE;
            RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
            void * allocPtr = (void *)PD_MSG_FIELD_O(ptr);
    #undef PD_MSG
    #undef PD_TYPE
            result->base.ptr = allocPtr;
        } else {
            // This is setting up the message that's issued when the DB is released
            ASSERT((ptr != NULL) && (*ptr == NULL));
            u64 mdSize;
            lockableMdSize((ocrObject_t*) result, (M_CLONE | M_DATA), &mdSize);
            // Uses the alloc function to make sure the memory is properly
            // aligned and is compatible with the marshalling code.
            u64 msgSize = MSG_MDCOMM_SZ + mdSize;
            ocrPolicyMsg_t * msg = (ocrPolicyMsg_t *) allocPolicyMsg(pd, &msgSize);
            initializePolicyMessage(msg, msgSize);
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
            msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
            PD_MSG_FIELD_I(mode) = M_CLONE | M_DATA;
            PD_MSG_FIELD_I(sizePayload) = mdSize;
            md_push_clone_t * mdPtr = (md_push_clone_t *) &PD_MSG_FIELD_I(payload);
            // Invoke clone serialization to make sure the message is
            // properly initialized when we will issue the release request.
            u64 serMode = M_CLONE; // ignore M_DATA since the DB ptr is backed by the message.
            lockableSerialize((ocrObjectFactory_t*)factory, resultGuid, (ocrObject_t *) result, &serMode, loc, (void **) &mdPtr, NULL);
            ASSERT(mdPtr->isEager == false);
            ASSERT(mdPtr->srcLocation == pd->myLocation);
            ASSERT(mdPtr->size == result->base.size);
            ASSERT(mdPtr->flags == result->base.flags);
            mdPtr->storage.directory = 0; //TODO: this is a bug but I don't know yet why it hangs when set
#undef PD_MSG
#undef PD_TYPE
            result->backingPtrMsg = msg;
#ifdef OCR_ASSERT
            u64 tmpBaseSize = 0, tmpMarshalledSize = 0;
            ocrPolicyMsgGetMsgSize(msg, &tmpBaseSize, &tmpMarshalledSize, MARSHALL_DBPTR | MARSHALL_NSADDR);
            ASSERT(msgSize == tmpBaseSize);
#endif
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
            // Fill that in now because when we 'issueReleaseRequest' we won't know the context
            //TODO would be nice to have some indexing functions here
            md_push_data_t * pdata = (md_push_data_t *) (mdPtr+1);
            result->base.ptr = &(pdata->dbPtr);
#undef PD_MSG
#undef PD_TYPE
        }
        *ptr = result->base.ptr;
    } else {
        result->backingPtrMsg = NULL;
        // If there was a valid ptr given always use it
        ASSERT(((ptr != NULL) && (*ptr != NULL)) || (ptr == NULL));
        result->base.ptr = (ptr != NULL) ? *ptr : NULL;
        //TODO: pb on TG when the caller gave us a ptr and it is valid, should use it.
    }
    // This is a remote creation for which we create first a local MD so
    // we need to handle registration here.
    // Do this at the very end; it indicates that the object is actually valid
    hal_fence();
    result->base.guid = resultGuid;
    if (isEager || (isClone && firstCreate)) {
        // When we are creating a local clone on behalf of a remote we need
        // to setup the proxy and register the guid, ptr into the GP
        MdProxy_t * mdProxy; u64 val;
        RESULT_ASSERT(pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], result->base.guid, &val, NULL, MD_PROXY, &mdProxy), ==, 0);
        ASSERT(flags & GUID_PROP_TORECORD);
    }
    if (flags & GUID_PROP_TORECORD) {
        RESULT_ASSERT(pd->guidProviders[0]->fcts.registerGuid(pd->guidProviders[0], result->base.guid, (u64) result), ==, 0);
    }
    guid->guid = resultGuid;
    guid->metaDataPtr = result;
    return 0;
}

u8 newDataBlockLockable(ocrDataBlockFactory_t *factory, ocrFatGuid_t *guid, ocrFatGuid_t allocator,
                        ocrFatGuid_t allocPD, u64 size, void** ptr, ocrHint_t *hint, u32 flags,
                        ocrParamList_t *perInstance) {
    // No GUID provided, need to get one assigned for the DB
    ocrLocation_t othLoc = INVALID_LOCATION;
    bool isLocal = true;
    if(!(flags & GUID_PROP_IS_LABELED)) {
        u64 hintValue = 0ULL;
        if ((hint != NULL) && (ocrGetHintValue(hint, OCR_HINT_DB_AFFINITY, &hintValue) == 0) && (hintValue != 0)) {
            //TODO-MD: Overall this is kind of an expensive check for locality...
            ocrGuid_t affGuid;
    #if GUID_BIT_COUNT == 64
            affGuid.guid = hintValue;
    #elif GUID_BIT_COUNT == 128
            affGuid.upper = 0ULL;
            affGuid.lower = hintValue;
    #endif
            ASSERT(!ocrGuidIsNull(affGuid));
            affinityToLocation(&othLoc, affGuid);
            ocrPolicyDomain_t * pd = NULL;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            isLocal = (othLoc == pd->myLocation);
        }

        if (!isLocal) {
            // Reserve a GUID for the datablock to be created.
            // This is currently a remote operation but we could implement a local cache.
            ocrPolicyDomain_t *pd = NULL;
            PD_MSG_STACK(msg);
            getCurrentEnv(&pd, NULL, NULL, &msg);
            msg.destLocation = othLoc;
    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_GUID_RESERVE
            msg.type = PD_MSG_GUID_RESERVE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_I(numberGuids) = 1;
            PD_MSG_FIELD_I(guidKind) = OCR_GUID_DB;
            PD_MSG_FIELD_I(properties) = 0; // Want to generate GUID but not labeled ones
            //BUG #527: memory reclaim: There is a leak if this fails
            u8 returnCode = pd->fcts.processMessage(pd, &msg, true);
            if(!((returnCode == 0) && ((returnCode = PD_MSG_FIELD_O(returnDetail)) == 0))) {
                ASSERT(false);
                return returnCode;
            }
            guid->guid = PD_MSG_FIELD_O(startGuid);
            flags |= GUID_PROP_ISVALID;
    #undef PD_MSG
    #undef PD_TYPE
        }
    }
    // Accomodate the use-case where there's no ptr provided but
    // we still want to allocate the data part, which is always what
    // we want when going through this interface.
    void ** dbPtr = (ptr == NULL) ? NULL : ptr;
    flags |= GUID_PROP_TORECORD;
    // Do a local creation. If we are doing a create on behalf of a remote node, additional
    // book-keeping is required to do a deferred creation when the DB is released.
    return newDataBlockLockableInternal(factory, guid, allocator, allocPD, size, dbPtr, hint, flags, perInstance, /*isEager*/false, /*isClone=*/!isLocal, /*firstCreate=*/true, othLoc);
    //TODO: to simplify we could do the registration here. Since this is an actual local creation stemming from user code
    //it's obvious we need to register the metadata. I think that would allow to eliminate the firstCreate parameter
}

u8 lockableSetHint(ocrDataBlock_t* self, ocrHint_t *hint) {
    ocrDataBlockLockable_t *derived = (ocrDataBlockLockable_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_SET(hint, rHint, OCR_HINT_COUNT_DB_LOCKABLE, ocrHintPropDbLockable, OCR_HINT_DB_PROP_START);
    return 0;
}

u8 lockableGetHint(ocrDataBlock_t* self, ocrHint_t *hint) {
    ocrDataBlockLockable_t *derived = (ocrDataBlockLockable_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_GET(hint, rHint, OCR_HINT_COUNT_DB_LOCKABLE, ocrHintPropDbLockable, OCR_HINT_DB_PROP_START);
    return 0;
}

ocrRuntimeHint_t* getRuntimeHintDbLockable(ocrDataBlock_t* self) {
    ocrDataBlockLockable_t *derived = (ocrDataBlockLockable_t*)self;
    return &(derived->hint);
}

#ifdef ENABLE_RESILIENCY

//TODO-resilience: fold that into the standard API to compute metadata size
u8 getSerializationSizeDataBlockLockable(ocrDataBlock_t* self, u64* outSize) {
    ocrDataBlockLockable_t *dself = (ocrDataBlockLockable_t*)self;

    u8 i;
    u32 localWaiterSize = 0;
    for (i=0; i< DB_MODE_COUNT; i++) {
        if (dself->localWaitQueues[i]) {
            dbWaiter_t * cur;
            u32 counter = 0;
            for (cur = dself->localWaitQueues[i]; cur != NULL; cur = cur->next, counter++);
            dself->waiterQueueCounters[i] = counter;
            localWaiterSize += counter;
        }
    }
    localWaiterSize *= (sizeof(dbWaiter_t));

    u32 remoteWaiterSize = 0;
    for (i=0; i< DB_MODE_COUNT; i++) {
        Queue_t * queue = dself->remoteWaitQueues[i];
        if (queue) {
            u32 j = 0, size = queueGetSize(queue);
            // Account for the data-structure
            remoteWaiterSize += sizeof(Queue_t);
            remoteWaiterSize += (sizeof(void**) * size);
            // Now get the serialized size of entries
            ocrPolicyMsg_t * msg;
            while (j < size) {
                msg = queueGet(queue, j);
                u64 baseSize, marshalledSize;
                ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, MARSHALL_FULL_COPY);
                remoteWaiterSize += (baseSize + marshalledSize);
                j++;
            }
            dself->waiterQueueCounters[i+DB_MODE_COUNT] = size;
        }
    }
    u64 dbSize =    sizeof(ocrDataBlockLockable_t) +
                    (dself->hint.hintVal ? OCR_HINT_COUNT_DB_LOCKABLE*sizeof(u64) : 0) +
                    localWaiterSize + remoteWaiterSize;
    // Account for the datablock payload
    if (dself->backingPtrMsg) { // If backed by a message just serialize that
        ASSERT(self->ptr != NULL);
        u64 baseSize, marshalledSize;
        ocrPolicyMsgGetMsgSize(dself->backingPtrMsg, &baseSize, &marshalledSize, MARSHALL_FULL_COPY);
        dbSize += (baseSize+marshalledSize);
    } else { // else add the payload size
        if (self->ptr != NULL) {
            dbSize += self->size;
        }
    }

    self->base.size = dbSize;
    *outSize = dbSize;
    return 0;
}

//TODO-resilience: fold that into the standard API to serialize
u8 serializeDataBlockLockable(ocrDataBlock_t* self, u8* buffer) {
    ocrDataBlockLockable_t *dself = (ocrDataBlockLockable_t*)self;
    ASSERT(buffer);

    u8* bufferHead = buffer;
    ocrDataBlockLockable_t *dbBuf = (ocrDataBlockLockable_t*)buffer;
    u64 len = sizeof(ocrDataBlockLockable_t);
    hal_memCopy(buffer, self, len, false);
    //TODO-resilience: if lock is held or worker is set then
    //this code is likely to be racy or have an active call stack
    dbBuf->worker = (dself->worker != NULL) ? (ocrWorker_t*)dself->worker->id : (ocrWorker_t*)(-1);
    buffer += len;

    if (dself->hint.hintVal) {
        dbBuf->hint.hintVal = (u64*)buffer;
        len = OCR_HINT_COUNT_DB_LOCKABLE*sizeof(u64);
        hal_memCopy(buffer, dself->hint.hintVal, len, false);
        buffer += len;
    }

    if (dself->backingPtrMsg) {
        u64 baseSize, marshalledSize;
        RESULT_ASSERT(ocrPolicyMsgGetMsgSize(dself->backingPtrMsg, &baseSize, &marshalledSize, MARSHALL_FULL_COPY), ==, 0);
        len = (baseSize+marshalledSize);
        initializePolicyMessage((ocrPolicyMsg_t*) buffer, len);
        RESULT_ASSERT(ocrPolicyMsgMarshallMsg(dself->backingPtrMsg, baseSize, buffer, MARSHALL_FULL_COPY), ==, 0);
        dbBuf->backingPtrMsg = (ocrPolicyMsg_t *) buffer;
        buffer += len;
        ASSERT(dbBuf->base.ptr != NULL);
        dbBuf->base.ptr = (void *) (((u8*)self->ptr) - ((u8*)dself->backingPtrMsg)); // store the offset in msg that contains the data
    } else {
        if (self->ptr) {
            dbBuf->base.ptr = (u64*)buffer;
            len = self->size;
            hal_memCopy(buffer, self->ptr, len, false);
            buffer += len;
        }
        // Note: Serialization seems to be akin to a backup snapshot here since
        // we ignore the content of bkPtr. Probably makes sense for SA though.
    }

    dbWaiter_t *waiter;
    len = sizeof(dbWaiter_t);
    u8 i = 0;
    // Serialize local waiters
    for (i=0; i< DB_MODE_COUNT; i++) {
        if (dself->localWaitQueues[i]) {
            dbBuf->localWaitQueues[i] = (dbWaiter_t*)buffer;
            u32 waiterCount = 0;
            for (waiter = dself->localWaitQueues[i]; waiter != NULL; waiter = waiter->next, buffer += len, waiterCount++) {
                hal_memCopy(buffer, waiter, len, false);
                dbWaiter_t *waiterBuf = (dbWaiter_t*)buffer;
                waiterBuf->next = waiter->next ? (dbWaiter_t*)(buffer + len) : NULL;
            }
            ASSERT(dself->waiterQueueCounters[i] == waiterCount);
        }
    }

    // Serialize remote waiters
    for (i=0; i< DB_MODE_COUNT; i++) {
        Queue_t * queue = dself->remoteWaitQueues[i];
        if (queue) {
            len = sizeof(Queue_t);
            hal_memCopy(buffer, queue, len, false);
            Queue_t * dstQueue = (Queue_t *) buffer;
            buffer += len;
            // Patch up the head to point to buffer
            dstQueue->head = (void **) buffer;
            u32 size = queueGetSize(queue);
            // Reserve space for head to store pointers to entries
            buffer += (sizeof(void *)*size);
            // From there extract and  serialize the message that are stored in the queue
            ocrPolicyMsg_t * msg;
            u32 j = 0;
            while (j < size) {
                msg = queueGet(queue, j);
                u64 baseSize, marshalledSize;
                RESULT_ASSERT(ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, MARSHALL_FULL_COPY), ==, 0);
                len = baseSize + marshalledSize;
                initializePolicyMessage((ocrPolicyMsg_t*) buffer, len);
                RESULT_ASSERT(ocrPolicyMsgMarshallMsg(msg, baseSize, buffer, MARSHALL_FULL_COPY), ==, 0);
                // Update queue's pointer to the serialized entry and advance
                // TODO Don't think I need to keep those updated since unmarshaller will walk the buffer
                dstQueue->head[j] = buffer;
                buffer += len;
                j++;
            }
            ASSERT(dself->waiterQueueCounters[i+DB_MODE_COUNT] == size);
        }
    }
    ASSERT((buffer - bufferHead) == self->base.size);
    return 0;
}

//TODO-resilience: fold that into the standard API to deserialize
u8 deserializeDataBlockLockable(u8* buffer, ocrDataBlock_t** self) {
    //TODO update pd pointer in the queue data structure
    ASSERT(self);
    ASSERT(buffer);
    u8* bufferHead = buffer;
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    //TODO-resilience: I'm assuming that the caller is updating the GP
    ocrDataBlockLockable_t * srcDb = (ocrDataBlockLockable_t*) buffer;
    u64 len = sizeof(ocrDataBlockLockable_t) +
              (srcDb->hint.hintVal ? OCR_HINT_COUNT_DB_LOCKABLE*sizeof(u64) : 0);
    ocrDataBlock_t * dstDbBase = (ocrDataBlock_t*)pd->fcts.pdMalloc(pd, len);
    ocrDataBlockLockable_t * dstDb = (ocrDataBlockLockable_t*)dstDbBase;

    u64 offset = 0;
    len = sizeof(ocrDataBlockLockable_t);
    hal_memCopy(dstDbBase, buffer, len, false);
    //TODO-resilience: see comment in serialize about lock and worker fields
    u64 workerId = (u64)(dstDb->worker);
    dstDb->worker = (workerId == ((u64)-1)) ? NULL : pd->workers[workerId];
    buffer += len;
    offset += len;

    if (dstDb->hint.hintVal != NULL) {
        len = OCR_HINT_COUNT_DB_LOCKABLE*sizeof(u64);
        dstDb->hint.hintVal = (u64*)((u8*)dstDbBase + offset);
        hal_memCopy(dstDb->hint.hintVal, buffer, len, false);
        buffer += len;
        offset += len;
    }

    // Deserialize datablock data payload
    if (dstDb->backingPtrMsg) {
        u64 baseSize, marshalledSize;
        RESULT_ASSERT(ocrPolicyMsgGetMsgSize((ocrPolicyMsg_t*) buffer, &baseSize, &marshalledSize, MARSHALL_FULL_COPY), ==, 0);
        len = baseSize+marshalledSize;
        ocrPolicyMsg_t *msg = (ocrPolicyMsg_t*)pd->fcts.pdMalloc(pd, len);
        RESULT_ASSERT(ocrPolicyMsgUnMarshallMsg(buffer, NULL, msg, MARSHALL_FULL_COPY), ==, 0);
        buffer += len;
        dstDb->backingPtrMsg = msg;
        dstDbBase->ptr = ((u8*)msg)+((u64)dstDbBase->ptr); // serialized 'ptr' is the offset in the msg
    } else {
        if (dstDbBase->ptr != NULL) {
            len = dstDbBase->size;
            dstDbBase->ptr = pd->fcts.pdMalloc(pd, len);
            hal_memCopy(dstDbBase->ptr, buffer, len, false);
            if (dstDbBase->bkPtr != NULL) {
                // Default the backup pointer to the deserialized version
                dstDbBase->bkPtr = pd->fcts.pdMalloc(pd, len);
                hal_memCopy(dstDbBase->bkPtr, buffer, len, false);
            }
            buffer += len;
        }
    }

    // Deserialize local waiters
    len = sizeof(dbWaiter_t);
    u32 i, j;
    dbWaiter_t * waiterPrev;
    for (i=0; i< DB_MODE_COUNT; i++) {
        if (dstDb->localWaitQueues[i]) {
            u32 count = dstDb->waiterQueueCounters[i];
            for (j = 0, waiterPrev = NULL; j < count; j++, buffer += len) {
                dbWaiter_t * waiter = (dbWaiter_t*) pd->fcts.pdMalloc(pd, len);
                hal_memCopy(waiter, buffer, len, false);
                //TODO-resilience: Is it guaranteed anywher that the actual fguid
                //pointer in the dbWaiter_t is valid on deserialize ?
                if (waiterPrev != NULL) { // fix-up previous entry
                    waiterPrev->next = waiter;
                } else { // set head
                    dstDb->localWaitQueues[i] = waiter;
                }
                waiterPrev = waiter;
                // Note the last entry in the list should have its next already set to NULL
            }
        }
    }

    // Deserialize remote waiters queues
    for (i=0; i< DB_MODE_COUNT; i++) {
        Queue_t * queue = dstDb->remoteWaitQueues[i];
        if (queue) {
            //TODO: I don't think I needed to update the head pointers.
            Queue_t * srcQueue = ((Queue_t *) &buffer);
            u32 nbEntries = srcQueue->tail;
            // Compute end of queue data-structure, where first serialized msg is
            len = sizeof(Queue_t) + (queue->tail * sizeof(void*));
            buffer += len;
            Queue_t * dstQueue = newBoundedQueue(pd, srcQueue->size);
            dstDb->remoteWaitQueues[i] = dstQueue;
            // Now deserialize each message
            for (j = 0; j < nbEntries; j++) {
                //TODO there's a bug here where the getMsgSize doesn't work
                u64 baseSize, marshalledSize;
                RESULT_ASSERT(ocrPolicyMsgGetMsgSize((ocrPolicyMsg_t*) buffer, &baseSize, &marshalledSize, MARSHALL_FULL_COPY), ==, 0);
                len = baseSize+marshalledSize;
                ocrPolicyMsg_t *msg = (ocrPolicyMsg_t*)pd->fcts.pdMalloc(pd, len);
                RESULT_ASSERT(ocrPolicyMsgUnMarshallMsg(buffer, NULL, msg, MARSHALL_FULL_COPY), ==, 0);
                buffer += len;
                queueAddLast(dstQueue, msg);
            }
        }
    }

    *self = dstDbBase;
    ASSERT((buffer - bufferHead) == (*self)->base.size);
    return 0;
}

u8 fixupDataBlockLockable(ocrDataBlock_t *self) {
    //Nothing to fixup
    return 0;
}

u8 resetDataBlockLockable(ocrDataBlock_t *self) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    // ASSERT(((ocrDataBlockLockable_t *)self)->backingPtrMsg == NULL);
    // The DB ptr can be backed by a message and in that case, there's
    // code assuming that the DB is in a certain state.
    //TODO-resilience: I don't know the semantic of reset, so I don't
    // know when, if ever, it is safe to nullify these.
    if(((ocrDataBlockLockable_t *)self)->backingPtrMsg) {
        pd->fcts.pdFree(pd, ((ocrDataBlockLockable_t *)self)->backingPtrMsg);
        ((ocrDataBlockLockable_t *)self)->backingPtrMsg = NULL;
        self->ptr = NULL;
    }
    if(self->ptr) {
        pd->fcts.pdFree(pd, self->ptr);
        self->ptr = NULL;
    }
    if(self->bkPtr) {
        pd->fcts.pdFree(pd, self->bkPtr);
        self->bkPtr = NULL;
    }
    pd->fcts.pdFree(pd, self);
    return 0;
}

#endif /* ENABLE_RESILIENCY */

static u8 lockableMdSize(ocrObject_t * dest, mdAction_t mode, u64 * size) {
    *size = 0;
    if (mode & M_CLONE) {
        ocrDataBlockLockable_t * dself = (ocrDataBlockLockable_t *) dest;
        // For hints only account for hints values and the mask(+1)
        u64 hintSize = OCR_RUNTIME_HINT_GET_SIZE(dself->hint.hintMask);
        *size += sizeof(md_push_clone_t) + ((hintSize > 0) ? ((OCR_HINT_COUNT_DB_LOCKABLE+1)*sizeof(u64)) : 0);
    }
    if (mode & M_ACQUIRE) {
        // The char * in md_push_acquire_t is part of the payload
        *size += sizeof(md_push_acquire_t) - sizeof(char*) + ((ocrDataBlock_t *) dest)->size;
    }
    if (mode & M_RELEASE) {
        // The char * in md_push_acquire_t is part of the payload
        *size += sizeof(md_push_release_t) - sizeof(char*) + (((ocrDataBlockLockable_t *) dest)->attributes.writeBack ? ((ocrDataBlock_t *) dest)->size : 0);
    }
    if (mode & M_DATA) {
        // The char * in md_push_acquire_t is part of the payload
        *size += sizeof(md_push_data_t) - sizeof(char*) + ((ocrDataBlock_t *) dest)->size;
    }
    // In current implementation we must have match something
    ASSERT(*size != 0);
    return 0;
}

// In this implementation we choose to do an explicit sendMessage here but we could
// also use the request's response field and let the PD send the response after the
// current processMessage invocation rooted there.
static void sendMdCommResponseAck(ocrPolicyDomain_t *pd, u64 msgId, ocrLocation_t dest) {
    PD_MSG_STACK(msgr);
    getCurrentEnv(NULL, NULL, NULL, &msgr);
#define PD_MSG (&msgr)
#define PD_TYPE PD_MSG_METADATA_COMM
    // Create a response message
    msgr.type = PD_MSG_METADATA_COMM | PD_MSG_RESPONSE;
    msgr.destLocation = dest;
    msgr.msgId = msgId;
    PD_MSG_FIELD_O(returnDetail) = 0;
    pd->fcts.sendMessage(pd, dest, &msgr, NULL, 0);
#undef PD_MSG
#undef PD_TYPE
}

static u8 lockableProcess(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t * mdPtr, ocrPolicyMsg_t * msg) __attribute__((unused));
static u8 lockableProcess(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t * mdPtr, ocrPolicyMsg_t * msg) {
    ASSERT((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_METADATA_COMM);
    u8 retCode = 0;
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
    msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
    u8 direction = PD_MSG_FIELD_I(direction);
    void * payload = &PD_MSG_FIELD_I(payload);
    u64 mdMode = PD_MSG_FIELD_I(mode);
    // Incoming Request to pull MD
    DPRINTF(DBG_LVL_DB_MD, "DB (GUID: "GUIDF") enter process\n", GUIDA(guid));
    if (direction == MD_DIR_PULL) {
#undef PD_MSG
#undef PD_TYPE
        ASSERT(mdPtr != NULL);
        ocrDataBlock_t * self = (ocrDataBlock_t *) mdPtr;
        ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t *) mdPtr;
        // Brokering a pull request: decode what's inside, try to do the operation and if not, resume later
        if (mdMode & M_ACQUIRE) {
            md_pull_acquire_t * mdMsg = (md_pull_acquire_t *) payload;
            // This is assuming that indeed the message is emitted by the PD that wants to acquire
            ocrLocation_t src = msg->srcLocation;
            u8 othMode = mdMsg->dbMode;
            ASSERT(!rself->attributes.hasPeers); // master
            hal_lock(&rself->lock);
            bool grant = remoteAcquire(self, &rself->attributes, othMode);
            DPRINTF(DBG_LVL_DB_MD, "DB (GUID: "GUIDF") process M_ACQUIRE grant=%d othMode=%d state=%d dbMode=%d hasPeers=%d wb=%d, isFetching=%d flags=0x%x numUsers=%d freeReq=%d\n", GUIDA(self->guid), (int) grant,
                othMode,
                (int) rself->attributes.state, (int) rself->attributes.dbMode,
                (int) rself->attributes.hasPeers, (int) rself->attributes.writeBack,
                (int) rself->attributes.isFetching, (int) rself->attributes.flags,
                (int) rself->attributes.numUsers, (int) rself->attributes.freeRequested);
            if (grant) {
                // Ideally this should just return so that the follow-up code in
                // the PD calls serialize. However, there's the issue of the lock we are
                // holding here. The legality seems to be impl dependent. Here the state
                // and mode of the DB can't change and the db ptr is kept around so no
                // harm would be done.
                answerFetchRequest(self, src, othMode);
            } else {
                ocrPolicyDomain_t * pd;
                getCurrentEnv(&pd, NULL, NULL, NULL);
                enqueueRemoteAcquire(pd, msg, &(rself->remoteWaitQueues[othMode]));
                retCode = OCR_EPEND;
            }
            hal_unlock(&rself->lock);
        } else if (mdMode & M_CLONE) {
            hal_lock(&rself->lock);
            // Preparing response
            // No synchronization needed here we're just reading RO data
            // Record the location requesting the PD
            u64 idLoc = (u64) msg->srcLocation;
            setTrackID(rself->mdLocTracker, idLoc);
            answerCloneRequest(self, msg->srcLocation);
            hal_unlock(&rself->lock);
        } else {
        ASSERT(false && "Unsupported datablock mdMode to broker");
        }
    } else {
#ifdef OCR_ASSERT
        u64 checkMdMode = mdMode;
#endif
        // Incoming Request pushing to this MD. Can be the answer to a pull request or an eager push.
        ASSERT(direction == MD_DIR_PUSH);
        if (mdMode & M_CLONE) {
            DPRINTF(DEBUG_LVL_VVERB, "Received M_CLONE for "GUIDF"\n", GUIDA(guid));
            // When we eagerly pushing, the GUID provider may or may not know the GUID
            // When doing the regular push/pull, the proxy is known and getVal returns OCR_EPEND
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
            // It is the responsibility of the Guid-Provider to make sure this code is thread-safe.
            // Typically, concurrency is addressed at the GP level and a single CLONE operation is
            // allowed there, others being enqueued on the callback of this call.
            ocrObject_t * dest = NULL;
            // We have nothing to deserialize too so the deserialize code has to allocate memory to deserialize the payload to.
            retCode = factory->deserialize(factory, guid, &dest, PD_MSG_FIELD_I(mode), (void *) &PD_MSG_FIELD_I(payload), (u64) PD_MSG_FIELD_I(sizePayload));
            // NOTE: Implementation ensures there's a single message generated for the initial clone
            // so that this registration is not concurrent with others for the same GUID
#undef PD_MSG
#undef PD_TYPE
            // In eager those would be set but we actually process them as part of deserialize
            if ((mdMode & M_DATA) || (mdMode & M_SATISFY)) {
                mdMode &= ~M_DATA;
                mdMode &= ~M_SATISFY;
#ifdef OCR_ASSERT
                checkMdMode = mdMode;
#endif
            }
#ifdef OCR_ASSERT
            checkMdMode &= ~M_CLONE;
#endif
        }
        if ((mdMode & M_DATA) || (mdMode & M_SATISFY)) {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
            ASSERT(mdPtr != NULL);
            retCode = factory->deserialize(factory, guid, &mdPtr, mdMode, (void *) &PD_MSG_FIELD_I(payload), (u64) PD_MSG_FIELD_I(sizePayload));
#undef PD_MSG
#undef PD_TYPE
#ifdef OCR_ASSERT
            checkMdMode &= ~M_SATISFY;
            checkMdMode &= ~M_DATA;
#endif
        }
        if (mdMode & M_ACQUIRE) {
            ASSERT(mdPtr != NULL);
            ocrDataBlock_t * self = (ocrDataBlock_t *) mdPtr;
            ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t *) mdPtr;
            // Extract payload arg into md_push_acquire_t
            md_push_acquire_t * mdMsg = (md_push_acquire_t *) payload;
            u8 othMode = mdMsg->dbMode;
            // In this implementation a push should always be awaited for and successful
            hal_lock(&rself->lock);
            ocrDataBlockLockableAttr_t attr = rself->attributes;
            ASSERT(attr.isFetching);
            ASSERT(attr.dbMode == DB_RO); // most liberal
            ASSERT(attr.state == STATE_IDLE);
            ASSERT(attr.numUsers == 0);
            ASSERT(attr.hasPeers);
            ASSERT(rself->mdPeers == msg->srcLocation); // single master DB as peer for now
            if (rself->backingPtrMsg) {
                ocrPolicyDomain_t * pd;
                getCurrentEnv(&pd, NULL, NULL, NULL);
                pd->fcts.pdFree(pd, rself->backingPtrMsg);
            }
            self->ptr = (void *) &(mdMsg->dbPtr);

            rself->attributes.writeBack = mdMsg->writeBack;
            ASSERT(((othMode & WR_MASK) & !(rself->attributes.flags & DB_PROP_SINGLE_ASSIGNMENT)) ? mdMsg->writeBack : !mdMsg->writeBack);
            rself->attributes.isFetching = false;
            rself->backingPtrMsg = msg;

            // Resume locally blocked acquire before releasing the lock.
            // This is just to make sure we give a chance to the blocked acquire
            // to resume. Else we could have one go, acquire/release and relinquish
            // the DB for the PD again.

            // Transition the MD state and mode
            rself->attributes.state = (othMode & WR_MASK) ? STATE_PRIME : STATE_SHARED;
            rself->attributes.dbMode = othMode;

            DPRINTF(DBG_LVL_DB_MD, "M_ACQUIRE,PUSH PROCESS: "GUIDF" wb=%d dbMode=%d msg_usefulSize=%"PRId64" msg_bufferSize=%"PRId64" dbSize=%"PRId64"\n",
                    GUIDA(self->guid), (int) mdMsg->writeBack, (int) mdMsg->dbMode, msg->usefulSize, msg->bufferSize, self->size);
            schedulePendingAcquire(self, &(rself->attributes));
            hal_unlock(&rself->lock);
            //TODO-MD-MSGBACK: return OCR_EPEND so that caller doesn't deallocate the message being processed
            retCode = OCR_EPEND;
#ifdef OCR_ASSERT
            checkMdMode &= ~M_ACQUIRE;
#endif
        }
        if (mdMode & M_RELEASE) {
            //Receiving a M_RELEASE push message. May or may not include data write back.
            ASSERT(mdPtr != NULL);
            ocrDataBlock_t * self = (ocrDataBlock_t *) mdPtr;
            ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t *) mdPtr;
            hal_lock(&rself->lock);
            md_push_release_t * mdMsg = (md_push_release_t *) payload;
            ASSERT(!rself->attributes.hasPeers); // Only master recv push release
            // If WB flag we need to deserialize
            DPRINTF(DBG_LVL_DB_MD, "M_RELEASE: "GUIDF" wb=%d dbMode=%d msg_usefulSize=%"PRId64" msg_bufferSize=%"PRId64"\n", GUIDA(self->guid), (int) mdMsg->writeBack, (int) mdMsg->dbMode, msg->usefulSize, msg->bufferSize);
            if (mdMsg->writeBack) {
                ocrPolicyDomain_t * pd;
                getCurrentEnv(&pd, NULL, NULL, NULL);
                if (rself->backingPtrMsg) {
                    pd->fcts.pdFree(pd, rself->backingPtrMsg);
                } else {
                    ASSERT(self->ptr != NULL);
                    PD_MSG_STACK(msg);
                    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_UNALLOC
                    msg.type = PD_MSG_MEM_UNALLOC | PD_MSG_REQUEST;
                    PD_MSG_FIELD_I(allocatingPD.guid) = self->allocatingPD;
                    PD_MSG_FIELD_I(allocatingPD.metaDataPtr) = NULL;
                    PD_MSG_FIELD_I(allocator.guid) = self->allocator;
                    PD_MSG_FIELD_I(allocator.metaDataPtr) = NULL;
                    PD_MSG_FIELD_I(ptr) = self->ptr;
                    PD_MSG_FIELD_I(type) = DB_MEMTYPE;
                    PD_MSG_FIELD_I(properties) = 0;
                    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
                }
                rself->backingPtrMsg = msg;
                self->ptr = (void *) &(mdMsg->dbPtr);
                //TODO-MD-MSGBACK: interesting example here.
                // - If I want to piggy back on the message to avoid a copy
                //   I can just set the pointer here and return OCR_EPEND (otherwise comp-worker destroys it)
                // - Else, I go through deserialize but I'd need to tweak the mode so that it knows
                //   I don't want to make a copy and just use the pointer. Additionally, the callsite (here)
                //   still need to do some bookeeping OCR_EPEND + save msg pointer.
                // ocrObject_t * dest = (ocrObject_t *) self;
                // factory->deserialize(factory, self->guid, &dest, M_RELEASE, (void *) mdMsg->dbPtr, (u64) PD_MSG_FIELD_I(sizePayload));
                retCode = OCR_EPEND;
            }
            // then try and transition
            remoteRelease(self, &((ocrDataBlockLockable_t *)self)->attributes);
#ifdef DB_STATS_LOCKABLE
            ((ocrDataBlockLockable_t *)self)->stats.counters[CNT_REMOTE_RELEASE]++;
#endif
            // Check if we need to perform a delayed
            // destruction that was gated by this release
            if(rself->attributes.numUsers == 0 &&
                rself->attributes.freeRequested == 1) {
                // Master shouldn't be doing any remote release.
                ASSERT(!rself->attributes.isReleasing);
                rself->worker = NULL;
                // Tricky case: we release and store the msg ptr however it turns out we're deallocating the DB
                // so we want the return code to be pending to avoid the caller doing the free on the message.
                if (msg == rself->backingPtrMsg) {
                    retCode = OCR_EPEND;
                }
                hal_unlock(&(rself->lock));
#ifndef LOCKABLE_RELEASE_ASYNC
                //TODO there might be a race here because we're trying to reply to a release
                //when destroying the event. Either the sender:
                // - doesn't know the db is being destroyed and is waiting for a response
                //   happens when the destroy originated from another PD
                // - knows it is being destroyed because it made the call and this is an auto-release
                ocrPolicyDomain_t * pd;
                getCurrentEnv(&pd, NULL, NULL, NULL);
                sendMdCommResponseAck(pd, msg->msgId, msg->srcLocation);
#endif
                DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] destruct from M_RELEASE\n", GUIDA(self->guid));
                lockableDestruct(self);
                return retCode;
            }
            DPRINTF(DBG_LVL_DB_MD, "M_RELEASE: "GUIDF") done state=%d dbMode=%d hasPeers=%d wb=%d, isFetching=%d flags=0x%x numUsers=%d freeReq=%d\n", GUIDA(self->guid),
                (int) rself->attributes.state, (int) rself->attributes.dbMode,
                (int) rself->attributes.hasPeers, (int) rself->attributes.writeBack,
                (int) rself->attributes.isFetching, (int) rself->attributes.flags,
                (int) rself->attributes.numUsers, rself->attributes.freeRequested);
            hal_unlock(&rself->lock);
#ifndef LOCKABLE_RELEASE_ASYNC
            ocrPolicyDomain_t * pd;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            sendMdCommResponseAck(pd, msg->msgId, msg->srcLocation);
#endif
#ifdef OCR_ASSERT
            checkMdMode &= ~M_RELEASE;
#endif
        }
        if (mdMode & M_DEL) {
            ocrDataBlock_t * self = (ocrDataBlock_t *) mdPtr;
            ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t *) mdPtr;
            hal_lock(&rself->lock);
            DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] setting freeRequest from M_DEL\n", GUIDA(self->guid));
            rself->attributes.freeRequested = 1;
            if (rself->attributes.hasPeers) { // Slave
                // Notification from the master the current MD is to be deleted
            } else { // Master
                // Notification from a slave it has executed ocrDbDestroy.
                // Remove slave from known peers
                issueDelMessage(rself, msg->srcLocation);
            }
            // Competing with pending release
            if ((rself->attributes.numUsers == 0) && (!rself->attributes.isReleasing)) {
                hal_unlock(&(rself->lock));
                DPRINTF(DEBUG_LVL_BUG, "DB["GUIDF"] destruct from M_DEL\n", GUIDA(self->guid));
                return lockableDestruct(self);
            } // else destruction will happen on the last release
            hal_unlock(&(rself->lock));
#ifdef OCR_ASSERT
            checkMdMode &= ~M_DEL;
#endif
        }
#ifdef OCR_ASSERT
        if (checkMdMode != 0) {
            DPRINTF(DEBUG_LVL_WARN, "0x%"PRIx64"\n", checkMdMode);
            ASSERT(false && "Unsupported datablock mode to broker");
        }
#endif
    }
    DPRINTF(DBG_LVL_DB_MD, "DB (GUID: "GUIDF") exit process\n", GUIDA(guid));
    return retCode;
}

//If yes, then we just want to send the M_DATA part
static bool isKnownNonCoherentLocation(ocrDataBlockLockable_t * dself, ocrLocation_t destLocation) {
    //TODO-MD-EAGER: Limitation: don't support multiple locations so far
    return (dself->nonCoherentLoc != INVALID_LOCATION);
}

//Record the location is non-coherent
static void registerKnownNonCoherentLocation(ocrDataBlockLockable_t * dself, ocrLocation_t destLocation) {
    //TODO-MD-EAGER: Limitation: don't support multiple locations so far
    ASSERT(dself->nonCoherentLoc == INVALID_LOCATION);
    dself->nonCoherentLoc = destLocation;
}

extern void mdLocalDeguidify(ocrPolicyDomain_t *self, ocrFatGuid_t *guid);

static u8 lockableCloneInternal(ocrObjectFactory_t * pfactory, ocrGuid_t guid, ocrObject_t ** mdPtr, ocrLocation_t destLocation, u32 type, u32 waitersCount, void * waitersPtr) {
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
    if (destLocation != pd->myLocation) {
        // This is invoked when an EAGER DB needs to be pushed to another PD
        ASSERT(HAS_MD_NON_COHERENT(type));
        // Retrieve the metadata pointer that must be present locally
        ocrFatGuid_t fatGuid;
        fatGuid.guid = guid;
        fatGuid.metaDataPtr = NULL;
        mdLocalDeguidify(pd, &fatGuid);
        ocrDataBlock_t * self = (ocrDataBlock_t *) fatGuid.metaDataPtr;
        ocrDataBlockLockable_t * dself = (ocrDataBlockLockable_t *) self;
        ASSERT(self != NULL);
        // Going to push data and it requires satifying an event at destination
        u64 mdMode = M_DATA | M_SATISFY;
        ASSERT(waitersCount != 0); // Probably too tight of a restriction. Leave it for debugging purpose.
        if (!isKnownNonCoherentLocation(dself, destLocation)) {
            registerKnownNonCoherentLocation(dself, destLocation);
            DPRINTF(DEBUG_LVL_VVERB, "db-md: registerKnownNonCoherentLocation M_CLONE "GUIDF"\n", GUIDA(guid));
            // First time an EAGER DB is pushed, set the clone flag to install it at destinatiokn
            mdMode |= M_CLONE;
#ifdef DB_STATS_LOCKABLE
            ((ocrDataBlockLockable_t *)self)->stats.counters[CNT_EAGER_CLONE]++;
#endif
        } else {
#ifdef DB_STATS_LOCKABLE
            ((ocrDataBlockLockable_t *)self)->stats.counters[CNT_EAGER_PULL]++;
#endif
        }
        // and only support this use case so far:
        ASSERT(HAS_MD_CLONE(type) && HAS_MD_NON_COHERENT(type));
        ocrPolicyMsg_t * msg;
        PD_MSG_STACK(msgStack);
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        // Query the serialized size for the move operation
        u64 baseMdSize = 0;
        lockableMdSize((ocrObject_t *) self, mdMode, &baseMdSize);
        // Add satisfy message size
        u64 waitersSz = (((u64)&(((md_push_satisfy_t*)0)->regNodesPtr)) + (sizeof(regNode_t) * waitersCount));
        u64 mdSize = baseMdSize + waitersSz;
        u64 msgSize = MSG_MDCOMM_SZ + mdSize;
        u32 msgProp = 0;
        if (msgSize > sizeof(ocrPolicyMsg_t)) {
            //TODO-MD-SLAB
            msg = allocPolicyMsg(pd, &msgSize);
            initializePolicyMessage(msg, msgSize);
            getCurrentEnv(NULL, NULL, NULL, msg);
            msgProp = PERSIST_MSG_PROP;
        } else {
            msg = &msgStack;
            getCurrentEnv(NULL, NULL, NULL, &msgStack);
        }
        // Fill in this call specific arguments
        msg->destLocation = destLocation;
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
        msg->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(guid) = self->guid;
        PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
        PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/ //TODO-MD-OP not clearly defined yet
        PD_MSG_FIELD_I(mode) = mdMode;
        PD_MSG_FIELD_I(factoryId) = self->fctId;
        PD_MSG_FIELD_I(response) = NULL;
        PD_MSG_FIELD_I(mdPtr) = NULL;
        DPRINTF(DEBUG_LVL_VVERB, "db-md: push "GUIDF" in mode=%"PRIx64" isEager(%"PRIu64")\n", GUIDA(guid), mdMode, (mdMode & M_CLONE));
        PD_MSG_FIELD_I(sizePayload) = mdSize;
        char * ptr = &(PD_MSG_FIELD_I(payload));
        lockableSerialize(pfactory, guid, (ocrObject_t *) self, &mdMode, destLocation, (void **) &ptr, &mdSize);
        if (mdMode & M_CLONE) {
            //TODO-MD-EAGER: see comment in serialize
            ((md_push_clone_t *)ptr)->isEager = true;
        }
        ptr += baseMdSize;
        md_push_satisfy_t * satPtr = (md_push_satisfy_t *) ptr;
        ASSERT(waitersCount == 1);
        satPtr->waitersCount = waitersCount;
        // serializing the data into the destBuffer
        hal_memCopy(&(satPtr->regNodesPtr), waitersPtr, (sizeof(regNode_t) * waitersCount), false);
#undef PD_MSG
#undef PD_TYPE
        pd->fcts.sendMessage(pd, destLocation, msg, NULL, msgProp);
    } else {
        // This implementation only pulls in clone mode
        ASSERT(HAS_MD_CLONE(type));
        // Since we just pull to clone, the destination of this
        // message is the location that owns the GUID.
        ocrLocation_t ownerLocation;
        RESULT_ASSERT(pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], guid, &ownerLocation), ==, 0);
        msg.destLocation = ownerLocation;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_METADATA_COMM
        msg.type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(guid) = guid;
        PD_MSG_FIELD_I(direction) = MD_DIR_PULL;
        PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
        PD_MSG_FIELD_I(mode) = M_CLONE;
        PD_MSG_FIELD_I(factoryId) = ((ocrDataBlockFactory_t *)pfactory)->factoryId;
        PD_MSG_FIELD_I(response) = NULL;
        PD_MSG_FIELD_I(mdPtr) = NULL;
        DPRINTF (DBG_LVL_DB_MD, "db-md: pull "GUIDF" in mode=M_CLONE\n", GUIDA(guid));
        PD_MSG_FIELD_I(sizePayload) = 0;
        // Don't add any specific payload since the src is encoded in the message
        pd->fcts.processMessage(pd, &msg, true);
#undef PD_MSG
#undef PD_TYPE
    }
    if (NULL != mdPtr) {
        *mdPtr = NULL;
    }
    return OCR_EPEND; // This is a remote operation that's pending
}

#define SER_WRITE(dest, src, sz) {hal_memCopy(dest, src, sz, false); char * _tmp__ = ((char *) dest) + (sz); dest=_tmp__;}

// Pre-declare with unused attribute otherwise TG assumes it is not used although the name is referenced when setting up function pointers
static u8 lockableCloneSatisfy(ocrObjectFactory_t * pfactory, ocrGuid_t guid, ocrObject_t ** mdPtr, ocrLocation_t destLocation, u32 type, u32 waitersCount, void * waitersPtr) __attribute__((unused));
static u8 lockableCloneSatisfy(ocrObjectFactory_t * pfactory, ocrGuid_t guid, ocrObject_t ** mdPtr, ocrLocation_t destLocation, u32 type, u32 waitersCount, void * waitersPtr) {
    return lockableCloneInternal(pfactory, guid, mdPtr, destLocation, type, waitersCount, waitersPtr);
}

// Pre-declare with unused attribute otherwise TG assumes it is not used although the name is referenced when setting up function pointers
static u8 lockableClone(ocrObjectFactory_t * pfactory, ocrGuid_t guid, ocrObject_t ** mdPtr, ocrLocation_t destLocation, u32 type) __attribute__((unused));
static u8 lockableClone(ocrObjectFactory_t * pfactory, ocrGuid_t guid, ocrObject_t ** mdPtr, ocrLocation_t destLocation, u32 type) {
    return lockableCloneInternal(pfactory, guid, mdPtr, destLocation, type, 0, NULL);
}

static u8 lockableSerialize(ocrObjectFactory_t * factory, ocrGuid_t guid,
                     ocrObject_t * src, u64 * mode, ocrLocation_t destLocation,
                     void ** destBuffer, u64 * destSize) {
    //TODO this should become a loop over modes based on bit fiddling
    // => Well actually it's sucky because we'd have to do the bits in order anyway
    ASSERT((destBuffer != NULL) && (*destBuffer != NULL));
    char * writePtr = *destBuffer;
    ocrDataBlock_t * self = (ocrDataBlock_t *) src;
    ocrDataBlockLockable_t * dself = (ocrDataBlockLockable_t *) self;
#ifdef OCR_ASSERT
    u64 checkMode = *mode;
#endif
    if (*mode & M_CLONE) {
        ASSERT(destBuffer != NULL);
        ocrPolicyDomain_t * pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        md_push_clone_t * mdBuffer = (md_push_clone_t *) writePtr;
        mdBuffer->srcLocation = pd->myLocation;
        mdBuffer->size = self->size;
        mdBuffer->flags = dself->attributes.flags;
        //TODO-MD-EAGER: The issue here is that we want to create a message that contains both state
        //and context-dependent information. Hence, serialize can only put default values while the
        //calling context patches it up.
        mdBuffer->isEager = false;
        writePtr = writePtr + sizeof(md_push_clone_t);
        u64 hintSize = OCR_RUNTIME_HINT_GET_SIZE(dself->hint.hintMask);
        if (hintSize > 0) {
            mdBuffer->storage.directory = STORAGE_ID_HINT; // TODO-STORAGE-API
            mdBuffer->storage.offset = 0;
            u64 * storagePtr = (u64 *) GET_STORAGE_PTR((&mdBuffer->storage), HINT);
            ASSERT(storagePtr != NULL);
            storagePtr[0] = dself->hint.hintMask;
            writePtr += (sizeof(u64));
            // DB_PROP_NO_HINT is not retained in the datablock flags so I'm not sure what to do here
            // ASSERT(!hasProperty(self->flags, DB_PROP_NO_HINT));
            u32 hintc = OCR_HINT_COUNT_DB_LOCKABLE;
            SER_WRITE(writePtr, dself->hint.hintVal, sizeof(u64)*hintc);
        } else {
            mdBuffer->storage.directory = 0;
            mdBuffer->storage.offset = 0;
        }
#ifdef OCR_ASSERT
        checkMode &= ~M_CLONE;
#endif
    }
    if (*mode & M_DATA) {
        ASSERT(destBuffer != NULL);
        md_push_data_t * mdBuffer = (md_push_data_t *) writePtr;
        ASSERT(mdBuffer != NULL);
        // serializing the data into the destBuffer
        SER_WRITE(writePtr, self->ptr, self->size);
#ifdef OCR_ASSERT
        checkMode &= ~M_DATA;
#endif
    }
    if (*mode & M_SATISFY) {
#ifdef OCR_ASSERT
        checkMode &= ~M_SATISFY;
#endif
    }
    //TODO-MD-EAGER: Need to revisit acquire as a compound operation with | M_DATA
    if (*mode & M_ACQUIRE) {
        ASSERT(destBuffer != NULL);
        md_push_acquire_t * mdBuffer = (md_push_acquire_t *) writePtr;
        ASSERT(mdBuffer != NULL);
        // serializing the data into the destBuffer, read of size 1
        hal_memCopy(&(mdBuffer->dbPtr), self->ptr, self->size, false);
        writePtr = writePtr+(sizeof(md_push_acquire_t)+self->size-1);
#ifdef OCR_ASSERT
        checkMode &= ~M_ACQUIRE;
#endif
    }
#ifdef OCR_ASSERT
    if (checkMode) {
        ASSERT(false && "Unhandled serialization mode");
    }
#endif
    return 0;
}

// Pre-declare with unused attribute otherwise TG assumes it is not used although the name is referenced when setting up function pointers
static u8 lockableDeserialize(ocrObjectFactory_t * pfactory, ocrGuid_t dbGuid, ocrObject_t ** dest, u64 mode, void * srcBuffer, u64 srcSize) __attribute__((unused));
static u8 lockableDeserialize(ocrObjectFactory_t * pfactory, ocrGuid_t dbGuid, ocrObject_t ** dest, u64 mode, void * srcBuffer, u64 srcSize) {
    char * curPtr = srcBuffer;
    bool isCloneRelease = false;
    bool isEager = false;
    u8 retCode = 0;
    if (mode & M_CLONE) {
        md_push_clone_t * mdMsg = srcBuffer;
        //TODO-MD-CLONE&ACQ the metadata_comm message we had sent, should contain the acquire mode
        //so that when the clone succeed, we actually have both the MD and got access granted
        ocrPolicyDomain_t * pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        ocrLocation_t dbLoc;
        pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], dbGuid, &dbLoc);
        ocrLocation_t curLoc = pd->myLocation;
        ocrDataBlockFactory_t * factory = (ocrDataBlockFactory_t *) pfactory;
        ocrFatGuid_t fguid;
        fguid.guid = dbGuid;
        // If the current location is not the one encoded in the DB, then we are getting a clone
        // of the original MD else it is a lazy creation on release. i.e. We created the DB on a
        // remote node and on release, we write it back and create it.
        bool isClone = (dbLoc != curLoc);
        ocrLocation_t srcLoc = mdMsg->srcLocation;
        ocrFatGuid_t allocator = pd->allocators[0]->fguid;
        ocrFatGuid_t allocPD = pd->fguid;
        // Detect lazy writeback
        isCloneRelease = (dbLoc == curLoc) && (mode & M_DATA);
        // Detect eager push
        isEager = mdMsg->isEager;
        DPRINTF(DEBUG_LVL_VVERB, "db-md: Deserialize M_CLONE "GUIDF" isEager=%d\n", GUIDA(dbGuid), isEager);
        // Can't have both mode true at the same time
        ASSERT(!(isEager && isCloneRelease));
        ASSERT(isEager ? isClone : true);
        if (isCloneRelease) {
            // This is a clone-release, meaning the deserialization incurs
            // both a MD creation and a data writeback. Happens when a DB
            // is created on PD A but hint is set to PD B.
            // Note: the GUID record is delayed down in M_DATA
            srcLoc = INVALID_LOCATION;
            isClone = false;
            retCode = OCR_EPEND;
            // Postpone GUID record until after data is copied in M_DATA
        } else {
            mdMsg->flags |= GUID_PROP_TORECORD;
        }
        bool hasHint = HAS_STORED((&mdMsg->storage), HINT);
        ocrHint_t dbHint;
        ocrHintInit(&dbHint, OCR_HINT_DB_T);
        ocrRuntimeHint_t rtHint;
        u64 hintSize = 0;
        if (hasHint) {
            u64 * ptr = (u64 *) GET_STORAGE_PTR((&mdMsg->storage), HINT);
            ASSERT(ptr != NULL);
            u64 hintMask = ptr[0];
            // hintSize = OCR_RUNTIME_HINT_GET_SIZE(hintMask);
            hintSize = OCR_HINT_COUNT_DB_LOCKABLE;
            // My hints here are runtime hints that are serialized as hintMask val0 val1 etc...
            rtHint.hintMask = hintMask;
            rtHint.hintVal  = &ptr[1];
            OCR_RUNTIME_HINT_GET(&dbHint, &rtHint, OCR_HINT_COUNT_DB_LOCKABLE, ocrHintPropDbLockable, OCR_HINT_DB_PROP_START);
        }

        // In this context we know this is a DB being pushed to the current PD
        // 1) Current PD requested a clone
        // 2) Receiving a DB instance that belong to current PD but has been created by another
        mdMsg->flags |= (GUID_PROP_ISVALID);
        DPRINTF(DBG_LVL_DB_MD, "newDataBlockLockableInternal on "GUIDF" msg=%p\n", GUIDA(dbGuid), mdMsg);
        RESULT_ASSERT(newDataBlockLockableInternal(factory, &fguid, allocator, allocPD, mdMsg->size, /*ptr=*/NULL,
                                                   ((hasHint) ? &dbHint : NULL_HINT), mdMsg->flags,
                                                   NULL, isEager, /*isClone=*/isClone, false, srcLoc), ==, 0);
        ASSERT(fguid.metaDataPtr != NULL);
        *dest = fguid.metaDataPtr;
        curPtr = (curPtr + (sizeof(md_push_clone_t) + (hasHint ? sizeof(u64)*(hintSize+1) : 0)));
    }

    if (mode & M_DATA) {
        // So here we've registered the GUID however we haven't set the pointer yet which is kind of sucky
        ocrDataBlock_t * self = (ocrDataBlock_t *) *dest;
        ocrDataBlockLockable_t * rself = (ocrDataBlockLockable_t *) self;
        bool isEager = rself->attributes.isEager;
        if (isCloneRelease || isEager) {
            DPRINTF(DEBUG_LVL_VVERB, "db-md: Deserialize M_DATA "GUIDF" isEager=%d\n", GUIDA(self->guid), isEager);
            ASSERT(*dest != NULL);
            // For the write back part
            ASSERT(isCloneRelease != isEager);
            ASSERT(isCloneRelease ? (self->ptr == NULL) : isEager);
            ASSERT(isCloneRelease ? (rself->backingPtrMsg == NULL) : isEager);
            md_push_data_t * mdRel = (md_push_data_t *) curPtr;
            ocrPolicyMsg_t * msg = NULL;
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_METADATA_COMM
            u64 spread = ((u64) &PD_MSG_FIELD_I(payload));
            msg = (ocrPolicyMsg_t *) (((char *) srcBuffer) - spread);
#undef PD_MSG
#undef PD_TYPE
            if (isEager) {
                // If properly ordered we should not have any users
                //TODO: Would want to have some safety mecanism in here
                ASSERT_BLOCK_BEGIN((rself->attributes.numUsers == 0))
                DPRINTF(DEBUG_LVL_WARN, "error: eager DB "GUIDF" is being overwritten by incoming new version\n", GUIDA(self->guid));
                ASSERT_BLOCK_END
                // Discard the previous copy if any
                if (rself->backingPtrMsg != NULL) {
                    ocrPolicyDomain_t * pd;
                    getCurrentEnv(&pd, NULL, NULL, NULL);
                    pd->fcts.pdFree(pd, rself->backingPtrMsg);
                }
                rself->backingPtrMsg = msg;
            } else {
                // Record the source that initiated the lazy release
                // since it has a copy of the metadata too.
                setTrackID(rself->mdLocTracker, msg->srcLocation);
                rself->backingPtrMsg = msg;
            }
            self->ptr = &(mdRel->dbPtr);
            if (isCloneRelease) {
                ocrPolicyDomain_t * pd;
                getCurrentEnv(&pd, NULL, NULL, NULL);
                // Registers the MD only now. Addresses the race where the DB would
                // have been registered in M_CLONE but its data ptr is not yet set.
                RESULT_ASSERT(pd->guidProviders[0]->fcts.registerGuid(pd->guidProviders[0], self->guid, (u64) self), ==, 0);
#ifndef LOCKABLE_RELEASE_ASYNC
                sendMdCommResponseAck(pd, msg->msgId, msg->srcLocation);
#endif
            }
            retCode = OCR_EPEND;
            curPtr = (curPtr + self->size);
        } else {
            ASSERT(false && "M_DATA only used for eager and cloneRelease");
        }
    }

    if (mode & M_SATISFY) {
        ocrPolicyDomain_t * pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        md_push_satisfy_t * pPtr = (md_push_satisfy_t *)  curPtr;
        u32 waitersCount = pPtr->waitersCount;
        curPtr = (char*) &(pPtr->regNodesPtr);
        regNode_t * waiters = (regNode_t *) curPtr;
        u32 i = 0;
        PD_MSG_STACK(msg);
        DPRINTF(DEBUG_LVL_VERB, "Processing M_SATISFY waitersCount=%"PRIu32"\n", waitersCount);
        ASSERT(waitersCount == 1);
        while (i < waitersCount) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
            // DPRINTF(DEBUG_LVL_WARN, "Satisfy eager push dependence on GUID="GUIDF" on slot %"PRIu32"\n", GUIDA(waiters[i].guid), waiters[i].slot);
            // ASSERT(waiters[i].slot == 0);
            getCurrentEnv(NULL, NULL, NULL, &msg);
            msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
            // Need to refill because out may overwrite some of the in fields
            PD_MSG_FIELD_I(satisfierGuid.guid) = NULL_GUID; //TODO didn't keep that around, not sure if that's an issue
            // Passing NULL since base may become invalid
            PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(guid.guid) = waiters[i].guid;
            PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(payload.guid) = dbGuid;
            PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID; //TODO didn't keep that around, not sure if that's an issue
            PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL; //TODO didn't keep that around, not sure if that's an issue
            PD_MSG_FIELD_I(slot) = waiters[i].slot;
#ifdef REG_ASYNC_SGL
            PD_MSG_FIELD_I(mode) = waiters[i].mode;
#endif
            PD_MSG_FIELD_I(properties) = 0;
            RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, false));
#undef PD_MSG
#undef PD_TYPE
            i++;
        }
    }

    if (mode & M_RELEASE) {
        ASSERT(false && "M_RELEASE should be handled in process");
    }

    return retCode;
}


/******************************************************/
/* OCR DATABLOCK LOCKABLE FACTORY                      */
/******************************************************/

void destructLockableFactory(ocrObjectFactory_t *factory) {
    runtimeChunkFree((u64)((ocrDataBlockFactory_t*)factory)->hintPropMap, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrDataBlockFactory_t *newDataBlockFactoryLockable(ocrParamList_t *perType, u32 factoryId) {
    ocrObjectFactory_t * bbase = (ocrObjectFactory_t *)
                                  runtimeChunkAlloc(sizeof(ocrDataBlockFactoryLockable_t), PERSISTENT_CHUNK);
    // Initialize the base's base
    bbase->fcts.processEvent = NULL;
    bbase->clone = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**, ocrLocation_t, u32), lockableClone);
    bbase->mdSize = FUNC_ADDR(u8 (*)(ocrObject_t * dest, u64, u64*), lockableMdSize);
    bbase->process = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t*, /*MD-COMM*/ocrPolicyMsg_t * msg), lockableProcess);
    bbase->serialize = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t*, u64*, ocrLocation_t, void**, u64*), lockableSerialize);
    bbase->deserialize = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**, u64, void*, u64), lockableDeserialize);
    ocrDataBlockFactory_t *base = (ocrDataBlockFactory_t*) bbase;
    base->instantiate = FUNC_ADDR(u8 (*)
                                  (ocrDataBlockFactory_t*, ocrFatGuid_t*, ocrFatGuid_t, ocrFatGuid_t,
                                   u64, void**, ocrHint_t*, u32, ocrParamList_t*), newDataBlockLockable);
    // Instance functions
    bbase->destruct = FUNC_ADDR(void (*)(ocrObjectFactory_t*), destructLockableFactory);
    base->fcts.cloneAndSatisfy = FUNC_ADDR(u8 (*)(ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t**, ocrLocation_t, u32, u32, void *), lockableCloneSatisfy);
    base->fcts.destruct = FUNC_ADDR(u8 (*)(ocrDataBlock_t*), lockableDestruct);
    base->fcts.acquire = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, void**, ocrFatGuid_t, ocrLocation_t, u32, ocrDbAccessMode_t, bool, u32), lockableAcquire);
    base->fcts.release = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t, ocrLocation_t, bool), lockableRelease);
    base->fcts.free = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t, ocrLocation_t, u32), lockableFree);
    base->fcts.registerWaiter = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t,
                                                 u32, bool), lockableRegisterWaiter);
    base->fcts.unregisterWaiter = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t,
                                                   u32, bool), lockableUnregisterWaiter);
    base->fcts.setHint = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrHint_t*), lockableSetHint);
    base->fcts.getHint = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrHint_t*), lockableGetHint);
    base->fcts.getRuntimeHint = FUNC_ADDR(ocrRuntimeHint_t* (*)(ocrDataBlock_t*), getRuntimeHintDbLockable);
#ifdef ENABLE_RESILIENCY
    base->fcts.getSerializationSize = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, u64*), getSerializationSizeDataBlockLockable);
    base->fcts.serialize = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, u8*), serializeDataBlockLockable);
    base->fcts.deserialize = FUNC_ADDR(u8 (*)(u8*, ocrDataBlock_t**), deserializeDataBlockLockable);
    base->fcts.fixup = FUNC_ADDR(u8 (*)(ocrDataBlock_t*), fixupDataBlockLockable);
    base->fcts.reset = FUNC_ADDR(u8 (*)(ocrDataBlock_t*), resetDataBlockLockable);
#endif
    base->factoryId = factoryId;
    //Setup hint framework
    base->hintPropMap = (u64*)runtimeChunkAlloc(sizeof(u64)*(OCR_HINT_DB_PROP_END - OCR_HINT_DB_PROP_START - 1), PERSISTENT_CHUNK);
    OCR_HINT_SETUP(base->hintPropMap, ocrHintPropDbLockable, OCR_HINT_COUNT_DB_LOCKABLE, OCR_HINT_DB_PROP_START, OCR_HINT_DB_PROP_END);

    return base;
}
#endif /* ENABLE_DATABLOCK_LOCKABLE */

