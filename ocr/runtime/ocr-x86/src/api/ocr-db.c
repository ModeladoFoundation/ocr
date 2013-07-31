/**
 * @brief Data-block implementation for OCR
 */

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "debug.h"
#include "ocr-allocator.h"
#include "ocr-datablock.h"
#include "ocr-db.h"
#include "ocr-policy-domain-getter.h"
#include "ocr-policy-domain.h"


#include <errno.h>

#if (__STDC_HOSTED__ == 1)
#include <string.h>
#endif

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
//#include "statistics/stats-llvm-callback.h"
//extern u8 startMemStat; 
#endif


u8 ocrDbCreate(ocrGuid_t *db, void** addr, u64 len, u16 flags,
               ocrGuid_t affinity, ocrInDbAllocator_t allocator) {

    // TODO: Currently location and allocator are ignored
    // ocrDataBlock_t *createdDb = newDataBlock(OCR_DATABLOCK_DEFAULT);
    // ocrDataBlock_t *createdDb = newDataBlock(OCR_DATABLOCK_PLACED);

    // TODO: I need to get the current policy to figure out my allocator.
    // Replace with allocator that is gotten from policy
    //
    ocrPolicyDomain_t* policy = getCurrentPD();
    ocrPolicyCtx_t* ctx = getCurrentWorkerContext();
    // TODO: Pass ocrInDbAllocator_t down to the DB factory
    policy->allocateDb(
        policy, db, addr, len, flags, affinity, allocator, ctx);

    ocrDataBlock_t* createdDb;
    deguidify(policy, *db, (u64*)&createdDb, NULL);

#ifdef OCR_ENABLE_STATISTICS
    ocrStats_t * stats = policy->getStats(policy);
    // Create the statistics process for this DB.
    stats->fctPtrs->createStatsProcess(stats, *db,
                                       &(createdDb->statProcess));
#endif

    ocrGuid_t edtGuid = getCurrentEDT();
#ifdef OCR_ENABLE_STATISTICS
    {
        ocrTask_t *task = NULL;
        deguidify(policy, edtGuid, (u64*)&task, NULL);
        ocrStatsProcess_t *srcProcess = edtGuid==0?NULL:&(task->statProcess);
        statsParamDb_t params = {.offset = 0, .size = len,
                                 .isWrite = 1};
        
        ocrStatsMessage_t *mess = stats->fctPtrs->createMessage(
            stats, STATS_DB_CREATE, 0, edtGuid, createdDb->guid,
            (ocrStatsParam_t*)&params);
        
        ocrStatsAsyncMessage(srcProcess, &(createdDb->statProcess), mess);
        
        // Acquire part
        *addr = createdDb->fctPtrs->acquire(createdDb, edtGuid, false);
        ocrStatsMessage_t *mess2 = stats->fctPtrs->createMessage(
            stats, STATS_DB_ACQ, 0, edtGuid, createdDb->guid, (ocrStatsParam_t*)&params);
        ocrStatsSyncMessage(srcProcess, &(createdDb->statProcess), mess2);
    }
#else
    *addr = createdDb->fctPtrs->acquire(createdDb, edtGuid, false);
#endif
    if(*addr == NULL) return ENOMEM;

    *db = createdDb->guid;

    // if(startMemStat)
    // {
    //     ocrTask_t *curTask = NULL;
    //     ocrWorker_t *worker = NULL;
    //     deguidify(getCurrentPD(), getCurrentWorkerContext()->sourceObj, (u64*)&worker, NULL);
    //     ocrGuid_t curTaskGuid = worker->fctPtrs->getCurrentEDT(worker);
    //     deguidify(getCurrentPD(), curTaskGuid, (u64*)&curTask, NULL);

    //     //deguidify(getCurrentPD(), getCurrentEDT(), (u64*)&curTask, NULL);
    //     addQueue(db, *addr, len, curTask);
    // }

    return 0;
}

u8 ocrDbDestroy(ocrGuid_t db) {
    ocrDataBlock_t *dataBlock = NULL;
    ocrPolicyDomain_t *policy = getCurrentPD();

    deguidify(policy, db, (u64*)&dataBlock, NULL);

    ocrGuid_t edtGuid = getCurrentEDT();
#ifdef OCR_ENABLE_STATISTICS
    {
        ocrTask_t *task = NULL;
        ocrDataBlock_t *dataBlock = NULL;
        deguidify(policy, edtGuid, (u64*)&task, NULL);
        deguidify(policy, db, (u64*)&dataBlock, NULL);

        ocrStatsProcess_t *srcProcess = edtGuid==0?NULL:&(task->statProcess);

        ocrStatsMessage_t *mess = policy->getStats(policy)->fctPtrs->createMessage(
            policy->getStats(policy), STATS_DB_DESTROY, 0, edtGuid, db, NULL);
        ocrStatsAsyncMessage(srcProcess, &(dataBlock->statProcess), mess);
    }
#endif
    // Make sure you do the free *AFTER* sending the message because the free could
    // destroy the datablock (and the stat process).
    u8 status = dataBlock->fctPtrs->free(dataBlock, edtGuid);
    return status;
}

