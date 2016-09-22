// OCR Runtime Hints

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef OCR_RUNTIME_HINT_H_
#define OCR_RUNTIME_HINT_H_

#include "ocr-types.h"
#include "ocr-runtime-types.h"
#include "utils/ocr-utils.h"

extern u64 ocrHintPropIndexStart[];
extern u64 ocrHintPropIndexEnd[];

/****************************************************/
/* OCR USER HINTS UTILS                             */
/****************************************************/
#define OCR_HINT_INDX(property, type)       (property - ocrHintPropIndexStart[type] - 1)
#define OCR_HINT_BIT_MASK(hint, property)   (0x1 << OCR_HINT_INDX(property, hint->type))
#define OCR_HINT_FIELD(hint, property)      ((u64*)(&(hint->args)))[OCR_HINT_INDX(property, hint->type)]

#define OCR_HINT_SETUP(map, props, count, _start, _end)             \
do {                                                                \
    u32 i;                                                          \
    for (i = 0; i < (_end - _start - 1); i++) {                     \
        map[i] = ((u64)-1);                                         \
    }                                                               \
    for (i = 0; i < count; i++) {                                   \
        u64 index = props[i] - _start - 1;                          \
        map[index] = i;                                             \
    }                                                               \
} while(0);

/****************************************************/
/* OCR RUNTIME HINTS                                */
/****************************************************/

typedef struct {
    u64 hintMask;   /**< Refer below for mask format details */
    u64 *hintVal;   /**< The array to hold supported hint values */
} ocrRuntimeHint_t;

/****************************************************/
/* OCR RUNTIME HINTS UTILS                          */
/****************************************************/
/**
 * OCR object hint mask format:
 * 52 bits ( 0 - 51) : Bit mask for hint properties
 *  6 bits (52 - 57) : Number of hints that are set
 *  3 bits (58 - 60) : Factory Id of the hint type
 *  3 bits (61 - 63) : Hint type
 **/
#define OCR_RUNTIME_HINT_PROP_BITS 52
#define OCR_RUNTIME_HINT_SIZE_BITS 6
#define OCR_RUNTIME_HINT_FACT_BITS 3
#define OCR_RUNTIME_HINT_TYPE_BITS 3

#define OCR_RUNTIME_HINT_PROP_OFFSET (0)
#define OCR_RUNTIME_HINT_SIZE_OFFSET (OCR_RUNTIME_HINT_PROP_OFFSET + OCR_RUNTIME_HINT_PROP_BITS)
#define OCR_RUNTIME_HINT_FACT_OFFSET (OCR_RUNTIME_HINT_SIZE_OFFSET + OCR_RUNTIME_HINT_SIZE_BITS)
#define OCR_RUNTIME_HINT_TYPE_OFFSET (OCR_RUNTIME_HINT_FACT_OFFSET + OCR_RUNTIME_HINT_FACT_BITS)

#define OCR_RUNTIME_HINT_PROP_MASK ((~(((u64)-1) << OCR_RUNTIME_HINT_PROP_BITS)) << OCR_RUNTIME_HINT_PROP_OFFSET)
#define OCR_RUNTIME_HINT_SIZE_MASK ((~(((u64)-1) << OCR_RUNTIME_HINT_SIZE_BITS)) << OCR_RUNTIME_HINT_SIZE_OFFSET)
#define OCR_RUNTIME_HINT_FACT_MASK ((~(((u64)-1) << OCR_RUNTIME_HINT_FACT_BITS)) << OCR_RUNTIME_HINT_FACT_OFFSET)
#define OCR_RUNTIME_HINT_TYPE_MASK ((~(((u64)-1) << OCR_RUNTIME_HINT_TYPE_BITS)) << OCR_RUNTIME_HINT_TYPE_OFFSET)

