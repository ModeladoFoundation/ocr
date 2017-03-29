/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __CE_POLICY_H__
#define __CE_POLICY_H__

#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_CE

#include "ocr-policy-domain.h"
#include "utils/ocr-utils.h"

/******************************************************/
/* OCR-CE POLICY DOMAIN                               */
/******************************************************/

#define CE_TAKE_CHUNK_SIZE 1        //number of tasks that one CE "takes" from another CE

typedef struct {
    ocrPolicyDomainFactory_t base;
} ocrPolicyDomainFactoryCe_t;

// This structure currently assumes only one worker per PD because
// it is only designed to gather information about PDs (ie: not
// asynchronous workers in the same PD)
typedef struct {
    u64 checkedInCount;        // Number of "neighbors" we need to worry about. This
                               // is the initial value of checkedIn. Typically, this is
                               // the number of "children" where the children of a CE are:
                               //   - its XEs
                               //   - if Block0 of a unit, all other blocks in the unit
                               //   - if Unit0, Block0 of a unit, all Block0 CEs of other units
    volatile u64 checkedIn;    // Number of children PDs we got "check-ins" from. It increments
                               // from 0 to checkedInCount
    u32 properties;            // Properties to be passed into the switchRL call
    ocrRunlevel_t oldBarrierRL; // Old RL (to verify barrier properties)
    ocrRunlevel_t barrierRL;   // RL on which we are performing a barrier. Mostly here for
                               // sanity check
    volatile u8 barrierState;  // State of the barrier (see RL_BARRIER_STATE_* in ce-policy.c)
    bool informOtherPDs;       // True if we need to inform other PDs of this RL (basically
                               // we are switching RL and need to tell them)
    bool informedParent;       // True if the parent has already been informed
    u32 pdStatus;              // one of RL_NODE_MASTER, RL_PD_MASTER or 0
} pdCeResumeSwitchRL_t;

typedef struct {
    ocrPolicyDomain_t base;
    u8 xeCount;                     // Number of XE's serviced by this CE
#ifdef OCR_SHARED_XE_POLICY_DOMAIN
    u8 masterXe;                    // The XE in charge of the Policy Domain. This should always
                                    // be XE 0 in the block durring the start up. Any XE may do the
                                    // teardown
#endif
    bool *ceCommTakeActive;         // Flag to coordinate the search for work
    pdCeResumeSwitchRL_t rlSwitch;  // Structure used to coordinate
    u32 nextVictimNeighbor;         // Which CE to steal from next (idx in neighbors)
    s32 nextVictimThrottle;         // BUG #268: Too basic of a throttling scheme
    ocrPolicyDomain_t **allPDs;     // BUG #694: GUIDs across PDs
} ocrPolicyDomainCe_t;

typedef struct {
    paramListPolicyDomainInst_t base;
    u32 xeCount;
    u32 neighborCount;
} paramListPolicyDomainCeInst_t;

ocrPolicyDomainFactory_t *newPolicyDomainFactoryCe(ocrParamList_t *perType);

#endif /* ENABLE_POLICY_DOMAIN_CE */
#endif /* __CE_POLICY_H__ */

