/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_XE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-policy-domain-tasks.h"
#include "ocr-sysboot.h"
#include "ocr-runtime-types.h"
#include "allocator/allocator-all.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#include "extensions/ocr-hints.h"
#include "policy-domain/xe/xe-policy.h"

#include "tg-bin-files.h"

#include "mmio-table.h"
#include "xstg-map.h"

#include "utils/profiler/profiler.h"

#define DEBUG_TYPE POLICY

#define RL_BARRIER_STATE_INVALID          0x0  // Barrier should never happen (used for checking)
#define RL_BARRIER_STATE_UNINIT           0x1  // Barrier has not been started (but children may be reporting)
#define RL_BARRIER_STATE_PARENT_NOTIFIED  0x4  // Parent has been notified
#define RL_BARRIER_STATE_PARENT_RESPONSE  0x8  // Parent has responded thereby releasing us
                                               // and children

// Barrier helper function (for RL switches)
// Wait for children to check in and inform parent
// Blocks until parent response
static void doRLBarrier(ocrPolicyDomain_t *policy) {
    ocrPolicyDomainXe_t *rself = (ocrPolicyDomainXe_t*)policy;
    ocrMsgHandle_t handle;
    ocrMsgHandle_t * pHandle = &handle;

    DPRINTF(DEBUG_LVL_VERB, "Location 0x%"PRIx64": Notifying parent 0x%"PRIx64" of reaching barrier %"PRId32"\n",
            policy->myLocation, policy->parentLocation, rself->rlSwitch.barrierRL);
    // We first notify our parent
    rself->rlSwitch.barrierState = RL_BARRIER_STATE_PARENT_NOTIFIED;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
    msg.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(runlevel) = rself->rlSwitch.barrierRL;
    PD_MSG_FIELD_I(properties) = RL_RESPONSE | RL_BARRIER | RL_FROM_MSG |
        (rself->rlSwitch.properties & (RL_BRING_UP | RL_TEAR_DOWN));
    PD_MSG_FIELD_I(errorCode) = policy->shutdownCode; // Always safe to do
    RESULT_ASSERT(policy->fcts.sendMessage(
                      policy, policy->parentLocation, &msg,
                      NULL, TWOWAY_MSG_PROP | PERSIST_MSG_PROP), ==, 0);
#undef PD_MSG
#undef PD_TYPE

    DPRINTF(DEBUG_LVL_VERB, "Location 0x%"PRIx64": waiting for parent release\n", policy->myLocation);
    // Now we wait for our parent to notify us
    while(rself->rlSwitch.barrierState != RL_BARRIER_STATE_PARENT_RESPONSE) {
        policy->commApis[0]->fcts.initHandle(policy->commApis[0], pHandle);
        RESULT_ASSERT(policy->fcts.waitMessage(policy, &pHandle), ==, 0);
        ASSERT(pHandle && pHandle == &handle);
        ocrPolicyMsg_t *msg = pHandle->response;
        RESULT_ASSERT(policy->fcts.processMessage(policy, msg, true), ==, 0);
        pHandle->destruct(pHandle);
    }
    DPRINTF(DEBUG_LVL_VERB, "Location 0x%"PRIx64": released by parent\n", policy->myLocation);
}

static void performNeighborDiscovery(ocrPolicyDomain_t *policy) {
    // Fill-in location tuples: ours and our parent's (the CE in FSIM)
#ifdef HAL_FSIM_XE
    policy->myLocation = (ocrLocation_t)(*(u64*)(AR_MSR_BASE + CORE_LOCATION_NUM * sizeof(u64)));
#endif // For TG-x86, set in the driver code
    policy->parentLocation = MAKE_CORE_ID(RACK_FROM_ID(policy->myLocation), CUBE_FROM_ID(policy->myLocation),
                                          SOCKET_FROM_ID(policy->myLocation), CLUSTER_FROM_ID(policy->myLocation),
                                          BLOCK_FROM_ID(policy->myLocation), ID_AGENT_CE);
    DPRINTF(DEBUG_LVL_INFO, "Got location 0x%"PRIx64" and parent location 0x%"PRIx64"\n", policy->myLocation, policy->parentLocation);
}

static void findNeighborsPd(ocrPolicyDomain_t *policy) {
#ifdef TG_X86_TARGET
    // Fill out the parentPD information which is needed
    // by the communication layer on TG-x86. See comment in ce-policy.c
    // for how this works
    ocrPolicyDomain_t** neighborsAll = policy->neighborPDs; // Initially set in the driver
    policy->neighborPDs = NULL; // We don't need it afterwards so cleaning up

    policy->parentPD = neighborsAll[CLUSTER_FROM_ID(policy->parentLocation)*MAX_NUM_BLOCK +
                                    BLOCK_FROM_ID(policy->parentLocation)*(MAX_NUM_XE+MAX_NUM_CE) +
                                    ID_AGENT_CE];
    ASSERT(policy->parentPD->myLocation == policy->parentLocation);
    DPRINTF(DEBUG_LVL_VERB, "PD %p (loc: 0x%"PRIx64") found parent at %p (loc: 0x%"PRIx64")\n",
            policy, policy->myLocation, policy->parentPD, policy->parentPD->myLocation);

#endif
}

static u8 helperSwitchInert(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, phase_t phase, u32 properties) {
    u64 i = 0;
    u64 maxCount = 0;
    u8 toReturn = 0;
    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->commApis[i]->fcts.switchRunlevel(
            policy->commApis[i], policy, runlevel, phase, properties, NULL, 0);
    }

    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->guidProviders[i]->fcts.switchRunlevel(
            policy->guidProviders[i], policy, runlevel, phase, properties, NULL, 0);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->allocators[i]->fcts.switchRunlevel(
            policy->allocators[i], policy, runlevel, phase, properties, NULL, 0);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->schedulers[i]->fcts.switchRunlevel(
            policy->schedulers[i], policy, runlevel, phase, properties, NULL, 0);
    }

    return toReturn;
}

