/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMP_PLATFORM_PTHREAD

#include "debug.h"

#include "ocr-policy-domain.h"

#include "ocr-sysboot.h"
#include "utils/ocr-utils.h"
#include "ocr-worker.h"

#include "pthread-comp-platform.h"

#include "utils/profiler/profiler-internal.h"

#define DEBUG_TYPE COMP_PLATFORM

extern void bindThread(u32 mask);

//TODO: I did a number of changes/cleanup here in another patchset. I'll try to push that soon

/**
 * The key we use to be able to find which compPlatform we are on
 */
static pthread_key_t selfKey;
static bool selfKeyInit = false;

#ifdef OCR_RUNTIME_PROFILER
pthread_key_t _profilerThreadData;
#endif

static pthread_once_t selfKeyInitialized = PTHREAD_ONCE_INIT;

static void * pthreadRoutineExecute(ocrWorker_t * worker) {
    return worker->fcts.run(worker);
}

static void pthreadRoutineInitializer(ocrCompPlatformPthread_t * pthreadCompPlatform) {
    s32 cpuBind = pthreadCompPlatform->binding;
    if(cpuBind != -1) {
        DPRINTF(DEBUG_LVL_INFO, "Binding comp-platform to cpu_id %"PRId32"\n", cpuBind);
        bindThread(cpuBind);
    }
#ifdef OCR_RUNTIME_PROFILER
    {
        _profilerData *d = NULL;
        if(pthreadCompPlatform->doProfile) {
            d = (_profilerData*) runtimeChunkAlloc(sizeof(_profilerData), PERSISTENT_CHUNK);
            _profilerDataInit(d);
            char buffer[50];
            snprintf(buffer, 50, "profiler_%"PRIx64"-%"PRIx64"",
                     ((ocrPolicyDomain_t *)(pthreadCompPlatform->base.pd))->myLocation, (u64)pthreadCompPlatform);
            d->output = fopen(buffer, "w");
            ASSERT(d->output);
        }
        RESULT_ASSERT(pthread_setspecific(_profilerThreadData, d), ==, 0);
    }
#endif
}

/*
 * Wrapper routine to allow initialization of local storage
 * before entering the worker routine.
 */
static void * pthreadRoutineWrapper(void * arg) {
    // Only called on slave workers (never master)
    ocrCompPlatformPthread_t * pthreadCompPlatform = (ocrCompPlatformPthread_t *) arg;
    pthreadRoutineInitializer(pthreadCompPlatform);
    // Real initialization happens in workers's run routine
    RESULT_ASSERT(pthread_setspecific(selfKey, &(pthreadCompPlatform->tls)), ==, 0);

    // Depending on whether we are a node master or a PD master or just a worker
    // we do different things
    switch(pthreadCompPlatform->threadStatus) {
    case 0:
        // regular worker
        return pthreadRoutineExecute(pthreadCompPlatform->base.worker);
    case RL_PD_MASTER:
    {
        // Since we do not start the worker right away, we need to at least
        // partially initialize the environment
        pthreadCompPlatform->base.fcts.setCurrentEnv(
            (ocrCompPlatform_t*)pthreadCompPlatform,
            ((ocrCompPlatform_t*)pthreadCompPlatform)->pd, NULL);

        // We need to continue the startup sequence
        ocrPolicyDomain_t *policy = pthreadCompPlatform->base.pd;
        RESULT_ASSERT(policy->fcts.switchRunlevel(
                          policy, RL_MEMORY_OK, RL_REQUEST |
                          RL_ASYNC | RL_BRING_UP | RL_PD_MASTER), ==, 0);
        RESULT_ASSERT(policy->fcts.switchRunlevel(
                          policy, RL_GUID_OK, RL_REQUEST | RL_BARRIER |
                          RL_BRING_UP | RL_PD_MASTER), ==, 0);
        RESULT_ASSERT(policy->fcts.switchRunlevel(
                          policy, RL_COMPUTE_OK, RL_REQUEST | RL_ASYNC |
                          RL_BRING_UP | RL_PD_MASTER), ==, 0);
        RESULT_ASSERT(policy->fcts.switchRunlevel(
                          policy, RL_USER_OK, RL_REQUEST | RL_ASYNC |
                          RL_BRING_UP | RL_PD_MASTER), ==, 0);
        // At this point, we actually start doing work
        // When we come back down here, we need to continue the transition
        // (RL_GUID_OK and RL_PD_OK). After that, we return and the NODE_MASTER will
        // continue to transition us
        RESULT_ASSERT(policy->fcts.switchRunlevel(
                          policy, RL_GUID_OK, RL_REQUEST | RL_BARRIER |
                          RL_TEAR_DOWN | RL_PD_MASTER), ==, 0);
        RESULT_ASSERT(policy->fcts.switchRunlevel(
                          policy, RL_PD_OK, RL_REQUEST | RL_ASYNC |
                          RL_TEAR_DOWN | RL_PD_MASTER), ==, 0);
        break;
    }
    case RL_NODE_MASTER:
    {
        // This should never happen. The NODE master thread should always
        // exist and never enter pthreadRoutineWrapper
        ASSERT(0);
        break;
    }
    default:
        ;
    }
    return NULL;
}

