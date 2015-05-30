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

typedef struct {
    ocrWorkerFactory_t base;
} ocrWorkerFactoryHc_t;

typedef struct _paramListWorkerHcInst_t {
    paramListWorkerInst_t base;
    ocrWorkerType_t workerType;
} paramListWorkerHcInst_t;

typedef enum {
    HC_WORKER_COMP,
    HC_WORKER_COMM
} hcWorkerType_t;

typedef struct {
    ocrWorker_t worker;
    // The HC implementation relies on integer ids to
    // map workers, schedulers and workpiles together
    u64 id;

    //Pause/Query/Resume variables for displaying EDTS
    //BUG #559: Possibly wrap in ifdef
    ocrGuid_t templateGuid;
    ocrGuid_t edtGuid;
    ocrEdt_t fctPtr;
#ifdef OCR_ENABLE_EDT_NAMING
    char * name;
#endif
    hcWorkerType_t hcType;
} ocrWorkerHc_t;

ocrGuid_t hcDumpNextEdt(ocrWorker_t *worker, u32 *wrkrSize);

ocrWorkerFactory_t* newOcrWorkerFactoryHc(ocrParamList_t *perType);

#endif /* ENABLE_WORKER_HC */
#endif /* __HC_WORKER_H__ */
