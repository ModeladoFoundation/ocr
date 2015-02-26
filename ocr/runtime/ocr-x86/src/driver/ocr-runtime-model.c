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

#include "ocr-executor.h"
#include "ocr-low-workers.h"
#include "ocr-scheduler.h"
#include "ocr-policy.h"
#include "ocr-runtime-model.h"
#include "ocr-config.h"

/**!
 * Helper function to build a module mapping
 */
static ocr_module_mapping_t build_ocr_module_mapping(ocr_mapping_kind kind, ocr_module_kind from, ocr_module_kind to) {
    ocr_module_mapping_t res;
    res.kind = kind;
    res.from = from;
    res.to = to;
    return res;
}

/**!
 * Function pointer type to define ocr modules mapping functions
 */
typedef void (*ocr_map_module_fct) (ocr_module_kind from, ocr_module_kind to,
                                    size_t nb_instances_from,
                                    ocr_module_t ** instances_from,
                                    size_t nb_instances_to,
                                    ocr_module_t ** instances_to);

/**!
 * One-to-many mapping function.
 * Maps a single instance of kind 'from' to 'n' instances of kind 'to'.
 * ex: maps a single scheduler to several workers.
 */
static void map_modules_one_to_many(ocr_module_kind from, ocr_module_kind to,
                                    size_t nb_instances_from,
                                    ocr_module_t ** instances_from,
                                    size_t nb_instances_to,
                                    ocr_module_t ** instances_to) {
    assert(nb_instances_from == 1);
    size_t i;
    for(i=0; i < nb_instances_to; i++) {
        instances_to[i]->map_fct(instances_to[i], from, 1, (void **) &instances_from[0]);
    }
}

/**!
 * One-to-one mapping function.
 * Maps each instance of kind 'from' to each instance of kind 'to' one to one.
 * ex: maps each executor to each worker in a one-one fashion.
 */
static void map_modules_one_to_one(ocr_module_kind from, ocr_module_kind to,
                                   size_t nb_instances_from,
                                   ocr_module_t ** instances_from,
                                   size_t nb_instances_to,
                                   ocr_module_t ** instances_to) {
    assert(nb_instances_from == nb_instances_to);
    size_t i;
    for(i=0; i < nb_instances_to; i++) {
        instances_to[i]->map_fct(instances_to[i], from, 1, (void **) &instances_from[i]);
    }
}

/**!
 * Many-to-one mapping function.
 * Maps all instance of kind 'from' to each instance of kind 'to'.
 * ex: maps all workpiles to each individual scheduler available.
 */

static void map_modules_many_to_one(ocr_module_kind from, ocr_module_kind to,
                                    size_t nb_instances_from,
                                    ocr_module_t ** instances_from,
                                    size_t nb_instances_to,
                                    ocr_module_t ** instances_to) {
    size_t i;
    for(i=0; i < nb_instances_to; i++) {
        instances_to[i]->map_fct(instances_to[i], from, nb_instances_from, (void **) instances_from);
    }
}

/**!
 * Given a mapping kind, returns a function pointer
 * to the implementation of the mapping kind.
 */
static ocr_map_module_fct get_module_mapping_function (ocr_mapping_kind mapping_kind) {
    switch(mapping_kind) {
    case MANY_TO_ONE_MAPPING:
        return map_modules_many_to_one;
    case ONE_TO_ONE_MAPPING:
        return map_modules_one_to_one;
    case ONE_TO_MANY_MAPPING:
        return map_modules_one_to_many;
    default:
        assert(false && "Unknown module mapping function");
        break;
    }
    return NULL;
}


/**!
 * Data-structure that stores ocr modules instances of a policy domain.
 * Used as an internal data-structure when building policy domains so
 * that generic code can be written when there's a need to find a particular
 * module's instance backing array.
 */
typedef struct {
    ocr_module_kind kind;
    size_t nb_instances;
    void ** instances;
} ocr_module_instance;

/**!
 * Utility function that goes over modules_kinds and looks for
 * a particular 'kind' of module. Sets pointers to its
 * number of instances and instances backing array.
 */
static void resolve_module_instances(ocr_module_kind kind, size_t nb_module_kind,
                                     ocr_module_instance * modules_kinds, size_t * nb_instances, void *** instances) {
    size_t i;
    for(i=0; i < nb_module_kind; i++) {
        if (kind == modules_kinds[i].kind) {
            *nb_instances = modules_kinds[i].nb_instances;
            *instances = modules_kinds[i].instances;
            return;
        }
    }
    assert(false && "Cannot resolve modules instances");
}

ocr_model_t* newModel ( int kind, int nInstances, void * per_type_configuration, void ** per_instance_configuration ) {
    ocr_model_t * model = (ocr_model_t *) malloc(sizeof(ocr_model_t));
    model->kind = kind;
    model->nb_instances = nInstances;
    model->per_type_configuration = per_type_configuration;
    model->per_instance_configuration = per_instance_configuration;
    return model;
}

static ocr_model_policy_t* defaultModelPolicyConstructor ( size_t nb_policy_domains ) {
    ocr_model_policy_t * defaultPolicy =
        (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));
    defaultPolicy->model.kind = ocr_policy_default_kind;
    defaultPolicy->model.nb_instances = nb_policy_domains;
    defaultPolicy->model.per_type_configuration = NULL;
    defaultPolicy->model.per_instance_configuration = NULL;

    defaultPolicy->nb_scheduler_types = 1;
    defaultPolicy->nb_worker_types = 1;
    defaultPolicy->nb_executor_types = 1;
    defaultPolicy->nb_workpile_types = 1;
    defaultPolicy->numMemTypes = 1;
    defaultPolicy->numAllocTypes = 1;

    // Default allocator
    ocrAllocatorModel_t *defaultAllocator =
        (ocrAllocatorModel_t*)malloc(sizeof(ocrAllocatorModel_t));
    defaultAllocator->model.per_type_configuration = NULL;
    defaultAllocator->model.per_instance_configuration = NULL;
    defaultAllocator->model.kind = OCR_ALLOCATOR_DEFAULT;
    defaultAllocator->model.nb_instances = 1;
    defaultAllocator->sizeManaged = gHackTotalMemSize;

    defaultPolicy->allocators = defaultAllocator;

    return defaultPolicy;
}

static void** setDefaultModelSchedulerConfigurations ( size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    size_t index_config = 0, n_all_schedulers = nb_schedulers*nb_policy_domains;

    void** scheduler_configurations = malloc(sizeof(scheduler_configuration*)*n_all_schedulers);
    for ( index_config = 0; index_config < n_all_schedulers; ++index_config ) {
        scheduler_configurations[index_config] = (scheduler_configuration*) malloc(sizeof(scheduler_configuration));
        scheduler_configuration* curr_config = (scheduler_configuration*)scheduler_configurations[index_config];
        curr_config->worker_id_begin = ( index_config / nb_schedulers ) * nb_workers;
        curr_config->worker_id_end = ( index_config / nb_schedulers ) * nb_workers + nb_workers - 1;
    }
    return scheduler_configurations;
}

