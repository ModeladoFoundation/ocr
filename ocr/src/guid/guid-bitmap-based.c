#ifdef ENABLE_GUID_BITMAP_BASED

// To be included by bitmap-based GUID implementations

static u32 hashGuidCounterModulo(void * ptr, u32 nbBuckets) {
    u64 guid = (u64) ptr;
    return ((guid & GUID_MASK(COUNTER)) % nbBuckets);
}

/**
 * @brief Utility function to extract a kind from a GUID.
 */
static ocrGuidKind getKindFromGuid(ocrGuid_t guid) {
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    return (ocrGuidKind) RSHIFT(KIND, guid.guid);
#elif GUID_BIT_COUNT == 128
    return (ocrGuidKind) RSHIFT(KIND, guid.lower);
#endif
}

/**
 * @brief Utility function to extract a kind from a GUID.
 */
static u64 extractLocIdFromGuid(ocrGuid_t guid) {
// See BUG #928 on GUID issues

#if GUID_BIT_COUNT == 64
    return (ocrGuidKind) RSHIFT(LOCHOME, guid.guid);
#elif GUID_BIT_COUNT == 128
    return (ocrGuidKind) RSHIFT(LOCHOME, guid.lower);
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
    ASSERT((locId < MAX_VAL(LOCHOME)) && "GUID location ID overflows");
    return locId;
}

static bool isLocalGuid(ocrGuidProvider_t* self, ocrGuid_t guid) {
#if defined(OCR_ASSERT) && !(defined(TG_XE_TARGET)) && !(defined(TG_CE_TARGET)) && !(defined(TG_X86_TARGET))
    int oth = (int) locIdtoLocation(extractLocIdFromGuid(guid));
    ASSERT((oth >= 0) && (oth <= self->pd->neighborCount) && "Invalid neighbor ID");
#endif
    return self->pd->myLocation == locIdtoLocation(extractLocIdFromGuid(guid));
}

/**
 * @brief Utility function to generate a new GUID.
 */
static u64 generateNextGuid(ocrGuidProvider_t* self, ocrGuidKind kind, ocrLocation_t targetLoc, u64 card) {
    RSELF_TYPE* rself = (RSELF_TYPE*)self;
    u64 shLocHome = LSHIFT(LOCHOME, locationToLocId(targetLoc));
    u64 shKind = LSHIFT(KIND, kind);
    u64 guid = (shLocHome | shKind);

#ifdef GUID_PROVIDER_LOC_MANGLE
    u64 shLocAlloc = LSHIFT(LOCALLOC, locationToLocId(self->pd->myLocation));
    guid |= shLocAlloc;
#endif

#ifdef GUID_PROVIDER_WID_INGUID
    ocrWorker_t * worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // GUIDs are generated before the current worker is setup.
    u64 wid = ((worker == NULL) ? 0 : worker->id);
    u64 shWid = LSHIFT(LOCWID, wid);
    guid |= shWid;
    u64 newCount = rself->guidCounters[wid*GUID_WID_CACHE_SIZE]+=card;
#else
    u64 newCount = hal_xadd64(&(rself->guidCounter), card);
#endif
    // double check if we overflow the guid's counter size
    //TODO this doesn't check properly now
    ASSERT(((newCount + card) < MAX_VAL(COUNTER)) && "GUID counter overflows");
    guid |= newCount;
    return guid;
}

//
// Function pointers for map-based GUID providers
//

/**
 * @brief Get the 'kind' of the guid pointed object.
 */
// Add
static u8 mapGetKind(ocrGuidProvider_t* self, ocrGuid_t guid, ocrGuidKind* kind) __attribute__((unused));
static u8 mapGetKind(ocrGuidProvider_t* self, ocrGuid_t guid, ocrGuidKind* kind) {
    *kind = getKindFromGuid(guid);
    return 0;
}

/**
 * @brief Resolve location of a GUID
 */
static u8 mapGetLocation(ocrGuidProvider_t* self, ocrGuid_t guid, ocrLocation_t* location) __attribute__((unused));
static u8 mapGetLocation(ocrGuidProvider_t* self, ocrGuid_t guid, ocrLocation_t* location) {
    //Resolve the actual location of the GUID
    *location = (ocrLocation_t) locIdtoLocation(extractLocIdFromGuid(guid));
    return 0;
}
#endif
