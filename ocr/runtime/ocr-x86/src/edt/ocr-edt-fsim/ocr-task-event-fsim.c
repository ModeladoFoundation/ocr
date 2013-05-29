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

#include <stdlib.h>
#include <assert.h>

#include "fsim.h"
#include "ocr-datablock.h"
#include "ocr-utils.h"
#include "debug.h"
#include "ocr-macros.h"

#define END_OF_LIST NULL

void hcTaskConstructInternal2 (ocrTaskHc_t* derived, ocrEdt_t funcPtr,
        u32 paramc, u64 * params, void** paramv, size_t nbDeps, ocrGuid_t outputEvent, ocrTaskFcts_t * taskFctPtrs) {
    if (nbDeps == 0) {
        derived->signalers = END_OF_LIST;
    } else {
        // Since we know how many dependences we have, preallocate signalers
        derived->signalers = checked_malloc(derived->signalers, sizeof(reg_node_t)*nbDeps);
    }
    derived->waiters = END_OF_LIST;
    derived->nbdeps = nbDeps;
    derived->p_function = funcPtr;
    // Initialize base
    ocrTask_t* base = (ocrTask_t*) derived;
    base->guid = UNINITIALIZED_GUID;
    globalGuidProvider->getGuid(globalGuidProvider, &(base->guid), (u64)base, OCR_GUID_EDT);
    base->paramc = paramc;
    base->params = params;
    base->paramv = paramv;
    base->outputEvent = outputEvent;
    base->fct_ptrs = taskFctPtrs;
    // Initialize ELS
    int i = 0;
    while (i < ELS_SIZE) {
        base->els[i++] = NULL_GUID;
    }
}

int fsim_task_is_message ( fsim_message_interface_t* fsim_base ) {
    return 0;
}

void destructTaskFsim ( ocrTask_t* base ) {
    ocrTaskFsim_t* derived = (ocrTaskFsim_t*) base;
    free(derived);
}

ocrTaskFsim_t* newTaskFsimInternal (ocrEdt_t funcPtr, u32 paramc, u64 * params, void ** paramv, u16 properties, size_t depc, ocrGuid_t outputEvent, ocrTaskFcts_t * taskFcts) {
    ocrTaskFsim_t* derived = (ocrTaskFsim_t*) malloc(sizeof(ocrTaskFsim_t));
    ocrTaskHc_t* hcTaskBase = &(derived->fsimBase.base);

    hcTaskConstructInternal2(hcTaskBase, funcPtr, paramc, params, paramv, depc, outputEvent, taskFcts);

    fsim_message_interface_t* fsimMessage = &(derived->fsimBase.message_interface);
    fsimMessage->is_message = fsim_task_is_message;

    return derived;
}

void destructTaskFactoryFsim ( ocrTaskFactory_t* base ) {
    ocrTaskFactoryFsim_t* derived = (ocrTaskFactoryFsim_t*) base;
    free(derived);
}

ocrGuid_t newTaskFsim ( ocrTaskFactory_t* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, u16 properties, size_t depc, ocrGuid_t * outputEventPtr) {
    ocrTaskFsim_t* edt = newTaskFsimInternal(fctPtr, paramc, params, paramv, properties, depc, NULL_GUID, factory->taskFcts);
    ocrTask_t* base = (ocrTask_t*) edt;
    return base->guid;
}

ocrTaskFactory_t* newTaskFactoryFsim(void * config) {
    ocrTaskFactoryFsim_t* derived = (ocrTaskFactoryFsim_t*) malloc(sizeof(ocrTaskFactoryFsim_t));
    ocrTaskFactory_t* base = (ocrTaskFactory_t*) derived;
    base->instantiate = newTaskFsim;
    base->destruct =  destructTaskFactoryFsim;
    // initialize singleton instance that carries implementation function pointers
    base->taskFcts = (ocrTaskFcts_t *) checked_malloc(base->taskFcts, sizeof(ocrTaskFcts_t));
    base->taskFcts->destruct = destructTaskFsim;
    base->taskFcts->execute = taskExecute;
    base->taskFcts->schedule = tryScheduleTask;
    return base;
}

void destructTaskFactoryFsimMessage ( ocrTaskFactory_t* base ) {
    ocrTaskFactoryFsimMessage_t* derived = (ocrTaskFactoryFsimMessage_t*) base;
    free(derived);
}

int fsim_message_task_is_message ( fsim_message_interface_t* fsim_base ) {
    return 1;
}

