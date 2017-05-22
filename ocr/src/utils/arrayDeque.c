/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#include "debug.h"

#include "ocr-errors.h"
#include "ocr-policy-domain.h"

#include "utils/arrayDeque.h"
#include "utils/deque.h"

typedef struct _arrayDequeWrapper_t {
    deque_t base;
    arrayDeque_t derived;
} arrayDequeWrapper_t;

#define DEBUG_TYPE UTIL

u8 arrayDequeInit(arrayDeque_t *deque, u32 chunkSize) {
    START_PROFILE(util_arrayDequeInit);
    ASSERT(deque);
    if(chunkSize <= 1 || (chunkSize & (chunkSize - 1)) != 0) {
        DPRINTF(DEBUG_LVL_WARN, "Chunksize for arrayDequeInit is not power of two or too small: %"PRIu32"\n", chunkSize);
        RETURN_PROFILE(OCR_EINVAL);
    }
    deque->chunkSize = chunkSize;
    deque->chunkCount = 0;
    deque->head = deque->tail = NULL;
    deque->headIdx = deque->tailIdx = 0;
    deque->isEmpty = true;

    RETURN_PROFILE(0);
}

u8 arrayDequeDestruct(arrayDeque_t *deque) {
    START_PROFILE(util_arrayDequeDestruct);
    ASSERT(deque);
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    arrayDequeChunk_t *chunk = deque->head;
    while(chunk) {
        arrayDequeChunk_t *t = chunk;
        chunk = chunk->next;
        pd->fcts.pdFree(pd, t);
    }

    RETURN_PROFILE(0);
}

u32 arrayDequeSize(arrayDeque_t *deque) {
    ASSERT(deque);
    ASSERT(deque->tailIdx >= deque->headIdx);
    // Note the weird !(deque->isEmpty). This is because we recycle indexes
    // (they do not just "grow") and we therefore need to be able to distinguish
    // if head and tail are both 0 if that means that the queue is empty
    // or just contains one element (at index 0). If isEmpty is true, the
    // queue is empty and the following line will return 0 (tailIdx = headIdx = 0).
    // For a queue with just one element, the indices will be of the same
    // value but the isEmpty flag will push the returned value to 1.
    return deque->tailIdx - deque->headIdx + !(deque->isEmpty);
}

u32 arrayDequeCapacity(arrayDeque_t *deque) {
    ASSERT(deque);
    return deque->chunkCount*deque->chunkSize;
}

u8 arrayDequePushAtTail(arrayDeque_t* deque, void* entry) {
    START_PROFILE(util_arrayDequePushAtTail);
    ASSERT(deque);
    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: pushing at tail %p\n", deque, entry);
    arrayDequeChunk_t *oldTail = deque->tail;
    u32 newIdx = deque->tailIdx + !deque->isEmpty; // If deque is empty, use the tailIdx since that
                                                   // element is not valid. Otherwise, use the next one
    // Determine if we should use the current tail or the next one
    DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: insertion IDX will be %"PRIu32"\n", deque, newIdx);
    if(newIdx && ((newIdx & (deque->chunkSize - 1)) == 0)) {
        DPRINTF(DEBUG_LVL_VVERB, "ArrayDeque %p: Going to next tail (%p to %p)\n", deque,
                deque->tail, deque->tail->next);
        // This means that we should move to the next "tail". For example, if
        // chunk-size is 4, this will be true for 4, 8, 12, etc.
        // This may be non-NULL if we have a padding tail already ready to go
        deque->tail = deque->tail->next;
    }

    // Do we have a tail (may not if we need to expand due to previous line)
    if(deque->tail == NULL) {
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        deque->tail = (arrayDequeChunk_t*)pd->fcts.pdMalloc(pd, sizeof(arrayDequeChunk_t) + sizeof(void*)*deque->chunkSize);
        ADebug(AllocDebugAllPD, "arrayDeque.c/arrayDequePushAtTail() "
               "pdMalloc(%ld), addr=%p\n",
               (sizeof(arrayDequeChunk_t) + sizeof(void*)*deque->chunkSize), deque->tail);
        if(deque->tail == NULL) {
            DPRINTF(DEBUG_LVL_WARN, "Not enough memory to allocate new chunk in array-deque\n");
            RETURN_PROFILE(OCR_ENOMEM);
        }
        ++deque->chunkCount;
        DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: creating new chunk at tail (%p). Count is now %"PRIu32"\n",
                deque, deque->tail, deque->chunkCount);
        // Set up the links properly
        deque->tail->prev = oldTail;
        if(oldTail != NULL)
            oldTail->next = deque->tail;
        deque->tail->next = NULL;

        // Point data to the right place
        deque->tail->data = (void**)((char*)deque->tail + sizeof(arrayDequeChunk_t));

        // In case we didn't have a head (first push)
        if(deque->isEmpty) {
            deque->head = deque->tail;
        }
    }

    // Set the entry properly
    // The & operation is a modulo.
    deque->tail->data[newIdx & (deque->chunkSize - 1)] = entry;
    deque->tailIdx = newIdx;
    deque->isEmpty = false;
    RETURN_PROFILE(0);
}

