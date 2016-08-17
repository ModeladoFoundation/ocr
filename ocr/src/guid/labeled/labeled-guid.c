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

// Default hashtable's number of buckets
//PERF: This parameter heavily impacts the GUID provider scalability !
#ifndef GUID_PROVIDER_NB_BUCKETS
#define GUID_PROVIDER_NB_BUCKETS 10000
#endif

// Guid is composed of : (1/0 LOCID KIND COUNTER)
// The 1 at the top is if this is a "reserved" GUID (for checking purposes)
#define GUID_BIT_SIZE 64
#ifndef GUID_PROVIDER_LOCID_SIZE
#define GUID_LOCID_SIZE 7 // Warning! 2^7 locId max, bump that up for more.
#else
#define GUID_LOCID_SIZE GUID_PROVIDER_LOCID_SIZE
#endif
#define GUID_KIND_SIZE 5 // Warning! check ocrGuidKind struct definition for correct size

#ifdef GUID_PROVIDER_WID_INGUID
#ifndef GUID_PROVIDER_WID_SIZE
#define GUID_WID_SIZE 4
#else
#define GUID_WID_SIZE GUID_PROVIDER_WID_SIZE
#endif

#define GUID_COUNTER_SIZE (GUID_BIT_SIZE-(GUID_LOCID_SIZE+GUID_KIND_SIZE+GUID_WID_SIZE+1))
#define GUID_WID_MASK (((((u64)1)<<GUID_WID_SIZE)-1)<<(GUID_COUNTER_SIZE))
#define GUID_COUNTER_MASK ((((u64)1)<<(GUID_COUNTER_SIZE))-1)
#define GUID_LOCID_MASK (((((u64)1)<<GUID_LOCID_SIZE)-1)<<(GUID_COUNTER_SIZE+GUID_WID_SIZE+GUID_KIND_SIZE))
#define GUID_LOCID_SHIFT_RIGHT (GUID_BIT_SIZE-GUID_LOCID_SIZE-1)
#define GUID_KIND_MASK (((((u64)1)<<GUID_KIND_SIZE)-1)<<(GUID_COUNTER_SIZE+GUID_WID_SIZE))
#define GUID_KIND_SHIFT_RIGHT (GUID_LOCID_SHIFT_RIGHT-GUID_KIND_SIZE)
#define GUID_WID_SHIFT_RIGHT (GUID_KIND_SHIFT_RIGHT-GUID_WID_SIZE)
#define WID_LOCATION (0)
#define KIND_LOCATION (GUID_WID_SIZE)
#define LOCID_LOCATION (KIND_LOCATION+GUID_KIND_SIZE)
#else
#define GUID_COUNTER_SIZE (GUID_BIT_SIZE-(GUID_KIND_SIZE+GUID_LOCID_SIZE+1))
#define GUID_COUNTER_MASK ((((u64)1)<<(GUID_COUNTER_SIZE))-1)
#define GUID_LOCID_MASK (((((u64)1)<<GUID_LOCID_SIZE)-1)<<(GUID_COUNTER_SIZE+GUID_KIND_SIZE))
#define GUID_LOCID_SHIFT_RIGHT (GUID_BIT_SIZE-GUID_LOCID_SIZE-1)
#define GUID_KIND_MASK (((((u64)1)<<GUID_KIND_SIZE)-1)<<GUID_COUNTER_SIZE)
#define GUID_KIND_SHIFT_RIGHT (GUID_LOCID_SHIFT_RIGHT-GUID_KIND_SIZE)
#define KIND_LOCATION (0)
#define LOCID_LOCATION (GUID_KIND_SIZE)
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

#ifdef GUID_PROVIDER_DESTRUCT_CHECK
// Fwd declaration
static ocrGuidKind getKindFromGuid(ocrGuid_t guid);

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

