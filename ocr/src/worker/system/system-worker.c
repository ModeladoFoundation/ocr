/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKER_SYSTEM

#include "debug.h"
#include "ocr-comp-platform.h"
#include "ocr-db.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "worker/hc/hc-worker.h"
#include "worker/system/system-worker.h"
#include "utils/deque.h"
#include "utils/tracer/tracer.h"
#include "utils/tracer/trace-events.h"

#define DEBUG_TYPE WORKER

/*********************************************************/
/* OCR-System WORKER
 *
 * @brief: Worker type responsible for monitoring runtime
 * events and reporting them through a tracing protocol.
 */
/*********************************************************/

#define IDX_OFFSET OCR_TRACE_TYPE_EDT

//Utility functions
bool allDequesEmpty(ocrPolicyDomain_t *pd){
    u32 i;
    for(i = 0; i < ((pd->workerCount)-1); i++){
        s32 head = ((ocrWorkerHc_t *)pd->workers[i])->sysDeque->head;
        s32 tail = ((ocrWorkerHc_t *)pd->workers[i])->sysDeque->tail;

        if(tail!=head)
            return false;
    }
    return true;
}

//Do custom processing for trace objects if provided by DPRINTF.
#ifdef OCR_TRACE_BINARY
void processTraceObject(ocrTraceObj_t *trace, FILE *f){

    fwrite(trace, sizeof(ocrTraceObj_t), 1, f);
    return;
}

static void drainSysDeques(FILE *f){
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    u32 i;
    //Iterate over each worker's deque
    for(i = 0; i < ((pd->workerCount)-1); i++){
        deque_t *deq = ((ocrWorkerHc_t *)pd->workers[i])->sysDeque;
        s32 head = deq->head;
        s32 tail = deq->tail;
        s32 remaining = tail-head;
        u32 j;
        for(j = 0; j < remaining; j++){
            //Pop and process all remaining records
            ocrTraceObj_t *tr = (ocrTraceObj_t *)(deq->popFromHead(deq,0));
            ASSERT(tr != NULL);

            processTraceObject(tr, f);
            pd->fcts.pdFree(pd, tr);
        }
    }
}
#endif

void drainCurrentDeque(ocrWorker_t *worker, FILE *f, s32 head, s32 tail, deque_t *deq){
    s32 remaining = tail-head;
    u32 i;
    for(i = 0; i < remaining; i++){
        ocrTraceObj_t *tr = (ocrTraceObj_t *)(deq->popFromHead(deq, 0));
        ASSERT(tr != NULL);
#ifdef OCR_TRACE_BINARY
        processTraceObject(tr, f);
#endif
        worker->pd->fcts.pdFree(worker->pd, tr);
    }
}

//workLoop for system worker: strictly responsible for querying/processing records in trace queues.
void workerLoopSystem(ocrWorker_t *worker){

    ASSERT(worker->curState == GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK))));

#ifdef OCR_TRACE_BINARY
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    u64 location = pd->myLocation;

    char traceName[32];
    SNPRINTF(traceName, 31, "trace_%lu.bin", location);

    //open file for binary writing. (One per policy domain)
    FILE *f = fopen(traceName, "w");
#endif

    u8 continueLoop = true;
    bool toDrain = false;

    do {
        while(worker->curState == worker->desiredState){
            u32 i;
            //Iterate over all comp. workers trace deques and pop records if they exist.
            for(i = 0; i < ((worker->pd->workerCount)-1); i++){
                //WARNING:  Broken abstraction.  Currently only supported on x86 so system worker
                //          looks directly into hc-worker. See bug #830
                deque_t *deq = ((ocrWorkerHc_t *)worker->pd->workers[i])->sysDeque;
                s32 head = deq->head;
                s32 tail = deq->tail;
                if(tail-head > 0){
                    //Atleast one trace object on deque.  Drain.
#ifdef OCR_TRACE_BINARY
                    drainCurrentDeque(worker, f, head, tail, deq);
#endif
                }
            }
        }

        ((ocrWorkerSystem_t *)worker)->readyForShutdown = true;
        switch(GET_STATE_RL(worker->desiredState)){
        case RL_USER_OK: {
            u8 desiredPhase = GET_STATE_PHASE(worker->desiredState);
            ASSERT(desiredPhase != RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK));
            ASSERT(worker->callback != NULL);
            worker->curState = GET_STATE(RL_USER_OK, desiredPhase);
            worker->callback(worker->pd, worker->callbackArg);
            break;
        }

        case RL_COMPUTE_OK: {


            u8 phase = GET_STATE_PHASE(worker->desiredState);
            if(RL_IS_FIRST_PHASE_DOWN(worker->pd, RL_COMPUTE_OK, phase)) {
                worker->curState = worker->desiredState;
                //We have succesfully shifted out of USER runlevel, and are breaking out of
                //our workLoop... Check if deques still contain items.
                if(!(allDequesEmpty(worker->pd))){
                    toDrain = true;
                }

                if(worker->callback != NULL){
                    worker->callback(worker->pd, worker->callbackArg);
                }

                continueLoop = false;

            }else{
                ASSERT(0);
            }
            break;
        }
        default:
            ASSERT(0);
        }
    } while(continueLoop);

    if(toDrain){

#ifdef OCR_TRACE_BINARY
        drainSysDeques(f);
        fclose(f);
#endif

    }


}

extern __thread bool inside_trace;

