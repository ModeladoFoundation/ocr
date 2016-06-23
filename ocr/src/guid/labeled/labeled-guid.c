/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_GUID_LABELED

#include "debug.h"
#include "guid/labeled/labeled-guid.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"

#define DEBUG_TYPE GUID

#define ENABLE_GUID_BITMAP_BASED 1
#include "guid/guid-bitmap.h"

// Default hashtable's number of buckets
//PERF: This parameter heavily impacts the GUID provider scalability !
#ifndef GUID_PROVIDER_NB_BUCKETS
#define GUID_PROVIDER_NB_BUCKETS 10000
#endif

// Guid is composed of : (1/0 LOCID KIND COUNTER)
#define GUID_RESERVED_SIZE  (1)
#define GUID_COUNTER_SIZE   (GUID_BIT_COUNT-(GUID_RESERVED_SIZE+GUID_LOCID_SIZE+GUID_KIND_SIZE))

// Start indices for each field
#define SIDX_RESERVED    (GUID_BIT_COUNT)
#define SIDX_LOCID       (SIDX_RESERVED-GUID_RESERVED_SIZE)
#define SIDX_LOCHOME     (SIDX_LOCID)
#define SIDX_LOCALLOC    (SIDX_LOCID-GUID_LOCHOME_SIZE)
#define SIDX_LOCWID      (SIDX_LOCALLOC-GUID_LOCALLOC_SIZE)
#define SIDX_KIND        (SIDX_LOCID-GUID_LOCID_SIZE)
#define SIDX_COUNTER     (SIDX_KIND-GUID_KIND_SIZE)

//TODO cleanup when we rewrite reserve
#ifdef GUID_PROVIDER_WID_INGUID
#define KIND_LOCATION (SIZE(LOCWID))
#define LOCID_LOCATION (KIND_LOCATION+SIZE(KIND))
#else
#define KIND_LOCATION (0)
#define LOCID_LOCATION (SIZE(KIND))
#endif

// See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
#define IS_RESERVED_GUID(guidVal) ((guidVal.guid & 0x8000000000000000ULL) != 0ULL)
#elif GUID_BIT_COUNT == 128
#define IS_RESERVED_GUID(guidVal) ((guidVal.lower & 0x8000000000000000ULL) != 0ULL)
#endif

#ifdef GUID_PROVIDER_CUSTOM_MAP
// Set -DGUID_PROVIDER_CUSTOM_MAP and put other #ifdef for alternate implementation here
#else
#define GP_RESOLVE_HASHTABLE(hashtable, key) hashtable
#define GP_HASHTABLE_CREATE_MODULO newHashtableBucketLocked
#define GP_HASHTABLE_DESTRUCT(hashtable, key, entryDealloc, deallocParam) destructHashtableBucketLocked(hashtable, entryDealloc, deallocParam)
#define GP_HASHTABLE_GET(hashtable, key) hashtableConcBucketLockedGet(GP_RESOLVE_HASHTABLE(hashtable,key), key)
#define GP_HASHTABLE_PUT(hashtable, key, value) hashtableConcBucketLockedPut(GP_RESOLVE_HASHTABLE(hashtable,key), key, value)
#define GP_HASHTABLE_DEL(hashtable, key, valueBack) hashtableConcBucketLockedRemove(GP_RESOLVE_HASHTABLE(hashtable,key), key, valueBack)
#endif

#ifdef GUID_PROVIDER_WID_INGUID
#define CACHE_SIZE 8
static u64 guidCounters[(((u64)1)<<GUID_WID_SIZE)*CACHE_SIZE];
#else
// GUID 'id' counter, atomically incr when a new GUID is requested
static u64 guidCounter = 0;
#endif
// Counter for the reserved part of the GUIDs
static u64 guidReservedCounter = 0;

// Utils for bitmap-based GUID implementations
#include "guid/guid-bitmap-based.c"


#ifdef GUID_PROVIDER_DESTRUCT_CHECK