/**
 * Called once by the master thread before others pthread are started.
 */
static void initializeKey() {
    RESULT_ASSERT(pthread_key_create(&selfKey, NULL), ==, 0);
#ifdef OCR_RUNTIME_PROFILER
    RESULT_ASSERT(pthread_key_create(&_profilerThreadData, &_profilerDataDestroy), ==, 0);
#endif
}

void pthreadDestruct (ocrCompPlatform_t * base) {
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
}

u8 pthreadSwitchRunlevel(ocrCompPlatform_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // The worker is the capable module and we operate as
    // inert wrt it
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    ocrCompPlatformPthread_t *pthreadCompPlatform = (ocrCompPlatformPthread_t*)self;
    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_CONFIG_PARSE, phase)) {
            if((properties & RL_NODE_MASTER) == RL_NODE_MASTER) {
                DPRINTF(DEBUG_LVL_VVERB, "I am RL_NODE_MASTER\n");
                pthreadCompPlatform->threadStatus = RL_NODE_MASTER;
            } else if(properties & RL_PD_MASTER) {
                pthreadCompPlatform->threadStatus = RL_PD_MASTER;
            } else {
                pthreadCompPlatform->threadStatus = 0;
            }
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_CONFIG_PARSE, phase)) {
            perThreadStorage_t *tls = pthread_getspecific(selfKey);
            // This code is called by the master thread once per comp-platform
            if (tls != NULL) {
                // This is necessary for legacy mode support so that the next time
                // we start the runtime we do not reuse the current thread old TLS.
                pthread_setspecific(selfKey, NULL);
            }
        }

        break;
    case RL_NETWORK_OK:
        // This is run only by NODE_MASTER and we need to join with all the other PD_MASTERs
        // created in the PD_OK stage
        if((properties & RL_TEAR_DOWN) && (RL_IS_FIRST_PHASE_DOWN(PD, RL_NETWORK_OK, phase))) {
            if(pthreadCompPlatform->threadStatus == RL_PD_MASTER) {
                // We don't join with ourself
                toReturn |= pthread_join(pthreadCompPlatform->osThread, NULL);
            }
        }
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            self->pd = PD;
            if(((properties & RL_NODE_MASTER) == RL_NODE_MASTER) &&
               RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
                // This means that we are the node master and therefore do not
                // need to start another thread. Instead, we set the current environment
                // for ourself
                ASSERT(pthread_getspecific(selfKey) == NULL); // The key has not been setup yet
                RESULT_ASSERT(pthread_setspecific(selfKey, &pthreadCompPlatform->tls), ==, 0);
                self->fcts.setCurrentEnv(self, self->pd, NULL);
            } else if(properties & RL_PD_MASTER) {
                // Excludes NODE_MASTER since that is caught in the first part of this if statement
                // We need to bring up the PD_MASTER thread for the other PDs and
                // they will then continue the switch. Note that we create the thread
                // only on the last phase
                if(RL_IS_LAST_PHASE_UP(PD, RL_PD_OK, phase)) {
                    DPRINTF(DEBUG_LVL_INFO, "Creating PD_MASTER thread\n");
                    pthread_attr_t attr;
                    toReturn |= pthread_attr_init(&attr);
                    if(!toReturn) {
                        toReturn |= pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
                        toReturn |= pthread_attr_setstacksize(&attr, pthreadCompPlatform->stackSize);
                    }
                    if(!toReturn) {
                        toReturn |= pthread_create(&(pthreadCompPlatform->osThread), &attr,
                                                   &pthreadRoutineWrapper, pthreadCompPlatform);
                    }
                }
            }
        }
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
            ocrCompPlatformPthread_t * pthreadCompPlatform = (ocrCompPlatformPthread_t *)self;
            if(properties & RL_PD_MASTER) {
                // For both PD_MASTER and NODE_MASTER
                pthreadRoutineInitializer(pthreadCompPlatform);
            } else {
                // We need to create another capable module
                pthread_attr_t attr;
                toReturn |= pthread_attr_init(&attr);
                //Note this call may fail if the system doesn't like the stack size asked for.
                if(!toReturn) {
                    toReturn |= pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
                    toReturn |= pthread_attr_setstacksize(&attr, pthreadCompPlatform->stackSize);
                }
                if(!toReturn) {
                    toReturn |= pthread_create(&(pthreadCompPlatform->osThread),
                                               &attr, &pthreadRoutineWrapper,
                                               pthreadCompPlatform);
                }
            }
        } else if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
            // At this point, this is run only by the master thread
            if(!(properties & RL_PD_MASTER)) {
                // We do not join with ourself; covers both PD_MASTER and NODE_MASTER
                toReturn |= pthread_join(pthreadCompPlatform->osThread, NULL);
            } else {
#ifdef OCR_RUNTIME_PROFILER
                // We also destroy the profile data here for the master thread
                _profilerData *pData = pthread_getspecific(_profilerThreadData);
                if(pData)
                    _profilerDataDestroy(pData);
#endif
            }
