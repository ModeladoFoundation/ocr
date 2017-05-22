/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_GUID_COUNTED_MAP

#include "debug.h"
#include "guid/counted/counted-map-guid.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-types.h"
#include "ocr-errors.h"

#if defined(TG_XE_TARGET) || defined(TG_CE_TARGET)
#include "xstg-map.h"
#endif

/*
 Implementation doc:
    - A hashtable stores (key, values) pairs of (GUIDs, MdProxy *)
    - A MdProxy is the representent for the actual metadata stored inside
    - Callers can be queued up on the MdProxy. For instance to handle multiple
      concurrent operations.
*/

#define DEBUG_TYPE GUID

#define ENABLE_GUID_BITMAP_BASED 1

// Default hashtable's number of buckets
//PERF: This parameter heavily impacts the GUID provider scalability !
#ifndef GUID_PROVIDER_NB_BUCKETS
#define GUID_PROVIDER_NB_BUCKETS 10000
#endif

// Guid is composed of : (LOCATIONS | KIND | COUNTER)
 // Not used by counted-map. Keep it for code to closer to labeled-guid.
#define GUID_RESERVED_SIZE  (0)
#define GUID_COUNTER_SIZE   (GUID_BIT_COUNT-(GUID_RESERVED_SIZE+GUID_LOCID_SIZE+GUID_KIND_SIZE))

// Start indices for each field
#define SIDX_RESERVED    (GUID_BIT_COUNT)
#define SIDX_LOCID       (SIDX_RESERVED-GUID_RESERVED_SIZE)
#define SIDX_LOCHOME     (SIDX_LOCID)
#define SIDX_LOCALLOC    (SIDX_LOCID-GUID_LOCHOME_SIZE)
#define SIDX_LOCWID      (SIDX_LOCALLOC-GUID_LOCALLOC_SIZE)
#define SIDX_KIND        (SIDX_LOCID-GUID_LOCID_SIZE)
#define SIDX_COUNTER     (SIDX_KIND-GUID_KIND_SIZE)

#ifdef GUID_PROVIDER_CUSTOM_MAP
// Set -DGUID_PROVIDER_CUSTOM_MAP and put other #ifdef for alternate implementation here
#else
#define GP_RESOLVE_HASHTABLE(hashtable, key) hashtable
#define GP_HASHTABLE_CREATE_MODULO newHashtableBucketLocked
#define GP_HASHTABLE_DESTRUCT(hashtable, key, entryDealloc, deallocParam) destructHashtableBucketLocked(hashtable, entryDealloc, deallocParam)
#define GP_HASHTABLE_GET(hashtable, key) hashtableConcBucketLockedGet(GP_RESOLVE_HASHTABLE(hashtable,key), key)
#define GP_HASHTABLE_PUT(hashtable, key, value) hashtableConcBucketLockedPut(GP_RESOLVE_HASHTABLE(hashtable,key), key, value)
#define GP_HASHTABLE_DEL(hashtable, key, valueBack) hashtableConcBucketLockedRemove(GP_RESOLVE_HASHTABLE(hashtable,key), key, valueBack)
#define GP_HASHTABLE_ITERATE(hashtable, iterate, args) iterateHashtable(hashtable, iterate, args)
#endif

#define RSELF_TYPE ocrGuidProviderCountedMap_t

// Utils for bitmap-based GUID implementations
#include "guid/guid-bitmap-based.c"

//BUG #989: MT opportunity
extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
extern u8 createProcessRequestEdtDistPolicy(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid, u64 * paramv);

#ifdef GUID_PROVIDER_DESTRUCT_CHECK

void countedMapHashmapEntryDestructChecker(void * key, void * value, void * deallocParam) {
    ocrGuid_t guid;
#if GUID_BIT_COUNT == 64
    guid.guid = (u64) key;
#elif GUID_BIT_COUNT == 128
    guid.upper = 0x0;
    guid.lower = (u64) key;
#endif
    ((u32*)deallocParam)[getKindFromGuid(guid)]++;
#ifdef GUID_PROVIDER_DESTRUCT_CHECK_VERBOSE
    DPRINTF(DEBUG_LVL_WARN, "Remnant GUID "GUIDF" of kind %s still registered on GUID provider\n", GUIDA(guid),  ocrGuidKindToChar(getKindFromGuid(guid)));
#endif
}

#endif /*GUID_PROVIDER_DESTRUCT_CHECK*/


void countedMapDestruct(ocrGuidProvider_t* self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}