void labeledGuidHashmapEntryDestructChecker(void * key, void * value, void * deallocParam) {
    ocrGuid_t guid;
#if GUID_BIT_COUNT == 64
    guid.guid = (u64) key;
#elif GUID_BIT_COUNT == 128
    guid.upper = 0x0;
    guid.lower = (u64) key;
#endif
    ((u32*)deallocParam)[getKindFromGuid(guid)]++;
#ifdef GUID_PROVIDER_DESTRUCT_CHECK_VERBOSE
    DPRINTF(DEBUG_LVL_WARN, "Remnant GUID "GUIDF" of kind %s still registered on GUID provider\n", GUIDA(guid), ocrGuidKindToChar(getKindFromGuid(guid)));
#endif
}
#endif

void labeledGuidDestruct(ocrGuidProvider_t* self) {
    runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

u8 labeledGuidSwitchRunlevel(ocrGuidProvider_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
#ifdef GUID_PROVIDER_WID_INGUID
        u32 i = 0, ub = PD->workerCount;
        u64 max = MAX_VAL(COUNTER);
        u64 incr = (max/ub);
        while (i < ub) {
            // Initialize to 'i' to distribute the count over the buckets. Helps with scalability.
            // This is knowing we use a modulo hash but is not hurting generally speaking...
            guidCounters[i*CACHE_SIZE] = incr*i;
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
            deallocFct entryDeallocator = labeledGuidHashmapEntryDestructChecker;
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
            GP_HASHTABLE_DESTRUCT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, NULL, entryDeallocator, deallocParam);
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
        ASSERT(self->pd == PD);
        if((properties & RL_BRING_UP) && RL_IS_LAST_PHASE_UP(PD, RL_GUID_OK, phase)) {
            // //TODO clean-up when isLocalGuid is used. Just to keep compiler happy with unused
            // bool res __attribute__((unused)) = isLocalGuid(self, NULL_GUID);
            //Initialize the map now that we have an assigned policy domain
            ocrGuidProviderLabeled_t * derived = (ocrGuidProviderLabeled_t *) self;
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

u8 labeledGuidReserve(ocrGuidProvider_t *self, ocrGuid_t *startGuid, u64* skipGuid,
                      u64 numberGuids, ocrGuidKind guidType) {
    // We just return a range using our "header" (location, etc) just like for
    // generateNextGuid
    // ocrGuidType_t and ocrGuidKind should be the same (there are more GuidKind but
    // the ones that are the same should match)
    u64 locId = (u64) locationToLocId(self->pd->myLocation);
    u64 locIdShifted = locId << LOCID_LOCATION;
    u64 kindShifted = guidType << KIND_LOCATION;
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    (*(startGuid)).guid = ((1 << (GUID_LOCID_SIZE + LOCID_LOCATION)) | locIdShifted | kindShifted) <<
        GUID_COUNTER_SIZE;
#elif GUID_BIT_COUNT == 128
    (*(startGuid)).lower = ((1 << (GUID_LOCID_SIZE + LOCID_LOCATION)) | locIdShifted | kindShifted) <<
        GUID_COUNTER_SIZE;
    (*(startGuid)).upper = 0x0;
#endif

    *skipGuid = 1; // Each GUID will just increment by 1
    u64 firstCount = hal_xadd64(&guidReservedCounter, numberGuids);
    ASSERT((firstCount  + numberGuids) < (MAX_VAL(COUNTER)));

    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    (*(startGuid)).guid |= firstCount;
#elif GUID_BIT_COUNT == 128
    (*(startGuid)).lower |= firstCount;
#endif

    DPRINTF(DEBUG_LVL_VVERB, "LabeledGUID reserved a range for %"PRIu64" GUIDs starting at "GUIDF"\n",
            numberGuids, GUIDA(*startGuid));
    return 0;
}

u8 labeledGuidUnreserve(ocrGuidProvider_t *self, ocrGuid_t startGuid, u64 skipGuid,
                        u64 numberGuids) {
    // We do not do anything (we don't reclaim right now)
    return 0;
}

/**
 * @brief Generate a guid for 'val' by increasing the guid counter.
 */
u8 labeledGuidGetGuid(ocrGuidProvider_t* self, ocrGuid_t* guid, u64 val, ocrGuidKind kind, ocrLocation_t targetLoc, u32 properties) {
    // Here no need to allocate
    u64 newGuid = generateNextGuid(self, kind, targetLoc, 1);
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: insert into hash table 0x%"PRIx64" -> 0x%"PRIx64"\n", newGuid, val);

    if (properties & GUID_PROP_TORECORD) {
        GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) newGuid, (void *) val);
    }
    // See BUG #928 on GUID issues

    GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) newGuid, (void *) val);
