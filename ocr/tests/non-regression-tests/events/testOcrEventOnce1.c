/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
/**
 * DESC: Add a once event as a dependence to an EDT twice (on different slots)
 */


ocrGuid_t taskForEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    int* res = (int*)depv[0].ptr;
    ocrPrintf("In the taskForEdt with value %"PRId32"\n", (*res));
    ocrAssert(*res == 42);
    // This is the last EDT to execute, terminate
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // Current thread is '0' and goes on with user code.
    ocrGuid_t e0;
    ocrEventCreate(&e0, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);

    // Creates the EDTa
    ocrGuid_t edtGuid;
    ocrGuid_t taskForEdtTemplateGuid;
    ocrEdtTemplateCreate(&taskForEdtTemplateGuid, taskForEdt, 0 /*paramc*/, 2/*depc*/);
    ocrEdtCreate(&edtGuid, taskForEdtTemplateGuid, EDT_PARAM_DEF, /*paramv=*/NULL, EDT_PARAM_DEF, /*depv=*/NULL,
                 /*properties=*/0, NULL_HINT, /*outEvent=*/NULL);

    // Register a dependence between an event and an edt
    ocrAddDependence(e0, edtGuid, 0, DB_MODE_CONST);
    ocrAddDependence(e0, edtGuid, 1, DB_MODE_CONST);

    int *k;
    ocrGuid_t dbGuid;
    ocrDbCreate(&dbGuid,(void **) &k,
                sizeof(int), /*flags=*/DB_PROP_NONE,
                /*location=*/NULL_HINT,
                NO_ALLOC);
    *k = 42;

    // Satisfy event's chain head
    ocrEventSatisfy(e0, dbGuid);

    return NULL_GUID;
}
