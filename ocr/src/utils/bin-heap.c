/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 *
 * Binary heap based in the implementation described in this book chapter:
 * http://opendatastructures.org/versions/edition-0.1e/ods-java/10_1_BinaryHeap_Implicit_Bi.html
 */

#include "ocr-config.h"

#include "ocr-hal.h"
#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "utils/bin-heap.h"

#define DEBUG_TYPE UTIL

/****************************************************/
/* BINARY HEAP BASE IMPLEMENTATIONS                 */
/****************************************************/

/*
 * BinHeap destroy
 */
void binHeapDestroy(ocrPolicyDomain_t *pd, binHeap_t* heap) {
    pd->fcts.pdFree(pd, (void*)heap->data);
    pd->fcts.pdFree(pd, heap);
}

/*
 * @brief base constructor for binHeaps. Think of this as a virtual class
 * where the function pointers to push and pop are set by the derived
 * implementation.
 */
static void baseBinHeapInit(binHeap_t* heap, ocrPolicyDomain_t *pd) {
    heap->count = 0;
    heap->data = NULL;
    heap->data = pd->fcts.pdMalloc(pd, sizeof(void*)*INIT_BIN_HEAP_CAPACITY);
    ASSERT(heap->data != NULL);
    heap->destruct = binHeapDestroy;
    // Set by derived implementation
    heap->push = NULL;
    heap->pop = NULL;
}

static void lockedBinHeapInit(binHeapLocked_t* heap, ocrPolicyDomain_t *pd) {
    baseBinHeapInit((binHeap_t*)heap, pd);
    heap->lock = 0;
}

static binHeap_t * newBaseBinHeap(ocrPolicyDomain_t *pd, ocrBinHeapType_t type) {
    binHeap_t* heap = NULL;
    switch(type) {
        case NO_LOCK_BASE_BIN_HEAP:
            heap = (binHeap_t*) pd->fcts.pdMalloc(pd, sizeof(binHeap_t));
            baseBinHeapInit(heap, pd);
            // Warning: function pointers must be specialized in caller
            break;
        case LOCK_BASE_BIN_HEAP:
            heap = (binHeap_t*) pd->fcts.pdMalloc(pd, sizeof(binHeapLocked_t));
            lockedBinHeapInit((binHeapLocked_t*)heap, pd);
            // Warning: function pointers must be specialized in caller
            break;
    default:
        ASSERT(0);
    }
    heap->type = type;
    return heap;
}


/****************************************************/
/* NON CONCURRENT BIN_HEAP BASED OPERATIONS         */
/****************************************************/

static inline u32 left(u32 i) { return 2*i + 1; }
static inline u32 right(u32 i) { return 2*i + 2; }
static inline u32 parent(u32 i) { return (i-1)/2; }

static inline bool heapOK(binHeap_t *heap, u32 p, u32 c) {
    return heap->data[p].priority > heap->data[c].priority;
}

static inline void swap(binHeap_t *heap, s64 i, s64 j) {
    const ocrBinHeapEntry_t tmp = heap->data[i];
    heap->data[i] = heap->data[j];
    heap->data[j] = tmp;
}

void bubbleUp(binHeap_t *heap, u32 i) {
    u32 p = parent(i);
    while (i > 0 && !heapOK(heap, p, i)) {
        swap(heap, p, i);
        i = p;
        p = parent(i);
    }
}

void trickleDown(binHeap_t *heap, u32 ii) {
    const u32 n = heap->count;
    s64 i = ii;
    do {
        s64 j = -1;
        u32 r = right(i);
        if (r < n && !heapOK(heap, i, r)) {
            u32 l = left(i);
            if (!heapOK(heap, r, l)) {
                j = l;
            } else {
                j = r;
            }
        } else {
            u32 l = left(i);
            if (l < n && !heapOK(heap, i, l)) {
                j = l;
            }
        }
        if (j >= 0) swap(heap, i, j);
        i = j;
    } while (i >= 0);
}

