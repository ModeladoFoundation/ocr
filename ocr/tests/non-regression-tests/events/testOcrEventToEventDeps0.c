/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
/**
 * DESC: Test event to event dependences (chaining two events)
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
    ocrEventCreate(&e0, OCR_EVENT_STICKY_T, true);

    ocrGuid_t e1;
    ocrEventCreate(&e1, OCR_EVENT_STICKY_T, true);

    ocrGuid_t e2;
    ocrEventCreate(&e2, OCR_EVENT_STICKY_T, true);

    // Event chaining
    ocrAddDependence(e0, e1, 0, DB_MODE_CONST);
    ocrAddDependence(e1, e2, 0, DB_MODE_CONST);

    // Creates the EDT
    ocrGuid_t edtGuid;
    ocrGuid_t taskForEdtTemplateGuid;
    ocrEdtTemplateCreate(&taskForEdtTemplateGuid, taskForEdt, 0 /*paramc*/, 1 /*depc*/);
    ocrEdtCreate(&edtGuid, taskForEdtTemplateGuid, EDT_PARAM_DEF, /*paramv=*/NULL, EDT_PARAM_DEF, /*depv=*/NULL,
                 /*properties=*/0, NULL_HINT, /*outEvent=*/NULL);

    // Register a dependence between an event and an edt
    ocrAddDependence(e2, edtGuid, 0, DB_MODE_CONST);

    int *k;
    ocrGuid_t db_guid;
    ocrDbCreate(&db_guid,(void **) &k,
                sizeof(int), /*flags=*/DB_PROP_NONE,
                /*location=*/NULL_HINT,
                NO_ALLOC);
    *k = 42;

    // Satisfy event's chain head
    ocrEventSatisfy(e0, db_guid);

    return NULL_GUID;
}
