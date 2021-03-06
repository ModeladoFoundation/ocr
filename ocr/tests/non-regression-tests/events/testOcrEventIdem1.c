/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"

/**
 * DESC: Satisfy an 'idempotent' event several times (subsequent ignored)
 */

ocrGuid_t computeEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
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
    ocrEventCreate(&e0, OCR_EVENT_IDEM_T, EVT_PROP_TAKES_ARG);
    ocrGuid_t e1;
    ocrEventCreate(&e1, OCR_EVENT_IDEM_T, EVT_PROP_TAKES_ARG);

    // Creates the EDT
    ocrGuid_t edtGuid;
    ocrGuid_t computeEdtTemplateGuid;
    ocrEdtTemplateCreate(&computeEdtTemplateGuid, computeEdt, 0 /*paramc*/, 2 /*depc*/);
    ocrEdtCreate(&edtGuid, computeEdtTemplateGuid, EDT_PARAM_DEF, /*paramv=*/NULL, EDT_PARAM_DEF, /*depv=*/NULL,
                 /*properties=*/0, NULL_HINT, /*outEvent=*/NULL);

    // Register a dependence between an event and an edt
    ocrAddDependence(e0, edtGuid, 0, DB_MODE_CONST);
    ocrAddDependence(e1, edtGuid, 1, DB_MODE_CONST);

    int *k0;
    ocrGuid_t dbGuid0;
    ocrDbCreate(&dbGuid0,(void **) &k0, sizeof(int), /*flags=*/0,
                /*location=*/NULL_HINT, NO_ALLOC);
    *k0 = 42;

    int *k1;
    ocrGuid_t dbGuid1;
    ocrDbCreate(&dbGuid1,(void **) &k1, sizeof(int), /*flags=*/0,
                /*location=*/NULL_HINT, NO_ALLOC);
    *k1 = 43;
    ocrDbRelease(dbGuid0);
    ocrDbRelease(dbGuid1);
    // Satisfy first slot with db pointing to '42'
    ocrEventSatisfy(e0, dbGuid0);

    // These should be ignored by the runtime and the db shouldn't be updated
    ocrEventSatisfy(e0, dbGuid1);
    ocrEventSatisfy(e0, dbGuid1);

    // Trigger the edt
    ocrEventSatisfy(e1, NULL_GUID);

    return NULL_GUID;
}
