/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"

#ifdef SAL_LINUX

#include "debug.h"

#include "ocr-config.h"
#include "ocr-db.h"
#include "ocr-edt.h"
#include "ocr-runtime.h"
#include "ocr-types.h"
#include "ocr-sysboot.h"
#include "ocr-version.h"
#include "extensions/ocr-legacy.h"
#include "machine-description/ocr-machine.h"
#include "utils/ocr-utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// For platform-specific finalizer
#ifdef ENABLE_COMM_PLATFORM_GASNET
#include <gasnet.h>
#endif

// For TG specific location functions
#ifdef TG_X86_TARGET
#include "xstg-map.h"
#include "tg-bin-files.h"
#endif

#define DEBUG_TYPE INIPARSING

#include "apex.h"

/* Configuration parsing options */
#ifndef ENABLE_EXTENSION_LEGACY
// Defined in ocr-legacy.h but only included in ENABLE_EXTENSION_LEGACY
typedef struct _ocrConfig_t {
    int userArgc;          /**< Application argc (after having stripped the OCR arguments) */
    char ** userArgv;      /**< Application argv (after having stripped the OCR arguments) */
    const char * iniFile;  /**< INI configuration file for the runtime */
} ocrConfig_t;
#endif
enum {
    OPT_NONE, OPT_CONFIG, OPT_VERSION, OPT_HELP
};

// Helper methods
static struct options {
    char *flag;
    char *env_flag;
    s32 option;
    char *help;
} ocrOptionDesc[] = {
    {
        "cfg", "OCR_CONFIG", OPT_CONFIG, "-ocr:cfg <file> : the OCR runtime configuration file to use."
    },
    {
        "version", "", OPT_VERSION, "-ocr:version : print OCR version"
    },
    {
        "help", "", OPT_HELP, "-ocr:help : print this message"
    },
    {
        NULL, NULL, 0, NULL
    }
};

static void printHelp(void) {
    struct options *p;

    fprintf(stderr, "Usage: program [<OCR options>] [<program options>]\n");
    fprintf(stderr, "OCR options:\n");

    for (p = ocrOptionDesc; p->flag; ++p)
        if (p->help)
            fprintf(stderr, "    %s, env: %s\n", p->help, p->env_flag);

    fprintf(stderr, "\n");
    fprintf(stderr, "https://github.com/01org/ocr\n");
}

static void printVersion(void) {
    fprintf(stderr, "Open Community Runtime (OCR) %s\n", OCR_VERSION);
}

static void setIniFile(ocrConfig_t * ocrConfig, const char * value) {
    struct stat st;
    if (stat(value, &st) != 0) {
        fprintf(stderr, "ERROR: cannot find runtime configuration file: %s\n", value);
        exit(1);
    }
    ocrConfig->iniFile = value;
}

static inline void checkNextArgExists(s32 i, s32 argc, char * option) {
    if (i == argc) {
        fprintf(stderr, "ERROR: No argument for OCR option %s\n", option);
        exit(1);
    }
}

static void checkOcrOption(ocrConfig_t * ocrConfig) {
    if (ocrConfig->iniFile == NULL) {
        fprintf(stderr, "ERROR: no runtime configuration file provided\n");
        exit(1);
    }
}

static void readFromEnv(ocrConfig_t * ocrConfig) {
    // Go over OCR options description and check
    // if some of the env variables are set.
    struct options  *p;
    for (p = ocrOptionDesc; p->flag; ++p) {
        char * opt = getenv(p->env_flag);
        // If variable defined and has value
        if ((opt != NULL) && (strcmp(opt, "") != 0)) {
            switch (p->option) {
            case OPT_CONFIG:
                setIniFile(ocrConfig, opt);
                break;
            }
        }
    }
}

static s32 readFromArgs(s32 argc, const char* argv[], ocrConfig_t * ocrConfig) {
    // Override any env variable with command line option
    s32 cur = 1;
    s32 userArgs = argc;
    char * ocrOptPrefix = "-ocr:";
    s32 ocrOptPrefixLg = strlen(ocrOptPrefix);
    while(cur < argc) {
        const char * arg = argv[cur];
        if (strncmp(ocrOptPrefix, arg, ocrOptPrefixLg) == 0) {
            // This is an OCR option
            const char * ocrArg = arg+ocrOptPrefixLg;
            if (strcmp("cfg", ocrArg) == 0) {
                checkNextArgExists(cur, argc, "cfg");
                setIniFile(ocrConfig, argv[cur+1]);
                argv[cur] = NULL;
                argv[cur+1] = NULL;
                cur++; // skip param
                userArgs-=2;
            } else if (strcmp("version", ocrArg) == 0) {
                printVersion();
                exit(0);
                break;
            } else if (strcmp("help", ocrArg) == 0) {
                printHelp();
                exit(0);
                break;
            }
        }
        cur++;
    }
    return userArgs;
}

static void ocrConfigInit(ocrConfig_t * ocrConfig) {
    ocrConfig->userArgc = 0;
    ocrConfig->userArgv = NULL;
    ocrConfig->iniFile = NULL;
}

