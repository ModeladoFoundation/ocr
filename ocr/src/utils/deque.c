/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"

#include "ocr-hal.h"
#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "utils/deque.h"
#include "utils/arrayDeque.h"

#define DEBUG_TYPE UTIL

/****************************************************/
/* DEQUE BASE IMPLEMENTATIONS                       */
/****************************************************/

/*
 * Deque destroy
 */
void dequeDestroy(ocrPolicyDomain_t *pd, deque_t* self) {
    pd->fcts.pdFree(pd, self->data);
    pd->fcts.pdFree(pd, self);
}

/*
 * Size of the deque (concurrent with other ops, may be inexact)
 */
u32 nonSyncDequeSize(deque_t* self) {
    s32 size = (self->tail - self->head);
    ASSERT(size >= 0);
    return ((u32) size);
}

/*
 * Size of the deque (concurrent with other ops, may be inexact)
 */
u32 nonSyncCircularDequeSize(deque_t* self) {
    s32 size = (self->tail - self->head);
    if (size < 0) {
        size = INIT_DEQUE_CAPACITY + size;
    }
    ASSERT(size >= 0 && size <= INIT_DEQUE_CAPACITY);
    return ((u32) size);
}

/*
 * Size of the deque (concurrent with other ops, may be inexact)
 */
u32 wstDequeSize(deque_t* self) {
    s32 size = (self->tail - self->head);
    // Work-stealing deque's pop tail implementation optimistically
    // decrement tail and then check if the queue is empty. Hence,
    // we can get a negative value here which we fixup.
    if (size < 0) {
        return 0;
    }
    return ((u32) size);
}

/*
 * @brief base constructor for deques. Think of this as a virtual class
 * where the function pointers to push and pop are set by the derived
 * implementation.
 */
static void baseDequeInit(deque_t* self, ocrPolicyDomain_t *pd, void * initValue) {
    self->head = 0;
    self->tail = 0;
    self->data = (volatile void **)pd->fcts.pdMalloc(pd, sizeof(void*)*INIT_DEQUE_CAPACITY);
    ADebug(AllocDebugAllPD, "deque.c/baseDequeInit() pdMalloc(%ld), addr=%p\n",
           (sizeof(void*)*INIT_DEQUE_CAPACITY), self->data);
    ASSERT(self->data != NULL);

    // This may not be necessary depending on the intented use
    u32 i=0;
    while(i < INIT_DEQUE_CAPACITY) {
        self->data[i] = initValue;
        ++i;
    }
    self->destruct = dequeDestroy;
    self->size = nonSyncDequeSize;
    // Set by derived implementation
    self->pushAtTail = NULL;
    self->popFromTail = NULL;
    self->pushAtHead = NULL;
    self->popFromHead = NULL;
}

static void singleLockedDequeInit(dequeSingleLocked_t* self, ocrPolicyDomain_t *pd, void * initValue) {
    baseDequeInit((deque_t*)self, pd, initValue);
    self->lock = INIT_LOCK;
}

static void dualLockedDequeInit(dequeDualLocked_t* self, ocrPolicyDomain_t *pd, void * initValue) {
    baseDequeInit((deque_t*)self, pd, initValue);
    self->lockH = INIT_LOCK;
    self->lockT = INIT_LOCK;
}