void checkHeap(binHeap_t *heap) {
#if _OCR_BIN_HEAP_DEBUG
    const u32 n = heap->count;
    u32 i;
    for (i=0;;i++) {
        if (left(i) > n) break;
        ASSERT(heap->data[i].priority >= heap->data[left(i)].priority);
        if (right(i) > n) break;
        ASSERT(heap->data[i].priority >= heap->data[right(i)].priority);
    }
#endif /* _OCR_BIN_HEAP_DEBUG */
}

/*
 * push an entry onto the tail of the binHeap
 */
void nonConcBinHeapPush(binHeap_t *heap, void *entry, s64 priority, u8 doTry) {
    const u32 n = heap->count;
    if (n == INIT_BIN_HEAP_CAPACITY) { /* binHeap looks full */
        ASSERT("Binary heap full, increase bin-heap's size" && 0);
    }
    heap->count++;
    ocrBinHeapEntry_t node = { priority, entry };
    heap->data[n] = node;
    bubbleUp(heap, n);
    checkHeap(heap);
}

/*
 * pop the task out of the binHeap from the tail
 */
void *nonConcBinHeapPop(binHeap_t *heap, u8 doTry) {
    if (heap->count == 0) return NULL;
    const u32 n = --heap->count;
    void *rt = heap->data[0].entry;
    heap->data[0] = heap->data[n];
    trickleDown(heap, 0);
    checkHeap(heap);
    return rt;
}


/******************************************************/
/* LOCKED BIN HEAP BASED OPERATIONS                   */
/******************************************************/

/*
 * Push an entry onto the binHeap
 * This operation locks the whole binHeap.
 */
void lockedBinHeapPush(binHeap_t *self, void *entry, s64 priority, u8 doTry) {
    binHeapLocked_t* dself = (binHeapLocked_t*)self;
    hal_lock32(&dself->lock);
    nonConcBinHeapPush(self, entry, priority, doTry);
    hal_unlock32(&dself->lock);
}

/*
 * Pop the task out of the binHeap
 * This operation locks the whole binHeap.
 */
void * lockedBinHeapPop(binHeap_t * self, u8 doTry) {
    binHeapLocked_t* dself = (binHeapLocked_t*)self;
    hal_lock32(&dself->lock);
    void *rt = nonConcBinHeapPop(self, doTry);
    hal_unlock32(&dself->lock);
    return rt;
}


/******************************************************/
/* BIN_HEAP CONSTRUCTORS                              */
/******************************************************/

/*
 * @brief BinHeap constructor. For a given type, create an instance and
 * initialize its base type.
 */
binHeap_t * newBinHeap(ocrPolicyDomain_t *pd, ocrBinHeapType_t type) {
    binHeap_t* heap = NULL;
    switch(type) {
    case NON_CONCURRENT_BIN_HEAP:
        heap = newBaseBinHeap(pd, NO_LOCK_BASE_BIN_HEAP);
        // Specialize push/pop implementations
        heap->push = nonConcBinHeapPush;
        heap->pop = nonConcBinHeapPop;
        break;
    case LOCKED_BIN_HEAP:
        heap = newBaseBinHeap(pd, LOCK_BASE_BIN_HEAP);
        // Specialize push/pop implementations
        heap->push =  lockedBinHeapPush;
        heap->pop = lockedBinHeapPop;
        break;
    default:
        ASSERT(0);
    }
    heap->type = type;
    return heap;
}

/**
 * @brief Unsynchronized implementation for push and pop
 */
binHeap_t* newNonConcurrentBinHeap(ocrPolicyDomain_t *pd) {
    binHeap_t* heap = newBinHeap(pd, NON_CONCURRENT_BIN_HEAP);
    return heap;
}

/**
 * @brief Allows multiple concurrent push and pop. All operations are serialized.
 */
binHeap_t* newLockedBinHeap(ocrPolicyDomain_t *pd) {
    binHeap_t* heap = newBinHeap(pd, LOCKED_BIN_HEAP);
    return heap;
}
