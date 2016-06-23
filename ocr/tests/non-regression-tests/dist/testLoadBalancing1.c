
/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: TODO
 */

#define NB_EDT 20

ocrGuid_t shutEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t runEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("runEdt\n");
    return NULL_GUID;
}

ocrGuid_t headEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u32 i = 0;
    ocrGuid_t runTpl;
    ocrEdtTemplateCreate(&runTpl, runEdt, 0, 1);
    ocrGuid_t dbGuid = depv[1].guid;
    while (i < NB_EDT) {
        ocrGuid_t edtGuid;
        ocrEdtCreate(&edtGuid, runTpl, 0, NULL, 1, &dbGuid, EDT_PROP_NONE, NULL_HINT, NULL);
        i++;
    }
    ocrEdtTemplateDestroy(runTpl);
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 affinityCount;
    ocrAffinityCount(AFFINITY_PD, &affinityCount);
    ASSERT(affinityCount >= 1);
    ocrGuid_t affinities[affinityCount];
    ocrAffinityGet(AFFINITY_PD, &affinityCount, affinities);
    ocrGuid_t dbGuid;
    void * event_array;
    ocrDbCreate(&dbGuid, &event_array, sizeof(void *), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    ocrDbRelease(dbGuid);
    // create local edt that depends on the remote edt, the db is automatically cloned
    ocrGuid_t headTpl;
    ocrEdtTemplateCreate(&headTpl, headEdt, 0, 2);
    ocrGuid_t fevtOut;
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, headTpl, 0, NULL, 2, NULL,
                 EDT_PROP_FINISH, NULL_HINT, &fevtOut);
    ocrAddDependence(dbGuid, edtGuid, 1, DB_MODE_RO);
    ocrGuid_t shutTpl;
    ocrEdtTemplateCreate(&shutTpl, shutEdt, 0, 1);
    ocrGuid_t shutEdt;
    ocrEdtCreate(&shutEdt, shutTpl, 0, NULL, 1, &fevtOut,
                 EDT_PROP_NONE, NULL_HINT, NULL);
    ocrAddDependence(NULL_GUID, edtGuid, 0, DB_MODE_NULL);

    return NULL_GUID;
}