#ifdef OCR_RUNTIME_PROFILER
            {
                //BUG #527 missing OCR_RUNTIME_PROFILER deallocation
            }
#endif
        }
        break;
    case RL_USER_OK:
        break;
    default:
        ASSERT(0);
    }
    return toReturn;
}

u8 pthreadGetThrottle(ocrCompPlatform_t *self, u64* value) {
    return 1;
}

u8 pthreadSetThrottle(ocrCompPlatform_t *self, u64 value) {
    return 1;
}

u8 pthreadSetCurrentEnv(ocrCompPlatform_t *self, ocrPolicyDomain_t *pd,
                        ocrWorker_t *worker) {

    ASSERT(ocrGuidIsEq(pd->fguid.guid, self->pd->fguid.guid));
    perThreadStorage_t *tls = pthread_getspecific(selfKey);
    tls->pd = pd;
    tls->worker = worker;
    return 0;
}

ocrCompPlatform_t* newCompPlatformPthread(ocrCompPlatformFactory_t *factory,
        ocrParamList_t *perInstance) {
    // initializeKey is called once and it's always the master thread since
    // it is the thread bringing up the runtime.
    pthread_once(&selfKeyInitialized,  initializeKey);
    selfKeyInit = true;
    ocrCompPlatformPthread_t * compPlatformPthread = (ocrCompPlatformPthread_t*)
            runtimeChunkAlloc(sizeof(ocrCompPlatformPthread_t), PERSISTENT_CHUNK);

    ocrCompPlatform_t * derived = (ocrCompPlatform_t *) compPlatformPthread;
    factory->initialize(factory, derived, perInstance);
    return derived;
}