#if GUID_BIT_COUNT == 64
    (*(guid)).guid =  newGuid;
#elif GUID_BIT_COUNT == 128
    (*(guid)).lower = newGuid;
    (*(guid)).upper = 0x0;
#else
#error Unknown GUID type
#endif
    return 0;
}

u8 labeledGuidCreateGuid(ocrGuidProvider_t* self, ocrFatGuid_t *fguid, u64 size, ocrGuidKind kind, ocrLocation_t targetLoc, u32 properties) {
    if(properties & GUID_PROP_IS_LABELED) {
        // We need to use the GUID provided; make sure it is non null and reserved
        ASSERT((!(ocrGuidIsNull(fguid->guid))) && (IS_RESERVED_GUID(fguid->guid)));

        // We need to fix this: ie: return a code saying we can't do the reservation
        // Ideally, we would either forward to the responsible party or return something
        // so the PD knows what to do. This is going to take a lot more infrastructure
        // change so we'll punt for now
        // Related to BUG #535 and to BUG #536
        ASSERT(extractLocIdFromGuid(fguid->guid) == locationToLocId(self->pd->myLocation));

        // Other sanity check
        ASSERT(getKindFromGuid(fguid->guid) == kind); // Kind properly encoded
        // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
        ASSERT((RSHIFT(COUNTER, fguid->guid.guid)) < guidReservedCounter); // Range actually reserved
#elif GUID_BIT_COUNT == 128
        ASSERT((RSHIFT(COUNTER, fguid->guid.lower)) < guidReservedCounter); // Range actually reserved
#endif
    }
    ocrPolicyDomain_t *policy = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&policy, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_ALLOC
    msg.type = PD_MSG_MEM_ALLOC | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(size) = size; // allocate 'size' payload as metadata
    PD_MSG_FIELD_I(properties) = 0;
    PD_MSG_FIELD_I(type) = GUID_MEMTYPE;

    RESULT_PROPAGATE(policy->fcts.processMessage (policy, &msg, true));
    void * ptr = (void *)PD_MSG_FIELD_O(ptr);

    // Update the fat GUID's metaDataPtr
    fguid->metaDataPtr = ptr;
    ASSERT(ptr);
#undef PD_TYPE
    (*(ocrGuid_t*)ptr) = NULL_GUID; // The first field is always the GUID, either directly as ocrGuid_t or a ocrFatGuid_t
                                    // This is used to determine if a GUID metadata is "ready". See bug #627
    hal_fence(); // Make sure the ptr update is visible before we update the hash table
    if(properties & GUID_PROP_IS_LABELED) {
        // Bug #865: Warning if ordering is important, first GUID_PROP_CHECK then GUID_PROP_BLOCK
        // because we want the first branch to intercept (GUID_PROP_CHECK | GUID_PROP_BLOCK)
        if((properties & GUID_PROP_CHECK) == GUID_PROP_CHECK) {
            // We need to actually check things
            DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: try insert into hash table "GUIDF" -> %p\n", GUIDA(fguid->guid), ptr);
            // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
            void *value = hashtableConcBucketLockedTryPut(
                ((ocrGuidProviderLabeled_t*)self)->guidImplTable,
                (void*)(fguid->guid.guid), ptr);
#elif GUID_BIT_COUNT == 128
            void *value = hashtableConcBucketLockedTryPut(
                ((ocrGuidProviderLabeled_t*)self)->guidImplTable,
                (void*)(fguid->guid.lower), ptr);
#endif
            if(value != ptr) {
                DPRINTF(DEBUG_LVL_VVERB, "LabeledGUID: FAILED to insert (got %p instead of %p)\n",
                        value, ptr);
                // Fail; already exists
                fguid->metaDataPtr = value; // Do we need to return this?
                // We now need to free the memory we allocated
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_TYPE PD_MSG_MEM_UNALLOC
                msg.type = PD_MSG_MEM_UNALLOC | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(allocatingPD.guid) = NULL_GUID;
                PD_MSG_FIELD_I(allocator.guid) = NULL_GUID;
                PD_MSG_FIELD_I(ptr) = ptr;
                PD_MSG_FIELD_I(type) = GUID_MEMTYPE;
                PD_MSG_FIELD_I(properties) = 0;
                RESULT_PROPAGATE(policy->fcts.processMessage(policy, &msg, true));
#undef PD_TYPE
                // Bug #627: We do not return OCR_EGUIDEXISTS until the GUID is valid. We test this
                // by looking at the first field of ptr and waiting for it to be the GUID value (meaning the
                // object has been initialized

                // Bug #865: When both GUID_PROP_BLOCK and GUID_PROP_CHECK are set it indicates the caller
                // wants to try to create the object but should retry asynchronously. In that case
                // we can't enter the blocking loop as the value pointer may become invalid if
                // there's an interleaved destroy call on the GUID.
                if ((properties & GUID_PROP_BLOCK) != GUID_PROP_BLOCK) {
                // See BUG #928 on GUID issues
                void * adjustedPtr = (((ocrObject_t *)(value))+1);
#if GUID_BIT_COUNT == 64
                    while((*(volatile u64*)adjustedPtr) != fguid->guid.guid);
#elif GUID_BIT_COUNT == 128
                    while((*(volatile u64*)adjustedPtr) != fguid->guid.lower);
#endif
                }
                hal_fence(); // May be overkill but there is a race that I don't get
                return OCR_EGUIDEXISTS;
            }
        } else if((properties & GUID_PROP_BLOCK) == GUID_PROP_BLOCK) {
            void* value = NULL;
            DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: force insert into hash table "GUIDF" -> %p\n", GUIDA(fguid->guid), ptr);
            do {

// See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
                value = hashtableConcBucketLockedTryPut(
                    ((ocrGuidProviderLabeled_t*)self)->guidImplTable,
                    (void*)(fguid->guid.guid), ptr);
#elif GUID_BIT_COUNT == 128
                value = hashtableConcBucketLockedTryPut(
                    ((ocrGuidProviderLabeled_t*)self)->guidImplTable,
                    (void*)(fguid->guid.lower), ptr);
#endif

            } while(value != ptr);
        } else {
            // "Trust me" mode. We insert into the hashtable
            DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: trust insert into hash table "GUIDF" -> %p\n", GUIDA(fguid->guid), ptr);

            // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
            GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t*)self)->guidImplTable,
                             (void*)(fguid->guid.guid), ptr);