static void setDefaultModelSchedulers ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( ocr_scheduler_default_kind, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersRandomVictimLocalPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_RANDOMVICTIM_LOCALPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersRandomVictimDataLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_RANDOMVICTIM_DATALOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersRandomVictimEventLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_RANDOMVICTIM_EVENTLOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersCyclicVictimLocalPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_CYCLICVICTIM_LOCALPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersCyclicVictimDataLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_CYCLICVICTIM_DATALOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersCyclicVictimEventLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_CYCLICVICTIM_EVENTLOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersHierCyclicVictimLocalPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_HIERCYCLICVICTIM_LOCALPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersHierCyclicVictimDataLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_HIERCYCLICVICTIM_DATALOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersHierCyclicVictimEventLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_HIERCYCLICVICTIM_EVENTLOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersHierRandomVictimLocalPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_HIERRANDOMVICTIM_LOCALPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersHierRandomVictimDataLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_HIERRANDOMVICTIM_DATALOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersHierRandomVictimEventLocalityPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_HIERRANDOMVICTIM_EVENTLOCALITYPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setSchedulersSocketOnlyVictimUserSocketPush ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers ) {
    void** scheduler_configurations = setDefaultModelSchedulerConfigurations ( nb_policy_domains, nb_schedulers, nb_workers );
    defaultPolicy->schedulers = newModel( OCR_SCHEDULER_SOCKETONLYVICTIM_USERSOCKETPUSH, nb_schedulers, NULL, scheduler_configurations );
}

static void setDefaultModelWorkers ( ocr_model_policy_t * defaultPolicy, size_t nb_policy_domains, size_t nb_workers, int* bind_map ) {
    size_t index_config = 0, n_all_workers = nb_workers*nb_policy_domains;
    void** worker_configurations = malloc(sizeof(worker_configuration*)*n_all_workers );
    for ( index_config = 0; index_config < n_all_workers; ++index_config ) {
        worker_configurations[index_config] = (worker_configuration*) malloc(sizeof(worker_configuration));
        worker_configuration* curr_config = (worker_configuration*)worker_configurations[index_config];
        curr_config->worker_id = index_config;
        curr_config->cpu_id = (( NULL != bind_map ) ? bind_map[index_config]: -1); 
    }
    defaultPolicy->workers    = newModel( ocr_worker_default_kind, nb_workers, NULL, worker_configurations);
}

static inline void setDefaultModelWorkpiles ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_default_kind, nb_workpiles, NULL, NULL );
}

/*dequeish heap variaties*/
static inline void setDefaultModelWorkpilesDequishHeapStealLast ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_dequeish_heap_steal_last_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDequishHeapStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_dequeish_heap_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDequishHeapCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_dequeish_heap_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

/*event priority queue variaties*/
static inline void setDefaultModelWorkpilesEventPriorityQueueStealLast ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_priority_queue_steal_last_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventPriorityQueueStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_priority_queue_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventPriorityQueueCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_priority_queue_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventPriorityQueueStealSelfish ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_priority_queue_steal_selfish_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventPriorityQueueStealAltruistic ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_priority_queue_steal_altruistic_kind, nb_workpiles, NULL, NULL );
}

/*data priority queue variaties*/
static inline void setDefaultModelWorkpilesDataPriorityQueueStealLast ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_priority_queue_steal_last_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataPriorityQueueStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_priority_queue_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataPriorityQueueCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_priority_queue_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataPriorityQueueStealSelfish ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_priority_queue_steal_selfish_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataPriorityQueueStealAltruistic ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_priority_queue_steal_altruistic_kind, nb_workpiles, NULL, NULL );
}

/*user priority queue variaties*/
static inline void setDefaultModelWorkpilesUserPriorityQueueStealLast ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_priority_queue_steal_last_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesUserPriorityQueueStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_priority_queue_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesUserPriorityQueueCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_priority_queue_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesUserPriorityQueueStealAltruistic ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_priority_queue_steal_altruistic_kind, nb_workpiles, NULL, NULL );
}

/*event sorted priority queue variaties*/
static inline void setDefaultModelWorkpilesEventSortedPriorityQueueStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_sorted_priority_queue_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventSortedPriorityQueueCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_sorted_priority_queue_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventSortedPriorityQueueStealSelfish ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_sorted_priority_queue_steal_selfish_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesEventSortedPriorityQueueStealAltruistic ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_event_sorted_priority_queue_steal_altruistic_kind, nb_workpiles, NULL, NULL );
}

/*data sorted priority queue variaties*/
static inline void setDefaultModelWorkpilesDataSortedPriorityQueueStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_sorted_priority_queue_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataSortedPriorityQueueCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_sorted_priority_queue_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataSortedPriorityQueueStealSelfish ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_sorted_priority_queue_steal_selfish_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesDataSortedPriorityQueueStealAltruistic ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_data_sorted_priority_queue_steal_altruistic_kind, nb_workpiles, NULL, NULL );
}

/*user sorted priority queue variaties*/
static inline void setDefaultModelWorkpilesUserSortedPriorityQueueStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_sorted_priority_queue_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesUserSortedPriorityQueueCountingStealHalf ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_sorted_priority_queue_counting_steal_half_kind, nb_workpiles, NULL, NULL );
}

static inline void setDefaultModelWorkpilesUserSortedPriorityQueueStealAltruistic ( ocr_model_policy_t * defaultPolicy, size_t nb_workpiles ) {
    defaultPolicy->workpiles  = newModel( ocr_workpile_user_sorted_priority_queue_steal_altruistic_kind, nb_workpiles, NULL, NULL );
}


static inline void setDefaultModelMappings ( ocr_model_policy_t * defaultPolicy ) {
    // Defines how ocr modules are bound together
    size_t nb_module_mappings = 5;
    ocr_module_mapping_t * defaultMapping =
        (ocr_module_mapping_t *) malloc(sizeof(ocr_module_mapping_t) * nb_module_mappings);
    // Note: this doesn't bind modules magically. You need to have a mapping function defined
    //       and set in the targeted implementation (see ocr_scheduler_hc implementation for reference).
    //       These just make sure the mapping functions you have defined are called
    defaultMapping[0] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_WORKPILE, OCR_SCHEDULER);
    defaultMapping[1] = build_ocr_module_mapping(ONE_TO_ONE_MAPPING, OCR_WORKER, OCR_EXECUTOR);
    defaultMapping[2] = build_ocr_module_mapping(ONE_TO_MANY_MAPPING, OCR_SCHEDULER, OCR_WORKER);
    defaultMapping[3] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_MEMORY, OCR_ALLOCATOR);
    defaultMapping[4] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_SCHEDULER, OCR_POLICY);
    defaultPolicy->nb_mappings = nb_module_mappings;
    defaultPolicy->mappings = defaultMapping;
}

/**
 * Default policy has one scheduler and a configurable
 * number of workers, executors and workpiles
 */
