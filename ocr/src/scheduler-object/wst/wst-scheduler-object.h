/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __WST_SCHEDULER_OBJECT_H__
#define __WST_SCHEDULER_OBJECT_H__

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_WST

#include "ocr-scheduler-object.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"

/****************************************************/
/* OCR WST SCHEDULER_OBJECT                         */
/****************************************************/

typedef struct _paramListSchedulerObjectWst_t {
    paramListSchedulerObject_t base;
    u32 numDeques;                          /* Number of deques */
} paramListSchedulerObjectWst_t;

typedef struct _ocrSchedulerObjectWst_t {
    ocrSchedulerObject_t base;
    ocrSchedulerObject_t **deques;          /* Array of deques */
    u32 numDeques;                          /* Number of deques */
    ocrSchedulerObject_t *plist;            /* Common priority list */
    ocrSchedulerObjectIterator_t *iterator;	/* Priority list iterator */
} ocrSchedulerObjectWst_t;

/****************************************************/
/* OCR WST SCHEDULER_OBJECT FACTORY                 */
/****************************************************/

typedef struct _ocrSchedulerObjectFactoryWst_t {
    ocrSchedulerObjectFactory_t base;
} ocrSchedulerObjectFactoryWst_t;

typedef struct _paramListSchedulerObjectFactWst_t {
    paramListSchedulerObjectFact_t base;
} paramListSchedulerObjectFactWst_t;

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryWst(ocrParamList_t *perType, u32 factoryId);

#endif /* ENABLE_SCHEDULER_OBJECT_WST */
#endif /* __WST_SCHEDULER_OBJECT_H__ */

