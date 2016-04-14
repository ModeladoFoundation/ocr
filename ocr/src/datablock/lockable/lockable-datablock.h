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
        u64 _padding : 1;
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
    ocrLocation_t itwLocation;
    ocrWorker_t * worker; /**< worker currently owning the DB internal lock */
    ocrRuntimeHint_t hint;
} ocrDataBlockLockable_t;

extern ocrDataBlockFactory_t* newDataBlockFactoryLockable(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_DATABLOCK_LOCKABLE */
#endif /* __DATABLOCK_LOCKABLE_H__ */
