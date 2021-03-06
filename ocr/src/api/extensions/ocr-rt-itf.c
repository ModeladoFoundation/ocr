/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#include "ocr-errors.h"
#ifdef ENABLE_EXTENSION_RTITF

#include "ocr-errors.h"
#include "debug.h"
#include "ocr-runtime.h"
#include "ocr-sal.h"

#include "utils/profiler/profiler.h"

#pragma message "RT-ITF extension is experimental and may not be supported on all platforms"

/**
   @brief Get @ offset in the currently running edt's local storage
   Note: not visible from the ocr user interface
 **/
ocrGuid_t ocrElsUserGet(u8 offset) {
    START_PROFILE(api_ocrElsUserGet);
    ocrTask_t *task = NULL;
    // User indexing start after runtime-reserved ELS slots
    offset = ELS_RUNTIME_SIZE + offset;
    getCurrentEnv(NULL, NULL, &task, NULL);
    RETURN_PROFILE(task->els[offset]);
}

/**
   @brief Set data @ offset in the currently running edt's local storage
   Note: not visible from the ocr user interface
 **/
void ocrElsUserSet(u8 offset, ocrGuid_t data) {
    START_PROFILE(api_ocrElsUserSet);
    // User indexing start after runtime-reserved ELS slots
    ocrTask_t *task = NULL;
    offset = ELS_RUNTIME_SIZE + offset;
    getCurrentEnv(NULL, NULL, &task, NULL);
    task->els[offset] = data;
    RETURN_PROFILE();
}

/**
   @brief Get the local data-store associated with the current EDT
          and its size
 **/
u8 ocrEdtLocalStorageGet(void **ptr, u64 *elsSize) {
    ocrTask_t *task = NULL;
    getCurrentEnv(NULL, NULL, &task, NULL);
    if((ptr == NULL) || (elsSize == NULL) || (task==NULL))
        return OCR_EINVAL;
    else {
        *ptr = task->els;
        *elsSize = ELS_USER_SIZE;
    }
    return 0;
}

u8 ocrCurrentEdtGet(ocrGuid_t *curEdt) {
    ocrTask_t *task = NULL;
    if(curEdt == NULL)
        return OCR_EINVAL;
    getCurrentEnv(NULL, NULL, &task, NULL);
    *curEdt = (task) ? task->guid : NULL_GUID;
    return 0;
}

u8 ocrCurrentEdtOutputGet(ocrGuid_t *outputEvent) {
    ocrTask_t *task = NULL;
    if(outputEvent == NULL)
        return OCR_EINVAL;
    getCurrentEnv(NULL, NULL, &task, NULL);
    *outputEvent = (task) ? task->outputEvent : NULL_GUID;
    return 0;
}

// exposed to runtime implementers as convenience
u64 ocrNbWorkers() {
    START_PROFILE(api_ocrNbWorkers)
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if(pd != NULL)
        RETURN_PROFILE(pd->workerCount);
    RETURN_PROFILE(0);
}

// exposed to runtime implementers as convenience
void ocrCurrentWorkerGuid(ocrGuid_t * workerGuid) {
    START_PROFILE(api_ocrCurrentWorkerGuid);
    ocrWorker_t *worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    *workerGuid = (worker) ? worker->fguid.guid : NULL_GUID;
    RETURN_PROFILE();
}

// Inform the OCR runtime the currently executing thread is logically blocked
u8 ocrInformLegacyCodeBlocking() {
    START_PROFILE(api_ocrInformLegacyCodeBlocking);
    // The current default implementation is to execute another EDT to try to make progress
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    RETURN_PROFILE(pd->schedulers[0]->fcts.monitorProgress(pd->schedulers[0], MAX_MONITOR_PROGRESS, NULL));
}

#endif /* ENABLE_EXTENSION_RTITF */