static deque_t * newBaseDeque(ocrPolicyDomain_t *pd, void * initValue, ocrDequeType_t type) {
    deque_t* self = NULL;
    switch(type) {
        case NO_LOCK_BASE_DEQUE:
            self = (deque_t*) pd->fcts.pdMalloc(pd, sizeof(deque_t));
            ADebug(AllocDebugAllPD, "deque.c/newBaseDeque()/NO_LOCK_BASE_DEQUE "
                   "pdMalloc(%ld), addr=%p\n", sizeof(deque_t), self);
            baseDequeInit(self, pd, initValue);
            // Warning: function pointers must be specialized in caller
            break;
        case SINGLE_LOCK_BASE_DEQUE:
            self = (deque_t*) pd->fcts.pdMalloc(pd, sizeof(dequeSingleLocked_t));
            ADebug(AllocDebugAllPD, "deque.c/newBaseDeque()/SINGLE_LOCK_BASE_DEQUE "
                   "pdMalloc(%ld), addr=%p\n", sizeof(dequeSingleLocked_t), self);
            singleLockedDequeInit((dequeSingleLocked_t*)self, pd, initValue);
            // Warning: function pointers must be specialized in caller
            break;
        case DUAL_LOCK_BASE_DEQUE:
            self = (deque_t*) pd->fcts.pdMalloc(pd, sizeof(dequeDualLocked_t));
            ADebug(AllocDebugAllPD, "deque.c/newBaseDeque()/DUAL_LOCK_BASE_DEQUE "
                   "pdMalloc(%ld), addr=%p\n", sizeof(dequeDualLocked_t), self);
            dualLockedDequeInit((dequeDualLocked_t*)self, pd, initValue);
            // Warning: function pointers must be specialized in caller
            break;
    default:
        ASSERT(0);
    }
    self->type = type;
    return self;
}

/****************************************************/
/* NON CONCURRENT DEQUE BASED OPERATIONS            */
/****************************************************/

/*
 * push an entry onto the tail of the deque
 */
void nonConcDequePushTailOverwrite(deque_t* self, void* entry, u8 doTry) {
    u32 head = self->head;
    u32 tail = self->tail;
    if (tail == INIT_DEQUE_CAPACITY + head) { /* deque looks full */
        ++(self->head); // Make room
        hal_fence();
    }
    u32 n = (self->tail) % INIT_DEQUE_CAPACITY;
    self->data[n] = entry;
    ++(self->tail);
}

/*
 * push an entry onto the tail of the deque
 */
void nonConcDequePushTail(deque_t* self, void* entry, u8 doTry) {
    u32 head = self->head;
    u32 tail = self->tail;
    /* deque looks full - may not grow the deque if some interleaving steal occur */
    ASSERT_CRITICAL("DEQUE full, increase deque's size" && !(tail == INIT_DEQUE_CAPACITY + head));
    u32 n = (self->tail) % INIT_DEQUE_CAPACITY;
    self->data[n] = entry;
    ++(self->tail);
}

/*
 * pop the task out of the deque from the tail
 */
void * nonConcDequePopTail(deque_t * self, u8 doTry) {
    ASSERT(self->tail >= self->head);
    if (self->tail == self->head)
        return NULL;
    --(self->tail);
    void * rt = (void*) self->data[(self->tail) % INIT_DEQUE_CAPACITY];
    return rt;
}

/*
 *  pop the task out of the deque from the head
 */
void * nonConcDequePopHead(deque_t * self, u8 doTry) {
    ASSERT(self->tail >= self->head);
    if (self->tail == self->head)
        return NULL;
    void * rt = (void*) self->data[(self->head) % INIT_DEQUE_CAPACITY];
    ++(self->head);
    return rt;
}

/****************************************************/
/* CONCURRENT DEQUE BASED OPERATIONS                */
/****************************************************/

/*
 * push an entry onto the tail of the deque
 */
void wstDequePushTail(deque_t* self, void* entry, u8 doTry) {
    s32 head = self->head;
    s32 tail = self->tail;
    /* deque looks full - may not grow the deque if some interleaving steal occur */
    ASSERT_CRITICAL("DEQUE full, increase deque's size" && !(tail == INIT_DEQUE_CAPACITY + head));
    s32 n = (self->tail) % INIT_DEQUE_CAPACITY;
    self->data[n] = entry;
    DPRINTF(DEBUG_LVL_VERB, "Pushing h:%"PRId32" t:%"PRId32" deq[%"PRId32"] elt:0x%p into conc deque @ 0x%p\n",
            head, self->tail, n, entry, self);
    hal_fence();
    ++(self->tail);
}