ocr_model_policy_t * defaultOcrModelPolicy(size_t nb_policy_domains, size_t nb_schedulers,
                                           size_t nb_workers, size_t nb_executors, size_t nb_workpiles, int* bind_map) {

    ocr_model_policy_t * defaultPolicy = defaultModelPolicyConstructor(nb_policy_domains);

    setDefaultModelSchedulers (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
    setDefaultModelWorkers (defaultPolicy, nb_policy_domains, nb_workers, bind_map);
    setDefaultModelWorkpiles (defaultPolicy, nb_workpiles);

    defaultPolicy->executors  = newModel( ocr_executor_default_kind, nb_executors, NULL, NULL );
    defaultPolicy->memories   = newModel( OCR_LOWMEMORY_DEFAULT, 1, NULL, NULL );

    setDefaultModelMappings(defaultPolicy);
    return defaultPolicy;
}

ocr_model_policy_t * defaultOcrModelPolicyDequishHeap(size_t nb_policy_domains, size_t nb_schedulers,
                                           size_t nb_workers, size_t nb_executors, size_t nb_workpiles, int* bind_map) {

    ocr_model_policy_t * defaultPolicy = defaultModelPolicyConstructor(nb_policy_domains);

    setDefaultModelSchedulers (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
    setDefaultModelWorkers (defaultPolicy, nb_policy_domains, nb_workers, bind_map);
    setDefaultModelWorkpilesDequishHeapStealLast (defaultPolicy, nb_workpiles);

    defaultPolicy->executors  = newModel( ocr_executor_default_kind, nb_executors, NULL, NULL );
    defaultPolicy->memories   = newModel( OCR_LOWMEMORY_DEFAULT, 1, NULL, NULL );

    setDefaultModelMappings(defaultPolicy);
    return defaultPolicy;
}

ocr_model_policy_t * defaultOcrModelPolicyPriorityHeap(size_t nb_policy_domains, size_t nb_schedulers,
                                           size_t nb_workers, size_t nb_executors, size_t nb_workpiles, int* bind_map) {

    ocr_model_policy_t * defaultPolicy = defaultModelPolicyConstructor(nb_policy_domains);

    setSchedulersRandomVictimEventLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
    setDefaultModelWorkers (defaultPolicy, nb_policy_domains, nb_workers, bind_map);
    setDefaultModelWorkpilesEventPriorityQueueStealLast (defaultPolicy, nb_workpiles);

    defaultPolicy->executors  = newModel( ocr_executor_default_kind, nb_executors, NULL, NULL );
    defaultPolicy->memories   = newModel( OCR_LOWMEMORY_DEFAULT, 1, NULL, NULL );

    setDefaultModelMappings(defaultPolicy);
    return defaultPolicy;
}

ocr_model_policy_t * ocrModelPolicyCreator ( 
              enum md_file_workpile_policy workpile_policy, enum md_file_steal_victim_policy steal_victim_policy,
              enum md_file_victim_extract_policy victim_extract_policy, enum md_file_push_policy push_policy, enum md_file_priority_policy priority_policy,
              size_t nb_policy_domains, size_t nb_schedulers, size_t nb_workers, size_t nb_executors, size_t nb_workpiles, int* bind_map) {
    ocr_model_policy_t * defaultPolicy = defaultModelPolicyConstructor(nb_policy_domains);

    setDefaultModelWorkers (defaultPolicy, nb_policy_domains, nb_workers, bind_map);
    switch ( workpile_policy ) {
        case MD_DEQUE:
            switch( victim_extract_policy ) {
                case MD_STEAL_LAST:
                    setDefaultModelWorkpiles (defaultPolicy, nb_workpiles);
                    break;
                case MD_STEAL_ALTRUISTIC:
                case MD_STEAL_SELFISH:
                case MD_STEAL_HALF:
                case MD_COUNTING_STEAL_HALF:
                    assert( 0 && "half, counting half altruistic or selfish stealing can not be used with deque");
                    break;
                default:
                    assert(0 && "Invalid workpile choice");
                    break;
            };
            break;
        case MD_DEQUEISH_HEAP:
            switch( victim_extract_policy ) {
                case MD_STEAL_LAST:
                    setDefaultModelWorkpilesDequishHeapStealLast (defaultPolicy, nb_workpiles);
                    break;
                case MD_STEAL_HALF:
                    setDefaultModelWorkpilesDequishHeapStealHalf (defaultPolicy, nb_workpiles);
                    break;
                case MD_COUNTING_STEAL_HALF:
                    setDefaultModelWorkpilesDequishHeapCountingStealHalf (defaultPolicy, nb_workpiles);
                    break;
                case MD_STEAL_ALTRUISTIC:
                case MD_STEAL_SELFISH:
                    assert( 0 && "altruistic or selfish stealing can not be used with dequeish heap");
                    break;
                default:
                    assert(0 && "Invalid workpile choice");
                    break;
            };
            break;
        case MD_PRIORITY_QUEUE:
            switch( victim_extract_policy ) {
                case MD_STEAL_HALF:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventPriorityQueueStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataPriorityQueueStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserPriorityQueueStealHalf (defaultPolicy, nb_workpiles);
                            break;
                    };
                    break;
                case MD_COUNTING_STEAL_HALF:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventPriorityQueueCountingStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataPriorityQueueCountingStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserPriorityQueueCountingStealHalf (defaultPolicy, nb_workpiles);
                            break;
                    };
                case MD_STEAL_LAST:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventPriorityQueueStealLast (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataPriorityQueueStealLast (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserPriorityQueueStealLast (defaultPolicy, nb_workpiles);
                            break;
                    };
                case MD_STEAL_ALTRUISTIC:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventPriorityQueueStealAltruistic (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataPriorityQueueStealAltruistic (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserPriorityQueueStealAltruistic (defaultPolicy, nb_workpiles);
                            break;
                    };
                case MD_STEAL_SELFISH:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventPriorityQueueStealSelfish (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataPriorityQueueStealSelfish (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            assert( 0 && "selfish stealing can not be used with user priorities");
                            break;
                    };
                default:
                    assert(0 && "Invalid workpile choice");
                    break;
            };
            break;
        case MD_SORTED_PRIORITY_QUEUE:
            switch( victim_extract_policy ) {
                case MD_STEAL_HALF:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventSortedPriorityQueueStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataSortedPriorityQueueStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserSortedPriorityQueueStealHalf (defaultPolicy, nb_workpiles);
                            break;
                    };
                    break;
                case MD_COUNTING_STEAL_HALF:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventSortedPriorityQueueCountingStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataSortedPriorityQueueCountingStealHalf (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserSortedPriorityQueueCountingStealHalf (defaultPolicy, nb_workpiles);
                            break;
                    };
                    break;
                case MD_STEAL_LAST:
                    switch(priority_policy) {
                        default:
                            assert( 0 && "steal last is altruistic stealing for sorted workpiles, use that designation");
                            break;
                    };
                    break;
                case MD_STEAL_ALTRUISTIC:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventSortedPriorityQueueStealAltruistic (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataSortedPriorityQueueStealAltruistic (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            setDefaultModelWorkpilesUserSortedPriorityQueueStealAltruistic (defaultPolicy, nb_workpiles);
                            break;
                    };
                    break;
                case MD_STEAL_SELFISH:
                    switch(priority_policy) {
                        case MD_EVENT_PRIORITY:
                            setDefaultModelWorkpilesEventSortedPriorityQueueStealSelfish (defaultPolicy, nb_workpiles);
                            break;
                        case MD_DATA_PRIORITY:
                            setDefaultModelWorkpilesDataSortedPriorityQueueStealSelfish (defaultPolicy, nb_workpiles);
                            break;
                        case MD_USER_PRIORITY:
                            assert( 0 && "selfish stealing can not be used with user priorities");
                            break;
                    };
                    break;
                default:
                    assert(0 && "Invalid workpile choice");
                    break;
            };
            break;
    };

    switch ( steal_victim_policy ) {
        case MD_RANDOM_VICTIM:
            switch ( push_policy ) {
                case MD_LOCAL_PUSH:
                    setSchedulersRandomVictimLocalPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_DATA_LOCALITY_PUSH:
                    setSchedulersRandomVictimDataLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_EVENT_LOCALITY_PUSH:
                    setSchedulersRandomVictimEventLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                default:
                    assert(0 && "Invalid scheduler choice");
                    break;
            }
            break;
        case MD_CYCLIC_VICTIM:
            switch ( push_policy ) {
                case MD_LOCAL_PUSH:
                    setSchedulersCyclicVictimLocalPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_DATA_LOCALITY_PUSH:
                    setSchedulersCyclicVictimDataLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_EVENT_LOCALITY_PUSH:
                    setSchedulersCyclicVictimEventLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                default:
                    assert(0 && "Invalid scheduler choice");
                    break;
            }
            break;
        case MD_HIER_CYCLIC_VICTIM:
            switch ( push_policy ) {
                case MD_LOCAL_PUSH:
                    setSchedulersHierCyclicVictimLocalPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_DATA_LOCALITY_PUSH:
                    setSchedulersHierCyclicVictimDataLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_EVENT_LOCALITY_PUSH:
                    setSchedulersHierCyclicVictimEventLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                default:
                    assert(0 && "Invalid scheduler choice");
                    break;
            }
            break;
        case MD_HIER_RANDOM_VICTIM:
            switch ( push_policy ) {
                case MD_LOCAL_PUSH:
                    setSchedulersHierRandomVictimLocalPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_DATA_LOCALITY_PUSH:
                    setSchedulersHierRandomVictimDataLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                case MD_EVENT_LOCALITY_PUSH:
                    setSchedulersHierRandomVictimEventLocalityPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                default:
                    assert(0 && "Invalid scheduler choice");
                    break;
            }
            break;
        case MD_SOCKET_ONLY_VICTIM:
            switch ( push_policy ) {
                case MD_USER_SOCKET_PUSH:
                    setSchedulersSocketOnlyVictimUserSocketPush (defaultPolicy, nb_policy_domains, nb_schedulers, nb_workers);
                    break;
                default:
                    assert(0 && "Invalid scheduler choice");
                    break;
            }
            break;
    };

    defaultPolicy->executors  = newModel( ocr_executor_default_kind, nb_executors, NULL, NULL );
    defaultPolicy->memories   = newModel( OCR_LOWMEMORY_DEFAULT, 1, NULL, NULL );

    setDefaultModelMappings(defaultPolicy);
    return defaultPolicy;
}

/**
 * FSIM XE policy domain has:
 * one XE scheduler for all XEs
 * one worker for each XEs
 * one executor for each XEs
 * two workpile for each XEs, one for real work, one for CE messages
 * one memory for all XEs
 * one allocator for all XEs
 *
 * configures workers to have IDs of (#CE..#CE+#XE]
 *
 */

ocr_model_policy_t * createXeModelPolicies ( size_t nb_CEs, size_t nb_XEs_per_CE ) {
    size_t nb_per_xe_schedulers = 1;
    size_t nb_per_xe_workers = 1;
    size_t nb_per_xe_executors = 1;
    size_t nb_per_xe_workpiles = 2;
    size_t nb_per_xe_memories = 1;
    size_t nb_per_xe_allocators = 1;

    size_t nb_XEs = nb_CEs * nb_XEs_per_CE;

    // there are #XE instances of a model
    ocr_model_policy_t * xePolicyModel = (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));
    xePolicyModel->model.kind = OCR_POLICY_XE;
    xePolicyModel->model.nb_instances = nb_XEs;
    xePolicyModel->model.per_type_configuration = NULL;
    xePolicyModel->model.per_instance_configuration = NULL;

    xePolicyModel->nb_scheduler_types = 1;
    xePolicyModel->nb_worker_types = 1;
    xePolicyModel->nb_executor_types = 1;
    xePolicyModel->nb_workpile_types = 1;
    xePolicyModel->numMemTypes = 1;
    xePolicyModel->numAllocTypes = 1;

    // XE scheduler
    size_t index_ce, index_xe;
    size_t index_config = 0;
    size_t worker_id_offset = 1;
    void** scheduler_configurations = malloc(sizeof(scheduler_configuration*)*nb_XEs*nb_per_xe_schedulers);
    for ( index_config = 0; index_config < nb_XEs; ++index_config ) {
        scheduler_configurations[index_config] = (scheduler_configuration*) malloc(sizeof(scheduler_configuration));
    }

    index_config = 0;
    for ( index_ce = 0; index_ce < nb_CEs; ++index_ce ) {
        for ( index_xe = 0; index_xe < nb_XEs_per_CE; ++index_xe, ++index_config ) {
            scheduler_configuration* curr_config = (scheduler_configuration*)scheduler_configurations[index_config];
            curr_config->worker_id_begin = index_xe + worker_id_offset;
            curr_config->worker_id_end = index_xe + worker_id_offset;
        }
        worker_id_offset += (1 + nb_XEs_per_CE); // because nothing says nasty code like your own strength reduction
    }

    index_config = 0;
    size_t n_all_workers = nb_XEs*nb_per_xe_workers;
    void** worker_configurations = malloc(sizeof(worker_configuration*)*n_all_workers );
    for ( index_config = 0; index_config < n_all_workers; ++index_config ) {
        worker_configurations[index_config] = (worker_configuration*) malloc(sizeof(worker_configuration));
    }

    index_config = 0;
    worker_id_offset = 1;
    for ( index_ce = 0; index_ce < nb_CEs; ++index_ce ) {
        for ( index_xe = 0; index_xe < nb_XEs_per_CE; ++index_xe, ++index_config ) {
            worker_configuration* curr_config = (worker_configuration*)worker_configurations[index_config];
            curr_config->worker_id = index_xe + worker_id_offset;
        }
        worker_id_offset += (1 + nb_XEs_per_CE); // because nothing says nasty code like your own strength reduction
    }

    xePolicyModel->schedulers = newModel ( ocr_scheduler_xe_kind, nb_per_xe_schedulers, NULL, scheduler_configurations );
    xePolicyModel->workers    = newModel ( ocr_worker_xe_kind, nb_per_xe_workers, NULL, worker_configurations );
    xePolicyModel->executors  = newModel ( ocr_executor_xe_kind, nb_per_xe_executors, NULL, NULL );
    xePolicyModel->workpiles  = newModel ( ocr_workpile_xe_kind, nb_per_xe_workpiles, NULL, NULL );
    xePolicyModel->memories   = newModel ( ocrLowMemoryXEKind, nb_per_xe_memories, NULL, NULL );

    // XE allocator
    ocrAllocatorModel_t *xeAllocator = (ocrAllocatorModel_t*)malloc(sizeof(ocrAllocatorModel_t));
    xeAllocator->model.per_type_configuration = NULL;
    xeAllocator->model.per_instance_configuration = NULL;
    xeAllocator->model.kind = ocrAllocatorXEKind;
    xeAllocator->model.nb_instances = nb_per_xe_allocators;
    xeAllocator->sizeManaged = gHackTotalMemSize;
    xePolicyModel->allocators = xeAllocator;

    // Defines how ocr modules are bound together
    size_t nb_module_mappings = 5;
    ocr_module_mapping_t * xeMapping =
        (ocr_module_mapping_t *) malloc(sizeof(ocr_module_mapping_t) * nb_module_mappings);
    // Note: this doesn't bind modules magically. You need to have a mapping function defined
    //       and set in the targeted implementation (see ocr_scheduler_hc implementation for reference).
    //       These just make sure the mapping functions you have defined are called
    xeMapping[0] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_WORKPILE, OCR_SCHEDULER);
    xeMapping[1] = build_ocr_module_mapping(ONE_TO_ONE_MAPPING, OCR_WORKER, OCR_EXECUTOR);
    xeMapping[2] = build_ocr_module_mapping(ONE_TO_MANY_MAPPING, OCR_SCHEDULER, OCR_WORKER);
    xeMapping[3] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_MEMORY, OCR_ALLOCATOR);
    xeMapping[4] = build_ocr_module_mapping(ONE_TO_ONE_MAPPING, OCR_SCHEDULER, OCR_POLICY);
    xePolicyModel->nb_mappings = nb_module_mappings;
    xePolicyModel->mappings = xeMapping;

    return xePolicyModel;
}
/**
 * FSIM CE policy domains has:
 * one CE scheduler for all CEs
 * one worker for each CEs
 * one executor for each CEs
 * two workpile for each CEs, one for real work, one for XE messages
 * one memory for all CEs
 * one allocator for all CEs
 */

