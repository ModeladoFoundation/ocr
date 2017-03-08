/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
#include "extensions/ocr-db-info.h"

/**
 * DESC: Get the size of an acquired data block
 */

#ifdef ENABLE_EXTENSION_DB_INFO

#define EXPECTED_DB_SIZE 12345

ocrGuid_t consumerEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 dbSize;
    ocrDbGetSize(depv[0].guid, &dbSize);

    ASSERT(dbSize == EXPECTED_DB_SIZE);
    PRINTF("Test passed! (dbSize = %" PRIu64 ")\n", dbSize);

    ocrShutdown();

    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t dbGuid;
    void *dbPtr;
    ocrDbCreate(&dbGuid, &dbPtr, EXPECTED_DB_SIZE, 0, NULL_HINT, NO_ALLOC);

    ocrGuid_t edtTpl;
    ocrEdtTemplateCreate(&edtTpl, consumerEdt, 0, 1);
    ocrGuid_t edt;
    ocrEdtCreate(&edt, edtTpl, EDT_PARAM_DEF, NULL,
                 EDT_PARAM_DEF, &dbGuid, EDT_PROP_NONE, NULL_HINT, NULL);

    return NULL_GUID;
}

#else

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("Test disabled - ENABLE_EXTENSION_DB_INFO not defined\n");
    ocrShutdown();
    return NULL_GUID;
}

#endif /* ENABLE_EXTENSION_DB_INFO */
