/**
 * @brief Simple implementation of a malloc wrapper
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_DATABLOCK_REGULAR

#include "ocr-hal.h"
#include "datablock/regular/regular-datablock.h"
#include "debug.h"
#include "ocr-comp-platform.h"
#include "ocr-datablock.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"
#include "extensions/ocr-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#endif


#define DEBUG_TYPE DATABLOCK

/***********************************************************/
/* OCR-Regular Datablock Hint Properties                   */
/* (Add implementation specific supported properties here) */
/***********************************************************/

u64 ocrHintPropDbRegular[] = {
#ifdef ENABLE_HINTS
    OCR_HINT_DB_AFFINITY
#endif
};

//Make sure OCR_HINT_COUNT_DB_REGULAR in regular-datablock.h is equal to the length of array ocrHintPropDbRegular
ocrStaticAssert((sizeof(ocrHintPropDbRegular)/sizeof(u64)) == OCR_HINT_COUNT_DB_REGULAR);
ocrStaticAssert(OCR_HINT_COUNT_DB_REGULAR < OCR_RUNTIME_HINT_PROP_BITS);

/******************************************************/
/* OCR-Regular Datablock                              */
/******************************************************/

// Forward declaraction
u8 regularDestruct(ocrDataBlock_t *self);

u8 regularAcquire(ocrDataBlock_t *self, void** ptr, ocrFatGuid_t edt, u32 edtSlot,
                  ocrDbAccessMode_t mode, bool isInternal, u32 properties) {

    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;
    *ptr = NULL;

    DPRINTF(DEBUG_LVL_VERB, "Acquiring DB @ 0x%"PRIx64" (GUID: "GUIDF") from EDT (GUID: "GUIDF") (runtime acquire: %"PRId32") size: %"PRIu64"\n",
            (u64)self->ptr, GUIDA(rself->base.guid), GUIDA(edt.guid), (u32)isInternal, self->size);

    // Critical section
    hal_lock(&(rself->lock));
    if(rself->attributes.freeRequested) {
        hal_unlock(&(rself->lock));
        return OCR_EACCES;
    }
    rself->attributes.numUsers += 1;
    if(isInternal)
        rself->attributes.internalUsers += 1;

    hal_unlock(&(rself->lock));
    // End critical section
    DPRINTF(DEBUG_LVL_VERB, "DB (GUID: "GUIDF") added EDT (GUID: "GUIDF"). Have %"PRId32" users (of which %"PRId32" runtime)\n",
            GUIDA(self->guid), GUIDA(edt.guid), rself->attributes.numUsers, rself->attributes.internalUsers);

#ifdef OCR_ENABLE_STATISTICS
    {
        statsDB_ACQ(pd, edt.guid, (ocrTask_t*)edt.metaDataPtr, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */
    *ptr = self->ptr;
    return 0;
}

u8 regularRelease(ocrDataBlock_t *self, ocrFatGuid_t edt,
                  ocrLocation_t srcLoc, bool isInternal) {

    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;

    DPRINTF(DEBUG_LVL_VERB, "Releasing DB @ 0x%"PRIx64" (GUID "GUIDF") from EDT "GUIDF" (runtime release: %"PRId32")\n",
            (u64)self->ptr, GUIDA(rself->base.guid), GUIDA(edt.guid), (u32)isInternal);

    // Start critical section
    hal_lock(&(rself->lock));

    rself->attributes.numUsers -= 1;
    if(isInternal)
        rself->attributes.internalUsers -= 1;

    DPRINTF(DEBUG_LVL_VVERB, "DB (GUID: "GUIDF") attributes: numUsers %"PRId32" (including %"PRId32" runtime users); freeRequested %"PRId32"\n",
            GUIDA(self->guid), rself->attributes.numUsers, rself->attributes.internalUsers, rself->attributes.freeRequested);
    // Check if we need to free the block
#ifdef OCR_ENABLE_STATISTICS
    {
        statsDB_REL(getCurrentPD(), edt.guid, (ocrTask_t*)edt.metaDataPtr, self->guid, self);
    }
#endif /* OCR_ENABLE_STATISTICS */

    if(rself->attributes.numUsers == 0  &&
            rself->attributes.internalUsers == 0 &&
            rself->attributes.freeRequested == 1) {
        // We need to actually free the data-block
        hal_unlock(&(rself->lock));
        return regularDestruct(self);
    } else {
        hal_unlock(&(rself->lock));
    }
    // End critical section

    return 0;
}

u8 regularDestruct(ocrDataBlock_t *self) {
    // We don't use a lock here. Maybe we should
#ifdef OCR_ASSERT
    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;
    // Check that no other EDT has acquired this datablock
    ASSERT(rself->attributes.numUsers == 0);
    ASSERT(rself->attributes.internalUsers == 0);
    ASSERT(rself->attributes.freeRequested == 1);
#endif

    DPRINTF(DEBUG_LVL_VERB, "Really freeing DB (GUID: "GUIDF")\n", GUIDA(self->guid));
    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *task = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &task, &msg);

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


#ifdef OCR_ENABLE_STATISTICS
    // This needs to be done before GUID is freed.
    {
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

u8 regularFree(ocrDataBlock_t *self, ocrFatGuid_t edt, ocrLocation_t srcLoc, u32 properties) {
    bool isInternal = ((properties & DB_PROP_RT_ACQUIRE) != 0);
    bool reqRelease = ((properties & DB_PROP_NO_RELEASE) == 0);
    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;

    DPRINTF(DEBUG_LVL_VERB, "Requesting a free for DB @ 0x%"PRIx64" (GUID: "GUIDF")\n",
            (u64)self->ptr, GUIDA(rself->base.guid));
    // Begin critical section
    hal_lock(&(rself->lock));
    if(rself->attributes.freeRequested) {
        hal_unlock(&(rself->lock));
        return OCR_EPERM;
    }
    rself->attributes.freeRequested = 1;
    hal_unlock(&(rself->lock));
    // End critical section


    // Critical section
    hal_lock(&(rself->lock));
    if(rself->attributes.numUsers == 0 && rself->attributes.internalUsers == 0) {
        hal_unlock(&(rself->lock));
        return regularDestruct(self);
    } else {
        hal_unlock(&(rself->lock));
        // The datablock may not have been acquired by the current EDT hence
        // we do not need to account for a release.
        if (reqRelease) {
            regularRelease(self, edt, srcLoc, isInternal);
        }
    }
    // End critical section

    return 0;
}

u8 regularRegisterWaiter(ocrDataBlock_t *self, ocrFatGuid_t waiter, u32 slot,
                         bool isDepAdd) {
    ASSERT(0);
    return OCR_ENOSYS;
}

u8 regularUnregisterWaiter(ocrDataBlock_t *self, ocrFatGuid_t waiter, u32 slot,
                           bool isDepRem) {
    ASSERT(0);
    return OCR_ENOSYS;
}

u8 newDataBlockRegular(ocrDataBlockFactory_t *factory, ocrFatGuid_t *guid, ocrFatGuid_t allocator,
                       ocrFatGuid_t allocPD, u64 size, void* ptr, ocrHint_t *hint, u32 flags,
                       ocrParamList_t *perInstance) {
    ocrPolicyDomain_t *pd = NULL;
    ocrTask_t *task = NULL;
    ocrGuid_t resultGuid = NULL_GUID;
    u8 returnValue = 0;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &task, &msg);

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
            targetLoc = affinityToLocation(affGuid);
        }
    }

    u32 hintc = (flags & DB_PROP_NO_HINT) ? 0 : OCR_HINT_COUNT_DB_REGULAR;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE
    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = *guid;
    PD_MSG_FIELD_I(size) = sizeof(ocrDataBlockRegular_t) + hintc*sizeof(u64);
    PD_MSG_FIELD_I(kind) = OCR_GUID_DB;
    PD_MSG_FIELD_I(targetLoc) = targetLoc;
    PD_MSG_FIELD_I(properties) = ((flags & (GUID_RT_PROP_ALL|GUID_PROP_ALL)) | GUID_PROP_TORECORD);
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));

    ocrDataBlockRegular_t *result = (ocrDataBlockRegular_t*)PD_MSG_FIELD_IO(guid.metaDataPtr);
    resultGuid = PD_MSG_FIELD_IO(guid.guid);
    returnValue = PD_MSG_FIELD_O(returnDetail);
