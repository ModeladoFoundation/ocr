/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __XE_POLICY_H__
#define __XE_POLICY_H__

#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_XE

#include "ocr-policy-domain.h"
#include "utils/ocr-utils.h"

/******************************************************/
/* OCR-XE POLICY DOMAIN                               */
/******************************************************/

typedef struct {
    ocrPolicyDomainFactory_t base;
} ocrPolicyDomainFactoryXe_t;

// This structure currently assumes only one worker per PD because
// it is only designed to gather information about PDs (ie: not
// asynchronous workers in the same PD)
typedef struct {
    ocrRunlevel_t barrierRL;   // RL on which we are performing a barrier. Mostly here for
                               // sanity check
    u32 properties;            // Properties for the switch of RL
    u32 pdStatus;              // Either RL_PD_MASTER, RL_NODE_MASTER or 0
    volatile u8 barrierState;  // State of the barrier (see RL_BARRIER_STATE_* in xe-policy.c)
} pdXeResumeSwitchRL_t;

typedef struct {
    ocrPolicyDomain_t base;
    void *packedArgsLocation;  // Keep this here.  If moved around, might make mismatch in
                               // .../tg/tgkrnl/inc/tg-bin-file.h, "magic" number XE_PDARGS_OFFSET.
    pdXeResumeSwitchRL_t rlSwitch; // Structure to keep track of runlevel switches
#ifdef OCR_SHARED_XE_POLICY_DOMAIN
    lock_t user_ok_teardown_lock; // This lock makes sure that XE0 waits until the RL_USER_OK
                                  // tear-down has completed before starting the RL_COMPUTE_OK tear-down
#endif
} ocrPolicyDomainXe_t;

typedef struct {
    paramListPolicyDomainInst_t base;
    u32 neighborCount;
} paramListPolicyDomainXeInst_t;

ocrPolicyDomainFactory_t *newPolicyDomainFactoryXe(ocrParamList_t *perType);

#ifdef ENABLE_SYSBOOT_FSIM
#include "tg-bin-files.h"
COMPILE_ASSERT(offsetof(ocrPolicyDomain_t, fcts) + offsetof(ocrPolicyDomainFcts_t, switchRunlevel) == PD_SWITCH_RL_OFFSET);
               // If this fails, go to <build>/tg-xe/tg-bin-files.h and change PD_SWITCH_RL_OFFSET
COMPILE_ASSERT(offsetof(ocrPolicyDomainXe_t, packedArgsLocation) == XE_PDARGS_OFFSET);
               // If this fails, go to tg/tgkrnl/inc/tg-bin-files.h and change XE_PDARGS_OFFSET. Also change in
               // <build>/tg-xe/tg-bin-files.h
#endif


#endif /* ENABLE_POLICY_DOMAIN_XE */
#endif /* __XE_POLICY_H__ */
