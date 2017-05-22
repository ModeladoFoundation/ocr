/**
 * @brief Simple implementation of a fsim wrapper
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_MEM_PLATFORM_FSIM

#include "ocr-hal.h"
#include "ocr-sal.h"
#include "debug.h"
#include "utils/rangeTracker.h"
#include "mem-platform/fsim/fsim-mem-platform.h"
#include "ocr-mem-platform.h"
#include "ocr-sysboot.h"

#define DEBUG_TYPE MEM_PLATFORM

// Poor man's basic lock
#define INIT_LOCKF(addr) do {*addr = INIT_LOCK;} while(0);
#define LOCK(addr) do { hal_lock(addr); } while(0);
#define UNLOCK(addr) do { hal_unlock(addr); } while(0);

/******************************************************/
/* OCR MEM PLATFORM FSIM IMPLEMENTATION             */
/******************************************************/

void fsimDestruct(ocrMemPlatform_t *self) {
    destroyRange(((ocrMemPlatformFsim_t*)self)->pRangeTracker);
    runtimeChunkFree((u64)self, NULL);
}

struct _ocrPolicyDomain_t;

u8 fsimSwitchRunlevel(ocrMemPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
        break;
    case RL_PD_OK:
        break;
    case RL_MEMORY_OK:
        if(properties & RL_BRING_UP) {
            // We can now set our PD (before this, we couldn't because
            // "our" PD might not have been started
            self->pd = PD;
            ASSERT(self->startAddr);
            self->endAddr = self->startAddr + self->size;

#ifdef SAL_FSIM_CE
            // Needed to workaround a Qemu issue where the Qemu-managed scratchpad isn't zeroed properly Bug #699
            if(RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase) && (val == 1)) {
                u32 i;
                for(i = 0; i<MEM_PLATFORM_ZEROED_AREA_SIZE && i<self->size; i+=8)
                    *(u64 *)(self->startAddr+i) = 0ULL;
           }
#endif

            DPRINTF(DEBUG_LVL_VERB, "Initializing memory range %"PRIx64" to %"PRIx64"\n", self->startAddr, self->endAddr);
            ocrMemPlatformFsim_t *rself = (ocrMemPlatformFsim_t*)self;
            rself->pRangeTracker = initializeRange(16, self->startAddr,
                    self->endAddr, USER_FREE_TAG);
        }
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

u8 fsimGetThrottle(ocrMemPlatform_t *self, u64 *value) {
    return 1; // Not supported
}

u8 fsimSetThrottle(ocrMemPlatform_t *self, u64 value) {
    return 1; // Not supported
}

void fsimGetRange(ocrMemPlatform_t *self, u64* startAddr,
                  u64 *endAddr) {
    if(startAddr) *startAddr = self->startAddr;
    if(endAddr) *endAddr = self->endAddr;
}

u8 fsimChunkAndTag(ocrMemPlatform_t *self, u64 *startAddr, u64 size,
                   ocrMemoryTag_t oldTag, ocrMemoryTag_t newTag) {

        ADebug(AllocDebugTgMem, "fsim-mem-platform.c/fsimChunkAndTag() "
               "PD=%p, startAddr=0x%lx, size=%lu\n", self, (u64)startAddr, size);

    if(oldTag >= MAX_TAG || newTag >= MAX_TAG) {
        DPRINTF(DEBUG_LVL_WARN, "Cannot chunk and tag because oldTag (%"PRId32") or newTag (%"PRId32") are bigger than %"PRId32"\n",
                (u32)oldTag, (u32)newTag, (u32)MAX_TAG);
        ADebug(AllocDebugTgMem, "fsim-mem-platform.c/fsimChunkAndTag() "
               "returns 3 (oldTag=%d, newTag=%d, MAX_TAG=%d)\n",
               (int)oldTag, (int)newTag, (int)MAX_TAG);
        return 3;
    }

    ocrMemPlatformFsim_t *rself = (ocrMemPlatformFsim_t *)self;

    u64 iterate = 0;
    u64 startRange, endRange;
    u8 result;
    LOCK(&(rself->pRangeTracker->lockChunkAndTag));
    /* I had to change this function's behavior a bit.. (it's a bit hacky, so please let me know
     * if you have suggestions how to properly do it)
     *
     * scenario: multiple CE calls fsimChunkAndTag() , and first one allocates a big chunk.
     *
     * original semantic : the rest tries to allocate another chunk, but fails, so fsim crashes.
     * or, it allocates different private chunk so each CE has its own private portion for BSM or CSM
     *
     * current semantic :  the other CEs arrive but they shouldn't allocate a new chunk, but they
     * should get the return value of that already-allocated chunk. So, fsimChunkAndTag() first check
     * if there's existing one and return it if true. Note that this is atomic behavior. It queries
     * if any other CE already allocated that chunk, and get that chunk if so. Otherwise, it does
     * allocation for all CEs that shares this memory.
     *
     * It's hacky at the moment, so we should define better (atomic) semantics for fsimChunkAndTag() or
     * other chunkAndTag() functions. Need a bit of redoing.
     */

     /* 'If' statement below checks if the call was successful (result == 0 &&) and also check
      * if the size is big enough (endRange - startRange >= size) , if both are met, it exits because
      * it found a good one. If not, it keeps calling getRegionWithTag(), if it fails eventually, it
      * exits while loop.
      *
      * Three cases.
      * (1) It returns when result == 0 AND size is big enough.
      * (2) Even if result == 0, if the size is small, it does another iteration by while loop
      *     i.e. call again getRegionWithTag()
      * (3) If result != 0 , it exits the while loop.
      */

    // first check if there's existing one. (query part)
    do {
        result = getRegionWithTag(rself->pRangeTracker, newTag, &startRange,
                                  &endRange, &iterate);
        if(result == 0 && endRange - startRange >= size) {
            *startAddr = startRange;
            DPRINTF(DEBUG_LVL_VERB, "ChunkAndTag returning (existing) start of 0x%"PRIx64" for size %"PRId64" (0x%"PRIx64") Tag %"PRId32"\n",
                    *startAddr, size, size, newTag);
            // exit.
            UNLOCK(&(rself->pRangeTracker->lockChunkAndTag));
            ADebug(AllocDebugTgMem, "fsim-mem-platform.c/fsimChunkAndTag() "
                   "returns old result: startAddr=%ld, size=%lu\n",
                   *startAddr, size);
            return result;
        }
    } while(result == 0);

    // now do chunkAndTag (allocation part)
    iterate = 0;
    do {
        result = getRegionWithTag(rself->pRangeTracker, oldTag, &startRange,
                                  &endRange, &iterate);
        if(result == 0 && endRange - startRange >= size) {
            // This is a fit, we do not look for "best" fit for now
            *startAddr = startRange;
            DPRINTF(DEBUG_LVL_VERB, "ChunkAndTag returning start of 0x%"PRIx64" for size %"PRId64" (0x%"PRIx64") and newTag %"PRId32"\n",
                    *startAddr, size, size, newTag);
            RESULT_ASSERT(splitRange(rself->pRangeTracker,
                                     startRange, size, newTag, 0), ==, 0);
            break;
        } else {
            if(result == 0) {
                DPRINTF(DEBUG_LVL_VVERB, "ChunkAndTag, found [0x%"PRIx64"; 0x%"PRIx64"[ but too small for size %"PRId64" (0x%"PRIx64")\n",
                        startRange, endRange, size, size);
            }
        }
    } while(result == 0);
    UNLOCK(&(rself->pRangeTracker->lockChunkAndTag));
            ADebug(AllocDebugTgMem, "fsim-mem-platform.c/fsimChunkAndTag() "
                   "returns new result: startAddr=%ld, size=%lu\n",
                   *startAddr, size);
    return result;
}