#elif GUID_BIT_COUNT == 128
            GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t*)self)->guidImplTable,
                             (void*)(fguid->guid.lower), ptr);
#else
#error Unknown GUID type
#endif
        }
    } else { // Not labeled
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
                GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid, (void *) ptr);
            }
        } else {
            labeledGuidGetGuid(self, &(fguid->guid), (u64) (fguid->metaDataPtr), kind, targetLoc, GUID_PROP_TORECORD);
            DPRINTF(DEBUG_LVL_VVERB, "Generating GUID "GUIDF"\n", GUIDA(fguid->guid));
        }
    }
#undef PD_MSG
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: create GUID: "GUIDF" -> 0x%p\n", GUIDA(fguid->guid), fguid->metaDataPtr);
    return 0;
}

/**
 * @brief Returns the value associated with a guid and its kind if requested.
 */
u8 labeledGuidGetVal(ocrGuidProvider_t* self, ocrGuid_t guid, u64* val, ocrGuidKind* kind, u32 mode, MdProxy_t ** proxy) {
    ocrGuidProviderLabeled_t * dself = (ocrGuidProviderLabeled_t *) self;
    if (IS_RESERVED_GUID(guid)) {
        // Current limitations for labeled GUID
        // Only affinity and templates for now
        ASSERT(mode != MD_FETCH);
    }
    // See BUG #928 on GUID issues
    #if GUID_BIT_COUNT == 64
        void * rguid = (void *) guid.guid;
    #elif GUID_BIT_COUNT == 128
        void * rguid = (void *) guid.lower;
    #else
    #error Unknown type of GUID
    #endif
    if (isLocalGuid(self, guid)) {
        *val = (u64) GP_HASHTABLE_GET(dself->guidImplTable, rguid);
        DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: got val for GUID "GUIDF": 0x%"PRIx64"\n", GUIDA(guid), *val);
        if ((*val != (u64)NULL) && IS_RESERVED_GUID(guid)) {
            // Bug #627: We do not return until the GUID is valid. We test this
            // by looking at the first field of ptr and waiting for it to be the GUID value (meaning the
            // object has been initialized
            volatile u64 * spinVal;
            if (isLocalGuid(self, guid)) {
                spinVal = val;
            } else {
                MdProxy_t * sproxy = (MdProxy_t *) val;
                spinVal = (volatile u64*) &sproxy->ptr;
            }
            void * adjustedPtr = (((ocrObject_t *)(*spinVal))+1);
            // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
            while((*(volatile u64*)(adjustedPtr)) != guid.guid);
#elif GUID_BIT_COUNT == 128
            while((*(volatile u64*)(adjustedPtr)) != guid.lower);
#endif
            hal_fence(); // May be overkill but there is a race that I don't get
        } // else val is not set and fall-through
    } else {
        // The GUID is remote, check if we have a local representent or need to fetch
        *val = 0; // Important for return code to be set properly
        MdProxy_t * mdProxy = (MdProxy_t *) GP_HASHTABLE_GET(dself->guidImplTable, rguid);
        if (mdProxy == NULL) {
            if (mode == MD_LOCAL) {
                if (kind) {
                    *kind = getKindFromGuid(guid);
                }
                return 0;
            }
            // Implementation limitation. For now DB relies on the proxy mecanism in hc-dist-policy
            ASSERT(getKindFromGuid(guid) != OCR_GUID_DB);

            // Use 'proxy' being set or not to determine if the caller wanted
            // to do the fetch if absent or is just querying for presence.
            if (proxy == NULL) { // MD_LOCAL is just to check and not fetch
                ASSERT(kind == NULL); // Shouldn't happen in current impl
                return 0;
            }
            //For labeled, currently delegating directly to the remote location that owns the reserved GUID
            ASSERT(!IS_RESERVED_GUID(guid) && "Labeled Limitation");
            ocrPolicyDomain_t * pd = self->pd;
            mdProxy = (MdProxy_t *) pd->fcts.pdMalloc(pd, sizeof(MdProxy_t));
            mdProxy->queueHead = (void *) REG_OPEN; // sentinel value
            mdProxy->ptr = 0;
            hal_fence(); // I think the lock in try put should make the writes visible
            MdProxy_t * oldMdProxy = (MdProxy_t *) hashtableConcBucketLockedTryPut(dself->guidImplTable, rguid, mdProxy);
            if (oldMdProxy == mdProxy) { // won
                // TODO two options:
                // 1- Issue the MD cloning here and link that operation's
                //    completion to the mdProxy
                PD_MSG_STACK(msgClone);
                getCurrentEnv(NULL, NULL, NULL, &msgClone);
#define PD_MSG (&msgClone)
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
                msgClone.type = PD_MSG_GUID_METADATA_CLONE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = guid;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                PD_MSG_FIELD_I(type) = MD_CLONE;
                PD_MSG_FIELD_I(dstLocation) = pd->myLocation;
                u8 returnCode = pd->fcts.processMessage(pd, &msgClone, false);
                ASSERT(returnCode == OCR_EPEND);
                // Warning: after this call we're potentially concurrent with the MD being registered on the GP
#undef PD_MSG
#undef PD_TYPE
                // 2- Return an error code along with the oldMdProxy event
                //    The caller will be responsible for calling MD cloning
                //    and setup the link. Note there's a race here when the
                //    md is resolve concurrently. It sounds it would be better
                //    to go through functions.
            } else {
                // lost competition, 2 cases:
                // 1) The MD is available (it's concurrent to this work thread)
                // 2) The MD is still being fetch
                pd->fcts.pdFree(pd, mdProxy); // we failed, free our proxy.
                // Read the content of the proxy anyhow
                // TODO: Open a feature bug for that.
                // It is safe to read the ptr because there cannot be a racing remove
                // operation on that entry. For now we made the choice that we do not
                // eagerly reclaim entries to evict ununsed GUID. The only time a GUID
                // is removed from the map is when the OCR object it represent is being
                // destroyed (hence there should be no concurrent read at that time).
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
        } else {
            //For labeled, currently delegating directly to the remote location that owns the reserved GUID
            ASSERT(!IS_RESERVED_GUID(guid) && "Labeled Limitation");
            *val = ((getKindFromGuid(guid) == OCR_GUID_DB) ? ((u64) mdProxy) : ((u64) mdProxy->ptr));
        }
        if (mode == MD_FETCH) {
            ASSERT(proxy != NULL);
            *proxy = mdProxy;
        }
    }
    if (kind) {
        *kind = getKindFromGuid(guid);
    }

    return (*val) ? 0 : OCR_EPEND;
}

//TODO-MD-MT These should become micro-tasks
extern ocrGuid_t processRequestEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
extern u8 createProcessRequestEdtDistPolicy(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid, u64 * paramv);

/**
 * @brief Associate an already existing GUID to a value.
 * This is useful in the context of distributed-OCR to register
 * a local metadata represent for a foreign GUID.
 */
u8 labeledGuidRegisterGuid(ocrGuidProvider_t* self, ocrGuid_t guid, u64 val) {
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: register GUID "GUIDF" -> 0x%"PRIx64"\n", GUIDA(guid), val);
    ocrGuidProviderLabeled_t * dself = (ocrGuidProviderLabeled_t *) self;
#if GUID_BIT_COUNT == 64
    void * rguid = (void *) guid.guid;
#elif GUID_BIT_COUNT == 128
    void * rguid = (void *) guid.lower;
#else
#error Unknown type of GUID
#endif
    if (isLocalGuid(self, guid)) {
        // See BUG #928 on GUID issues
        GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) rguid, (void *) val);
    } else {
        // Datablocks not yet supported as part of MdProxy_t - handled at the dist-PD level
        if (getKindFromGuid(guid) == OCR_GUID_DB) {
            // See BUG #928 on GUID issues
            GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) rguid, (void *) val);
            return 0;
        }
        MdProxy_t * mdProxy = (MdProxy_t *) hashtableConcBucketLockedGet(dself->guidImplTable, (void *) rguid);
        // Must have setup a mdProxy before being able to register.
        ASSERT(mdProxy != NULL);
        mdProxy->ptr = val;
        hal_fence(); // This may be redundant with the CAS
        u64 newValue = (u64) REG_CLOSED;
        u64 curValue = 0;
        u64 oldValue = 0;
        do {
            MdProxyNode_t * head = mdProxy->queueHead;
            ASSERT(head != REG_CLOSED);
            curValue = (u64) head;
            oldValue = hal_cmpswap64((u64*) &(mdProxy->queueHead), curValue, newValue);
        } while(oldValue != curValue);
        ocrGuid_t processRequestTemplateGuid;
        ocrEdtTemplateCreate(&processRequestTemplateGuid, &processRequestEdt, 1, 0);
        MdProxyNode_t * queueHead = (MdProxyNode_t *) oldValue;
        DPRINTF(DEBUG_LVL_VVERB,"About to process stored clone requests for GUID "GUIDF" queueHead=%p)\n", GUIDA(guid), queueHead);
        while (queueHead != ((void*) REG_OPEN)) { // sentinel value
            DPRINTF(DEBUG_LVL_VVERB,"Processing stored clone requests for GUID "GUIDF"\n", GUIDA(guid));
            u64 paramv = (u64) queueHead->msg;
            ocrPolicyDomain_t * pd = self->pd;
            createProcessRequestEdtDistPolicy(pd, processRequestTemplateGuid, &paramv);
            MdProxyNode_t * currNode = queueHead;
            queueHead = queueHead->next;
            pd->fcts.pdFree(pd, currNode);
        }
        ocrEdtTemplateDestroy(processRequestTemplateGuid);
    }
    return 0;
}

