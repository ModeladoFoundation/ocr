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
#include <stdio.h>
#include <string.h>

#include "ocr-runtime.h"
#include "ocr-config.h"
#include "ocr-guid.h"

size_t n_root_policy_nodes;
ocr_policy_domain_t ** root_policies;

// the runtime fork/join-er
ocr_worker_t* master_worker;
u64 gHackTotalMemSize;

//TODO we should have an argument option parsing library
/**!
 * Utility function to remove ocr arguments before handing argv
 * over to the user program
 */
static void shift_arguments(int * argc, char ** argv, int start_offset, int shift_offset) {
    int i = start_offset;
    int j = shift_offset;
    while ( j < *argc) {
        argv[i++] = argv[j++];
    }

    *argc = (*argc - (shift_offset-start_offset));
}

/**!
 * Check if we have a machine description passed as argument
 */
static char * parseOcrOptions_MachineDescription(int * argc, char ** argv) {
    int i = 0;
    char * md_file = NULL;
    while(i < *argc) {
        if (strcmp(argv[i], "-md") == 0) {
            md_file = argv[i+1];
            shift_arguments(argc, argv, i, i+2);
        }
        i++;
    }
    return md_file;
}

static char * parseOcrOptions_Binding (int * argc, char ** argv) {
    int i = 0;
    char * bind_file = NULL;
    while(i < *argc) {
        if (strcmp(argv[i], "-bind") == 0) {
            bind_file = argv[i+1];
            shift_arguments(argc, argv, i, i+2);
        }
        i++;
    }
    return bind_file;
}

static int * read_bind_file ( char * bind_file ) {
    FILE *fp;
    int temp_map[1024];
    int cpu, n = 0;

    fp=fopen(bind_file, "r");
    while(fscanf(fp, "%d", &cpu) != EOF) {
        temp_map[n] = cpu;
        n++;
        assert((n < 1024) && "cpu set limit reached. Ignoring thread binding.");
    }
    int *bind_map = (int *)malloc(sizeof(int) * n);
    memcpy(bind_map, temp_map, sizeof(int) * n);
    return bind_map;
}

/**!
 * Initialize the OCR runtime.
 * @param argc number of command line options
 * @param argv Pointer to options
 * @param fnc Number of function pointers (UNUSED)
 * @param funcs array of function pointers to be used as edts (UNUSED)
 *
 * Note: removes OCR options from argc / argv
 */

/*
fprintf(stderr, "chose default (deque, local push, local pop, random steal)\n");
policy_model = defaultOcrModelPolicy(nb_policy_domain,
        nb_schedulers_per_policy_domain, nb_workers_per_policy_domain,
        nb_executors_per_policy_domain, nb_workpiles_per_policy_domain, bind_map);
fprintf(stderr, "chose (dequeish_heap, local push, local pop, random steal\n");
policy_model = defaultOcrModelPolicyDequishHeap(nb_policy_domain,
        nb_schedulers_per_policy_domain, nb_workers_per_policy_domain,
        nb_executors_per_policy_domain, nb_workpiles_per_policy_domain, bind_map);
fprintf(stderr, "chose bar_priority_heap\n");
policy_model = defaultOcrModelPolicyPriorityHeap;
*/