#define OCR_RUNTIME_HINT_MASK_INIT(_mask, _type, _fact)                                                         \
do {                                                                                                            \
    ASSERT(((u64)_type) < (0x1UL << OCR_RUNTIME_HINT_TYPE_BITS));                                               \
    ASSERT(((u64)_fact) < (0x1UL << OCR_RUNTIME_HINT_FACT_BITS));                                               \
    _mask = (((u64)_type) << OCR_RUNTIME_HINT_TYPE_OFFSET) | (((u64)_fact) << OCR_RUNTIME_HINT_FACT_OFFSET);    \
} while(0);

#define OCR_RUNTIME_HINT_GET_PROP(_mask) ((_mask & OCR_RUNTIME_HINT_PROP_MASK) >> OCR_RUNTIME_HINT_PROP_OFFSET)
#define OCR_RUNTIME_HINT_GET_SIZE(_mask) ((_mask & OCR_RUNTIME_HINT_SIZE_MASK) >> OCR_RUNTIME_HINT_SIZE_OFFSET)
#define OCR_RUNTIME_HINT_GET_FACT(_mask) ((_mask & OCR_RUNTIME_HINT_FACT_MASK) >> OCR_RUNTIME_HINT_FACT_OFFSET)
#define OCR_RUNTIME_HINT_GET_TYPE(_mask) ((_mask & OCR_RUNTIME_HINT_TYPE_MASK) >> OCR_RUNTIME_HINT_TYPE_OFFSET)

#define OCR_RUNTIME_HINT_SET_SIZE(_mask, _size)                                                         \
do {                                                                                                    \
    _mask = (_mask & (~OCR_RUNTIME_HINT_SIZE_MASK)) | (((u64)_size) << OCR_RUNTIME_HINT_SIZE_OFFSET);   \
} while(0);

#define OCR_RUNTIME_HINT_SET(_uHint, _rHint, _count, _prop, _start)             \
do {                                                                            \
    u32 i, size = 0;                                                            \
    if (OCR_RUNTIME_HINT_GET_TYPE((_rHint)->hintMask) != (u64)((_uHint)->type)) \
        return OCR_EINVAL;                                                      \
                                                                                \
    u64 runtimePropMask = OCR_RUNTIME_HINT_GET_PROP((_rHint)->hintMask);        \
    for (i = 0; i < _count; i++) {                                              \
        u32 index = _prop[i] - _start - 1;                                      \
        u64 mask = 0x1UL << index;                                              \
        if ((_uHint)->propMask & mask) {                                        \
            (_rHint)->hintVal[i] = ((u64*)(&((_uHint)->args)))[index];          \
            if ((runtimePropMask & mask) == 0) {                                \
                (_rHint)->hintMask |= mask;                                     \
                size++;                                                         \
            }                                                                   \
        }                                                                       \
    }                                                                           \
    if (size > 0) {                                                             \
        u64 newSize = OCR_RUNTIME_HINT_GET_SIZE((_rHint)->hintMask) + size;     \
        OCR_RUNTIME_HINT_SET_SIZE((_rHint)->hintMask, newSize);                 \
    }                                                                           \
} while(0);

#define OCR_RUNTIME_HINT_GET(_uHint, _rHint, _count, _prop, _start)             \
do {                                                                            \
    u32 i;                                                                      \
    if (OCR_RUNTIME_HINT_GET_TYPE((_rHint)->hintMask) != (u64)((_uHint)->type)) \
        return OCR_EINVAL;                                                      \
                                                                                \
    u64 runtimePropMask = OCR_RUNTIME_HINT_GET_PROP((_rHint)->hintMask);        \
    for (i = 0; i < _count; i++) {                                              \
        u32 index = _prop[i] - _start - 1;                                      \
        u64 mask = 0x1UL << index;                                              \
        if (runtimePropMask & mask) {                                           \
            ((u64*)(&((_uHint)->args)))[index] = (_rHint)->hintVal[i];          \
            (_uHint)->propMask |= mask;                                         \
        }                                                                       \
    }                                                                           \
} while(0);

#endif /* OCR_RUNTIME_HINT_H_ */
