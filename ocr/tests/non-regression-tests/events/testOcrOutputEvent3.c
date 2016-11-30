/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"

#define N 5

/**
 * DESC: Chain an edt to another edt's output event.
 */

// This edt is triggered when the output event of the other edt is satisfied by the runtime
ocrGuid_t chainedEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrDbDestroy(depv[1].guid);
    ocrShutdown(); // This is the last EDT to execute, terminate
    return NULL_GUID;
}

ocrGuid_t childrenEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // When this edt terminates, runtime will satisfy its parent finish event
    // automatically
    return NULL_GUID;
}

ocrGuid_t taskForEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // When this edt terminates, the runtime will satisfy its output event automatically
    ocrGuid_t childrenGuid;
    ocrGuid_t childrenEdtTemplateGuid;
    ocrEdtTemplateCreate(&childrenEdtTemplateGuid, childrenEdt, 0 /*paramc*/, 0 /*depc*/);
    for( int i = 0; i < N; ++i )
        ocrEdtCreate(&childrenGuid, childrenEdtTemplateGuid, 0, NULL, 0, NULL,
                EDT_PROP_NONE, NULL_HINT, NULL);
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // Current thread is '0' and goes on with user code.
    ocrGuid_t event_guid;
    ocrEventCreate(&event_guid, OCR_EVENT_STICKY_T, true);

    // Setup output event before EDT creation.
    // Allows to add dependences before the event can be satisfied.
    // OCR_EVENT_LATCH_T is required here, as EDT has finish property.
    ocrGuid_t output_event_guid;
    ocrEventCreate(&output_event_guid, OCR_EVENT_LATCH_T, EVT_PROP_NONE);

    // Setup edt input event
    ocrGuid_t input_event_guid;
    ocrEventCreate(&input_event_guid, OCR_EVENT_ONCE_T, true);

    // Create the chained EDT and add input and output events as dependences.
    ocrGuid_t chainedEdtGuid;
    ocrGuid_t chainedEdtTemplateGuid;
    ocrEdtTemplateCreate(&chainedEdtTemplateGuid, chainedEdt, 0 /*paramc*/, 2 /*depc*/);
    ocrEdtCreate(&chainedEdtGuid, chainedEdtTemplateGuid, 0, NULL, 2, NULL,
                 EDT_PROP_FINISH, NULL_HINT, NULL);
    ocrAddDependence(output_event_guid, chainedEdtGuid, 0, DB_MODE_CONST);
    ocrAddDependence(input_event_guid, chainedEdtGuid, 1, DB_MODE_CONST);

    // Satisfy Output event's first pre-slot
    ocrEventSatisfySlot(output_event_guid, NULL_GUID, OCR_EVENT_LATCH_INCR_SLOT);

    // Creates the parent EDT
    ocrGuid_t edtGuid;
    ocrGuid_t taskForEdtTemplateGuid;
    ocrEdtTemplateCreate(&taskForEdtTemplateGuid, taskForEdt, 0 /*paramc*/, 0 /*depc*/);
    ocrEdtCreate(&edtGuid, taskForEdtTemplateGuid, 0, NULL, 0, NULL,
                 EDT_PROP_FINISH|EDT_PROP_OEVT_VALID, NULL_HINT,
                 /*outEvent=*/&output_event_guid);

    // Transmit the parent edt's guid as a parameter to the chained edt
    // Build input db for the chained edt
    ocrGuid_t * guid_ref;
    ocrGuid_t db_guid;
    ocrDbCreate(&db_guid,(void **) &guid_ref, sizeof(ocrGuid_t), /*flags=*/DB_PROP_NONE, /*loc=*/NULL_HINT, NO_ALLOC);
    *guid_ref = edtGuid;
    // Satisfy the input slot of the chained edt
    ocrEventSatisfy(input_event_guid, db_guid);

    // Satisfy the parent edt. At this point it should run
    // to completion and satisfy its output event with its guid
    // which should trigger the chained edt since all its input
    // dependencies will be satisfied.
    ocrEventSatisfy(event_guid, NULL_GUID);

    return NULL_GUID;
}