// Function to cause run-level switches in this PD
u8 xePdSwitchRunlevel(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, u32 properties) {
#ifdef ENABLE_SYSBOOT_FSIM
    if (XE_PDARGS_OFFSET != offsetof(ocrPolicyDomainXe_t, packedArgsLocation)) {
        DPRINTF(DEBUG_LVL_WARN, "XE_PDARGS_OFFSET (in .../ss/common/include/tg-bin-files.h) is 0x%"PRIx64".  Should be 0x%"PRIx64"\n",
            (u64) XE_PDARGS_OFFSET, (u64) offsetof(ocrPolicyDomainXe_t, packedArgsLocation));
        ASSERT (0);
    }
#endif

#define GET_PHASE(counter) curPhase = (properties & RL_BRING_UP)?counter:(phaseCount - counter - 1)

    u32 maxCount = 0;
    s32 i=0, j=0, k=0, phaseCount=0, curPhase = 0;

    u8 toReturn = 0;

    u32 origProperties = properties;
    u32 masterWorkerProperties = 0;

    ocrPolicyDomainXe_t* rself = (ocrPolicyDomainXe_t*)policy;
    // Check properties
    u32 amNodeMaster = (properties & RL_NODE_MASTER) == RL_NODE_MASTER;
    u32 amPDMaster = properties & RL_PD_MASTER;

    properties &= ~RL_FROM_MSG; // Strip this out from the rest; only valuable for the PD
    masterWorkerProperties = properties;
    properties &= ~RL_NODE_MASTER;



    switch(runlevel) {
    case RL_CONFIG_PARSE:
    {
        // Are we bringing the machine up
        if(properties & RL_BRING_UP) {
            for(i = 0; i < RL_MAX; ++i) {
                for(j = 0; j < RL_PHASE_MAX; ++j) {
                     // Everything has at least one phase on both up and down
                    policy->phasesPerRunlevel[i][j] = (1<<4) + 1;
                }
            }

            phaseCount = 2;
            // For RL_CONFIG_PARSE, we set it to 2 on bring up
            policy->phasesPerRunlevel[RL_CONFIG_PARSE][0] = (1<<4) + 2;
            ASSERT(policy->workerCount == 1); // We only handle one worker per PD

            // See comment in ce-policy.c for why this is here
            performNeighborDiscovery(policy);
        } else {
            // Tear down
            phaseCount = policy->phasesPerRunlevel[RL_CONFIG_PARSE][0] >> 4;
        }
        // Both cases
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                if(toReturn) break;
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase,
                    j==0?masterWorkerProperties:properties, NULL, 0);
            }
            if(!(toReturn) && i == 0 && (properties & RL_BRING_UP)) {
                // After the first phase, we update the counts
                // Coalesce the phasesPerRunLevel by taking the maximum
                for(k = 0; k < RL_MAX; ++k) {
                    u32 finalCount = policy->phasesPerRunlevel[k][0];
                    for(j = 1; j < RL_PHASE_MAX; ++j) {
                        // Deal with UP phase count
                        u32 newCount = 0;
                        newCount = (policy->phasesPerRunlevel[k][j] & 0xF) > (finalCount & 0xF)?
                            (policy->phasesPerRunlevel[k][j] & 0xF):(finalCount & 0xF);
                        // And now the DOWN phase count
                        newCount |= ((policy->phasesPerRunlevel[k][j] >> 4) > (finalCount >> 4)?
                                     (policy->phasesPerRunlevel[k][j] >> 4):(finalCount >> 4)) << 4;
                        finalCount = newCount;
                    }
                    policy->phasesPerRunlevel[k][0] = finalCount;
                }
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_CONFIG_PARSE(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, curPhase, toReturn);
        }

        break;
    }
    case RL_NETWORK_OK:
    {
        if(properties & RL_BRING_UP) {
            findNeighborsPd(policy);
        }
        // We just pass the information down here
        phaseCount = ((policy->phasesPerRunlevel[RL_NETWORK_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                if(toReturn) break;
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_NETWORK_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, curPhase, toReturn);
        }
        break;
    }
    case RL_PD_OK:
    {
        // Just pass it down. We don't do much in the XEs
        phaseCount = ((policy->phasesPerRunlevel[RL_PD_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;

        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                if(toReturn) break;
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_PD_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, curPhase, toReturn);
        }
        break;
    }
    case RL_MEMORY_OK:
    {
        // In this runlevel, in the current implementation, each thread is the
        // PD master after PD_OK so we just check here
        ASSERT(amPDMaster);
        phaseCount = ((policy->phasesPerRunlevel[RL_MEMORY_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;

        // We just pass things down
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                if(toReturn) break;
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_MEMORY_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, curPhase, toReturn);
        }
        break;
    }
    case RL_GUID_OK:
    {
        // TG has multiple PDs (one per CE/XE). We therefore proceed as follows:
        //     - do local transition
        //     - send a response to our parent (our CE; unasked, the CE is waiting for it)
        //     - wait for release from parent
        // NOTE: This protocol is simple and assumes that all PDs behave appropriately (ie:
        // all send their report to their parent without prodding)

        if(properties & RL_BRING_UP) {
            // This step includes a barrier
            ASSERT(properties & RL_BARRIER);
            phaseCount = RL_GET_PHASE_COUNT_UP(policy, RL_GUID_OK);
            maxCount = policy->workerCount;

            // Before we switch any of the inert components, set up the tables
            COMPILE_ASSERT(PDSTT_COMM <= 2);
            policy->strandTables[PDSTT_EVT - 1] = policy->fcts.pdMalloc(policy, sizeof(pdStrandTable_t));
            policy->strandTables[PDSTT_COMM - 1] = policy->fcts.pdMalloc(policy, sizeof(pdStrandTable_t));

            // We need to make sure we have our micro tables up and running
            toReturn = (policy->strandTables[PDSTT_EVT-1] == NULL) ||
            (policy->strandTables[PDSTT_COMM-1] == NULL);

            if (toReturn) {
                DPRINTF(DEBUG_LVL_WARN, "Cannot allocate strand tables\n");
                ASSERT(0);
            } else {
                DPRINTF(DEBUG_LVL_VERB, "Created EVT strand table @ %p\n",
                        policy->strandTables[PDSTT_EVT-1]);
                toReturn |= pdInitializeStrandTable(policy, policy->strandTables[PDSTT_EVT-1], 0);
                DPRINTF(DEBUG_LVL_VERB, "Created COMM strand table @ %p\n",
                        policy->strandTables[PDSTT_COMM-1]);
                toReturn |= pdInitializeStrandTable(policy, policy->strandTables[PDSTT_COMM-1], 0);
            }

            for(i = 0; i < phaseCount; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);
                for(j = 0; j < maxCount; ++j) {
                    if(toReturn) break;
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i,
                        j==0?masterWorkerProperties:properties, NULL, 0);
                }
            }
            if(toReturn == 0) {
                // At this stage, we need to wait for the barrier. We set it up
                rself->rlSwitch.properties = origProperties;
                ASSERT(rself->rlSwitch.barrierRL == RL_GUID_OK);
                ASSERT(rself->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT);
                // Do the barrier
                doRLBarrier(policy);
                // Setup the next one, in this case, it's the teardown barrier
                rself->rlSwitch.barrierRL = RL_USER_OK;
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_UNINIT;
                rself->rlSwitch.properties = RL_TEAR_DOWN | RL_BARRIER;
            }
        } else {
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_GUID_OK);
            maxCount = policy->workerCount;
            for(i = phaseCount; i >= 0; --i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);
                for(j = 0; j < maxCount; ++j) {
                    if(toReturn) break;
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, j==0?masterWorkerProperties:properties, NULL, 0);
                }
            }

            // At the end, we clear out the strand tables and free them.
            DPRINTF(DEBUG_LVL_VERB, "Emptying strand tables\n");
            RESULT_ASSERT(pdProcessStrands(policy, NP_WORK, PDSTT_EMPTYTABLES), ==, 0);
            // Free the tables
            DPRINTF(DEBUG_LVL_VERB, "Freeing EVT strand table: %p\n", policy->strandTables[PDSTT_EVT-1]);
            policy->fcts.pdFree(policy, policy->strandTables[PDSTT_EVT-1]);
            policy->strandTables[PDSTT_EVT-1] = NULL;

            DPRINTF(DEBUG_LVL_VERB, "Freeing COMM strand table: %p\n", policy->strandTables[PDSTT_COMM-1]);
            policy->fcts.pdFree(policy, policy->strandTables[PDSTT_COMM-1]);
            policy->strandTables[PDSTT_COMM-1] = NULL;
        }

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_GUID_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, i-1, toReturn);
        }
        break;
    }
    case RL_COMPUTE_OK:
    {
        if(properties & RL_BRING_UP) {
            phaseCount = RL_GET_PHASE_COUNT_UP(policy, RL_COMPUTE_OK);
            for(i = 0; i < phaseCount; ++i) {
                if(toReturn) break;
                if(RL_IS_FIRST_PHASE_UP(policy, RL_COMPUTE_OK, i)) {
                    guidify(policy, (u64)policy, &(policy->fguid), OCR_GUID_POLICY);
                    policy->placer = NULL; // No placer for TG
                }
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);

                maxCount = policy->workerCount;
                for(j = 1; j < maxCount; ++j) {
                    if(toReturn) break;
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
                if(!toReturn) {
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, masterWorkerProperties, NULL, 0);
                }
            }
        } else {
            // Tear down
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_COMPUTE_OK);
            maxCount = policy->workerCount;
            for(i = phaseCount; i >= 0; --i) {
                if(toReturn) break;
                if(RL_IS_LAST_PHASE_DOWN(policy, RL_COMPUTE_OK, i)) {
                    // We need to deguidify ourself here
                    PD_MSG_STACK(msg);
                    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
                    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                    PD_MSG_FIELD_I(guid) = policy->fguid;
                    PD_MSG_FIELD_I(properties) = 0;
                    toReturn |= policy->fcts.processMessage(policy, &msg, false);
                    policy->fguid.guid = NULL_GUID;
#undef PD_MSG
#undef PD_TYPE
                }
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);

                maxCount = policy->workerCount;
                for(j = 1; j < maxCount; ++j) {
                    if(toReturn) break;
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties,
                            NULL, 0);
                }
                if(!toReturn) {
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, masterWorkerProperties, NULL, 0);
                }
            }
            if(toReturn == 0) {
                // At this stage, we need to wait for the barrier. We set it up
                rself->rlSwitch.properties = origProperties;
                ASSERT(rself->rlSwitch.barrierRL == RL_COMPUTE_OK);
                ASSERT(rself->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT);
                // Do the barrier
                doRLBarrier(policy);
                // There is no next barrier on teardown so we clear things
                rself->rlSwitch.barrierRL = 0;
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_INVALID;
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_COMPUTE_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, i-1, toReturn);
        }
        break;
    }
    case RL_USER_OK:
    {
        if(properties & RL_BRING_UP) {
            phaseCount = RL_GET_PHASE_COUNT_UP(policy, RL_USER_OK);
            maxCount = policy->workerCount;
            for(i = 0; i < phaseCount; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);
                for(j = 1; j < maxCount; ++j) {
                    if(toReturn) break;
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
                // We always start the capable worker last (ya ya, there is only one right now but
                // leaving the logic
                if(toReturn) break;
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, masterWorkerProperties, NULL, 0);
            }

            if(toReturn) {
                DPRINTF(DEBUG_LVL_WARN, "RL_USER_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, i-1, toReturn);
            }

            // When I get here, it means that I dropped out of the RL_USER_OK level
            DPRINTF(DEBUG_LVL_INFO, "PD_MASTER worker dropped out\n");

            // First wait on the barrier at the end of RL_USER_OK
            if(toReturn == 0) {
                doRLBarrier(policy);
                // Setup the next one, in this case, the second teardown barrier
                rself->rlSwitch.barrierRL = RL_COMPUTE_OK;
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_UNINIT;
            }

            // Continue our bring-down (we need to get to get down past COMPUTE_OK)
            policy->fcts.switchRunlevel(policy, RL_COMPUTE_OK, RL_REQUEST | RL_TEAR_DOWN | RL_BARRIER |
                                        ((amPDMaster)?RL_PD_MASTER:0) | ((amNodeMaster)?RL_NODE_MASTER:0));
            // At this point, we can drop out and the driver code will take over taking us down the
            // other runlevels.
        } else {
            ASSERT(rself->rlSwitch.barrierRL == RL_USER_OK);
            ASSERT(rself->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT);
            ASSERT(rself->rlSwitch.properties & RL_TEAR_DOWN);

            // Do our own teardown
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_USER_OK);
            maxCount = policy->workerCount;
            for(i = phaseCount; i >= 0; --i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);


                for(j = 1; j < maxCount; ++j) {
                    if(toReturn) break;
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
                if(toReturn) break;
                // Worker 0 is considered the capable one by convention
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, masterWorkerProperties,
                    NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_USER_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", properties, i-1, toReturn);
        }
        break;
    }
    default:
        // Unknown runlevel
        ASSERT(0);
        break;
    }

    return 0;
}