void ocrParseArgs(s32 argc, const char* argv[], ocrConfig_t * ocrConfig) {

    // Zero-ed the ocrConfig
    ocrConfigInit(ocrConfig);

    // First retrieve options from environment variables
    readFromEnv(ocrConfig);

    // Override any env variable with command line args
    s32 userArgs = readFromArgs(argc, argv, ocrConfig);

    // Check for mandatory options
    checkOcrOption(ocrConfig);

    // Pack argument list
    s32 cur = 0;
    s32 head = 0;
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

extern u32 type_max[];

const char *type_str[] = {
    "GuidType",
    "MemPlatformType",
    "MemTargetType",
    "AllocatorType",
    "CommApiType",
    "CommPlatformType",
    "CompPlatformType",
    "CompTargetType",
    "WorkPileType",
    "WorkerType",
    "SchedulerType",
    "SchedulerObjectType",
    "SchedulerHeuristicType",
    "PolicyDomainType",
    "TaskType",
    "TaskTemplateType",
    "DataBlockType",
    "EventType",
};

const char *inst_str[] = {
    "GuidInst",
    "MemPlatformInst",
    "MemTargetInst",
    "AllocatorInst",
    "CommApiInst",
    "CommPlatformInst",
    "CompPlatformInst",
    "CompTargetInst",
    "WorkPileInst",
    "WorkerInst",
    "SchedulerInst",
    "SchedulerObjectInst",
    "SchedulerHeuristicInst",
    "PolicyDomainInst",
    "NULL",
    "NULL",
    "NULL",
    "NULL",
};

/* The following arrays define the list of dependences */
#define OCR_CONFIG_DEP_COUNT(x) (sizeof(x) / sizeof(dep_t))
static dep_t instanceDeps[] = {
    { memtarget_type, memplatform_type, "memplatform"},
    { allocator_type, memtarget_type, "memtarget"},
    { commapi_type, commplatform_type, "commplatform"},
    { comptarget_type, compplatform_type, "compplatform"},
    { worker_type, comptarget_type, "comptarget"},
    { scheduler_type, workpile_type, "workpile"},
    { scheduler_type, schedulerObject_type, "schedulerObject"},
    { scheduler_type, schedulerHeuristic_type, "schedulerHeuristic"},
    { policydomain_type, guid_type, "guid"},
    { policydomain_type, allocator_type, "allocator"},
    { policydomain_type, worker_type, "worker"},
    { policydomain_type, scheduler_type, "scheduler"},
    { policydomain_type, commapi_type, "commapi"},
    { policydomain_type, policydomain_type, "parent"},
};
// Special case of policy domain pointing to types rather than instances
static dep_t typeDeps[] = {
    { policydomain_type, taskfactory_type, "taskfactory"},
    { policydomain_type, tasktemplatefactory_type, "tasktemplatefactory"},
    { policydomain_type, datablockfactory_type, "datablockfactory"},
    { policydomain_type, eventfactory_type, "eventfactory"},
    { policydomain_type, schedulerObject_type, "schedulerObjectfactory"},
};

extern char* populate_type(ocrParamList_t **type_param, type_enum index, dictionary *dict, char *secname);
int populate_inst(ocrParamList_t **inst_param, int inst_param_size, void **instance, int *type_counts, char ***factory_names, void ***all_factories, void ***all_instances, type_enum index, dictionary *dict, char *secname);
extern int build_deps (dictionary *dict, int A, int B, char *refstr, void ***all_instances, ocrParamList_t ***inst_params);
extern int build_deps_types (int A, int B, char *refstr, void **pdinst, int pdcount, int *type_counts, void ***all_factories, ocrParamList_t ***type_params);
extern void *create_factory (type_enum index, char *factory_name, ocrParamList_t *paramlist);
extern int read_range(dictionary *dict, char *sec, char *field, int *low, int *high);
extern void free_instance(void *instance, type_enum inst_type);

ocrGuid_t __attribute__ ((weak)) mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    // This is just to make the linker happy and shouldn't be executed
    printf("error: no mainEdt defined.\n");
    ASSERT(false);
    return NULL_GUID;
}

void **all_factories[sizeof(type_str)/sizeof(const char *)];
void **all_instances[sizeof(inst_str)/sizeof(const char *)];
int total_types = sizeof(type_str)/sizeof(const char *);
int type_counts[sizeof(type_str)/sizeof(const char *)];
int inst_counts[sizeof(inst_str)/sizeof(const char *)];
ocrParamList_t **type_params[sizeof(type_str)/sizeof(const char *)];
char **factory_names[sizeof(type_str)/sizeof(const char *)];
ocrParamList_t **inst_params[sizeof(inst_str)/sizeof(const char *)];

#ifdef ENABLE_BUILDER_ONLY

#define APP_BINARY    "CrossPlatform:app_file"
#define OUTPUT_BINARY "CrossPlatform:struct_file"
#define ARGS_BINARY   "CrossPlatform:args_file"
#define START_ADDRESS "CrossPlatform:start_address"
#define DRAM_OFFSET   "CrossPlatform:dram_offset"

char *app_binary;
char *output_binary;
char *args_binary;

extern int extract_functions(const char *);
extern void free_functions(void);
extern void *getAddress(const char *fname);
extern char persistent_chunk[];
extern u64 persistent_pointer;
extern u64 args_pointer;
extern u64 dram_offset;

/* Format of this file:
 *
 * +--------------------------+
 * | header size (incl) (u64) | = (6)*sizeof(u64)
 * +--------------------------+
 * |  abs. location  (u64)    | = CeMemSize - 5K
 * +--------------------------+
 * |  abs.address of PD (u64) | = abs.location + PD offset
 * +--------------------------+
 * |ptr to salPdDriver() (u64)| = pointer to salPdDriver
 * +--------------------------+
 * |  size (of structs) (u64) | = persistent_pointer
 * +--------------------------+
 * |      PD->mylocation      | = DEPRECATED, to be removed
 * +--------------------------+
 * |                          |
 * |     structs be here      |
 * |                          |
 * +--------------------------+
 */