void destructTaskFsimMessage ( ocrTask_t* base ) {
    ocrTaskFsim_t* derived = (ocrTaskFsim_t*) base;
    free(derived);
}

ocrTaskFsimMessage_t* newTaskFsimMessageInternal (ocrEdt_t funcPtr, ocrTaskFcts_t * taskFcts) {
    ocrTaskFsimMessage_t* derived = (ocrTaskFsimMessage_t*) malloc(sizeof(ocrTaskFsimMessage_t));
    ocrTaskHc_t* hcTaskBase = &(derived->fsimBase.base);

    hcTaskConstructInternal2(hcTaskBase, NULL, 0, NULL, NULL, 0, NULL_GUID, taskFcts);

    fsim_message_interface_t* fsimMessage = &(derived->fsimBase.message_interface);
    fsimMessage->is_message = fsim_message_task_is_message;
    return derived;
}

ocrGuid_t newTaskFsimMessage ( ocrTaskFactory_t* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, u16 properties, size_t depc, ocrGuid_t * outputEventPtr) {
    ocrTaskFsimMessage_t* edt = newTaskFsimMessageInternal(fctPtr, factory->taskFcts);
    ocrTask_t* base = (ocrTask_t*) edt;
    return base->guid;
}

ocrTaskFactory_t* newTaskFactoryFsimMessage(void * config) {
    ocrTaskFactoryFsimMessage_t* derived = (ocrTaskFactoryFsimMessage_t*) malloc(sizeof(ocrTaskFactoryFsimMessage_t));
    ocrTaskFactory_t* base = (ocrTaskFactory_t*) derived;
    base->instantiate = newTaskFsimMessage;
    base->destruct =  destructTaskFactoryFsimMessage;
    // initialize singleton instance that carries implementation function pointers
    base->taskFcts = (ocrTaskFcts_t *) checked_malloc(base->taskFcts, sizeof(ocrTaskFcts_t));
    base->taskFcts->destruct = destructTaskFsimMessage;
    base->taskFcts->execute = taskExecute;
    base->taskFcts->schedule = tryScheduleTask;
    return base;
}

void * xe_worker_computation_routine (void * arg) {
    ocr_worker_t * baseWorker = (ocr_worker_t *) arg;

    /* associate current thread with the worker */
    associate_comp_platform_and_worker(baseWorker);

    ocrGuid_t workerGuid = get_worker_guid(baseWorker);
    ocr_scheduler_t * xeScheduler = get_worker_scheduler(baseWorker);

    log_worker(INFO, "Starting scheduler routine of worker %d\n", get_worker_id(baseWorker));

    while ( baseWorker->is_running(baseWorker) ) {
        //
        // try to extract work from an XE's workpile of assigned-by-CE-tasks
        // as of now always of size 1
        ocrGuid_t taskGuid = xeScheduler->take(xeScheduler, workerGuid);

        if ( NULL_GUID != taskGuid ) {
            // if managed to find work that was assigned by CE, execute it
            ocrTask_t* currTask = NULL;
            globalGuidProvider->getVal(globalGuidProvider, taskGuid, (u64*)&(currTask), NULL);
            baseWorker->setCurrentEDT(baseWorker,taskGuid);
            currTask->fct_ptrs->execute(currTask);
            baseWorker->setCurrentEDT(baseWorker, NULL_GUID);
        } else {
            // TODO sagnak, this assumes (*A LOT*) the structure below, is this fair?
            // no assigned work found, now we have to create a 'message task'
            // by using our policy domain's message task factory
            ocr_policy_domain_t* policy_domain = xeScheduler->domain;
            ocrTaskFactory_t* message_task_factory = policy_domain->taskFactories[1];
            // the message to the CE says 'give me work' and notes who is asking for it
            ocrGuid_t messageTaskGuid = message_task_factory->instantiate(message_task_factory, NULL, 0, NULL, NULL, 0, 0, NULL);
            ocrTaskFsimMessage_t* derived = NULL;
            globalGuidProvider->getVal(globalGuidProvider, messageTaskGuid, (u64*)&(derived), NULL);
            derived -> type = GIVE_ME_WORK;
            derived -> from_worker_guid = workerGuid;

            // there is no work left and the message has been sent, so turn of the worker
            // baseWorker->stop(baseWorker);
            xe_worker_t* derivedWorker = (xe_worker_t*)baseWorker;
            pthread_mutex_lock(&derivedWorker->isRunningMutex);
            if ( baseWorker->is_running(baseWorker) ) {
                // give the work to the XE scheduler, which in turn should give it to the CE
                // through policy domain hand out and the scheduler differentiates tasks by type (RTTI) like
                xeScheduler->give(xeScheduler, workerGuid, messageTaskGuid);
                pthread_cond_wait(&derivedWorker->isRunningCond, &derivedWorker->isRunningMutex);
            }
            pthread_mutex_unlock(&derivedWorker->isRunningMutex);
        }
    }
    return NULL;
}