// See bug #932
// THe list of messages here are the ones that can originate from
// the user but don't have REQ_RESPONSE set
static void setReturnDetail(ocrPolicyMsg_t * msg, u8 returnDetail) {
    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_EVT_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EVT_DESTROY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DEP_SATISFY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_EDTTEMP_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_WORK_CREATE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_WORK_CREATE
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_WORK_DESTROY:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DEP_ADD:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_ADD
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DEP_DYNADD:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_DYNADD
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    case PD_MSG_DB_FREE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_FREE
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    default:
    ASSERT("Unhandled message type in setReturnDetail");
    break;
    }
}

void xePolicyDomainDestruct(ocrPolicyDomain_t * policy) {
    // Destroying instances
    u64 i = 0;
    u64 maxCount = 0;

    // Note: As soon as worker '0' is stopped; its thread is
    // free to fall-through and continue shutting down the
    // policy domain
    maxCount = policy->workerCount;
    for(i = 0; i < maxCount; i++) {
        policy->workers[i]->fcts.destruct(policy->workers[i]);
    }

    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; i++) {
        policy->commApis[i]->fcts.destruct(policy->commApis[i]);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        policy->schedulers[i]->fcts.destruct(policy->schedulers[i]);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        policy->allocators[i]->fcts.destruct(policy->allocators[i]);
    }

    // Destroy these last in case some of the other destructs make use of them
    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        policy->guidProviders[i]->fcts.destruct(policy->guidProviders[i]);
    }

    // Destroy self
    runtimeChunkFree((u64)policy->workers, NULL);
    runtimeChunkFree((u64)policy->schedulers, NULL);
    runtimeChunkFree((u64)policy->allocators, NULL);
    runtimeChunkFree((u64)policy->factories, NULL);
    runtimeChunkFree((u64)policy->guidProviders, NULL);
    runtimeChunkFree((u64)policy, NULL);
}