void CEModelPoliciesHelper ( ocr_model_policy_t * cePolicyModel ) {
    size_t nb_ce_executors = 1;
    size_t nb_ce_memories = 1;
    size_t nb_ce_allocators = 1;

    cePolicyModel->nb_scheduler_types = 1;
    cePolicyModel->nb_worker_types = 1;
    cePolicyModel->nb_executor_types = 1;
    cePolicyModel->nb_workpile_types = 2;
    cePolicyModel->numMemTypes = 1;
    cePolicyModel->numAllocTypes = 1;

    cePolicyModel->executors = newModel ( ocr_executor_ce_kind, nb_ce_executors, NULL, NULL );
    cePolicyModel->memories  = newModel ( ocrLowMemoryCEKind, nb_ce_memories, NULL, NULL );

    // CE workpile
    ocr_model_t * ceWorkpiles = (ocr_model_t *) malloc(sizeof(ocr_model_t)*2);
    ceWorkpiles[0] = (ocr_model_t){.kind =    ocr_workpile_ce_work_kind, .nb_instances = 1, .per_type_configuration = NULL, .per_instance_configuration = NULL };
    ceWorkpiles[1] = (ocr_model_t){.kind = ocr_workpile_ce_message_kind, .nb_instances = 1, .per_type_configuration = NULL, .per_instance_configuration = NULL };
    cePolicyModel->workpiles = ceWorkpiles;

    // CE allocator
    ocrAllocatorModel_t *ceAllocator = (ocrAllocatorModel_t*)malloc(sizeof(ocrAllocatorModel_t));
    ceAllocator->model.per_type_configuration = NULL;
    ceAllocator->model.per_instance_configuration = NULL;
    ceAllocator->model.kind = ocrAllocatorXEKind;
    ceAllocator->model.nb_instances = nb_ce_allocators;
    ceAllocator->sizeManaged = gHackTotalMemSize;
    cePolicyModel->allocators = ceAllocator;

    // Defines how ocr modules are bound together
    size_t nb_module_mappings = 5;
    ocr_module_mapping_t * ceMapping =
        (ocr_module_mapping_t *) malloc(sizeof(ocr_module_mapping_t) * nb_module_mappings);
    // Note: this doesn't bind modules magically. You need to have a mapping function defined
    //       and set in the targeted implementation (see ocr_scheduler_hc implementation for reference).
    //       These just make sure the mapping functions you have defined are called
    ceMapping[0] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_WORKPILE, OCR_SCHEDULER);
    ceMapping[1] = build_ocr_module_mapping(ONE_TO_ONE_MAPPING, OCR_WORKER, OCR_EXECUTOR);
    ceMapping[2] = build_ocr_module_mapping(ONE_TO_MANY_MAPPING, OCR_SCHEDULER, OCR_WORKER);
    ceMapping[3] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_MEMORY, OCR_ALLOCATOR);
    ceMapping[4] = build_ocr_module_mapping(ONE_TO_ONE_MAPPING, OCR_SCHEDULER, OCR_POLICY);
    cePolicyModel->nb_mappings = nb_module_mappings;
    cePolicyModel->mappings = ceMapping;


}