void dumpStructs(void *pd, const char* output_binary, u64 start_address) {
    FILE *fp = fopen(output_binary, "w");
    u64 value, i, totu64 = 0;
    u64 offset;
    u64 *ptrs = (u64 *)&persistent_chunk;

    if(fp == NULL) printf("Unable to open file %s for writing\n", output_binary);
    else {

        // Write the header
        // Header size
        value = 6*sizeof(u64);
        fwrite(&value, sizeof(u64), 1, fp);
        DPRINTF(DEBUG_LVL_VERB, "Wrote header size: 0x%"PRIx64"\n", value);
        totu64++;

        // Absolute location - currently read from config file
        fwrite(&start_address, sizeof(u64), 1, fp);
        DPRINTF(DEBUG_LVL_VERB, "Wrote abs location: 0x%"PRIx64"\n", start_address);
        totu64++;

        // PD address
        offset = (u64)pd - (u64)&persistent_chunk + (u64)start_address;
        fwrite(&offset, sizeof(u64), 1, fp);
        DPRINTF(DEBUG_LVL_VERB, "Wrote PD address: 0x%"PRIx64"\n", offset);
        totu64++;

        value = (u64)getAddress("salPdDriver");
        fwrite(&value, sizeof(u64), 1, fp);
        DPRINTF(DEBUG_LVL_VERB, "Wrote salPdDriver address: 0x%"PRIx64"\n", value);
        totu64++;

        // Size of all structs
        fwrite(&persistent_pointer, sizeof(u64), 1, fp);
        DPRINTF(DEBUG_LVL_VERB, "Wrote size of all structs: 0x%"PRIx64"\n", persistent_pointer);
        totu64++;

        // myLocation
        offset = (u64)(&((ocrPolicyDomain_t *)pd)->myLocation) - (u64)&persistent_chunk + (u64)start_address;
        fwrite(&offset, sizeof(u64), 1, fp);
        DPRINTF(DEBUG_LVL_VERB, "Wrote my location: 0x%"PRIx64"\n", offset);
        totu64++;

        // Fix up all the pointers
        // (Bug #596: potential low-likelihood bug due to address collision; need to be improved upon)
        for(i = 0; i<(persistent_pointer/sizeof(u64)); i++) {
            if((ptrs[i] > (u64)ptrs) && (ptrs[i] < (u64)(ptrs+persistent_pointer))) {
                ptrs[i] -= (u64)ptrs;
                ptrs[i] += start_address;
            }
            DPRINTF(DEBUG_LVL_VVERB, "ptrs[%"PRId64"]: 0x%"PRIx64"\n", i, ptrs[i]);
        }
        fwrite(&persistent_chunk, sizeof(char), persistent_pointer, fp);
        DPRINTF(DEBUG_LVL_INFO, "Wrote %"PRId64" bytes to %s\n", persistent_pointer+totu64*8, output_binary);
    }
    fclose(fp);
}

void dumpArgs(const char* args_binary) {
    FILE *fp = fopen(args_binary, "w");
    void *userArgs = userArgsGet();

    if(fp == NULL) printf("Unable to open file %s for writing\n", args_binary);
    else {
        fwrite(userArgs, sizeof(u8), args_pointer, fp);
        DPRINTF(DEBUG_LVL_INFO, "Wrote %"PRId64" bytes to %s\n", args_pointer, args_binary);
    }
    fclose(fp);
}

void builderPreamble(dictionary *dict) {

    app_binary = iniparser_getstring(dict, APP_BINARY, NULL);
    output_binary = iniparser_getstring(dict, OUTPUT_BINARY, NULL);
    args_binary = iniparser_getstring(dict, ARGS_BINARY, NULL);
    dram_offset = (u64)iniparser_getlonglong(dict, DRAM_OFFSET, 0);

    if(app_binary==NULL || output_binary==NULL) {
        printf("Unable to read %s and %s; got %s and %s respectively\n", APP_BINARY, OUTPUT_BINARY, app_binary, output_binary);
    } else {
        extract_functions(app_binary);
    }
}

#endif

extern bool key_exists(dictionary *dict, char *sec, char *field);

/**
 * @brief Calls platformn specific initialization code.
 *
 * Allows to call platform specific initialization code
 * before OCR is built and started.
 *
 * The only concrete use case for this function is to
 * call MPI_INIT. Implementers should avoid relying on
 * it as it may be deprecated in near future.
 */
void platformSpecificInit(ocrConfig_t * ocrConfig) {
#ifdef ENABLE_COMM_PLATFORM_MPI
    extern void platformInitMPIComm(int *argc, char *** argv);
    platformInitMPIComm(&ocrConfig->userArgc, &ocrConfig->userArgv);
#endif

#ifdef ENABLE_COMM_PLATFORM_GASNET
    extern void platformInitGasnetComm(int *argc, char *** argv);
    platformInitGasnetComm(&ocrConfig->userArgc, &ocrConfig->userArgv);
#endif
}

/**
 * @brief Calls platform specific finalizer code.
 *
 * Allows to call platform specific finalizer code
 * that may be invoked independently of any OCR
 * module being still alive.
 *
 * Warning ! the call may eventually call 'exit'.
 */
