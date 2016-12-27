/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#include "debug.h"
#ifdef SAL_LINUX

#define DEBUG_TYPE SAL
#include "ocr-types.h"
#include "ocr-errors.h"

//Including platform specific headers for fault injection
#ifdef ENABLE_RESILIENCY
#include "policy-domain/hc/hc-policy.h"
#include "task/hc/hc-task.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#if defined(linux)
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */
#endif

#ifdef __MACH__

#include <mach/mach_time.h>
static double conversion_factor;
static int getTimeNs_initialized = 0;

u64 salGetTime(){
    double timeStamp;
    if(!getTimeNs_initialized){
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        conversion_factor = (double)timebase.numer / (double)timebase.denom;
        getTimeNs_initialized = 1;
    }
    timeStamp = mach_absolute_time()*conversion_factor;
    return (u64)timeStamp;
}

#else
#include <time.h>

u64 salGetTime(){
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000000UL + ((u64)ts.tv_nsec);

}
#endif /*__MACH__*/

#ifdef ENABLE_EXTENSION_PERF

static u64 perfEventOpen(struct perf_event_attr *hw_event)
{
   u64 ret;

   ret = syscall(__NR_perf_event_open, hw_event, 0, -1, -1, 0);
                                               // pid, cpu, group_perfFd, flags
   return ret;
}

// FIXME: The following should come from config file
s32 counter_type[PERF_HW_MAX] = { PERF_TYPE_HARDWARE, PERF_TYPE_HARDWARE, PERF_TYPE_HARDWARE, PERF_TYPE_RAW };
s32 counter_cfg [PERF_HW_MAX] = { PERF_COUNT_HW_BUS_CYCLES, PERF_COUNT_HW_CACHE_REFERENCES, PERF_COUNT_HW_CACHE_MISSES, 0xf110 };

u64 salPerfInit(salPerfCounter* perfCtr) {
    u32 i;
    u32 retval = 0;

    for(i = 0; i < PERF_HW_MAX; i++) {
        memset(&perfCtr[i].perfAttr, 0, sizeof(perfCtr[i].perfAttr));
        perfCtr[i].perfAttr.type = counter_type[i];
        perfCtr[i].perfAttr.size = sizeof(struct perf_event_attr);
        perfCtr[i].perfAttr.config = counter_cfg[i];
        perfCtr[i].perfAttr.disabled = 1;
        perfCtr[i].perfAttr.exclude_kernel = 1;
        perfCtr[i].perfAttr.exclude_hv = 1;

        perfCtr[i].perfFd = perfEventOpen(&perfCtr[i].perfAttr);
        if (perfCtr[i].perfFd == -1) {
            DPRINTF(DEBUG_LVL_WARN, "Error opening counter 0x%"PRIx64"\n", (u64)perfCtr[i].perfAttr.config);
            retval = OCR_EFAULT;
        }
    }

    return retval;
}

u64 salPerfStart(salPerfCounter* perfCtr) {
    u32 i;
    u32 retval = 0;

    for(i = 0; i < PERF_HW_MAX; i++) {
        retval = ioctl(perfCtr[i].perfFd, PERF_EVENT_IOC_RESET, 0);
        if(retval) DPRINTF(DEBUG_LVL_WARN, "Unable to reset counter %"PRId32"\n", i);
        retval = ioctl(perfCtr[i].perfFd, PERF_EVENT_IOC_ENABLE, 0);
        if(retval) DPRINTF(DEBUG_LVL_WARN, "Unable to enable counter %"PRId32"\n", i);
    }

    return retval;
}

u64 salPerfStop(salPerfCounter* perfCtr) {
    u32 i;
    u32 retval = 0;

    for(i = 0; i < PERF_HW_MAX; i++) {
        retval = ioctl(perfCtr[i].perfFd, PERF_EVENT_IOC_DISABLE, 0);
        if(retval) DPRINTF(DEBUG_LVL_WARN, "Unable to disable counter %"PRId32"\n", i);
        retval = read(perfCtr[i].perfFd, &perfCtr[i].perfVal, sizeof(u64));
        if(retval < 0) DPRINTF(DEBUG_LVL_WARN, "Unable to read counter %"PRId32"\n", i);
    }
    // Convert L1_REFERENCES to L1_HITS by subtracting misses
    perfCtr[PERF_L1_HITS].perfVal -= perfCtr[PERF_L1_MISSES].perfVal;

    return retval;
}

