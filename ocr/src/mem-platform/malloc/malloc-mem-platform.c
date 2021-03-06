/**
 * @brief Simple implementation of a malloc wrapper
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_MEM_PLATFORM_MALLOC

#include "ocr-hal.h"
#include "debug.h"
#include "utils/rangeTracker.h"
#include "ocr-sysboot.h"
#include "ocr-types.h"
#include "ocr-mem-platform.h"
#include "ocr-policy-domain.h"
#include "mem-platform/malloc/malloc-mem-platform.h"

#include <stdlib.h>
#include <string.h>

// Poor man's basic lock
#define INIT_LOCKF(addr) do {*addr = INIT_LOCK;} while(0);
#define LOCK(addr) do { hal_lock(addr); } while(0);
#define UNLOCK(addr) do { hal_unlock(addr); } while(0);

/******************************************************/
/* OCR MEM PLATFORM MALLOC IMPLEMENTATION             */
/******************************************************/

void mallocDestruct(ocrMemPlatform_t *self) {
    // BUG #673: Deal with objects owned by multiple PDs
    //runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
}

// BUG #673: This mem-platform may be shared by multiple threads (for example
// one SPAD shared by 2 CEs. We therefore do the malloc/free and what not extremely early
// on so that only the NODE_MASTER does it in a race free manner.
u8 mallocSwitchRunlevel(ocrMemPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                        phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ocrAssert(callback == NULL);

    // Verify properties for this call
    ocrAssert((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ocrAssert(!(properties & RL_FROM_MSG));

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        // This should ideally be in MEMORY_OK
        // NOTE: This is serial because only thread is up until PD_OK
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_NETWORK_OK, phase)) {
            if(self->startAddr != 0ULL)
                break; // We break out early since we are already initialized
            // This is where we need to update the memory
            // using the sysboot functions
            self->startAddr = (u64)malloc(self->size);
            // Check that the mem-platform size in config file is reasonable
            ocrAssert(self->startAddr);
            self->endAddr = self->startAddr + self->size;

            // rangeTracker will be located at self->startAddr, and it should be zero'ed
            // since initializeRange() assumes zero-ed 'lock' and 'inited' variables
            ocrAssert(self->size >= MEM_PLATFORM_ZEROED_AREA_SIZE);    // make sure no buffer overrun
            // zero beginning part to cover rangeTracker and pad, and allocator metadata part i.e. pool header (pool_t)
            memset((void *)self->startAddr , 0, MEM_PLATFORM_ZEROED_AREA_SIZE);

            ocrMemPlatformMalloc_t *rself = (ocrMemPlatformMalloc_t*)self;
            rself->pRangeTracker = initializeRange(
                16, self->startAddr, self->endAddr, USER_FREE_TAG);
        } else if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_NETWORK_OK, phase)) {
            // This is also serial because after PD_OK we are down to one thread
            ocrMemPlatformMalloc_t *rself = (ocrMemPlatformMalloc_t*)self;
            // The first guy through here does this
            if(self->startAddr != 0ULL) {
                if(rself->pRangeTracker)    // in case of mallocproxy, pRangeTracker==0
                    destroyRange(rself->pRangeTracker);
                // Here we can free the memory we allocated
                free((void*)(self->startAddr));
                self->startAddr = 0ULL;
            }
        }
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            // We can now set our PD (before this, we couldn't because
            // "our" PD might not have been started
            self->pd = PD;
        }
        break;
    case RL_MEMORY_OK:
        // Should ideally do what's in NETWORK_OK
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ocrAssert(0);
    }
    return toReturn;
}

u8 mallocGetThrottle(ocrMemPlatform_t *self, u64 *value) {
    return 1; // Not supported
}

u8 mallocSetThrottle(ocrMemPlatform_t *self, u64 value) {
    return 1; // Not supported
}

void mallocGetRange(ocrMemPlatform_t *self, u64* startAddr,
                    u64 *endAddr) {
    if(startAddr) *startAddr = self->startAddr;
    if(endAddr) *endAddr = self->endAddr;
}