void * ce_worker_computation_routine(void * arg) {
    ocr_worker_t * ceWorker = (ocr_worker_t *) arg;
    ocr_scheduler_t * ceScheduler = get_worker_scheduler(ceWorker);

    /* associate current thread with the worker */
    associate_comp_platform_and_worker(ceWorker);

    ocrGuid_t ceWorkerGuid = get_worker_guid(ceWorker);
    log_worker(INFO, "Starting scheduler routine of worker %d\n", get_worker_id(ceWorker));
    while(ceWorker->is_running(ceWorker)) {

        // pop a 'message task' to handle messaging
        // the 'state' of the scheduler should point to the message workpile
        ocrGuid_t messageTaskGuid = ceScheduler->take(ceScheduler, ceWorkerGuid);

        // TODO sagnak, oh the assumptions, clean below
        if ( NULL_GUID != messageTaskGuid ) {

            // check if the popped task is of 'message task' type
            ocrTaskFsimBase_t* currTask = NULL;
            globalGuidProvider->getVal(globalGuidProvider, messageTaskGuid, (u64*)&(currTask), NULL);

            fsim_message_interface_t* taskAsMessage = &(currTask->message_interface);
            assert(taskAsMessage->is_message(taskAsMessage) && "hoping to pop a message-task; popped non-message task");

            // TODO sagnak, we seem to 'know' the underlying task type, not sure if this is a good idea
            ocrTaskFsimMessage_t* currMessageTask = (ocrTaskFsimMessage_t*) currTask;

            // TODO sagnak, we seem to 'know' the underlying scheduler type, not sure if this is a good idea
            ce_scheduler_t* derivedCeScheduler = (ce_scheduler_t*) ceScheduler;

            // find the worker, scheduler the message originated from
            ocrGuid_t targetWorkerGuid = currMessageTask->from_worker_guid;
            ocr_worker_t* targetWorker= NULL;
            globalGuidProvider->getVal(globalGuidProvider, targetWorkerGuid, (u64*)&(targetWorker), NULL);
            ocr_scheduler_t* targetScheduler = get_worker_scheduler(targetWorker);

            switch (currMessageTask->type) {
            case GIVE_ME_WORK:
            {
                // if the XE is asking for work
                // change the scheduler 'state' to 'executable task' popping
                derivedCeScheduler -> in_message_popping_mode = 0;
                // pop an 'executable task'
                ocrGuid_t toXeTaskGuid = ceScheduler->take(ceScheduler, ceWorkerGuid);
                // change the scheduler 'state' back to 'message task' popping
                derivedCeScheduler -> in_message_popping_mode = 1;

                if ( NULL_GUID != toXeTaskGuid ) {
                    targetScheduler->give(targetScheduler, ceWorkerGuid, toXeTaskGuid);
                    // now that the XE has work, it may be restarted
                    // targetWorker->start(targetWorker);
                    //
                    xe_worker_t* derivedWorker = (xe_worker_t*)targetWorker;
                    pthread_mutex_lock(&derivedWorker->isRunningMutex);
                    pthread_cond_signal(&derivedWorker->isRunningCond);
                    pthread_mutex_unlock(&derivedWorker->isRunningMutex);
                } else {
                    // if there was no successful task handing to XE, do not lose the message
                    ceScheduler->give(ceScheduler, ceWorkerGuid, messageTaskGuid);
                }
            }
            break;
            case PICK_MY_WORK_UP:
            {
                ocrGuid_t taskFromXe = targetScheduler->take( targetScheduler, ceWorkerGuid );
                if ( NULL_GUID != taskFromXe ) {
                    ceScheduler->give(ceScheduler, ceWorkerGuid, taskFromXe);
                } else {
                    // if there was no successful task picking from XE, do not lose the message
                    ceScheduler->give(ceScheduler, ceWorkerGuid, messageTaskGuid);
                }
            }
            break;
            };

            // ceWorker->setCurrentEDT(ceWorker,messageTaskGuid);
            // curr_task->execute(curr_task);
            // ceWorker->setCurrentEDT(ceWorker, NULL_GUID);
        }
    }
    return NULL;
}