u64 salPerfShutdown(salPerfCounter *perfCtr) {
    u32 i;
    u32 retval = 0;

    for(i = 0; i < PERF_HW_MAX; i++) {
        close(perfCtr[i].perfFd);
    }
    return retval;
}

#endif

#ifdef ENABLE_RESILIENCY

#define FD_CHKPT_INITVAL -1
int fdChkpt = FD_CHKPT_INITVAL;
u64 chkptPhase = 0;

typedef enum {
    YEAR,
    MONTH,
    DAY,
    HOUR,
    MINUTE,
    SECOND,
    NUM_TIME_UNITS
} timeUnits;

u64 salGetCalTime() {
    int i;
    char fname[PATH_MAX];
    fname[0] = '\0';
    time_t t = time(NULL);
    struct tm curTime = *(localtime(&t));
    for (i = 0; i < NUM_TIME_UNITS; i++) {
        int t = 0;
        switch(i) {
        case YEAR:
            t = 1900 + curTime.tm_year;
            break;
        case MONTH:
            t = 1 + curTime.tm_mon;
            break;
        case DAY:
            t = curTime.tm_mday;
            break;
        case HOUR:
            t = curTime.tm_hour;
            break;
        case MINUTE:
            t = curTime.tm_min;
            break;
        case SECOND:
            t = curTime.tm_sec;
            break;
        default:
            break;
        }
        if (t < 10) sprintf(fname, "%s0%d", fname, t);
        else        sprintf(fname, "%s%d", fname, t);
    }
    return strtoul(fname, NULL, 10);
}

static const char* salGetExecutableName() {
    static char *execName = NULL;
    if (execName == NULL) {
        ocrPolicyDomain_t *pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        char filenameBuf[4096];
        u64 filenameBufSize = readlink("/proc/self/exe", filenameBuf, 4096);
        if (filenameBufSize <= 0) {
            fprintf(stderr, "readlink failed\n");
            ASSERT(0);
            return NULL;
        }
        char *filename = pd->fcts.pdMalloc(pd, filenameBufSize + 1);
        strncpy(filename, filenameBuf, filenameBufSize);
        filename[filenameBufSize] = '\0';
        execName = filename;
    }
    return (const char*)execName;
}

static const char* salGetCheckpointSummaryFileName() {
    static char *checkpointSummaryName = NULL;
    if (checkpointSummaryName == NULL) {
        ocrPolicyDomain_t *pd;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        const char* execName = salGetExecutableName();
        char appendStr[] = ".chkpt\0";
        u32 filenamesize = strlen(execName) + strlen(appendStr) + 1;
        char *filename = pd->fcts.pdMalloc(pd, filenamesize + 1);
        int rc = snprintf(filename, filenamesize, "%s%s", execName, appendStr);
        if (rc < 0 || rc >= filenamesize) {
            fprintf(stderr, "snprintf failed: (filename: %s)\n", filename);
            ASSERT(0);
            return NULL;
        }
        filename[filenamesize] = '\0';
        checkpointSummaryName = filename;
    }
    return (const char*)checkpointSummaryName;
}

static const char* salConstructPdCheckpointFileName(u64 calTime, u64 phase, u64 loc) {
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    char filenameBuf[4096];
    const char* execName = salGetExecutableName();
    sprintf(filenameBuf, "%s.%lu.%lu.%lu.chkpt", execName, calTime, phase, loc);
    u64 filenameBufSize = strlen(filenameBuf);
    char *filename = pd->fcts.pdMalloc(pd, filenameBufSize + 1);
    strncpy(filename, filenameBuf, filenameBufSize);
    filename[filenameBufSize] = '\0';
    return (const char*)filename;
}

static u8 salGetCheckpointNameTokens(char *checkpointStr, u64 *time, u64 *phase, u64 *loc) {
    char *tok = strtok(checkpointStr, ".");
    const char* execName = salGetExecutableName();
    if (strcmp(tok, execName) != 0)
        return 1;

    tok = strtok(NULL, ".");
    if (tok == NULL)
        return 1;
    u64 calTime = strtoul(tok, NULL, 10);
    ASSERT(calTime <= salGetCalTime());

    tok = strtok(NULL, ".");
    if (tok == NULL)
        return 1;
    u64 ph = strtoul(tok, NULL, 10);
    ASSERT(ph > 0);

    tok = strtok(NULL, ".");
    if (tok == NULL)
        return 1;
    u64 location = strtoul(tok, NULL, 10);

    *time = calTime;
    *phase = ph;
    *loc = location;
    return 0;
}

