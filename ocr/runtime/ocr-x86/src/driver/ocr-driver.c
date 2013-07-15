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


#include "debug.h"
#include "machine-description/ocr-machine.h"
#include "ocr-config.h"
#include "ocr-db.h"
#include "ocr-edt.h"
#include "ocr-lib.h"
#include "ocr-runtime.h"
#include "ocr-types.h"
#include "ocr-utils.h"
#include "ocr.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>


#ifdef OCR_ENABLE_STATISTICS
#include "ocr-stat-user.h"
#endif

#define DEBUG_TYPE INIPARSING

const char *type_str[] = {
    "GuidType",
    "MemPlatformType",
    "MemTargetType",
    "AllocatorType",
    "CompPlatformType",
    "CompTargetType",
    "WorkPileType",
    "WorkerType",
    "SchedulerType",
    "PolicyDomainType",
};

const char *inst_str[] = {
    "GuidInst",
    "MemPlatformInst",
    "MemTargetInst",
    "AllocatorInst",
    "CompPlatformInst",
    "CompTargetInst",
    "WorkPileInst",
    "WorkerInst",
    "SchedulerInst",
    "PolicyDomainInst",
};

/* The below array defines the list of dependences */

dep_t deps[] = {
    { memtarget_type, memplatform_type, "memplatform"},
    { allocator_type, memtarget_type, "memtarget"},
    { comptarget_type, compplatform_type, "compplatform"},
    { worker_type, comptarget_type, "comptarget"},
    { scheduler_type, workpile_type, "workpile"},
    { scheduler_type, worker_type, "worker"},
    { policydomain_type, guid_type, "guid"},
    { policydomain_type, memtarget_type, "memtarget"},
    { policydomain_type, allocator_type, "allocator"},
    { policydomain_type, comptarget_type, "comptarget"},
    { policydomain_type, workpile_type, "workpile"},
    { policydomain_type, worker_type, "worker"},
    { policydomain_type, scheduler_type, "scheduler"},
};

extern char* populate_type(ocrParamList_t **type_param, type_enum index, dictionary *dict, char *secname);
int populate_inst(ocrParamList_t **inst_param, ocrMappable_t **instance, int *type_counts, char ***factory_names, void ***all_factories, ocrMappable_t ***all_instances, type_enum index, dictionary *dict, char *secname);
extern int build_deps (dictionary *dict, int A, int B, char *refstr, ocrMappable_t ***all_instances, ocrParamList_t ***inst_params);
extern void *create_factory (type_enum index, char *factory_name, ocrParamList_t *paramlist);
extern int read_range(dictionary *dict, char *sec, char *field, int *low, int *high);
extern void free_instance(ocrMappable_t *instance, type_enum inst_type);

ocrGuid_t __attribute__ ((weak)) mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // This is just to make the linker happy and shouldn't be executed
    printf("error: no mainEdt defined.\n");
    ASSERT(false);
    return NULL_GUID;
}

void **all_factories[sizeof(type_str)/sizeof(const char *)];
ocrMappable_t **all_instances[sizeof(inst_str)/sizeof(const char *)];
int total_types = sizeof(type_str)/sizeof(const char *);
int type_counts[sizeof(type_str)/sizeof(const char *)];
int inst_counts[sizeof(inst_str)/sizeof(const char *)];
ocrParamList_t **type_params[sizeof(type_str)/sizeof(const char *)];
char **factory_names[sizeof(type_str)/sizeof(const char *)];        // ~9 different kinds (memtarget, comptarget, etc.); each with diff names (tlsf, malloc, etc.); each pointing to a char*
ocrParamList_t **inst_params[sizeof(inst_str)/sizeof(const char *)];