ocr_model_policy_t * createCeModelPolicies ( size_t nb_CEs, size_t nb_XEs_per_CE ) {
    ocr_model_policy_t * cePolicyModel = (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));

    size_t nb_ce_schedulers = 1;
    size_t nb_ce_workers = 1;

    cePolicyModel->model.kind = ocr_policy_ce_kind;
    cePolicyModel->model.per_type_configuration = NULL;
    cePolicyModel->model.per_instance_configuration = NULL;
    cePolicyModel->model.nb_instances = nb_CEs;

    size_t index_config = 0, n_all_schedulers = nb_ce_schedulers*nb_CEs;
    void** scheduler_configurations = malloc(sizeof(scheduler_configuration*)*n_all_schedulers);
    for ( index_config = 0; index_config < n_all_schedulers; ++index_config ) {
        scheduler_configurations[index_config] = (scheduler_configuration*) malloc(sizeof(scheduler_configuration));
        scheduler_configuration* curr_config = (scheduler_configuration*)scheduler_configurations[index_config];
        curr_config->worker_id_begin = (1+nb_XEs_per_CE) * (1+index_config);
        curr_config->worker_id_end = (1+nb_XEs_per_CE) * (1+index_config) + nb_ce_workers - 1 ;
    }
    
    index_config = 0;
    size_t n_all_workers = nb_ce_workers*nb_CEs;
    void** worker_configurations = malloc(sizeof(worker_configuration*)*n_all_workers );
    for ( index_config = 0; index_config < n_all_workers; ++index_config ) {
        worker_configurations[index_config] = (worker_configuration*) malloc(sizeof(worker_configuration));
        worker_configuration* curr_config = (worker_configuration*)worker_configurations[index_config];
        curr_config->worker_id = (1+nb_XEs_per_CE) * (1+index_config);
    }

    cePolicyModel->schedulers = newModel ( ocr_scheduler_ce_kind, nb_ce_schedulers, NULL, scheduler_configurations);
    cePolicyModel->workers = newModel ( ocr_worker_ce_kind, nb_ce_workers, NULL, worker_configurations);

    CEModelPoliciesHelper(cePolicyModel);
    return cePolicyModel;
}

