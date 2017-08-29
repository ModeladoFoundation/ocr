/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */



#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: OCR-DIST - create an EDT that creates a child EDT in the same affinity group
 */

ocrGuid_t shutdownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t currentAffinity;
    ocrAffinityGetCurrent(&currentAffinity);
    ocrPrintf("shutdownEdt: executing at "GUIDF"\n", GUIDA(currentAffinity));
    ocrAssert((currentAffinity.guid) == paramv[0]);
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t remoteEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t currentAffinity;
    ocrAffinityGetCurrent(&currentAffinity);
    ocrPrintf("remoteEdt: executing at affinity "GUIDF"\n", GUIDA(currentAffinity));
    // Create a new EDT with affinity set to current EDT's affinity
    ocrGuid_t shutdownEdtTemplateGuid;
    ocrEdtTemplateCreate(&shutdownEdtTemplateGuid, shutdownEdt, 1, 0);
    ocrHint_t edtHint;
    ocrHintInit( &edtHint, OCR_HINT_EDT_T);
    ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( currentAffinity) );
    ocrGuid_t edtGuid;
    u64 nparamv = (u64) currentAffinity.guid;
    ocrEdtCreate(&edtGuid, shutdownEdtTemplateGuid, EDT_PARAM_DEF, &nparamv, EDT_PARAM_DEF, NULL,
        EDT_PROP_NONE, &edtHint, NULL);
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 affinityCount;
    ocrAffinityCount(AFFINITY_PD, &affinityCount);
    ocrAssert(affinityCount >= 1);
    ocrGuid_t affinities[affinityCount];
    ocrAffinityGet(AFFINITY_PD, &affinityCount, affinities);
    ocrGuid_t edtAffinity = affinities[affinityCount-1];

    ocrGuid_t remoteEdtTemplateGuid;
    ocrEdtTemplateCreate(&remoteEdtTemplateGuid, remoteEdt, 0, 0);
    ocrHint_t edtHint;
    ocrHintInit( &edtHint, OCR_HINT_EDT_T );
    ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( edtAffinity) );

    ocrPrintf("mainEdt: create remote EDT\n");
    ocrGuid_t edtGuid;
    ocrEdtCreate(&edtGuid, remoteEdtTemplateGuid, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
        EDT_PROP_NONE, &edtHint, NULL);

    return NULL_GUID;
}
