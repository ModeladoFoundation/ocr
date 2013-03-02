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

struct ocr_event_factory_struct* xe_event_factory_constructor(void) {
    xe_event_factory* derived = (xe_event_factory*) malloc(sizeof(xe_event_factory));
    ocr_event_factory* base = (ocr_event_factory*) derived;
    base->create = xe_event_factory_create;
    base->destruct =  xe_event_factory_destructor;
    return base;
}

void xe_event_factory_destructor ( struct ocr_event_factory_struct* base ) {
    xe_event_factory* derived = (xe_event_factory*) base;
    free(derived);
}

#define UNINITIALIZED_REGISTER_LIST ((register_list_node_t*) -1)
#define EMPTY_REGISTER_LIST (NULL)

void xe_event_destructor ( struct ocr_event_struct* base ) {
    xe_event_t* derived = (xe_event_t*)base;
    globalGuidProvider->releaseGuid(globalGuidProvider, base->guid);
    free(derived);
}

ocrGuid_t xe_event_get (struct ocr_event_struct* event) {
    xe_event_t* derived = (xe_event_t*)event;
    if ( derived->datum == UNINITIALIZED_GUID ) return ERROR_GUID;
    return derived->datum;
}

register_list_node_t* xe_event_compete_for_put ( xe_event_t* derived, ocrGuid_t data_for_put_id ) {
    assert ( derived->datum == UNINITIALIZED_GUID && "violated single assignment property for EDFs");

    volatile register_list_node_t* registerListOfEDF = NULL;

    derived->datum = data_for_put_id;
    registerListOfEDF = derived->register_list;
    while ( !__sync_bool_compare_and_swap( &(derived->register_list), registerListOfEDF, EMPTY_REGISTER_LIST)) {
        registerListOfEDF = derived->register_list;
    }
    return (register_list_node_t*) registerListOfEDF;
}

void xe_event_signal_waiters( register_list_node_t* task_id_list ) {
    register_list_node_t* curr = task_id_list;

    while ( UNINITIALIZED_REGISTER_LIST != curr ) {
        ocrGuid_t wid = ocr_get_current_worker_guid();
        ocrGuid_t curr_task_guid = curr->task_guid;

        ocr_task_t* curr_task = NULL;
        globalGuidProvider->getVal(globalGuidProvider, curr_task_guid, (u64*)&curr_task, NULL);

        if ( curr_task->iterate_waiting_frontier(curr_task) ) {
            ocr_worker_t* w = NULL;
            globalGuidProvider->getVal(globalGuidProvider, wid, (u64*)&w, NULL);

            ocr_scheduler_t * scheduler = get_worker_scheduler(w);
            scheduler->give(scheduler, wid, curr_task_guid);
        }
        curr = curr->next;
    }
}

void xe_event_put (struct ocr_event_struct* event, ocrGuid_t db) {
    xe_event_t* derived = (xe_event_t*)event;
    register_list_node_t* task_list = xe_event_compete_for_put(derived, db);
    xe_event_signal_waiters(task_list);
}

bool xe_event_register_if_not_ready(struct ocr_event_struct* event, ocrGuid_t polling_task_id ) {
    xe_event_t* derived = (xe_event_t*)event;
    bool success = false;
    volatile register_list_node_t* registerListOfEDF = derived -> register_list;

    if ( registerListOfEDF != EMPTY_REGISTER_LIST ) {
        register_list_node_t* new_node = (register_list_node_t*)malloc(sizeof(register_list_node_t));
        new_node->task_guid = polling_task_id;

        while ( registerListOfEDF != EMPTY_REGISTER_LIST && !success ) {
            new_node -> next = (register_list_node_t*) registerListOfEDF;

            success = __sync_bool_compare_and_swap(&(derived -> register_list), registerListOfEDF, new_node);

            if ( !success ) {
                registerListOfEDF = derived -> register_list;
            }
        }

    }
    return success;
}

struct ocr_event_struct* xe_event_constructor(ocrEventTypes_t eventType, bool takesArg) {
    xe_event_t* derived = (xe_event_t*) malloc(sizeof(xe_event_t));
    derived->datum = UNINITIALIZED_GUID;
    derived->register_list = UNINITIALIZED_REGISTER_LIST;
    ocr_event_t* base = (ocr_event_t*)derived;
    base->guid = UNINITIALIZED_GUID;

