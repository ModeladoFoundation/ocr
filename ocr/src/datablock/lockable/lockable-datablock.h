/**
 * @brief Simple data-block implementation.
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __DATABLOCK_LOCKABLE_H__
#define __DATABLOCK_LOCKABLE_H__

#include "ocr-config.h"
#ifdef ENABLE_DATABLOCK_LOCKABLE

#include "ocr-allocator.h"
#include "ocr-datablock.h"
#include "ocr-hal.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"

#ifdef ENABLE_HINTS
/**< The number of hint properties supported by this implementation */
#define OCR_HINT_COUNT_DB_LOCKABLE   1
#else
#define OCR_HINT_COUNT_DB_LOCKABLE   0
#endif

typedef struct {
    ocrDataBlockFactory_t base;
} ocrDataBlockFactoryLockable_t;

typedef union {
    struct {
        u64 flags : 16;
        u64 numUsers : 15;
        u64 internalUsers : 15;
        u64 freeRequested: 1;
        u64 modeLock : 2;
        u64 singleAssign : 1;
#ifdef OCR_HACK_DB_MOVE
        u64 isMoving : 1;       /**< Data-block is in the process of moving */
        u64 needsEvict: 1;      /**< Data-block needs to be evicted out whenever possible */
        u64 _padding : 11;
#else
        u64 _padding : 13;
#endif
    };
    u64 data;
} ocrDataBlockLockableAttr_t;

// Declared in .c
struct dbWaiter_t;

typedef struct _ocrDataBlockLockable_t {
    ocrDataBlock_t base;

    /* Data for the data-block */
    lock_t lock; /**< Lock for this data-block */
    ocrDataBlockLockableAttr_t attributes; /**< Attributes for this data-block */

    struct _dbWaiter_t * ewWaiterList;  /**< EDTs waiting for exclusive write access */
    struct _dbWaiter_t * itwWaiterList; /**< EDTs waiting for intent-to-write access */
    struct _dbWaiter_t * roWaiterList;  /**< EDTs waiting for read only access */
#ifdef OCR_HACK_DB_MOVE
    struct _dbWaiter_t *otherWaiterList;      /**< EDTs waiting because the DB is being moved (no mode restrictions) */
#endif
    ocrLocation_t itwLocation;
    ocrWorker_t * worker; /**< worker currently owning the DB internal lock */
    ocrRuntimeHint_t hint;
#ifdef ENABLE_RESILIENCY
    u32 ewWaiterCount, itwWaiterCount, roWaiterCount;
#ifdef OCR_HACK_DB_MOVE
    u32 otherWaiterCount;
#endif /* OCR_HACK_DB_MOVE */
#endif /* ENABLE_RESILIENCY */
#ifdef OCR_HACK_DB_MOVE
    void* originalPtr;  /**< Pointer to the original location of this data-block (copied back here after moving) */
    ocrLocation_t acquireLocation; /**< Used to determine when to evict (when this guy releases) */
#endif
} ocrDataBlockLockable_t;

extern ocrDataBlockFactory_t* newDataBlockFactoryLockable(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_DATABLOCK_LOCKABLE */
#endif /* __DATABLOCK_LOCKABLE_H__ */