/*
 * pop the task out of the deque from the tail
 */
void * wstDequePopTail(deque_t * self, u8 doTry) {
    hal_fence();
    s32 tail = self->tail;
    --tail;
    self->tail = tail;
    hal_fence();
    s32 head = self->head;

    if (tail < head) {
        self->tail = self->head;
        return NULL;
    }
    void * rt = (void*) self->data[(tail) % INIT_DEQUE_CAPACITY];

    if (tail > head) {
        DPRINTF(DEBUG_LVL_VERB, "Popping (tail) h:%"PRId32" t:%"PRId32" deq[%"PRId32"] elt:0x%"PRIx64" from conc deque @ 0x%"PRIx64"\n",
                head, tail, tail % INIT_DEQUE_CAPACITY, (u64)rt, (u64)self);
        return rt;
    }

    /* now size == 1, I need to compete with the thieves */
    if (hal_cmpswap32(&self->head, head, head + 1) != head)
        rt = NULL; /* losing in competition */

    /* now the deque is empty */
    self->tail = self->head;
    DPRINTF(DEBUG_LVL_VERB, "Popping (tail 2) h:%"PRId32" t:%"PRId32" deq[%"PRId32"] elt:0x%"PRIx64" from conc deque @ 0x%"PRIx64"\n",
            head, tail, tail % INIT_DEQUE_CAPACITY, (u64)rt, (u64)self);
    return rt;
}

/*
 * the steal protocol
 */
void * wstDequePopHead(deque_t * self, u8 doTry) {
    s32 head, tail;
    do {
        head = self->head;
        hal_fence();
        tail = self->tail;
        if (tail <= head) {
            return NULL;
        }

        // The data must be read here, BEFORE the cas succeeds.
        // If the tail wraps around the buffer, so that H=x and T=H+N
        // as soon as the steal has done the cas, a push could happen
        // at index 'x' and overwrite the value to be stolen.

        void * rt = (void *) self->data[head % INIT_DEQUE_CAPACITY];

        /* compete with other thieves and possibly the owner (if the size == 1) */
        if (hal_cmpswap32(&self->head, head, head + 1) == head) { /* competing */
            DPRINTF(DEBUG_LVL_VERB, "Popping (head) h:%"PRId32" t:%"PRId32" deq[%"PRId32"] elt:0x%"PRIx64" from conc deque @ 0x%"PRIx64"\n",
                     head, tail, head % INIT_DEQUE_CAPACITY, (u64)rt, (u64)self);
            return rt;
        }
    } while (doTry == 0);
    return NULL;
}

/******************************************************/
/* SINGLE LOCKED DEQUE BASED OPERATIONS               */
/******************************************************/

/*
 * Push an entry onto the tail of the deque
 * This operation locks the whole deque.
 */
void lockedDequePushTail(deque_t* self, void* entry, u8 doTry) {
    dequeSingleLocked_t* dself = (dequeSingleLocked_t*)self;
    hal_lock(&dself->lock);
    u32 head = self->head;
    u32 tail = self->tail;
    /* deque looks full - may not grow the deque if some interleaving steal occur */
    ASSERT_CRITICAL("DEQUE full, increase deque's size" && !(tail == INIT_DEQUE_CAPACITY + head));
    u32 n = (self->tail) % INIT_DEQUE_CAPACITY;
    self->data[n] = entry;
    ++(self->tail);
    hal_unlock(&dself->lock);
}

/*
 * Pop the task out of the deque from the tail
 * This operation locks the whole deque.
 */
void * lockedDequePopTail(deque_t * self, u8 doTry) {
    dequeSingleLocked_t* dself = (dequeSingleLocked_t*)self;
    hal_lock(&dself->lock);
    ASSERT(self->tail >= self->head);
    if (self->tail == self->head) {
        hal_unlock(&dself->lock);
        return NULL;
    }
    --(self->tail);
    void * rt = (void*) self->data[(self->tail) % INIT_DEQUE_CAPACITY];
    hal_unlock(&dself->lock);
    return rt;
}

