/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: OCR-DIST - create a remote EDT and destroy it inside the EDT
 */

ocrGuid_t shutdownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t remoteEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPrintf("[remote] RemoteEdt: executing\n");
    ocrGuid_t edtTemplateGuid = {.guid=paramv[0]};
    ocrEdtTemplateDestroy(edtTemplateGuid);

    // shutdown
    ocrGuid_t templ;
    ocrEdtTemplateCreate(&templ, shutdownEdt, 0, 0);
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, templ, 0, NULL, 0, NULL,
        EDT_PROP_NONE, NULL_HINT, NULL);

    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 affinityCount;
    ocrAffinityCount(AFFINITY_PD, &affinityCount);
    ocrAssert(affinityCount >= 1);
    ocrGuid_t affinities[affinityCount];
    ocrAffinityGet(AFFINITY_PD, &affinityCount, affinities);
    ocrGuid_t edtAffinity = affinities[affinityCount-1];

    // create local edt that depends on the remote edt, the db is automatically cloned
    ocrGuid_t edtTemplateGuid;
    ocrEdtTemplateCreate(&edtTemplateGuid, remoteEdt, 1, 0);
    ocrHint_t edtHint;
    ocrHintInit( &edtHint, OCR_HINT_EDT_T );
    ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( edtAffinity) );

    u64 args[] = {edtTemplateGuid.guid};
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, edtTemplateGuid, 1, args, 0, NULL,
        EDT_PROP_NONE, &edtHint, NULL);

    return NULL_GUID;
}