static void localDeguidify(ocrPolicyDomain_t *self, ocrFatGuid_t *guid) {
    if((!(ocrGuidIsNull(guid->guid))) && (!(ocrGuidIsUninitialized(guid->guid)))) {
        // The XE cannot deguidify since it does not really have a GUID
        // provider and relies on the CE for that. It used to be OK
        // when we used the PTR GUID provider since deguidification was
        // just reading a memory location but that was a bad assumption.
        // There are only two places where localDeguidify is called (when
        // tasks come back in) so if this fails, it means the CE is not
        // deguidifying the tasks prior to sending them back to the XE
        ASSERT(guid->metaDataPtr != NULL);
    }
}


#define NUM_MEM_LEVELS_SUPPORTED 8

static u8 xeAllocateDb(ocrPolicyDomain_t *self, ocrFatGuid_t *guid, void** ptr, u64 size,
                       u32 properties, u64 engineIndex,
                       ocrHint_t *hint, ocrInDbAllocator_t allocator,
                       u64 prescription) {
    // This function allocates a data block for the requestor, who is either this computing agent or a
    // different one that sent us a message.  After getting that data block, it "guidifies" the results
    // which, by the way, ultimately causes xeMemAlloc (just below) to run.
    //
    // Currently, the "affinity" and "allocator" arguments are ignored, and I expect that these will
    // eventually be eliminated here and instead, above this level, processed into the "prescription"
    // variable, which has been added to this argument list.  The prescription indicates an order in
    // which to attempt to allocate the block to a pool.
    u64 idx = 0;
//    void* result = allocateDddatablock (self, size, engineIndex, prescription, &idx);

    int preferredLevel = 0;
    u64 hintValue = 0ULL;
    if (hint != NULL_HINT) {
        if (ocrGetHintValue(hint, OCR_HINT_DB_NEAR, &hintValue) == 0 && hintValue) {
            preferredLevel = 1;
        } else if (ocrGetHintValue(hint, OCR_HINT_DB_INTER, &hintValue) == 0 && hintValue) {
            preferredLevel = 2;
        } else if (ocrGetHintValue(hint, OCR_HINT_DB_FAR, &hintValue) == 0 && hintValue) {
            preferredLevel = 3;
        }
        DPRINTF(DEBUG_LVL_VERB, "xeAllocateDb preferredLevel set to %"PRId32"\n", preferredLevel);
        if (preferredLevel >= 2) {
            return OCR_ENOMEM;
        }
    }

    s8 allocatorIndex = 0;
    *ptr = self->allocators[allocatorIndex]->fcts.allocate(self->allocators[allocatorIndex], size, 0);
    // DPRINTF(DEBUG_LVL_WARN, "xeAllocateDb successfully returning %p\n", result);

    if (*ptr) {
        u8 returnValue = 0;
        returnValue = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->instantiate(
            (ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]), guid, self->allocators[idx]->fguid, self->fguid,
            size, *ptr, hint, properties, NULL);
        if(returnValue != 0) {
            allocatorFreeFunction(*ptr);
        }
        return returnValue;
    } else {
        return OCR_ENOMEM;
    }
}

static u8 xeProcessResponse(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u32 properties) {
    if (msg->srcLocation == self->myLocation) {
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |=  PD_MSG_RESPONSE;
    } else {
        ASSERT(0);
    }
    return 0;
}

