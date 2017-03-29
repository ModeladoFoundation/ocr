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

#if !defined(ENABLE_NEWLIB_SCAFFOLD_TG)
int memcpy(void * dst, void * src, u64 len) {
    hal_memCopy(dst, src, len, 0);
    return len;
}
#endif
#endif


// Ugly globals, but similar globals exist in pthread as well
ocrPolicyDomain_t *myPD = NULL;
#if defined(SAL_FSIM_XE) && defined(OCR_SHARED_XE_POLICY_DOMAIN)
// This must be local because it should point (BR) to a different worker for each XE.
#include <xstgintrin.h>
ocrWorker_t *myWorker xstglocal = NULL;
#else
ocrWorker_t *myWorker = NULL;
#endif

static void * fsimRoutineExecute(ocrWorker_t * worker) {
    // This is actually started directly in the worker on fsim
    return NULL;
}

void fsimCompDestruct (ocrCompPlatform_t * base) {
    runtimeChunkFree((u64)base, NULL);
}

u8 fsimCompSwitchRunlevel(ocrCompPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // The worker is the capable module and we operate as
    // inert wrt it
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));
    // FIXME: This is not true for XEs ASSERT((properties & RL_NODE_MASTER) == RL_NODE_MASTER);

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
            self->pd = PD;
            self->fcts.setCurrentEnv(self, self->pd, NULL);
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            self->fcts.setCurrentEnv(self, self->pd, self->worker);
            fsimRoutineExecute(self->worker);
        }
        break;
    case RL_USER_OK:
        break;
    default:
        ASSERT(0);
    }
    return toReturn;
}

u8 fsimCompGetThrottle(ocrCompPlatform_t *self, u64* value) {
    return 1;
}

u8 fsimCompSetThrottle(ocrCompPlatform_t *self, u64 value) {
    return 1;
}

u8 fsimCompSetCurrentEnv(ocrCompPlatform_t *self, ocrPolicyDomain_t *pd,
                         ocrWorker_t *worker) {

    if(pd) myPD = pd;
    if(worker) myWorker = worker;
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
        msg->destLocation = msg->srcLocation;
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
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), fsimCompSwitchRunlevel);
    base->platformFcts.getThrottle = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, u64*), fsimCompGetThrottle);
    base->platformFcts.setThrottle = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, u64), fsimCompSetThrottle);
    base->platformFcts.setCurrentEnv = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, ocrPolicyDomain_t*, ocrWorker_t*), fsimCompSetCurrentEnv);

    return base;
}
#endif /* ENABLE_COMP_PLATFORM_FSIM */