    globalGuidProvider->getGuid(globalGuidProvider, &(base->guid), (u64)base, OCR_GUID_EVENT);
    base->destruct = xe_event_destructor;
    base->get = xe_event_get;
    base->put = xe_event_put;
    base->registerIfNotReady = xe_event_register_if_not_ready;
    return base;
}

ocrGuid_t xe_event_factory_create ( struct ocr_event_factory_struct* factory, ocrEventTypes_t eventType, bool takesArg ) {
    //TODO LIMITATION Support other events types
    if (eventType != OCR_EVENT_STICKY_T) {
        assert("LIMITATION: Only sticky events are supported" && false);
    }
    // takesArg indicates whether or not this event carries any data
    // If not one can provide a more compact implementation
    //This would have to be different for different types of events
    //We can have a switch here and dispatch call to different event constructors
    ocr_event_t * res = xe_event_constructor(eventType, takesArg);
    return res->guid;
}

xe_await_list_t* xe_await_list_constructor( size_t al_size ) {
    xe_await_list_t* derived = (xe_await_list_t*)malloc(sizeof(xe_await_list_t));

    derived->array = malloc(sizeof(ocr_event_t*) * (al_size+1));
    derived->array[al_size] = NULL;
    derived->waitingFrontier = &derived->array[0];
    return derived;
}

xe_await_list_t* xe_await_list_constructor_with_event_list ( event_list_t* el) {
    xe_await_list_t* derived = (xe_await_list_t*)malloc(sizeof(xe_await_list_t));

    derived->array = malloc(sizeof(ocr_event_t*)*(el->size+1));
    derived->waitingFrontier = &derived->array[0];
    size_t i, size = el->size;
    event_list_node_t* curr = el->head;
    for ( i = 0; i < size; ++i, curr = curr -> next ) {
        derived->array[i] = curr->event;
    }
    derived->array[el->size] = NULL;
    return derived;
}

void xe_await_list_destructor( xe_await_list_t* derived ) {
    free(derived);
}

void xe_task_destruct ( ocr_task_t* base ) {
    xe_task_t* derived = (xe_task_t*)base;
    xe_await_list_destructor(derived->awaitList);
    globalGuidProvider->releaseGuid(globalGuidProvider, base->guid);
    free(derived);
}

bool xe_task_iterate_waiting_frontier ( ocr_task_t* base ) {
    xe_task_t* derived = (xe_task_t*)base;
    ocr_event_t** currEventToWaitOn = derived->awaitList->waitingFrontier;

    ocrGuid_t my_guid = base->guid;

    while (*currEventToWaitOn && !(*currEventToWaitOn)->registerIfNotReady (*currEventToWaitOn, my_guid) ) {
        ++currEventToWaitOn;
    }
    derived->awaitList->waitingFrontier = currEventToWaitOn;
    return *currEventToWaitOn == NULL;
}

void xe_task_schedule( ocr_task_t* base, ocrGuid_t wid ) {
    if ( base->iterate_waiting_frontier(base) ) {

        ocrGuid_t this_guid = base->guid;

        ocr_worker_t* w = NULL;
        globalGuidProvider->getVal(globalGuidProvider, wid, (u64*)&w, NULL);

        ocr_scheduler_t * scheduler = get_worker_scheduler(w);
        scheduler->give(scheduler, wid, this_guid);
    }
}

void xe_task_execute ( ocr_task_t* base ) {
    xe_task_t* derived = (xe_task_t*)base;
    ocr_event_t* curr = derived->awaitList->array[0];
    ocrDataBlock_t *db = NULL;
    ocrGuid_t dbGuid = UNINITIALIZED_GUID;
    size_t i = 0;
    //TODO this is computed for now but when we'll support slots
    //we will have to have the size when constructing the edt
    ocr_event_t* ptr = curr;
    while ( NULL != ptr ) {
        ptr = derived->awaitList->array[++i];
    };
    derived->nbdeps = i; i = 0;
    derived->depv = (ocrEdtDep_t *) malloc(sizeof(ocrEdtDep_t) * derived->nbdeps);
    while ( NULL != curr ) {
        dbGuid = curr->get(curr);
        derived->depv[i].guid = dbGuid;
        if(dbGuid != NULL_GUID) {
            globalGuidProvider->getVal(globalGuidProvider, dbGuid, (u64*)&db, NULL);

            derived->depv[i].ptr = db->acquire(db, base->guid, true);
        } else
            derived->depv[i].ptr = NULL;

        curr = derived->awaitList->array[++i];
    };
        derived->p_function(base->paramc, base->params, base->paramv, derived->nbdeps, derived->depv);

    // Now we clean up and release the GUIDs that we have to release
    for(i=0; i<derived->nbdeps; ++i) {
        if(derived->depv[i].guid != NULL_GUID) {
            globalGuidProvider->getVal(globalGuidProvider, derived->depv[i].guid, (u64*)&db, NULL);
            RESULT_ASSERT(db->release(db, base->guid, true), ==, 0);
        }
    }
}

