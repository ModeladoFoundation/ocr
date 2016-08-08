/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __HC_WORKER_H__
#define __HC_WORKER_H__

#include "ocr-config.h"
#ifdef ENABLE_WORKER_HC

#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"
#include "utils/deque.h"

typedef struct {
    ocrWorkerFactory_t base;
} ocrWorkerFactoryHc_t;

typedef struct _paramListWorkerHcInst_t {
    paramListWorkerInst_t base;
    ocrWorkerType_t workerType;
} paramListWorkerHcInst_t;

typedef enum {
    HC_WORKER_COMP,
    HC_WORKER_COMM,
    HC_WORKER_SYSTEM
} hcWorkerType_t;

typedef struct {
    ocrWorker_t worker;
    // The HC implementation relies on integer ids to
    // map workers, schedulers and workpiles together
    u64 id;
    ocrGuid_t edtGuid;
#ifdef OCR_ENABLE_EDT_NAMING
    const char * name;
#endif
    hcWorkerType_t hcType;
    u8 legacySecondStart;
    deque_t *sysDeque;
#ifdef ENABLE_EXTENSION_PERF
    salPerfCounter *perfCtrs;
#endif
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    u32 isHelping;
    bool stealFirst;
#endif
} ocrWorkerHc_t;

ocrWorkerFactory_t* newOcrWorkerFactoryHc(ocrParamList_t *perType);

#endif /* ENABLE_WORKER_HC */
#endif /* __HC_WORKER_H__ */