static void bringUpRuntime(const char *inifile) {
    int i, j, count, nsec;
    dictionary *dict = iniparser_load(inifile);


    // INIT
    for (j = 0; j < total_types; j++) {
        type_params[j] = NULL; type_counts[j] = 0; factory_names[j] = NULL;
        inst_params[j] = NULL; inst_counts[j] = 0;
        all_factories[j] = NULL; all_instances[j] = NULL;
    }

    // POPULATE TYPES
    DO_DEBUG(DEBUG_LVL_INFO)
        DEBUG("========= Create factories ==========\n");
    END_DEBUG

    nsec = iniparser_getnsec(dict);
    for (i = 0; i < nsec; i++) {
        for (j = 0; j < total_types; j++) {
            if (strncasecmp(type_str[j], iniparser_getsecname(dict, i), strlen(type_str[j]))==0) {
                type_counts[j]++;
            }
        }
    }

    for (i = 0; i < nsec; i++) {
        for (j = 0; j < total_types; j++) {
            if (strncasecmp(type_str[j], iniparser_getsecname(dict, i), strlen(type_str[j]))==0) {
                if(type_counts[j] && type_params[j]==NULL) {
                    type_params[j] = (ocrParamList_t **)calloc(1, type_counts[j] * sizeof(ocrParamList_t *));
                    factory_names[j] = (char **)calloc(1, type_counts[j] * sizeof(char *));
                    all_factories[j] = (void **)calloc(1, type_counts[j] * sizeof(void *));
                    count = 0;
                }
                factory_names[j][count] = populate_type(&type_params[j][count], j, dict, iniparser_getsecname(dict, i));
                all_factories[j][count] = create_factory(j, factory_names[j][count], type_params[j][count]);
                if (all_factories[j][count] == NULL) {
                    free(factory_names[j][count]);
                    factory_names[j][count] = NULL;
                }
                count++;
            }
        }
    }

    // POPULATE INSTANCES
    DO_DEBUG(DEBUG_LVL_INFO)
        DEBUG("========= Create instances ==========\n");
    END_DEBUG

    for (i = 0; i < nsec; i++) {
        for (j = 0; j < total_types; j++) {
            if (strncasecmp(inst_str[j], iniparser_getsecname(dict, i), strlen(inst_str[j]))==0) {
                int low, high, count;
                count = read_range(dict, iniparser_getsecname(dict, i), "id", &low, &high);
                inst_counts[j]+=count;
            }
        }
    }

    for (i = 0; i < nsec; i++) {
        for (j = total_types-1; j >= 0; j--) {
            if (strncasecmp(inst_str[j], iniparser_getsecname(dict, i), strlen(inst_str[j]))==0) {
                if(inst_counts[j] && inst_params[j] == NULL) {
                    DO_DEBUG(DEBUG_LVL_INFO)
                        DEBUG("Create %d instances of %s\n", inst_counts[j], inst_str[j]);
                    END_DEBUG
                    inst_params[j] = (ocrParamList_t **)calloc(1, inst_counts[j] * sizeof(ocrParamList_t *));
                    all_instances[j] = (ocrMappable_t **)calloc(1, inst_counts[j] * sizeof(ocrMappable_t *));
                    count = 0;
                }
                populate_inst(inst_params[j], all_instances[j], type_counts, factory_names, all_factories, all_instances, j, dict, iniparser_getsecname(dict, i));
            }
        }
    }

    // FIXME: Ugly follows
    ocrCompPlatformFactory_t *compPlatformFactory;
    compPlatformFactory = (ocrCompPlatformFactory_t *) all_factories[compplatform_type][0];
    compPlatformFactory->setIdentifyingFunctions(compPlatformFactory);

    // BUILD DEPENDENCES
    DO_DEBUG(DEBUG_LVL_INFO)
        DEBUG("========= Build dependences ==========\n");
    END_DEBUG

    for (i = 0; i < sizeof(deps)/sizeof(dep_t); i++) {
        build_deps(dict, deps[i].from, deps[i].to, deps[i].refstr, all_instances, inst_params);
    }
    dictionary_del (dict);
    // START EXECUTION
    DO_DEBUG(DEBUG_LVL_INFO)
        DEBUG("========= Start execution ==========\n");
    END_DEBUG
    ocrPolicyDomain_t *rootPolicy;
    rootPolicy = (ocrPolicyDomain_t *) all_instances[policydomain_type][0]; // FIXME: Ugly
    rootPolicy->start(rootPolicy);
}

static void freeUpRuntime (void)
{
    int i, j;

    for (i = 0; i < total_types; i++) {
        for (j = 0; j < type_counts[i]; j++) {
            free (all_factories[i][j]);
            free (type_params[i][j]);
            free (factory_names[i][j]);
        }
        free (all_factories[i]);
    }
/*
    for (i = 0; i < total_types; i++)
        for (j = 0; j < inst_counts[i]; j++)
            free_instance(all_instances[i][j], i);
*/
    for (i = 0; i < total_types; i++) {
        for (j = 0; j < inst_counts[i]; j++) {
            if(inst_params[i][j])
                free (inst_params[i][j]);
        }
        if(inst_params[i])
            free (inst_params[i]);
        free (all_instances[i]);
    }
}

static inline void checkNextArgExists(int i, int argc, char * option) {
    if (i == argc) {
        printf("No argument for ocr option %s\n", option);
        ASSERT(false);
    }
}

