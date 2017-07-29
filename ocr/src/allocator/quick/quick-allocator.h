/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __ALLOCATOR_QUICK_H__
#define __ALLOCATOR_QUICK_H__

#include "ocr-config.h"
#ifdef ENABLE_ALLOCATOR_QUICK

#include "ocr-allocator.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"

typedef struct {
    ocrAllocatorFactory_t base;
} ocrAllocatorFactoryQuick_t;

#ifdef OCR_CACHE_LINE_OFFSET_ALLOCATIONS
/*  Cache Line Conflicts
 *
 *  Architectures with data cache can have performance issues if large arrays
 *  have starting addresses that map to similar cache lines.  For example, if
 *  the cache lines are based on the bottom 12 bits of an address, the page
 *  size is 4096 (2^12) and large allocations are page aligned, then the
 *  following code, in a two-way cache will cause every access to wipe out
 *  cache lines:
 *     u8 A[1024*1024], B[1024*1024], C[1024*1024];
 *     for (i=0; i<1024*1024; i++)
 *        A[i] = B[i] + C[i];
 *  Because X[i] for all three arrays will map to the same cache line(s).
 *  Performance improvements of 5% or more can be had by offsetting start
 *  address used by the user for large arrays.
 */

/** Structure to hold minimum size allocation for which cache line offset
 *  handling should be done, and the amount of offset that should be applied
 *  to each "large" allocation. */
typedef struct {
    u64 offset;     // Offset delta for "large" allocations to reduce cache
                    // line conflicts
    u64 largeSize;  // Size of allocation which is considered "large" in
                    // allocations.
    u64 cacheSize;  // Size of cache line address space.  In example above 2^12.
    u64 curOffset;  // Atomic! Offset for next memory allocation.
} ocrAllocatorQuick_CacheLineHints_t;

/** Only instance in this program of this structure. */
extern ocrAllocatorQuick_CacheLineHints_t ocrQuickCacheLineHints;


typedef struct {
    void *realAddr;   // Used to indicate were real user addr was before offset
    u64 negativeOne;  // Always -1 to indicat this is not real alloc header
} ocrAllocatorQuick_offsetHeader_t;

/** Structure to hold Quick Allocator header information.
 *  (See discussion above about cache line conflicts).
 *  The 'userAddr' is offset from the 'poolAddr' by the offset amount
 *  defined in OcrAllocatorQuick  */
#endif // OCR_CACHE_LINE_OFFSET_ALLOCATIONS
typedef struct {
    ocrAllocator_t base;
    volatile u64 poolAddr;  // Address of the 8-byte-aligned net pool storage space.
    u64 poolSize;
    u8  poolStorageOffset;  // Distance from poolAddr to storage address of the pool (which wasn't necessarily 8-byte aligned).
    u8  poolStorageSuffix;  // Bytes at end of storage space not usable for the pool.
} ocrAllocatorQuick_t;

typedef struct {
    paramListAllocatorInst_t base;
} paramListAllocatorQuick_t;

extern ocrAllocatorFactory_t* newAllocatorFactoryQuick(ocrParamList_t *perType);

void quickDeallocate(void* address);

#endif /* ENABLE_ALLOCATOR_QUICK */
#endif /* __QUICK_ALLOCATOR_H__ */