ocr_model_policy_t * createCeMasteredModelPolicy ( size_t nb_XEs_per_CE ) {
    ocr_model_policy_t * cePolicyModel = (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));

    size_t nb_ce_schedulers = 1;
    size_t nb_ce_workers = 1;

    cePolicyModel->model.kind = ocr_policy_ce_mastered_kind;
    cePolicyModel->model.per_type_configuration = NULL;
    cePolicyModel->model.per_instance_configuration = NULL;
    cePolicyModel->model.nb_instances = 1;

    // Mastered-CE scheduler
    size_t index_config = 0, n_all_schedulers = nb_ce_schedulers;
    void** scheduler_configurations = malloc(sizeof(scheduler_configuration*)*n_all_schedulers);
    for ( index_config = 0; index_config < n_all_schedulers; ++index_config ) {
        scheduler_configurations[index_config] = (scheduler_configuration*) malloc(sizeof(scheduler_configuration));
        scheduler_configuration* curr_config = (scheduler_configuration*)scheduler_configurations[index_config];
        curr_config->worker_id_begin = 0;
        curr_config->worker_id_end = curr_config->worker_id_begin + nb_ce_workers - 1;
    }

    index_config = 0;
    size_t n_all_workers = nb_ce_workers;
    void** worker_configurations = malloc(sizeof(worker_configuration*)*n_all_workers );
    for ( index_config = 0; index_config < n_all_workers; ++index_config ) {
        worker_configurations[index_config] = (worker_configuration*) malloc(sizeof(worker_configuration));
        worker_configuration* curr_config = (worker_configuration*)worker_configurations[index_config];
        curr_config->worker_id = index_config;
    }

    cePolicyModel->schedulers = newModel ( ocr_scheduler_ce_kind, nb_ce_schedulers, NULL, scheduler_configurations);
    cePolicyModel->workers = newModel ( ocr_worker_ce_kind, nb_ce_workers, NULL, worker_configurations);

    CEModelPoliciesHelper(cePolicyModel);
    return cePolicyModel;
}

ocr_model_policy_t * createEmptyModelPolicyHelper ( size_t nPlaces ) {
    ocr_model_policy_t * policyModel = (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));
    policyModel->model.kind = OCR_PLACE_POLICY;
    policyModel->model.nb_instances = nPlaces;
    policyModel->model.per_type_configuration = NULL;
    policyModel->model.per_instance_configuration = NULL;
    policyModel->nb_scheduler_types = 0;
    policyModel->nb_worker_types    = 0;
    policyModel->nb_executor_types  = 0;
    policyModel->nb_workpile_types  = 0;
    policyModel->numMemTypes = 0;
    policyModel->numAllocTypes = 0;
    policyModel->nb_mappings = 0;

    policyModel->schedulers = NULL;
    policyModel->workers = NULL;
    policyModel->executors = NULL;
    policyModel->workpiles = NULL;
    policyModel->memories = NULL;
    policyModel->allocators = NULL;
    policyModel->mappings = NULL;

    return policyModel;
}

ocr_model_policy_t * createThorRootModelPolicy ( ) {
    return createEmptyModelPolicyHelper(1);
}

ocr_model_policy_t * createThorL3ModelPolicies ( size_t n_L3s ) {
    return createEmptyModelPolicyHelper(n_L3s);
}

ocr_model_policy_t * createThorL2ModelPolicies ( size_t n_L2s ) {
    return createEmptyModelPolicyHelper(n_L2s);
}

ocr_model_policy_t * createThorL1ModelPolicies ( size_t n_L1s ) {
    return createEmptyModelPolicyHelper(n_L1s);
}
void createThorWorkerModelPoliciesHelper ( ocr_model_policy_t * leafPolicyModel, size_t nb_policy_domains, size_t nb_workers, size_t worker_offset ) {
    int nb_schedulers = 1;
    int nb_executors = 1;
    int nb_workpiles = 1;

    leafPolicyModel->model.nb_instances = nb_policy_domains;
    leafPolicyModel->model.per_type_configuration = NULL;
    leafPolicyModel->model.per_instance_configuration = NULL;

    leafPolicyModel->nb_scheduler_types = 1;
    leafPolicyModel->nb_worker_types = 1;
    leafPolicyModel->nb_executor_types = 1;
    leafPolicyModel->nb_workpile_types = 1;
    leafPolicyModel->numMemTypes = 1;
    leafPolicyModel->numAllocTypes = 1;

    // Default allocator
    ocrAllocatorModel_t *defaultAllocator = (ocrAllocatorModel_t*)malloc(sizeof(ocrAllocatorModel_t));
    defaultAllocator->model.per_type_configuration = NULL;
    defaultAllocator->model.per_instance_configuration = NULL;
    defaultAllocator->model.kind = OCR_ALLOCATOR_DEFAULT;
    defaultAllocator->model.nb_instances = 1;
    defaultAllocator->sizeManaged = gHackTotalMemSize;

    leafPolicyModel->allocators = defaultAllocator;

    size_t index_config = 0, n_all_schedulers = nb_schedulers*nb_policy_domains;

    void** scheduler_configurations = malloc(sizeof(scheduler_configuration*)*n_all_schedulers);
    for ( index_config = 0; index_config < n_all_schedulers; ++index_config ) {
        scheduler_configurations[index_config] = (scheduler_configuration*) malloc(sizeof(scheduler_configuration));
        scheduler_configuration* curr_config = (scheduler_configuration*)scheduler_configurations[index_config];
        curr_config->worker_id_begin = worker_offset + ( index_config / nb_schedulers ) * nb_workers;
        curr_config->worker_id_end = worker_offset + ( index_config / nb_schedulers ) * nb_workers + nb_workers - 1;
    }

    leafPolicyModel->schedulers = newModel( OCR_PLACED_SCHEDULER, nb_schedulers, NULL, scheduler_configurations );
    leafPolicyModel->executors  = newModel( ocr_executor_default_kind, nb_executors, NULL, NULL );
    leafPolicyModel->workpiles  = newModel( ocr_workpile_default_kind, nb_workpiles, NULL, NULL );
    leafPolicyModel->memories   = newModel( OCR_LOWMEMORY_DEFAULT, 1, NULL, NULL );

    // Defines how ocr modules are bound together
    size_t nb_module_mappings = 5;
    ocr_module_mapping_t * defaultMapping =
        (ocr_module_mapping_t *) malloc(sizeof(ocr_module_mapping_t) * nb_module_mappings);
    // Note: this doesn't bind modules magically. You need to have a mapping function defined
    //       and set in the targeted implementation (see ocr_scheduler_hc implementation for reference).
    //       These just make sure the mapping functions you have defined are called
    defaultMapping[0] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_WORKPILE, OCR_SCHEDULER);
    defaultMapping[1] = build_ocr_module_mapping(ONE_TO_ONE_MAPPING, OCR_WORKER, OCR_EXECUTOR);
    defaultMapping[2] = build_ocr_module_mapping(ONE_TO_MANY_MAPPING, OCR_SCHEDULER, OCR_WORKER);
    defaultMapping[3] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_MEMORY, OCR_ALLOCATOR);
    defaultMapping[4] = build_ocr_module_mapping(MANY_TO_ONE_MAPPING, OCR_SCHEDULER, OCR_POLICY);
    leafPolicyModel->nb_mappings = nb_module_mappings;
    leafPolicyModel->mappings = defaultMapping;

}