void xe_task_add_dependence ( ocr_task_t* base, ocr_event_t* dep, size_t index ) {
    xe_task_t* derived = (xe_task_t*)base;
    derived->awaitList->array[index] = dep;
}

static void xe_task_construct_internal (xe_task_t* derived, ocrEdt_t funcPtr, u32 paramc, u64 * params, void** paramv) {
    derived->nbdeps = 0;
    derived->depv = NULL;
    derived->p_function = funcPtr;
    ocr_task_t* base = (ocr_task_t*) derived;
    base->guid = UNINITIALIZED_GUID;
    globalGuidProvider->getGuid(globalGuidProvider, &(base->guid), (u64)base, OCR_GUID_EDT);
    base->paramc = paramc;
    base->params = params;
    base->paramv = paramv;
    base->destruct = xe_task_destruct;
    base->iterate_waiting_frontier = xe_task_iterate_waiting_frontier;
    base->execute = xe_task_execute;
    base->schedule = xe_task_schedule;
    base->add_dependence = xe_task_add_dependence;
}

xe_task_t* xe_task_construct_with_event_list (ocrEdt_t funcPtr, u32 paramc, u64 * params, void** paramv, event_list_t* el) {
    xe_task_t* derived = (xe_task_t*)malloc(sizeof(xe_task_t));
    derived->awaitList = xe_await_list_constructor_with_event_list(el);
    xe_task_construct_internal(derived, funcPtr, paramc, params, paramv);
    return derived;
}

xe_task_t* xe_task_construct (ocrEdt_t funcPtr, u32 paramc, u64 * params, void** paramv, size_t dep_list_size) {
    xe_task_t* derived = (xe_task_t*)malloc(sizeof(xe_task_t));
    derived->awaitList = xe_await_list_constructor(dep_list_size);
    xe_task_construct_internal(derived, funcPtr, paramc, params, paramv);
    return derived;
}

struct ocr_event_factory_struct* ce_event_factory_constructor(void) {
    ce_event_factory* derived = (ce_event_factory*) malloc(sizeof(ce_event_factory));
    ocr_event_factory* base = (ocr_event_factory*) derived;
    base->create = ce_event_factory_create;
    base->destruct =  ce_event_factory_destructor;
    return base;
}

void ce_event_factory_destructor ( struct ocr_event_factory_struct* base ) {
    ce_event_factory* derived = (ce_event_factory*) base;
    free(derived);
}

void ce_event_destructor ( struct ocr_event_struct* base ) {
    ce_event_t* derived = (ce_event_t*)base;
    globalGuidProvider->releaseGuid(globalGuidProvider, base->guid);
    free(derived);
}

ocrGuid_t ce_event_get (struct ocr_event_struct* event) {
    ce_event_t* derived = (ce_event_t*)event;
    if ( derived->datum == UNINITIALIZED_GUID ) return ERROR_GUID;
    return derived->datum;
}

register_list_node_t* ce_event_compete_for_put ( ce_event_t* derived, ocrGuid_t data_for_put_id ) {
    assert ( derived->datum == UNINITIALIZED_GUID && "violated single assignment property for EDFs");

    volatile register_list_node_t* registerListOfEDF = NULL;

    derived->datum = data_for_put_id;
    registerListOfEDF = derived->register_list;
    while ( !__sync_bool_compare_and_swap( &(derived->register_list), registerListOfEDF, EMPTY_REGISTER_LIST)) {
        registerListOfEDF = derived->register_list;
    }
    return (register_list_node_t*) registerListOfEDF;
}