void ocrInit(int * argc, char ** argv, u32 fnc, ocrEdt_t funcs[]) {

    // Intialize the GUID provider
    globalGuidProvider = newGuidProvider(OCR_GUIDPROVIDER_DEFAULT);

    u32 nbHardThreads = ocr_config_default_nb_hardware_threads;
    gHackTotalMemSize = 512*1024*1024; /* 64 MB default */
    char * md_file = parseOcrOptions_MachineDescription(argc, argv);

    char * bind_file = parseOcrOptions_Binding (argc, argv);
    int* bind_map = NULL;
    
    if ( NULL != bind_file ) {
        bind_map = read_bind_file(bind_file);
    }

    /* sagnak begin */
    if ( md_file != NULL && !strncmp(md_file, "bar_", 4)) {
        size_t nb_policy_domain = 1;
        size_t nb_workers_per_policy_domain = nbHardThreads;
        size_t nb_workpiles_per_policy_domain = nbHardThreads;
        size_t nb_executors_per_policy_domain = nbHardThreads;
        size_t nb_schedulers_per_policy_domain = 1;

        ocr_model_policy_t * policy_model = NULL;
        
        enum md_file_workpile_policy         workpile_policy         = MD_DEQUE;
        enum md_file_steal_victim_policy     steal_victim_policy     = MD_RANDOM_VICTIM;
        enum md_file_victim_extract_policy   victim_extract_policy   = MD_STEAL_LAST;
        enum md_file_push_policy             push_policy             = MD_LOCAL_PUSH;

        char* md_file_tokens = strtok(md_file,"_");
        md_file_tokens = strtok(NULL,"_"); /*parse the bar_ part*/
        while ( NULL != md_file_tokens ) {
            if(!strcmp(md_file_tokens, "dqheap")) {
                workpile_policy = MD_DEQUEISH_HEAP;
            } else if (!strcmp(md_file_tokens, "pqheap")) {
                workpile_policy = MD_PRIORITY_QUEUE;
            } else if (!strcmp(md_file_tokens, "ndeque")) {
                workpile_policy = MD_DEQUE;
            } else if (!strcmp(md_file_tokens, "cycsteal")) {
                steal_victim_policy = MD_CYCLIC_VICTIM;
            } else if (!strcmp(md_file_tokens, "rndsteal")) {
                steal_victim_policy = MD_RANDOM_VICTIM;
            } else if (!strcmp(md_file_tokens, "hiercyclicsteal")) {
                steal_victim_policy = MD_HIER_CYCLIC_VICTIM;
            } else if (!strcmp(md_file_tokens, "hierrandomsteal")) {
                steal_victim_policy = MD_HIER_RANDOM_VICTIM;
            } else if (!strcmp(md_file_tokens, "socketonlysteal")) {
                steal_victim_policy = MD_SOCKET_ONLY_VICTIM;
            } else if (!strcmp(md_file_tokens, "laststeal")) {
                victim_extract_policy = MD_STEAL_LAST;
            } else if (!strcmp(md_file_tokens, "altruisticsteal")) {
                victim_extract_policy = MD_STEAL_ALTRUISTIC;
            } else if (!strcmp(md_file_tokens, "selfishsteal")) {
                victim_extract_policy = MD_STEAL_SELFISH ;
            } else if (!strcmp(md_file_tokens, "tailpush")) {
                push_policy = MD_LOCAL_PUSH;
            } else if (!strcmp(md_file_tokens, "localitypush")) {
                push_policy = MD_LOCALITY_PUSH ;
            } else if (!strcmp(md_file_tokens, "usersocketpush")) {
                push_policy = MD_USER_SOCKET_PUSH ;
            } else {
                assert (0 && "md file parse error");
            }
            md_file_tokens = strtok(NULL,"_"); 
        }

        policy_model = ocrModelPolicyCreator( workpile_policy, steal_victim_policy, victim_extract_policy, push_policy,
                        nb_policy_domain,
                        nb_schedulers_per_policy_domain, nb_workers_per_policy_domain,
                        nb_executors_per_policy_domain, nb_workpiles_per_policy_domain,
                        bind_map);

        //TODO LIMITATION for now support only one policy
        n_root_policy_nodes = nb_policy_domain;
        root_policies = instantiateModel(policy_model);

        root_policies[0]->n_successors = 0;
        root_policies[0]->successors = NULL;
        root_policies[0]->n_predecessors = 0;
        root_policies[0]->predecessors = NULL;

        master_worker = root_policies[0]->workers[0];
        root_policies[0]->start(root_policies[0]);
    } else if ( md_file != NULL && !strncmp(md_file,"fsim",5) ) {
        // sagnak TODO handle nb_CEs <= 1 case
        size_t nb_CEs = 2;
        size_t nb_XE_per_CEs = 3;
        size_t nb_XEs = nb_XE_per_CEs * nb_CEs;

        ocr_model_policy_t * xe_policy_models = createXeModelPolicies ( nb_CEs, nb_XE_per_CEs );
        ocr_model_policy_t * ce_policy_models = NULL;
        if ( nb_CEs > 1 )
            ce_policy_models = createCeModelPolicies ( nb_CEs-1, nb_XE_per_CEs );
        ocr_model_policy_t * ce_mastered_policy_model = createCeMasteredModelPolicy( nb_XE_per_CEs );

        ocr_policy_domain_t ** xe_policy_domains = instantiateModel(xe_policy_models);
        ocr_policy_domain_t ** ce_policy_domains = NULL;
        if ( nb_CEs > 1 )
            ce_policy_domains = instantiateModel(ce_policy_models);
        ocr_policy_domain_t ** ce_mastered_policy_domain = instantiateModel(ce_mastered_policy_model);

        n_root_policy_nodes = nb_CEs;
        root_policies = (ocr_policy_domain_t**) malloc(sizeof(ocr_policy_domain_t*)*nb_CEs);

        root_policies[0] = ce_mastered_policy_domain[0];

        size_t idx = 0;
        for ( idx = 0; idx < nb_CEs-1; ++idx ) {
            root_policies[idx+1] = ce_policy_domains[idx];
        }

        idx = 0;
        ce_mastered_policy_domain[0]->n_successors = nb_XE_per_CEs;
        ce_mastered_policy_domain[0]->successors = &(xe_policy_domains[idx*nb_XE_per_CEs]);
        ce_mastered_policy_domain[0]->n_predecessors = 0;
        ce_mastered_policy_domain[0]->predecessors = NULL;

        for ( idx = 1; idx < nb_CEs; ++idx ) {
            ocr_policy_domain_t *curr = ce_policy_domains[idx-1];

            curr->n_successors = nb_XE_per_CEs;
            curr->successors = &(xe_policy_domains[idx*nb_XE_per_CEs]);
            curr->n_predecessors = 0;
            curr->predecessors = NULL;
        }

        // sagnak: should this instead be recursive?
        for ( idx = 0; idx < nb_XE_per_CEs; ++idx ) {
            ocr_policy_domain_t *curr = xe_policy_domains[idx];

            curr->n_successors = 0;
            curr->successors = NULL;
            curr->n_predecessors = 1;
            curr->predecessors = ce_mastered_policy_domain;
        }

        for ( idx = nb_XE_per_CEs; idx < nb_XEs; ++idx ) {
            ocr_policy_domain_t *curr = xe_policy_domains[idx];

            curr->n_successors = 0;
            curr->successors = NULL;
            curr->n_predecessors = 1;
            curr->predecessors = &(ce_policy_domains[idx/nb_XE_per_CEs-1]);
        }

        ce_mastered_policy_domain[0]->start(ce_mastered_policy_domain[0]);

        for ( idx = 1; idx < nb_CEs; ++idx ) {
            ocr_policy_domain_t *curr = ce_policy_domains[idx-1];
            curr->start(curr);
        }
        for ( idx = 0; idx < nb_XEs; ++idx ) {
            ocr_policy_domain_t *curr = xe_policy_domains[idx];
            curr->start(curr);
        }

        master_worker = ce_mastered_policy_domain[0]->workers[0];

    } else if ( md_file != NULL && !strncmp(md_file,"thor",4) ) {

        size_t n_L3s = 2;
        size_t n_L2s_per_L3 = 8;
        size_t n_L1s_per_L2 = 1;
        size_t n_workers_per_L1 = 1;

        size_t n_L2s = n_L2s_per_L3 * n_L3s; // 16
        size_t n_L1s = n_L1s_per_L2 * n_L2s; // 16
        size_t n_workers = n_workers_per_L1 * n_L1s; //16

        ocr_model_policy_t * root_policy_model = createThorRootModelPolicy ( );
        ocr_model_policy_t * l3_policy_model = createThorL3ModelPolicies ( n_L3s );
        ocr_model_policy_t * l2_policy_model = createThorL2ModelPolicies ( n_L2s );
        ocr_model_policy_t * l1_policy_model = createThorL1ModelPolicies ( n_L1s );
        ocr_model_policy_t * mastered_worker_policy_model = createThorMasteredWorkerModelPolicies ( );
        ocr_model_policy_t * worker_policy_model = createThorWorkerModelPolicies ( n_workers - 1 );

        ocr_policy_domain_t ** thor_root_policy_domains = instantiateModel(root_policy_model);
        ocr_policy_domain_t ** thor_l3_policy_domains = instantiateModel(l3_policy_model);
        ocr_policy_domain_t ** thor_l2_policy_domains = instantiateModel(l2_policy_model);
        ocr_policy_domain_t ** thor_l1_policy_domains = instantiateModel(l1_policy_model);
        ocr_policy_domain_t ** thor_mastered_worker_policy_domains = instantiateModel(mastered_worker_policy_model);
        ocr_policy_domain_t ** thor_worker_policy_domains = instantiateModel(worker_policy_model);

        n_root_policy_nodes = 1;
        root_policies = (ocr_policy_domain_t**) malloc(sizeof(ocr_policy_domain_t*));
        root_policies[0] = thor_root_policy_domains[0];

        size_t breadthFirstLabel = 0;

        thor_root_policy_domains[0]->n_successors = n_L3s;
        thor_root_policy_domains[0]->successors = thor_l3_policy_domains;
        thor_root_policy_domains[0]->n_predecessors = 0;
        thor_root_policy_domains[0]->predecessors = NULL;
        thor_root_policy_domains[0]->id = breadthFirstLabel++; 

        size_t idx = 0;
        for ( idx = 0; idx < n_L3s; ++idx ) {
            ocr_policy_domain_t *curr = thor_l3_policy_domains[idx];
            curr->id = breadthFirstLabel++; 

            curr->n_successors = n_L2s_per_L3;
            curr->successors = &(thor_l2_policy_domains[idx*n_L2s_per_L3]);
            curr->n_predecessors = 1;
            curr->predecessors = thor_root_policy_domains;
        }

        for ( idx = 0; idx < n_L2s; ++idx ) {
            ocr_policy_domain_t *curr = thor_l2_policy_domains[idx];
            curr->id = breadthFirstLabel++; 

            curr->n_successors = n_L1s_per_L2;
            curr->successors = &(thor_l1_policy_domains[idx*n_L1s_per_L2]);
            curr->n_predecessors = 1;
            curr->predecessors = &(thor_l3_policy_domains[idx/n_L2s_per_L3]);
        }

        // idx = 0 condition for ( idx = 0; idx < n_L1s; ++idx )
        {
            ocr_policy_domain_t **nasty_successor_buffering = (ocr_policy_domain_t **)malloc(n_workers_per_L1*sizeof(ocr_policy_domain_t *));
            nasty_successor_buffering[0] = thor_mastered_worker_policy_domains[0];
            for ( idx = 1; idx < n_workers_per_L1; ++idx ) {
                nasty_successor_buffering[idx] = thor_worker_policy_domains[idx-1];
            }
            idx = 0;
            ocr_policy_domain_t *curr = thor_l1_policy_domains[idx];
            curr->id = breadthFirstLabel++; 

            curr->n_successors = n_workers_per_L1;
            curr->successors = nasty_successor_buffering;
            curr->n_predecessors = 1;
            curr->predecessors = &(thor_l2_policy_domains[idx/n_L1s_per_L2]);
        }

        for ( idx = 1; idx < n_L1s; ++idx ) {
            ocr_policy_domain_t *curr = thor_l1_policy_domains[idx];
            curr->id = breadthFirstLabel++; 

            curr->n_successors = n_workers_per_L1;
            curr->successors = &(thor_worker_policy_domains[idx*n_workers_per_L1 - 1]);
            curr->n_predecessors = 1;
            curr->predecessors = &(thor_l2_policy_domains[idx/n_L1s_per_L2]);
        }

        // idx = 0 condition for ( idx = 1; idx < n_workers; ++idx ) 
        {
            idx = 0;
            ocr_policy_domain_t *curr = thor_mastered_worker_policy_domains[idx];
            curr->id = breadthFirstLabel++; 
            curr->n_successors = 0;
            curr->successors = NULL;
            curr->n_predecessors = 1;
            curr->predecessors = &(thor_l1_policy_domains[idx/n_workers_per_L1]);
        }

        for ( idx = 1; idx < n_workers; ++idx ) {
            ocr_policy_domain_t *curr = thor_worker_policy_domains[idx-1];
            curr->id = breadthFirstLabel++; 

            curr->n_successors = 0;
            curr->successors = NULL;
            curr->n_predecessors = 1;
            curr->predecessors = &(thor_l1_policy_domains[idx/n_workers_per_L1]);
        }

        // does not do anything as these are mere 'empty' places
        thor_root_policy_domains[0]->start(thor_root_policy_domains[0]);
        for ( idx = 0; idx < n_L3s; ++idx ) {
            thor_l3_policy_domains[idx]->start(thor_l3_policy_domains[idx]);
        }
        for ( idx = 0; idx < n_L2s; ++idx ) {
            thor_l2_policy_domains[idx]->start(thor_l2_policy_domains[idx]);
        }
        for ( idx = 0; idx < n_L1s; ++idx ) {
            thor_l1_policy_domains[idx]->start(thor_l1_policy_domains[idx]);
        }


        thor_mastered_worker_policy_domains[0]->start(thor_mastered_worker_policy_domains[0]);
        for ( idx = 1; idx < n_workers; ++idx ) {
            thor_worker_policy_domains[idx-1]->start(thor_worker_policy_domains[idx-1]);
        }

        master_worker = thor_mastered_worker_policy_domains[0]->workers[0];
    } else {
        if (md_file != NULL) {
            //TODO need a file stat to check
            setMachineDescriptionFromPDL(md_file);
            MachineDescription * md = getMachineDescription();
            if (md == NULL) {
                // Something went wrong when reading the machine description file
                ocr_abort();
            } else {
                nbHardThreads = MachineDescription_getNumHardwareThreads(md);
                //	gHackTotalMemSize = MachineDescription_getDramSize(md);
            }
        }

        // This is the default policy
        // TODO this should be declared in the default policy model
        size_t nb_policy_domain = 1;
        size_t nb_workers_per_policy_domain = nbHardThreads;
        size_t nb_workpiles_per_policy_domain = nbHardThreads;
        size_t nb_executors_per_policy_domain = nbHardThreads;
        size_t nb_schedulers_per_policy_domain = 1;

        ocr_model_policy_t * policy_model = defaultOcrModelPolicy(nb_policy_domain,
                nb_schedulers_per_policy_domain, nb_workers_per_policy_domain,
                nb_executors_per_policy_domain, nb_workpiles_per_policy_domain, bind_map);

        //TODO LIMITATION for now support only one policy
        n_root_policy_nodes = nb_policy_domain;
        root_policies = instantiateModel(policy_model);

        root_policies[0]->n_successors = 0;
        root_policies[0]->successors = NULL;
        root_policies[0]->n_predecessors = 0;
        root_policies[0]->predecessors = NULL;

        master_worker = root_policies[0]->workers[0];
        root_policies[0]->start(root_policies[0]);
    }

    associate_executor_and_worker(master_worker);

    if ( NULL != bind_file ) {
        free(bind_map);
    }
}