void platformSpecificFinalizer(u8 returnCode) {
#ifdef ENABLE_COMM_PLATFORM_GASNET
    // Spec says client should include a barrier before gasnet_exit()
    gasnet_barrier_notify(0,GASNET_BARRIERFLAG_ANONYMOUS);
    gasnet_barrier_wait(0,GASNET_BARRIERFLAG_ANONYMOUS);
    gasnet_exit(returnCode);
#endif
}

void bringUpRuntime(ocrConfig_t *ocrConfig) {
    const char *inifile = ocrConfig->iniFile;
    ASSERT(inifile != NULL);

    int i, j, nsec;
    dictionary *dict = iniparser_load(inifile);

#ifdef ENABLE_BUILDER_ONLY
    builderPreamble(dict);
#endif

    // INIT
    for (j = 0; j < total_types; j++) {
        type_params[j] = NULL;
        type_counts[j] = 0;
        factory_names[j] = NULL;
        inst_params[j] = NULL;
        inst_counts[j] = 0;
        all_factories[j] = NULL;
        all_instances[j] = NULL;
    }

    // Check the config file version number
    char *version = (char *) iniparser_getstring(dict, "General:version", NULL);
    if(version == NULL || (strncmp(version, OCR_VERSION, strlen(OCR_VERSION)) != 0))
        DPRINTF(DEBUG_LVL_WARN, "Configuration file (%s) doesn't match OCR version %s, use at your own risk\n", version, OCR_VERSION);

    // POPULATE TYPES
    DPRINTF(DEBUG_LVL_INFO, "========= Create factories ==========\n");

    nsec = iniparser_getnsec(dict);
    // Initialize type_counts by counting the number of things we find in the config file
    for (i = 0; i < nsec; i++) {
        char * secname = iniparser_getsecname(dict, i);
        for (j = 0; j < total_types; j++) {
            if (strncasecmp(type_str[j], secname, strlen(type_str[j]))==0) {
                type_counts[j] += 1;
            }
        }
    }
#ifdef OCR_ASSERT
    for (j = 0; j < total_types; j++) {
        ASSERT(type_counts[j] <=  type_max[j]);
    }
#endif

    // We change the way we allocate all_factories to create everything first and then
    // pointers into the array later. This allows us to deal with factories as one big chunk
    // and we can index them all sequentially.

    u32 countNP = 0; // Number of non-persistent factories
    u32 countP  = 0; // Number of persistent factories
    for(j=0;j<taskfactory_type; ++j) {
        countNP += type_counts[j];
    }
    for(; j<total_types; ++j) {
        countP += type_counts[j];
    }

    // Allocate the parameters, names and factories
    type_params[0] = (ocrParamList_t**)runtimeChunkAlloc((countNP+countP)*sizeof(ocrParamList_t*), NONPERSISTENT_CHUNK);
    factory_names[0] = (char**)runtimeChunkAlloc((countNP+countP)*sizeof(char*), NONPERSISTENT_CHUNK);
    all_factories[0] = (void**)runtimeChunkAlloc(countNP*sizeof(void*), NONPERSISTENT_CHUNK);
    all_factories[taskfactory_type] = (void**)runtimeChunkAlloc(countP*sizeof(void*), PERSISTENT_CHUNK);

    // Set up sub-pointers properly
    u32 runningCount = type_counts[0];
    for(j=1; j<taskfactory_type; ++j) {
        type_params[j] = &(type_params[0][runningCount]);
        factory_names[j] = &(factory_names[0][runningCount]);
        all_factories[j] = &(all_factories[0][runningCount]);
        runningCount += type_counts[j];
    }
    u32 NPrunningCount = runningCount;
    for(; j<total_types; ++j) {
        type_params[j] = &(type_params[0][runningCount]);
        factory_names[j] = &(factory_names[0][runningCount]);
        all_factories[j] = &(all_factories[taskfactory_type][runningCount-NPrunningCount]);
        runningCount += type_counts[j];
    }
    // Reset type_counts to properly index into the arrays
    for(j=0; j<total_types; ++j) {
        type_counts[j] = 0;
    }
    u32 totalCount = 0;
    // Now populate the types.
    // We populate in all_factories first the types that we care to give
    // an index to (the persistent factories) and then all the rest. This is
    // to ensure that the IDs that are passed (totalCount) do actually match
    // the factories array that will be created in the PD
    for(j = taskfactory_type; j < total_types; j++) {
        for(i = 0; i < nsec; i++) {
            char *secname = iniparser_getsecname(dict, i);
            if (strncasecmp(type_str[j], secname, strlen(type_str[j]))==0) {
                factory_names[j][type_counts[j]] = populate_type(&type_params[j][type_counts[j]], j, dict, secname);
                // Set the factory ID for this factory
                type_params[j][type_counts[j]]->id = totalCount;
                all_factories[j][type_counts[j]] = create_factory(j, factory_names[j][type_counts[j]], type_params[j][type_counts[j]]);

                if (all_factories[j][type_counts[j]] == NULL) {
                    runtimeChunkFree((u64)factory_names[j][type_counts[j]], NONPERSISTENT_CHUNK);
                    factory_names[j][type_counts[j]] = NULL;
                }
                ++type_counts[j];
                ++totalCount;
            }
        }
    }

    // Same thing for all the other ones
    for(j = 0; j < taskfactory_type; j++) {
        for(i = 0; i < nsec; i++) {
            char *secname = iniparser_getsecname(dict, i);
            if (strncasecmp(type_str[j], secname, strlen(type_str[j]))==0) {
                factory_names[j][type_counts[j]] = populate_type(&type_params[j][type_counts[j]], j, dict, secname);
                // Set the factory ID for this factory
                type_params[j][type_counts[j]]->id = totalCount;
                all_factories[j][type_counts[j]] = create_factory(j, factory_names[j][type_counts[j]], type_params[j][type_counts[j]]);

                if (all_factories[j][type_counts[j]] == NULL) {
                    runtimeChunkFree((u64)factory_names[j][type_counts[j]], NONPERSISTENT_CHUNK);
                    factory_names[j][type_counts[j]] = NULL;
                }
                ++type_counts[j];
                ++totalCount;
            }
        }
    }

    // POPULATE INSTANCES
    DPRINTF(DEBUG_LVL_INFO, "========= Create instances ==========\n");

    for (i = 0; i < nsec; i++) {
        char * secname = iniparser_getsecname(dict, i);
        for (j = 0; j < total_types; j++) {
            if (strncasecmp(inst_str[j], secname, strlen(inst_str[j]))==0) {
                int low, high, count;
                count = read_range(dict, secname, "id", &low, &high);
                inst_counts[j]+=count;
            }
        }
    }

    for (i = 0; i < nsec; i++) {
        char * secname = iniparser_getsecname(dict, i);
        for (j = total_types-1; j >= 0; j--) {
            if (strncasecmp(inst_str[j], secname, strlen(inst_str[j]))==0) {
                if(inst_counts[j] && inst_params[j] == NULL) {
                    DPRINTF(DEBUG_LVL_INFO, "Create %"PRId32" instances of %s\n", inst_counts[j], inst_str[j]);
                    inst_params[j] = (ocrParamList_t **)runtimeChunkAlloc(inst_counts[j] * sizeof(ocrParamList_t *), NONPERSISTENT_CHUNK);
                    all_instances[j] = (void **)runtimeChunkAlloc((inst_counts[j]+1) * sizeof(void *), NONPERSISTENT_CHUNK); // We create an "end of instances" marker
                    all_instances[j][inst_counts[j]] = NULL;
                }
                populate_inst(inst_params[j], inst_counts[j], all_instances[j], type_counts, factory_names, all_factories, all_instances, j, dict, secname);
            }
        }
    }

    // BUILD DEPENDENCES
    DPRINTF(DEBUG_LVL_INFO, "========= Build dependences ==========\n");

    for (i = 0; i < OCR_CONFIG_DEP_COUNT(instanceDeps); i++) {
        build_deps(dict, instanceDeps[i].from, instanceDeps[i].to, instanceDeps[i].refstr,
                   all_instances, inst_params);
    }

    // Special case of policy domain pointing to types rather than instances
    for (i = 0; i < OCR_CONFIG_DEP_COUNT(typeDeps); i++) {
        build_deps_types(typeDeps[i].from, typeDeps[i].to, typeDeps[i].refstr,
                         all_instances[typeDeps[i].from], inst_counts[typeDeps[i].from],
                         type_counts, all_factories, type_params);
    }

#ifdef ENABLE_BUILDER_ONLY
    // If we are doing the builder, we set this up here because we can't do runtimeChunkAlloc in ce-policy.c like
    // we do for everything but TG
    for(i=0; i < inst_counts[policydomain_type]; ++i) {
        ocrPolicyDomain_t *policy = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
        policy->neighbors = (ocrLocation_t*)runtimeChunkAlloc(policy->neighborCount*sizeof(ocrLocation_t), PERSISTENT_CHUNK);
    }
#else
    // We point each PD to all other PDs if needed for neighbor discovery.
    // Note that at this point neighborsPDs points to *everyone*. If needed, when the
    // PD starts, it will update its copy of neighborsPDs to match exactly what it needs
    for(i=0; i < inst_counts[policydomain_type]; ++i) {
        ((ocrPolicyDomain_t*)all_instances[policydomain_type][i])->neighborPDs = (ocrPolicyDomain_t**)all_instances[policydomain_type];
    }
#endif
#ifdef TG_X86_TARGET
    // Bug #694 Really fugly!!
    // In TG-x86, we are going to create an array to contain all the policy domains so that we
    // can deguidify things that are "owned" by other PDs.
    // WARNING: This code assumes that if you have more than 1 block, you have MAX_NUM_XE and MAX_NUM_CE
    // and that if you have more than one cluster, each cluster has MAX_NUM_BLOCK. This is similar to the restriction
    // made by the neighbor discovery but adds the restriction on the number of XE/CE

    // Since we already have all_instances, we can just reuse that but we will re-order things
    // so that it is indiceable using cluster*MAX_NUM_BLOCK + block*(MAX_NUM_XE+MAX_NUM_CE) + agent
    for(i=0; i < inst_counts[policydomain_type]; ++i) {
        // We figure out myLocation based on its current value which is just an index (based on xeCount = 8)
        // Assumptions:
        //    - same number of XEs per block
        //    - if multiple units, all have MAX_NUM_BLOCK blocks in them
        // 0: CE of block 0
        // 1-8: XEs of block 0
        // 9: CE of block 1
        // 10-17: XEs of block 1
        // 18: CE of block 2
        // 19-26: XEs of block 2
        // ...
        // This scheme assumes that ID_AGENT_CE is 0 and that the XEs follow
        COMPILE_ASSERT(0 == ID_AGENT_CE);
        COMPILE_ASSERT(MAX_NUM_CE == ID_AGENT_XE0);
        ocrPolicyDomain_t *policy = (ocrPolicyDomain_t*)(all_instances[policydomain_type][i]);
        u32 myCluster = (policy->myLocation) / ((MAX_NUM_XE + MAX_NUM_CE)*MAX_NUM_BLOCK);
        u32 myBlock = (policy->myLocation - myCluster*((MAX_NUM_XE + MAX_NUM_CE)*MAX_NUM_BLOCK)) / (MAX_NUM_XE + MAX_NUM_CE);
        policy->myLocation = MAKE_CORE_ID(0, 0, 0, myCluster, myBlock, (policy->myLocation - myCluster*MAX_NUM_BLOCK - myBlock*(MAX_NUM_XE+MAX_NUM_CE)));
    }

    ocrPolicyDomain_t* tpd = (ocrPolicyDomain_t*)(all_instances[policydomain_type][0]);
    ASSERT(inst_counts[policydomain_type] > 0);
    i = 0;
    while(true) {
        u32 tpdIdx = CLUSTER_FROM_ID(tpd->myLocation)*MAX_NUM_BLOCK +
            BLOCK_FROM_ID(tpd->myLocation)*(MAX_NUM_XE+MAX_NUM_CE) +
            AGENT_FROM_ID(tpd->myLocation);
        if(tpdIdx != i) {
            // We move this one to the proper location and continue to
            // do so until we have the proper domain at i
            ocrPolicyDomain_t *tt = (ocrPolicyDomain_t*)(all_instances[policydomain_type][tpdIdx]);
            all_instances[policydomain_type][tpdIdx] = (void*)tpd;
            tpd = tt;
        } else {
            all_instances[policydomain_type][i] = (void*)tpd;
            if(++i < inst_counts[policydomain_type])
                tpd = (ocrPolicyDomain_t*)(all_instances[policydomain_type][i]);
            else
                break;
        }
    }
#endif

 //     for (i = 0; i < nsec; i++) {
 //         char *secname = iniparser_getsecname(dict, i);
 //         if (strncasecmp("PolicyDomainInst", secname, strlen("PolicyDomainInst"))==0) {
 //           if (key_exists(dict, secname, "neighbors")) {
 //             int neighbors_low, neighbors_high, neighbors_count;
 //             neighbors_count = read_range(dict, secname, "neighbors", &neighbors_low, &neighbors_high);
 //             if (neighbors_count > 0) {
 //                 int low, high, count;
 //                 count = read_range(dict, secname, "id", &low, &high);
 //                 ASSERT(count == 1 && low == high);
 //                 ocrPolicyDomain_t *pd = (ocrPolicyDomain_t*)all_instances[policydomain_type][low];
 //                 ASSERT(neighbors_count == pd->neighborCount);
 //                 pd->neighbors = (ocrLocation_t*)runtimeChunkAlloc(sizeof(ocrLocation_t) * neighbors_count, PERSISTENT_CHUNK);
 // #ifndef ENABLE_BUILDER_ONLY
 //                 pd->neighborPDs = (ocrPolicyDomain_t**)runtimeChunkAlloc(sizeof(ocrPolicyDomain_t*) * neighbors_count, NONPERSISTENT_CHUNK);
 //                 int idx = 0;
 //                 DPRINTF(DEBUG_LVL_VERB, "PD%"PRIu64" neighbors (%"PRId32"): ", (u64)pd->myLocation, neighbors_count);
 //                 for (j = neighbors_low; j <= neighbors_high; j++) {
 //                     if (j != low) {
 //                         ocrPolicyDomain_t *neighborPd = (ocrPolicyDomain_t*)all_instances[policydomain_type][j];
 //                         pd->neighborPDs[idx] = neighborPd;
 //                         pd->neighbors[idx++] = neighborPd->myLocation;
 //                         DPRINTF(DEBUG_LVL_VERB, "%"PRIu64" ", (u64)neighborPd->myLocation);
 //                     }
 //                 }
 //                 DPRINTF(DEBUG_LVL_VERB, "\n");
 // #endif
 //             }
 //           }
 //         }
 //     }

    // START EXECUTION
    DPRINTF(DEBUG_LVL_INFO, "========= Start execution ==========\n");

#ifdef ENABLE_BUILDER_ONLY
    {
        u64 start_address = (u64)iniparser_getlonglong(dict, START_ADDRESS, 0);
        for(i = 0; i < inst_counts[policydomain_type]; i++)
            dumpStructs(all_instances[policydomain_type][i], output_binary, start_address);
        if(args_binary) dumpArgs(args_binary);
        free_functions();
    }
#else
    ocrPolicyDomain_t *rootPolicy;
    u32 rootPolicyID = 0;
#ifdef TG_X86_TARGET
    rootPolicyID = ID_AGENT_CE;
#endif
    rootPolicy = (ocrPolicyDomain_t *)all_instances[policydomain_type][rootPolicyID]; // This is Unit 0, Block 0 CE
    ocrPolicyDomain_t *otherPolicyDomains = NULL;
    // Runlevel switch. Everything is initialized at this point and so we switch
    // to the first runlevel (CONFIG_PARSE). This will, in particular, enable all
    // modules within a PD to become aware of each other's runaction requirements
    // Transition all PDs to CONFIG_PARSE

    for(i = 0; i < inst_counts[policydomain_type]; ++i) {
        if(i == rootPolicyID) continue;
        otherPolicyDomains = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
        RESULT_ASSERT(otherPolicyDomains->fcts.switchRunlevel(otherPolicyDomains, RL_CONFIG_PARSE,
                                                              RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_PD_MASTER),
                      ==, 0);
    }
    RESULT_ASSERT(rootPolicy->fcts.switchRunlevel(rootPolicy, RL_CONFIG_PARSE, RL_REQUEST |
                                                  RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
                  ==, 0);

    // BUG #583: This part may need to be specialized a bit. Basically,
    // we need this capable thread to determine which PDs it is responsible
    // for bringing to PD_OK. On TG, this will only be the one PD (each core brings
    // its own PD to a start) whereas on TG-emul, it is all PDs. This goes back
    // to the issue of having the runtime "select" the part of the configuration
    // that is of interest to it.

    // Transition all PDs to NETWORK_OK
    // At this stage, everything is inert so all operations are synchronous
    for(i = 0; i < inst_counts[policydomain_type]; ++i) {
        if(i == rootPolicyID) continue;
        otherPolicyDomains = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
        RESULT_ASSERT(otherPolicyDomains->fcts.switchRunlevel(otherPolicyDomains, RL_NETWORK_OK,
                                                              RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_PD_MASTER),
                      ==, 0);
    }
    RESULT_ASSERT(rootPolicy->fcts.switchRunlevel(rootPolicy, RL_NETWORK_OK,
                                                  RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
                  ==, 0);

    // Transition all PDs to PD_OK
    // This creates a capable module for each PD. The worker/thread executing
    // this code is the capable thread in the rootPolicy. We then continue executing
    // with just rootPolicy
    for(i = 0; i < inst_counts[policydomain_type]; ++i) {
        if(i == rootPolicyID) continue;
        otherPolicyDomains = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
        RESULT_ASSERT(otherPolicyDomains->fcts.switchRunlevel(otherPolicyDomains, RL_PD_OK,
                                                              RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_PD_MASTER),
                      ==, 0);
    }
    RESULT_ASSERT(rootPolicy->fcts.switchRunlevel(rootPolicy, RL_PD_OK,
                                                  RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
                  ==, 0);

    // Transition the root PD to MEMORY_OK
    RESULT_ASSERT(rootPolicy->fcts.switchRunlevel(rootPolicy, RL_MEMORY_OK,
                                                  RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
                  ==, 0);

    // Transition the root PD to GUID_OK and wait for all PDs to transition
    RESULT_ASSERT(rootPolicy->fcts.switchRunlevel(rootPolicy, RL_GUID_OK,
                                                  RL_REQUEST | RL_BARRIER | RL_BRING_UP | RL_NODE_MASTER),
                  ==, 0);

    // Transition the root PD to COMPUTE_OK
    RESULT_ASSERT(rootPolicy->fcts.switchRunlevel(rootPolicy, RL_COMPUTE_OK,
                                                  RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
                  ==, 0);

#endif
    iniparser_freedict(dict);

#ifdef ENABLE_BUILDER_ONLY
    exit(0);
#endif
}

void freeUpRuntime (bool doTeardown, u8 *returnCode) {
    u32 i, j;
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomain_t *otherPolicyDomains = NULL;
    u32 rootPolicyID = 0;
#ifdef TG_X86_TARGET
    rootPolicyID = ID_AGENT_CE;
#endif
    if(doTeardown) {
        // When we need to do the tear-down, we need to continue from RL_GUID_OK all the way down to CONFIG_PARSE
        // Bug #597 Need to ensure that only NODE_MASTER comes out of here. Other PD_MASTER need to stay in their PDs
        RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_GUID_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                      ==, 0);
        RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_MEMORY_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                      ==, 0);

        // Bug #597 The switch of other PD's runlevels needs to happen with ocrPdMessages_t
        RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_PD_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                      ==, 0);

        // Here everyone is down except NODE_MASTER
        for(i = 0; i < inst_counts[policydomain_type]; ++i) {
            if(i == rootPolicyID) continue;
            otherPolicyDomains = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
            RESULT_ASSERT(otherPolicyDomains->fcts.switchRunlevel(otherPolicyDomains, RL_NETWORK_OK,
                                                                  RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_PD_MASTER), ==, 0);
        }
        RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_NETWORK_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                      ==, 0);

        for(i = 0; i < inst_counts[policydomain_type]; ++i) {
            if(i == rootPolicyID) continue;
            otherPolicyDomains = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
            RESULT_ASSERT(otherPolicyDomains->fcts.switchRunlevel(otherPolicyDomains, RL_CONFIG_PARSE,
                                                                  RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_PD_MASTER), ==, 0);
        }
        RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_CONFIG_PARSE, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                      ==, 0);
    }

    // Read the return code
    *returnCode = pd->shutdownCode;

    for(i = 0; i < inst_counts[policydomain_type]; ++i) {
        if(i == rootPolicyID) continue;
        otherPolicyDomains = (ocrPolicyDomain_t*)all_instances[policydomain_type][i];
        otherPolicyDomains->fcts.destruct(otherPolicyDomains);
    }

    pd->fcts.destruct(pd);

    for (i = 0; i < total_types; i++) {
        for (j = 0; j < type_counts[i]; j++) {
            if(i<=policydomain_type)
                if(all_factories[i][j])
                    runtimeChunkFree((u64)all_factories[i][j], NONPERSISTENT_CHUNK);
            if(type_params[i][j])
                runtimeChunkFree((u64)type_params[i][j], NONPERSISTENT_CHUNK);
            if(factory_names[i][j])
                runtimeChunkFree((u64)factory_names[i][j], NONPERSISTENT_CHUNK);
        }
    }

    runtimeChunkFree((u64)all_factories[0], NONPERSISTENT_CHUNK);
    runtimeChunkFree((u64)type_params[0], NONPERSISTENT_CHUNK);
    runtimeChunkFree((u64)factory_names[0], NONPERSISTENT_CHUNK);
    runtimeChunkFree((u64)all_factories[taskfactory_type], PERSISTENT_CHUNK);
    for (i = 0; i < total_types; i++) {
        for (j = 0; j < inst_counts[i]; j++) {
            if(inst_params[i][j])
                runtimeChunkFree((u64)inst_params[i][j], NONPERSISTENT_CHUNK);
        }
        if(inst_params[i])
            runtimeChunkFree((u64)inst_params[i], NONPERSISTENT_CHUNK);
        if(all_instances[i])
            runtimeChunkFree((u64)all_instances[i], NONPERSISTENT_CHUNK);
    }
}