//Create a new checkpoint buffer
u8* salCreatePdCheckpoint(char **name, u64 size) {
    ASSERT(name);

    if (fdChkpt >= 0) {
        fprintf(stderr, "Cannot open new checkpoint buffer. Previous buffer has not been closed yet. \n");
        ASSERT(0);
        return NULL;
    }

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomainHc_t *hcPolicy = (ocrPolicyDomainHc_t*)pd;
    const char *filename = salConstructPdCheckpointFileName(hcPolicy->calTime, ++chkptPhase, pd->myLocation);

    int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR );
    if (fd<0) {
        fprintf(stderr, "open failed: (filename: %s)\n", filename);
        ASSERT(0);
        return NULL;
    }

    size = ((size >> 12) + !!(size & 0xFFFULL)) << 12; //Align size to 4096 bytes
    if (fd>=0) {
        int rc = ftruncate(fd, size);
        if (rc) {
            fprintf(stderr, "ftruncate failed: (filename: %s filedesc: %d)\n", filename, fd);
            ASSERT(0);
            return NULL;
        }
    }

    u8 *ptr = (u8*)mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap failed for size %lu (filename: %s filedesc: %d)\n", size, filename, fd);
        ASSERT(0);
        return NULL;
    }

    *name = (char*)filename;
    fdChkpt = fd;
    return ptr;
}

//Open a previously closed stable checkpoint
//salGetCheckpointName() provides the stable checkpoint name
u8* salOpenPdCheckpoint(char *name, u64 *size) {
    if (fdChkpt >= 0) {
        fprintf(stderr, "Cannot open new checkpoint buffer. Previous buffer has not been closed yet. \n");
        ASSERT(0);
        return NULL;
    }

    ASSERT(name);
    const char *filename = name;
    struct stat sb;
    if (stat(filename, &sb) == -1) {
        fprintf(stderr, "stat failed: (filename: %s)\n", filename);
        ASSERT(0);
        return NULL;
    }

    u64 filesize = sb.st_size;
    ASSERT(filesize > 0);

    int fd = open(filename, O_RDWR, S_IRUSR | S_IWUSR );
    if (fd<0) {
        fprintf(stderr, "open failed: (filename: %s)\n", filename);
        ASSERT(0);
        return NULL;
    }

    u8 *ptr = (u8*)mmap( NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap failed for size %lu (filename: %s filedesc: %d)\n", filesize, filename, fd);
        ASSERT(0);
        return NULL;
    }

    if (size)
        *size = filesize;

    fdChkpt = fd;
    return ptr;
}

//Close a previously opened checkpoint buffer
u8 salClosePdCheckpoint(u8 *buffer, u64 size) {
    ASSERT(buffer);
    if (fdChkpt < 0) {
        fprintf(stderr, "Invalid buffer %p. No checkpoint buffer found. \n", buffer);
        ASSERT(0);
        return 1;
    }

    int rc = msync(buffer, size, MS_INVALIDATE | MS_SYNC);
    if (rc) {
        fprintf(stderr, "msync failed for buffer %p of size %lu\n", buffer, size);
        ASSERT(0);
        return 1;
    }

    rc = munmap(buffer, size);
    if (rc) {
        fprintf(stderr, "munmap failed for buffer %p of size %lu\n", buffer, size);
        ASSERT(0);
        return 1;
    }

    rc = close(fdChkpt);
    if (rc) {
        fprintf(stderr, "close failed: (filedesc: %d)\n", fdChkpt);
        ASSERT(0);
        return 1;
    }

    fdChkpt = FD_CHKPT_INITVAL;
    return 0;
}

//Remove a checkpoint
u8 salRemovePdCheckpoint(char *name) {
    if (name == NULL)
        return 0;
    int rc = unlink(name);
    if (rc) {
        fprintf(stderr, "unlink failed: (filename: %s)\n", name);
        ASSERT(0);
        return 1;
    }
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    pd->fcts.pdFree(pd, name);
    return 0;
}