/*
 * Push an entry onto the tail of the deque
 * This operation locks the whole deque.
 */
void lockedDequePushHead(deque_t* self, void* entry, u8 doTry) {
    dequeSingleLocked_t* dself = (dequeSingleLocked_t*)self;
    hal_lock(&dself->lock);
    u32 head = self->head;
    u32 tail = self->tail;
    /* deque looks full - may not grow the deque if some interleaving steal occur */
    ASSERT_CRITICAL("DEQUE full, increase deque's size" && !(tail == INIT_DEQUE_CAPACITY + head));
    // Not full so I must be able to write
    u32 n;
    if (head == tail) { // empty
        ++(self->tail);
        n = head % INIT_DEQUE_CAPACITY;
    } else if (head == 0) { // no space at head, need to shift
        hal_memMove(&self->data[1], self->data, (tail-head) * sizeof(void *), false);
        self->tail++;
        n = 0;
    } else { // ok to just prepend
        --(self->head);
        n = (head-1) % INIT_DEQUE_CAPACITY;
    }
    self->data[n] = entry;
    hal_unlock(&dself->lock);
}


/*
 * Pop the task out of the deque from the head
 * This operation locks the whole deque.
 */
void * lockedDequePopHead(deque_t * self, u8 doTry) {
    dequeSingleLocked_t* dself = (dequeSingleLocked_t*)self;
    hal_lock(&dself->lock);
    ASSERT(self->tail >= self->head);
    if (self->tail == self->head) {
        hal_unlock(&dself->lock);
        return NULL;
    }
    void * rt = (void*) self->data[(self->head) % INIT_DEQUE_CAPACITY];
    ++(self->head);
    hal_unlock(&dself->lock);
    return rt;
}


/*
 * Push an entry onto the tail of the deque
 * This operation locks the whole deque.
 */
void lockedDequePushTailSemiConc(deque_t* self, void* entry, u8 doTry) {
    dequeSingleLocked_t* dself = (dequeSingleLocked_t*)self;
    ASSERT(entry != NULL);
    hal_lock(&dself->lock);
    u32 head = self->head;
    u32 tail = ((u32)self->tail);
    u32 ptail = (tail == (INIT_DEQUE_CAPACITY-1)) ? 0 : tail+1;
    ASSERT_CRITICAL("DEQUE full, increase deque's size" && !(ptail == head));
    self->data[tail] = entry;
    // The fence ensures a concurrent head pop cannot see
    // self->tail increased without seeing the entry being written
    hal_fence();
    self->tail = ptail;
    hal_unlock(&dself->lock);
}

/*
 *  pop the task out of the deque from the head
 */
void * nonConcDequePopHeadSemiConc(deque_t * self, u8 doTry) {
    u32 head = self->head;
    u32 tail = self->tail;
    if (head == tail) {
        return NULL;
    }
    void * rt = (void*) self->data[head];
    ASSERT(rt != NULL);
#ifdef OCR_ASSERT
    self->data[head] = NULL; // DEBUG
    hal_fence();
#endif
    self->head = ((head == (INIT_DEQUE_CAPACITY-1)) ? 0 : head+1);
    return rt;
}

/******************************************************/
/* DEQUE CONSTRUCTORS                                 */
/******************************************************/

/*
 * @brief Deque constructor. For a given type, create an instance and
 * initialize its base type.
 */