/**
 * @brief Packs user args in a contiguous chunk of memory.
 *
 * The packing contains some information at the beginning of the memory
 * that is not seen by the mainEdt and are stripped out by the runtime
 * before spawning the mainEdt.
 *
 * The encoding format is as follow:
 *      |totalLengh|argc|offsets|argv strings|
 * Size of each element:
 *      |u64|u64|u64*argc|strlen(argv[0:argc-1])+1|
 *          ^ -> '0' of the offset calculation
 * Note
 * - totalLengh:   Total length of the packed arguments (everything minus this u64 part)
 * - argc:         Number of arguments
 * - offsets:      The offsets for each argument where the argv data is located in the chunk
 *                 Warning: Offsets are computed starting from the second u64 !
 * - argv strings: All argv strings one after the other separated by \0;
 *
 * @param argc Number of user-level arguments to pack in a DB
 * @param argv The actual arguments
 */
static void * packUserArguments(int argc, char ** argv) {
    // Prepare arguments for the mainEdt
    ASSERT(argc < 64); // For now
    u32 i;
    u64* offsets = (u64*) runtimeChunkAlloc(sizeof(u64)*argc, ARGS_CHUNK);
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
    // Create a memory chunk containing the parameters
    u64 extraOffset = (maxArg + 1)*sizeof(u64);
    void* ptr = (void *) runtimeChunkAlloc(totalLength + sizeof(u64) + extraOffset, ARGS_CHUNK);

    // Copy in the values to the ptr. The format is as follows:
    // - First 4 bytes encode the size of the packed arguments
    //   (stripped out before passing the packed args to the mainEdt)
    // - Next 4 bytes encode the number of arguments (u64) (called argc)
    // - After that, an array of argc u64 offsets is encoded.
    // - The strings are then placed after that at the offsets encoded
    //
    // The use case is therefore as follows:
    // - (The first 4 bytes are stripped before being handed over to the mainEdt)
    // - Cast the DB to a u64* and read the number of arguments and
    //   offsets (or whatever offset you need)
    // - Cast the DB to a char* and access the char* at the offset
    //   read. This will be a null terminated string.

    // Copy the metadata
    u64* dbAsU64 = (u64*)ptr;
    dbAsU64[0] = totalLength + extraOffset; // do not account for the first element
    dbAsU64[1] = (u64)maxArg;
    for(i = 2; i < maxArg+2; ++i) {
        dbAsU64[i] = offsets[i-2] + extraOffset;
    }

    // Copy the actual arguments, skipping over the first totalLength element
    char* dbAsChar = (char*) ((u64)ptr+sizeof(u64));
    while(argsUsed) {
        u32 pos = fls64(argsUsed);
        argsUsed &= ~(1ULL<<pos);
        strcpy(dbAsChar + extraOffset + offsets[63 - pos], argv[63 - pos]);
    }

    runtimeChunkFree((u64) offsets, NONPERSISTENT_CHUNK);
    return ptr;
}

