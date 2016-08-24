/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */



#include "ocr-config.h"
//Temporary condition until x86-mpi uses the new scheduler by default
//or the x86 mpi-pause resume gets merged(keeps Jenkins happy)
#ifdef ENABLE_EXTENSION_PAUSE

#include "ocr-types.h"
#include "ocr-db.h"
#include "ocr-hal.h"
#include "ocr-sal.h"
#include "ocr-policy-domain.h"
#include "comp-platform/pthread/pthread-comp-platform.h"

//Bug 846: pqr-utils not platform independent (hc-pd assumed)
#include "policy-domain/hc/hc-policy.h"
#include "ocr-scheduler.h"
#include "ocr-scheduler-object.h"
#include "scheduler-object/deq/deq-scheduler-object.h"
#include "scheduler-object/wst/wst-scheduler-object.h"

//return guid of next EDT on specified worker's workpile
ocrGuid_t hcDumpNextEdt(ocrWorker_t *worker, u32 *size){
    ocrPolicyDomain_t *pd = worker->pd;
    ocrSchedulerObject_t *schedObj;
    ocrSchedulerObjectDeq_t *deqObj;
    ocrSchedulerObjectWst_t *wstObj;

    ocrTask_t *curTask = NULL;

    schedObj = (ocrSchedulerObject_t *)pd->schedulers[0]->rootObj;
    wstObj = (ocrSchedulerObjectWst_t *)schedObj;
    deqObj = (ocrSchedulerObjectDeq_t *)wstObj->deques[worker->id];

    u32 tail = (deqObj->deque->tail%INIT_DEQUE_CAPACITY);
    u32 deqSize = deqObj->deque->size(deqObj->deque);

    if(deqSize > 0){

        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
        ocrFatGuid_t fguid;
        // See BUG #928 on GUIDs
#if GUID_BIT_COUNT == 64
        fguid.guid.guid = deqObj->deque->data[tail-1];
#elif GUID_BIT_COUNT == 128
        fguid.guid.lower = deqObj->deque->data[tail-1];
        fguid.guid.upper = 0x0;
#endif
        fguid.metaDataPtr = NULL;

    #define PD_MSG (&msg)
    #define PD_TYPE PD_MSG_GUID_INFO
        msg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid.guid) = fguid.guid;
        PD_MSG_FIELD_IO(guid.metaDataPtr) = fguid.metaDataPtr;
        PD_MSG_FIELD_I(properties) = RMETA_GUIDPROP | KIND_GUIDPROP;
        RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
        ocrGuidKind msgKind = PD_MSG_FIELD_O(kind);
        curTask = (ocrTask_t *)PD_MSG_FIELD_IO(guid.metaDataPtr);
    #undef PD_MSG
    #undef PD_TYPE

        if(msgKind != OCR_GUID_EDT){
            return NULL_GUID;

        }else if(curTask != NULL){
            //One queried task per worker (next EDT guid)
            *size = 1;
            return curTask->guid;
        }

    }else{
        //One queried task per worker (next EDT guid)
        *size = 1;

        return NULL_GUID;
    }
    return NULL_GUID;
}


ocrGuid_t hcQueryNextEdts(ocrPolicyDomainHc_t *rself, void **result, u32 *qSize){
    u64 i;
    *qSize = 0;
    ocrGuid_t *deqGuids;
    ocrGuid_t dataDb;

    ocrDbCreate(&dataDb, (void **)&deqGuids, sizeof(ocrGuid_t)*(rself->base.workerCount), 0,
                NULL_HINT, 1234);

    for(i = 0; i < rself->base.workerCount; i++){
        u32 wrkrSize;
        deqGuids[i] = hcDumpNextEdt(rself->base.workers[i], &wrkrSize);
        *qSize = (*qSize) + wrkrSize;
    }

    *result = deqGuids;
    return dataDb;
}


ocrGuid_t hcQueryAllEdts(ocrPolicyDomainHc_t *rself, void **result, u32 *qsize){
    ocrPolicyDomain_t  *pd = &rself->base;
    ocrSchedulerObject_t *schedObj;
    ocrSchedulerObjectWst_t *wstObj;
    ocrSchedulerObjectDeq_t *deqObj;
    u64 dataBlockSize = 0;
    u32 i = 0;


    //Get number of runnable EDTs to create DB of proper size.
    for(i = 0; i < rself->base.workerCount; i++){
        schedObj = (ocrSchedulerObject_t *)pd->schedulers[0]->rootObj;
        wstObj = (ocrSchedulerObjectWst_t *)schedObj;
        deqObj = (ocrSchedulerObjectDeq_t *)wstObj->deques[i];

        u32 head = (deqObj->deque->head%INIT_DEQUE_CAPACITY);
        u32 tail = (deqObj->deque->tail%INIT_DEQUE_CAPACITY);
        u32 deqSize = tail-head;

        if(deqSize > 0){
            dataBlockSize += deqSize;
        }
    }
    ocrGuid_t *deqGuids;
    ocrGuid_t dataDb;
    u32 idxOffset = -1;

    ocrDbCreate(&dataDb, (void **)&deqGuids, sizeof(ocrGuid_t)*(dataBlockSize), 0,
                NULL_HINT, 1234);

    //Populate datablock with workpile EDTs.
    for(i = 0; i < rself->base.workerCount; i++){

        schedObj = (ocrSchedulerObject_t *)pd->schedulers[0]->rootObj;
        wstObj = (ocrSchedulerObjectWst_t *)schedObj;
        deqObj = (ocrSchedulerObjectDeq_t *)wstObj->deques[i];

        u32 head = (deqObj->deque->head%INIT_DEQUE_CAPACITY);
        u32 tail = (deqObj->deque->tail%INIT_DEQUE_CAPACITY);
        u32 deqSize = tail-head;

        if(deqSize > 0){
            u32 j;
            for(j = head; j < tail; j++){
                idxOffset++;
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
                ocrFatGuid_t fguid;
                fguid.guid = (ocrGuid_t)deqObj->deque->data[j];
                fguid.metaDataPtr = NULL;

            #define PD_MSG (&msg)
            #define PD_TYPE PD_MSG_GUID_INFO
                msg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_IO(guid.guid) = fguid.guid;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = fguid.metaDataPtr;
                PD_MSG_FIELD_I(properties) = RMETA_GUIDPROP | KIND_GUIDPROP;
                RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
                ocrGuidKind msgKind = PD_MSG_FIELD_O(kind);
                ocrTask_t *curTask = (ocrTask_t *)PD_MSG_FIELD_IO(guid.metaDataPtr);
            #undef PD_MSG
            #undef PD_TYPE

                if(msgKind != OCR_GUID_EDT){
                    deqGuids[idxOffset] = NULL_GUID;
                    continue;
                }else if(curTask != NULL){
                    deqGuids[idxOffset] = curTask->guid;
                }
            }
        }
    }

    *result = deqGuids;
    *qsize = dataBlockSize;
    return dataDb;
}

ocrGuid_t hcQueryPreviousDatablock(ocrPolicyDomainHc_t *self, void **result, u32 *qSize){
    *qSize = 0;
    ocrGuid_t *prevDb;
    ocrGuid_t dataDb;

    ocrDbCreate(&dataDb, (void **)&prevDb, sizeof(ocrGuid_t), 0,
                NULL_HINT, 1234);
    prevDb[0] = self->pqrFlags.prevDb;

    *qSize = 1;
    *result = prevDb;
    return dataDb;
}

#endif /* ENABLE_POLICY_DOMAIN_HC */
