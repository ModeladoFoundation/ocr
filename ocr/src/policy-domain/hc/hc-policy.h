/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __HC_POLICY_H__
#define __HC_POLICY_H__

#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_HC

#include "ocr-policy-domain.h"
#include "utils/ocr-utils.h"

/******************************************************/
/* OCR-HC POLICY DOMAIN                               */
/******************************************************/

typedef struct {
    ocrPolicyDomainFactory_t base;
} ocrPolicyDomainFactoryHc_t;

typedef struct {
    volatile u64 checkedIn;  // Will initially contain the number of workers we need to
                             // check in and will decrement to zero
    ocrRunlevel_t runlevel;
    s8 nextPhase;
    u32 properties;
} pdHcResumeSwitchRL_t;

typedef struct {
    ocrPolicyDomain_t base;
    u32 rank;           // For MPI use
    volatile u32 pausingWorker; //Worker responsible for pause
    volatile bool runtimePause; //flag to indicate pause
    volatile u32 pauseCounter; //number of paused workers
    volatile u32 queryCounter; //number of workers succesfully queried

    pdHcResumeSwitchRL_t rlSwitch; // Used for asynchronous RL switch
} ocrPolicyDomainHc_t;

typedef struct {
    paramListPolicyDomainInst_t base;
    u32 rank;
} paramListPolicyDomainHcInst_t;

//TODO: Cleanup
void hcDumpWorkerData(ocrPolicyDomainHc_t *rself);

ocrPolicyDomainFactory_t *newPolicyDomainFactoryHc(ocrParamList_t *perType);

ocrPolicyDomain_t * newPolicyDomainHc(ocrPolicyDomainFactory_t * policy,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrCost_t *costFunction, ocrParamList_t *perInstance);

#endif /* ENABLE_POLICY_DOMAIN_HC */
#endif /* __HC_POLICY_H__ */