u8 countedMapSwitchRunlevel(ocrGuidProvider_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        // Nothing
        break;
    case RL_PD_OK:
        if ((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
            self->pd = PD;
#if defined(TG_XE_TARGET) || defined(TG_CE_TARGET)
            // HACK: Since we can "query" the GUID provider of another agent, we make
            // the PD address be socket relative so that we extract the correct
            // value irrespective of the agent we are querying from
            {
                ocrLocation_t myLocation = self->pd->myLocation;
                self->pd = (ocrPolicyDomain_t*)(
                    SR_L1_BASE(CLUSTER_FROM_ID(myLocation), BLOCK_FROM_ID(myLocation), AGENT_FROM_ID(myLocation))
                    + (u64)(self->pd) - AR_L1_BASE);
            }
#endif
#ifdef GUID_PROVIDER_WID_INGUID
            ocrGuidProviderCountedMap_t *rself = (ocrGuidProviderCountedMap_t*)self;
            u32 i = 0, ub = PD->workerCount;
            ASSERT(ub <= MAX_VAL(LOCWID));
            u64 max = MAX_VAL(COUNTER);
            u64 incr = (max/ub);
            while (i < ub) {
                // Initialize to 'i' to distribute the count over the buckets. Helps with scalability.
                // This is knowing we use a modulo hash but is not hurting generally speaking...
                rself->guidCounters[i*GUID_WID_CACHE_SIZE] = incr*i;
                i++;
            }
#endif
        }
        break;
    case RL_MEMORY_OK:
        // Nothing to do
        if ((properties & RL_TEAR_DOWN) && RL_IS_FIRST_PHASE_DOWN(PD, RL_GUID_OK, phase)) {
            // What could the map contain at that point ?
            // - Non-freed OCR objects from the user program.
            // - GUIDs internally used by the runtime (module's guids)
            // Since this is below GUID_OK, nobody should have access to those GUIDs
            // anymore and we could dispose of them safely.
            // Note: - Do we want (and can we) destroy user objects ? i.e. need to
            //       call their specific destructors which may not work in MEM_OK ?
            //       - If there are any runtime GUID not deallocated then they should
            //       be considered as leaking memory.
#ifdef GUID_PROVIDER_DESTRUCT_CHECK
            deallocFct entryDeallocator = countedMapHashmapEntryDestructChecker;
            u32 guidTypeCounters[OCR_GUID_MAX];
            u32 i;
            for(i=0; i < OCR_GUID_MAX; i++) {
                guidTypeCounters[i] = 0;
            }
            void * deallocParam = (void *) guidTypeCounters;
#else
            deallocFct entryDeallocator = NULL;
            void * deallocParam = NULL;
#endif
            GP_HASHTABLE_DESTRUCT(((ocrGuidProviderCountedMap_t *) self)->guidImplTable, NULL, entryDeallocator, deallocParam);
#ifdef GUID_PROVIDER_DESTRUCT_CHECK
            PRINTF("=========================\n");
            PRINTF("Remnant GUIDs summary:\n");
            for(i=0; i < OCR_GUID_MAX; i++) {
                if (guidTypeCounters[i] != 0) {
                    PRINTF("%s => %"PRIu32" instances\n", ocrGuidKindToChar(i), guidTypeCounters[i]);
                }
            }
            PRINTF("=========================\n");
#endif
        }
        break;
    case RL_GUID_OK:
        if((properties & RL_BRING_UP) && RL_IS_LAST_PHASE_UP(PD, RL_GUID_OK, phase)) {
            //Initialize the map now that we have an assigned policy domain
            ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
            derived->guidImplTable = GP_HASHTABLE_CREATE_MODULO(PD, GUID_PROVIDER_NB_BUCKETS, hashGuidCounterModulo);
#ifdef GUID_PROVIDER_WID_INGUID
            ASSERT(((PD->workerCount-1) < MAX_VAL(LOCWID)) && "GUID worker count overflows");
#endif
        }
        break;
    case RL_COMPUTE_OK:
        // We can allocate our map here because the memory is up
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

u8 countedMapGuidReserve(ocrGuidProvider_t *self, ocrGuid_t* startGuid, u64* skipGuid,
                         u64 numberGuids, ocrGuidKind guidKind, u32 properties) {
    RSELF_TYPE * rself = (RSELF_TYPE *) self;
#ifdef GUID_PROVIDER_WID_INGUID
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // GUIDs are generated before the current worker is setup.
    u64 wid = ((worker == NULL) ? 0 : worker->id);
    u64 shWid = LSHIFT(LOCWID, wid);
    u64 * counter = &(rself->guidCounters[wid*GUID_WID_CACHE_SIZE]);
#else
    u64 * counter = &(rself->guidCounter);
#endif
    *skipGuid = 1; // Each GUID will just increment by 1
    // Mark the GUID as being reserved
    u64 newGuid = generateNextGuid(self, guidKind, self->pd->myLocation, numberGuids, counter);
#ifdef GUID_PROVIDER_WID_INGUID
    newGuid |= shWid;
#endif
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    startGuid->guid = newGuid;
#elif GUID_BIT_COUNT == 128
    startGuid->upper = 0x0;
    startGuid->lower = newGuid;
#endif
    DPRINTF(DEBUG_LVL_VVERB, "LabeledGUID reserved a range for %"PRIu64" GUIDs starting at "GUIDF"\n",
            numberGuids, GUIDA(*startGuid));

    return 0;

}

u8 countedMapGuidUnreserve(ocrGuidProvider_t *self, ocrGuid_t startGuid, u64 skipGuid,
                           u64 numberGuids) {
    // We do not do anything (we don't reclaim right now)
    return OCR_ENOTSUP;
}

/**
 * @brief Generate a guid for 'val' by increasing the guid counter.
 */
static u8 countedMapGetGuid(ocrGuidProvider_t* self, ocrGuid_t* guid, u64 val, ocrGuidKind kind, ocrLocation_t targetLoc, u32 properties) {
    RSELF_TYPE * rself = (RSELF_TYPE *) self;
#ifdef GUID_PROVIDER_WID_INGUID
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // GUIDs are generated before the current worker is setup.
    u64 wid = ((worker == NULL) ? 0 : worker->id);
    u64 shWid = LSHIFT(LOCWID, wid);
    u64 * counter = &(rself->guidCounters[wid*GUID_WID_CACHE_SIZE]);
#else
    u64 * counter = &(rself->guidCounter);
#endif
    u64 newGuid = generateNextGuid(self, kind, targetLoc, 1, counter);
#ifdef GUID_PROVIDER_WID_INGUID
    newGuid |= shWid;
#endif
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    ocrGuid_t tempGuid = {.guid = newGuid};
#elif GUID_BIT_COUNT == 128
    ocrGuid_t tempGuid = {.lower = (u64)newGuid, .upper = 0x0};
#else
#error Unknown type of GUID
#endif

    if (properties & GUID_PROP_TORECORD) {
        DPRINTF(DEBUG_LVL_VVERB,"Recording %"PRIx64" @ %"PRIx64"\n", newGuid, val);
        // Inject proxy for foreign guids. Stems from pushing OCR objects to other PDs
        void * toPut = (void *) val;
        if (!isLocalGuidCheck(self, tempGuid)) {
            ocrPolicyDomain_t * pd = self->pd;
            MdProxy_t * mdProxy = (MdProxy_t *) pd->fcts.pdMalloc(pd, sizeof(MdProxy_t));
            ADebug(AllocDebugAllPD, "labeledGuidGetGuid(1) size = %ld, addr=%p\n",
                   sizeof(MdProxy_t), mdProxy);
#ifdef ENABLE_RESILIENCY
            mdProxy->base.kind = OCR_GUID_MD_PROXY;
            mdProxy->base.size = sizeof(MdProxy_t);
#endif
            mdProxy->ptr = val;
            DPRINTF(DEBUG_LVL_VVERB,"countedMapGetGuid record "GUIDF"\n", GUIDA(*guid));
            mdProxy->queueHead=REG_CLOSED;
            toPut = (void *) mdProxy;
        }
        GP_HASHTABLE_PUT(((ocrGuidProviderCountedMap_t *) self)->guidImplTable, (void *) newGuid, (void *) toPut);
    }
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    guid->guid =  newGuid;
#elif GUID_BIT_COUNT == 128
    guid->lower = newGuid;
    guid->upper = 0x0;
#else
#error Unknown GUID type
#endif
    return 0;
}

/**
 * @brief Allocates a piece of memory that embeds both
 * the guid and some meta-data payload behind it
 * fatGuid's metaDataPtr will point to.
 */
u8 countedMapCreateGuid(ocrGuidProvider_t* self, ocrFatGuid_t *fguid, u64 size, ocrGuidKind kind, ocrLocation_t targetLoc, u32 properties) {
    if(properties & GUID_PROP_IS_LABELED) {
        // Not supported; use labeled provider
        DPRINTF(DEBUG_LVL_WARN, "error: Must use labeled GUID provider for labeled GUID support, current is counted-map\n");
        ASSERT(false);
    }

    PD_MSG_STACK(msg);
    ocrPolicyDomain_t *policy = NULL;
    getCurrentEnv(&policy, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_ALLOC
    msg.type = PD_MSG_MEM_ALLOC | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(size) = size; // allocate 'size' payload as metadata
    PD_MSG_FIELD_I(properties) = 0;
    PD_MSG_FIELD_I(type) = GUID_MEMTYPE;

    RESULT_PROPAGATE(policy->fcts.processMessage(policy, &msg, true));
    void * ptr = (void *)PD_MSG_FIELD_O(ptr);
    // Two cases, with MD the guid may already be known and we just need to allocate space for the clone
    // else this is a brand new creation, we need to generate a guid and update the fatGuid.
    if (properties & GUID_PROP_ISVALID) {
        if (properties & GUID_PROP_TORECORD) {
            DPRINTF(DEBUG_LVL_VVERB, "Recording "GUIDF" @ %p\n", GUIDA(fguid->guid), ptr);
#if GUID_BIT_COUNT == 64
            u64 guid = fguid->guid.guid;
#elif GUID_BIT_COUNT == 128
            u64 guid = fguid->guid.lower;
#else
#error Unknown type of GUID
#endif
            void * toPut = ptr;
            // Inject proxy for foreign guids. Stems from pushing OCR objects to other PDs.
            // For instance, when doing load-balancing
            if (!isLocalGuidCheck(self, fguid->guid)) {
                ocrPolicyDomain_t * pd = self->pd;
                MdProxy_t * mdProxy = (MdProxy_t *) pd->fcts.pdMalloc(pd, sizeof(MdProxy_t));
                ADebug(AllocDebugAllPD,
                       "labeledGuidGetGuid(2) size = %ld, addr=%p\n",
                       sizeof(MdProxy_t), mdProxy);
#ifdef ENABLE_RESILIENCY
                mdProxy->base.kind = OCR_GUID_MD_PROXY;
                mdProxy->base.size = sizeof(MdProxy_t);
#endif
                mdProxy->ptr = (u64) ptr;
                mdProxy->queueHead = REG_CLOSED;
                toPut = (void *) mdProxy;
            }
            GP_HASHTABLE_PUT(((ocrGuidProviderCountedMap_t *) self)->guidImplTable, (void *) guid, (void *) toPut);
        }
    } else {
        // Generate and record the GUID
        countedMapGetGuid(self, &(fguid->guid), (u64) ptr, kind, targetLoc,
                          (properties & GUID_PROP_TORECORD) ? GUID_PROP_TORECORD : 0);
        DPRINTF(DEBUG_LVL_VVERB, "Generating GUID "GUIDF"\n", GUIDA(fguid->guid));
    }
    ASSERT(!ocrGuidIsNull(fguid->guid) && !ocrGuidIsUninitialized(fguid->guid));
    // Update the fat GUID's metaDataPtr
    fguid->metaDataPtr = ptr;
#undef PD_MSG
#undef PD_TYPE
    return 0;
}


/**
 * @brief Associate an already existing GUID to a value.
 * This is useful in the context of distributed-OCR to register
 * a local metadata represent for a foreign GUID.
 */
u8 countedMapRegisterGuid(ocrGuidProvider_t* self, ocrGuid_t guid, u64 val) {
    ocrGuidProviderCountedMap_t * dself = (ocrGuidProviderCountedMap_t *) self;
#if GUID_BIT_COUNT == 64
        void * rguid = (void *) guid.guid;
#elif GUID_BIT_COUNT == 128
        void * rguid = (void *) guid.lower;
#else
#error Unknown type of GUID
#endif
    int oth = (int) locIdtoLocation(extractLocIdFromGuid(guid));
    if (isLocalGuidCheck(self, guid)) {
        // See BUG #928 on GUID issues
        GP_HASHTABLE_PUT(dself->guidImplTable, (void *) rguid, (void *) val);
        ASSERT(oth == self->pd->myLocation);
    } else {
        MdProxy_t * mdProxy = (MdProxy_t *) GP_HASHTABLE_GET(dself->guidImplTable, rguid);
        // Must have setup a mdProxy before being able to register.
        ASSERT(mdProxy != NULL);
        mdProxy->ptr = val;
        hal_fence(); // This may be redundant with the CAS
        u64 newValue = (u64) REG_CLOSED;
        DPRINTF(DEBUG_LVL_VVERB,"countedMapRegisterGuid record "GUIDF"\n", GUIDA(guid));
        u64 curValue = 0;
        u64 oldValue = 0;
        do {
            MdProxyNode_t * head = mdProxy->queueHead;
            ASSERT(head != REG_CLOSED);
            curValue = (u64) head;
            oldValue = hal_cmpswap64((u64*) &(mdProxy->queueHead), curValue, newValue);
        } while(oldValue != curValue);
        MdProxyNode_t * queueHead = (MdProxyNode_t *) oldValue;
        if (((u64)queueHead) != REG_OPEN) {
            ocrGuid_t processRequestTemplateGuid;
            ocrEdtTemplateCreate(&processRequestTemplateGuid, &processRequestEdt, 1, 0);
            DPRINTF(DEBUG_LVL_VVERB,"About to process stored clone requests for GUID "GUIDF" queueHead=%p)\n", GUIDA(guid), queueHead);
            //BUG #989: MT opportunity - Asynchronously process operations queued on the MD to be available
            //TODO instead of going over these one after the other to find out they may not be enabled,
            //submit the bulk to the MD so that it can sort it out
            while (queueHead != ((void*) REG_OPEN)) { // sentinel value
                DPRINTF(DEBUG_LVL_VVERB,"Processing stored clone requests for GUID "GUIDF"\n", GUIDA(guid));
                ocrPolicyMsg_t * msg = queueHead->msg;
                if ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) {
                    //TODO-MD-DBRTACQ
                    // This is to let the PD know the message is re-processed and
                    // there's no calling context that will read the response.
                    ASSERT(msg->type & PD_MSG_REQ_RESPONSE);
                    msg->type &= ~PD_MSG_REQ_RESPONSE;
                }
                u64 paramv = (u64) queueHead->msg;
                ocrPolicyDomain_t * pd = self->pd;
                createProcessRequestEdtDistPolicy(pd, processRequestTemplateGuid, &paramv);
                MdProxyNode_t * currNode = queueHead;
                queueHead = queueHead->next;
                pd->fcts.pdFree(pd, currNode);
            }
            ocrEdtTemplateDestroy(processRequestTemplateGuid);
        }
    }
    return 0;
}


/**
 * @brief Returns the value associated with a guid and its kind if requested.
 *        This is potentially asynchronous. Check the return value for OCR_EPEND
 * @param[in] mode  Control whether or not to fetch the MD if not available
 *
 *
 */
static u8 countedMapGetVal(ocrGuidProvider_t* self, ocrGuid_t guid, u64* val, ocrGuidKind* kind, u32 mode, MdProxy_t ** proxy) {
    ASSERT(!ocrGuidIsNull(guid) && !ocrGuidIsError(guid) && !ocrGuidIsUninitialized(guid));

    ocrGuidProviderCountedMap_t * dself = (ocrGuidProviderCountedMap_t *) self;
    if (kind) {
        *kind = getKindFromGuid(guid);
    }

    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    void * rguid = (void *) guid.guid;
#elif GUID_BIT_COUNT == 128
    void * rguid = (void *) guid.lower;
#else
#error Unknown type of GUID
#endif

    if (isLocalGuidCheck(self, guid)) {
        *val = (u64) GP_HASHTABLE_GET(dself->guidImplTable, rguid);
    } else {
        // The GUID is remote, check if we have a local representent or need to fetch
        *val = 0; // Important for return code to be set properly
        MdProxy_t * mdProxy = (MdProxy_t *) GP_HASHTABLE_GET(dself->guidImplTable, rguid);
        if (mdProxy == NULL) {
            if (mode == MD_LOCAL) {
                return 0;
            } // else the mode is fetch
            // This is a concurrent operation. Multiple concurrent call may try to do the fetch
            ASSERT(((mode == MD_FETCH) || (mode == MD_PROXY)) && (proxy != NULL));
            // Optimistically try to enqueue.
            ocrPolicyDomain_t * pd = self->pd;
            mdProxy = (MdProxy_t *) pd->fcts.pdMalloc(pd, sizeof(MdProxy_t));
            ADebug(AllocDebugAllPD, "labeledGuidGetGuid(3) size = %ld, addr=%p\n",
                   sizeof(MdProxy_t), mdProxy);
#ifdef ENABLE_RESILIENCY
            mdProxy->base.kind = OCR_GUID_MD_PROXY;
            mdProxy->base.size = sizeof(MdProxy_t);
#endif
            // This is where failed attempts would register for callback
            mdProxy->queueHead = (void *) REG_OPEN; // sentinel value
            mdProxy->ptr = 0;
            hal_fence(); // I think the lock in try put should make the writes visible
            MdProxy_t * oldMdProxy = (MdProxy_t *) hashtableConcBucketLockedTryPut(dself->guidImplTable, rguid, mdProxy);
            if (oldMdProxy == mdProxy) { // won
                if (mode == MD_PROXY) {
                    *proxy = oldMdProxy;
                    return 0;
                }
                // TODO two options:
                // 1- Issue the MD cloning here and link the operation's completion to the mdProxy
                // Sketch implementation:
                // - Get low-level info
                //   - no-op for now because we extract kind from GUID and factory is always 0
                //   * TODO gp->resolveLowLevelInfo(gp); // no-op
                // - Once we have that:
                //   - Read the kind and factory id
                //      * TODO: Create base type ocrObjectFactory_t for all factories to extend
                //      * TODO: ocrObjectFactory_t * pd->resolveFactory(pd, ocrGuid_t);
                //          * Q: Does PD is the right place to have the factories ?
                //   - Invoke "clone/fetch" code. This is a non-blocking call that will return OCR_EPEND
                //      * TODO: ocrObjectFactory_t interface to call deserialize with convention that srcBuffer==NULL
                //
                PD_MSG_STACK(msgClone);
                getCurrentEnv(NULL, NULL, NULL, &msgClone);
#define PD_MSG (&msgClone)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
                msgClone.type = PD_MSG_GUID_METADATA_CLONE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = guid;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(type) = MD_CLONE;
                PD_MSG_FIELD_I(dstLocation) = pd->myLocation;
                // The message processing is asynchronous
                u8 returnCode = pd->fcts.processMessage(pd, &msgClone, false);
                if (returnCode == 0) { // Clone succeeded
                    // This code is potentially concurrent with other clones
                    countedMapRegisterGuid(self, guid, (u64) PD_MSG_FIELD_IO(guid.metaDataPtr));
                    *val = (u64) PD_MSG_FIELD_IO(guid.metaDataPtr);
                    *proxy = NULL;
                } else {
                    // Warning: after this call we're potentially concurrent with the MD being registered on the GP
                    ASSERT(returnCode == OCR_EPEND);
                }
#undef PD_MSG
#undef PD_TYPE
                // 2- Return an error code along with the oldMdProxy event
                //    The caller will be responsible for calling MD cloning
                //    and setup the link. Note there's a race here when the
                //    md is resolve concurrently. It sounds it would be better
                //    to go through functions.
            } else {
                ASSERT(mode != MD_PROXY); // By contract, no competition on MD_PROXY
                // lost competition, 2 cases:
                // 1) The MD is available (it's concurrent to this thread of execution)
                // 2) The MD is still being fetch
                pd->fcts.pdFree(pd, mdProxy); // we failed, free our proxy.
                // Read the content of the proxy anyhow
                // TODO: Open a feature bug for that.
                // It is safe to read the ptr because there cannot be a racing remove
                // operation on that entry. For now we made the choice that we do not
                // eagerly reclaim entries to evict ununsed GUID. The only time a GUID
                // is removed from the map is when the OCR object it represents is being
                // destroyed (hence there should be no concurrent read at that time).
                // This is a racy check but it's ok, the caller would have to enqueue itself
                // on the proxy and the race is addressed there.
                *val = (u64) oldMdProxy->ptr;
                if (*val == 0) {
                    // MD is still being fetch, multiple options:
                    // - Opt 1: Continuation
                    //          WAIT_FOR(oldMdProxy);
                    //          When resumed, the metadata is available locally
                    // - Opt 2: Return a 'proxy' runtime event for caller to register on
                    // For now return OCR_EPEND and let the caller deal with it
                }
                mdProxy = oldMdProxy;
            }
        } else { // mdProxy not NULL
            // Implementation limitation. For now DB relies on the proxy mecanism in hc-dist-policy
            *val = (u64) mdProxy->ptr;
        }
        if (mode == MD_FETCH) {
            ASSERT(proxy != NULL);
            *proxy = mdProxy;
            DPRINTF(DEBUG_LVL_VVERB, "MD_FETCH for "GUIDF"\n", GUIDA(guid));
        } else {
            ASSERT(proxy == NULL);
        }
    } // end GUID is not local
    return (*val) ? 0 : OCR_EPEND;
}

/**
 * @brief Remove an already existing GUID and its associated value from the provider
 */
u8 countedMapUnregisterGuid(ocrGuidProvider_t* self, ocrGuid_t guid, u64 ** val) {
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    GP_HASHTABLE_DEL(((ocrGuidProviderCountedMap_t *) self)->guidImplTable, (void *) guid.guid, (void **) val);
#elif GUID_BIT_COUNT == 128
    GP_HASHTABLE_DEL(((ocrGuidProviderCountedMap_t *) self)->guidImplTable, (void *) guid.lower, (void **) val);
#else
#error Unknown type of GUID
#endif
    return 0;
}

u8 countedMapReleaseGuid(ocrGuidProvider_t *self, ocrFatGuid_t fatGuid, bool releaseVal) {
    ocrGuid_t guid = fatGuid.guid;
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    void * lguid = (void *) guid.guid;
#elif GUID_BIT_COUNT == 128
    void * lguid = (void *) guid.lower;
#else
#error Unknown GUID type
#endif
    RESULT_ASSERT(GP_HASHTABLE_DEL(derived->guidImplTable, lguid, NULL), ==, true);
    // If there's metaData associated with guid we need to deallocate memory
    if(releaseVal && (fatGuid.metaDataPtr != NULL)) {
        PD_MSG_STACK(msg);
        ocrPolicyDomain_t *policy = NULL;
        getCurrentEnv(&policy, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_UNALLOC
        msg.type = PD_MSG_MEM_UNALLOC | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_I(allocatingPD.guid) = NULL_GUID;
        PD_MSG_FIELD_I(allocatingPD.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(allocator.guid) = NULL_GUID;
        PD_MSG_FIELD_I(allocator.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(ptr) = fatGuid.metaDataPtr;
        PD_MSG_FIELD_I(type) = GUID_MEMTYPE;
        PD_MSG_FIELD_I(properties) = 0;
        RESULT_PROPAGATE(policy->fcts.processMessage(policy, &msg, true));
#undef PD_MSG
#undef PD_TYPE
    }
    return 0;
}

#ifdef ENABLE_RESILIENCY

//Returns the serialization size of the MdProxy only,
//excluding the size of linked OCR object in mdProxy->ptr
u64 getSerializationSizeMdProxy(MdProxy_t *mdProxy) {
    ASSERT((mdProxy != NULL) && (mdProxy->base.kind == OCR_GUID_MD_PROXY));
    u64 mdProxySize = sizeof(MdProxy_t);
    mdProxy->numNodes = 0;
    if (mdProxy->queueHead != REG_CLOSED) {
        MdProxyNode_t * queueNode = mdProxy->queueHead;
        while (queueNode != ((void*) REG_OPEN)) { // sentinel value
            mdProxySize += sizeof(MdProxyNode_t);
            u64 baseSize = 0, marshalledSize = 0;
            ocrPolicyMsgGetMsgSize(queueNode->msg, &baseSize, &marshalledSize, MARSHALL_FULL_COPY | MARSHALL_NSADDR);
            mdProxySize += baseSize + marshalledSize;
            mdProxy->numNodes++;
            queueNode = queueNode->next;
        }
    }
    mdProxy->base.size = mdProxySize;
    return mdProxySize;
}

//Serializes MdProxy only into the buffer,
//excluding the linked OCR object in mdProxy->ptr
//Returns the size of buffer used to serialize
u64 serializeMdProxy(MdProxy_t *mdProxy, u8* buffer) {
    ASSERT(buffer);
    ASSERT((mdProxy != NULL) && (mdProxy->base.kind == OCR_GUID_MD_PROXY));
    u8* bufferHead = buffer;
    MdProxy_t *mdProxyBuf = (MdProxy_t*)buffer;
    u64 len = sizeof(MdProxy_t);
    hal_memCopy(buffer, mdProxy, len, false);
    buffer += len;

    u32 numNodes = 0;
    if (mdProxy->queueHead != REG_CLOSED) {
        MdProxyNode_t * queueNode = mdProxy->queueHead;
        mdProxyBuf->queueHead = (MdProxyNode_t*)buffer;
        while (queueNode != ((void*) REG_OPEN)) { // sentinel value
            MdProxyNode_t *queueBuf = (MdProxyNode_t*)buffer;
            len = sizeof(MdProxyNode_t);
            hal_memCopy(buffer, queueNode, len, false);
            buffer += len;

            //Serialize msg
            queueBuf->msg = (ocrPolicyMsg_t*)buffer;
            u64 baseSize = 0, marshalledSize = 0;
            ocrPolicyMsgGetMsgSize(queueNode->msg, &baseSize, &marshalledSize, MARSHALL_NSADDR);
            len = baseSize + marshalledSize;
            ocrPolicyMsg_t *msgBuf = (ocrPolicyMsg_t*)buffer;
            initializePolicyMessage(msgBuf, len);
            ocrPolicyMsgMarshallMsg(queueNode->msg, baseSize, buffer, MARSHALL_FULL_COPY | MARSHALL_NSADDR);
            ASSERT(queueBuf->msg->bufferSize == len);
            buffer += len;

            numNodes++;
            queueNode = queueNode->next;
            if (queueNode != ((void*) REG_OPEN)) {
                queueBuf->next = (MdProxyNode_t*)buffer;
            } else {
                ASSERT(queueBuf->next == ((void*) REG_OPEN));
            }
        }
    }

    mdProxyBuf->ptr = (u64) buffer; //ocrObject in ptr will be serialized after this function returns
    u64 offset = buffer - bufferHead;
    ASSERT(numNodes == mdProxy->numNodes);
    ASSERT(offset == mdProxy->base.size);
    return offset;
}

//Deserializes and creates a new MdProxy object from the buffer,
//excluding the linked OCR object in mdProxy->ptr
//Returns the size of buffer used to deserialize
u64 deserializeMdProxy(u8* buffer, MdProxy_t **proxy) {
    ASSERT(buffer);
    ASSERT(proxy);
    u8* bufferHead = buffer;
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    u64 len = sizeof(MdProxy_t);
    MdProxy_t *mdProxy = (MdProxy_t*)pd->fcts.pdMalloc(pd, len);
    ADebug(AllocDebugAllPD, "deserializeMdProxy(1) size = %ld, addr=%p\n",
           len, mdProxy);
    hal_memCopy(mdProxy, buffer, len, false);
    ASSERT(mdProxy->base.kind == OCR_GUID_MD_PROXY);
    mdProxy->ptr = 0; //this will be setup after the ocr object is deserialized
    buffer += len;

    if (mdProxy->queueHead != REG_CLOSED) {
        mdProxy->queueHead = NULL;
        MdProxyNode_t *queuePrev = NULL;
        u32 i;
        for (i = 0; i < mdProxy->numNodes; i++) {
            len = sizeof(MdProxyNode_t);
            MdProxyNode_t *mdProxyNode = (MdProxyNode_t*)pd->fcts.pdMalloc(pd, len);
            ADebug(AllocDebugAllPD, "deserializeMdProxy(2) size = %ld, addr=%p\n",
                   len, mdProxyNode);
            hal_memCopy(mdProxyNode, buffer, len, false);
            mdProxyNode->next = ((void*) REG_OPEN);
            if (mdProxy->queueHead == NULL) {
                mdProxy->queueHead = mdProxyNode;
            } else {
                ASSERT(queuePrev != NULL);
                queuePrev->next = mdProxyNode;
            }
            queuePrev = mdProxyNode;
            buffer += len;

            ocrPolicyMsg_t *msgBuf = (ocrPolicyMsg_t*)buffer;
            len = msgBuf->bufferSize;
            ocrPolicyMsg_t *msg = (ocrPolicyMsg_t*)pd->fcts.pdMalloc(pd, len);
            ADebug(AllocDebugAllPD, "deserializeMdProxy(3) size = %ld, addr=%p\n",
                   len, msg);
            initializePolicyMessage(msg, len);
            ocrPolicyMsgUnMarshallMsg(buffer, NULL, msg, MARSHALL_FULL_COPY | MARSHALL_NSADDR);
            mdProxyNode->msg = msg;
            buffer += len;
        }
    }

    u64 offset = buffer - bufferHead;
    ASSERT(offset == mdProxy->base.size);
    *proxy = mdProxy;
    return offset;
}

//Fixup pointers in an MdProxy object excluding
//the linked OCR object in mdProxy->ptr
void fixupMdProxy(MdProxy_t *mdProxy) {
    //Nothing to fixup
    return;
}

//Destructs an MdProxy object excluding the
//linked OCR object in mdProxy->ptr
void destructMdProxy(MdProxy_t *mdProxy) {
    ASSERT((mdProxy != NULL) && (mdProxy->base.kind == OCR_GUID_MD_PROXY));
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if (mdProxy->queueHead != REG_CLOSED) {
        MdProxyNode_t * queueNode = mdProxy->queueHead;
        while (queueNode != ((void*) REG_OPEN)) { // sentinel value
            MdProxyNode_t * curNode = (MdProxyNode_t*)queueNode;
            queueNode = queueNode->next;
            ASSERT(curNode->msg);
            pd->fcts.pdFree(pd, curNode->msg);
            pd->fcts.pdFree(pd, curNode);
        }
    }
    pd->fcts.pdFree(pd, mdProxy);
}

void calcSerializationSize(void * key, void * value, void * args) {
    ASSERT(key != NULL);
    ASSERT(value != NULL);
    ASSERT(args != NULL);

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t * curEdt = NULL;
    getCurrentEnv(&pd, NULL, &curEdt, NULL);

    //Do not checkpoint current EDT
    if (value == curEdt)
        return;

    u64 *size = (u64*)args;
    ocrGuid_t guid;
#if GUID_BIT_COUNT == 64
    guid.guid = (u64)key;
#elif GUID_BIT_COUNT == 128
    guid.upper = 0;
    guid.lower = (u64)key;
#else
#error Unknown type of GUID
#endif

    ocrGuidProvider_t* self = pd->guidProviders[0];
    ocrGuidKind kind;
    self->fcts.getKind(self, guid, &kind);

    ocrObject_t * ocrObj = (ocrObject_t*)value;
    MdProxy_t * mdProxy = NULL;
    u64 mdProxySize = 0;
    if (!isLocalGuidCheck(self, guid)) {
        mdProxy = (MdProxy_t*)value;
        ASSERT(mdProxy->base.kind == OCR_GUID_MD_PROXY);
        ocrObj = (ocrObject_t*)mdProxy->ptr;
        ASSERT(ocrObj); // TODO-resilience: this can be null if the MD hasn't been resolved yet
        mdProxySize = getSerializationSizeMdProxy(mdProxy);
        ASSERT(mdProxySize > 0 && mdProxy->base.size == mdProxySize);
    }

    u64 mdSize = 0;
    if (kind == OCR_GUID_DB) {
        ASSERT(ocrObj->kind == kind);
        ocrDataBlock_t *db = (ocrDataBlock_t*)ocrObj;
        ASSERT(ocrGuidIsEq(guid, db->guid));
        ((ocrDataBlockFactory_t*)(pd->factories[pd->datablockFactoryIdx]))->fcts.getSerializationSize(db, &mdSize);
        ASSERT(mdSize > 0 || ocrObj->size == mdSize);
    } else if (kind == OCR_GUID_EDT) {
        ASSERT(ocrObj->kind == kind);
        ocrTask_t *edt = (ocrTask_t*)ocrObj;
        ASSERT(ocrGuidIsEq(guid, edt->guid));
        ((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.getSerializationSize(edt, &mdSize);
        ASSERT(mdSize > 0 || ocrObj->size == mdSize);
    } else if (kind == OCR_GUID_EDT_TEMPLATE) {
        ASSERT(ocrObj->kind == kind);
        ocrTaskTemplate_t *tmpl = (ocrTaskTemplate_t*)ocrObj;
        ((ocrTaskTemplateFactory_t*)(pd->factories[pd->taskTemplateFactoryIdx]))->fcts.getSerializationSize(tmpl, &mdSize);
        ASSERT(mdSize > 0 || ocrObj->size == mdSize);
    } else if (kind & OCR_GUID_EVENT) {
        ASSERT(ocrObj->kind == kind);
        ocrEvent_t *evt = (ocrEvent_t*)ocrObj;
        ASSERT(ocrGuidIsEq(guid, evt->guid));
        ((ocrEventFactory_t*)(pd->factories[pd->eventFactoryIdx]))->commonFcts.getSerializationSize(evt, &mdSize);
        ASSERT(mdSize > 0 || ocrObj->size == mdSize);
    } else if (kind == OCR_GUID_AFFINITY) {
        mdSize = sizeof(ocrAffinity_t);
    } else {
        switch(kind) {
        case OCR_GUID_ALLOCATOR:
        case OCR_GUID_POLICY:
        case OCR_GUID_WORKER:
        case OCR_GUID_MEMTARGET:
        case OCR_GUID_COMPTARGET:
        case OCR_GUID_SCHEDULER:
        case OCR_GUID_WORKPILE:
        case OCR_GUID_COMM:
        case OCR_GUID_SCHEDULER_OBJECT:
        case OCR_GUID_SCHEDULER_HEURISTIC:
        case OCR_GUID_GUIDMAP:
            {
                ASSERT(mdProxy == NULL);
                return;
            }
        default:
            {
                DPRINTF(DEBUG_LVL_WARN, "Unknown guid kind found\n");
                ASSERT(0);
                return;
            }
        }
    }
    ASSERT(mdSize > 0);
    *size += sizeof(ocrGuid_t) + mdProxySize + mdSize;

    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    derived->objectsCounted++;
}

u8 getSerializationSizeGuidProviderCounted(ocrGuidProvider_t* self, u64* size) {
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    derived->objectsCounted = 0;
    *size = 0;
    GP_HASHTABLE_ITERATE(derived->guidImplTable, calcSerializationSize, (void*)size);
    ASSERT(derived->objectsCounted > 0 && *size > 0);
    *size += sizeof(ocrObject_t) + sizeof(u64);
#ifdef GUID_PROVIDER_WID_INGUID
#error "Unsupported option for resiliency"
#endif
    self->base.size = *size;
    self->base.kind = OCR_GUID_GUIDMAP;
    return 0;
}

void serializeGuid(void * key, void * value, void * args) {
    ASSERT(key != NULL);
    ASSERT(value != NULL);
    ASSERT(args != NULL);

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t * curEdt = NULL;
    getCurrentEnv(&pd, NULL, &curEdt, NULL);

    //Do not checkpoint current EDT
    if (value == curEdt)
        return;

    u8 **buffer = (u8**)args;
    u8* ptr = *buffer;
    ocrGuid_t guid;
#if GUID_BIT_COUNT == 64
    guid.guid = (u64)key;
#elif GUID_BIT_COUNT == 128
    guid.upper = 0;
    guid.lower = (u64)key;
#else
#error Unknown type of GUID
#endif

    *((ocrGuid_t*)ptr) = guid;
    ptr += sizeof(ocrGuid_t);

    u64 len = 0;
    ocrGuidProvider_t* self = pd->guidProviders[0];
    ocrGuidKind kind;
    self->fcts.getKind(self, guid, &kind);

    ocrObject_t * ocrObj = (ocrObject_t*)value;
    MdProxy_t * mdProxy = NULL;
    if (!isLocalGuidCheck(self, guid)) {
        mdProxy = (MdProxy_t*)value;
        ASSERT(mdProxy->base.kind == OCR_GUID_MD_PROXY);
        ocrObj = (ocrObject_t*)mdProxy->ptr;
        len = serializeMdProxy(mdProxy, ptr);
        ptr += len;
    }
    ASSERT(ocrObj);

    u64 size = 0;
    if (kind == OCR_GUID_DB) {
        ASSERT(ocrObj->kind == kind);
        size = ocrObj->size;
        ocrDataBlock_t *db = (ocrDataBlock_t*)ocrObj;
        ASSERT(ocrGuidIsEq(guid, db->guid));
        //TODO-resilience: use standard API
        ((ocrDataBlockFactory_t*)(pd->factories[pd->datablockFactoryIdx]))->fcts.serialize(db, ptr);
    } else if (kind == OCR_GUID_EDT) {
        ASSERT(ocrObj->kind == kind);
        size = ocrObj->size;
        ocrTask_t *edt = (ocrTask_t*)ocrObj;
        ASSERT(ocrGuidIsEq(guid, edt->guid));
        //TODO-resilience: use standard API
        ((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.serialize(edt, ptr);
    } else if (kind == OCR_GUID_EDT_TEMPLATE) {
        ASSERT(ocrObj->kind == kind);
        size = ocrObj->size;
        ocrTaskTemplate_t *tmpl = (ocrTaskTemplate_t*)ocrObj;
        //TODO-resilience: use standard API
        ((ocrTaskTemplateFactory_t*)(pd->factories[pd->taskTemplateFactoryIdx]))->fcts.serialize(tmpl, ptr);
    } else if (kind & OCR_GUID_EVENT) {
        ASSERT(ocrObj->kind == kind);
        size = ocrObj->size;
        ocrEvent_t *evt = (ocrEvent_t*)ocrObj;
        ASSERT(ocrGuidIsEq(guid, evt->guid));
        //TODO-resilience: use standard API
        ((ocrEventFactory_t*)(pd->factories[pd->eventFactoryIdx]))->commonFcts.serialize(evt, ptr);
    } else if (kind == OCR_GUID_AFFINITY) {
        size = sizeof(ocrAffinity_t);
        hal_memCopy(ptr, value, size, false);
    } else {
        switch(kind) {
        case OCR_GUID_ALLOCATOR:
        case OCR_GUID_POLICY:
        case OCR_GUID_WORKER:
        case OCR_GUID_MEMTARGET:
        case OCR_GUID_COMPTARGET:
        case OCR_GUID_SCHEDULER:
        case OCR_GUID_WORKPILE:
        case OCR_GUID_COMM:
        case OCR_GUID_SCHEDULER_OBJECT:
        case OCR_GUID_SCHEDULER_HEURISTIC:
        case OCR_GUID_GUIDMAP:
            {
                ASSERT(mdProxy == NULL);
                return;
            }
        default:
            {
                DPRINTF(DEBUG_LVL_WARN, "Unknown guid kind found in serialize\n");
                ASSERT(0);
                return;
            }
        }
    }
    ASSERT(size > 0);
    ptr += size;

    *buffer = ptr;

    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    derived->objectsSerialized++;
}

u8 serializeGuidProviderCounted(ocrGuidProvider_t* self, u8* buffer) {
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    derived->objectsSerialized = 0;
    u8* bufferHead = buffer;
    ocrObject_t * ocrObj = (ocrObject_t *)bufferHead;
    *ocrObj = self->base;
    buffer += sizeof(ocrObject_t);
    u64 *guidCounter = (u64*)buffer;
    *guidCounter = derived->guidCounter;
    buffer += sizeof(u64);

    GP_HASHTABLE_ITERATE(derived->guidImplTable, serializeGuid, (void*)(&buffer));

    if ((buffer - bufferHead) != self->base.size) {
        DPRINTF(DEBUG_LVL_WARN, "Checkpoint buffer overflow! (Buffer Size: %lu Serialized Size: %lu Overflow: %lu Start: %p End: %p)\n",
            self->base.size, (buffer - bufferHead), ((buffer - bufferHead) - self->base.size), bufferHead, buffer);
        DPRINTF(DEBUG_LVL_WARN, "Objects counted: %lu Objects serialized: %lu\n", derived->objectsCounted, derived->objectsSerialized);
        ASSERT(0);
    }
    return 0;
}

u8 deserializeGuidProviderCounted(ocrGuidProvider_t* self, u8* buffer) {
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;

    ocrObject_t * ocrObj = (ocrObject_t *)buffer;
    ASSERT(ocrObj->kind == OCR_GUID_GUIDMAP);
    u8* endOfBuffer = buffer + ocrObj->size;
    buffer += sizeof(ocrObject_t);
    u64 *guidCounter = (u64*)buffer;
    derived->guidCounter = *guidCounter;
    buffer += sizeof(u64);

    while(buffer < endOfBuffer) {
        ocrGuid_t guid = *((ocrGuid_t*)buffer);
        ASSERT(!ocrGuidIsNull(guid));
        buffer += sizeof(ocrGuid_t);

        ocrGuidKind kind;
        self->fcts.getKind(self, guid, &kind);

        MdProxy_t * mdProxy = NULL;
        if (!isLocalGuidCheck(self, guid)) {
            u64 mdProxySize = deserializeMdProxy(buffer, &mdProxy);
            ASSERT(mdProxySize > 0 && mdProxy != NULL);
            buffer += mdProxySize;
        }

        u64 size = 0;
        void *val = NULL;
        if (kind == OCR_GUID_DB) {
            ocrObj = (ocrObject_t *)buffer;
            size = ocrObj->size;
            ASSERT(ocrObj->kind == kind);
            ocrDataBlock_t *db = NULL;
            ((ocrDataBlockFactory_t*)(pd->factories[pd->datablockFactoryIdx]))->fcts.deserialize(buffer, &db);
            ASSERT(db != NULL && ocrGuidIsEq(guid, db->guid));
            val = db;
        } else if (kind == OCR_GUID_EDT) {
            ocrObj = (ocrObject_t *)buffer;
            ASSERT(ocrObj->kind == kind);
            size = ocrObj->size;
            ocrTask_t *edt = NULL;
            ((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.deserialize(buffer, &edt);
            ASSERT(edt != NULL && ocrGuidIsEq(guid, edt->guid));
            val = edt;
        } else if (kind == OCR_GUID_EDT_TEMPLATE) {
            ocrObj = (ocrObject_t *)buffer;
            ASSERT(ocrObj->kind == kind);
            size = ocrObj->size;
            ocrTaskTemplate_t *tmpl = NULL;
            ((ocrTaskTemplateFactory_t*)(pd->factories[pd->taskTemplateFactoryIdx]))->fcts.deserialize(buffer, &tmpl);
            val = tmpl;
        } else if (kind & OCR_GUID_EVENT) {
            ocrObj = (ocrObject_t *)buffer;
            ASSERT(ocrObj->kind == kind);
            size = ocrObj->size;
            ocrEvent_t *evt = NULL;
            ((ocrEventFactory_t*)(pd->factories[pd->eventFactoryIdx]))->commonFcts.deserialize(buffer, &evt);
            ASSERT(evt != NULL && ocrGuidIsEq(guid, evt->guid));
            val = evt;
        } else if (kind == OCR_GUID_AFFINITY) {
            size = sizeof(ocrAffinity_t);
            ocrAffinity_t *aff = pd->fcts.pdMalloc(pd, size);
            ADebug(AllocDebugAllPD,
                   "deserializeGuidProviderCounted(1) size = %ld, addr=%p\n",
                   size, aff);
            hal_memCopy(aff, buffer, size, false);
            val = aff;
        } else {
            DPRINTF(DEBUG_LVL_WARN, "Unknown guid kind found in deserialize\n");
            ASSERT(0);
            return 1;
        }
        ASSERT(size > 0 && val != NULL);

        if (mdProxy != NULL) {
            mdProxy->ptr = (u64)val;
            val = mdProxy;
        }

#if GUID_BIT_COUNT == 64
        GP_HASHTABLE_PUT(derived->guidImplTable, (void *) guid.guid, val);
#elif GUID_BIT_COUNT == 128
        GP_HASHTABLE_PUT(derived->guidImplTable, (void *) guid.lower, val);
#else
#error Unknown type of GUID
#endif
        buffer += size;
    }
    return 0;
}

void fixupGuid(void * key, void * value, void * args) {
    ASSERT(key != NULL);
    ASSERT(value != NULL);
    ASSERT(args != NULL);

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t * curEdt = NULL;
    getCurrentEnv(&pd, NULL, &curEdt, NULL);

    //Do not change current EDT
    if (value == curEdt)
        return;

    ocrGuid_t guid;
#if GUID_BIT_COUNT == 64
    guid.guid = (u64)key;
#elif GUID_BIT_COUNT == 128
    guid.upper = 0;
    guid.lower = (u64)key;
#else
#error Unknown type of GUID
#endif
    ocrGuidProvider_t* self = pd->guidProviders[0];
    ocrGuidKind kind;
    self->fcts.getKind(self, guid, &kind);

    MdProxy_t * mdProxy = NULL;
    if (!isLocalGuidCheck(self, guid)) {
        mdProxy = (MdProxy_t*)value;
        ASSERT(mdProxy->base.kind == OCR_GUID_MD_PROXY);
        value = (void*)mdProxy->ptr;
        ASSERT(value);
        fixupMdProxy(mdProxy);
    }

    if (kind == OCR_GUID_DB) {
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrDataBlock_t *db = (ocrDataBlock_t*)value;
        ASSERT(ocrGuidIsEq(guid, db->guid));
        ((ocrDataBlockFactory_t*)(pd->factories[pd->datablockFactoryIdx]))->fcts.fixup(db);
    } else if (kind == OCR_GUID_EDT) {
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrTask_t *edt = (ocrTask_t*)value;
        ASSERT(ocrGuidIsEq(guid, edt->guid));
        ((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.fixup(edt);
    } else if (kind == OCR_GUID_EDT_TEMPLATE) {
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrTaskTemplate_t *tmpl = (ocrTaskTemplate_t*)value;
        ((ocrTaskTemplateFactory_t*)(pd->factories[pd->taskTemplateFactoryIdx]))->fcts.fixup(tmpl);
    } else if (kind & OCR_GUID_EVENT) {
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrEvent_t *evt = (ocrEvent_t*)value;
        ASSERT(ocrGuidIsEq(guid, evt->guid));
        ((ocrEventFactory_t*)(pd->factories[pd->eventFactoryIdx]))->commonFcts.fixup(evt);
    } else if (kind == OCR_GUID_AFFINITY) {
        //Nothing to fixup
    } else {
        switch(kind) {
        case OCR_GUID_ALLOCATOR:
        case OCR_GUID_POLICY:
        case OCR_GUID_WORKER:
        case OCR_GUID_MEMTARGET:
        case OCR_GUID_COMPTARGET:
        case OCR_GUID_SCHEDULER:
        case OCR_GUID_WORKPILE:
        case OCR_GUID_COMM:
        case OCR_GUID_SCHEDULER_OBJECT:
        case OCR_GUID_SCHEDULER_HEURISTIC:
        case OCR_GUID_GUIDMAP:
            return;
        default:
            {
                DPRINTF(DEBUG_LVL_WARN, "Unknown guid kind found\n");
                ASSERT(0);
                return;
            }
        }
    }
}

u8 fixupGuidProviderCounted(ocrGuidProvider_t* self) {
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    GP_HASHTABLE_ITERATE(derived->guidImplTable, fixupGuid, self);
    return 0;
}

void resetProgramState(void * key, void * value, void * args) {
    ASSERT(key != NULL);
    ASSERT(value != NULL);
    ASSERT(args != NULL);
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) args;

    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t * curEdt = NULL;
    getCurrentEnv(&pd, NULL, &curEdt, NULL);

    //Do not reset current EDT
    if (value == curEdt)
        return;

    ocrGuid_t guid;
#if GUID_BIT_COUNT == 64
    guid.guid = (u64)key;
#elif GUID_BIT_COUNT == 128
    guid.upper = 0;
    guid.lower = (u64)key;
#else
#error Unknown type of GUID
#endif
    ocrGuidProvider_t* self = pd->guidProviders[0];
    ocrGuidKind kind;
    self->fcts.getKind(self, guid, &kind);

    MdProxy_t * mdProxy = NULL;
    if (!isLocalGuidCheck(self, guid)) {
        mdProxy = (MdProxy_t*)value;
        ASSERT(mdProxy->base.kind == OCR_GUID_MD_PROXY);
        value = (void*)mdProxy->ptr;
        ASSERT(value);
    }

    void *val = NULL;
    if (kind == OCR_GUID_DB) {
        GP_HASHTABLE_DEL(derived->guidImplTable, key, &val);
        ASSERT((value == val) || (mdProxy == val));
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrDataBlock_t *db = (ocrDataBlock_t*)value;
        ASSERT(ocrGuidIsEq(guid, db->guid));
        ((ocrDataBlockFactory_t*)(pd->factories[pd->datablockFactoryIdx]))->fcts.reset(db);
    } else if (kind == OCR_GUID_EDT) {
        GP_HASHTABLE_DEL(derived->guidImplTable, key, &val);
        ASSERT((value == val) || (mdProxy == val));
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrTask_t *edt = (ocrTask_t*)value;
        ASSERT(ocrGuidIsEq(guid, edt->guid));
        ((ocrTaskFactory_t*)(pd->factories[pd->taskFactoryIdx]))->fcts.reset(edt);
    } else if (kind == OCR_GUID_EDT_TEMPLATE) {
        GP_HASHTABLE_DEL(derived->guidImplTable, key, &val);
        ASSERT((value == val) || (mdProxy == val));
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrTaskTemplate_t *tmpl = (ocrTaskTemplate_t*)value;
        ((ocrTaskTemplateFactory_t*)(pd->factories[pd->taskTemplateFactoryIdx]))->fcts.reset(tmpl);
    } else if (kind & OCR_GUID_EVENT) {
        GP_HASHTABLE_DEL(derived->guidImplTable, key, &val);
        ASSERT((value == val) || (mdProxy == val));
        ocrObject_t * ocrObj = (ocrObject_t*)value;
        ASSERT(ocrObj->kind == kind);
        ocrEvent_t *evt = (ocrEvent_t*)value;
        ASSERT(ocrGuidIsEq(guid, evt->guid));
        ((ocrEventFactory_t*)(pd->factories[pd->eventFactoryIdx]))->commonFcts.reset(evt);
    } else if (kind == OCR_GUID_AFFINITY) {
        GP_HASHTABLE_DEL(derived->guidImplTable, key, &val);
        ASSERT(value == val);
        pd->fcts.pdFree(pd, value);
    } else {
        switch(kind) {
        case OCR_GUID_ALLOCATOR:
        case OCR_GUID_POLICY:
        case OCR_GUID_WORKER:
        case OCR_GUID_MEMTARGET:
        case OCR_GUID_COMPTARGET:
        case OCR_GUID_SCHEDULER:
        case OCR_GUID_WORKPILE:
        case OCR_GUID_COMM:
        case OCR_GUID_SCHEDULER_OBJECT:
        case OCR_GUID_SCHEDULER_HEURISTIC:
        case OCR_GUID_GUIDMAP:
            return;
        default:
            {
                DPRINTF(DEBUG_LVL_WARN, "Unknown guid kind found\n");
                ASSERT(0);
                return;
            }
        }
    }

    if (mdProxy != NULL) {
        destructMdProxy(mdProxy);
    }
}

u8 resetGuidProviderCounted(ocrGuidProvider_t* self) {
    ocrGuidProviderCountedMap_t * derived = (ocrGuidProviderCountedMap_t *) self;
    GP_HASHTABLE_ITERATE(derived->guidImplTable, resetProgramState, self);
    return 0;
}

#endif

static ocrGuidProvider_t* newGuidProviderCountedMap(ocrGuidProviderFactory_t *factory,
        ocrParamList_t *perInstance) {
    ocrGuidProvider_t *base = (ocrGuidProvider_t*) runtimeChunkAlloc(sizeof(ocrGuidProviderCountedMap_t), PERSISTENT_CHUNK);
    ocrGuidProviderCountedMap_t *rself = (ocrGuidProviderCountedMap_t*)base;
    base->fcts = factory->providerFcts;
    base->pd = NULL;
    base->id = factory->factoryId;
#ifdef GUID_PROVIDER_WID_INGUID
    {
        u32 i = 0;
        for(; i < (MAX_VAL(LOCWID)*GUID_WID_CACHE_SIZE); ++i) {
            rself->guidCounters[0] = 0;
        }
    }
#else
    rself->guidCounter = 0;
#endif
#ifdef ENABLE_RESILIENCY
    rself->objectsCounted = 0;
    rself->objectsSerialized = 0;
#endif
    return base;
}

/****************************************************/
/* OCR GUID PROVIDER COUNTED MAP FACTORY            */
/****************************************************/

static void destructGuidProviderFactoryCountedMap(ocrGuidProviderFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrGuidProviderFactory_t *newGuidProviderFactoryCountedMap(ocrParamList_t *typeArg, u32 factoryId) {
    ocrGuidProviderFactory_t *base = (ocrGuidProviderFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrGuidProviderFactoryCountedMap_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newGuidProviderCountedMap;
    base->destruct = &destructGuidProviderFactoryCountedMap;
    base->factoryId = factoryId;

    base->providerFcts.destruct = FUNC_ADDR(void (*)(ocrGuidProvider_t*), countedMapDestruct);
    base->providerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), countedMapSwitchRunlevel);
    base->providerFcts.guidReserve = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t*, u64*, u64, ocrGuidKind, u32), countedMapGuidReserve);
    base->providerFcts.guidUnreserve = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64, u64), countedMapGuidUnreserve);
    base->providerFcts.getGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t*, u64, ocrGuidKind, ocrLocation_t, u32 properties), countedMapGetGuid);
    base->providerFcts.createGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrFatGuid_t*, u64, ocrGuidKind, ocrLocation_t, u32), countedMapCreateGuid);
    base->providerFcts.getVal = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64*, ocrGuidKind*, u32, MdProxy_t**), countedMapGetVal);
    base->providerFcts.getKind = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, ocrGuidKind*), mapGetKind);
    base->providerFcts.getLocation = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, ocrLocation_t*), mapGetLocation);
    base->providerFcts.registerGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64), countedMapRegisterGuid);
    base->providerFcts.unregisterGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64**), countedMapUnregisterGuid);
    base->providerFcts.releaseGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrFatGuid_t, bool), countedMapReleaseGuid);
#ifdef ENABLE_RESILIENCY
    base->providerFcts.getSerializationSize = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, u64*), getSerializationSizeGuidProviderCounted);
    base->providerFcts.serialize = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, u8*), serializeGuidProviderCounted);
    base->providerFcts.deserialize = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, u8*), deserializeGuidProviderCounted);
    base->providerFcts.reset = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*), resetGuidProviderCounted);
    base->providerFcts.fixup = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*), fixupGuidProviderCounted);
#endif
    return base;
}

#endif /* ENABLE_GUID_COUNTED_MAP */