ocr_model_policy_t * createThorMasteredWorkerModelPolicies ( ) {
    ocr_model_policy_t * leafPolicyModel = (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));
    createThorWorkerModelPoliciesHelper(leafPolicyModel, 1, 1, 0);
        
    leafPolicyModel->model.kind = OCR_MASTERED_LEAF_PLACE_POLICY;

    size_t index_config = 0;
    void** worker_configurations = malloc(sizeof(worker_configuration*));
    worker_configurations[index_config] = (worker_configuration*) malloc(sizeof(worker_configuration));
    worker_configuration* curr_config = (worker_configuration*)worker_configurations[index_config];
    curr_config->worker_id = index_config;

    leafPolicyModel->workers = newModel(ocr_worker_default_kind, 1, NULL, worker_configurations);

    return leafPolicyModel;
}

ocr_model_policy_t * createThorWorkerModelPolicies ( size_t n_workers ) {
    ocr_model_policy_t * leafPolicyModel = (ocr_model_policy_t *) malloc(sizeof(ocr_model_policy_t));
    createThorWorkerModelPoliciesHelper(leafPolicyModel, n_workers, 1, 1);
        
    leafPolicyModel->model.kind = OCR_LEAF_PLACE_POLICY;

    size_t index_config = 0;
    void** worker_configurations = malloc(sizeof(worker_configuration*)*n_workers );
    for ( index_config = 0; index_config < n_workers; ++index_config ) {
        worker_configurations[index_config] = (worker_configuration*) malloc(sizeof(worker_configuration));
        worker_configuration* curr_config = (worker_configuration*)worker_configurations[index_config];
        curr_config->worker_id = 1 + index_config;
    }
    leafPolicyModel->workers = newModel(ocr_worker_default_kind, 1, NULL, worker_configurations);

    return leafPolicyModel;
}

void destructOcrModelPolicy(ocr_model_policy_t * model) {
    if (model->schedulers != NULL) {
        free(model->schedulers);
    }
    if (model->workers != NULL) {
        free(model->workers);
    }
    if (model->executors != NULL) {
        free(model->executors);
    }
    if (model->workpiles != NULL) {
        free(model->workpiles);
    }
    if (model->mappings != NULL) {
        free(model->mappings);
    }
    if(model->memories != NULL) {
        free(model->memories);
    }
    if(model->allocators != NULL) {
        free(model->allocators);
    }
    free(model);
}

static  void create_configure_all_schedulers ( ocr_scheduler_t ** all_schedulers, int n_policy_domains, int nb_component_types, ocr_model_t* components ) {
    size_t idx = 0, index_policy_domain = 0;
    for (; index_policy_domain < n_policy_domains; ++index_policy_domain ) {
        size_t type = 0, instance = 0;
        for (; type < nb_component_types; ++type) {
            // For each type, create the number of instances asked for.
            ocr_model_t curr_model = components[type];
            for ( instance = 0; instance < curr_model.nb_instances; ++instance, ++idx ) {
                // Call the factory method based on the model's type kind.
                all_schedulers[idx] = newScheduler(curr_model.kind);
                all_schedulers[idx]->create(all_schedulers[idx], curr_model.per_type_configuration,
                                            (curr_model.per_instance_configuration) ? curr_model.per_instance_configuration[idx]: NULL );
            }
        }
    }
}

static  void create_configure_all_workpiles ( ocr_workpile_t ** all_workpiles, int n_policy_domains, int nb_component_types, ocr_model_t* components ) {
    size_t idx = 0, index_policy_domain = 0;
    for (; index_policy_domain < n_policy_domains; ++index_policy_domain ) {
        size_t type = 0, instance = 0;
        for (; type < nb_component_types; ++type) {
            // For each type, create the number of instances asked for.
            ocr_model_t curr_model = components[type];
            for ( instance = 0; instance < curr_model.nb_instances; ++instance, ++idx ) {
                // Call the factory method based on the model's type kind.
                all_workpiles[idx] = newWorkpile(curr_model.kind);
                all_workpiles[idx]->create(all_workpiles[idx], curr_model.per_type_configuration);
            }
        }
    }
}

static  void create_configure_all_workers ( ocr_worker_t** all_workers, int n_policy_domains, int nb_component_types, ocr_model_t* components ) {
    size_t idx = 0, index_policy_domain = 0;
    for (; index_policy_domain < n_policy_domains; ++index_policy_domain ) {
        size_t type = 0, instance = 0;
        for (; type < nb_component_types; ++type) {
            // For each type, create the number of instances asked for.
            ocr_model_t curr_model = components[type];
            for ( instance = 0; instance < curr_model.nb_instances; ++instance, ++idx ) {
                // Call the factory method based on the model's type kind.
                all_workers[idx] = newWorker(curr_model.kind);
                all_workers[idx]->create(all_workers[idx], curr_model.per_type_configuration,
                                         (curr_model.per_instance_configuration) ? curr_model.per_instance_configuration[idx]: NULL );
            }
        }
    }
}

static  void create_configure_all_executors ( ocr_executor_t ** all_executors, int n_policy_domains, int nb_component_types, ocr_model_t* components ) {
    size_t idx = 0, index_policy_domain = 0;
    for (; index_policy_domain < n_policy_domains; ++index_policy_domain ) {
        size_t type = 0, instance = 0;
        for (; type < nb_component_types; ++type) {
            // For each type, create the number of instances asked for.
            ocr_model_t curr_component_model = components[type];
            for ( instance = 0; instance < curr_component_model.nb_instances; ++instance, ++idx ) {
                // Call the factory method based on the model's type kind.
                all_executors[idx] = newExecutor(curr_component_model.kind);
                all_executors[idx]->create(all_executors[idx], curr_component_model.per_type_configuration);
            }
        }
    }
}

static  void create_configure_all_allocators ( ocrAllocator_t ** all_allocators, int n_policy_domains, int nb_component_types, ocrAllocatorModel_t* components ) {
    size_t idx = 0, index_policy_domain = 0;
    for (; index_policy_domain < n_policy_domains; ++index_policy_domain ) {
        size_t type = 0, instance = 0;
        for (; type < nb_component_types; ++type) {
            // For each type, create the number of instances asked for.
            ocrAllocatorModel_t curr_component_model = components[type];
            for ( instance = 0; instance < curr_component_model.model.nb_instances; ++instance, ++idx ) {
                // Call the factory method based on the model's type kind.
                all_allocators[idx] = newAllocator(curr_component_model.model.kind);
                all_allocators[idx]->create(all_allocators[idx], curr_component_model.sizeManaged, curr_component_model.model.per_type_configuration);
            }
        }
    }
}

static  void create_configure_all_memories ( ocrLowMemory_t ** all_memories, int n_policy_domains, int nb_component_types, ocr_model_t* components ) {
    size_t idx = 0, index_policy_domain = 0;
    for (; index_policy_domain < n_policy_domains; ++index_policy_domain ) {
        size_t type = 0, instance = 0;
        for (; type < nb_component_types; ++type) {
            // For each type, create the number of instances asked for.
            ocr_model_t curr_component_model = components[type];
            for ( instance = 0; instance < curr_component_model.nb_instances; ++instance, ++idx ) {
                // Call the factory method based on the model's type kind.
                all_memories[idx] = newLowMemory(curr_component_model.kind);
                all_memories[idx]->create(all_memories[idx],curr_component_model.per_type_configuration);
            }
        }
    }
}

/**!
 * Given a policy domain model, go over its modules and instantiate them.
 */