#undef PD_MSG
#undef PD_TYPE

    if(returnValue != 0) {
        return returnValue;
    }

    ASSERT(result);
    result->base.fctId = factory->factoryId;
    result->base.allocator = allocator.guid;
    result->base.allocatingPD = allocPD.guid;
    result->base.size = size;
    result->base.ptr = ptr;
    // Only keep flags that represent the nature of
    // the DB as opposed to one-time usage creation flags
    result->base.flags = (flags & DB_PROP_SINGLE_ASSIGNMENT);
    result->base.fctId = factory->factoryId;
    result->lock = INIT_LOCK;
    result->attributes.flags = result->base.flags;
    result->attributes.numUsers = 0;
    result->attributes.internalUsers = 0;
    result->attributes.freeRequested = 0;

    if (hintc == 0) {
        result->hint.hintMask = 0;
        result->hint.hintVal = NULL;
    } else {
        OCR_RUNTIME_HINT_MASK_INIT(result->hint.hintMask, OCR_HINT_DB_T, factory->factoryId);
        result->hint.hintVal = (u64*)((u64)result + sizeof(ocrDataBlockRegular_t));
    }
#ifdef OCR_ENABLE_STATISTICS
    statsDB_CREATE(pd, task->guid, task, allocator.guid,
                   (ocrAllocator_t*)allocator.metaDataPtr, result->base.guid,
                   &(result->base));
