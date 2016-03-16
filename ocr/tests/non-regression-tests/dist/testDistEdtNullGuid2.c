#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: Create a null-GUID EDT that has depv containing a sticky event and a db.
 */

ocrGuid_t terminateEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u32 * dbPtr = (u32 *) depv[0].ptr;
    PRINTF("Received depv[0] 0x%lx\n", depv[0].guid);
    u32 i = 0;
    while (i < 10) {
        ASSERT(dbPtr[i] == i);
        i++;
    }
    PRINTF("Everything went OK\n");
    ocrShutdown(); // This is the last EDT to execute, terminate
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 affinityCount;
    ocrAffinityCount(AFFINITY_PD, &affinityCount);
    ASSERT(affinityCount >= 1);
    ocrGuid_t affinities[affinityCount];
    ocrAffinityGet(AFFINITY_PD, &affinityCount, affinities);
    ocrGuid_t edtAffinity = affinities[affinityCount-1]; //TODO this implies we know current PD is '0'

    ocrGuid_t event1Guid;
    ocrEventCreate(&event1Guid,OCR_EVENT_ONCE_T, EVT_PROP_NONE);

    ocrGuid_t db1Guid;
    u32 * db1Ptr;
    ocrDbCreate(&db1Guid, (void**) &db1Ptr, sizeof(u32)*10, 0, NULL_HINT, NO_ALLOC);
    u32 i = 0;
    while (i < 10) {
        db1Ptr[i] = i;
        i++;
    }
    ocrDbRelease(db1Guid);
    PRINTF("DB guid is 0x%lx\n", db1Guid);
    ocrGuid_t terminateEdtTemplateGuid;
    ocrEdtTemplateCreate(&terminateEdtTemplateGuid, terminateEdt, 0 /*paramc*/, 2 /*depc*/);
    ocrHint_t edtHint;
    ocrHintInit( &edtHint, OCR_HINT_EDT_T );
    ocrSetHintValue( & edtHint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( edtAffinity) );
    ocrGuid_t ndepv[] = {db1Guid, event1Guid};

    ocrEdtCreate(NULL, terminateEdtTemplateGuid, 0, NULL, 2, ndepv,
                 EDT_PROP_NONE, &edtHint, /*outEvent=*/ NULL);

    ocrEventSatisfy(event1Guid, NULL_GUID);
    return NULL_GUID;
}
