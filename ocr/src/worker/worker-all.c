/*
 *  * This file is subject to the license agreement located in the file LICENSE
 *   * and cannot be distributed without it. This notice cannot be
 *    * removed or modified.
 *     */

#include "worker/worker-all.h"
#include "debug.h"

const char * worker_types[] = {
#ifdef ENABLE_WORKER_HC
   "HC",
#endif
#ifdef ENABLE_WORKER_HC_COMM
   "HC_COMM",
#endif
#ifdef ENABLE_WORKER_HC_COMM_MT
   "HC_COMM_MT",
#endif
#ifdef ENABLE_WORKER_XE
   "XE",
#endif
#ifdef ENABLE_WORKER_CE
   "CE",
#endif
#ifdef ENABLE_WORKER_SYSTEM
   "SYSTEM",
#endif
   NULL
};

const char * ocrWorkerType_types[] = {
    "single",
    "master",
    "slave",
    "system",
    NULL
};

ocrWorkerFactory_t * newWorkerFactory(workerType_t type, ocrParamList_t *perType) {
    switch(type) {
#ifdef ENABLE_WORKER_HC
    case workerHc_id:
      return newOcrWorkerFactoryHc(perType);
#endif
#ifdef ENABLE_WORKER_HC_COMM
    case workerHcComm_id:
      return newOcrWorkerFactoryHcComm(perType);
#endif
#ifdef ENABLE_WORKER_HC_COMM_MT
    case workerHcCommMT_id:
      return newOcrWorkerFactoryHcCommMT(perType);
#endif
#ifdef ENABLE_WORKER_XE
    case workerXe_id:
        return newOcrWorkerFactoryXe(perType);
#endif
#ifdef ENABLE_WORKER_CE
    case workerCe_id:
        return newOcrWorkerFactoryCe(perType);
#endif
#ifdef ENABLE_WORKER_SYSTEM
    case workerSystem_id:
        return newOcrWorkerFactorySystem(perType);
#endif
    default:
        ASSERT(0);
    }
    return NULL;
}

void initializeWorkerOcr(ocrWorkerFactory_t * factory, ocrWorker_t * self, ocrParamList_t *perInstance) {
    self->fguid.guid = UNINITIALIZED_GUID;
    self->fguid.metaDataPtr = self;
    self->pd = NULL;
    self->location = 0;
    self->curTask = NULL;
    self->fcts = factory->workerFcts;
    self->curState = self->desiredState = GET_STATE(RL_CONFIG_PARSE, 0);
    self->callback = NULL;
    self->callbackArg = 0ULL;
    self->id = ((paramListWorkerInst_t *) perInstance)->workerId;
#ifdef OCR_MONITOR_SCHEDULER
    self->isSeeking = false;
#endif
#ifdef ENABLE_RESILIENCY
    self->activeDepv = NULL;
    self->notifyLock = INIT_LOCK;
#endif
}