/**
 * @brief Remove an already existing GUID and its associated value from the provider
 */
u8 labeledGuidUnregisterGuid(ocrGuidProvider_t* self, ocrGuid_t guid, u64 ** val) {
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    GP_HASHTABLE_DEL(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid.guid, (void **) val);
#elif GUID_BIT_COUNT == 128
    GP_HASHTABLE_DEL(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid.lower, (void **) val);
#else
#error Unknown GUID type
#endif
    return 0;
}

u8 labeledGuidReleaseGuid(ocrGuidProvider_t *self, ocrFatGuid_t fatGuid, bool releaseVal) {
    // We can only destroy GUIDs that we created
    ASSERT(extractLocIdFromGuid(fatGuid.guid) == locationToLocId(self->pd->myLocation));
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: release GUID "GUIDF"\n", GUIDA(fatGuid.guid));
    ocrGuid_t guid = fatGuid.guid;
    // We *first* remove the GUID from the hashtable otherwise the following race
    // could occur:
    //   - free the metadata
    //   - another thread trying to create the same GUID creates the metadata at the *same* address
    //   - the other thread tries to insert, this succeeds immediately since it's
    //     the same value for the pointer (already in the hashtable)
    //   - this function removes the value from the hashtable
    //   => the creator thinks all is swell but the data was actually *removed*
    ocrGuidProviderLabeled_t * derived = (ocrGuidProviderLabeled_t *) self;
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    RESULT_ASSERT(GP_HASHTABLE_DEL(derived->guidImplTable, (void *)guid.guid, NULL), ==, true);
#elif GUID_BIT_COUNT == 128
    RESULT_ASSERT(GP_HASHTABLE_DEL(derived->guidImplTable, (void *)guid.lower, NULL), ==, true);
#else
#error Unknown GUID type
#endif
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
        RESULT_PROPAGATE(policy->fcts.processMessage (policy, &msg, true));
#undef PD_MSG
#undef PD_TYPE
    }
    return 0;
}