ocr_policy_domain_t ** instantiateModel(ocr_model_policy_t * model) {

    // Compute total number of workers, executors and workpiles, allocators and memories
    int per_policy_domain_total_nb_schedulers = 0;
    int per_policy_domain_total_nb_workers = 0;
    int per_policy_domain_total_nb_executors = 0;
    int per_policy_domain_total_nb_workpiles = 0;
    u64 totalNumMemories = 0;
    u64 totalNumAllocators = 0;
    size_t j = 0;
    for ( j = 0; j < model->nb_scheduler_types; ++j ) {
        per_policy_domain_total_nb_schedulers += model->schedulers[j].nb_instances;
    }
    for ( j = 0; j < model->nb_worker_types; ++j ) {
        per_policy_domain_total_nb_workers += model->workers[j].nb_instances;
    }
    for ( j = 0; j < model->nb_executor_types; ++j ) {
        per_policy_domain_total_nb_executors += model->executors[j].nb_instances;
    }
    for ( j = 0; j < model->nb_workpile_types; ++j ) {
        per_policy_domain_total_nb_workpiles += model->workpiles[j].nb_instances;
    }
    for ( j = 0; j < model->numAllocTypes; ++j ) {
        totalNumAllocators += model->allocators[j].model.nb_instances;
    }
    for ( j = 0; j < model->numMemTypes; ++j ) {
        totalNumMemories += model->memories[j].nb_instances;
    }

    int n_policy_domains = model->model.nb_instances;
    ocr_policy_domain_t ** policyDomains = (ocr_policy_domain_t **) malloc( sizeof(ocr_policy_domain_t*) * n_policy_domains );

    // Allocate memory for ocr components
    // Components instances are grouped into one big chunk of memory
    ocr_scheduler_t** all_schedulers = (ocr_scheduler_t**) malloc(sizeof(ocr_scheduler_t*) * per_policy_domain_total_nb_schedulers * n_policy_domains );
    ocr_worker_t   ** all_workers    = (ocr_worker_t   **) malloc(sizeof(ocr_worker_t   *) * per_policy_domain_total_nb_workers    * n_policy_domains );
    ocr_executor_t ** all_executors  = (ocr_executor_t **) malloc(sizeof(ocr_executor_t *) * per_policy_domain_total_nb_executors  * n_policy_domains );
    ocr_workpile_t ** all_workpiles  = (ocr_workpile_t **) malloc(sizeof(ocr_workpile_t *) * per_policy_domain_total_nb_workpiles  * n_policy_domains );
    ocrAllocator_t ** all_allocators = (ocrAllocator_t **) malloc(sizeof(ocrAllocator_t *) * totalNumAllocators * n_policy_domains );
    ocrLowMemory_t ** all_memories   = (ocrLowMemory_t **) malloc(sizeof(ocrLowMemory_t *) * totalNumMemories * n_policy_domains );

    //
    // Build instances of each ocr modules
    //
    //TODO would be nice to make creation more generic

    create_configure_all_schedulers ( all_schedulers, n_policy_domains, model->nb_scheduler_types, model->schedulers);
    create_configure_all_workers    ( all_workers   , n_policy_domains, model->nb_worker_types, model->workers );
    create_configure_all_executors  ( all_executors , n_policy_domains, model->nb_executor_types, model->executors);
    create_configure_all_workpiles  ( all_workpiles , n_policy_domains, model->nb_workpile_types, model->workpiles);
    create_configure_all_allocators ( all_allocators, n_policy_domains, model->numAllocTypes, model->allocators);
    create_configure_all_memories   ( all_memories  , n_policy_domains, model->numMemTypes, model->memories);


    int idx;
    for ( idx = 0; idx < n_policy_domains; ++idx ) {

        ocr_scheduler_t** schedulers = (ocr_scheduler_t**) &(all_schedulers[ idx * per_policy_domain_total_nb_schedulers ]);
        ocr_worker_t   ** workers    = (ocr_worker_t   **) &(all_workers   [ idx * per_policy_domain_total_nb_workers    ]);
        ocr_executor_t ** executors  = (ocr_executor_t **) &(all_executors [ idx * per_policy_domain_total_nb_executors  ]);
        ocr_workpile_t ** workpiles  = (ocr_workpile_t **) &(all_workpiles [ idx * per_policy_domain_total_nb_workpiles  ]);
        ocrAllocator_t ** allocators = (ocrAllocator_t **) &(all_allocators[ idx * totalNumAllocators                    ]);
        ocrLowMemory_t ** memories   = (ocrLowMemory_t **) &(all_memories  [ idx * totalNumMemories                      ]);

        // Create an instance of the policy domain
        policyDomains[idx] = newPolicy(model->model.kind,
                                       per_policy_domain_total_nb_workpiles, per_policy_domain_total_nb_workers,
                                       per_policy_domain_total_nb_executors, per_policy_domain_total_nb_schedulers);

        policyDomains[idx]->create(policyDomains[idx], NULL, schedulers,
                                   workers, executors, workpiles, allocators, memories);

        // This is only needed because we want to be able to
        // write generic code to find instances' backing arrays
        // given a module kind.
        size_t nb_ocr_modules = 7;
        ocr_module_instance modules_kinds[nb_ocr_modules];
        modules_kinds[0] = (ocr_module_instance){.kind = OCR_WORKER,	.nb_instances = per_policy_domain_total_nb_workers,	.instances = (void **) workers};
        modules_kinds[1] = (ocr_module_instance){.kind = OCR_EXECUTOR,	.nb_instances = per_policy_domain_total_nb_executors,	.instances = (void **) executors};
        modules_kinds[2] = (ocr_module_instance){.kind = OCR_WORKPILE,	.nb_instances = per_policy_domain_total_nb_workpiles,	.instances = (void **) workpiles};
        modules_kinds[3] = (ocr_module_instance){.kind = OCR_SCHEDULER,	.nb_instances = per_policy_domain_total_nb_schedulers,	.instances = (void **) schedulers};
        modules_kinds[4] = (ocr_module_instance){.kind = OCR_ALLOCATOR,	.nb_instances = totalNumAllocators,	.instances = (void **) allocators};
        modules_kinds[5] = (ocr_module_instance){.kind = OCR_MEMORY,	.nb_instances = totalNumMemories,	.instances = (void **) memories};
        modules_kinds[6] = (ocr_module_instance){.kind = OCR_POLICY,	.nb_instances = 1,			.instances = (void **) &(policyDomains[idx])};

        //
        // Bind instances of each ocr components through mapping functions
        //   - Binding is the last thing we do as we may need information set
        //     during the 'create' phase
        //
        size_t instance, nb_instances_from, nb_instances_to;
        void ** from_instances;
        void ** to_instances;
        for ( instance = 0; instance < model->nb_mappings; ++instance ) {
            ocr_module_mapping_t * mapping = (model->mappings + instance);
            // Resolve modules instances we want to map
            resolve_module_instances(mapping->from, nb_ocr_modules, modules_kinds, &nb_instances_from, &from_instances);
            resolve_module_instances(mapping->to, nb_ocr_modules, modules_kinds, &nb_instances_to, &to_instances);
            // Resolve mapping function to use and call it
            ocr_map_module_fct map_fct = get_module_mapping_function(mapping->kind);

            map_fct(mapping->from, mapping->to,
                    nb_instances_from, (ocr_module_t **) from_instances,
                    nb_instances_to, (ocr_module_t **) to_instances);
        }

    }
    return policyDomains;
}
