/* Copyright (c) 2012, Rice University

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1.  Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   2.  Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided
   with the distribution.
   3.  Neither the name of Intel Corporation
   nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific
   prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include <assert.h>

#include "ocr-edt.h"
#include "ocr-runtime.h"
#include "ocr-guid.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-stat-user.h"
#include "ocr-config.h"
#endif


u8 ocrEventCreate(ocrGuid_t *guid, ocrEventTypes_t eventType, bool takesArg) {
    ocrPolicyDomain_t* policy_domain = get_current_policy_domain();
    ocrEventFactory_t * eventFactory = policy_domain->getEventFactoryForUserEvents(policy_domain);
    *guid = eventFactory->instantiate(eventFactory, eventType, takesArg);
    return 0;
}

u8 ocrEventDestroy(ocrGuid_t eventGuid) {
    ocrEvent_t * event = NULL;
    globalGuidProvider->getVal(globalGuidProvider, eventGuid, (u64*)&event, NULL);
    event->fct_ptrs->destruct(event);
    return 0;
}

u8 ocrEventSatisfySlot(ocrGuid_t eventGuid, ocrGuid_t dataGuid /*= INVALID_GUID*/, int slot) {
    assert(eventGuid != NULL_GUID);
    ocrEvent_t * event = NULL;
    globalGuidProvider->getVal(globalGuidProvider, eventGuid, (u64*)&event, NULL);
    event->fct_ptrs->satisfy(event, dataGuid, slot);
    return 0;
}

u8 ocrEventSatisfy(ocrGuid_t eventGuid, ocrGuid_t dataGuid /*= INVALID_GUID*/) {
    return ocrEventSatisfySlot(eventGuid, dataGuid, 0);
}

u8 ocrEdtCreate(ocrGuid_t* edtGuid, ocrEdt_t funcPtr,
                u32 paramc, u64 * params, void** paramv,
                u16 properties, u32 depc, ocrGuid_t* depv /*= NULL*/, ocrGuid_t * outputEvent) {

    ocrPolicyDomain_t* policy_domain = get_current_policy_domain();
    ocrTaskFactory_t* taskFactory = policy_domain->getTaskFactoryForUserTasks(policy_domain);
    //TODO LIMITATION handle pre-built dependence vector
    *edtGuid = taskFactory->instantiate(taskFactory, funcPtr, paramc, params, paramv, properties, depc, outputEvent);
#ifdef OCR_ENABLE_STATISTICS
    // Create the statistics process for this EDT and also update clocks properly
    ocr_task_t *task = NULL;
    globalGuidProvider->getVal(globalGuidProvider, *edtGuid, (u64*)&task, NULL);
    ocrStatsProcessCreate(&(task->statProcess), *edtGuid);
    ocrStatsFilter_t *t = NEW_FILTER(simple);
    t->create(t, GocrFilterAggregator, NULL);
    ocrStatsProcessRegisterFilter(&(task->statProcess), (0x1F), t);

    // Now send the message that the EDT was created
    {
        ocr_worker_t *worker = NULL;
        ocr_task_t *curTask = NULL;

        globalGuidProvider->getVal(globalGuidProvider, ocr_get_current_worker_guid(), (u64*)&worker, NULL);
        ocrGuid_t curTaskGuid = worker->getCurrentEDT(worker);
        globalGuidProvider->getVal(globalGuidProvider, curTaskGuid, (u64*)&curTask, NULL);

        ocrStatsProcess_t *srcProcess = curTaskGuid==0?&GfakeProcess:&(curTask->statProcess);
        ocrStatsMessage_t *mess = NEW_MESSAGE(simple);
        mess->create(mess, STATS_EDT_CREATE, 0, curTaskGuid, *edtGuid, NULL);
        ocrStatsAsyncMessage(srcProcess, &(task->statProcess), mess);
    }
#endif

    // If guids dependencies were provided, add them now
    if(depv != NULL) {
        assert(depc != 0);
        u32 i = 0;
        while(i < depc) {
            // TODO replace with a single runtime call with all dependencies
            ocrAddDependence(depv[i], *edtGuid, i);
            i++;
        }
    }
    return 0;
}

//TODO DEPR: impacts edtCreate and addDependence
u8 ocrEdtSchedule(ocrGuid_t edtGuid) {
    ocrGuid_t worker_guid = ocr_get_current_worker_guid();
    ocrTask_t * task = NULL;
    globalGuidProvider->getVal(globalGuidProvider, edtGuid, (u64*)&task, NULL);
    task->fct_ptrs->schedule(task, worker_guid);
    return 0;
}

u8 ocrEdtDestroy(ocrGuid_t edtGuid) {
    ocrTask_t * task = NULL;
    globalGuidProvider->getVal(globalGuidProvider, edtGuid, (u64*)&task, NULL);
    task->fct_ptrs->destruct(task);
    return 0;
}

u8 ocrAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot) {
    registerDependence(source, destination, slot);
    return 0;
}

/**
   @brief Get @ offset in the currently running edt's local storage
   Note: not visible from the ocr user interface
 **/
ocrGuid_t ocrElsUserGet(u8 offset) {
    // User indexing start after runtime-reserved ELS slots
    offset = ELS_RUNTIME_SIZE + offset;
    ocrGuid_t workerGuid = ocr_get_current_worker_guid();
    ocrWorker_t * worker = NULL;
    globalGuidProvider->getVal(globalGuidProvider, workerGuid, (u64*)&(worker), NULL);
    ocrGuid_t edtGuid = worker->getCurrentEDT(worker);
    ocrTask_t * edt = NULL;
    globalGuidProvider->getVal(globalGuidProvider, edtGuid, (u64*)&(edt), NULL);
    return edt->els[offset];
}

/**
   @brief Set data @ offset in the currently running edt's local storage
   Note: not visible from the ocr user interface
 **/
void ocrElsUserSet(u8 offset, ocrGuid_t data) {
    // User indexing start after runtime-reserved ELS slots
    offset = ELS_RUNTIME_SIZE + offset;
    ocrGuid_t workerGuid = ocr_get_current_worker_guid();
    ocrWorker_t * worker = NULL;
    globalGuidProvider->getVal(globalGuidProvider, workerGuid, (u64*)&(worker), NULL);
    ocrGuid_t edtGuid = worker->getCurrentEDT(worker);
    ocrTask_t * edt = NULL;
    globalGuidProvider->getVal(globalGuidProvider, edtGuid, (u64*)&(edt), NULL);
    edt->els[offset] = data;
}
