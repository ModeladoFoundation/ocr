/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"

/**
 * DESC: Pure control dependence with ocrAddDependence/DB_MODE_NULL
 */

ocrGuid_t otherEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ASSERT(depc == 1);
    ASSERT(depv[0].guid == NULL_GUID);
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t tplGuid;
    ocrEdtTemplateCreate(&tplGuid, otherEdt, 0 /*paramc*/, 1 /*depc*/);
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, tplGuid, 0, NULL, 1, NULL,
                 EDT_PROP_NONE, PICK_1_1(NULL_HINT,NULL_GUID), NULL);
    ocrAddDependence(NULL_GUID, edtGuid, 0, DB_MODE_NULL);
    return 0;
}
