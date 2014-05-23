/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __CE_COMPONENT_H__
#define __CE_COMPONENT_H__

#include "ocr-config.h"
#ifdef ENABLE_COMPONENT_CE_BASE

#include "utils/ocr-utils.h"
#include "utils/deque.h"
#include "ocr-component.h"

/****************************************************/
/* OCR CE BASE COMPONENT                            */
/****************************************************/

/*! \brief CE component implementation for a shared memory workstealing runtime
 */
typedef struct {
    ocrComponent_t base;
    deque_t * deque;
} ocrComponentCeBase_t;

typedef struct {
    ocrComponentFactory_t baseFactory;
    u32 dequeInitSize;
} ocrComponentFactoryCeBase_t;

typedef struct _paramListComponentFactCeBase_t {
    paramListComponentFact_t base;
    u32 dequeInitSize;
} paramListComponentFactCeBase_t;

ocrComponentFactory_t * newOcrComponentFactoryCe(ocrParamList_t* perType);

#endif /* ENABLE_COMPONENT_CE_BASE */

#endif /* __CE_COMPONENT_H__ */