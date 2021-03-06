/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __CE_WORKER_H__
#define __CE_WORKER_H__

#include "ocr-config.h"
#ifdef ENABLE_WORKER_CE

#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"

typedef struct {
    ocrWorkerFactory_t base;
} ocrWorkerFactoryCe_t;

typedef struct _paramListWorkerCeInst_t {
    paramListWorkerInst_t base;
    ocrWorkerType_t workerType;
} paramListWorkerCeInst_t;

typedef struct {
    ocrWorker_t worker;
    // The CE implementation relies on integer ids to
    // map workers, schedulers and workpiles together
    u64 id;
} ocrWorkerCe_t;

ocrWorkerFactory_t* newOcrWorkerFactoryCe(ocrParamList_t *perType);

#endif /* ENABLE_WORKER_CE */
#endif /* __CE_WORKER_H__ */