// This main function is used for x86 platforms
int __attribute__ ((weak)) main(int argc, const char* argv[]) {
    apex_init("main");
    // Parse parameters. The idea is to extract the ones relevant
    // to the runtime and pass all the other ones down to the mainEdt
    ocrConfig_t ocrConfig;
    ocrConfig.userArgc = argc;
    ocrConfig.userArgv = (char **) argv;

    ocrPolicyDomain_t *pd = NULL;

    // Things that must initialize before OCR is started
    platformSpecificInit(&ocrConfig);

    ocrParseArgs(argc, argv, &ocrConfig);

    // Register pointer to the mainEdt
    mainEdtSet(mainEdt);

    // Pack and save user args for the mainEdt
    void * packedUserArgv = packUserArguments(ocrConfig.userArgc, ocrConfig.userArgv);
    userArgsSet(packedUserArgv);

    // Set up the runtime
    bringUpRuntime(&ocrConfig);

    // Here, we are in COMPUTE_OK. We just need to transition to USER_OK
    // which will start mainEdt
    getCurrentEnv(&pd, NULL, NULL, NULL);
    RESULT_ASSERT(
        pd->fcts.switchRunlevel(pd, RL_USER_OK, RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
        ==, 0);

    u8 returnCode = 0;

    freeUpRuntime(true, &returnCode);

    // Warning: Finalizer specific to platforms may call exit
    platformSpecificFinalizer(returnCode);

    apex_finalize();
    return (int)returnCode;
}

#endif