u8 mallocChunkAndTag(ocrMemPlatform_t *self, u64 *startAddr, u64 size,
                     ocrMemoryTag_t oldTag, ocrMemoryTag_t newTag) {

    if(oldTag >= MAX_TAG || newTag >= MAX_TAG)
        return 3;

    ocrMemPlatformMalloc_t *rself = (ocrMemPlatformMalloc_t *)self;

    u64 iterate = 0;
    u64 startRange, endRange;
    u8 result;
    LOCK(&(rself->pRangeTracker->lockChunkAndTag));
    // first check if there's existing one. (query part)
    do {
        result = getRegionWithTag(rself->pRangeTracker, newTag, &startRange,
                                  &endRange, &iterate);
        if(result == 0 && endRange - startRange >= size) {
            *startAddr = startRange;
//            printf("ChunkAndTag returning (existing) start of 0x%"PRIx64" for size %"PRId64" (0x%"PRIx64") Tag %"PRId32"\n",
//                    *startAddr, size, size, newTag);
            // exit.
            UNLOCK(&(rself->pRangeTracker->lockChunkAndTag));
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
//            printf("ChunkAndTag returning start of 0x%"PRIx64" for size %"PRId64" (0x%"PRIx64") and newTag %"PRId32"\n",
//                    *startAddr, size, size, newTag);
            RESULT_ASSERT(splitRange(rself->pRangeTracker,
                                     startRange, size, newTag, 0), ==, 0);
            break;
        } else {
            if(result == 0) {
//                printf("ChunkAndTag, found [0x%"PRIx64"; 0x%"PRIx64"[ but too small for size %"PRId64" (0xllx)\n",
//                        startRange, endRange, size, size);
            }
        }
    } while(result == 0);

    UNLOCK(&(rself->pRangeTracker->lockChunkAndTag));
    return result;
}

u8 mallocTag(ocrMemPlatform_t *self, u64 startAddr, u64 endAddr,
             ocrMemoryTag_t newTag) {

    if(newTag >= MAX_TAG)
        return 3;

    ocrMemPlatformMalloc_t *rself = (ocrMemPlatformMalloc_t *)self;

    LOCK(&(rself->lock));
    RESULT_ASSERT(splitRange(rself->pRangeTracker, startAddr,
                             endAddr - startAddr, newTag, 0), ==, 0);
    UNLOCK(&(rself->lock));
    return 0;
}

u8 mallocQueryTag(ocrMemPlatform_t *self, u64 *start, u64* end,
                  ocrMemoryTag_t *resultTag, u64 addr) {
    ocrMemPlatformMalloc_t *rself = (ocrMemPlatformMalloc_t *)self;

    RESULT_ASSERT(getTag(rself->pRangeTracker, addr, start, end, resultTag),
                  ==, 0);
    return 0;
}

ocrMemPlatform_t* newMemPlatformMalloc(ocrMemPlatformFactory_t * factory,
                                       ocrParamList_t *perInstance) {

    ocrMemPlatform_t *result = (ocrMemPlatform_t*)
                               runtimeChunkAlloc(sizeof(ocrMemPlatformMalloc_t), PERSISTENT_CHUNK);
    factory->initialize(factory, result, perInstance);
    return result;
}

void initializeMemPlatformMalloc(ocrMemPlatformFactory_t * factory, ocrMemPlatform_t * result, ocrParamList_t * perInstance) {
    initializeMemPlatformOcr(factory, result, perInstance);
    ocrMemPlatformMalloc_t *rself = (ocrMemPlatformMalloc_t*)result;
    INIT_LOCKF(&(rself->lock));
}

/******************************************************/
/* OCR MEM PLATFORM MALLOC FACTORY                    */
/******************************************************/

void destructMemPlatformFactoryMalloc(ocrMemPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrMemPlatformFactory_t *newMemPlatformFactoryMalloc(ocrParamList_t *perType) {
    ocrMemPlatformFactory_t *base = (ocrMemPlatformFactory_t*)
                                    runtimeChunkAlloc(sizeof(ocrMemPlatformFactoryMalloc_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newMemPlatformMalloc;
    base->initialize = &initializeMemPlatformMalloc;
    base->destruct = &destructMemPlatformFactoryMalloc;
    base->platformFcts.destruct = FUNC_ADDR(void (*) (ocrMemPlatform_t *), mallocDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrMemPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), mallocSwitchRunlevel);
    base->platformFcts.getThrottle = FUNC_ADDR(u8 (*) (ocrMemPlatform_t *, u64 *), mallocGetThrottle);
    base->platformFcts.setThrottle = FUNC_ADDR(u8 (*) (ocrMemPlatform_t *, u64), mallocSetThrottle);
    base->platformFcts.getRange = FUNC_ADDR(void (*) (ocrMemPlatform_t *, u64 *, u64 *), mallocGetRange);
    base->platformFcts.chunkAndTag = FUNC_ADDR(u8 (*) (ocrMemPlatform_t *, u64 *, u64, ocrMemoryTag_t, ocrMemoryTag_t), mallocChunkAndTag);
    base->platformFcts.tag = FUNC_ADDR(u8 (*) (ocrMemPlatform_t *, u64, u64, ocrMemoryTag_t), mallocTag);
    base->platformFcts.queryTag = FUNC_ADDR(u8 (*) (ocrMemPlatform_t *, u64 *, u64 *, ocrMemoryTag_t *, u64), mallocQueryTag);
    return base;
}

#endif /* ENABLE_MEM_PLATFORM_MALLOC */
