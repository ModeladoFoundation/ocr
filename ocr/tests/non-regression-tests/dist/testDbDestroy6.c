/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: Create a datablock and destroy it immediately (shutdown in separate EDT)
 */

#define TYPE_ELEM_DB int
#define NB_ELEM_DB 20


ocrGuid_t endEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t computeEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    void * dbPtr;
    ocrGuid_t dbGuid;
    ocrDbCreate(&dbGuid, &dbPtr, sizeof(TYPE_ELEM_DB) * NB_ELEM_DB, 0, PICK_1_1(NULL_HINT,NULL_GUID), NO_ALLOC);
    PRINTF("Created 0x%lx\n", dbGuid);
    ocrDbDestroy(dbGuid);
    ocrGuid_t tplGuid;
    ocrEdtTemplateCreate(&tplGuid, endEdt, 0, 0);
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, tplGuid, 0, NULL, 0, NULL, EDT_PROP_NONE, PICK_1_1(NULL_HINT,NULL_GUID), NULL);
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t tplGuid;
    ocrEdtTemplateCreate(&tplGuid, computeEdt, 0, 0);
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, tplGuid, 0, NULL, 0, NULL, EDT_PROP_NONE, PICK_1_1(NULL_HINT,NULL_GUID), NULL);

    return NULL_GUID;
}