void initializeCompPlatformPthread(ocrCompPlatformFactory_t * factory, ocrCompPlatform_t * derived, ocrParamList_t * perInstance) {
    initializeCompPlatformOcr(factory, derived, perInstance);
    paramListCompPlatformPthread_t * params =
        (paramListCompPlatformPthread_t *) perInstance;

    ocrCompPlatformPthread_t *compPlatformPthread = (ocrCompPlatformPthread_t *)derived;
    compPlatformPthread->base.fcts = factory->platformFcts;
    compPlatformPthread->binding = (params != NULL) ? params->binding : -1;
    compPlatformPthread->stackSize = ((params != NULL) && (params->stackSize > 0)) ? params->stackSize : 8388608;
#ifdef OCR_RUNTIME_PROFILER
    compPlatformPthread->doProfile = (params != NULL) ? params->doProfile:true;
#endif
    ((ocrCompPlatformPthread_t*)compPlatformPthread)->tls.pd = NULL;
    ((ocrCompPlatformPthread_t*)compPlatformPthread)->tls.worker = NULL;
}

/******************************************************/
/* OCR COMP PLATFORM PTHREAD FACTORY                  */
/******************************************************/

void destructCompPlatformFactoryPthread(ocrCompPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

void getCurrentEnv(ocrPolicyDomain_t** pd, ocrWorker_t** worker,
                   ocrTask_t **task, ocrPolicyMsg_t* msg) {
    START_PROFILE(cp_getCurrentEnv);
    if (!selfKeyInit) {
        // Key may not have been initialized at runtime boot
        // but the logging facility may invoke getCurrentEnv
        // We cannot rely on 'pthread_getspecific' as behavior
        // is undefined when 'selfKey' hasn't been initialized yet.
        RETURN_PROFILE();
    }
    perThreadStorage_t *tls = pthread_getspecific(selfKey);
    if(tls == NULL) {
        // TLS may be NULLat runtime boot but the logging facility
        // may invoke getCurrentEnv
        RETURN_PROFILE();
    }
    if(pd)
        *pd = tls->pd;
    if(worker)
        *worker = tls->worker;
    if(task && tls->worker)
        *task = tls->worker->curTask;
    if(msg) {
        ASSERT(tls->pd != NULL);
        //By default set src and dest location to current location.
        msg->srcLocation = tls->pd->myLocation;
        msg->destLocation = msg->srcLocation;
        msg->usefulSize = 0; // Convention to say that the size is not yet set
    }
    RETURN_PROFILE();
}

ocrCompPlatformFactory_t *newCompPlatformFactoryPthread(ocrParamList_t *perType) {
    ocrCompPlatformFactory_t *base = (ocrCompPlatformFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrCompPlatformFactoryPthread_t), NONPERSISTENT_CHUNK);

    ocrCompPlatformFactoryPthread_t * derived = (ocrCompPlatformFactoryPthread_t *) base;

    base->instantiate = &newCompPlatformPthread;
    base->initialize = &initializeCompPlatformPthread;
    base->destruct = &destructCompPlatformFactoryPthread;
    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCompPlatform_t*), pthreadDestruct);
    base->platformFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), pthreadSwitchRunlevel);
    base->platformFcts.getThrottle = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, u64*), pthreadGetThrottle);
    base->platformFcts.setThrottle = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, u64), pthreadSetThrottle);
    base->platformFcts.setCurrentEnv = FUNC_ADDR(u8 (*)(ocrCompPlatform_t*, ocrPolicyDomain_t*, ocrWorker_t*), pthreadSetCurrentEnv);

    paramListCompPlatformPthread_t * params =
        (paramListCompPlatformPthread_t *) perType;
    derived->stackSize = ((params != NULL) && (params->stackSize > 0)) ? params->stackSize : 8388608;

    return base;
}
#endif /* ENABLE_COMP_PLATFORM_PTHREAD */