static u8 xeProcessCeRequest(ocrPolicyDomain_t *self, ocrPolicyMsg_t **msg) {
    u8 returnCode = 0;
    u32 type = ((*msg)->type & PD_MSG_TYPE_ONLY);
    ASSERT((*msg)->type & PD_MSG_REQUEST);
    if ((*msg)->type & PD_MSG_REQ_RESPONSE) {
        // For blocking messages, we are going to use a persistent buffer
        // and wait for the response
        ocrMsgHandle_t handle;
        ocrMsgHandle_t *pHandle = &handle;
        returnCode = self->fcts.sendMessage(self, self->parentLocation, (*msg),
                                            &pHandle, (TWOWAY_MSG_PROP | PERSIST_MSG_PROP));
        if (returnCode == 0) {
            ASSERT(pHandle && pHandle->msg && pHandle == &handle);
            ASSERT(pHandle->msg == *msg); // This is what we passed in
            RESULT_ASSERT(self->fcts.waitMessage(self, &pHandle), ==, 0);
            ASSERT(pHandle->response);
            DPRINTF(DEBUG_LVL_VVERB, "XE got response from CE @ %p of type 0x%"PRIx32"\n",
                    pHandle->response, pHandle->response->type);
            // Check if the message was a proper response and came from the right place
            ASSERT(pHandle->response->srcLocation == self->parentLocation);
            ASSERT(pHandle->response->destLocation == self->myLocation);

            // Check for shutdown message
            if(type != (pHandle->response->type & PD_MSG_TYPE_ONLY)) {
                // This is currently just the shutdown message
                ASSERT((pHandle->response->type & PD_MSG_TYPE_ONLY) == PD_MSG_MGT_RL_NOTIFY);
                DPRINTF(DEBUG_LVL_VERB, "XE got a shutdown response; processing as new message\n");
                // We process this as a new message
                self->fcts.processMessage(self, pHandle->response, false);
                pHandle->destruct(pHandle);
                return OCR_ECANCELED;
            }

            // Fall-through case is if we actually received a non-shutdown response
            ASSERT((pHandle->response->type & PD_MSG_TYPE_ONLY) == type);
            if(pHandle->response != *msg) {
                // We need to copy things back into *msg
                // BUG #68: This should go away when that issue is fully implemented
                // We use the marshalling function to "copy" this message
                DPRINTF(DEBUG_LVL_VVERB, "Copying response from %p to %p\n",
                        pHandle->response, *msg);
                u64 baseSize = 0, marshalledSize = 0;
                ocrPolicyMsgGetMsgSize(pHandle->response, &baseSize, &marshalledSize, 0);
                // For now, it must fit in a single message
                ASSERT(baseSize + marshalledSize <= sizeof(ocrPolicyMsg_t));
                ocrPolicyMsgMarshallMsg(pHandle->response, baseSize, (u8*)*msg, MARSHALL_DUPLICATE);
            }
            pHandle->destruct(pHandle);
        }
    } else {
        returnCode = self->fcts.sendMessage(self, self->parentLocation, (*msg), NULL, 0);
        // See Bug #932
        setReturnDetail(*msg, returnCode);
    }
    return returnCode;
}