u8 ocrDbAcquire(ocrGuid_t db, void** addr, u16 flags) {
    ocrDataBlock_t *dataBlock = NULL;
    ocrPolicyDomain_t *policy = getCurrentPD();
    deguidify(policy, db, (u64*)&dataBlock, NULL);

    ocrGuid_t edtGuid = getCurrentEDT();

    *addr = dataBlock->fctPtrs->acquire(dataBlock, edtGuid, false);
#ifdef OCR_ENABLE_STATISTICS
    {
        ocrTask_t *task = NULL;
        deguidify(policy, edtGuid, (u64*)&task, NULL);

        ocrStatsProcess_t *srcProcess = edtGuid==0?NULL:&(task->statProcess);

        statsParamDb_t params = {.offset = 0, .size = dataBlock->size,
                                 .isWrite = 1};

        ocrStatsMessage_t *mess = policy->getStats(policy)->fctPtrs->createMessage(
            policy->getStats(policy), STATS_DB_ACQ, 0, edtGuid, db,
            (ocrStatsParam_t*)&params);
        ocrStatsSyncMessage(srcProcess, &(dataBlock->statProcess), mess);
    }
#endif
    if(*addr == NULL) return EPERM;
    return 0;
}

u8 ocrDbRelease(ocrGuid_t db) {
    ocrDataBlock_t *dataBlock = NULL;
    ocrPolicyDomain_t *policy = getCurrentPD();
    deguidify(policy, db, (u64*)&dataBlock, NULL);

    ocrGuid_t edtGuid = getCurrentEDT();
#ifdef OCR_ENABLE_STATISTICS
    {
        ocrTask_t *task = NULL;
        
        u8 result = dataBlock->fctPtrs->release(dataBlock, edtGuid, false);

        deguidify(policy, edtGuid, (u64*)&task, NULL);

        ocrStatsProcess_t *srcProcess = edtGuid==0?NULL:&(task->statProcess);

        statsParamDb_t params = {.offset = 0, .size = dataBlock->size,
                                 .isWrite = 0};

        ocrStatsMessage_t *mess = policy->getStats(policy)->fctPtrs->createMessage(
            policy->getStats(policy), STATS_DB_REL, 0, edtGuid, db,
            (ocrStatsParam_t*)&params);
        
        ocrStatsAsyncMessage(srcProcess, &(dataBlock->statProcess), mess);
    
        return result;
    }
#else
    return dataBlock->fctPtrs->release(dataBlock, edtGuid, false);
#endif
}

u8 ocrDbMalloc(ocrGuid_t guid, u64 size, void** addr) {
    return EINVAL; /* not yet implemented */
}

u8 ocrDbMallocOffset(ocrGuid_t guid, u64 size, u64* offset) {
    return EINVAL; /* not yet implemented */
}

struct ocrDbCopy_args {
        ocrGuid_t destination;
        u64 destinationOffset;
        ocrGuid_t source;
        u64 sourceOffset;
        u64 size;
} ocrDbCopy_args;

// TODO: Re-enable
/*
ocrGuid_t ocrDbCopy_edt ( u32 paramc, u64 * params, void* paramv[], u32 depc, ocrEdtDep_t depv[]) {
    char *sptr, *dptr;
    struct ocrDbCopy_args * pv = (struct ocrDbCopy_args *) depv[0].ptr;
    ocrGuid_t destination = pv->destination;
    u64 destinationOffset = pv->destinationOffset;
    ocrGuid_t source = pv->source;
    u64 sourceOffset = pv->sourceOffset;
    u64 size = pv->size;

    ocrDbAcquire(source, (void *) &sptr, 0);
    ocrDbAcquire(destination, (void *) &dptr, 0);

    sptr += sourceOffset;
    dptr += destinationOffset;

#if (__STDC_HOSTED__ == 1)
    memcpy((void *)dptr, (const void *)sptr, size);
#else
    int i;
    for (i = 0; i < size; i++) {
        dptr[i] = sptr[i];
    }
#endif

    ocrDbRelease(source);
    ocrDbRelease(destination);

    ocrGuid_t paramDbGuid = (ocrGuid_t)depv[0].guid;
    ocrDbDestroy(paramDbGuid);

    return NULL_GUID;
}
*/

u8 ocrDbCopy(ocrGuid_t destination, u64 destinationOffset, ocrGuid_t source,
             u64 sourceOffset, u64 size, u64 copyType, ocrGuid_t *completionEvt) {
    ASSERT(0);

    // // Create the event
    // ocrGuid_t eventGuid;
    // ocrEventCreate(&eventGuid, OCR_EVENT_STICKY_T, TRUE); /*TODO: Replace with ONCE after that is supported */

    // // Create the EDT
    // ocrGuid_t edtGuid;
    // ocrEdtCreate(&edtGuid, ocrDbCopy_edt, 0, NULL, NULL, 0, 1, &eventGuid, completionEvt);
    // ocrEdtSchedule(edtGuid);

    // // Create the copy params
    // ocrGuid_t paramDbGuid;
    // struct ocrDbCopy_args * dbArgs = NULL;
    // void * ptr = (void *) dbArgs;
    // // Warning: directly casting dbArgs to (void **) causes a type-punning warning with gcc-4.1.2
    // ocrDbCreate(&paramDbGuid, &ptr, sizeof(ocrDbCopy_args), 0xdead, NULL, NO_ALLOC);

    // dbArgs->destination = destination;
    // dbArgs->destinationOffset = destinationOffset;
    // dbArgs->source = source;
    // dbArgs->sourceOffset = sourceOffset;
    // dbArgs->size = size;

    // ocrEventSatisfy(eventGuid, paramDbGuid);

    // /* ocrDbRelease(paramDbGuid); TODO: BUG: Release tries to free */

    return 0;
}

u8 ocrDbFree(ocrGuid_t guid, void* addr) {
    return EINVAL; /* not yet implemented */
}

u8 ocrDbFreeOffset(ocrGuid_t guid, u64 offset) {
    return EINVAL; /* not yet implemented */
}