u8 fsimTag(ocrMemPlatform_t *self, u64 startAddr, u64 endAddr,
           ocrMemoryTag_t newTag) {

    if(newTag >= MAX_TAG)
        return 3;

    ocrMemPlatformFsim_t *rself = (ocrMemPlatformFsim_t *)self;

    LOCK(&(rself->lock));
    RESULT_ASSERT(splitRange(rself->pRangeTracker, startAddr,
                             endAddr - startAddr, newTag, 0), ==, 0);
    UNLOCK(&(rself->lock));
    return 0;
}

u8 fsimQueryTag(ocrMemPlatform_t *self, u64 *start, u64* end,
                ocrMemoryTag_t *resultTag, u64 addr) {
    ocrMemPlatformFsim_t *rself = (ocrMemPlatformFsim_t *)self;

    RESULT_ASSERT(getTag(rself->pRangeTracker, addr, start, end, resultTag),
                  ==, 0);
    return 0;
}

ocrMemPlatform_t* newMemPlatformFsim(ocrMemPlatformFactory_t * factory,
                                     ocrParamList_t *perInstance) {

    ocrMemPlatform_t *result = (ocrMemPlatform_t*)
                               runtimeChunkAlloc(sizeof(ocrMemPlatformFsim_t), PERSISTENT_CHUNK);
    factory->initialize(factory, result, perInstance);

    return result;
}

void initializeMemPlatformFsim(ocrMemPlatformFactory_t * factory,
                               ocrMemPlatform_t * result, ocrParamList_t * perInstance) {

    initializeMemPlatformOcr(factory, result, perInstance);
    ocrMemPlatformFsim_t *rself = (ocrMemPlatformFsim_t*)result;
    result->startAddr = ((paramListMemPlatformFsim_t *)perInstance)->start;
    INIT_LOCKF(&(rself->lock));
}

/******************************************************/
/* OCR MEM PLATFORM FSIM FACTORY                    */
/******************************************************/

void destructMemPlatformFactoryFsim(ocrMemPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrMemPlatformFactory_t *newMemPlatformFactoryFsim(ocrParamList_t *perType) {
    ocrMemPlatformFactory_t *base = (ocrMemPlatformFactory_t*)
                                    runtimeChunkAlloc(sizeof(ocrMemPlatformFactoryFsim_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newMemPlatformFsim;
    base->initialize = &initializeMemPlatformFsim;
    base->destruct = &destructMemPlatformFactoryFsim;
    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrMemPlatform_t*), fsimDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), fsimSwitchRunlevel);
    base->platformFcts.getThrottle = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, u64*), fsimGetThrottle);
    base->platformFcts.setThrottle = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, u64), fsimSetThrottle);
    base->platformFcts.getRange = FUNC_ADDR(void (*)(ocrMemPlatform_t*, u64*, u64*), fsimGetRange);
    base->platformFcts.chunkAndTag = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, u64*, u64, ocrMemoryTag_t, ocrMemoryTag_t), fsimChunkAndTag);
    base->platformFcts.tag = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, u64, u64, ocrMemoryTag_t), fsimTag);
    base->platformFcts.queryTag = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, u64*, u64*, ocrMemoryTag_t*, u64), fsimQueryTag);

    return base;
}

#endif /* ENABLE_MEM_PLATFORM_FSIM */
