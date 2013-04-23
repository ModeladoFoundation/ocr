/**
 * @brief Simple implementation of a malloc wrapper
 * @authors Romain Cledat, Intel Corporation
 * Copyright (c) 2012, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the
 *       distribution.
 *    3. Neither the name of Intel Corporation nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **/

#include <stdlib.h>
#include <errno.h>
#include "regular.h"
#include "debug.h"
#include "ocr-task-event.h"
#include "ocr-guid.h"
#include "ocr-config.h"
#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

void regularCreate(ocrDataBlock_t *self, ocrGuid_t allocatorGuid, u64 size,
                   u16 flags, void* configuration) {

    ocrAllocator_t *allocator = NULL;
    globalGuidProvider->getVal(globalGuidProvider, allocatorGuid, (u64*)&allocator, NULL);
    ASSERT(allocator);
    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;

    rself->ptr = allocator->allocate(allocator, size);
    rself->allocatorGuid = allocatorGuid;
    rself->size = size;
    rself->lock = GocrLockFactory->instantiate(GocrLockFactory, NULL);
    rself->attributes.flags = flags;
    rself->attributes.numUsers = 0;
    rself->attributes.freeRequested = 0;
    ocrGuidTrackerInit(&(rself->usersTracker));
}

void regularDestruct(ocrDataBlock_t *self) {
    // We don't use a lock here. Maybe we should
    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;
    ASSERT(rself->attributes.numUsers == 0);
    ASSERT(rself->attributes.freeRequested == 0);
    rself->lock->destruct(rself->lock);

    // Tell the allocator to free the data-block
    ocrAllocator_t *allocator = NULL;
    globalGuidProvider->getVal(globalGuidProvider, rself->allocatorGuid, (u64*)&allocator, NULL);

    allocator->free(allocator, rself->ptr); // TODO sagnak first argument was rself->allocator

    // TODO: This is not pretty to be here but I can't put this in the ocrDbFree because
    // the semantics of ocrDbFree is that it will wait for all acquire/releases to have
    // completed so I have to release the block when it is really being freed. This current
    // OCR version does not really implement delayed freeing but may in the future.
#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcessDestruct(&(rself->base.statProcess));
#endif

    globalGuidProvider->releaseGuid(globalGuidProvider, self->guid);
    free(rself);
}

void* regularAcquire(ocrDataBlock_t *self, ocrGuid_t edt, bool isInternal) {
    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;

    // Critical section
    rself->lock->lock(rself->lock);
    if(rself->attributes.freeRequested) {
        rself->lock->unlock(rself->lock);
        return NULL;
    }
    u32 idForEdt = ocrGuidTrackerFind(&(rself->usersTracker), edt);
    if(idForEdt > 63)
        idForEdt = ocrGuidTrackerTrack(&(rself->usersTracker), edt);
    else {
        rself->lock->unlock(rself->lock);
        return rself->ptr;
    }

    if(idForEdt > 63) {
        rself->lock->unlock(rself->lock);
        return NULL;
    }
    rself->attributes.numUsers += 1;
    if(isInternal)
        rself->attributes.internalUsers += 1;

    rself->lock->unlock(rself->lock);
    // End critical section

    return rself->ptr;
}

u8 regularRelease(ocrDataBlock_t *self, ocrGuid_t edt,
                  bool isInternal) {

    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;
    u32 edtId = ocrGuidTrackerFind(&(rself->usersTracker), edt);
    bool isTracked = true;

    // Start critical section
    rself->lock->lock(rself->lock);
    if(edtId > 63 || rself->usersTracker.slots[edtId] != edt) {
        // We did not find it. The runtime may be
        // re-releasing it
        if(isInternal) {
            // This is not necessarily an error
            rself->attributes.internalUsers -= 1;
            isTracked = false;
        } else {
            // Definitely a problem here
            rself->lock->unlock(rself->lock);
            return (u8)EACCES;
        }
    }

    if(isTracked) {
        ocrGuidTrackerRemove(&(rself->usersTracker), edt, edtId);
        rself->attributes.numUsers -= 1;
        if(isInternal) {
            rself->attributes.internalUsers -= 1;
        }
    }
    // Check if we need to free the block
    if(rself->attributes.numUsers == 0  &&
       rself->attributes.internalUsers == 0 &&
       rself->attributes.freeRequested == 1) {
        // We need to actually free the data-block
        rself->lock->unlock(rself->lock);
        regularDestruct(self);
    } else {
        rself->lock->unlock(rself->lock);
    }
    // End critical section

    return 0;
}

u8 regularFree(ocrDataBlock_t *self, ocrGuid_t edt) {
    ocrDataBlockRegular_t *rself = (ocrDataBlockRegular_t*)self;

    u32 id = ocrGuidTrackerFind(&(rself->usersTracker), edt);
    // Begin critical section
    rself->lock->lock(rself->lock);
    if(rself->attributes.freeRequested) {
        rself->lock->unlock(rself->lock);
        return EPERM;
    }
    rself->attributes.freeRequested = 1;
    rself->lock->unlock(rself->lock);
    // End critical section

    // We can call free without having acquired the block
    if(id < 64) {
        regularRelease(self, edt, false);
    }
    // Now check if we can actually free the block

    // Critical section
    rself->lock->lock(rself->lock);
    if(rself->attributes.numUsers == 0) {
        rself->lock->unlock(rself->lock);
        regularDestruct(self);
    } else {
        rself->lock->unlock(rself->lock);
    }
    // End critical section

    return 0;
}

ocrDataBlock_t* newDataBlockRegular() {
    ocrDataBlockRegular_t *result = (ocrDataBlockRegular_t*)malloc(sizeof(ocrDataBlockRegular_t));
    result->base.guid = UNINITIALIZED_GUID;
    globalGuidProvider->getGuid(globalGuidProvider, &(result->base.guid), (u64)result, OCR_GUID_DB);

    result->base.create = &regularCreate;
    result->base.destruct = &regularDestruct;
    result->base.acquire = &regularAcquire;
    result->base.release = &regularRelease;
    result->base.free = &regularFree;
    result->lock = NULL;

    return (ocrDataBlock_t*)result;
}
