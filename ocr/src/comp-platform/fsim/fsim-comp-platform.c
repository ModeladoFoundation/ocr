/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMP_PLATFORM_FSIM

#include "debug.h"

#include "ocr-policy-domain.h"

#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"

#include "fsim-comp-platform.h"

#define DEBUG_TYPE COMP_PLATFORM


#if defined(SAL_FSIM_XE)
//
// Apparently, the compiler needs memcpy() as a proper function and
// cannot do without it for portable code... Hence, we need to define
// it here for XE LLVM, else we get undefined references.
//
// It's a tool-chain thing, not really hardware, system, or platform,
// but we have to stick it somewhere and the HAL and SAL are headers
// only -- hence this placement.
//
int memcpy(void * dst, void * src, u64 len) {
    __asm__ __volatile__("dma.copyregion %1, %0, %2\n\t"
                         "fence 0xF\n\t"
                         : : "r" (dst), "r" (src), "r" (len));
    return len;
}
#endif


// Ugly globals, but similar globals exist in pthread as well
// FIXME: These globals need to be moved out into their own registers

ocrPolicyDomain_t *myPD = NULL;
ocrWorker_t *myWorker = NULL;

static void * fsimRoutineExecute(ocrWorker_t * worker) {
    return worker->fcts.run(worker);
}

void fsimCompDestruct (ocrCompPlatform_t * base) {
    runtimeChunkFree((u64)base, NULL);
}

u8 fsimCompSwitchRunlevel(ocrCompPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         u32 phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // The worker is the capable module and we operate as
    // inert wrt it
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    ocrCompPlatformFsim_t *pthreadCompPlatform = (ocrCompPlatformFsim_t*)self;
    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_CONFIG_PARSE, phase)) {
            ASSERT(self->worker != NULL);
        }
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            self->pd = PD;
            if(properties & RL_PD_MASTER) {
                // We need to make sure we have the current environment set
                // at least partially as the PD may be used
                self->fcts.setCurrentEnv(self, self->pd, NULL);
            }
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
#if 0
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            if(properties & RL_PD_MASTER) {
                // We do not need to create another thread
                // Only do the binding
                s32 cpuBind = pthreadCompPlatform->binding;
                if(cpuBind != -1) {
                    DPRINTF(DEBUG_LVL_INFO, "Binding comp-platform to cpu_id %d\n", cpuBind);
                    bindThread(cpuBind);
                }
#ifdef OCR_RUNTIME_PROFILER
                {
                    _profilerData *d = (_profilerData*)malloc(sizeof(_profilerData));
                    char buffer[50];
                    snprintf(buffer, 50, "profiler_%lx-%lx", PD->myLocation, 0UL);
                    d->output = fopen(buffer, "w");
                    ASSERT(d->output);
                    RESULT_ASSERT(pthread_setspecific(_profilerThreadData, d), ==, 0);
                }
#endif
            } else {
                // We need to create another capable module
                ocrCompPlatformPthread_t * pthreadCompPlatform = (ocrCompPlatformPthread_t *)self;
                pthread_attr_t attr;
                toReturn |= pthread_attr_init(&attr);
                //Note this call may fail if the system doesn't like the stack size asked for.
                if(!toReturn)
                    toReturn |= pthread_attr_setstacksize(&attr, pthreadCompPlatform->stackSize);
                if(!toReturn) {
                    toReturn |= pthread_create(&(pthreadCompPlatform->osThread),
                                               &attr, &pthreadRoutineWrapper,
                                               pthreadCompPlatform);
                }
            }
        } else if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
            // At this point, this is run only by the master thread
            if(!(properties & RL_PD_MASTER)) {
                // We do not join with ourself
                toReturn |= pthread_join(pthreadCompPlatform->osThread, NULL);
            }
        }
#endif
        break;
    case RL_USER_OK:
        break;
    default:
        ASSERT(0);
    }
    return toReturn;
}

void fsimCompBegin(ocrCompPlatform_t * compPlatform, ocrPolicyDomain_t * PD, ocrWorkerType_t workerType) {
    compPlatform->pd = PD;
}

void fsimCompStart(ocrCompPlatform_t * compPlatform, ocrPolicyDomain_t * PD, ocrWorker_t * worker) {
    compPlatform->worker = worker;
    fsimRoutineExecute(worker);
}

void fsimCompStop(ocrCompPlatform_t * compPlatform, ocrRunlevel_t newRl, u32 action) {
    // Nothing to do really
}

u8 fsimCompGetThrottle(ocrCompPlatform_t *self, u64* value) {
    return 1;
}

u8 fsimCompSetThrottle(ocrCompPlatform_t *self, u64 value) {
    return 1;
}

u8 fsimCompSetCurrentEnv(ocrCompPlatform_t *self, ocrPolicyDomain_t *pd,
                         ocrWorker_t *worker) {

    myPD = pd;
    myWorker = worker;
    return 0;
}

/******************************************************/
/* OCR COMP PLATFORM FSIM FACTORY                  */
/******************************************************/

ocrCompPlatform_t* newCompPlatformFsim(ocrCompPlatformFactory_t *factory,
                                       ocrParamList_t *perInstance) {

    ocrCompPlatformFsim_t * compPlatformFsim = (ocrCompPlatformFsim_t*)
            runtimeChunkAlloc(sizeof(ocrCompPlatformFsim_t), PERSISTENT_CHUNK);
    ocrCompPlatform_t *base = (ocrCompPlatform_t*)compPlatformFsim;
    factory->initialize(factory, base, perInstance);
    return base;
}

void initializeCompPlatformFsim(ocrCompPlatformFactory_t * factory, ocrCompPlatform_t *base, ocrParamList_t *perInstance) {
    initializeCompPlatformOcr(factory, base, perInstance);
}

void destructCompPlatformFactoryFsim(ocrCompPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NULL);
}

void getCurrentEnv(ocrPolicyDomain_t** pd, ocrWorker_t** worker,
                   ocrTask_t **task, ocrPolicyMsg_t* msg) {
    if(pd)
        *pd = myPD;
    if(worker)
        *worker = myWorker;
    if(myWorker && task)
        *task = myWorker->curTask;
    if(msg) {
        msg->srcLocation = myPD->myLocation;
        msg->usefulSize = 0;
    }
}

ocrCompPlatformFactory_t *newCompPlatformFactoryFsim(ocrParamList_t *perType) {
    ocrCompPlatformFactory_t *base = (ocrCompPlatformFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrCompPlatformFactoryFsim_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newCompPlatformFsim;
    base->initialize = &initializeCompPlatformFsim;
    base->destruct = &destructCompPlatformFactoryFsim;
    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCompPlatform_t*), fsimCompDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         u32, u32, void (*)(ocrPolicyDomain_t*, u64), u64), fsimCompSwitchRunlevel);
    base->platformFcts.getThrottle = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, u64*), fsimCompGetThrottle);
    base->platformFcts.setThrottle = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, u64), fsimCompSetThrottle);
    base->platformFcts.setCurrentEnv = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, ocrPolicyDomain_t*, ocrWorker_t*), fsimCompSetCurrentEnv);

    return base;
}
#endif /* ENABLE_COMP_PLATFORM_FSIM */
