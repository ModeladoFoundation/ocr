/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __XE_WORKER_H__
#define __XE_WORKER_H__

#include "ocr-config.h"
#ifdef ENABLE_WORKER_XE

#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"

#ifdef ENABLE_EXTENSION_PERF
#include "ocr-sal.h"
#endif

typedef struct {
    ocrWorkerFactory_t base;
} ocrWorkerFactoryXe_t;

typedef struct _paramListWorkerXeInst_t {
    paramListWorkerInst_t base;
} paramListWorkerXeInst_t;

typedef struct {
    ocrWorker_t worker;
    // The HC implementation relies on integer ids to
    // map workers, schedulers and workpiles together
    u64 id;
#ifdef ENABLE_EXTENSION_PERF
    salPerfCounter* perfCtrs;
#endif
} ocrWorkerXe_t;

ocrWorkerFactory_t* newOcrWorkerFactoryXe(ocrParamList_t *perType);

#endif /* ENABLE_WORKER_XE */
#endif /* __XE_WORKER_H__ */
