/**
 * @brief Simple data-block implementation.
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __DATABLOCK_REGULAR_H__
#define __DATABLOCK_REGULAR_H__

#include "ocr-config.h"
#ifdef ENABLE_DATABLOCK_REGULAR

#include "ocr-allocator.h"
#include "ocr-datablock.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"

#ifdef ENABLE_HINTS
/**< The number of hint properties supported by this implementation */
#define OCR_HINT_COUNT_DB_REGULAR   1
#else
#define OCR_HINT_COUNT_DB_REGULAR   0
#endif

typedef struct {
    ocrDataBlockFactory_t base;
} ocrDataBlockFactoryRegular_t;

typedef union {
    struct {
        volatile u64 flags : 16;
        volatile u64 numUsers : 15;
        volatile u64 internalUsers : 15;
        volatile u64 freeRequested: 1;
        volatile u64 singleAssign : 1;
        u64 _padding : 16;
    };
    u64 data;
} ocrDataBlockRegularAttr_t;

typedef struct _ocrDataBlockRegular_t {
    ocrDataBlock_t base; /* Data for the data-block */
    lock_t lock; /**< Lock for this data-block */
    ocrDataBlockRegularAttr_t attributes; /**< Attributes for this data-block */
    ocrRuntimeHint_t hint;
} ocrDataBlockRegular_t;

extern ocrDataBlockFactory_t* newDataBlockFactoryRegular(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_DATABLOCK_REGULAR */
#endif /* __DATABLOCK_REGULAR_H__ */
