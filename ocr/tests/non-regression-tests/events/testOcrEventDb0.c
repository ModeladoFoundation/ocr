/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
/**
 * DESC: Test satisfy event with db
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
    ocrGuid_t event_guid;
    ocrEventCreate(&event_guid, OCR_EVENT_STICKY_T, true);

    // Creates the EDT
    ocrGuid_t edtGuid;
    ocrGuid_t taskForEdtTemplateGuid;
    ocrEdtTemplateCreate(&taskForEdtTemplateGuid, taskForEdt, 0 /*paramc*/, 1 /*depc*/);
    ocrEdtCreate(&edtGuid, taskForEdtTemplateGuid, EDT_PARAM_DEF, /*paramv=*/NULL, EDT_PARAM_DEF, /*depv=*/NULL,
                 /*properties=*/0, NULL_HINT, /*outEvent=*/NULL);

    // Register a dependence between an event and an edt
    ocrAddDependence(event_guid, edtGuid, 0, DB_MODE_CONST);

    int *k;
    ocrGuid_t db_guid;
    ocrDbCreate(&db_guid,(void **) &k,
                sizeof(int), /*flags=*/DB_PROP_NONE,
                /*location=*/NULL_HINT,
                NO_ALLOC);
    *k = 42;
    ocrDbRelease(db_guid);
    ocrEventSatisfy(event_guid, db_guid);
    return NULL_GUID;
}
