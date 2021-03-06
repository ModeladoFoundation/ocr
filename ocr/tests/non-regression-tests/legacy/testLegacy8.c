/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"

// Only tested when OCR library interface is available
#ifdef ENABLE_EXTENSION_LEGACY

#include "extensions/ocr-legacy.h"

/**
 * DESC: Test reading the DB returned by ocrLegacyBlockProgress
 */

#define MARK 999

// This is the "key" EDT that is responsible for spawning all the workers
ocrGuid_t keyEdt ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 * array;
    ocrGuid_t dbGuid;
    ocrDbCreate(&dbGuid,(void **) &array, sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    array[0] = MARK;
    ocrDbRelease(dbGuid);
    return dbGuid;
}

void ocrBlock(ocrConfig_t cfg) {
    ocrGuid_t legacyCtx;
    ocrGuid_t template, handle;

    ocrGuid_t ctrlDep;
    ocrGuid_t outputGuid;
    void *result;
    u64 size;

    ocrLegacyInit(&legacyCtx, &cfg);
    ocrEdtTemplateCreate(&template, keyEdt, 0, 1);

    ctrlDep = NULL_GUID;
    ocrLegacySpawnOCR(&handle, template, 0, NULL, 1, &ctrlDep, legacyCtx);

    ocrLegacyBlockProgress(handle, &outputGuid, &result, &size, LEGACY_PROP_NONE);
    ocrAssert(!(ocrGuidIsNull(outputGuid)));
    ocrAssert(result != NULL);
    ocrAssert(((u64 *) result)[0] == MARK);
    ocrAssert(size == sizeof(u64));
    ocrEventDestroy(handle);
    ocrEdtTemplateDestroy(template);
    // This generates a warning and an error
    // ocrDbDestroy(outputGuid);
    ocrShutdown();
    ocrLegacyFinalize(legacyCtx, true);
}

int main(int argc, const char *argv[]) {
    ocrConfig_t ocrConfig;
    ocrParseArgs(argc, argv, &ocrConfig);
    ocrPrintf("Legacy code...\n");
    ocrBlock(ocrConfig);
    ocrPrintf("Back to legacy code, done.\n");
    return 0;
}

#else

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

#endif