void ce_event_signal_waiters( register_list_node_t* task_id_list ) {
    register_list_node_t* curr = task_id_list;

    while ( UNINITIALIZED_REGISTER_LIST != curr ) {
        ocrGuid_t wid = ocr_get_current_worker_guid();
        ocrGuid_t curr_task_guid = curr->task_guid;

        ocr_task_t* curr_task = NULL;
        globalGuidProvider->getVal(globalGuidProvider, curr_task_guid, (u64*)&curr_task, NULL);

        if ( curr_task->iterate_waiting_frontier(curr_task) ) {
            ocr_worker_t* w = NULL;
            globalGuidProvider->getVal(globalGuidProvider, wid, (u64*)&w, NULL);

            ocr_scheduler_t * scheduler = get_worker_scheduler(w);
            scheduler->give(scheduler, wid, curr_task_guid);
        }
        curr = curr->next;
    }
}

void ce_event_put (struct ocr_event_struct* event, ocrGuid_t db) {
    ce_event_t* derived = (ce_event_t*)event;
    register_list_node_t* task_list = ce_event_compete_for_put(derived, db);
    ce_event_signal_waiters(task_list);
}

bool ce_event_register_if_not_ready(struct ocr_event_struct* event, ocrGuid_t polling_task_id ) {
    ce_event_t* derived = (ce_event_t*)event;
    bool success = false;
    volatile register_list_node_t* registerListOfEDF = derived -> register_list;

    if ( registerListOfEDF != EMPTY_REGISTER_LIST ) {
        register_list_node_t* new_node = (register_list_node_t*)malloc(sizeof(register_list_node_t));
        new_node->task_guid = polling_task_id;

        while ( registerListOfEDF != EMPTY_REGISTER_LIST && !success ) {
            new_node -> next = (register_list_node_t*) registerListOfEDF;

            success = __sync_bool_compare_and_swap(&(derived -> register_list), registerListOfEDF, new_node);

            if ( !success ) {
                registerListOfEDF = derived -> register_list;
            }
        }

    }
    return success;
}

struct ocr_event_struct* ce_event_constructor(ocrEventTypes_t eventType, bool takesArg) {
    ce_event_t* derived = (ce_event_t*) malloc(sizeof(ce_event_t));
    derived->datum = UNINITIALIZED_GUID;
    derived->register_list = UNINITIALIZED_REGISTER_LIST;
    ocr_event_t* base = (ocr_event_t*)derived;
    base->guid = UNINITIALIZED_GUID;

    globalGuidProvider->getGuid(globalGuidProvider, &(base->guid), (u64)base, OCR_GUID_EVENT);
    base->destruct = ce_event_destructor;
    base->get = ce_event_get;
    base->put = ce_event_put;
    base->registerIfNotReady = ce_event_register_if_not_ready;
    return base;
}

ocrGuid_t ce_event_factory_create ( struct ocr_event_factory_struct* factory, ocrEventTypes_t eventType, bool takesArg ) {
    //TODO LIMITATION Support other events types
    if (eventType != OCR_EVENT_STICKY_T) {
        assert("LIMITATION: Only sticky events are supported" && false);
    }
    // takesArg indicates whether or not this event carries any data
    // If not one can provide a more compact implementation
    //This would have to be different for different types of events
    //We can have a switch here and dispatch call to different event constructors
    ocr_event_t * res = ce_event_constructor(eventType, takesArg);
    return res->guid;
}

ce_await_list_t* ce_await_list_constructor( size_t al_size ) {
    ce_await_list_t* derived = (ce_await_list_t*)malloc(sizeof(ce_await_list_t));

    derived->array = malloc(sizeof(ocr_event_t*) * (al_size+1));
    derived->array[al_size] = NULL;
    derived->waitingFrontier = &derived->array[0];
    return derived;
}

ce_await_list_t* ce_await_list_constructor_with_event_list ( event_list_t* el) {
    ce_await_list_t* derived = (ce_await_list_t*)malloc(sizeof(ce_await_list_t));

    derived->array = malloc(sizeof(ocr_event_t*)*(el->size+1));
    derived->waitingFrontier = &derived->array[0];
    size_t i, size = el->size;
    event_list_node_t* curr = el->head;
    for ( i = 0; i < size; ++i, curr = curr -> next ) {
        derived->array[i] = curr->event;
    }
    derived->array[el->size] = NULL;
    return derived;
}

void ce_await_list_destructor( ce_await_list_t* derived ) {
    free(derived);
}

void ce_task_destruct ( ocr_task_t* base ) {
    ce_task_t* derived = (ce_task_t*)base;
    ce_await_list_destructor(derived->awaitList);
    globalGuidProvider->releaseGuid(globalGuidProvider, base->guid);
    free(derived);
}