deque_t * newDeque(ocrPolicyDomain_t *pd, void * initValue, ocrDequeType_t type) {
    deque_t* self = NULL;
    switch(type) {
    case WORK_STEALING_DEQUE:
        self = newBaseDeque(pd, initValue, NO_LOCK_BASE_DEQUE);
        // Specialize push/pop implementations
        self->size = wstDequeSize;
        self->pushAtTail = wstDequePushTail;
        self->popFromTail = wstDequePopTail;
        self->pushAtHead = NULL;
        self->popFromHead = wstDequePopHead;
        break;
    case NON_CONCURRENT_DEQUE:
        self = newBaseDeque(pd, initValue, NO_LOCK_BASE_DEQUE);
        // Specialize push/pop implementations
        self->pushAtTail = nonConcDequePushTail;
        self->popFromTail = nonConcDequePopTail;
        self->pushAtHead = NULL;
        self->popFromHead = nonConcDequePopHead;
        break;
    case NON_CONCURRENT_OVERWRITE_DEQUE:
        self = newBaseDeque(pd, initValue, NO_LOCK_BASE_DEQUE);
        // Specialize push/pop implementations
        self->pushAtTail = nonConcDequePushTailOverwrite;
        self->popFromTail = NULL;
        self->pushAtHead = NULL;
        self->popFromHead = nonConcDequePopHead;
        break;
    case SEMI_CONCURRENT_DEQUE:
        self = newBaseDeque(pd, initValue, SINGLE_LOCK_BASE_DEQUE);
        self->size = nonSyncCircularDequeSize;
        // Specialize push/pop implementations
        self->pushAtTail = lockedDequePushTailSemiConc;
        self->popFromTail = NULL;
        self->pushAtHead = NULL;
        self->popFromHead = nonConcDequePopHeadSemiConc;
        break;
    case LOCKED_DEQUE:
        self = newBaseDeque(pd, initValue, SINGLE_LOCK_BASE_DEQUE);
        // Specialize push/pop implementations
        self->pushAtTail =  lockedDequePushTail;
        self->popFromTail = lockedDequePopTail;
        self->pushAtHead = lockedDequePushHead;
        self->popFromHead = lockedDequePopHead;
        break;
    case ARRAY_DEQUE:
        self = newArrayQueue(pd, initValue);
        break;
    default:
        ASSERT(0);
    }
    self->type = type;
    return self;
}

/**
 * @brief The workstealing deque is a concurrent deque that supports
 * push at the tail, and pop from either tail or head. Popping from the
 * head is usually called a steal.
 */
deque_t* newWorkStealingDeque(ocrPolicyDomain_t *pd, void * initValue) {
    deque_t* self = newDeque(pd, initValue, WORK_STEALING_DEQUE);
    return self;
}

/**
 * @brief Unsynchronized implementation for push and pop
 */
deque_t* newNonConcurrentQueue(ocrPolicyDomain_t *pd, void * initValue) {
    deque_t* self = newDeque(pd, initValue, NON_CONCURRENT_DEQUE);
    return self;
}

/**
 * @brief Allows multiple concurrent push at the tail (a single succeed
 at a time, others fail and need to retry). Pop from head are not synchronized.
 */
deque_t* newSemiConcurrentQueue(ocrPolicyDomain_t *pd, void * initValue) {
    deque_t* self = newDeque(pd, initValue, SEMI_CONCURRENT_DEQUE);
    return self;
}

/**
 * @brief Allows multiple concurrent push at the tail (a single succeed
 at a time, others fail and need to retry). Pop from head are not synchronized.
 * If full, overwrites oldest value. Useful for perfmon where inaccuracy
 * is acceptable.
 */
deque_t* newNonConcurrentOverwriteQueue(ocrPolicyDomain_t *pd, void * initValue) {
    deque_t* self = newDeque(pd, initValue, NON_CONCURRENT_OVERWRITE_DEQUE);
    return self;
}

/**
 * @brief Allows multiple concurrent push and pop. All operations are serialized.
 */
deque_t* newLockedQueue(ocrPolicyDomain_t *pd, void * initValue) {
    deque_t* self = newDeque(pd, initValue, LOCKED_DEQUE);
    return self;
}


