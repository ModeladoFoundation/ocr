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
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrEventFactory_t * eventFactory = getEventFactoryFromPd(pd);
    ocrEvent_t * event = eventFactory->instantiate(eventFactory, eventType, takesArg);
    *guid = event->guid;
    return 0;
}

u8 ocrEventDestroy(ocrGuid_t eventGuid) {
    ocrEvent_t * event = NULL;
    deguidify(getCurrentPD(), eventGuid, (u64*)&event, NULL);
    event->fctPtrs->destruct(event);
    return 0;
}

u8 ocrEventSatisfySlot(ocrGuid_t eventGuid, ocrGuid_t dataGuid /*= INVALID_GUID*/, int slot) {
    assert(eventGuid != NULL_GUID);
    ocrEvent_t * event = NULL;
    deguidify(getCurrentPD(), eventGuid, (u64*)&event, NULL);
    event->fctPtrs->satisfy(event, dataGuid, slot);
    return 0;
}

u8 ocrEventSatisfy(ocrGuid_t eventGuid, ocrGuid_t dataGuid /*= INVALID_GUID*/) {
    return ocrEventSatisfySlot(eventGuid, dataGuid, 0);
}

u8 ocrEdtCreate(ocrGuid_t* edtGuid, ocrEdt_t funcPtr,
                u32 paramc, u64 * params, void** paramv,
                u16 properties, u32 depc, ocrGuid_t* depv /*= NULL*/, ocrGuid_t * outputEvent) {

    ocrPolicyDomain_t * pd = getCurrentPD();
    //TODO the task template should be created in the user realm
    ocrTaskTemplateFactory_t* taskTemplateFactory = getTaskTemplateFactoryFromPd(pd);
    ocrTaskTemplate_t* taskTemplate = taskTemplateFactory->instantiate(taskTemplateFactory, funcPtr, paramc, depc);
    ocrTaskFactory_t* taskFactory = getTaskFactoryFromPd(pd);
    //TODO LIMITATION handle pre-built dependence vector
    ocrTask_t * task = taskFactory->instantiate(taskFactory, taskTemplate, params, paramv, properties, outputEvent);
    *edtGuid = task->guid;

#ifdef OCR_ENABLE_STATISTICS
    // Create the statistics process for this EDT and also update clocks properly
    ocrStatsProcessCreate(&(task->statProcess), *edtGuid);
    ocrStatsFilter_t *t = NEW_FILTER(simple);
    t->create(t, GocrFilterAggregator, NULL);
    ocrStatsProcessRegisterFilter(&(task->statProcess), (0x1F), t);

    // Now send the message that the EDT was created
    {
        ocrWorker_t *worker = NULL;
        ocrTask_t *curTask = NULL;

        deguidify(pd, getCurrentWorkerContext()->sourceObj, (u64*)&worker, NULL);
        ocrGuid_t curTaskGuid = worker->fctPtrs->getCurrentEDT(worker);
        deguidify(pd, curTaskGuid, (u64*)&curTask, NULL);

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
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrTask_t * task = NULL;
    deguidify(pd, edtGuid, (u64*)&task, NULL);
    task->fctPtrs->schedule(task);
    return 0;
}

u8 ocrEdtDestroy(ocrGuid_t edtGuid) {
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrTask_t * task = NULL;
    deguidify(pd, edtGuid, (u64*)&task, NULL);
    task->fctPtrs->destruct(task);
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
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrGuid_t edtGuid = getCurrentEDT();
    ocrTask_t * edt = NULL;
    deguidify(pd, edtGuid, (u64*)&(edt), NULL);
    return edt->els[offset];
}

/**
   @brief Set data @ offset in the currently running edt's local storage
   Note: not visible from the ocr user interface
 **/
void ocrElsUserSet(u8 offset, ocrGuid_t data) {
    // User indexing start after runtime-reserved ELS slots
    offset = ELS_RUNTIME_SIZE + offset;
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrGuid_t edtGuid = getCurrentEDT();
    ocrTask_t * edt = NULL;
    deguidify(pd, edtGuid, (u64*)&(edt), NULL);
    edt->els[offset] = data;
}