bool ce_task_iterate_waiting_frontier ( ocr_task_t* base ) {
    ce_task_t* derived = (ce_task_t*)base;
    ocr_event_t** currEventToWaitOn = derived->awaitList->waitingFrontier;

    ocrGuid_t my_guid = base->guid;

    while (*currEventToWaitOn && !(*currEventToWaitOn)->registerIfNotReady (*currEventToWaitOn, my_guid) ) {
        ++currEventToWaitOn;
    }
    derived->awaitList->waitingFrontier = currEventToWaitOn;
    return *currEventToWaitOn == NULL;
}

void ce_task_schedule( ocr_task_t* base, ocrGuid_t wid ) {
    if ( base->iterate_waiting_frontier(base) ) {

        ocrGuid_t this_guid = base->guid;

        ocr_worker_t* w = NULL;
        globalGuidProvider->getVal(globalGuidProvider, wid, (u64*)&w, NULL);

        ocr_scheduler_t * scheduler = get_worker_scheduler(w);
        scheduler->give(scheduler, wid, this_guid);
    }
}

void ce_task_execute ( ocr_task_t* base ) {
    ce_task_t* derived = (ce_task_t*)base;
    ocr_event_t* curr = derived->awaitList->array[0];
    ocrDataBlock_t *db = NULL;
    ocrGuid_t dbGuid = UNINITIALIZED_GUID;
    size_t i = 0;
    //TODO this is computed for now but when we'll support slots
    //we will have to have the size when constructing the edt
    ocr_event_t* ptr = curr;
    while ( NULL != ptr ) {
        ptr = derived->awaitList->array[++i];
    };
    derived->nbdeps = i; i = 0;
    derived->depv = (ocrEdtDep_t *) malloc(sizeof(ocrEdtDep_t) * derived->nbdeps);
    while ( NULL != curr ) {
        dbGuid = curr->get(curr);
        derived->depv[i].guid = dbGuid;
        if(dbGuid != NULL_GUID) {
            globalGuidProvider->getVal(globalGuidProvider, dbGuid, (u64*)&db, NULL);

            derived->depv[i].ptr = db->acquire(db, base->guid, true);
        } else
            derived->depv[i].ptr = NULL;

        curr = derived->awaitList->array[++i];
    };
        derived->p_function(base->paramc, base->params, base->paramv, derived->nbdeps, derived->depv);

    // Now we clean up and release the GUIDs that we have to release
    for(i=0; i<derived->nbdeps; ++i) {
        if(derived->depv[i].guid != NULL_GUID) {
            globalGuidProvider->getVal(globalGuidProvider, derived->depv[i].guid, (u64*)&db, NULL);
            RESULT_ASSERT(db->release(db, base->guid, true), ==, 0);
        }
    }
}

void ce_task_add_dependence ( ocr_task_t* base, ocr_event_t* dep, size_t index ) {
    ce_task_t* derived = (ce_task_t*)base;
    derived->awaitList->array[index] = dep;
}

static void ce_task_construct_internal (ce_task_t* derived, ocrEdt_t funcPtr, u32 paramc, u64 * params, void** paramv) {
    derived->nbdeps = 0;
    derived->depv = NULL;
    derived->p_function = funcPtr;
    ocr_task_t* base = (ocr_task_t*) derived;
    base->guid = UNINITIALIZED_GUID;
    globalGuidProvider->getGuid(globalGuidProvider, &(base->guid), (u64)base, OCR_GUID_EDT);
    base->paramc = paramc;
    base->params = params;
    base->paramv = paramv;
    base->destruct = ce_task_destruct;
    base->iterate_waiting_frontier = ce_task_iterate_waiting_frontier;
    base->execute = ce_task_execute;
    base->schedule = ce_task_schedule;
    base->add_dependence = ce_task_add_dependence;
}

ce_task_t* ce_task_construct_with_event_list (ocrEdt_t funcPtr, u32 paramc, u64 * params, void** paramv, event_list_t* el) {
    ce_task_t* derived = (ce_task_t*)malloc(sizeof(ce_task_t));
    derived->awaitList = ce_await_list_constructor_with_event_list(el);
    ce_task_construct_internal(derived, funcPtr, paramc, params, paramv);
    return derived;
}

ce_task_t* ce_task_construct (ocrEdt_t funcPtr, u32 paramc, u64 * params, void** paramv, size_t dep_list_size) {
    ce_task_t* derived = (ce_task_t*)malloc(sizeof(ce_task_t));
    derived->awaitList = ce_await_list_constructor(dep_list_size);
    ce_task_construct_internal(derived, funcPtr, paramc, params, paramv);
    return derived;
}

