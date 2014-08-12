/*
 *  * This file is subject to the license agreement located in the file LICENSE
 *   * and cannot be distributed without it. This notice cannot be
 *    * removed or modified.
 *     */

#include "worker/worker-all.h"
#include "debug.h"

const char * worker_types[] = {
    "HC",
    "XE",
    "CE",
    NULL
};

const char * ocrWorkerType_types[] = {
    "single",
    "master",
    "slave",
    NULL
};

ocrWorkerFactory_t * newWorkerFactory(workerType_t type, ocrParamList_t *perType) {
    switch(type) {
#ifdef ENABLE_WORKER_XE
    case workerXe_id:
        return newOcrWorkerFactoryXe(perType);
#endif
#ifdef ENABLE_WORKER_CE
    case workerCe_id:
        return newOcrWorkerFactoryCe(perType);
#endif
#ifdef ENABLE_WORKER_HC
    case workerHc_id:
        return newOcrWorkerFactoryHc(perType);
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
    self->dpCtxt.task = NULL_GUID;
    self->dpCtxt.lb = 0;
    self->dpCtxt.ub = 0;
    self->dpCtxt.lock = 0;
    self->dpCtxt.active = 0;
    self->dpCtxt.curIndex = (u64)(-1);
}