//Mark the checkpoint name as stable.
//This updates the checkpoint summary file
u8 salSetPdCheckpoint(char *name) {
    ASSERT(name != NULL && chkptPhase > 0);
    if (strlen(name) >= 4096) {
        ASSERT(0);
        return 1;
    }

    const char* filename = salGetCheckpointSummaryFileName();
    FILE *fp = fopen(filename, "w");
    if (fp == NULL) {
        fprintf(stderr, "fopen failed: (filename: %s)\n", filename);
        ASSERT(0);
        return 1;
    }

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomainHc_t *hcPolicy = (ocrPolicyDomainHc_t*)pd;
    char checkpointStr[4096];
    strcpy(checkpointStr, name);
    u64 calTime = 0;
    u64 phase = 0;
    u64 loc = 0;
    int rc = salGetCheckpointNameTokens(checkpointStr, &calTime, &phase, &loc);
    if (rc < 0 || calTime != hcPolicy->calTime || phase != chkptPhase || loc != pd->myLocation) {
        fprintf(stderr, "Cannot set checkpoint. Invalid checkpoint name: (filename: %s)\n", name);
        ASSERT(0);
        return 1;
    }

    char filenameBuf[4096];
    const char* execName = salGetExecutableName();
    rc = sprintf(filenameBuf, "%s.%lu.%lu.%u", execName, hcPolicy->calTime, chkptPhase, pd->neighborCount + 1);
    if (rc < 0 || rc >= 4096) {
        fprintf(stderr, "sprintf failed: (filename: %s)\n", filename);
        ASSERT(0);
        return 1;
    }

    rc = fputs(filenameBuf, fp);
    if (rc == EOF || rc < 0) {
        fprintf(stderr, "fputs failed: (filename: %s)\n", filename);
        ASSERT(0);
        return 1;
    }

    rc = fclose(fp);
    if (rc != 0) {
        fprintf(stderr, "fclose failed: (filename: %s)\n", filename);
        ASSERT(0);
        return 1;
    }
    return 0;
}

//Get the name of the current stable checkpoint
char* salGetCheckpointName() {
    struct stat sb;
    const char* filename = salGetCheckpointSummaryFileName();
    if (stat(filename, &sb) == -1)
        return NULL;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr, "fopen failed: (filename: %s)\n", filename);
        ASSERT(0);
        return NULL;
    }

    char checkpointStr[4096];
    if (fgets(checkpointStr, 4096, fp) == NULL) {
        fprintf(stderr, "fgets failed: (filename: %s)\n", filename);
        ASSERT(0);
        return NULL;
    }

    u64 calTime = 0;
    u64 phase = 0;
    u64 numLocations = 0;
    int rc = salGetCheckpointNameTokens(checkpointStr, &calTime, &phase, &numLocations);
    if (rc < 0)
        return NULL;

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if (numLocations != (pd->neighborCount + 1))
        return NULL;

    const char *chkptFilename = salConstructPdCheckpointFileName(calTime, phase, pd->myLocation);
    return (char*)chkptFilename;
}

//Check if any valid checkpoint exists
static bool salCheckpointExistsInternal(bool doQuery) {
    struct stat sb;
    const char* filename = salGetCheckpointSummaryFileName();
    if (stat(filename, &sb) == -1)
        return false;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr, "fopen failed: (filename: %s)\n", filename);
        ASSERT(0);
        return false;
    }

    char checkpointStr[4096];
    if (fgets(checkpointStr, 4096, fp) == NULL) {
        fprintf(stderr, "fgets failed: (filename: %s)\n", filename);
        ASSERT(0);
        return false;
    }

    u64 calTime = 0;
    u64 phase = 0;
    u64 numLocations = 0;
    int rc = salGetCheckpointNameTokens(checkpointStr, &calTime, &phase, &numLocations);
    if (rc < 0)
        return false;

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if (numLocations != (pd->neighborCount + 1))
        return false;

    u32 i;
    for (i = 0; i < numLocations; i++) {
        char *chkptFilename = (char*)salConstructPdCheckpointFileName(calTime, phase, i);
        if (stat(chkptFilename, &sb) == -1) {
            pd->fcts.pdFree(pd, chkptFilename);
            return false;
        }
        pd->fcts.pdFree(pd, chkptFilename);
    }

    if (doQuery) {
        char ans;
        printf("Found existing checkpoint: (timestamp: %lu phase: %lu) \nDo you want to resume? [Y/N] : ", calTime, phase);
        scanf ("%c", &ans);
        if (ans != 'Y' && ans != 'y')
            return false;
        fprintf(stderr, "Resuming from checkpoint: (timestamp: %lu phase: %lu)\n", calTime, phase);
    }
    return true;
}

