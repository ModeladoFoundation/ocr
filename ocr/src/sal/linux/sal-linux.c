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


#ifdef ENABLE_EXTENSION_PAUSE

#include <signal.h>
#include "utils/pqr-utils.h"

//Including platform specific headers for fault injection
#ifdef ENABLE_RESILIENCY
#include "policy-domain/hc/hc-policy.h"
#include "task/hc/hc-task.h"
#endif

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