u8 arrayDequePopFromTail(arrayDeque_t* deque, void** entry) {
    START_PROFILE(util_arrayDequePopFromTail);
    ASSERT(deque);
    ASSERT(entry);

    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: attempting to pop from tail\n", deque);
    if(deque->isEmpty) {
        // Empty deque
        DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: queue is empty\n", deque);
        RETURN_PROFILE(OCR_EAGAIN);
    }

    ASSERT(deque->tail);
    *entry = deque->tail->data[deque->tailIdx & (deque->chunkSize - 1)];
    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: element %p popped\n", deque, *entry);

    // We now determine if we need to remove the tail element
    if(deque->tailIdx == deque->headIdx) {
        // We only had one element so we leave things as is but set isEmpty
        deque->isEmpty = true;
        RETURN_PROFILE(0);
    }

    // If we have more than one element, tailIdx is always > 0
    ASSERT(deque->tailIdx > 0);
    arrayDequeChunk_t *victimChunk = NULL;
    if((deque->tailIdx & (deque->chunkSize - 1)) == 0) {
        // This means, for a size of 4 for example, that we are at 4, 8, 12, etc so the start of a chunk
        // Removing this element means the chunk can now go away
        // Instead of always removing it, we always keep "padding"
        if(deque->tail->next != NULL) {
            // We have a padding of two chunks now so we can remove one
            victimChunk = deque->tail->next;
            deque->tail->next = NULL;
            DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: victim chunk %p\n", deque, victimChunk);
        }
        ASSERT(deque->tail->prev); // Should exist since tailIdx >= chunkSize
        // Move to the previous tail
        deque->tail = deque->tail->prev;
    }
    --deque->tailIdx;

    if(victimChunk) {
        // Check if we need padding up front
        if(deque->head->prev == NULL) {
            // We will add the victim to the front since we have
            // less than half the chunk left over in front and we have no additional
            // padding
            DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: moving victim chunk %p to head\n", deque,
                    victimChunk);
            victimChunk->prev = NULL;
            victimChunk->next = deque->head;
            deque->head->prev = victimChunk;
            // We do not increment the indices or change the head. It's just
            // there, lurking in case we need to expand
        } else {
            // We remove the chunk
            ocrPolicyDomain_t *pd = NULL;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            pd->fcts.pdFree(pd, victimChunk);
            --deque->chunkCount;
            DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: deleting victim chunk %p; count is now %"PRIu32"\n", deque,
                    victimChunk, deque->chunkCount);
        }
    }
    RETURN_PROFILE(0);
}

u8 arrayDequePeekFromTail(arrayDeque_t* deque, void** entry) {
    START_PROFILE(util_arrayDequePeekFromTail);
    ASSERT(deque);
    ASSERT(entry);

    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: attempting to peek from tail\n", deque);
    if(deque->isEmpty) {
        // Empty deque
        DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: queue is empty\n", deque);
        RETURN_PROFILE(OCR_EAGAIN);
    }

    ASSERT(deque->tail);
    *entry = deque->tail->data[deque->tailIdx & (deque->chunkSize - 1)];
    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: element %p peeked\n", deque, *entry);

    RETURN_PROFILE(0);
}

