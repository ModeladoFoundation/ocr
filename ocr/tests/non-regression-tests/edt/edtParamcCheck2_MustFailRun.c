/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */



#include "ocr.h"
#include "extensions/ocr-affinity.h"


/**
 * DESC: Test runtime checks paramv is set when paramc > 0 (try to spawn at remote PD)
 */

ocrGuid_t terminateEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPrintf("Everything went OK\n");
    ocrShutdown(); // This is the last EDT to execute, terminate
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 affinityCount;
    ocrAffinityCount(AFFINITY_PD, &affinityCount);
    ocrAssert(affinityCount >= 1);
    ocrGuid_t affinities[affinityCount];
    ocrAffinityGet(AFFINITY_PD, &affinityCount, affinities);
    ocrGuid_t edtAffinity = affinities[affinityCount-1];

    ocrGuid_t terminateEdtGuid;
    ocrGuid_t terminateEdtTemplateGuid;
    ocrEdtTemplateCreate(&terminateEdtTemplateGuid, terminateEdt, EDT_PARAM_UNK /*paramc*/, 0 /*depc*/);
    ocrHint_t edtHint;
    ocrHintInit( &edtHint, OCR_HINT_EDT_T );
    ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( edtAffinity) );
    u8 res = ocrEdtCreate(&terminateEdtGuid, terminateEdtTemplateGuid, 1, NULL, 0, NULL,
                 /*properties=*/EDT_PROP_FINISH, &edtHint, /*outEvent=*/ NULL);
    ocrAssert(!res);
    return NULL_GUID;
}