u8 xePolicyDomainProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {

    u8 returnCode = 0;


    DPRINTF(DEBUG_LVL_VVERB, "Going to process message of type 0x%"PRIx64"\n",
            (msg->type & PD_MSG_TYPE_ONLY));
    switch(msg->type & PD_MSG_TYPE_ONLY) {

    // try direct DB alloc, if fails, fallback to CE
    case PD_MSG_DB_CREATE: {
        START_PROFILE(pd_xe_DbCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        ASSERT((PD_MSG_FIELD_I(dbType) == USER_DBTYPE) || (PD_MSG_FIELD_I(dbType) == RUNTIME_DBTYPE));
        DPRINTF(DEBUG_LVL_VVERB, "DB_CREATE request from 0x%"PRIx64" for size %"PRIu64"\n",
                msg->srcLocation, PD_MSG_FIELD_IO(size));
// BUG #145: The prescription needs to be derived from the affinity, and needs to default to something sensible.
        u64 engineIndex = self->myLocation & 0xF;
        // getEngineIndex(self, msg->srcLocation);
        ocrFatGuid_t edtFatGuid = {.guid = PD_MSG_FIELD_I(edt.guid), .metaDataPtr = PD_MSG_FIELD_I(edt.metaDataPtr)};
        u64 reqSize = PD_MSG_FIELD_IO(size);

        u8 ret = xeAllocateDb(
            self, &(PD_MSG_FIELD_IO(guid)), &(PD_MSG_FIELD_O(ptr)), reqSize,
            PD_MSG_FIELD_IO(properties), engineIndex,
            PD_MSG_FIELD_I(hint), PD_MSG_FIELD_I(allocator), 0 /*PRESCRIPTION*/);
        if (ret == 0) {
            PD_MSG_FIELD_O(returnDetail) = ret;
            if(PD_MSG_FIELD_O(returnDetail) == 0) {
                ocrDataBlock_t *db= PD_MSG_FIELD_IO(guid.metaDataPtr);
                ASSERT(db);
                if((PD_MSG_FIELD_IO(properties) & GUID_PROP_IS_LABELED) ||
                   (PD_MSG_FIELD_IO(properties) & DB_PROP_NO_ACQUIRE)) {
                    DPRINTF(DEBUG_LVL_INFO, "Not acquiring DB since disabled by property flags\n");
                    PD_MSG_FIELD_O(ptr) = NULL;
                } else {
                    ASSERT(db->fctId == ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->factoryId);
                    PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.acquire(
                        db, &(PD_MSG_FIELD_O(ptr)), edtFatGuid, EDT_SLOT_NONE,
                        DB_MODE_RW, !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE), (u32)DB_MODE_RW);
                }
            } else {
                // Cannot acquire
                PD_MSG_FIELD_O(ptr) = NULL;
            }
            DPRINTF(DEBUG_LVL_VVERB, "DB_CREATE response for size %"PRIu64": GUID: "GUIDF"; PTR: %p)\n",
                    reqSize, GUIDA(PD_MSG_FIELD_IO(guid.guid)), PD_MSG_FIELD_O(ptr));
            returnCode = xeProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
            EXIT_PROFILE;
            break;
        } else if(ret == OCR_EGUIDEXISTS) {
            // No point falling out to the CE if the GUID exists; it will only tell us the same thing
            EXIT_PROFILE;
            break;
        }
        // fallbacks to CE
        EXIT_PROFILE;
    }


    // First type of messages: things that we offload completely to the CE
    case PD_MSG_DB_DESTROY:
    case PD_MSG_DB_ACQUIRE: case PD_MSG_DB_RELEASE: case PD_MSG_DB_FREE:
    case PD_MSG_MEM_ALLOC: case PD_MSG_MEM_UNALLOC:
    case PD_MSG_WORK_CREATE: case PD_MSG_WORK_DESTROY:
    case PD_MSG_EDTTEMP_CREATE: case PD_MSG_EDTTEMP_DESTROY:
    case PD_MSG_EVT_CREATE: case PD_MSG_EVT_DESTROY: case PD_MSG_EVT_GET:
    case PD_MSG_GUID_CREATE: case PD_MSG_GUID_INFO: case PD_MSG_GUID_DESTROY:
    case PD_MSG_COMM_TAKE: //This is enabled until we move TAKE heuristic in CE policy domain to inside scheduler
    case PD_MSG_SCHED_GET_WORK:
    case PD_MSG_SCHED_NOTIFY:
    case PD_MSG_DEP_ADD: case PD_MSG_DEP_REGSIGNALER: case PD_MSG_DEP_REGWAITER:
    case PD_MSG_HINT_SET: case PD_MSG_HINT_GET:
    case PD_MSG_DEP_SATISFY:
    case PD_MSG_GUID_RESERVE: case PD_MSG_GUID_UNRESERVE: {
        START_PROFILE(pd_xe_OffloadtoCE);

        if((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) {
            START_PROFILE(pd_xe_resolveTemp);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
            if((s32)(PD_MSG_FIELD_IO(paramc)) < 0) {
                // We need to resolve the template with a GUID_INFO call to the CE
                PD_MSG_STACK(tMsg);
                getCurrentEnv(NULL, NULL, NULL, &tMsg);
                ocrFatGuid_t tGuid = PD_MSG_FIELD_I(templateGuid);
#undef PD_MSG
#undef PD_TYPE
#define PD_MSG (&tMsg)
#define PD_TYPE PD_MSG_GUID_INFO
                tMsg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                tMsg.destLocation = self->parentLocation;
                PD_MSG_FIELD_IO(guid) = tGuid;
                PD_MSG_FIELD_I(properties) = 0;
                RESULT_ASSERT(self->fcts.processMessage(self, &tMsg, true), ==, 0);
                ocrTaskTemplate_t *template = (ocrTaskTemplate_t*)PD_MSG_FIELD_IO(guid.metaDataPtr);
#undef PD_MSG
#undef PD_TYPE
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                PD_MSG_FIELD_IO(paramc) = template->paramc;
            }
#undef PD_MSG
#undef PD_TYPE
            EXIT_PROFILE;
        }

        DPRINTF(DEBUG_LVL_VVERB, "Offloading message of type 0x%"PRIx64" to CE\n",
                msg->type & PD_MSG_TYPE_ONLY);
        returnCode = xeProcessCeRequest(self, &msg);

        if(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_COMM_TAKE) && (returnCode == 0)) {
            START_PROFILE(pd_xe_Take);
#define PD_MSG msg
#define PD_TYPE PD_MSG_COMM_TAKE
            if (PD_MSG_FIELD_IO(guidCount) > 0) {
                DPRINTF(DEBUG_LVL_VVERB, "Received EDT with GUID "GUIDF" (@ %p)\n",
                        GUIDA(PD_MSG_FIELD_IO(guids[0].guid)), &(PD_MSG_FIELD_IO(guids[0].guid)));
                localDeguidify(self, (PD_MSG_FIELD_IO(guids)));
                DPRINTF(DEBUG_LVL_VVERB, "Received EDT ("GUIDF"; %p\n",
                        GUIDA((PD_MSG_FIELD_IO(guids))->guid), (PD_MSG_FIELD_IO(guids))->metaDataPtr);
                // For now, we return the execute function for EDTs
                PD_MSG_FIELD_IO(extra) = (u64)(((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.execute);
            }
#undef PD_MSG
#undef PD_TYPE
            EXIT_PROFILE;
        } else if(((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_SCHED_GET_WORK) && (returnCode == 0)) {
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_GET_WORK
            ASSERT(PD_MSG_FIELD_IO(schedArgs).kind == OCR_SCHED_WORK_EDT_USER);
            ocrFatGuid_t *fguid = &PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt;
            if (!(ocrGuidIsNull(fguid->guid))) {
                DPRINTF(DEBUG_LVL_VVERB, "Received EDT with GUID "GUIDF"\n", GUIDA(fguid->guid));
                localDeguidify(self, fguid);
                DPRINTF(DEBUG_LVL_VVERB, "Received EDT ("GUIDF"; %p)\n",
                        GUIDA(fguid->guid), fguid->metaDataPtr);
                PD_MSG_FIELD_O(factoryId) = 0;
            }
#undef PD_MSG
#undef PD_TYPE
        }
        EXIT_PROFILE;
        break;
    }

    // Messages are not handled at all
    case PD_MSG_WORK_EXECUTE: case PD_MSG_DEP_UNREGSIGNALER:
    case PD_MSG_DEP_UNREGWAITER: case PD_MSG_SAL_PRINT:
    case PD_MSG_SAL_READ: case PD_MSG_SAL_WRITE:
    case PD_MSG_MGT_REGISTER: case PD_MSG_MGT_UNREGISTER:
    case PD_MSG_SAL_TERMINATE:
    case PD_MSG_GUID_METADATA_CLONE: case PD_MSG_MGT_MONITOR_PROGRESS: case PD_MSG_METADATA_COMM:
    {
        DPRINTF(DEBUG_LVL_WARN, "XE PD does not handle call of type 0x%"PRIx32"\n",
                (u32)(msg->type & PD_MSG_TYPE_ONLY));
        ASSERT(0);
        returnCode = OCR_ENOTSUP;
        break;
    }

    // Messages handled locally
    case PD_MSG_DEP_DYNADD: {
        START_PROFILE(pd_xe_DepDynAdd);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNADD
        // Check to make sure that the EDT is only doing this to
        // itself
        // Also, this should only happen when there is an actual EDT
        ASSERT(curTask &&
               ocrGuidIsEq(curTask->guid, PD_MSG_FIELD_I(edt.guid)));

        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNADD req/resp for GUID "GUIDF"\n",
                GUIDA(PD_MSG_FIELD_I(db.guid)));
        ASSERT(curTask->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.notifyDbAcquire(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_DYNREMOVE: {
        START_PROFILE(pd_xe_DepDynRemove);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        // Check to make sure that the EDT is only doing this to
        // itself
        // Also, this should only happen when there is an actual EDT
        ASSERT(curTask &&
               ocrGuidIsEq(curTask->guid, PD_MSG_FIELD_I(edt.guid)));
        DPRINTF(DEBUG_LVL_VVERB, "DEP_DYNREMOVE req/resp for GUID "GUIDF"\n",
                GUIDA(PD_MSG_FIELD_I(db.guid)));
        ASSERT(curTask->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.notifyDbRelease(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_MGT_RL_NOTIFY: {
        START_PROFILE(pd_xe_mgt_notify);
        ocrPolicyDomainXe_t *rself = (ocrPolicyDomainXe_t*)self;
#define PD_MSG msg
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
        DPRINTF(DEBUG_LVL_VERB, "Received RL_NOTIFY from 0x%"PRIx64" with properties 0x%"PRIx32"\n",
                msg->srcLocation, PD_MSG_FIELD_I(properties));
        if(PD_MSG_FIELD_I(properties) & RL_FROM_MSG) {
            // This is a message that can only come from the CE
            // It is either a request to shutdown (answer given to
            // another query) or a response that we can proceed past
            // a barrier
            ASSERT(msg->srcLocation == self->parentLocation);
            if(PD_MSG_FIELD_I(properties) & RL_RELEASE) {
                // This is a release from the CE
                // Check that we match on the runlevel
                DPRINTF(DEBUG_LVL_VVERB, "Release from CE\n");
                ASSERT(PD_MSG_FIELD_I(runlevel) == rself->rlSwitch.barrierRL);
                ASSERT(rself->rlSwitch.barrierState == RL_BARRIER_STATE_PARENT_NOTIFIED);
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_PARENT_RESPONSE;
            } else {
                // This is a request for a change of runlevel
                if(PD_MSG_FIELD_I(runlevel) == RL_USER_OK &&
                   (PD_MSG_FIELD_I(properties) & RL_TEAR_DOWN)) {
                    // Record the shutdown code
                    self->shutdownCode = PD_MSG_FIELD_I(errorCode);
                }
                ASSERT(PD_MSG_FIELD_I(runlevel) == rself->rlSwitch.barrierRL);
                if(rself->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT) {
                    DPRINTF(DEBUG_LVL_VVERB, "Request to switch to RL %"PRIu32"\n", rself->rlSwitch.barrierRL);
                    RESULT_ASSERT(self->fcts.switchRunlevel(
                                      self, PD_MSG_FIELD_I(runlevel),
                                      PD_MSG_FIELD_I(properties) | rself->rlSwitch.pdStatus), ==, 0);
                } else {
                    // We already know about the shutdown so we just ignore this
                    DPRINTF(DEBUG_LVL_INFO, "IGNORE 0: runlevel: %"PRIu32", properties: %"PRIu32", rlSwitch.barrierRL: %"PRIu32" rlSwitch.barrierState: %"PRIu32"\n",
                            PD_MSG_FIELD_I(runlevel), PD_MSG_FIELD_I(properties), rself->rlSwitch.barrierRL,
                            rself->rlSwitch.barrierState);
                }
            }
        } else {
            // This is a local shutdown request. We need to start shutting down
            DPRINTF(DEBUG_LVL_VVERB, "Initial, user-initiated, shutdown notification\n");
            // Record the shutdown code to be able to pass it along
            self->shutdownCode = PD_MSG_FIELD_I(errorCode);
            RESULT_ASSERT(self->fcts.switchRunlevel(
                              self, RL_USER_OK, RL_TEAR_DOWN | RL_BARRIER |
                              RL_REQUEST | RL_PD_MASTER), ==, 0);
            // After this, we will drop out in switchRunlevel and proceed with
            // shutdown
        }
        EXIT_PROFILE;
        break;
#undef PD_MSG
#undef PD_TYPE
    }
    default: {
        DPRINTF(DEBUG_LVL_WARN, "Unknown message type 0x%"PRIx32"\n", (u32)(msg->type & PD_MSG_TYPE_ONLY));
        ASSERT(0);
    }
    }; // End of giant switch

    return returnCode;
}

u8 xePdProcessEvent(ocrPolicyDomain_t* self, pdEvent_t **evt, u32 idx) {
    // Simple version to test out micro tasks for now. This just executes a blocking
    // call to the regular process message and returns NULL
    ASSERT(idx == 0);
    ASSERT(((*evt)->properties & PDEVT_TYPE_MASK) == PDEVT_TYPE_MSG);
    pdEventMsg_t *evtMsg = (pdEventMsg_t*)*evt;
    xePolicyDomainProcessMessage(self, evtMsg->msg, true);
    *evt = NULL;
    return 0;
}

u8 xePdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {

    ocrMsgHandle_t thandle;
    ocrMsgHandle_t *pthandle = &thandle;
    ASSERT(target == self->parentLocation); // We should only be sending to our parent
    // Update the message fields
    message->destLocation = target;
    message->srcLocation = self->myLocation;
    while(self->commApis[0]->fcts.sendMessage(self->commApis[0], target, message,
                                              handle, properties) != 0) {
        self->commApis[0]->fcts.initHandle(self->commApis[0], pthandle);
        u8 status = self->fcts.pollMessage(self, &pthandle);
        if(status == 0 || status == POLL_MORE_MESSAGE) {
            RESULT_ASSERT(self->fcts.processMessage(self, pthandle->response, true), ==, 0);
            pthandle->destruct(pthandle);
        }
    }
    return 0;
}

u8 xePdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
    return self->commApis[0]->fcts.pollMessage(self->commApis[0], handle);
}

u8 xePdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
    return self->commApis[0]->fcts.waitMessage(self->commApis[0], handle);
}

void* xePdMalloc(ocrPolicyDomain_t *self, u64 size) {
    START_PROFILE(pd_xe_pdMalloc);

    void* result;
    s8 allocatorIndex = 0;
    result = self->allocators[allocatorIndex]->fcts.allocate(self->allocators[allocatorIndex], size, OCR_ALLOC_HINT_RUNTIME);
    if (result) {
        RETURN_PROFILE(result);
    }
    DPRINTF(DEBUG_LVL_INFO, "xePdMalloc falls back to MSG_MEM_ALLOC for size %"PRId64"\n", (u64) size);
    // fallback to messaging

    // send allocation mesg to CE
    void *ptr;
    PD_MSG_STACK(msg);
    ocrPolicyMsg_t* pmsg = &msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_ALLOC
    msg.type = PD_MSG_MEM_ALLOC  | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(type) = DB_MEMTYPE;
    PD_MSG_FIELD_I(size) = size;
    ASSERT(self->workerCount == 1);              // Assert this XE has exactly one worker.
    u8 msgResult = xeProcessCeRequest(self, &pmsg);
    if(msgResult == 0) {
        ptr = PD_MSG_FIELD_O(ptr);
    } else {
        ptr = NULL;
    }
#undef PD_TYPE
#undef PD_MSG
    RETURN_PROFILE(ptr);
}

void xePdFree(ocrPolicyDomain_t *self, void* addr) {
    START_PROFILE(pd_xe_pdFree);

    // Sometimes XE frees blocks that CE or other XE allocated.
    // XE can free directly even if it was allocated by CE. OK.
    allocatorFreeFunction(addr);
    RETURN_PROFILE();
#if 0    // old code for messaging. Will be removed later.
    // send deallocation mesg to CE
    PD_MSG_STACK(msg);
    ocrPolicyMsg_t *pmsg = &msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MEM_UNALLOC
    msg.type = PD_MSG_MEM_UNALLOC | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(ptr) = addr;
    PD_MSG_FIELD_I(type) = DB_MEMTYPE;
    PD_MSG_FIELD_I(properties) = 0;
    xeProcessCeRequest(self, &pmsg);
#undef PD_MSG
#undef PD_TYPE
    RETURN_PROFILE();
#endif
}

ocrPolicyDomain_t * newPolicyDomainXe(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrParamList_t *perInstance) {
    ocrPolicyDomainXe_t * derived = (ocrPolicyDomainXe_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainXe_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;
    ASSERT(base);
#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, base, statsObject, perInstance);
#else
    factory->initialize(factory, base, perInstance);
#endif
    return base;
}

void initializePolicyDomainXe(ocrPolicyDomainFactory_t * factory, ocrPolicyDomain_t* self,
#ifdef OCR_ENABLE_STATISTICS
                              ocrStats_t *statsObject,
#endif
                              ocrParamList_t *perInstance) {
#ifdef OCR_ENABLE_STATISTICS
    self->statsObject = statsObject;
#endif

    initializePolicyDomainOcr(factory, self, perInstance);
    ocrPolicyDomainXe_t * derived = (ocrPolicyDomainXe_t *) self;
    derived->packedArgsLocation = NULL;
    derived->rlSwitch.barrierRL = RL_GUID_OK;
    derived->rlSwitch.barrierState = RL_BARRIER_STATE_UNINIT;
    derived->rlSwitch.pdStatus = 0;
}

static void destructPolicyDomainFactoryXe(ocrPolicyDomainFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrPolicyDomainFactory_t * newPolicyDomainFactoryXe(ocrParamList_t *perType) {
    ocrPolicyDomainFactory_t* base = (ocrPolicyDomainFactory_t*) runtimeChunkAlloc(sizeof(ocrPolicyDomainFactoryXe_t), NONPERSISTENT_CHUNK);

    ASSERT(base); // Check allocation

#ifdef OCR_ENABLE_STATISTICS
    base->instantiate = FUNC_ADDR(ocrPolicyDomain_t*(*)(ocrPolicyDomainFactory_t*,ocrCost_t*,
                                  ocrParamList_t*), newPolicyDomainXe);
#endif

    base->instantiate = &newPolicyDomainXe;
    base->initialize = &initializePolicyDomainXe;
    base->destruct = &destructPolicyDomainFactoryXe;

    base->policyDomainFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrRunlevel_t, u32), xePdSwitchRunlevel);
    base->policyDomainFcts.destruct = FUNC_ADDR(void(*)(ocrPolicyDomain_t*), xePolicyDomainDestruct);
    base->policyDomainFcts.processMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*,ocrPolicyMsg_t*,u8), xePolicyDomainProcessMessage);
    base->policyDomainFcts.processEvent = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, pdEvent_t**, u32), xePdProcessEvent);
    base->policyDomainFcts.sendMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*, ocrLocation_t, ocrPolicyMsg_t*, ocrMsgHandle_t**, u32),
                                         xePdSendMessage);
    base->policyDomainFcts.pollMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), xePdPollMessage);
    base->policyDomainFcts.waitMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), xePdWaitMessage);

    base->policyDomainFcts.pdMalloc = FUNC_ADDR(void*(*)(ocrPolicyDomain_t*,u64), xePdMalloc);
    base->policyDomainFcts.pdFree = FUNC_ADDR(void(*)(ocrPolicyDomain_t*,void*), xePdFree);
    return base;
}

#endif /* ENABLE_POLICY_DOMAIN_XE */
