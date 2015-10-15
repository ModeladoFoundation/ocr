/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"

#ifdef ENABLE_EXTENSION_LABELING
#include "extensions/ocr-labeling.h"

/**
 * DESC: Create a labeled GUID and create two events out of it with GUID_PROP_CHECK
 */

ocrGuid_t mapFunc(ocrGuid_t startGuid, u64 stride, s64* params, s64* tuple) {
    return (ocrGuid_t)(tuple[0]*stride + startGuid);
}

ocrGuid_t shutEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {

    ocrGuid_t mapGuid = NULL_GUID;
    ocrGuidMapCreate(&mapGuid, 0, mapFunc, NULL, 10, GUID_USER_EVENT_STICKY);

    s64 val = 0;
    ocrGuid_t evtGuid;
    ocrGuidFromLabel(&evtGuid, mapGuid, &val);
    u8 retCode = ocrEventCreate(&evtGuid, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK);
    ASSERT(retCode == 0);

    retCode = ocrEventCreate(&evtGuid, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK);
    ASSERT(retCode == OCR_EGUIDEXISTS);

    ocrGuid_t templGuid;
    ocrEdtTemplateCreate(&templGuid, shutEdt, 0, 1);

    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, templGuid, 0, NULL, 1, &evtGuid, EDT_PROP_NONE, NULL_GUID, NULL);

    ocrEventSatisfy(evtGuid, NULL_GUID);

    return NULL_GUID;
}

#else

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("Test disabled - ENABLE_EXTENSION_LABELING not defined\n");
    ocrShutdown();
    return NULL_GUID;
}

#endif