void xe_task_factory_destructor ( struct ocr_task_factory_struct* base ) {
    xe_task_factory* derived = (xe_task_factory*) base;
    free(derived);
}

ocrGuid_t xe_task_factory_create_with_event_list (struct ocr_task_factory_struct* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, event_list_t* l) {
    xe_task_t* edt = xe_task_construct_with_event_list(fctPtr, paramc, params, paramv, l);
    ocr_task_t* base = (ocr_task_t*) edt;
    return base->guid;
}

ocrGuid_t xe_task_factory_create ( struct ocr_task_factory_struct* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, size_t dep_l_size) {
    xe_task_t* edt = xe_task_construct(fctPtr, paramc, params, paramv, dep_l_size);
    ocr_task_t* base = (ocr_task_t*) edt;
    return base->guid;
}

struct ocr_task_factory_struct* xe_task_factory_constructor(void) {
    xe_task_factory* derived = (xe_task_factory*) malloc(sizeof(xe_task_factory));
    ocr_task_factory* base = (ocr_task_factory*) derived;
    base->create = xe_task_factory_create;
    base->destruct =  xe_task_factory_destructor;
    return base;
}

void * xe_worker_computation_routine(void * arg) {
    ocr_worker_t * worker = (ocr_worker_t *) arg;
    /* associate current thread with the worker */
    associate_executor_and_worker(worker);
    ocrGuid_t workerGuid = get_worker_guid(worker);
    ocr_scheduler_t * scheduler = get_worker_scheduler(worker);
    log_worker(INFO, "Starting scheduler routine of worker %d\n", get_worker_id(worker));
    while(worker->is_running(worker)) {
        ocrGuid_t taskGuid = scheduler->take(scheduler, workerGuid);
        if (taskGuid != NULL_GUID) {
            ocr_task_t* curr_task = NULL;
            globalGuidProvider->getVal(globalGuidProvider, taskGuid, (u64*)&(curr_task), NULL);
            worker->setCurrentEDT(worker,taskGuid);
            curr_task->execute(curr_task);
            worker->setCurrentEDT(worker, NULL_GUID);
        }
    }
    return NULL;
}

void ce_task_factory_destructor ( struct ocr_task_factory_struct* base ) {
    ce_task_factory* derived = (ce_task_factory*) base;
    free(derived);
}

ocrGuid_t ce_task_factory_create_with_event_list (struct ocr_task_factory_struct* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, event_list_t* l) {
    ce_task_t* edt = ce_task_construct_with_event_list(fctPtr, paramc, params, paramv, l);
    ocr_task_t* base = (ocr_task_t*) edt;
    return base->guid;
}

ocrGuid_t ce_task_factory_create ( struct ocr_task_factory_struct* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, size_t dep_l_size) {
    ce_task_t* edt = ce_task_construct(fctPtr, paramc, params, paramv, dep_l_size);
    ocr_task_t* base = (ocr_task_t*) edt;
    return base->guid;
}

struct ocr_task_factory_struct* ce_task_factory_constructor(void) {
    ce_task_factory* derived = (ce_task_factory*) malloc(sizeof(ce_task_factory));
    ocr_task_factory* base = (ocr_task_factory*) derived;
    base->create = ce_task_factory_create;
    base->destruct =  ce_task_factory_destructor;
    return base;
}

void * ce_worker_computation_routine(void * arg) {
    ocr_worker_t * worker = (ocr_worker_t *) arg;
    /* associate current thread with the worker */
    associate_executor_and_worker(worker);
    ocrGuid_t workerGuid = get_worker_guid(worker);
    ocr_scheduler_t * scheduler = get_worker_scheduler(worker);
    log_worker(INFO, "Starting scheduler routine of worker %d\n", get_worker_id(worker));
    while(worker->is_running(worker)) {
        ocrGuid_t taskGuid = scheduler->take(scheduler, workerGuid);
        if (taskGuid != NULL_GUID) {
            ocr_task_t* curr_task = NULL;
            globalGuidProvider->getVal(globalGuidProvider, taskGuid, (u64*)&(curr_task), NULL);
            worker->setCurrentEDT(worker,taskGuid);
            curr_task->execute(curr_task);
            worker->setCurrentEDT(worker, NULL_GUID);
        }
    }
    return NULL;
}