//Check if any valid checkpoint exists
bool salCheckpointExists() {
    return salCheckpointExistsInternal(false);
}

//Check if any valid checkpoint exists
//If yes, then ask user if program should
//be resumed from that checkpoint
bool salCheckpointExistsResumeQuery() {
    return salCheckpointExistsInternal(true);
}

#endif

#ifdef ENABLE_EXTENSION_PAUSE

#include <signal.h>
#include "utils/pqr-utils.h"

/* NOTE: Below is an optional interface allowing users to
 *       send SIGUSR1 and SIGUSR2 to control pause/query/resume
 *       during execution.  By default the signaled pause command
 *       will block until it succeeds uncontended.
 *
 * SIGUSR1: Toggles Pause and Resume
 * SIGUSR2: Query the contents of queued tasks (will only
 *          succeed if runtime is paused)
 *
 *      This was implemented as a segway into actual API calls and
 *      may be deprecated in the future.
 */
void sig_handler(u32 sigNum) {

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomainHc_t *globalPD = (ocrPolicyDomainHc_t *) pd;


    if(sigNum == SIGUSR1 && globalPD->pqrFlags.runtimePause == false){
        DPRINTF(DEBUG_LVL_WARN, "Pausing Runtime\n");
        salPause(true);
        return;
    }

    if(sigNum == SIGUSR1 && globalPD->pqrFlags.runtimePause == true){
        DPRINTF(DEBUG_LVL_WARN, "Resuming Runtime\n");
        salResume(1);
    }

    if(sigNum == SIGUSR2){
        DPRINTF(DEBUG_LVL_WARN, "Begin fault injection\n");
        salInjectFault();
        return;
    }
}

u32 salPause(bool isBlocking){

    ocrPolicyDomain_t *pd;
    ocrWorker_t *baseWorker;
    getCurrentEnv(&pd, &baseWorker, NULL, NULL);
    ocrPolicyDomainHc_t *self = (ocrPolicyDomainHc_t *) pd;

    while(hal_cmpswap32((u32*)&self->pqrFlags.runtimePause, false, true) == true) {
        // Already paused - try to pause self only if blocked
        if(isBlocking == false)
            return 0;
        // Blocking pause
        if(self->pqrFlags.runtimePause == true) {
            hal_xadd32((u32*)&self->pqrFlags.pauseCounter, 1);
            //Pause called - stop workers
            while(self->pqrFlags.runtimePause == true) {
                hal_pause();
            }
            hal_xadd32((u32*)&self->pqrFlags.pauseCounter, -1);
        }
    }

    hal_xadd32((u32*)&self->pqrFlags.pauseCounter, 1);

    u32 compWorkerCount;
    if(pd->workers[(pd->workerCount)-1]->type == SYSTEM_WORKERTYPE)
        compWorkerCount = (pd->workerCount)-1;
    else
        compWorkerCount = (pd->workerCount);
    while(self->pqrFlags.pauseCounter < compWorkerCount){
        hal_pause();
    }

    return 1;
}

ocrGuid_t salQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags){

    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomainHc_t *self = (ocrPolicyDomainHc_t *) pd;
    ocrGuid_t dataDb = NULL_GUID;

    if(self->pqrFlags.runtimePause == false)
        return NULL_GUID;

    switch(query){
        case OCR_QUERY_READY_EDTS:
            dataDb = hcQueryNextEdts(self, result, size);
            *size = (*size)*sizeof(ocrGuid_t);
            break;

        case OCR_QUERY_EVENTS:
            break;

        case OCR_QUERY_LAST_SATISFIED_DB:
            dataDb = hcQueryPreviousDatablock(self, result, size);
            *size = (*size)*(sizeof(ocrGuid_t));
            break;

        case OCR_QUERY_ALL_EDTS:
            dataDb = hcQueryAllEdts(self, result, size);
            *size = (*size)*(sizeof(ocrGuid_t));
            break;

        default:
            break;
    }

    return dataDb;
}