u8 systemWorkerSwitchRunlevel(ocrWorker_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                            phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t *, u64), u64 val)
{
    u8 toReturn = 0;
    ocrWorkerSystem_t *sysWorker = (ocrWorkerSystem_t *) self;
    switch(runlevel) {
        case RL_CONFIG_PARSE:
        case RL_NETWORK_OK:
        case RL_PD_OK:
        case RL_MEMORY_OK:
        case RL_GUID_OK:
        case RL_COMPUTE_OK:
            sysWorker->baseSwitchRunlevel(self, PD, runlevel, phase, properties, callback, val);
            break;
        case RL_USER_OK:
        {

            toReturn |= self->computes[0]->fcts.switchRunlevel(self->computes[0], PD, runlevel, phase, properties,
                                                                   NULL, 0);
            if(properties & RL_BRING_UP){
                if(RL_IS_LAST_PHASE_UP(PD, RL_USER_OK, phase)){
                    if(!(properties & RL_PD_MASTER)){
                        hal_fence();
                        self->desiredState = GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK)));
                    }else{

                        self->curState = self->desiredState = GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(PD, RL_USER_OK)));
                        workerLoopSystem(self);
                    }
                }

            }else if(properties & RL_TEAR_DOWN){
                sysWorker->baseSwitchRunlevel(self, PD, runlevel, phase, properties, callback, val);
            }
            break;
        }
        default:
            ASSERT(0);
    }

    return toReturn;
}

void* systemRunWorker(ocrWorker_t * worker){

    inside_trace = true; // The system worker should never output any traces

    ASSERT(worker->callback != NULL);
    worker->callback(worker->pd, worker->callbackArg);

    worker->computes[0]->fcts.setCurrentEnv(worker->computes[0], worker->pd, worker);
    worker->curState = GET_STATE(RL_COMPUTE_OK, 0);

    while(worker->curState == worker->desiredState);

    ASSERT(worker->desiredState == GET_STATE(RL_USER_OK, (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_USER_OK))));

    worker->curState = worker->desiredState;
    workerLoopSystem(worker);

    ASSERT((worker->curState == worker->desiredState) &&
            (worker->curState == GET_STATE(RL_COMPUTE_OK, (RL_GET_PHASE_COUNT_DOWN(worker->pd, RL_COMPUTE_OK) - 1))));
    return NULL;

}


/**
 * @brief Builds instance of a System Worker
 */
ocrWorker_t* newWorkerSystem(ocrWorkerFactory_t * factory, ocrParamList_t * perInstance) {
    ocrWorker_t *base = (ocrWorker_t*)runtimeChunkAlloc(sizeof(ocrWorkerSystem_t), PERSISTENT_CHUNK);
    factory->initialize(factory, base, perInstance);
    return (ocrWorker_t *) base;
}

/**
 * @brief Deallocate worker's resources
 */
void destructWorkerSystem(ocrWorker_t *base) {
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
}

/**
 * @brief Initialize instance of a System worker
 */
void initializeWorkerSystem(ocrWorkerFactory_t * factory, ocrWorker_t * self, ocrParamList_t * perInstance){
    ocrWorkerFactorySystem_t * derivedFactory = (ocrWorkerFactorySystem_t *) factory;
    derivedFactory->baseInitialize(factory, self, perInstance);

    ocrWorkerHc_t * workerHc = (ocrWorkerHc_t *)self;
    workerHc->hcType = HC_WORKER_SYSTEM;

    initializeWorkerOcr(factory, self, perInstance);
    self->type = SYSTEM_WORKERTYPE;

    ocrWorkerSystem_t *workerSys = (ocrWorkerSystem_t *)self;
    workerSys->baseSwitchRunlevel = derivedFactory->baseSwitchRunlevel;
    workerSys->id = ((paramListWorkerInst_t *)perInstance)->workerId;

}

/******************************************************/
/* OCR-SYSTEM WORKER FACTORY                          */
/******************************************************/

void destructWorkerFactorySystem(ocrWorkerFactory_t * factory){
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrWorkerFactory_t * newOcrWorkerFactorySystem(ocrParamList_t * perType) {
    ocrWorkerFactory_t * baseFactory = newOcrWorkerFactoryHc(perType);
    ocrWorkerFcts_t baseFcts = baseFactory->workerFcts;


    ocrWorkerFactorySystem_t *derived = (ocrWorkerFactorySystem_t *)runtimeChunkAlloc(sizeof(ocrWorkerFactorySystem_t), NONPERSISTENT_CHUNK);
    ocrWorkerFactory_t *base = (ocrWorkerFactory_t *)derived;

    derived->baseInitialize = baseFactory->initialize;
    derived->baseSwitchRunlevel = baseFcts.switchRunlevel;

    base->instantiate = FUNC_ADDR(ocrWorker_t* (*)(ocrWorkerFactory_t*, ocrParamList_t*), newWorkerSystem);
    base->initialize = FUNC_ADDR(void (*) (ocrWorkerFactory_t*, ocrWorker_t*, ocrParamList_t*), initializeWorkerSystem);
    base->destruct = FUNC_ADDR(void (*)(ocrWorkerFactory_t*), destructWorkerFactorySystem);

    base->workerFcts = baseFcts;
    base->workerFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64),
                                                systemWorkerSwitchRunlevel);
    base->workerFcts.destruct = FUNC_ADDR(void (*) (ocrWorker_t *), destructWorkerSystem);
    base->workerFcts.run = FUNC_ADDR(void* (*) (ocrWorker_t *), systemRunWorker);
    baseFactory->destruct(baseFactory);
    return base;

}

#endif /* ENABLE_WORKER_SYSTEM */