static void recursive_policy_finish_helper ( ocr_policy_domain_t* curr ) {
    if ( curr ) {
        int index = 0; // successor index
        for ( ; index < curr->n_successors; ++index ) {
            recursive_policy_finish_helper(curr->successors[index]);
        }
        curr->finish(curr);
    }
}

static void recursive_policy_stop_helper ( ocr_policy_domain_t* curr ) {
    if ( curr ) {
        int index = 0; // successor index
        for ( ; index < curr->n_successors; ++index ) {
            recursive_policy_stop_helper(curr->successors[index]);
        }
        curr->stop(curr);
    }
}

void ocrFinish() {
    master_worker->stop(master_worker);
}

static void recursive_policy_destruct_helper ( ocr_policy_domain_t* curr ) {
    if ( curr ) {
        int index = 0; // successor index
        for ( ; index < curr->n_successors; ++index ) {
            recursive_policy_destruct_helper(curr->successors[index]);
        }
        curr->destruct(curr);
    }
}

static inline void unravel () {
    // current root policy index
    int index = 0;

    for ( index = 0; index < n_root_policy_nodes; ++index ) {
        recursive_policy_finish_helper(root_policies[index]);
    }

    for ( index = 0; index < n_root_policy_nodes; ++index ) {
        recursive_policy_stop_helper(root_policies[index]);
    }

    for ( index = 0; index < n_root_policy_nodes; ++index ) {
        recursive_policy_destruct_helper(root_policies[index]);
    }

    globalGuidProvider->destruct(globalGuidProvider);
    free(root_policies);
}

void ocrCleanup() {
    master_worker->routine(master_worker);

    unravel();
}