void salResume(u32 flag){
    ocrPolicyDomain_t *pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPolicyDomainHc_t *self = (ocrPolicyDomainHc_t *) pd;

    if(hal_cmpswap32((u32*)&self->pqrFlags.runtimePause, true, false) == true)
        hal_xadd32((u32*)&self->pqrFlags.pauseCounter, -1);

    return;
}

void salInjectFault(void) {
#ifdef ENABLE_RESILIENCY
    u32 i, j;
    ocrPolicyDomain_t *pd;
    ocrTask_t *task;
    PD_MSG_STACK(msg)
    getCurrentEnv(&pd, NULL, &task, &msg);
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)pd;

    bool faultInjected = false;
    while(!faultInjected) {
        if (rself->shutdownInProgress != 0) {
            DPRINTF(DEBUG_LVL_WARN, "Unable to inject fault - shutdown in progress\n");
            return;
        }

        if (rself->fault != 0) {
            DPRINTF(DEBUG_LVL_WARN, "Unable to inject fault - previous fault recovery pending\n");
            return;
        }

        for (i = 0; i < pd->workerCount && !faultInjected; i++) {
            ocrWorker_t *worker = pd->workers[i];
            hal_lock(&worker->notifyLock);
            if (worker->activeDepv != NULL) {
                ocrEdtDep_t *depv = (ocrEdtDep_t*)worker->activeDepv;
                u32 depc = task->depc;
                for (j = 0; j < depc; j++) {
                    ocrGuid_t corruptDb = depv[j].guid;
                    if(!ocrGuidIsNull(corruptDb)){
                        ocrFaultArgs_t faultArgs;
                        faultArgs.kind = OCR_FAULT_DATABLOCK_CORRUPTION;
                        faultArgs.OCR_FAULT_ARG_FIELD(OCR_FAULT_DATABLOCK_CORRUPTION).db.guid = corruptDb;
                        faultArgs.OCR_FAULT_ARG_FIELD(OCR_FAULT_DATABLOCK_CORRUPTION).db.metaDataPtr = NULL;
                    #define PD_MSG (&msg)
                    #define PD_TYPE PD_MSG_RESILIENCY_NOTIFY
                        msg.type = PD_MSG_RESILIENCY_NOTIFY | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                        PD_MSG_FIELD_I(properties) = 0;
                        PD_MSG_FIELD_I(faultArgs) = faultArgs;
                        u8 returnCode __attribute__((unused)) = pd->fcts.processMessage(pd, &msg, true);
                        if (PD_MSG_FIELD_O(returnDetail) == 0) {
                            DPRINTF(DEBUG_LVL_WARN, "Corrupting datablock: "GUIDF" by changing a value to 0xff...f\n", GUIDA(corruptDb));
                            faultInjected = true;
                            break;
                        } else {
                            DPRINTF(DEBUG_LVL_INFO, "Unable to inject fault - resiliency manager notify failed\n");
                        }
                    #undef PD_MSG
                    #undef PD_TYPE
                    }
                }
            }
            hal_unlock(&worker->notifyLock);
        }
    }
#endif
}
void registerSignalHandler(){

    struct sigaction action;
    action.sa_handler = ((void (*)(int))&sig_handler);
    action.sa_flags = SA_RESTART;
    sigfillset(&action.sa_mask);
    if(sigaction(SIGUSR1, &action, NULL) != 0) {
        DPRINTF(DEBUG_LVL_WARN, "Couldn't catch SIGUSR1...\n");
    }
     if(sigaction(SIGUSR2, &action, NULL) != 0) {
        DPRINTF(DEBUG_LVL_WARN, "Couldn't catch SIGUSR2...\n");
    }
}

#else
void sig_handler(u32 sigNum){
    return;
}

u32 salPause(bool isBlocking){
    DPRINTF(DEBUG_LVL_WARN, "PQR unsupported on this platform\n");
    return 0;
}

ocrGuid_t salQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags){
    DPRINTF(DEBUG_LVL_WARN, "PQR unsupported on this platform\n");
    return NULL_GUID;
}

void salResume(u32 flag){
    DPRINTF(DEBUG_LVL_WARN, "PQR unsupported on this platform\n");
}

void salInjectFault(void){
    DPRINTF(DEBUG_LVL_WARN, "ENABLE_EXTENSION_PAUSE currently undefined in ocr-config.h\n");
}

void registerSignalHandler(){
    return;
}


#endif /* ENABLE_EXTENSION_PAUSE  */
#endif