u8 arrayDequePushAtHead(arrayDeque_t* deque, void* entry) {
    START_PROFILE(util_arrayDequePushAtHead);
    ASSERT(deque);
    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: pushing at head %p\n", deque, entry);
    arrayDequeChunk_t *oldHead = deque->head;
    // Determine if we should use the current head or the next one
    u32 newIdx = deque->headIdx - !deque->isEmpty; // Same as for the pushAtTail case. Use headIdx
                                                   // if empty since that element is invalid. Else use
                                                   // the previous one
    DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: insertion IDX will be %"PRIu32"\n", deque, newIdx);
    if(newIdx == (u32)-1) {
        // This means that we should move to the previous "head"
        if(deque->head != NULL) {
            // If we already have a head, check for the previous head
            // This can be non-NULL if we had a padding chunk ready to go
            DPRINTF(DEBUG_LVL_VVERB, "ArrayDeque %p: Going to previous head (%p to %p)\n", deque,
                    deque->head, deque->head->prev);
            deque->head = deque->head->prev;
        }
    }

    // Do we have a head (may not if we need to expand due to previous line)
    if(deque->head == NULL) {
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        deque->head = (arrayDequeChunk_t*)pd->fcts.pdMalloc(pd, sizeof(arrayDequeChunk_t) + sizeof(void*)*deque->chunkSize);
        ADebug(AllocDebugAllPD, "arrayDeque.c/arrayDequePushAtHead() "
               "pdMalloc(%ld), addr=%p\n",
               (sizeof(arrayDequeChunk_t) + sizeof(void*)*deque->chunkSize), deque->head);
        if(deque->head == NULL) {
            DPRINTF(DEBUG_LVL_WARN, "Not enough memory to allocate new chunk in array-deque\n");
            RETURN_PROFILE(OCR_ENOMEM);
        }
        ++deque->chunkCount;
        DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: creating new chunk at head (%p). Count is now %"PRIu32"\n",
                deque, deque->head, deque->chunkCount);
        // Set up the links properly
        deque->head->prev = NULL;
        deque->head->next = oldHead;
        if(oldHead != NULL)
            oldHead->prev = deque->head;

        // Point data to the right place
        deque->head->data = (void**)((char*)deque->head + sizeof(arrayDequeChunk_t));

        // In case we didn't have a tail (first push)
        if(deque->isEmpty) {
            deque->tail = deque->head;
        }
    }

    // Shift all indices if we added a new chunk (or are using
    // a chunk that was there for padding) except if this is the first
    // creation
    if(deque->isEmpty) {
        newIdx = 0;
    } else if(oldHead != deque->head) {
        // headIdx will be updated when setting to newIdx
        deque->tailIdx += deque->chunkSize;
        newIdx += deque->chunkSize;
    }

    // Set the entry properly
    deque->head->data[newIdx & (deque->chunkSize - 1)] = entry;
    deque->headIdx = newIdx;
    deque->isEmpty = false;
    RETURN_PROFILE(0);
}

u8 arrayDequePopFromHead(arrayDeque_t* deque, void** entry) {
    START_PROFILE(util_arrayDequePopFromHead);
    ASSERT(deque);
    ASSERT(entry);

    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: attempting to pop from head\n", deque);
    if(deque->isEmpty) {
        // Empty deque
        DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: queue is empty\n", deque);
        RETURN_PROFILE(OCR_EAGAIN);
    }

    ASSERT(deque->head);
    *entry = deque->head->data[deque->headIdx & (deque->chunkSize - 1)];
    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: element %p popped\n", deque, *entry);

    // We now determine if we need to remove the head element
    if(deque->headIdx == deque->tailIdx) {
        // We only had one element so we leave things as is but set isEmpty
        deque->isEmpty = true;
        RETURN_PROFILE(0);
    }

    arrayDequeChunk_t *victimChunk = NULL;
    if(deque->headIdx == (deque->chunkSize - 1)) {
        // This means, for a size of 4 for example, that we are at 3 so at the end of the head chunk
        // Removing this element means the chunk can now go away
        // Instead of always removing it, we always keep "padding"
        if(deque->head->prev != NULL) {
            // We have a padding of two chunks now so we can remove one
            victimChunk = deque->head->prev;
            deque->head->prev = NULL;
            DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: victim chunk %p\n", deque, victimChunk);
        }
        ASSERT(deque->head->next); // Should exist since we are not empty after
                                   // removing the element so tailIdx has to be
                                   // > headIdx and in the next chunk
        deque->head = deque->head->next;

        // In all cases, we update our counters to reflect the fact that we now have
        // one less chunk up in front
        // This puts headIdx to -1 but will be increased to 0 after the if
        deque->headIdx -= deque->chunkSize;
        deque->tailIdx -= deque->chunkSize;
    }
    ++deque->headIdx;

    if(victimChunk) {
        // Check if we need padding in the back
        if(deque->tail->next == NULL) {
            // We will add the victim to the back since we have
            // no additional padding at the end
            DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: moving victim chunk %p to tail\n", deque,
                    victimChunk);
            victimChunk->prev = deque->tail;
            victimChunk->next = NULL;
            deque->tail->next = victimChunk;
        } else {
            // We remove the chunk
            ocrPolicyDomain_t *pd = NULL;
            getCurrentEnv(&pd, NULL, NULL, NULL);
            pd->fcts.pdFree(pd, victimChunk);
            --deque->chunkCount;
            DPRINTF(DEBUG_LVL_VERB, "ArrayDeque %p: deleting victim chunk %p; count is now %"PRIu32"\n", deque,
                    victimChunk, deque->chunkCount);
        }
    }
    RETURN_PROFILE(0);
}