static u32 hashGuidCounterModulo(void * ptr, u32 nbBuckets) {
    u64 guid = (u64) ptr;
    return ((guid & GUID_COUNTER_MASK) % nbBuckets);
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
            ocrGuidProviderLabeled_t *rself = (ocrGuidProviderLabeled_t*)self;
            u32 i = 0, ub = PD->workerCount;
            u64 max = ((u64)1<<GUID_COUNTER_SIZE);
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
            //Initialize the map now that we have an assigned policy domain
            ocrGuidProviderLabeled_t * derived = (ocrGuidProviderLabeled_t *) self;
            derived->guidImplTable = GP_HASHTABLE_CREATE_MODULO(PD, GUID_PROVIDER_NB_BUCKETS, hashGuidCounterModulo);
#ifdef GUID_PROVIDER_WID_INGUID
            ASSERT(((PD->workerCount-1) < ((u64)1 << GUID_WID_SIZE)) && "GUID worker count overflows");
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

/**
 * @brief Utility function to extract a kind from a GUID.
 */
static ocrGuidKind getKindFromGuid(ocrGuid_t guid) {
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    return (ocrGuidKind) ((guid.guid & GUID_KIND_MASK) >> GUID_KIND_SHIFT_RIGHT);
#elif GUID_BIT_COUNT == 128
    return (ocrGuidKind) ((guid.lower & GUID_KIND_MASK) >> GUID_KIND_SHIFT_RIGHT);
#endif
}

/**
 * @brief Utility function to extract a kind from a GUID.
 */
static u64 extractLocIdFromGuid(ocrGuid_t guid) {
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    return (u64) ((guid.guid & GUID_LOCID_MASK) >> GUID_LOCID_SHIFT_RIGHT);
#elif GUID_BIT_COUNT == 128
    return (u64) ((guid.lower & GUID_LOCID_MASK) >> GUID_LOCID_SHIFT_RIGHT);
#endif
}

static ocrLocation_t locIdtoLocation(u64 locId) {
    //BUG #605 Locations spec: We assume there will be a mapping
    //between a location and an 'id' stored in the guid. For now identity
    // except for TG and TG-x86
#if defined(TG_X86_TARGET) || defined(TG_XE_TARGET) || defined(TG_CE_TARGET)
    return (ocrLocation_t)(1ULL<<60 | (locId << 16));
#else
    return (ocrLocation_t) (locId);
#endif
}

static u64 locationToLocId(ocrLocation_t location) {
    //BUG #605 Locations spec: We assume there will be a mapping
    //between a location and an 'id' stored in the guid. For now identity except
    // for TG and TG-x86
#if defined(TG_X86_TARGET) || defined(TG_XE_TARGET) || defined(TG_CE_TARGET)
    u64 locId = (((u64)(location)) & 0xFFFFFFFFULL) >> 16; // Strips out top 1 and bottom zeros
#else
    u64 locId = (u64)(location);
#endif
    // Make sure we're not overflowing location size
    ASSERT((locId < (1<<GUID_LOCID_SIZE)) && "GUID location ID overflows");
    return locId;
}

/**
 * @brief Utility function to generate a new GUID.
 */
static u64 generateNextGuid(ocrGuidProvider_t* self, ocrGuidKind kind) {
    ocrGuidProviderLabeled_t *rself = (ocrGuidProviderLabeled_t*)self;
    u64 locId = (u64) locationToLocId(self->pd->myLocation);
    DPRINTF(DEBUG_LVL_VERB, "Got location 0x%"PRIx64"\n", locId);
    u64 locIdShifted = locId << LOCID_LOCATION;
    u64 kindShifted = kind << KIND_LOCATION;
#ifdef GUID_PROVIDER_WID_INGUID
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // ASSERT((worker != NULL) && "GUID-Provider Cannot identify currently executing worker");
    // GUIDs are generated before the current worker is setup.
    u64 wid = (worker == NULL) ? 0 : worker->id;
    u64 widShifted = (wid << WID_LOCATION);
    u64 guid = (locIdShifted | kindShifted | widShifted) << GUID_COUNTER_SIZE;
    u64 newCount = rself->guidCounters[wid*GUID_WID_CACHE_SIZE]++;
#else
    u64 guid = (locIdShifted | kindShifted) << GUID_COUNTER_SIZE;
    DPRINTF(DEBUG_LVL_VERB, "Before increment of GUIDCounter. Addr @ %p\n", &(rself->guidCounter));
    u64 newCount = hal_xadd64(&(rself->guidCounter), 1);
#endif
    // double check if we overflow the guid's counter size
    ASSERT((newCount + 1 < ((u64)1<<GUID_COUNTER_SIZE)) && "GUID counter overflows");
    guid |= newCount;

    //64-bit assumption.  Should probably return a guid.
    DPRINTF(DEBUG_LVL_VVERB, "LabeledGUID generated GUID %"PRIx64"\n", guid);
    return guid;
}

u8 labeledGuidReserve(ocrGuidProvider_t *self, ocrGuid_t *startGuid, u64* skipGuid,
                      u64 numberGuids, ocrGuidKind guidType) {
    ocrGuidProviderLabeled_t *rself = (ocrGuidProviderLabeled_t*)self;
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
    u64 firstCount = hal_xadd64(&(rself->guidReservedCounter), numberGuids);
    ASSERT(firstCount  + numberGuids < (u64)1<<GUID_COUNTER_SIZE);

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
u8 labeledGuidGetGuid(ocrGuidProvider_t* self, ocrGuid_t* guid, u64 val, ocrGuidKind kind) {
    DPRINTF(DEBUG_LVL_VERB, "Start GetGuid: self=%p, guid=%p, val=0x%"PRIx64"\n", self, guid, val);
    // Here no need to allocate
    u64 newGuid = generateNextGuid(self, kind);
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: insert into hash table 0x%"PRIx64" -> 0x%"PRIx64"\n", newGuid, val);
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

u8 labeledGuidCreateGuid(ocrGuidProvider_t* self, ocrFatGuid_t *fguid, u64 size, ocrGuidKind kind, u32 properties) {

    ocrGuidProviderLabeled_t *rself = (ocrGuidProviderLabeled_t*)self;
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
        ASSERT((fguid->guid.guid & GUID_COUNTER_MASK) < rself->guidReservedCounter); // Range actually reserved
#elif GUID_BIT_COUNT == 128
        ASSERT((fguid->guid.lower & GUID_COUNTER_MASK) < rself->guidReservedCounter); // Range actually reserved
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
#if GUID_BIT_COUNT == 64
                    while((*(volatile u64*)value) != fguid->guid.guid);
#elif GUID_BIT_COUNT == 128
                    while((*(volatile u64*)value) != fguid->guid.lower);
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
    } else {
        labeledGuidGetGuid(self, &(fguid->guid), (u64)(fguid->metaDataPtr), kind);
    }
#undef PD_MSG
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: create GUID: "GUIDF" -> 0x%p\n", GUIDA(fguid->guid), fguid->metaDataPtr);
    return 0;
}

/**
 * @brief Returns the value associated with a guid and its kind if requested.
 */
u8 labeledGuidGetVal(ocrGuidProvider_t* self, ocrGuid_t guid, u64* val, ocrGuidKind* kind) {
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    *val = (u64) GP_HASHTABLE_GET(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid.guid);
#elif GUID_BIT_COUNT == 128
    *val = (u64) GP_HASHTABLE_GET(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid.lower);
#else
#error Unknown GUID type
#endif
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: got val for GUID "GUIDF": 0x%"PRIx64"\n", GUIDA(guid), *val);
    if(*val == (u64)NULL) {
        // Does not exist in the hashtable
        if(kind) {
            *kind = getKindFromGuid(guid);
        }
        return OCR_EPERM;
    } else {
        // Bug #627: We do not return until the GUID is valid. We test this
        // by looking at the first field of ptr and waiting for it to be the GUID value (meaning the
        // object has been initialized
        if(IS_RESERVED_GUID(guid)) {

            // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
            while((*(volatile u64*)(*val)) != guid.guid);
#elif GUID_BIT_COUNT == 128
            while((*(volatile u64*)(*val)) != guid.lower);
#endif
            hal_fence(); // May be overkill but there is a race that I don't get
        }
        if(kind) {
            *kind = getKindFromGuid(guid);
        }
    }

    return 0;
}

/**
 * @brief Get the 'kind' of the guid pointed object.
 */
u8 labeledGuidGetKind(ocrGuidProvider_t* self, ocrGuid_t guid, ocrGuidKind* kind) {
    *kind = getKindFromGuid(guid);
    return 0;
}

/**
 * @brief Resolve location of a GUID
 */
u8 labeledGuidGetLocation(ocrGuidProvider_t* self, ocrGuid_t guid, ocrLocation_t* location) {
    //Resolve the actual location of the GUID
    *location = (ocrLocation_t) locIdtoLocation(extractLocIdFromGuid(guid));
    return 0;
}

/**
 * @brief Associate an already existing GUID to a value.
 * This is useful in the context of distributed-OCR to register
 * a local metadata represent for a foreign GUID.
 */
u8 labeledGuidRegisterGuid(ocrGuidProvider_t* self, ocrGuid_t guid, u64 val) {
    DPRINTF(DEBUG_LVL_VERB, "LabeledGUID: register GUID "GUIDF" -> 0x%"PRIx64"\n", GUIDA(guid), val);
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid.guid, (void *) val);
#elif GUID_BIT_COUNT == 128
    GP_HASHTABLE_PUT(((ocrGuidProviderLabeled_t *) self)->guidImplTable, (void *) guid.lower, (void *) val);
#else
#error Unknown GUID type
#endif
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
    ocrGuidProviderLabeled_t *rself = (ocrGuidProviderLabeled_t*)base;
    base->fcts = factory->providerFcts;
    base->pd = NULL;
    base->id = factory->factoryId;
#ifdef GUID_PROVIDER_WID_INGUID
    {
        u32 i = 0;
        for(; i < ((u64)1<<GUID_WID_SIZE)*GUID_WID_CACHE_SIZE; ++i) {
            rself->guidCounters[0] = 0;
        }
    }
#else
    rself->guidCounter = 0;
#endif
    rself->guidReservedCounter = 0;
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
    base->providerFcts.getGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t*, u64, ocrGuidKind), labeledGuidGetGuid);
    base->providerFcts.createGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrFatGuid_t*, u64, ocrGuidKind, u32), labeledGuidCreateGuid);
    base->providerFcts.getVal = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64*, ocrGuidKind*), labeledGuidGetVal);
    base->providerFcts.getKind = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, ocrGuidKind*), labeledGuidGetKind);
    base->providerFcts.getLocation = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, ocrLocation_t*), labeledGuidGetLocation);
    base->providerFcts.registerGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64), labeledGuidRegisterGuid);
    base->providerFcts.unregisterGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrGuid_t, u64**), labeledGuidUnregisterGuid);
    base->providerFcts.releaseGuid = FUNC_ADDR(u8 (*)(ocrGuidProvider_t*, ocrFatGuid_t, bool), labeledGuidReleaseGuid);

    return base;
}

#endif /* ENABLE_GUID_LABELED */