static ocrGuidProvider_t* newGuidProviderLabeled(ocrGuidProviderFactory_t *factory,
                                                 ocrParamList_t *perInstance) {
    ocrGuidProvider_t *base = (ocrGuidProvider_t*) runtimeChunkAlloc(sizeof(ocrGuidProviderLabeled_t), PERSISTENT_CHUNK);
    base->fcts = factory->providerFcts;
    base->pd = NULL;
    base->id = factory->factoryId;
    return base;
}

/****************************************************/
/* OCR GUID PROVIDER LABELED FACTORY                */
/****************************************************/

static void destructGuidProviderFactoryLabeled(ocrGuidProviderFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrGuidProviderFactory_t *newGuidProviderFactoryLabeled(ocrParamList_t *typeArg, u32 factoryId) {
    ocrGuidProviderFactory_t *base = (ocrGuidProviderFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrGuidProviderFactoryLabeled_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newGuidProviderLabeled;
    base->destruct = &destructGuidProviderFactoryLabeled;
    base->factoryId = factoryId;
    base->providerFcts.destruct = FUNC_ADDR(void (*)(ocrGuidProvider_t*), labeledGuidDestruct);
    base->providerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64),
        labeledGuidSwitchRunlevel);
    base->providerFcts.guidReserve = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t*, u64*, u64, ocrGuidKind), labeledGuidReserve);
    base->providerFcts.guidUnreserve = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64, u64), labeledGuidUnreserve);
    base->providerFcts.getGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t*, u64, ocrGuidKind, ocrLocation_t, u32), labeledGuidGetGuid);
    base->providerFcts.createGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrFatGuid_t*, u64, ocrGuidKind, ocrLocation_t, u32), labeledGuidCreateGuid);
    base->providerFcts.getVal = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64*, ocrGuidKind*, u32, MdProxy_t**), labeledGuidGetVal);
    base->providerFcts.getKind = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, ocrGuidKind*), mapGetKind);
    base->providerFcts.getLocation = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, ocrLocation_t*), mapGetLocation);
    base->providerFcts.registerGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64), labeledGuidRegisterGuid);
    base->providerFcts.unregisterGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64**), labeledGuidUnregisterGuid);
    base->providerFcts.releaseGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrFatGuid_t, bool), labeledGuidReleaseGuid);

    return base;
}

#endif /* ENABLE_GUID_LABELED */