#endif /* OCR_ENABLE_STATISTICS */
    DPRINTF(DEBUG_LVL_VERB, "Creating a datablock of size %"PRIu64", @ 0x%"PRIx64" (GUID: "GUIDF")\n",
            size, (u64)result->base.ptr, GUIDA(result->base.guid));

    // Do this at the very end; it indicates that the object
    // is actually valid
    hal_fence();
    result->base.guid = resultGuid;

    guid->guid = resultGuid;
    guid->metaDataPtr = result;
    return 0;
}

u8 regularSetHint(ocrDataBlock_t* self, ocrHint_t *hint) {
    ocrDataBlockRegular_t *derived = (ocrDataBlockRegular_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_SET(hint, rHint, OCR_HINT_COUNT_DB_REGULAR, ocrHintPropDbRegular, OCR_HINT_DB_PROP_START);
    return 0;
}

u8 regularGetHint(ocrDataBlock_t* self, ocrHint_t *hint) {
    ocrDataBlockRegular_t *derived = (ocrDataBlockRegular_t*)self;
    ocrRuntimeHint_t *rHint = &(derived->hint);
    OCR_RUNTIME_HINT_GET(hint, rHint, OCR_HINT_COUNT_DB_REGULAR, ocrHintPropDbRegular, OCR_HINT_DB_PROP_START);
    return 0;
}

ocrRuntimeHint_t* getRuntimeHintDbRegular(ocrDataBlock_t* self) {
    ocrDataBlockRegular_t *derived = (ocrDataBlockRegular_t*)self;
    return &(derived->hint);
}

/******************************************************/
/* OCR DATABLOCK REGULAR FACTORY                      */
/******************************************************/

void destructRegularFactory(ocrObjectFactory_t *factory) {
    runtimeChunkFree((u64)((ocrDataBlockFactory_t*)factory)->hintPropMap, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrDataBlockFactory_t *newDataBlockFactoryRegular(ocrParamList_t *perType, u32 factoryId) {
    ocrObjectFactory_t * bbase = (ocrObjectFactory_t *)
                                  runtimeChunkAlloc(sizeof(ocrDataBlockFactoryRegular_t), PERSISTENT_CHUNK);
    bbase->clone = NULL;
    bbase->serialize = NULL;
    bbase->deserialize = NULL;
    bbase->mdSize = NULL;

    ocrDataBlockFactory_t* base = (ocrDataBlockFactory_t*) bbase;

    // Initialize base's base
    base->base.fcts.processEvent = NULL;

    base->instantiate = FUNC_ADDR(u8 (*)
                                  (ocrDataBlockFactory_t*, ocrFatGuid_t *, ocrFatGuid_t, ocrFatGuid_t,
                                   u64, void*, ocrHint_t*, u32, ocrParamList_t*), newDataBlockRegular);
    base->base.destruct = FUNC_ADDR(void (*)(ocrObjectFactory_t*), destructRegularFactory);

    // Functions for the instance
    base->fcts.destruct = FUNC_ADDR(u8 (*)(ocrDataBlock_t*), regularDestruct);
    base->fcts.acquire = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, void**, ocrFatGuid_t, ocrLocation_t, u32, ocrDbAccessMode_t, bool, u32), regularAcquire);
    base->fcts.release = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t, ocrLocation_t, bool), regularRelease);
    base->fcts.free = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t, ocrLocation_t, u32), regularFree);
    base->fcts.registerWaiter = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t,
                                                 u32, bool), regularRegisterWaiter);
    base->fcts.unregisterWaiter = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrFatGuid_t,
                                                   u32, bool), regularUnregisterWaiter);
    base->fcts.setHint = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrHint_t*), regularSetHint);
    base->fcts.getHint = FUNC_ADDR(u8 (*)(ocrDataBlock_t*, ocrHint_t*), regularGetHint);
    base->fcts.getRuntimeHint = FUNC_ADDR(ocrRuntimeHint_t* (*)(ocrDataBlock_t*), getRuntimeHintDbRegular);
    base->factoryId = factoryId;
    //Setup hint framework
    base->hintPropMap = (u64*)runtimeChunkAlloc(sizeof(u64)*(OCR_HINT_DB_PROP_END - OCR_HINT_DB_PROP_START - 1), PERSISTENT_CHUNK);
    OCR_HINT_SETUP(base->hintPropMap, ocrHintPropDbRegular, OCR_HINT_COUNT_DB_REGULAR, OCR_HINT_DB_PROP_START, OCR_HINT_DB_PROP_END);

    return base;
}
#endif /* ENABLE_DATABLOCK_REGULAR */