u8 arrayDequePeekFromHead(arrayDeque_t* deque, void** entry) {
    START_PROFILE(util_arrayDequePopFromHead);
    ASSERT(deque);
    ASSERT(entry);

    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: attempting to peek from head\n", deque);
    if(deque->isEmpty) {
        // Empty deque
        DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: queue is empty\n", deque);
        RETURN_PROFILE(OCR_EAGAIN);
    }

    ASSERT(deque->head);
    *entry = deque->head->data[deque->headIdx & (deque->chunkSize - 1)];
    DPRINTF(DEBUG_LVL_INFO, "ArrayDeque %p: element %p peeked\n", deque, *entry);

    RETURN_PROFILE(0);
}


/**
 * @brief Wrapper function for size
 */
static u32 adWsize(struct _ocrDeque_t *self) {
    arrayDequeWrapper_t *rself = (arrayDequeWrapper_t*)self;
    arrayDeque_t * dself = (arrayDeque_t*)(&(rself->derived));
    hal_lock(&(self->lock));
    u32 cnt = arrayDequeSize(dself);
    hal_unlock(&(self->lock));
    return cnt;
}

/**
 * @brief Wrapper function for destruct
 */
static void adWdestruct(ocrPolicyDomain_t *pd, struct _ocrDeque_t *self) {
    arrayDequeWrapper_t *rself = (arrayDequeWrapper_t*)self;
    arrayDeque_t * dself = (arrayDeque_t*)(&(rself->derived));
    hal_lock(&(self->lock));
    RESULT_ASSERT(arrayDequeDestruct(dself), ==, 0);
    hal_unlock(&(self->lock));
    pd->fcts.pdFree(pd, self);
}

/**
 * @brief Wrapper function for push at tail
 */
static void adWpushAtTail(struct _ocrDeque_t *self, void* entry, u8 doTry) {
    arrayDequeWrapper_t *rself = (arrayDequeWrapper_t*)self;
    arrayDeque_t * dself = (arrayDeque_t*)(&(rself->derived));
    hal_lock(&(self->lock));
    RESULT_ASSERT(arrayDequePushAtTail(dself, entry), ==, 0);
    hal_unlock(&(self->lock));
}

/**
 * @brief Wrapper function for pop from tail
 */
static void* adWpopFromTail(struct _ocrDeque_t *self, u8 doTry) {
    arrayDequeWrapper_t *rself = (arrayDequeWrapper_t*)self;
    arrayDeque_t * dself = (arrayDeque_t*)(&(rself->derived));
    void * entry = NULL;
    u8 status = 0;
    hal_lock(&(self->lock));
    status = arrayDequePopFromTail(dself, &entry);
    hal_unlock(&(self->lock));
    if(status == OCR_EAGAIN)
        return NULL;
    ASSERT(status == 0);
    return entry;
}

/**
 * @brief Wrapper function for push at head
 */
static void adWpushAtHead(struct _ocrDeque_t *self, void* entry, u8 doTry) {
    arrayDequeWrapper_t *rself = (arrayDequeWrapper_t*)self;
    arrayDeque_t * dself = (arrayDeque_t*)(&(rself->derived));
    hal_lock(&(self->lock));
    RESULT_ASSERT(arrayDequePushAtHead(dself, entry), ==, 0);
    hal_unlock(&(self->lock));
}

/**
 * @brief Wrapper function for pop from head
 */
static void* adWpopFromHead(struct _ocrDeque_t *self, u8 doTry) {
    arrayDequeWrapper_t *rself = (arrayDequeWrapper_t*)self;
    arrayDeque_t * dself = (arrayDeque_t*)(&(rself->derived));
    void * entry = NULL;
    u8 status = 0;
    hal_lock(&(self->lock));
    status = arrayDequePopFromHead(dself, &entry);
    hal_unlock(&(self->lock));
    if(status == OCR_EAGAIN)
        return NULL;
    ASSERT(status == 0);
    return entry;
}

/**
 * @brief Allows multiple concurrent push and pop. All operations are serialized.
 */
deque_t* newArrayQueue(ocrPolicyDomain_t *pd, void * initValue) {
    arrayDequeWrapper_t * self = pd->fcts.pdMalloc(
        pd, sizeof(arrayDequeWrapper_t));
    ADebug(AllocDebugAllPD, "arrayDeque.c/newArrayQueue() "
           "pdMalloc(%ld), addr=%p\n", sizeof(arrayDequeWrapper_t), self);

    arrayDequeInit(&(self->derived), 4);
    self->base.lock = INIT_LOCK;
    self->base.head = 0;
    self->base.tail = 0;
    self->base.data = NULL;
    self->base.size = adWsize;
    self->base.destruct = adWdestruct;
    self->base.pushAtTail = adWpushAtTail;
    self->base.popFromTail = adWpopFromTail;
    self->base.pushAtHead = adWpushAtHead;
    self->base.popFromHead = adWpopFromHead;
    return (deque_t*) self;
}
