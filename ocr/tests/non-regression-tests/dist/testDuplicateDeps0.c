
/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: EDT with multiple dependences on the same DB (provided at creation)
 */

#define TYPE_ELEM_DB int
#define NB_ELEM_DB 20

ocrGuid_t readerEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t dbCloneGuid = (ocrGuid_t) depv[0].guid;
    ocrPrintf("[remote] readerEdt: executing, depends on remote DB guid "GUIDF" \n", GUIDA(dbCloneGuid));
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // Create a DB
    void * dbPtr;
    ocrGuid_t dbGuid;
    u64 nbElem = NB_ELEM_DB;
    ocrDbCreate(&dbGuid, &dbPtr, sizeof(TYPE_ELEM_DB) * NB_ELEM_DB, 0, NULL_HINT, NO_ALLOC);
    int v = 1;
    int i = 0;
    int * data = (int *) dbPtr;
    while (i < nbElem) {
        data[i] = v++;
        i++;
    }
    ocrDbRelease(dbGuid);
    ocrPrintf("[local] mainEdt: local DB guid is "GUIDF", dbPtr=%p\n", GUIDA(dbGuid), dbPtr);

    // create local edt that depends on the remote edt, the db is automatically cloned
    ocrGuid_t readerEdtTemplateGuid;
    ocrEdtTemplateCreate(&readerEdtTemplateGuid, readerEdt, 0, 3);

    ocrGuid_t dbGuids [3] = {dbGuid, dbGuid, dbGuid};
    ocrGuid_t readerEdtGuid;
    ocrEdtCreate(&readerEdtGuid, readerEdtTemplateGuid, 0, NULL, 3, dbGuids,
                 EDT_PROP_NONE, NULL_HINT, NULL);

    return NULL_GUID;
}
