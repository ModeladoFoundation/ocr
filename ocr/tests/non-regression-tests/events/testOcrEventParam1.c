/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
/**
 * DESC: Test passing paramv and depv to ocrEdtCreate
 */


ocrGuid_t taskForEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPrintf("In the taskForEdt with value %"PRId32"\n", (int)paramv[0]);
    ocrAssert(paramc == 1);
    ocrAssert(paramv[0] == 32);
    void * ptr = depv[0].ptr;
    ocrAssert(*((int*)ptr) == 42);
    // This is the last EDT to execute, terminate
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // Current thread is '0' and goes on with user code.
    ocrGuid_t ndepv[1];
    ocrGuid_t eventGuid;
    ocrEventCreate(&eventGuid, OCR_EVENT_STICKY_T, true);
    ndepv[0] = eventGuid;

    // Creates the EDT
    u32 nparamc = 1;
    u64 nparamv = 32;

    // 'ndepv' stores dependencies, so no need to call
    // ocrAddDependence later on to register events.
    ocrGuid_t edtGuid;
    ocrGuid_t taskForEdtTemplateGuid;
    ocrEdtTemplateCreate(&taskForEdtTemplateGuid, taskForEdt, nparamc, 1 /*depc*/);
    ocrEdtCreate(&edtGuid, taskForEdtTemplateGuid, EDT_PARAM_DEF, &nparamv, EDT_PARAM_DEF, /*depv=*/ndepv,
                 /*properties=*/0, NULL_HINT, /*outEvent=*/NULL);

    int *k;
    ocrGuid_t dbGuid;
    ocrDbCreate(&dbGuid,(void **) &k,
                sizeof(int), /*flags=*/DB_PROP_NONE,
                /*location=*/NULL_HINT,
                NO_ALLOC);
    *k = 42;

    ocrEventSatisfy(eventGuid, dbGuid);

    return NULL_GUID;
}
