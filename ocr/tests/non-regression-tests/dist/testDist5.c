/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: OCR-DIST - create two remote EDT depending on db containing events to enable the sink EDT
 */

ocrGuid_t remoteEdt1(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("[remote] RemoteEdt1 will satisfy [0] 0x%"PRIx64" \n", ((u64 *) depv[0].ptr)[0]);
    PRINTF("[remote] RemoteEdt1 will satisfy [1] 0x%"PRIx64" \n", ((u64 *) depv[0].ptr)[1]);
    ocrEventSatisfy(((ocrGuid_t *) depv[0].ptr)[0], NULL_GUID);
    return NULL_GUID;
}

ocrGuid_t remoteEdt2(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("[remote] RemoteEdt2 will satisfy [0] 0x%"PRIx64" \n", ((u64 *) depv[0].ptr)[0]);
    PRINTF("[remote] RemoteEdt2 will satisfy [1] 0x%"PRIx64" \n", ((u64 *) depv[0].ptr)[1]);
    ocrEventSatisfy(((ocrGuid_t *) depv[0].ptr)[1], NULL_GUID);
    return NULL_GUID;
}

ocrGuid_t shutdownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 affinityCount;
    ocrAffinityCount(AFFINITY_PD, &affinityCount);
    ASSERT(affinityCount >= 1);
    ocrGuid_t affinities[affinityCount];
    ocrAffinityGet(AFFINITY_PD, &affinityCount, affinities);
    ocrGuid_t edtAffinity = affinities[affinityCount-1];

    // Setup the sink EDT
    ocrGuid_t event1Guid;
    ocrEventCreate(&event1Guid,OCR_EVENT_ONCE_T, EVT_PROP_NONE);
    ocrGuid_t event2Guid;
    ocrEventCreate(&event2Guid,OCR_EVENT_ONCE_T, EVT_PROP_NONE);
    ocrGuid_t dbGuid;
    ocrGuid_t * dbPtr;
    ocrDbCreate(&dbGuid, (void **)&dbPtr, sizeof(ocrGuid_t) * 2, DB_PROP_SINGLE_ASSIGNMENT, NULL_HINT, NO_ALLOC);
    dbPtr[0] = event1Guid;
    dbPtr[1] = event2Guid;
    ocrDbRelease(dbGuid);
    ocrGuid_t shutdownEdtTemplateGuid;
    ocrEdtTemplateCreate(&shutdownEdtTemplateGuid, shutdownEdt, 0, 2);
    ocrGuid_t edtShutdownGuid;
    ocrGuid_t shutdownDepv[2] = {event1Guid, event2Guid};
    ocrEdtCreate(&edtShutdownGuid, shutdownEdtTemplateGuid, 0, NULL, 2, shutdownDepv,
        EDT_PROP_NONE, NULL_HINT, NULL);
    PRINTF("[local] events guids are [0]="GUIDF" [1]="GUIDF"\n",GUIDA(event1Guid), GUIDA(event2Guid));
    // Setup the test
    ocrGuid_t remoteEdt1TemplateGuid;
    ocrEdtTemplateCreate(&remoteEdt1TemplateGuid, remoteEdt1, 0, 1);
    ocrHint_t edtHint;
    ocrHintInit( &edtHint, OCR_HINT_EDT_T );
    ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( edtAffinity) );
    ocrGuid_t edt1Guid;
    ocrEdtCreate(&edt1Guid, remoteEdt1TemplateGuid, 0, NULL, 1, NULL,
        EDT_PROP_NONE, &edtHint, NULL);
    ocrAddDependence(dbGuid, edt1Guid, 0, DB_MODE_CONST);
    ocrGuid_t remoteEdt2TemplateGuid;
    ocrEdtTemplateCreate(&remoteEdt2TemplateGuid, remoteEdt2, 0, 1);
    ocrGuid_t edt2Guid;
    ocrEdtCreate(&edt2Guid, remoteEdt2TemplateGuid, 0, NULL, 1, NULL,
        EDT_PROP_NONE, &edtHint, NULL);
    ocrAddDependence(dbGuid, edt2Guid, 0, DB_MODE_CONST);

    return NULL_GUID;
}
