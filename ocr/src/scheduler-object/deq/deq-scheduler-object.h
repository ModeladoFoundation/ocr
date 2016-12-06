/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __DEQ_SCHEDULER_OBJECT_H__
#define __DEQ_SCHEDULER_OBJECT_H__

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_DEQ

#include "ocr-scheduler-object.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "utils/deque.h"

/****************************************************/
/* OCR DEQ SCHEDULER_OBJECT                         */
/****************************************************/

typedef struct _paramListSchedulerObjectDeq_t {
    paramListSchedulerObject_t base;
    ocrDequeType_t type;
} paramListSchedulerObjectDeq_t;

typedef struct _ocrSchedulerObjectDeq_t {
    ocrSchedulerObject_t base;
    ocrDequeType_t dequeType;   // type of deque
    deque_t *deque;             // the deque
#ifdef ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
    deque_t *dequeRt;           // deque for runtime edts
#endif
} ocrSchedulerObjectDeq_t;

/****************************************************/
/* OCR DEQ SCHEDULER_OBJECT FACTORY                 */
/****************************************************/

typedef struct _ocrSchedulerObjectFactoryDeq_t {
    ocrSchedulerObjectFactory_t base;
} ocrSchedulerObjectFactoryDeq_t;

typedef struct _paramListSchedulerObjectFactDeq_t {
    paramListSchedulerObjectFact_t base;
} paramListSchedulerObjectFactDeq_t;

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryDeq(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_SCHEDULER_OBJECT_DEQ */
#endif /* __DEQ_SCHEDULER_OBJECT_H__ */