void ocrParseArgs(int argc, const char* argv[], ocrConfig_t * ocrConfig) {
    int cur = 1;
    int userArgs = argc;
    char * ocrOptPrefix = "-ocr:";
    int ocrOptPrefixLg = strlen(ocrOptPrefix);

    while(cur < argc) {
        const char * arg = argv[cur];
        if (strncmp(ocrOptPrefix, arg, ocrOptPrefixLg) == 0) {
            // This is an OCR option
            const char * ocrArg = arg+ocrOptPrefixLg;
            if (strcmp("cfg", ocrArg) == 0) {
                checkNextArgExists(cur, argc, "cfg");
                ocrConfig->iniFile = argv[cur+1];
                argv[cur] = NULL;
                argv[cur+1] = NULL;
                cur++; // skip param
                userArgs-=2;
            }
        }
        cur++;
    }
    // Pack argument list
    cur = 0;
    int head = 0;
    while(cur < argc) {
        if(argv[cur] != NULL) {
            if (cur == head) {
                head++;
            } else {
                argv[head] = argv[cur];
                argv[cur] = NULL;
                head++;
            }
        }
        cur++;
    }
    ocrConfig->userArgc = userArgs;
    ocrConfig->userArgv = (char **) argv;
}

/**
 * @param argc Number of user-level arguments to pack in a DB
 * @param argv The actual arguments
 */
static ocrGuid_t packUserArgumentsInDb(int argc, char ** argv) {
  // Now prepare arguments for the mainEdt
    ASSERT(argc < 64); // For now
    u32 i;
    u64* offsets = (u64*)malloc(argc*sizeof(u64));
    u64 argsUsed = 0ULL;
    u64 totalLength = 0;
    u32 maxArg = 0;
    // Gets all the possible offsets
    for(i = 0; i < argc; ++i) {
        // If the argument should be passed down
        offsets[maxArg++] = totalLength*sizeof(char);
        totalLength += strlen(argv[i]) + 1; // +1 for the NULL terminating char
        argsUsed |= (1ULL<<(63-i));
    }
    //--maxArg;
    // Create the datablock containing the parameters
    ocrGuid_t dbGuid;
    void* dbPtr;

    ocrDbCreate(&dbGuid, &dbPtr, totalLength + (maxArg + 1)*sizeof(u64),
                DB_PROP_NONE, NULL_GUID, NO_ALLOC);

    // Copy in the values to the data-block. The format is as follows:
    // - First 4 bytes encode the number of arguments (u64) (called argc)
    // - After that, an array of argc u64 offsets is encoded.
    // - The strings are then placed after that at the offsets encoded
    //
    // The use case is therefore as follows:
    // - Cast the DB to a u64* and read the number of arguments and
    //   offsets (or whatever offset you need)
    // - Case the DB to a char* and access the char* at the offset
    //   read. This will be a null terminated string.

    // Copy the metadata
    u64* dbAsU64 = (u64*)dbPtr;
    dbAsU64[0] = (u64)maxArg;
    u64 extraOffset = (maxArg + 1)*sizeof(u64);
    for(i = 0; i < maxArg; ++i) {
        dbAsU64[i+1] = offsets[i] + extraOffset;
    }

    // Copy the actual arguments
    char* dbAsChar = (char*)dbPtr;
    while(argsUsed) {
        u32 pos = fls64(argsUsed);
        argsUsed &= ~(1ULL<<pos);
        strcpy(dbAsChar + extraOffset + offsets[63 - pos], argv[63 - pos]);
    }

    return dbGuid;
}

/**!
 * Setup and starts the OCR runtime.
 * @param ocrConfig Data-structure containing runtime options
 */
void ocrInit(ocrConfig_t * ocrConfig) {

    const char * iniFile = ocrConfig->iniFile;
    ASSERT(iniFile != NULL);

    bringUpRuntime(iniFile);

    // At this point the runtime is up
#ifdef OCR_ENABLE_STATISTICS
    GocrFilterAggregator = NEW_FILTER(filedump);
    GocrFilterAggregator->create(GocrFilterAggregator, NULL, NULL);

    // HUGE HUGE Hack
    ocrStatsProcessCreate(&GfakeProcess, 0);
#endif
}

int __attribute__ ((weak)) main(int argc, const char* argv[]) {
    // Parse parameters. The idea is to extract the ones relevant
    // to the runtime and pass all the other ones down to the mainEdt
    ocrConfig_t ocrConfig;
    ocrParseArgs(argc, argv, &ocrConfig);

    // Setup up the runtime
    ocrInit(&ocrConfig);

    ocrGuid_t userArgsDbGuid = packUserArgumentsInDb(ocrConfig.userArgc, ocrConfig.userArgv);

    // Here the runtime is fully functional

    // Prepare the mainEdt for scheduling
    // We now create the EDT and launch it
    ocrGuid_t edtTemplateGuid, edtGuid;
    ocrEdtTemplateCreate(&edtTemplateGuid, mainEdt, 0, 1);
    ocrEdtCreate(&edtGuid, edtTemplateGuid, EDT_PARAM_DEF, /* paramv = */ NULL,
                /* depc = */ EDT_PARAM_DEF, /* depv = */ &userArgsDbGuid,
                EDT_PROP_NONE, NULL_GUID, NULL);

    ocrFinalize();

    return 0;
}

// TODO sagnak everything below is DUMB and RIDICULOUS and
// will have to be undone and done again

struct _ocrPolicyDomainLinkedListNode;
typedef struct _ocrPolicyDomainLinkedListNode {
    ocrPolicyDomain_t* pd;
    struct _ocrPolicyDomainLinkedListNode* next;
} ocrPolicyDomainLinkedListNode;

// walkthrough the linked list and return TRUE if instance exists
int isMember ( ocrPolicyDomainLinkedListNode *dummyHead, ocrPolicyDomainLinkedListNode *tail, ocrPolicyDomain_t* instance ) {
    ocrPolicyDomainLinkedListNode* curr = dummyHead->next;
    for ( ; NULL != curr && curr->pd != instance; curr = curr -> next ) {
    }
    return NULL != curr;
}

void recurseBuildDepthFirstSpanningTreeLinkedList ( ocrPolicyDomainLinkedListNode *dummyHead, ocrPolicyDomainLinkedListNode *tail, ocrPolicyDomain_t* currPD ) {
    ocrPolicyDomainLinkedListNode *currNode = (ocrPolicyDomainLinkedListNode*) malloc(sizeof(ocrPolicyDomainLinkedListNode));
    currNode -> pd = currPD;
    currNode -> next = NULL;
    tail -> next = currNode;
    tail = currNode;

    u64 neighborCount = currPD->neighborCount;
    u64 i = 0;
    for ( ; i < neighborCount; ++i ) {
        ocrPolicyDomain_t* currNeighbor = currPD->neighbors[i];
        if ( !isMember(dummyHead,tail,currNeighbor) ) {
            recurseBuildDepthFirstSpanningTreeLinkedList(dummyHead, tail, currNeighbor);
        }
    }
}

ocrPolicyDomainLinkedListNode *buildDepthFirstSpanningTreeLinkedList ( ocrPolicyDomain_t* currPD ) {

    ocrPolicyDomainLinkedListNode *dummyHead = (ocrPolicyDomainLinkedListNode*) malloc(sizeof(ocrPolicyDomainLinkedListNode));
    ocrPolicyDomainLinkedListNode *tail = dummyHead;
    dummyHead -> pd = NULL;
    dummyHead -> next = NULL;

    recurseBuildDepthFirstSpanningTreeLinkedList(dummyHead, tail, currPD);
    ocrPolicyDomainLinkedListNode *head = dummyHead->next;
    free(dummyHead);
    return head;
}

void destructLinkedList ( ocrPolicyDomainLinkedListNode* head ) {
    ocrPolicyDomainLinkedListNode *curr = head;
    ocrPolicyDomainLinkedListNode *next = NULL;
    while ( NULL != curr ) {
        next = curr->next;
        free(curr);
        curr = next;
    }
}
void linearTraverseFinish( ocrPolicyDomainLinkedListNode* curr ) {
    ocrPolicyDomainLinkedListNode *head = curr;
    for ( ; NULL != curr; curr = curr -> next ) {
        ocrPolicyDomain_t* pd = curr->pd;
        pd->finish(pd);
    }
    destructLinkedList(head);
}

void linearTraverseStop ( ocrPolicyDomainLinkedListNode* curr ) {
    ocrPolicyDomainLinkedListNode *head = curr;
    for ( ; NULL != curr; curr = curr -> next ) {
        ocrPolicyDomain_t* pd = curr->pd;
        pd->stop(pd);
    }
    destructLinkedList(head);
}

void ocrShutdown() {
    ocrPolicyDomain_t* currPD = getCurrentPD();
    ocrPolicyDomainLinkedListNode * spanningTreeHead = buildDepthFirstSpanningTreeLinkedList(currPD); //N^2
    linearTraverseFinish(spanningTreeHead);
}

void ocrFinalize() {
    ocrPolicyDomain_t* masterPD = getMasterPD();
    ocrPolicyDomainLinkedListNode * spanningTreeHead = buildDepthFirstSpanningTreeLinkedList(masterPD); //N^2
    linearTraverseStop(spanningTreeHead);
    masterPD->destruct(masterPD);
    freeUpRuntime();
    //TODO we need to start by stopping the master PD which
    //controls stopping down PD located on the same machine.
// #ifdef OCR_ENABLE_STATISTICS
//     ocrStatsProcessDestruct(&GfakeProcess);
//     GocrFilterAggregator->destruct(GocrFilterAggregator);
// #endif
}
