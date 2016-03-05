/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

// BUG #145:  The prescription needs to be derived from the affinity, and needs to default to something sensible.
// All this prescription logic needs to be moved out of the PD, into an abstract allocator level.
//#define PRESCRIPTION 0xFEDCBA9876544321LL   // L1 remnant, L2 slice, L2 remnant, L3 slice (twice), L3 remnant, remaining levels each slice-then-remnant.
#define PRESCRIPTION 0xECA86420LL   // L1,L2,...
#define PRESCRIPTION_HINT_MASK 0x1
#define PRESCRIPTION_HINT_NUMBITS 1
#define PRESCRIPTION_LEVEL_MASK 0x7
#define PRESCRIPTION_LEVEL_NUMBITS 3
#define NUM_MEM_LEVELS_SUPPORTED 8

#define RL_BARRIER_STATE_INVALID          0x0  // Barrier should never happen (used for checking)
#define RL_BARRIER_STATE_UNINIT           0x1  // Barrier has not been started (but children may be reporting)
#define RL_BARRIER_STATE_CHILD_WAIT       0x2  // Waiting for children to report in
#define RL_BARRIER_STATE_PARENT_NOTIFIED  0x4  // Parent has been notified
#define RL_BARRIER_STATE_PARENT_RESPONSE  0x8  // Parent has responded thereby releasing us
                                               // and children

#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_CE

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#include "ocr-comm-platform.h"
#include "policy-domain/ce/ce-policy.h"
#include "allocator/allocator-all.h"
#include "extensions/ocr-hints.h"

#include "xstg-map.h"

#include "tg-bin-files.h"

#define DEBUG_TYPE POLICY

extern void ocrShutdown(void);

// Helper routines for RL barriers with other PDs

// Inform other PDs of a change of RL
static void doRLInform(ocrPolicyDomain_t *policy) {
    // We only inform our children CEs and our parent
    u32 i;
    u32 myCluster = CLUSTER_FROM_ID(policy->myLocation);
    u32 myBlock = BLOCK_FROM_ID(policy->myLocation);
    // amGrandMaster is true if Cluster0, Block0
    bool amGrandMaster = (myCluster == 0) && (myBlock == 0);
    bool amClusterMaster = myBlock == 0;
    ocrPolicyDomainCe_t *rself = (ocrPolicyDomainCe_t*)policy;

    DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: Informing children\n", policy->myLocation);
    // Set a flag because some sends may block and we might be other messages in between
    rself->rlSwitch.informOtherPDs = true;

    PD_MSG_STACK(msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
    msg.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
    msg.srcLocation = policy->myLocation;
    PD_MSG_FIELD_I(runlevel) = rself->rlSwitch.barrierRL;
    PD_MSG_FIELD_I(properties) = rself->rlSwitch.properties;
    PD_MSG_FIELD_I(errorCode) = policy->shutdownCode;
#undef PD_MSG
#undef PD_TYPE

    if(policy->neighborCount) {
        // This will be > 0 if there are more than one cluster
        s32 clusterCount = (s32)policy->neighborCount - (MAX_NUM_BLOCK-1) + 1 ; // We exclude ourself in the neighborCount and add one for the cluster count
        if(clusterCount > 0 && amGrandMaster) {
            // I need to inform the other clusters
            // clusterCount does not include cluster0 so we start at 1
            for(i = 1; i < clusterCount; ++i) {
                getCurrentEnv(NULL, NULL, NULL, &msg);
                msg.destLocation = MAKE_CORE_ID(0, 0, 0, i, 0, ID_AGENT_CE);
                DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Informing cluster CE 0x%lx\n",
                        policy->myLocation, msg.destLocation);
                RESULT_ASSERT(policy->fcts.sendMessage(policy, msg.destLocation,
                                                       &msg, NULL, 0), ==, 0);
            }
        }
        if(amClusterMaster) {
            // I need to inform the other blocks in my cluster
            u32 blockCount = MAX_NUM_BLOCK;
            // If only one cluster, cap the number of blocks
            if(clusterCount <= 0) blockCount = policy->neighborCount + 1;
            // Start at one to skip ourself
            for(i = 1; i < blockCount; ++i) {
                getCurrentEnv(NULL, NULL, NULL, &msg);
                msg.destLocation = MAKE_CORE_ID(0, 0, 0, myCluster, i, ID_AGENT_CE);
                DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Informing block CE 0x%lx\n",
                        policy->myLocation, msg.destLocation);
                RESULT_ASSERT(policy->fcts.sendMessage(policy, msg.destLocation,
                                                       &msg, NULL, 0), ==, 0);
            }
        }
        if(!(rself->rlSwitch.informedParent) && (policy->myLocation != policy->parentLocation)) {
            getCurrentEnv(NULL, NULL, NULL, &msg);
            msg.destLocation = policy->parentLocation;
            DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Informing parent 0x%lx\n",
                    policy->myLocation, msg.destLocation);
            rself->rlSwitch.informedParent = true;
            RESULT_ASSERT(policy->fcts.sendMessage(policy, msg.destLocation,
                                                   &msg, NULL, 0), ==, 0);
        }
    }
}

// Wait for children to check in and inform parent
// Blocks until parent response
static void doRLBarrier(ocrPolicyDomain_t *policy) {
    // We loop until rlSwitch's counts match
    ocrPolicyDomainCe_t *rself = (ocrPolicyDomainCe_t*)policy;
    ocrMsgHandle_t handle;
    ocrMsgHandle_t *pHandle = &handle;
    DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: RL Barrier for RL %u\n", policy->myLocation,
            rself->rlSwitch.barrierRL);
    while(rself->rlSwitch.checkedIn != rself->rlSwitch.checkedInCount) {
        DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: looping on barrier %u of %u\n",
                policy->myLocation, rself->rlSwitch.checkedIn, rself->rlSwitch.checkedInCount);
        // We are still waiting on our children to report in
        policy->commApis[0]->fcts.initHandle(policy->commApis[0], pHandle);
        RESULT_ASSERT(policy->fcts.waitMessage(policy, &pHandle), ==, 0);
        ASSERT(pHandle && pHandle == &handle);
        ocrPolicyMsg_t *msg = pHandle->response;
        ASSERT(pHandle->msg == NULL); // Never use persistent messages
        RESULT_ASSERT(policy->fcts.processMessage(policy, msg, true), ==, 0);
        pHandle->destruct(pHandle);
    }
    DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: All children checked in (%u)\n",
            policy->myLocation, rself->rlSwitch.checkedIn);

    // We unset rself->rlSwitch.informOtherPDs so we can continue
    // processing messages if needed
    rself->rlSwitch.informOtherPDs = false;
    rself->rlSwitch.informedParent = false;

    // All our children reported in. We now need to inform our parent
    if(policy->parentLocation != policy->myLocation) {
        DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: Informing parent 0x%lx of release\n",
                policy->myLocation, policy->parentLocation);
        rself->rlSwitch.barrierState = RL_BARRIER_STATE_PARENT_NOTIFIED;
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
        // Notify messages are really one way (two one way messages)
        msg.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
        msg.destLocation = policy->parentLocation;
        PD_MSG_FIELD_I(runlevel) = rself->rlSwitch.barrierRL;
        PD_MSG_FIELD_I(properties) = RL_RESPONSE | RL_ASYNC | RL_FROM_MSG |
            (rself->rlSwitch.properties & (RL_BRING_UP | RL_TEAR_DOWN));
        PD_MSG_FIELD_I(errorCode) = policy->shutdownCode; // Always OK to do
        RESULT_ASSERT(policy->fcts.sendMessage(policy, policy->parentLocation,
                                               &msg, NULL, 0), ==, 0);
#undef PD_MSG
#undef PD_TYPE
    } else {
        rself->rlSwitch.barrierState = RL_BARRIER_STATE_PARENT_RESPONSE;
    }

    // Now we wait for our parent to notify us
    while(rself->rlSwitch.barrierState != RL_BARRIER_STATE_PARENT_RESPONSE) {
        DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Looping waiting for parent response\n", policy->myLocation);
        policy->commApis[0]->fcts.initHandle(policy->commApis[0], pHandle);
        RESULT_ASSERT(policy->fcts.waitMessage(policy, &pHandle), ==, 0);
        ASSERT(pHandle && pHandle == &handle);
        ocrPolicyMsg_t *msg = pHandle->response;
        // We never send a persistent message so we should *never* have handle->msg set
        ASSERT(pHandle->msg == NULL);
        RESULT_ASSERT(policy->fcts.processMessage(policy, msg, true), ==, 0);
        pHandle->destruct(pHandle);
    }
    // We are now good to go. We will release the children in the next phase (doRLRelease)
    DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: Got release from parent\n", policy->myLocation);
}

// Release children from barrier
static void doRLRelease(ocrPolicyDomain_t *policy) {
    // We send a message to all our children that we are done
    // We go over our children starting with the CEs and then the XEs
    // Note that in this phase, we do not look for messages from other CEs (or XEs)
    // because we want to block any other messages to change RL. This will
    // not cause a deadlock because the children we are sending to
    // are blocked anyways.
    u32 i;
    u32 myCluster = CLUSTER_FROM_ID(policy->myLocation);
    u32 myBlock = BLOCK_FROM_ID(policy->myLocation);
    // amGrandMaster is true if Cluster0, Block0
    bool amGrandMaster = (myCluster == 0) && (myBlock == 0);
    bool amClusterMaster = myBlock == 0;
    ocrPolicyDomainCe_t *rself = (ocrPolicyDomainCe_t*)policy;

    DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: Releasing children\n", policy->myLocation);
    PD_MSG_STACK(msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
    msg.type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(runlevel) = rself->rlSwitch.oldBarrierRL;
    PD_MSG_FIELD_I(properties) = RL_RELEASE | RL_FROM_MSG | RL_ASYNC |
        (rself->rlSwitch.properties & (RL_BRING_UP | RL_TEAR_DOWN));

    if(policy->neighborCount) {
        // This will be > 0 if there are more than one cluster
        s32 clusterCount = (s32)policy->neighborCount - (MAX_NUM_BLOCK-1) + 1 ; // We exclude ourself in the neighborCount and add one for the cluster count
        if(clusterCount > 0 && amGrandMaster) {
            // I need to inform the other clusters
            // clusterCount does not include cluster0 so we start at 1
            for(i = 1; i < clusterCount; ++i) {
                getCurrentEnv(NULL, NULL, NULL, &msg);
                msg.destLocation = MAKE_CORE_ID(0, 0, 0, i, 0, ID_AGENT_CE);
                DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Releasing cluster CE 0x%lx\n",
                        policy->myLocation, msg.destLocation);
                // Send message until success but avoid the poll that is
                // present in sendMessage
                while(policy->fcts.sendMessage(policy, msg.destLocation,
                                               &msg, NULL, 1))
                    ;
            }
        }
        if(amClusterMaster) {
            // I need to inform the other blocks in my cluster
            u32 blockCount = MAX_NUM_BLOCK;
            // If only one cluster, cap the number of blocks
            if(clusterCount <= 0) blockCount = policy->neighborCount + 1;
            // Start at one to skip ourself
            for(i = 1; i < blockCount; ++i) {
                getCurrentEnv(NULL, NULL, NULL, &msg);
                msg.destLocation = MAKE_CORE_ID(0, 0, 0, myCluster, i, ID_AGENT_CE);
                DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Releasing block CE 0x%lx\n",
                        policy->myLocation, msg.destLocation);
                while(policy->fcts.sendMessage(policy, msg.destLocation,
                                               &msg, NULL, 1))
                    ;
            }
        }
    }
    // Finally, release the XEs
    for(i=0; i < rself->xeCount; ++i) {
        getCurrentEnv(NULL, NULL, NULL, &msg);
        msg.destLocation = MAKE_CORE_ID(0, 0, 0, myCluster, myBlock, (ID_AGENT_XE0 + i));
        DPRINTF(DEBUG_LVL_VVERB, "Location 0x%lx: Releasing XE 0x%lx\n",
                policy->myLocation, msg.destLocation);
        while(policy->fcts.sendMessage(policy, msg.destLocation,
                                       &msg, NULL, 1))
            ;
    }
#undef PD_MSG
#undef PD_TYPE
    DPRINTF(DEBUG_LVL_VERB, "Location 0x%lx: All children released\n", policy->myLocation);
}

// Returns true if location is a child of policy
static bool isChildLocation(ocrPolicyDomain_t *policy, ocrLocation_t location) {
    u32 myCluster = CLUSTER_FROM_ID(policy->myLocation);
    u32 myBlock = BLOCK_FROM_ID(policy->myLocation);
    // amGrandMaster is true if Cluster0, Block0
    bool amGrandMaster = (myCluster == 0) && (myBlock == 0);
    bool amClusterMaster = myBlock == 0;

    if(location == policy->myLocation) return false;

    // Check if one of our XEs
    if(BLOCK_FROM_ID(location) == myBlock && CLUSTER_FROM_ID(location) == myCluster
       && AGENT_FROM_ID(location) > ID_AGENT_CE) {
        return true;
    }

    // If we are the cluster master, another CE of the same cluster is our child
    if(amClusterMaster && (CLUSTER_FROM_ID(location) == myCluster) &&
       AGENT_FROM_ID(location) == ID_AGENT_CE) {
        return true;
    }

    // If we are the grand master, block 0 of other clusters is our child
    if(amGrandMaster && (BLOCK_FROM_ID(location) == 0) && AGENT_FROM_ID(location) == ID_AGENT_CE) {
        return true;
    }
    return false;
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

static void performNeighborDiscovery(ocrPolicyDomain_t *policy) {
    // First figure out who we are
    // Fill-in location tuples: ours and our parent's (the CE in FSIM)
#ifdef HAL_FSIM_CE
    // On FSim, read the MSR
    // Bug #820: This was a MMIO gate
    policy->myLocation = *((ocrLocation_t*)(AR_MSR_BASE + CORE_LOCATION_NUM * sizeof(u64)));
#endif // On TG-x86, it is set in the driver code
    // My parent is my cluster's block 0 CE
    policy->parentLocation = MAKE_CORE_ID(RACK_FROM_ID(policy->myLocation), CUBE_FROM_ID(policy->myLocation),
                                          SOCKET_FROM_ID(policy->myLocation), CLUSTER_FROM_ID(policy->myLocation),
                                          0, ID_AGENT_CE);

    // If I'm a block 0 CE, my parent is cluster 0 block 0 CE
    if(BLOCK_FROM_ID(policy->myLocation) == 0)
        policy->parentLocation = MAKE_CORE_ID(RACK_FROM_ID(policy->myLocation), CUBE_FROM_ID(policy->myLocation),
                                              SOCKET_FROM_ID(policy->myLocation), 0,
                                              0, ID_AGENT_CE);
    // BUG #231: Generalize this to cover higher levels of hierarchy too.

    // Now that I know who I am, I figure out who my neighbors are
    // Neighbor discovery
    // In the general case, all other CEs in my cluster are my neighbors
    // However, if I'm block 0 CE, my neighbors also include cluster 0 block 0 CE
    // unless I'm cluster 0 block 0 CE
    // TODO: Extend this to Sockets & beyond

    u32 i = 0;
    u32 ncount = 0;

#ifndef HAL_FSIM_CE
    policy->neighbors = (ocrLocation_t*)runtimeChunkAlloc(policy->neighborCount*sizeof(ocrLocation_t), PERSISTENT_CHUNK);
    DPRINTF(DEBUG_LVL_VERB, "Allocated neighbors for count %u @ 0x%lx\n", policy->neighborCount, policy->neighbors);
#endif

    if(policy->neighborCount) {
        for (i = 0; i < MAX_NUM_BLOCK; i++) {
            u32 myCluster = CLUSTER_FROM_ID(policy->myLocation);
            if(policy->myLocation != MAKE_CORE_ID(0, 0, 0, myCluster, i, ID_AGENT_CE)) {
                policy->neighbors[ncount++] = MAKE_CORE_ID(0, 0, 0, myCluster, i, ID_AGENT_CE);
                DPRINTF(DEBUG_LVL_VERB, "For cluster %u and block %u, got agent ID 0x%x and setting to %u (@ 0x%lx)\n",
                        myCluster, i, policy->neighbors[ncount-1], ncount-1, &(policy->neighbors[ncount-1]));
            }
            if(ncount >= policy->neighborCount) break;
        }

        // Block 0 of any cluster also has block 0 of other clusters as neighbors
        if((ncount < policy->neighborCount) && (BLOCK_FROM_ID(policy->myLocation) == 0)) {
            for (i = 0; i < MAX_NUM_CLUSTER; i++) {
                if(policy->myLocation != MAKE_CORE_ID(0, 0, 0, i, 0, ID_AGENT_CE)) {
                    policy->neighbors[ncount++] = MAKE_CORE_ID(0, 0, 0, i, 0, ID_AGENT_CE);
                    DPRINTF(DEBUG_LVL_VERB, "For cluster %u, got cluster agent ID 0x%x and setting to %u (@ 0x%lx)\n",
                            i, policy->neighbors[ncount-1], ncount-1, &(policy->neighbors[ncount-1]));
                }
                if(ncount >= policy->neighborCount) break;
            }
        }
    }
    policy->neighborCount = ncount;

    DPRINTF(DEBUG_LVL_INFO, "Got location 0x%lx and parent location 0x%lx\n", policy->myLocation, policy->parentLocation);
}

static void findNeighborsPd(ocrPolicyDomain_t *policy) {
#ifdef TG_X86_TARGET
    u32 myCluster = CLUSTER_FROM_ID(policy->myLocation);
    u32 myBlock = BLOCK_FROM_ID(policy->myLocation);

    // Fill out the neighborPDs structure which is needed
    // by the communication layer on TG-x86. Note that this
    // relies on having had all PDs do performNeighborDiscovery
    // already so needs to be across a barrier in the RL scheme.
    // Currently, we do this as the first step of NETWORK_OK
    // while performNeighborDiscovery is done as the first step in
    // RL_CONFIG_PARSE
    u32 i;
    ocrPolicyDomain_t** neighborsAll = policy->neighborPDs; // Initially set in the driver
    ocrPolicyDomainCe_t *rself = (ocrPolicyDomainCe_t*)policy;
    rself->allPDs = neighborsAll; // Bug #694: Keep track of this for remote GUID resolution
    policy->neighborPDs = (ocrPolicyDomain_t**)runtimeChunkAlloc(
        (policy->neighborCount + rself->xeCount)*sizeof(ocrPolicyDomain_t*), PERSISTENT_CHUNK);
    policy->parentPD = NULL;

    // neighborsAll is properly sorted and contains all possible neighbors for the entire machine
    // (all CEs and XEs). We extract from there just the ones that are of interest to us (our XEs
    // and the CEs we are supposed to communicate with)
    for(i=0; i < rself->xeCount; ++i) {
        ocrLocation_t curNeighbor = MAKE_CORE_ID(0, 0, 0, myCluster, myBlock, (ID_AGENT_XE0 + i));
        policy->neighborPDs[i] = neighborsAll[myCluster*MAX_NUM_BLOCK + myBlock*(MAX_NUM_XE + MAX_NUM_CE) + i + MAX_NUM_CE];
        ASSERT(curNeighbor == policy->neighborPDs[i]->myLocation);
        DPRINTF(DEBUG_LVL_VERB, "PD 0x%lx (loc: 0x%lx) found XE neighbor %u at 0x%lx (loc: 0x%lx)\n",
                policy, policy->myLocation, i, policy->neighborPDs[i],
                policy->neighborPDs[i]->myLocation);
    }
    for(i = rself->xeCount; i < policy->neighborCount + rself->xeCount; ++i) {
        ocrLocation_t curNeighbor = policy->neighbors[i - rself->xeCount];
        ASSERT(AGENT_FROM_ID(curNeighbor) == ID_AGENT_CE); // It should be a CE here
        policy->neighborPDs[i] = neighborsAll[CLUSTER_FROM_ID(curNeighbor)*MAX_NUM_BLOCK +
                                              BLOCK_FROM_ID(curNeighbor)*(MAX_NUM_XE + MAX_NUM_CE) +
                                              AGENT_FROM_ID(curNeighbor)];
        ASSERT(curNeighbor == policy->neighborPDs[i]->myLocation);
        if(curNeighbor == policy->parentLocation) {
            policy->parentPD = policy->neighborPDs[i];
        }
        DPRINTF(DEBUG_LVL_VERB, "PD 0x%lx (loc: 0x%lx) found neighbor %u at 0x%lx (loc: 0x%lx)\n",
                policy, policy->myLocation, i, policy->neighborPDs[i], policy->neighborPDs[i]->myLocation);
    }
    // We should have also found our parent
    if(policy->parentPD == NULL) {
        // Case with no neighbors
        ASSERT(policy->parentLocation == policy->myLocation);
        policy->parentPD = policy;
    }
    ASSERT(policy->parentPD != NULL);
    DPRINTF(DEBUG_LVL_VERB, "PD 0x%lx (loc: 0x%lx) found parent at 0x%lx (loc: 0x%lx)\n",
            policy, policy->myLocation, policy->parentPD, policy->parentPD->myLocation);

#endif
}

// Function to cause run-level switches in this PD
u8 cePdSwitchRunlevel(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, u32 properties) {

#define GET_PHASE(counter) curPhase = (properties & RL_BRING_UP)?counter:(phaseCount - counter - 1)

    u32 maxCount = 0;
    s32 i=0, j=0, k=0, phaseCount=0, curPhase = 0;

    u8 toReturn = 0;

    u32 origProperties = properties;
    u32 masterWorkerProperties = 0;

    ocrPolicyDomainCe_t* rself = (ocrPolicyDomainCe_t*)policy;
    // Check properties
    u32 amNodeMaster = (properties & RL_NODE_MASTER) == RL_NODE_MASTER;
    u32 amPDMaster = properties & RL_PD_MASTER;

    u32 fromPDMsg = properties & RL_FROM_MSG;
    properties &= ~RL_FROM_MSG; // Strip this out from the rest; only valuable for the PD
    masterWorkerProperties = properties;
    properties &= ~RL_NODE_MASTER;


    switch (runlevel) {
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

            // We need to perform network discovery here because on TG-x86, the setting
            // up of the communication channels relies on the implicit barrier between RL_CONFIG_PARSE
            // and RL_NETWORK_OK and the comm platforms will need the neighbor information
            // during RL_CONFIG_PARSE
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
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
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
            DPRINTF(DEBUG_LVL_WARN, "RL_CONFIG_PARSE(%d) phase %d failed: %d\n", properties, curPhase, toReturn);
        }

        break;
    }
    case RL_NETWORK_OK:
    {
        if(properties & RL_BRING_UP) {
            // Fill in our neighborPDs structure if needed
            findNeighborsPd(policy);
            // Set the ceCommTakeActive structure up properly
            for(i = 0; i < policy->neighborCount; ++i) {
                rself->ceCommTakeActive[i] = 0;
            }
        }
        // Transition all other modules
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
        if(toReturn && (properties & RL_TEAR_DOWN)) {
            // Free up the neighborPDs structure
#ifdef TG_X86_TARGET
            runtimeChunkFree((u64)(policy->neighborPDs), PERSISTENT_CHUNK);
#endif
            runtimeChunkFree((u64)(policy->neighbors), PERSISTENT_CHUNK);
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_NETWORK_OK(%d) phase %d failed: %d\n", properties, curPhase, toReturn);
        }
        break;
    }
    case RL_PD_OK:
    {
        phaseCount = ((policy->phasesPerRunlevel[RL_PD_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;

        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            // At bring up, for the first phase, we count the number of shutdowns
            // we need to keep track of
            if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(policy, RL_PD_OK, curPhase)) {
                // The XEs are our children (always)
                rself->rlSwitch.checkedInCount = rself->xeCount;
                // Block 0 also collects other blocks in its cluster
                if(BLOCK_FROM_ID(policy->myLocation) == 0) {
                    // Neighbors include all blocks in my cluster and if I am
                    // block 0, I also have all other block 0s in other clusters
                    rself->rlSwitch.checkedInCount += policy->neighborCount;
                    if(CLUSTER_FROM_ID(policy->myLocation)) {
                        // If I am not cluster 0, my children do *not* include
                        // block0 of other clusters
                        u32 otherblocks = 0;
                        u32 j;
                        for(j = 0; j<policy->neighborCount; ++j)
                            if(BLOCK_FROM_ID(policy->neighbors[j]) == 0)
                                ++otherblocks;
                        rself->rlSwitch.checkedInCount -= otherblocks;
                    }
                }
            }
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                if(toReturn) break;
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_PD_OK(%d) phase %d failed: %d\n", properties, curPhase, toReturn);
        }
        break;
    }
    case RL_MEMORY_OK:
    {
        // In this runlevel, in the current implementation, each thread is the
        // PD master after PD_OK so we just check here
        ASSERT(amPDMaster);
        phaseCount = ((policy->phasesPerRunlevel[RL_MEMORY_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;

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
        if((toReturn == 0) && (properties & RL_BRING_UP)) {
            // Here we set whether we are a node master or a pd master. This is important during
            // shutdown
            rself->rlSwitch.pdStatus = amNodeMaster?RL_NODE_MASTER:RL_PD_MASTER;

            // Last phase of bring up; figure out what allocators to use
            u64 low, high, level, engineIdx;
            maxCount = policy->allocatorCount;
            low = 0;
            for(i = 0; i < maxCount; ++i) {
                level = policy->allocators[low]->memories[0]->level;
                high = i+1;
                if (high == maxCount || policy->allocators[high]->memories[0]->level != level) {
                    // One or more allocators for some level were provided in the config file.
                    // Their indices in the array of allocators span from low(inclusive)
                    // to high (exclusive).  From this information,
                    // initialize the allocatorIndexLookup table for that level, for all agents.
                    if (high - low == 1) {
                        // All agents in the block use the same allocator
                        // (usage conjecture: on TG, this is used for all but L1.)
                        for (engineIdx = 0; engineIdx <= rself->xeCount; ++engineIdx) {
                            policy->allocatorIndexLookup[
                                engineIdx*NUM_MEM_LEVELS_SUPPORTED+level] = low;
                        }
                    } else if (high - low == 2) {
                        // All agents except the last in the block (i.e. the XEs)
                        // use the same allocator. The last (i.e. the CE) uses a
                        // different allocator. (usage conjecture: on TG,
                        // this is might be used to experiment with XEs sharing a
                        // common SPAD that is different than what the CE uses.
                        // This is presently just for academic interst.)
                        for (engineIdx = 0; engineIdx < rself->xeCount; ++engineIdx) {
                            policy->allocatorIndexLookup[
                                engineIdx*NUM_MEM_LEVELS_SUPPORTED+level] = low;
                        }
                        policy->allocatorIndexLookup[
                            engineIdx*NUM_MEM_LEVELS_SUPPORTED+level] = low+1;
                    } else if (high - low == rself->xeCount+1) {
                        // All agents in the block use different allocators
                        // (usage conjecture: on TG, this is used for L1.)
                        for (engineIdx = 0; engineIdx <= rself->xeCount; ++engineIdx) {
                            policy->allocatorIndexLookup[
                                engineIdx*NUM_MEM_LEVELS_SUPPORTED+level] = low+engineIdx;
                        }
                    } else {
                        DPRINTF(DEBUG_LVL_WARN, "Unable to spread allocator[%ld:%ld] (level %ld) across %ld XEs plus the CE\n",
                                (u64)low, (u64)high-1, (u64)level, (u64)rself->xeCount);
                        ASSERT (0);
                    }
                    low = high;
                }
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_MEMORY_OK(%d) phase %d failed: %d\n", properties, curPhase, toReturn);
        }
        break;
    }
    case RL_GUID_OK:
    {
        // TG has multiple PDs (one per CE/XE). We therefore proceed as follows:
        //     - do local transition
        //     - wait for all our children to report to us
        //     - send a response to our parent
        //     - wait for release from parent
        //     - release all children
        // NOTE: This protocol is simple and assumes that all PDs behave appropriately (ie:
        // all send their report to their parent without prodding)

        if(properties & RL_BRING_UP) {
            // This step includes a barrier
            ASSERT(properties & RL_BARRIER);
            phaseCount = RL_GET_PHASE_COUNT_UP(policy, RL_GUID_OK);
            maxCount = policy->workerCount;

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
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_CHILD_WAIT;
                // Do the barrier
                doRLBarrier(policy);
                // Setup the next one, in this case, it's the teardown barrier
                rself->rlSwitch.oldBarrierRL = RL_GUID_OK;
                rself->rlSwitch.barrierRL = RL_USER_OK;
                rself->rlSwitch.checkedIn = 0;
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_UNINIT;
                rself->rlSwitch.properties = RL_REQUEST | RL_TEAR_DOWN | RL_BARRIER | RL_FROM_MSG;
                // Release the children
                doRLRelease(policy);
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
        }

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_GUID_OK(%d) phase %d failed: %d\n", properties, i-1, toReturn);
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
                    policy->platformModel = NULL; // No platform model for TG
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
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_CHILD_WAIT;
                // Do the barrier
                doRLBarrier(policy);
                // There is no next barrier on teardown so we clear things
                rself->rlSwitch.oldBarrierRL = RL_COMPUTE_OK;
                rself->rlSwitch.barrierRL = 0;
                rself->rlSwitch.checkedIn = 0;
                rself->rlSwitch.barrierState = RL_BARRIER_STATE_INVALID;
                // Release the children
                doRLRelease(policy);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_COMPUTE_OK(%d) phase %d failed: %d\n", properties, i-1, toReturn);
        }
        break;
    }
    case RL_USER_OK:
    {
        if(properties & RL_BRING_UP) {
            for(i=0; i<policy->neighborCount; ++i) {
                DPRINTF(DEBUG_LVL_VERB, "Neighbors[%u] = 0x%lx\n", i, policy->neighbors[i]);
            }
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
                DPRINTF(DEBUG_LVL_WARN, "RL_USER_OK(%d) phase %d failed: %d\n", properties, i-1, toReturn);
            }

            // When I get here, it means that I dropped out of the RL_USER_OK level
            DPRINTF(DEBUG_LVL_INFO, "PD_MASTER worker dropped out\n");

            // This will do the barrier that we setup in the shutdown part of this runlevel
            doRLBarrier(policy);
            // Setup the next one, in this case, the second teardown barrier
            rself->rlSwitch.oldBarrierRL = RL_USER_OK;
            rself->rlSwitch.barrierRL = RL_COMPUTE_OK;
            rself->rlSwitch.checkedIn = 0;
            rself->rlSwitch.barrierState = RL_BARRIER_STATE_UNINIT;
            // Release the children
            doRLRelease(policy);

            // Continue our bring-down (we need to get to get down past COMPUTE_OK)
            policy->fcts.switchRunlevel(policy, RL_COMPUTE_OK, RL_REQUEST | RL_TEAR_DOWN | RL_BARRIER |
                                        ((amPDMaster)?RL_PD_MASTER:0) | ((amNodeMaster)?RL_NODE_MASTER:0));
            // At this point, we can drop out and the driver code will take over taking us down the
            // other runlevels.
        } else {
            ASSERT(rself->rlSwitch.barrierRL == RL_USER_OK);
            ASSERT(rself->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT);
            ASSERT(rself->rlSwitch.properties & (RL_TEAR_DOWN | RL_FROM_MSG));
            ASSERT(fromPDMsg); // Shutdown requests always come from a message from one of the XEs

            // We set this here now because otherwise we may process more messages
            // from XEs that know about our shutdown before we drop out
            rself->rlSwitch.barrierState = RL_BARRIER_STATE_CHILD_WAIT;
            doRLInform(policy); // Inform children that we are shutting down

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
            DPRINTF(DEBUG_LVL_WARN, "RL_USER_OK(%d) phase %d failed: %d\n", properties, i-1, toReturn);
        }
        break;
    }
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void cePolicyDomainDestruct(ocrPolicyDomain_t * policy) {
    // Destroying instances
    u64 i = 0;
    u64 maxCount = 0;

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
    runtimeChunkFree((u64)policy->allocatorIndexLookup, NULL);
    runtimeChunkFree((u64)policy->taskFactories, NULL);
    runtimeChunkFree((u64)policy->taskTemplateFactories, NULL);
    runtimeChunkFree((u64)policy->dbFactories, NULL);
    runtimeChunkFree((u64)policy->eventFactories, NULL);
    runtimeChunkFree((u64)policy->guidProviders, NULL);
    runtimeChunkFree((u64)policy, NULL);
}

static void localDeguidify(ocrPolicyDomain_t *self, ocrFatGuid_t *guid, ocrGuidKind *kind) {
    ASSERT(self->guidProviderCount == 1);
    // Bug #694: All this logic should be moved into the GUID provider
    if(guid->guid != NULL_GUID && guid->guid != UNINITIALIZED_GUID) {
        if(guid->metaDataPtr == NULL) {
            PD_MSG_STACK(ceMsg);
            getCurrentEnv(NULL, NULL, NULL, &ceMsg);
#define PD_MSG (&ceMsg)
#define PD_TYPE PD_MSG_GUID_INFO
            ceMsg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_IO(guid) = *guid;
            PD_MSG_FIELD_I(properties) = 0; // I really only need the resolved GUID
            RESULT_ASSERT(self->fcts.processMessage(self, &ceMsg, true), ==, 0);
            guid->metaDataPtr = PD_MSG_FIELD_IO(guid.metaDataPtr);
#undef PD_TYPE
#undef PD_MSG
        }
        if(kind != NULL) {
            // We use a cheaper call that should be able to extract a kind
            // directly from the GUID. This implies that the kind is
            // encoded in the GUID which is the case for now
            self->guidProviders[0]->fcts.getKind(self->guidProviders[0], guid->guid, kind);
        }
    } else {
        if(kind != NULL)
            *kind = OCR_GUID_NONE;
    }
}

static u8 getEngineIndex(ocrPolicyDomain_t *self, ocrLocation_t location) {

    ocrPolicyDomainCe_t* derived = (ocrPolicyDomainCe_t*) self;

    // We need the block-relative index of the engine (aka agent).  That index for XE's ranges from
    // 0 to xeCount-1, and the engine index for the CE is next, i.e. it equals xeCount.  We derive
    // the index from the system-wide absolute location of the agent.  If a time comes when different
    // chips in a system might have different numbers of XEs per block, this code might break, and
    // have to be reworked.

    ocrLocation_t blockRelativeEngineIndex =
        location -         // Absolute location of the agent for which the allocation is being done.
        self->myLocation + // Absolute location of the CE doing the allocation.
        derived->xeCount;  // Offset so that first XE is 0, last is xeCount-1, and CE is xeCount.

    // BUG #270: Temporary override to avoid a bug.
    if(location == self->myLocation)
        blockRelativeEngineIndex = derived->xeCount;
    else blockRelativeEngineIndex = location & 0xF;

    ASSERT(blockRelativeEngineIndex >= 0);
    ASSERT(blockRelativeEngineIndex <= derived->xeCount);
    return blockRelativeEngineIndex;
}


// In all these functions, we consider only a single PD. In other words, in CE, we
// deal with everything locally and never send messages

// allocateDatablock:  Utility used by ceAllocateDb and ceMemAlloc, just below.
static void* allocateDatablock (ocrPolicyDomain_t *self,
                                u64                size,
                                u64                engineIndex,
                                u64                prescription,
                                u64                preferredLevel,
                                u64               *allocatorIndexReturn) {
    void* result;
    u64 allocatorHints;  // Allocator hint
    u64 levelIndex; // "Level" of the memory hierarchy, taken from the "prescription" to pick from the allocators array the allocator to try.
    s8 allocatorIndex;

    ASSERT (self->allocatorCount > 0);

    do {
        allocatorHints = (prescription & PRESCRIPTION_HINT_MASK) ?
            (OCR_ALLOC_HINT_NONE) :              // If the hint is non-zero, pick this (e.g. for tlsf, tries "remnant" pool.)
            (OCR_ALLOC_HINT_REDUCE_CONTENTION);  // If the hint is zero, pick this (e.g. for tlsf, tries a round-robin-chosen "slice" pool.)
        prescription >>= PRESCRIPTION_HINT_NUMBITS;
        levelIndex = prescription & PRESCRIPTION_LEVEL_MASK;
        prescription >>= PRESCRIPTION_LEVEL_NUMBITS;
        if (levelIndex < preferredLevel) {       // don't try if less than preferredLevel
            DPRINTF(DEBUG_LVL_VVERB, "allocateDatablock skips level %ld\n", levelIndex);
            continue;
        }
        allocatorIndex = self->allocatorIndexLookup[engineIndex*NUM_MEM_LEVELS_SUPPORTED+levelIndex]; // Lookup index of allocator to use for requesting engine (aka agent) at prescribed memory hierarchy level.
        if ((allocatorIndex < 0) ||
            (allocatorIndex >= self->allocatorCount) ||
            (self->allocators[allocatorIndex] == NULL)) continue;  // Skip this allocator if it doesn't exist.
        result = self->allocators[allocatorIndex]->fcts.allocate(self->allocators[allocatorIndex], size, allocatorHints);
        if (result) {
            DPRINTF (DEBUG_LVL_VVERB, "Success allocation %5ld-byte block to allocator %ld (level %ld) for engine %ld (%2ld) -- 0x%lx\n",
            (u64) size, (u64) allocatorIndex, (u64) levelIndex, (u64) engineIndex, (u64) self->myLocation, (u64) (((u64*) result)[-1]));
            *allocatorIndexReturn = allocatorIndex;
            return result;
        }
    } while (prescription != 0);

    return NULL;
}

static u8 ceAllocateDb(ocrPolicyDomain_t *self, ocrFatGuid_t *guid, void** ptr, u64 size,
                       u32 properties, u64 engineIndex,
                       ocrFatGuid_t affinity, ocrInDbAllocator_t allocator,
                       u64 prescription) {
    // This function allocates a data block for the requestor, who is either this computing agent or a
    // different one that sent us a message.  After getting that data block, it "guidifies" the results
    // which, by the way, ultimately causes ceMemAlloc (just below) to run.
    //
    // Currently, the "affinity" and "allocator" arguments are ignored, and I expect that these will
    // eventually be eliminated here and instead, above this level, processed into the "prescription"
    // variable, which has been added to this argument list.  The prescription indicates an order in
    // which to attempt to allocate the block to a pool.

    u64 idx;
    void* result;
    int preferredLevel = 0;
    if ((u64)affinity.guid > 0 && (u64)affinity.guid <= NUM_MEM_LEVELS_SUPPORTED) {
        preferredLevel = (u64)affinity.guid;
        DPRINTF(DEBUG_LVL_WARN, "ceAllocateDb affinity.guid %llx  .metaDataPtr %p\n", affinity.guid, affinity.metaDataPtr);
        DPRINTF(DEBUG_LVL_WARN, "ceAllocateDb preferred %ld\n", preferredLevel);
        result = allocateDatablock (self, size, engineIndex, prescription, preferredLevel, &idx);
        if (!result) {
            DPRINTF(DEBUG_LVL_WARN, "ceAllocateDb ignores preferredLevel hint to be successful in alloc%ld\n", preferredLevel);
            result = allocateDatablock (self, size, engineIndex, prescription, 0, &idx);
        }
    } else {
        result = allocateDatablock (self, size, engineIndex, prescription, 0, &idx);
    }

    if (result) {
        ocrDataBlock_t *block = self->dbFactories[0]->instantiate(
            self->dbFactories[0], self->allocators[idx]->fguid, self->fguid,
            size, result, properties, NULL);
        *ptr = result;
        (*guid).guid = block->guid;
        (*guid).metaDataPtr = block;
        return 0;
    } else {
        DPRINTF(DEBUG_LVL_WARN, "ceAllocateDb returning NULL for size %ld\n", (u64) size);
        return OCR_ENOMEM;
    }
}

static u8 ceMemAlloc(ocrPolicyDomain_t *self, ocrFatGuid_t* allocator, u64 size,
                     u64 engineIndex, ocrMemType_t memType, void** ptr,
                     u64 prescription) {
    // Like hcAllocateDb, this function also allocates a data block.  But it does NOT guidify
    // the results.  The main usage of this function is to allocate space for the guid needed
    // by ceAllocateDb; so if this function also guidified its results, you'd be in an infinite
    // guidification loop!
    void* result;
    u64 idx;
    ASSERT (memType == GUID_MEMTYPE || memType == DB_MEMTYPE);
    result = allocateDatablock (self, size, engineIndex, prescription, 0, &idx);

    if (result) {
        *ptr = result;
        *allocator = self->allocators[idx]->fguid;
        return 0;
    } else {
        DPRINTF(DEBUG_LVL_WARN, "ceMemAlloc returning NULL for size %ld\n", (u64) size);
        return OCR_ENOMEM;
    }
}

static u8 ceMemUnalloc(ocrPolicyDomain_t *self, ocrFatGuid_t* allocator,
                       void* ptr, ocrMemType_t memType) {
    allocatorFreeFunction(ptr);
    return 0;
}

static u8 ceCreateEdt(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                      ocrFatGuid_t  edtTemplate, u32 *paramc, u64* paramv,
                      u32 *depc, u32 properties, ocrFatGuid_t affinity,
                      ocrFatGuid_t * outputEvent, ocrTask_t * currentEdt,
                      ocrFatGuid_t parentLatch) {


    ocrTaskTemplate_t *taskTemplate = (ocrTaskTemplate_t*)edtTemplate.metaDataPtr;

    ASSERT(((taskTemplate->paramc == EDT_PARAM_UNK) && *paramc != EDT_PARAM_DEF) ||
           (taskTemplate->paramc != EDT_PARAM_UNK && (*paramc == EDT_PARAM_DEF ||
                                                      taskTemplate->paramc == *paramc)));
    ASSERT(((taskTemplate->depc == EDT_PARAM_UNK) && *depc != EDT_PARAM_DEF) ||
           (taskTemplate->depc != EDT_PARAM_UNK && (*depc == EDT_PARAM_DEF ||
                                                    taskTemplate->depc == *depc)));

    if(*paramc == EDT_PARAM_DEF) {
        *paramc = taskTemplate->paramc;
    }

    if(*depc == EDT_PARAM_DEF) {
        *depc = taskTemplate->depc;
    }
    // Check paramc/paramv combination validity
    if((*paramc > 0) && (paramv == NULL)) {
        DPRINTF(DEBUG_LVL_WARN,"error: EDT paramc set to %d but paramv is NULL\n", *paramc);
        ASSERT(false);
        return OCR_EINVAL;
    }
    if((*paramc == 0) && (paramv != NULL)) {
        DPRINTF(DEBUG_LVL_WARN,"error: EDT paramc set to zero but paramv not NULL\n");
        ASSERT(false);
        return OCR_EINVAL;
    }

    u8 res = self->taskFactories[0]->instantiate(
        self->taskFactories[0], guid, edtTemplate, *paramc, paramv,
        *depc, properties, affinity, outputEvent, currentEdt, parentLatch, NULL);

    ASSERT(!res);

    return 0;
}

static u8 ceCreateEdtTemplate(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                              ocrEdt_t func, u32 paramc, u32 depc, const char* funcName) {


    ocrTaskTemplate_t *base = self->taskTemplateFactories[0]->instantiate(
        self->taskTemplateFactories[0], func, paramc, depc, funcName, NULL);
    (*guid).guid = base->guid;
    (*guid).metaDataPtr = base;
    return 0;
}

static u8 ceCreateEvent(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                        ocrEventTypes_t type, u32 properties) {

    return self->eventFactories[0]->instantiate(
        self->eventFactories[0], guid, type, properties, NULL);
}

static u8 convertDepAddToSatisfy(ocrPolicyDomain_t *self, ocrFatGuid_t dbGuid,
                                   ocrFatGuid_t destGuid, u32 slot) {

    PD_MSG_STACK(msg);
    ocrTask_t *curTask = NULL;
    getCurrentEnv(NULL, NULL, &curTask, &msg);
    ocrFatGuid_t curEdt = {.guid = curTask?curTask->guid:NULL_GUID, .metaDataPtr = curTask };
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(satisfierGuid) = curEdt;
    PD_MSG_FIELD_I(guid) = destGuid;
    PD_MSG_FIELD_I(payload) = dbGuid;
    PD_MSG_FIELD_I(currentEdt) = curEdt;
    PD_MSG_FIELD_I(slot) = slot;
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_PROPAGATE(self->fcts.processMessage(self, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

#ifdef OCR_ENABLE_STATISTICS
static ocrStats_t* ceGetStats(ocrPolicyDomain_t *self) {
    return self->statsObject;
}
#endif

static u8 ceProcessResponse(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u32 properties) {
    if(!(msg->type & PD_MSG_REQ_RESPONSE)) {
        // No need to send a response in this case
        return 0;
    }

    msg->type &= ~PD_MSG_REQUEST;
    msg->type |=  PD_MSG_RESPONSE;
    if(msg->srcLocation != self->myLocation) {
        msg->destLocation = msg->srcLocation;
        msg->srcLocation = self->myLocation;

        RESULT_ASSERT(self->fcts.sendMessage(self, msg->destLocation, msg,
                                             NULL, properties), ==, 0);
    }
    return 0;
}

u8 cePolicyDomainProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {

    u8 returnCode = 0;

    ocrPolicyDomainCe_t *rself = (ocrPolicyDomainCe_t*)self;

    DPRINTF(DEBUG_LVL_INFO, "Processing message 0x%lx of type 0x%x from 0x%lx\n",
            msg, msg->type & PD_MSG_TYPE_ONLY, msg->srcLocation);

    if(msg->srcLocation != self->myLocation && rself->rlSwitch.informOtherPDs &&
       ((msg->type & PD_MSG_TYPE_ONLY) != PD_MSG_MGT_RL_NOTIFY)) {
        DPRINTF(DEBUG_LVL_VERB, "Shutdown in progress..\n");
        if(msg->type & PD_MSG_RESPONSE) {
            // Process as usual
        } else if(msg->type & PD_MSG_REQ_RESPONSE) {
            DPRINTF(DEBUG_LVL_VVERB, "Replying with a change of RL answer to message (type 0x%lx -> 0x%lx)\n",
                    msg->type, PD_MSG_MGT_RL_NOTIFY);
            // In this case, we need to just inform the other PDs that we are switching
            // RL.
            if(msg->srcLocation == self->parentLocation) {
                // If a response is required, we NEED to answer (otherwise the underlying
                // comm-platform freaks out due to the reservation of the response slot
                rself->rlSwitch.informedParent = true;
            }
#define PD_MSG msg
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
            msg->type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST | PD_MSG_RESPONSE_OVERRIDE;
            PD_MSG_FIELD_I(runlevel) = rself->rlSwitch.barrierRL;
            PD_MSG_FIELD_I(properties) = rself->rlSwitch.properties;
            PD_MSG_FIELD_I(errorCode) = self->shutdownCode; // Always safe to do
#undef PD_MSG
#undef PD_TYPE

            msg->destLocation = msg->srcLocation;
            msg->srcLocation = self->myLocation;
            RESULT_ASSERT(self->fcts.sendMessage(self, msg->destLocation, msg, NULL, 0), ==, 0);
            return returnCode;
        } else {
            DPRINTF(DEBUG_LVL_VVERB, "Message being processed because no response required (after shutdown)\n",
                    msg->srcLocation);
        }
    }


    switch((u32)(msg->type & PD_MSG_TYPE_ONLY)) {
    // Some messages are not handled at all
    case PD_MSG_DB_DESTROY:
    case PD_MSG_WORK_EXECUTE: case PD_MSG_DEP_UNREGSIGNALER:
    case PD_MSG_DEP_UNREGWAITER: case PD_MSG_DEP_DYNADD:
    case PD_MSG_DEP_DYNREMOVE:
    case PD_MSG_GUID_METADATA_CLONE:
    case PD_MSG_SAL_TERMINATE:
    case PD_MSG_MGT_REGISTER:
    case PD_MSG_MGT_UNREGISTER:
    case PD_MSG_MGT_MONITOR_PROGRESS:
    {
        DPRINTF(DEBUG_LVL_WARN, "CE PD does not handle call of type 0x%x\n",
                (u32)(msg->type & PD_MSG_TYPE_ONLY));
        ASSERT(0);
    }

    // Most messages are handled locally
    case PD_MSG_DB_CREATE: {
        START_PROFILE(pd_ce_DbCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        // BUG #584: Add properties whether DB needs to be acquired or not
        // This would impact where we do the PD_MSG_MEM_ALLOC for example
        // For now we deal with both USER and RT dbs the same way
        ASSERT((PD_MSG_FIELD_I(dbType) == USER_DBTYPE) || (PD_MSG_FIELD_I(dbType) == RUNTIME_DBTYPE));
        DPRINTF(DEBUG_LVL_VERB, "Processing DB_CREATE for size %lu\n",
                PD_MSG_FIELD_IO(size));
        // BUG #145: The prescription needs to be derived from the affinity, and needs to default to something sensible.
        u64 engineIndex = getEngineIndex(self, msg->srcLocation);
        ocrFatGuid_t edtFatGuid = {.guid = PD_MSG_FIELD_I(edt.guid), .metaDataPtr = PD_MSG_FIELD_I(edt.metaDataPtr)};
        u64 reqSize = PD_MSG_FIELD_IO(size);

        PD_MSG_FIELD_O(returnDetail) = ceAllocateDb(
            self, &(PD_MSG_FIELD_IO(guid)), &(PD_MSG_FIELD_O(ptr)), reqSize,
            PD_MSG_FIELD_IO(properties), engineIndex,
            PD_MSG_FIELD_I(affinity), PD_MSG_FIELD_I(allocator), PRESCRIPTION);
        if(PD_MSG_FIELD_O(returnDetail) == 0) {
            ocrDataBlock_t *db= PD_MSG_FIELD_IO(guid.metaDataPtr);
            ASSERT(db);

            // Set the affinity hint (TODO: Use sched notify)
            if (msg->srcLocation != self->myLocation) {
                ocrHint_t hintVar;
                RESULT_ASSERT(ocrHintInit(&hintVar, OCR_HINT_DB_T), ==, 0);
                RESULT_ASSERT(ocrSetHintValue(&hintVar, OCR_HINT_DB_MEM_AFFINITY, (u64)(msg->srcLocation)), ==, 0);
                RESULT_ASSERT(ocrSetHint(db->guid, &hintVar), ==, 0);
            }

            // BUG #584: Check if properties want DB acquired
            ASSERT(db->fctId == self->dbFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.acquire(
                db, &(PD_MSG_FIELD_O(ptr)), edtFatGuid, EDT_SLOT_NONE,
                DB_MODE_RW, !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE), (u32)DB_MODE_RW);
        } else {
            // Cannot acquire
            PD_MSG_FIELD_O(ptr) = NULL;
        }
        DPRINTF(DEBUG_LVL_VERB, "DB_CREATE response for size %lu: GUID: 0x%lx; PTR: 0x%lx)\n",
                reqSize, PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_O(ptr));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_ACQUIRE: {
        START_PROFILE(pd_ce_DbAcquire);
        // Call the appropriate acquire function
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_ACQUIRE
        if (msg->type & PD_MSG_REQUEST) {
            localDeguidify(self, &(PD_MSG_FIELD_IO(guid)), NULL);
            localDeguidify(self, &(PD_MSG_FIELD_IO(edt)), NULL);
            ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr));
            DPRINTF(DEBUG_LVL_VERB, "Processing DB_ACQUIRE request for GUID 0x%lx\n",
                    PD_MSG_FIELD_IO(guid.guid));
            ASSERT(db->fctId == self->dbFactories[0]->factoryId);
            //ASSERT(!(msg->type & PD_MSG_REQ_RESPONSE));
            PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.acquire(
                db, &(PD_MSG_FIELD_O(ptr)), PD_MSG_FIELD_IO(edt), PD_MSG_FIELD_IO(edtSlot),
                (ocrDbAccessMode_t)(PD_MSG_FIELD_IO(properties) & (u32)DB_ACCESS_MODE_MASK),
                !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE), PD_MSG_FIELD_IO(properties));
            PD_MSG_FIELD_O(size) = db->size;
            PD_MSG_FIELD_IO(properties) |= db->flags;
            // NOTE: at this point the DB may not have been acquired.
            // In that case PD_MSG_FIELD_O(returnDetail) == OCR_EBUSY
            // The XE checks for this return code.
            DPRINTF(DEBUG_LVL_VERB, "DB_ACQUIRE response for GUID 0x%lx: PTR: 0x%lx\n",
                    PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_O(ptr));
            returnCode = ceProcessResponse(self, msg, 0);
        } else {
            // This a callback response to an acquire that was pending. The CE just need
            // to call the task's dependenceResolve to iterate the task's acquire frontier
            ASSERT(msg->type & PD_MSG_RESPONSE);
            // asynchronous callback on acquire, reading response
            ocrFatGuid_t edtFGuid = PD_MSG_FIELD_IO(edt);
            ocrFatGuid_t dbFGuid = PD_MSG_FIELD_IO(guid);
            u32 edtSlot = PD_MSG_FIELD_IO(edtSlot);
            DPRINTF(DEBUG_LVL_VERB, "Processing DB_ACQUIRE response for GUID 0x%lx; resolving dependence %u for EDT 0x%lx\n",
                    dbFGuid.guid, edtSlot, edtFGuid);
            localDeguidify(self, &edtFGuid, NULL);
            // At this point the edt MUST be local as well as the db data pointer.
            ocrTask_t* task = (ocrTask_t*) edtFGuid.metaDataPtr;
            PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.dependenceResolved(task, dbFGuid.guid, PD_MSG_FIELD_O(ptr), edtSlot);
        }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_RELEASE: {
        START_PROFILE(pd_ce_DbRelease);
        // Call the appropriate release function
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_RELEASE
        localDeguidify(self, &(PD_MSG_FIELD_IO(guid)), NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(edt)), NULL);
        ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr));
        ASSERT(db->fctId == self->dbFactories[0]->factoryId);
        //ASSERT(!(msg->type & PD_MSG_REQ_RESPONSE));
        DPRINTF(DEBUG_LVL_VERB, "Processing DB_RELEASE req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_IO(guid.guid));
        PD_MSG_FIELD_O(returnDetail) =
            self->dbFactories[0]->fcts.release(db, PD_MSG_FIELD_I(edt), !!(PD_MSG_FIELD_I(properties) & DB_PROP_RT_ACQUIRE));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_FREE: {
        START_PROFILE(pd_ce_DbFree);
        // Call the appropriate free function
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_FREE
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(edt)), NULL);
        ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
        DPRINTF(DEBUG_LVL_VERB, "Processing DB_FREE req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_I(guid.guid));
        ASSERT(db->fctId == self->dbFactories[0]->factoryId);
        ASSERT(!(msg->type & PD_MSG_REQ_RESPONSE));
        PD_MSG_FIELD_O(returnDetail) =
            self->dbFactories[0]->fcts.free(db, PD_MSG_FIELD_I(edt), !!(PD_MSG_FIELD_I(properties) & DB_PROP_RT_ACQUIRE));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MEM_ALLOC: {
        START_PROFILE(pd_ce_MemAlloc);
        u64 engineIndex = getEngineIndex(self, msg->srcLocation);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MEM_ALLOC
        DPRINTF(DEBUG_LVL_VERB, "Processing MEM_ALLOC request for size %lu\n",
                PD_MSG_FIELD_I(size));
        PD_MSG_FIELD_O(returnDetail) = ceMemAlloc(
            self, &(PD_MSG_FIELD_O(allocator)), PD_MSG_FIELD_I(size),
            engineIndex, PD_MSG_FIELD_I(type), &(PD_MSG_FIELD_O(ptr)),
            PRESCRIPTION);
        PD_MSG_FIELD_O(allocatingPD) = self->fguid;
        DPRINTF(DEBUG_LVL_VERB, "MEM_ALLOC response: PTR: 0x%lx\n",
                PD_MSG_FIELD_O(ptr));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MEM_UNALLOC: {
        START_PROFILE(pd_ce_MemUnalloc);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MEM_UNALLOC
        // REC: Do we really care about the allocator returned and what not?
        ocrFatGuid_t allocGuid = {.guid = PD_MSG_FIELD_I(allocator.guid), .metaDataPtr = PD_MSG_FIELD_I(allocator.metaDataPtr) };
        DPRINTF(DEBUG_LVL_VERB, "Processing MEM_UNALLOC req/resp for PTR 0x%lx\n",
                PD_MSG_FIELD_I(ptr));
        PD_MSG_FIELD_O(returnDetail) = ceMemUnalloc(
            self, &allocGuid, PD_MSG_FIELD_I(ptr), PD_MSG_FIELD_I(type));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_WORK_CREATE: {
        START_PROFILE(pd_ce_WorkCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
        localDeguidify(self, &(PD_MSG_FIELD_I(templateGuid)), NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(affinity)), NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(currentEdt)), NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(parentLatch)), NULL);
        ocrFatGuid_t *outputEvent = NULL;
        if(PD_MSG_FIELD_IO(outputEvent.guid) == UNINITIALIZED_GUID) {
            outputEvent = &(PD_MSG_FIELD_IO(outputEvent));
        }
        ASSERT((PD_MSG_FIELD_I(workType) == EDT_USER_WORKTYPE) || (PD_MSG_FIELD_I(workType) == EDT_RT_WORKTYPE));
        DPRINTF(DEBUG_LVL_VERB, "Processing WORK_CREATE request\n");
        PD_MSG_FIELD_O(returnDetail) = ceCreateEdt(
            self, &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(templateGuid),
            &PD_MSG_FIELD_IO(paramc), PD_MSG_FIELD_I(paramv), &PD_MSG_FIELD_IO(depc),
            PD_MSG_FIELD_I(properties), PD_MSG_FIELD_I(affinity), outputEvent,
            (ocrTask_t*)(PD_MSG_FIELD_I(currentEdt).metaDataPtr), PD_MSG_FIELD_I(parentLatch));
        DPRINTF(DEBUG_LVL_VERB, "WORK_CREATE response: GUID: 0x%lx\n",
                PD_MSG_FIELD_IO(guid.guid));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_WORK_DESTROY: {
        START_PROFILE(pd_hc_WorkDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), NULL);
        ocrTask_t *task = (ocrTask_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        ASSERT(task);
        DPRINTF(DEBUG_LVL_VERB, "WORK_DESTROY req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_I(guid.guid));
        ASSERT(task->fctId == self->taskFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.destruct(task);
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EDTTEMP_CREATE: {
        START_PROFILE(pd_ce_EdtTempCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EDTTEMP_CREATE
        DPRINTF(DEBUG_LVL_VERB, "Processing EDTTEMP_CREATE request\n");
#ifdef OCR_ENABLE_EDT_NAMING
        const char* edtName = PD_MSG_FIELD_I(funcName);
#else
        const char* edtName = "";
#endif
        PD_MSG_FIELD_O(returnDetail) = ceCreateEdtTemplate(
            self, &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(funcPtr), PD_MSG_FIELD_I(paramc),
            PD_MSG_FIELD_I(depc), edtName);
        DPRINTF(DEBUG_LVL_VERB, "EDTTEMP_CREATE response: GUID: 0x%lx\n",
                PD_MSG_FIELD_IO(guid.guid));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EDTTEMP_DESTROY: {
        START_PROFILE(pd_ce_EdtTempDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), NULL);
        ocrTaskTemplate_t *tTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
        DPRINTF(DEBUG_LVL_VERB, "Processing EDTTEMP_DESTROY req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_I(guid.guid));
        ASSERT(tTemplate->fctId == self->taskTemplateFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskTemplateFactories[0]->fcts.destruct(tTemplate);
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_CREATE: {
        START_PROFILE(pd_ce_EvtCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_CREATE
        ocrEventTypes_t type = PD_MSG_FIELD_I(type);
        DPRINTF(DEBUG_LVL_VERB, "Processing EVT_CREATE request of type %u\n",
                type);

        PD_MSG_FIELD_O(returnDetail) = ceCreateEvent(self, &(PD_MSG_FIELD_IO(guid)),
                                                     type, PD_MSG_FIELD_I(properties));
        DPRINTF(DEBUG_LVL_VERB, "EVT_CREATE response for type %u: GUID: 0x%lx\n",
                type, PD_MSG_FIELD_IO(guid.guid));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_DESTROY: {
        START_PROFILE(pd_ce_EvtDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), NULL);
        ocrEvent_t *evt = (ocrEvent_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        DPRINTF(DEBUG_LVL_VERB, "Processing EVT_DESTROY req/resp for GUID 0x%lx\n",
                PD_MSG_FIELD_I(guid.guid));
        ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->fcts[evt->kind].destruct(evt);
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_GET: {
        START_PROFILE(pd_ce_EvtGet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_GET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), NULL);
        ocrEvent_t *evt = (ocrEvent_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        DPRINTF(DEBUG_LVL_VERB, "Processing EVT_GET request for GUID 0x%lx\n",
                PD_MSG_FIELD_I(guid.guid));
        ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
        PD_MSG_FIELD_O(data) = self->eventFactories[0]->fcts[evt->kind].get(evt);
        DPRINTF(DEBUG_LVL_VERB, "EVT_GET response for GUID 0x%lx: db-GUID: 0x%lx\n",
                evt->guid, PD_MSG_FIELD_O(data.guid));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_CREATE: {
        START_PROFILE(pd_ce_GuidCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_CREATE


        // The following code is kept around to show how something a bit
        // more asynchronous would look like but we need a LOT more
        // support in the runtime
        // Bug #695
        /*
        if(msg->type & PD_MSG_RESPONSE) {
            DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE response from 0x%lx destined for 0x%lx\n",
                    msg->srcLocation, msg->origSrcLocation);
            // This is a response to a request we already sent on behalf of
            // someone else (either another CE or an XE). It should never be for us
            // since we do not create labeled GUIDs ourself
            ocrLocation_t jumpTarget = msg->origSrcLocation;
            ASSERT(jumpTarget != self->myLocation);
            if(CLUSTER_FROM_ID(jumpTarget) == CLUSTER_FROM_ID(self->myLocation)) {
                // We check if we are in the same block
                if(BLOCK_FROM_ID(jumpTarget) == BLOCK_FROM_ID(self->myLocation)) {
                    DPRINTF(DEBUG_LVL_VVERB, "Responding with GUID response to my XE 0x%lx\n",
                            jumpTarget);
                } else {
                    // Forward to the proper block
                    jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(jumpTarget), BLOCK_FROM_ID(jumpTarget), ID_AGENT_CE);
                    msg->srcLocation = self->myLocation;
                    msg->destLocation = jumpTarget;
                }
            } else {
                // We need to forward to the proper cluster
                if(BLOCK_FROM_ID(self->myLocation) == 0) {
                    jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(jumpTarget), 0, ID_AGENT_CE);
                } else {
                    jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(self->myLocation), 0, ID_AGENT_CE);
                }
            }
            DPRINTF(DEBUG_LVL_VVERB, "Forwarding response to 0x%lx\n", jumpTarget);
            msg->srcLocation = self->myLocation;
            msg->destLocation = jumpTarget;
            RESULT_ASSERT(self->fcts.sendMessage(self, msg->destLocation, msg,
                                                 NULL, 0), ==, 0);
        } else {
            // This is a request
            ASSERT(msg->type & PD_MSG_REQUEST);
            if(PD_MSG_FIELD_I(size) != 0) {
                // Here we need to create a metadata area as well
                DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE(new) request from 0x%lx\n",
                        msg->srcLocation);
                if((PD_MSG_FIELD_I(properties)) & GUID_PROP_IS_LABELED) {
                    // In this case, we need to offload this to the CE that can actually
                    // process this.
                    ocrLocation_t guidLocation;
                    self->guidProviders[0]->fcts.getLocation(self->guidProviders[0],
                                                             PD_MSG_FIELD_IO(guid.guid),
                                                             &guidLocation);
                    if(guidLocation == self->myLocation) {
                        // We are all good, we can process this just fine
                        DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE(new), labeled, processed locally\n");
                        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.createGuid(
                            self->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                            PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(properties));
                        DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE response: GUID: 0x%lx\n",
                                PD_MSG_FIELD_IO(guid.guid));
                        returnCode = ceProcessResponse(self, msg, 0);
                    } else {
                        // Find who to offload-to:
                        //   - If in same cluster, offload to that (we have a direct connection
                        //   - If in different cluster, we first go to our "head" block (block 0 for
                        //   our cluster), then we jump to the target cluster (block 0 of that cluster), before
                        //   jumpting to the target block (3 hops maximum)
                        ocrLocation_t jumpTarget;
                        if(CLUSTER_FROM_ID(guidLocation) == CLUSTER_FROM_ID(self->myLocation)) {
                            jumpTarget = guidLocation;
                        } else {3
                            if(BLOCK_FROM_ID(self->myLocation) == 0) {
                                // We are the head block for this cluster
                                jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(guidLocation), 0, ID_AGENT_CE);
                            } else {
                                // Jump to our head block
                                jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(self->myLocation), 0, ID_AGENT_CE);
                            }
                        }
                        // Offload to the other CE
                        DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE(new), labeled, offloading to 0x%x; belongs to 0x%x\n",
                                jumpTarget, guidLocation);
                        if(msg->origSrcLocation == UNDEFINED_LOCATION) {
                            msg->origSrcLocation = msg->srcLocation;
                        }
                        PD_MSG_STACK(ceMsg);
                        getCurrentEnv(NULL, NULL, NULL, &ceMsg);
                        u64 baseSize = 0, marshalledSize = 0;
                        ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, 0);
                        // For now, it must fit in a single message
                        ASSERT(baseSize + marshalledSize <= sizeof(ocrPolicyMsg_t));
                        ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)&ceMsg, MARSHALL_DUPLICATE);
#undef PD_MSG
#define PD_MSG (&ceMsg)
                        ceMsg.destLocation = jumpTarget;
                        ceMsg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                        RESULT_ASSERT(self->fcts.sendMessage(
                                          self, ceMsg.destLocation, &ceMsg, NULL, 0),
                                      ==, 0);
#undef PD_MSG
#define PD_MSG msg
                    }
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE(new), non-labeled, processed locally\n");
                    PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.createGuid(
                        self->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                        PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(properties));
                    DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE response: GUID: 0x%lx\n",
                            PD_MSG_FIELD_IO(guid.guid));
                    returnCode = ceProcessResponse(self, msg, 0);
                }
            } else {
                // Here we just need to associate a GUID
                ocrGuid_t temp;
                DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE(exist) request from 0x%lx\n",
                        msg->srcLocation);
                PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getGuid(
                    self->guidProviders[0], &temp, (u64)PD_MSG_FIELD_IO(guid.metaDataPtr),
                    PD_MSG_FIELD_I(kind));
                PD_MSG_FIELD_IO(guid.guid) = temp;
                DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE response: GUID: 0x%lx\n",
                        PD_MSG_FIELD_IO(guid.guid));
                returnCode = ceProcessResponse(self, msg, 0);
            }
        }
        */
        // Currently, only requests for GUID_CREATE
        DPRINTF(DEBUG_LVL_VERB, "Processing GUID_CREATE request\n");
        ASSERT(msg->type & PD_MSG_REQUEST);
        if(PD_MSG_FIELD_I(size) != 0) {
            // Here we need to create a metadata area as well
            DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE(new) request for size %u\n",
                    PD_MSG_FIELD_I(size));
            if((PD_MSG_FIELD_I(properties)) & GUID_PROP_IS_LABELED) {
                // We need to create this using the proper CE guid provider
                ocrLocation_t guidLocation;
                self->guidProviders[0]->fcts.getLocation(self->guidProviders[0],
                                                         PD_MSG_FIELD_IO(guid.guid),
                                                         &guidLocation);
                if(guidLocation == self->myLocation || guidLocation == INVALID_LOCATION) {
                    // We are all good, we can process this just fine. INVALID_LOCATION means
                    // everyone can do this operation
                    DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (new), labeled, processed locally\n");
                    PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.createGuid(
                        self->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                        PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(properties));
                    DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (new, local) response: GUID: 0x%lx\n",
                            PD_MSG_FIELD_IO(guid.guid));
                    returnCode = ceProcessResponse(self, msg, 0);
                } else {
                    // Go and ask the other CE's provider
                    DPRINTF(DEBUG_LVL_VVERB, "Cannot create labeled GUID for 0x%lx, asking 0x%lx\n",
                        PD_MSG_FIELD_IO(guid.guid), guidLocation);
#ifdef TG_X86_TARGET
                    ocrPolicyDomain_t *otherPd = rself->allPDs[CLUSTER_FROM_ID(guidLocation)*MAX_NUM_BLOCK +
                                                               BLOCK_FROM_ID(guidLocation)*(MAX_NUM_XE+MAX_NUM_CE) +
                                                               AGENT_FROM_ID(guidLocation)];
#else
                    // Self is always relative to ourself (the CE). All PDs are at the same location
                    // relatively to the CE itself
                    ocrPolicyDomain_t *otherPd = (ocrPolicyDomain_t*)(
                        SR_L1_BASE(CLUSTER_FROM_ID(guidLocation), BLOCK_FROM_ID(guidLocation), ID_AGENT_CE) + ((u64)(self) - AR_L1_BASE));
#endif
                    // Note that this a HACK for now because we do not have asynchronous runtime
                    // execution (Bug #695). There is a risk that getCurrentEnv is called in this
                    // chain which may screw things up. So far it seems OK :).
                    ASSERT(otherPd->myLocation == guidLocation);
                    PD_MSG_FIELD_O(returnDetail) = otherPd->guidProviders[0]->fcts.createGuid(
                        otherPd->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                        PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(properties));
                    DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (new, remote) response: GUID: 0x%lx\n",
                            PD_MSG_FIELD_IO(guid.guid));
                    returnCode = ceProcessResponse(self, msg, 0);
                }
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (new), non-labeled, processed locally\n");
                PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.createGuid(
                    self->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                    PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(properties));
                DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (new) response: GUID: 0x%lx\n",
                        PD_MSG_FIELD_IO(guid.guid));
                returnCode = ceProcessResponse(self, msg, 0);
            }
        } else {
            // Here we just need to associate a GUID. This should never be labeled
            ASSERT(!(PD_MSG_FIELD_I(properties) & GUID_PROP_IS_LABELED));
            ocrGuid_t temp;
            DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (exist) for value 0x%lx\n", PD_MSG_FIELD_IO(guid.metaDataPtr));
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getGuid(
                self->guidProviders[0], &temp, (u64)PD_MSG_FIELD_IO(guid.metaDataPtr),
                PD_MSG_FIELD_I(kind));
            PD_MSG_FIELD_IO(guid.guid) = temp;
            DPRINTF(DEBUG_LVL_VVERB, "GUID_CREATE (exist) response: GUID: 0x%lx\n",
                    PD_MSG_FIELD_IO(guid.guid));
            returnCode = ceProcessResponse(self, msg, 0);
        }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_INFO: {
        START_PROFILE(pd_ce_GuidInfo);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_INFO
        // We need to resolve the GUID
        DPRINTF(DEBUG_LVL_VERB, "Processing GUID_INFO request for GUID 0x%lx\n", PD_MSG_FIELD_IO(guid.guid));
        if(self->guidProviders[0]->fcts.getVal(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid),
                                               (u64*)(&(PD_MSG_FIELD_IO(guid.metaDataPtr))), NULL)) {
            // If we get here, it means our GUID provider has no clue about
            // this GUID so we probably have to go fetch it from somewhere else
            ocrLocation_t guidLocation;
            self->guidProviders[0]->fcts.getLocation(self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid),
                                                     &guidLocation);
            // If we get an error, we should really not know about this GUID
            ASSERT(guidLocation != INVALID_LOCATION && guidLocation != self->myLocation);

            // Go and ask the other CE's provider
            DPRINTF(DEBUG_LVL_VVERB, "Cannot resolve GUID_INFO, asking 0x%lx\n",
                    guidLocation);
#ifdef TG_X86_TARGET
            ocrPolicyDomain_t *otherPd = rself->allPDs[CLUSTER_FROM_ID(guidLocation)*MAX_NUM_BLOCK +
                                                       BLOCK_FROM_ID(guidLocation)*(MAX_NUM_XE+MAX_NUM_CE) +
                                                       AGENT_FROM_ID(guidLocation)];
#else
            ocrPolicyDomain_t *otherPd = (ocrPolicyDomain_t*)(
                SR_L1_BASE(CLUSTER_FROM_ID(guidLocation), BLOCK_FROM_ID(guidLocation), ID_AGENT_CE) + (u64)(self) - AR_L1_BASE);
#endif
            ASSERT(otherPd->myLocation == guidLocation);
            RESULT_ASSERT(otherPd->guidProviders[0]->fcts.getVal(otherPd->guidProviders[0], PD_MSG_FIELD_IO(guid.guid),
                                                                 (u64*)(&(PD_MSG_FIELD_IO(guid.metaDataPtr))), NULL), ==, 0);

            if(PD_MSG_FIELD_I(properties) & KIND_GUIDPROP) {
                PD_MSG_FIELD_O(returnDetail) = otherPd->guidProviders[0]->fcts.getKind(
                    otherPd->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(kind)));
                if(PD_MSG_FIELD_O(returnDetail) == 0)
                    PD_MSG_FIELD_O(returnDetail) = KIND_GUIDPROP | WMETA_GUIDPROP | RMETA_GUIDPROP;
            } else if (PD_MSG_FIELD_I(properties) & LOCATION_GUIDPROP) {
                PD_MSG_FIELD_O(returnDetail) = otherPd->guidProviders[0]->fcts.getLocation(
                    otherPd->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(location)));
                if(PD_MSG_FIELD_O(returnDetail) == 0) {
                    PD_MSG_FIELD_O(returnDetail) = LOCATION_GUIDPROP | WMETA_GUIDPROP | RMETA_GUIDPROP;
                }
            } else {
                PD_MSG_FIELD_O(returnDetail) = WMETA_GUIDPROP | RMETA_GUIDPROP;
            }

            // The results are *not* cached because we don't know how to "un-cache" them and with
            // labeled GUIDs, it is legal to reuse GUIDs.
        } else {
            DPRINTF(DEBUG_LVL_VVERB, "GUID is locally known\n");
            if(PD_MSG_FIELD_I(properties) & KIND_GUIDPROP) {
                PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getKind(
                    self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(kind)));
                if(PD_MSG_FIELD_O(returnDetail) == 0)
                    PD_MSG_FIELD_O(returnDetail) = KIND_GUIDPROP | WMETA_GUIDPROP | RMETA_GUIDPROP;
            } else if (PD_MSG_FIELD_I(properties) & LOCATION_GUIDPROP) {
                PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getLocation(
                    self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(location)));
                if(PD_MSG_FIELD_O(returnDetail) == 0) {
                    PD_MSG_FIELD_O(returnDetail) = LOCATION_GUIDPROP | WMETA_GUIDPROP | RMETA_GUIDPROP;
                }
            } else {
                PD_MSG_FIELD_O(returnDetail) = WMETA_GUIDPROP | RMETA_GUIDPROP;
            }
        }
        DPRINTF(DEBUG_LVL_VERB, "GUID_INFO response: GUID: 0x%lx, PTR: 0x%lx\n",
                PD_MSG_FIELD_IO(guid.guid), PD_MSG_FIELD_IO(guid.metaDataPtr));
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_RESERVE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_RESERVE
        DPRINTF(DEBUG_LVL_VERB, "Processing GUID_RESERVE request for %u GUIDs of type %u\n",
                PD_MSG_FIELD_I(numberGuids), PD_MSG_FIELD_I(guidKind));
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.guidReserve(
            self->guidProviders[0], &(PD_MSG_FIELD_O(startGuid)), &(PD_MSG_FIELD_O(skipGuid)),
            PD_MSG_FIELD_I(numberGuids), PD_MSG_FIELD_I(guidKind));
        DPRINTF(DEBUG_LVL_VERB, "GUID_RESERVE response: start 0x%lx\n",
                PD_MSG_FIELD_O(startGuid));
#undef PD_MSG
#undef PD_TYPE
        returnCode = ceProcessResponse(self, msg, 0);
        break;
    }

    case PD_MSG_GUID_UNRESERVE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_UNRESERVE
        DPRINTF(DEBUG_LVL_VERB, "Processing GUID_UNRESERVE req/resp for start 0x%lx\n",
                PD_MSG_FIELD_I(startGuid));
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.guidUnreserve(
            self->guidProviders[0], PD_MSG_FIELD_I(startGuid), PD_MSG_FIELD_I(skipGuid),
            PD_MSG_FIELD_I(numberGuids));
#undef PD_MSG
#undef PD_TYPE
        returnCode = ceProcessResponse(self, msg, 0);
        break;
    }
    case PD_MSG_GUID_DESTROY: {
        START_PROFILE(pd_ce_GuidDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_DESTROY
        ocrLocation_t guidLocation;
        if(PD_MSG_FIELD_I(guid.guid) == NULL_GUID || PD_MSG_FIELD_I(guid.guid) == UNINITIALIZED_GUID
           || PD_MSG_FIELD_I(guid.guid) == ERROR_GUID) {
            // This can happen in very rare cases when the shutdown is noticed before the
            // GUIDIFY returns a valid value
            DPRINTF(DEBUG_LVL_INFO, "Trying to destroy NULL_GUID, ignoring\n");
            PD_MSG_FIELD_O(returnDetail) = 0;
        } else {
            DPRINTF(DEBUG_LVL_VERB, "Processing GUID_DESTROY for GUID 0x%lx\n", PD_MSG_FIELD_I(guid.guid));

            self->guidProviders[0]->fcts.getLocation(self->guidProviders[0], PD_MSG_FIELD_I(guid.guid),
                                                 &guidLocation);
            if(guidLocation == INVALID_LOCATION || guidLocation == self->myLocation) {
                DPRINTF(DEBUG_LVL_VVERB, "GUID_DESTROY for GUID 0x%lx can be dealt with locally\n",
                        PD_MSG_FIELD_I(guid.guid));

                PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.releaseGuid(
                    self->guidProviders[0], PD_MSG_FIELD_I(guid), PD_MSG_FIELD_I(properties) & 1);
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "Cannot process local GUID_DESTROY, asking 0x%lx\n",
                        guidLocation);
#ifdef TG_X86_TARGET
                ocrPolicyDomain_t *otherPd = rself->allPDs[CLUSTER_FROM_ID(guidLocation)*MAX_NUM_BLOCK +
                                                           BLOCK_FROM_ID(guidLocation)*(MAX_NUM_XE+MAX_NUM_CE) +
                                                           AGENT_FROM_ID(guidLocation)];
#else
                ocrPolicyDomain_t *otherPd = (ocrPolicyDomain_t*)(
                    SR_L1_BASE(CLUSTER_FROM_ID(guidLocation), BLOCK_FROM_ID(guidLocation), ID_AGENT_CE) + (u64)(self) - AR_L1_BASE);
#endif
                ASSERT(otherPd->myLocation == guidLocation);
                RESULT_ASSERT(otherPd->guidProviders[0]->fcts.releaseGuid(otherPd->guidProviders[0],
                                                                          PD_MSG_FIELD_I(guid),
                                                                          PD_MSG_FIELD_I(properties) & 1), ==, 0);
                /*
                // Same as for GUID info. Keeping here as example of what can be done
                // Remove for now because of a possible race condition between a destroy
                // and a GUID_INFO (which uses a faster path).
                // We are not allowed to deguidify this so we need to offload it
                // Find who to offload-to:
                //   - If in same cluster, offload to that (we have a direct connection
                //   - If in different cluster, we first go to our "head" block (block 0 for
                //   our cluster), then we jump to the target cluster (block 0 of that cluster), before
                //   jumpting to the target block (3 hops maximum)
                ocrLocation_t jumpTarget;
                if(CLUSTER_FROM_ID(guidLocation) == CLUSTER_FROM_ID(self->myLocation)) {
                    jumpTarget = guidLocation;
                } else {
                    if(BLOCK_FROM_ID(self->myLocation) == 0) {
                        // We are the head block for this cluster
                        jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(guidLocation), 0, ID_AGENT_CE);
                    } else {
                        // Jump to our head block
                        jumpTarget = MAKE_CORE_ID(0, 0, 0, CLUSTER_FROM_ID(self->myLocation), 0, ID_AGENT_CE);
                    }
                }
                DPRINTF(DEBUG_LVL_VVERB, "GUID_DESTROY for GUID 0x%lx will be offloaded to 0x%lx (owned by 0x%lx)\n",
                        PD_MSG_FIELD_I(guid.guid), jumpTarget, guidLocation);
                PD_MSG_STACK(ceMsg);
                getCurrentEnv(NULL, NULL, NULL, &ceMsg);
                u64 baseSize = 0, marshalledSize = 0;
                ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, 0);
                // For now, it must fit in a single message
                ASSERT(baseSize + marshalledSize <= sizeof(ocrPolicyMsg_t));
                ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)&ceMsg, MARSHALL_DUPLICATE);
#undef PD_MSG
#define PD_MSG (&ceMsg)
                ceMsg.destLocation = jumpTarget;
                ceMsg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                RESULT_ASSERT(self->fcts.sendMessage(
                                  self, ceMsg.destLocation, &ceMsg, NULL, 0),
                              ==, 0);
#undef PD_MSG
#define PD_MSG msg
                */
            }
        }
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_COMM_TAKE:
    {
        START_PROFILE(pd_ce_Take);
        // TAKE protocol on multiblock TG:
        //
        // When a XE is out of work it requests the CE for work.
        // If the CE is out of work, it pings a random neighbor CE for work.
        // So, a CE can receive a TAKE request from either a XE or another CE.
        // For a CE, the reaction to a TAKE request is currently the same,
        // whether it came from a XE or another CE, which is:
        //
        // Ask your own scheduler first.
        // If no work was found locally, start to ping other CE's for new work.
        // The protocol makes sure that an empty CE does not request back
        // to a requesting CE. It also makes sure (through local flags), that
        // it does not resend a request to another CE to which it had already
        // posted one earlier.
        //
        // This protocol may cause an exponentially growing number
        // of search messages until it fills up the system or finds work.
        // It may or may not be the fastest way to grab work, but definitely
        // not energy efficient nor communication-wise cheap. Also, who receives
        // what work is totally random.
        //
        // This implementation will serve as the initial baseline for future
        // improvements.
#define PD_MSG msg
#define PD_TYPE PD_MSG_COMM_TAKE
        ASSERT(PD_MSG_FIELD_IO(type) == OCR_GUID_EDT);
        // A TAKE request can come from either a CE or XE
        if (msg->type & PD_MSG_REQUEST) {
            DPRINTF(DEBUG_LVL_VERB, "Processing COMM_TAKE request for %u EDTs with incoming array @ 0x%lx\n",
                    PD_MSG_FIELD_IO(guidCount), PD_MSG_FIELD_IO(guids));

            // If we get a request but the CE/XE doing the requesting hasn't requested
            // any specific guids, PD_MSG_FIELD_IO(guids) will be NULL so we set something up
            ocrFatGuid_t fguid[CE_TAKE_CHUNK_SIZE] = {{0}};
            if(PD_MSG_FIELD_IO(guids) == NULL) {
                PD_MSG_FIELD_IO(guids) = &(fguid[0]);
                ASSERT(PD_MSG_FIELD_IO(guidCount) <= CE_TAKE_CHUNK_SIZE);
            }
            //First check if my own scheduler can give out work
            PD_MSG_FIELD_O(returnDetail) = self->schedulers[0]->fcts.takeEdt(
                self->schedulers[0], &(PD_MSG_FIELD_IO(guidCount)), PD_MSG_FIELD_IO(guids));

            DPRINTF(DEBUG_LVL_VVERB, "After local takeEdt, have returnDetail %u, guidCount %u\n",
                    (u32)(PD_MSG_FIELD_O(returnDetail)), PD_MSG_FIELD_IO(guidCount));
            //If my scheduler does not have work, (i.e guidCount == 0)
            //then we need to start looking for work on other CE's.
            ocrPolicyDomainCe_t * cePolicy = (ocrPolicyDomainCe_t*)self;
            // BUG #268: very basic self-throttling mechanism
            if ((PD_MSG_FIELD_IO(guidCount) == 0) &&
                (--cePolicy->nextVictimThrottle <= 0)) {

                cePolicy->nextVictimThrottle = 20*(self->neighborCount-1);
                //Try other CE's
                //Check if I already have an active work request pending on a CE
                //If not, then we post one
                if (self->neighborCount &&
                   (cePolicy->ceCommTakeActive[cePolicy->nextVictimNeighbor] == 0) &&
                   (msg->srcLocation != self->neighbors[cePolicy->nextVictimNeighbor])) {
#undef PD_MSG
#define PD_MSG (&ceMsg)
                    PD_MSG_STACK(ceMsg);
                    getCurrentEnv(NULL, NULL, NULL, &ceMsg);
                    ocrFatGuid_t fguid[CE_TAKE_CHUNK_SIZE] = {{0}};
                    ceMsg.destLocation = self->neighbors[cePolicy->nextVictimNeighbor];
                    ceMsg.type = PD_MSG_COMM_TAKE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                    PD_MSG_FIELD_IO(guids) = &(fguid[0]);
                    PD_MSG_FIELD_IO(guidCount) = CE_TAKE_CHUNK_SIZE;
                    PD_MSG_FIELD_I(properties) = 0;
                    PD_MSG_FIELD_IO(type) = OCR_GUID_EDT;
                    // Make sure to set this before because the send may get other
                    // requests in the meantime (if it can't send right away) and that
                    // may generate more takes
                    cePolicy->ceCommTakeActive[cePolicy->nextVictimNeighbor] = 1;
                    DPRINTF(DEBUG_LVL_VVERB, "Sending steal to 0x%lx\n", self->neighbors[cePolicy->nextVictimNeighbor]);
                    if(self->fcts.sendMessage(self, ceMsg.destLocation, &ceMsg, NULL, 1) != 0) {
                        // We couldn't send the steal message but it doesn't matter (we'll just
                        // get the work later). We don't insist for now. This will work better
                        // once the runtime is more asynchronous
                        DPRINTF(DEBUG_LVL_VVERB, "Steal not successful, will retry next time\n");
                        cePolicy->ceCommTakeActive[cePolicy->nextVictimNeighbor] = 0;
                        cePolicy->nextVictimThrottle = 1;
                    } else {
                        if(++cePolicy->nextVictimNeighbor >= self->neighborCount)
                            cePolicy->nextVictimNeighbor = 0;
                    }
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "Did not attempt to steal from 0x%lx\n", self->neighbors[cePolicy->nextVictimNeighbor]);
                    if(++cePolicy->nextVictimNeighbor >= self->neighborCount)
                        cePolicy->nextVictimNeighbor = 0;
                }

#undef PD_MSG
#define PD_MSG msg
            }
            // Respond to the requester
            // If my own scheduler had extra work, we respond with work
            // If not, then we respond with no work (but we've already put out work requests by now)
            DPRINTF(DEBUG_LVL_VERB, "Response for COMM_TAKE: GUID 0x%lx count: %u\n",
                    PD_MSG_FIELD_IO(guids[0].guid), PD_MSG_FIELD_IO(guidCount));
            returnCode = ceProcessResponse(self, msg, 0);
        } else { // A TAKE response has to be from another CE responding to my own request
            DPRINTF(DEBUG_LVL_VERB, "Processing COMM_TAKE response from another CE with %u EDTs\n",
                    PD_MSG_FIELD_IO(guidCount));
            ASSERT(msg->type & PD_MSG_RESPONSE);
            u32 i, j;
            ocrPolicyDomainCe_t * cePolicy = (ocrPolicyDomainCe_t*)self;
            for (i = 0; i < self->neighborCount; i++) {
                if (msg->srcLocation == self->neighbors[i]) {
                    cePolicy->ceCommTakeActive[i] = 0;
                    if(PD_MSG_FIELD_IO(guidCount) > 0) {
                        //If my work request was responded with work,
                        //then we store it in our own scheduler
                        ASSERT(PD_MSG_FIELD_IO(type) == OCR_GUID_EDT);
                        ocrFatGuid_t *fEdtGuids = PD_MSG_FIELD_IO(guids);

                        DPRINTF(DEBUG_LVL_VVERB, "Received %u EDTs: ",
                                PD_MSG_FIELD_IO(guidCount));
                        for (j = 0; j < PD_MSG_FIELD_IO(guidCount); j++) {
                            DPRINTF(DEBUG_LVL_INFO, "(guid:0x%lx metadata: %p) ", fEdtGuids[j].guid, fEdtGuids[j].metaDataPtr);
                        }
                        DPRINTF(DEBUG_LVL_INFO, "\n");

                        PD_MSG_FIELD_O(returnDetail) = self->schedulers[0]->fcts.giveEdt(
                            self->schedulers[0], &(PD_MSG_FIELD_IO(guidCount)), fEdtGuids);
                    }
                    break;
                }
            }
            ASSERT(i < self->neighborCount);
        }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_GET_WORK: {
        START_PROFILE(pd_ce_Sched_Get_Work);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_GET_WORK
        if (msg->type & PD_MSG_REQUEST) {
            u8 retVal = 0;
            ocrSchedulerOpWorkArgs_t *workArgs = &PD_MSG_FIELD_IO(schedArgs);
            workArgs->base.location = msg->srcLocation;
            retVal = self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_GET_WORK].invoke(
                         self->schedulers[0], (ocrSchedulerOpArgs_t*)workArgs, (ocrRuntimeHint_t*)msg);

            if (retVal == 0) {
                PD_MSG_FIELD_O(returnDetail) = 0;
                returnCode = ceProcessResponse(self, msg, 0);
            }
        } else {
            ASSERT(msg->type & PD_MSG_RESPONSE);
            ocrSchedulerOpWorkArgs_t *workArgs = &PD_MSG_FIELD_IO(schedArgs);
            ocrFatGuid_t fguid = workArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt;
            workArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.guid = NULL_GUID;
            workArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt.metaDataPtr = NULL;
            if (fguid.guid != NULL_GUID) {
                localDeguidify(self, &fguid, NULL);
                ocrSchedulerOpNotifyArgs_t notifyArgs;
                notifyArgs.base.location = msg->srcLocation;
                notifyArgs.kind = OCR_SCHED_NOTIFY_EDT_READY;
                notifyArgs.OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid = fguid;
                returnCode = self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
                        self->schedulers[0], (ocrSchedulerOpArgs_t*)(&notifyArgs), NULL);
            } else {
                // ASSERT(0); //HACK: We are probably in shutdown. Ignore!
            }
        }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_NOTIFY: {
        DPRINTF(DEBUG_LVL_VERB, "Processing req/resp SCHED_NOTIFY\n");
        START_PROFILE(pd_ce_Sched_Notify);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_NOTIFY
        ocrSchedulerOpNotifyArgs_t *notifyArgs = &PD_MSG_FIELD_IO(schedArgs);
        notifyArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)notifyArgs, NULL);
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_UPDATE: {
        START_PROFILE(pd_ce_Sched_Update);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_UPDATE
        ASSERT(msg->srcLocation == self->myLocation);
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.update(
                self->schedulers[0], PD_MSG_FIELD_I(properties));
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_ADD: {
        START_PROFILE(pd_ce_DepAdd);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_ADD
        DPRINTF(DEBUG_LVL_VERB, "Processing DEP_ADD req/resp for 0x%lx -> 0x%lx\n",
                msg->srcLocation, PD_MSG_FIELD_I(source.guid), PD_MSG_FIELD_I(dest.guid));
        // We first get information about the source and destination
        ocrGuidKind srcKind, dstKind;
        localDeguidify(self, &(PD_MSG_FIELD_I(source)), &srcKind);
        localDeguidify(self, &(PD_MSG_FIELD_I(dest)), &dstKind);

        ocrFatGuid_t src = PD_MSG_FIELD_I(source);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        ocrDbAccessMode_t mode = (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK);
        if (srcKind == NULL_GUID) {
            DPRINTF(DEBUG_LVL_VVERB, "Src is NULL, transforming to satisfy\n");
            //NOTE: Handle 'NULL_GUID' case here to be safe although
            //we've already caught it in ocrAddDependence for performance
            // This is equivalent to an immediate satisfy
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, PD_MSG_FIELD_I(slot));
        } else if (srcKind == OCR_GUID_DB) {
            if(dstKind & OCR_GUID_EVENT) {
                DPRINTF(DEBUG_LVL_VVERB, "DB -> EVT: converting to satisfy\n");
                // This is equivalent to an immediate satisfy
                PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                    self, src, dest, PD_MSG_FIELD_I(slot));
            } else {
                // NOTE: We could use convertDepAddToSatisfy since adding a DB dependence
                // is equivalent to satisfy. However, we want to go through the register
                // function to make sure the access mode is recorded.
                DPRINTF(DEBUG_LVL_VVERB, "DB -> EDT\n");
                ASSERT(dstKind == OCR_GUID_EDT);
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
                ocrFatGuid_t sourceGuid = PD_MSG_FIELD_I(source);
                ocrFatGuid_t destGuid = PD_MSG_FIELD_I(dest);
                u32 slot = PD_MSG_FIELD_I(slot);
#undef PD_MSG
#undef PD_TYPE
#define PD_MSG (&registerMsg)
#define PD_TYPE PD_MSG_DEP_REGSIGNALER
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST;
                // Registers sourceGuid (signaler) onto destGuid
                PD_MSG_FIELD_I(signaler) = sourceGuid;
                PD_MSG_FIELD_I(dest) = destGuid;
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(mode) = mode;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                RESULT_PROPAGATE(self->fcts.processMessage(self, &registerMsg, true));
#undef PD_MSG
#undef PD_TYPE
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_ADD
            }
        } else {
            // Is it ok not to use messages here ?

            if (dstKind == OCR_GUID_EDT) {
                DPRINTF(DEBUG_LVL_VVERB, "EVT -> EDT; registering the event as a signaler of the EDT\n");
                ocrTask_t *task = (ocrTask_t*)(dest.metaDataPtr);
                ASSERT(task->fctId == self->taskFactories[0]->factoryId);
                self->taskFactories[0]->fcts.registerSignaler(task, src, PD_MSG_FIELD_I(slot),
                    PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK, true);
            }
            bool srcIsNonPersistent = ((srcKind == OCR_GUID_EVENT_ONCE) ||
                                        (srcKind == OCR_GUID_EVENT_LATCH));
            if (srcIsNonPersistent || (dstKind & OCR_GUID_EVENT)) {
                DPRINTF(DEBUG_LVL_VVERB, "Non persistent source or destination event, registering destination as waiter on event\n");
                ocrEvent_t *evt = (ocrEvent_t*)(src.metaDataPtr);
                ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
                self->eventFactories[0]->fcts[evt->kind].registerWaiter(
                    evt, dest, PD_MSG_FIELD_I(slot), true);
            }
        }

#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_REGSIGNALER: {
        START_PROFILE(pd_ce_DepRegSignaler);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_REGSIGNALER
        // We first get information about the signaler and destination
        DPRINTF(DEBUG_LVL_VERB, "Processing DEP_REGSIGNALER req/resp on 0x%lx: signaler 0x%lx, slot is %u\n",
                PD_MSG_FIELD_I(dest.guid),  PD_MSG_FIELD_I(signaler.guid), PD_MSG_FIELD_I(slot));
        ocrGuidKind signalerKind, dstKind;
        localDeguidify(self, &(PD_MSG_FIELD_I(signaler)), &signalerKind);
        localDeguidify(self, &(PD_MSG_FIELD_I(dest)), &dstKind);

        ocrFatGuid_t signaler = PD_MSG_FIELD_I(signaler);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        bool isAddDep = PD_MSG_FIELD_I(properties);

        if (dstKind & OCR_GUID_EVENT) {
            ocrEvent_t *evt = (ocrEvent_t*)(dest.metaDataPtr);
            ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
            self->eventFactories[0]->fcts[evt->kind].registerSignaler(
                evt, signaler, PD_MSG_FIELD_I(slot), PD_MSG_FIELD_I(mode), isAddDep);
        } else if(dstKind == OCR_GUID_EDT) {
            ocrTask_t *edt = (ocrTask_t*)(dest.metaDataPtr);
            ASSERT(edt->fctId == self->taskFactories[0]->factoryId);
            self->taskFactories[0]->fcts.registerSignaler(
                edt, signaler, PD_MSG_FIELD_I(slot), PD_MSG_FIELD_I(mode), isAddDep);
        } else {
            ASSERT(0); // No other things we can register signalers on
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_REGWAITER: {
        START_PROFILE(pd_ce_DepRegWaiter);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_REGWAITER
        // We first get information about the signaler and destination
        DPRINTF(DEBUG_LVL_VERB, "Processing DEP_REGWAITER req/resp on 0x%lx; waiter is 0x%lx, slot is %u\n",
                PD_MSG_FIELD_I(dest.guid), PD_MSG_FIELD_I(waiter.guid), PD_MSG_FIELD_I(slot));
        ocrGuidKind waiterKind, dstKind;
        localDeguidify(self, &(PD_MSG_FIELD_I(waiter)), &waiterKind);
        localDeguidify(self, &(PD_MSG_FIELD_I(dest)), &dstKind);

        ocrFatGuid_t waiter = PD_MSG_FIELD_I(waiter);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);

        ASSERT(dstKind & OCR_GUID_EVENT); // Waiters can only wait on events
        ocrEvent_t *evt = (ocrEvent_t*)(dest.metaDataPtr);
        ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
        self->eventFactories[0]->fcts[evt->kind].registerWaiter(
            evt, waiter, PD_MSG_FIELD_I(slot), false);
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_SATISFY: {
        START_PROFILE(pd_ce_DepSatisfy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_SATISFY
        DPRINTF(DEBUG_LVL_VERB, "Processing DEP_SATISFY req/resp for event/edt 0x%lx with db 0x%lx for slot %u\n",
                PD_MSG_FIELD_I(guid.guid), PD_MSG_FIELD_I(payload.guid), PD_MSG_FIELD_I(slot));
        ocrGuidKind dstKind;
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), &dstKind);

        ocrFatGuid_t dst = PD_MSG_FIELD_I(guid);
        if(dstKind & OCR_GUID_EVENT) {
            DPRINTF(DEBUG_LVL_VVERB, "Destination is an event\n");
            ocrEvent_t *evt = (ocrEvent_t*)(dst.metaDataPtr);
            ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
            self->eventFactories[0]->fcts[evt->kind].satisfy(
                evt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot));
        } else {
            if(dstKind == OCR_GUID_EDT) {
                DPRINTF(DEBUG_LVL_VVERB, "Destination is an EDT\n");
                ocrTask_t *edt = (ocrTask_t*)(dst.metaDataPtr);
                ASSERT(edt->fctId == self->taskFactories[0]->factoryId);
                self->taskFactories[0]->fcts.satisfy(
                    edt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot));
            } else {
                ASSERT(0); // We can't satisfy anything else
            }
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_HINT_SET: {
        START_PROFILE(pd_ce_HintSet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_HINT_SET
        ocrGuidKind kind = OCR_GUID_NONE;
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), &kind);
        switch(PD_MSG_FIELD_I(hint.type)) {
        case OCR_HINT_EDT_T:
            {
                if (kind == OCR_GUID_EDT_TEMPLATE) {
                    ocrTaskTemplate_t* taskTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskTemplateFactories[0]->fcts.setHint(taskTemplate, &(PD_MSG_FIELD_I(hint)));
                } else {
                    ASSERT(kind == OCR_GUID_EDT);
                    ocrTask_t *task = (ocrTask_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.setHint(task, &(PD_MSG_FIELD_I(hint)));
                }
            }
            break;
        case OCR_HINT_DB_T:
            {
                ASSERT(kind == OCR_GUID_DB);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.setHint(db, &(PD_MSG_FIELD_I(hint)));
            }
            break;
        case OCR_HINT_EVT_T:
            {
                ASSERT(kind & OCR_GUID_EVENT);
                ocrEvent_t *evt = (ocrEvent_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->commonFcts.setHint(evt, &(PD_MSG_FIELD_I(hint)));
            }
            break;
        case OCR_HINT_GROUP_T:
        default:
            ASSERT(0);
            PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            break;
        }
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_HINT_GET: {
        START_PROFILE(pd_hc_HintGet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_HINT_GET
        ocrGuidKind kind = OCR_GUID_NONE;
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)), &kind);
        switch(PD_MSG_FIELD_IO(hint.type)) {
        case OCR_HINT_EDT_T:
            {
                if (kind == OCR_GUID_EDT_TEMPLATE) {
                    ocrTaskTemplate_t* taskTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskTemplateFactories[0]->fcts.getHint(taskTemplate, &(PD_MSG_FIELD_IO(hint)));
                } else {
                    ASSERT(kind == OCR_GUID_EDT);
                    ocrTask_t *task = (ocrTask_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.getHint(task, &(PD_MSG_FIELD_IO(hint)));
                }
            }
            break;
        case OCR_HINT_DB_T:
            {
                ASSERT(kind == OCR_GUID_DB);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.getHint(db, &(PD_MSG_FIELD_IO(hint)));
            }
            break;
        case OCR_HINT_EVT_T:
            {
                ASSERT(kind & OCR_GUID_EVENT);
                ocrEvent_t *evt = (ocrEvent_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->commonFcts.getHint(evt, &(PD_MSG_FIELD_IO(hint)));
            }
            break;
        case OCR_HINT_GROUP_T:
        default:
            ASSERT(0);
            PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            break;
        }
        returnCode = ceProcessResponse(self, msg, 0);
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MGT_RL_NOTIFY:
    {
        START_PROFILE(pd_mgt_notify);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
        DPRINTF(DEBUG_LVL_VERB, "Received RL_NOTIFY from 0x%lx with properties 0x%x\n",
                msg->srcLocation, PD_MSG_FIELD_I(properties));
        if(PD_MSG_FIELD_I(properties) & RL_FROM_MSG) {
            ocrPolicyDomainCe_t * cePolicy = (ocrPolicyDomainCe_t*)self;
            // This is a request to change a runlevel
            // or answer that we can proceed past the runlevel change
            // We check who it is coming from. It should either be from one
            // of our children or our parent
            if(msg->srcLocation == self->parentLocation) {
                if(PD_MSG_FIELD_I(properties) & RL_RELEASE) {
                    DPRINTF(DEBUG_LVL_VERB, "RL_NOTIFY from parent releasing me\n");
                    // This is a relase from the CE
                    // Check that we match on the runlevel
                    ASSERT(PD_MSG_FIELD_I(runlevel) == cePolicy->rlSwitch.barrierRL);
                    ASSERT(cePolicy->rlSwitch.barrierState == RL_BARRIER_STATE_PARENT_NOTIFIED);
                    cePolicy->rlSwitch.barrierState = RL_BARRIER_STATE_PARENT_RESPONSE;
                } else {
                    DPRINTF(DEBUG_LVL_VERB, "RL_NOTIFY from parent requesting change to RL %u\n",
                            PD_MSG_FIELD_I(runlevel));
                    // This is a request for a change of runlevel
                    // (as an answer to some other message we sent)
                    ASSERT(PD_MSG_FIELD_I(runlevel) == cePolicy->rlSwitch.barrierRL);
                    if(cePolicy->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT) {
                        if(PD_MSG_FIELD_I(runlevel) == RL_USER_OK &&
                           (PD_MSG_FIELD_I(properties) & RL_TEAR_DOWN)) {
                            self->shutdownCode = PD_MSG_FIELD_I(errorCode);
                            //Release pending responses from the scheduler
                            RESULT_ASSERT(self->schedulers[0]->fcts.update(self->schedulers[0], OCR_SCHEDULER_UPDATE_PROP_SHUTDOWN), ==, 0);
                        }
                        RESULT_ASSERT(self->fcts.switchRunlevel(
                                          self, PD_MSG_FIELD_I(runlevel),
                                          PD_MSG_FIELD_I(properties) | cePolicy->rlSwitch.pdStatus), ==, 0);
                    } else {
                        // We ignore. A message we sent to the parent may have crossed
                        // our informing the parent of a shutdown
                        DPRINTF(DEBUG_LVL_VERB, "IGNORE 0: runlevel: %u, properties: %u, rlSwitch.barrierState: %u, rlSwitch.barrierRL: %u, rlSwitch.oldBarrierRL: %u\n",
                                PD_MSG_FIELD_I(runlevel), PD_MSG_FIELD_I(properties), cePolicy->rlSwitch.barrierState, cePolicy->rlSwitch.barrierRL,
                                cePolicy->rlSwitch.oldBarrierRL);
                    }
                }
            } else if(isChildLocation(self, msg->srcLocation)) {
                // If condition checks:
                //   - the barrier has not yet been reached (ie: the CE has never been informed of the switch in RL
                //   - it's the RL_USER_OK runlevel and teardown (these two conditions check that this is the
                //     initial shutdown message
                if(cePolicy->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT &&
                   (PD_MSG_FIELD_I(runlevel) == RL_USER_OK) && (PD_MSG_FIELD_I(properties) & RL_TEAR_DOWN)) {
                    DPRINTF(DEBUG_LVL_VERB, "RL_NOTIFY: initial shutdown notification with code %u\n",
                            PD_MSG_FIELD_I(errorCode));
                    ASSERT(cePolicy->rlSwitch.barrierRL == RL_USER_OK);
                    ASSERT(cePolicy->rlSwitch.checkedIn == 0); // No other XE or child CE should have notified us
                    if(!(PD_MSG_FIELD_I(properties) & RL_RESPONSE)) {
                        // If this was not a response, it means it was an answer by another CE and so
                        // we will get another message from that CE when it has really reached the barrier
                        ASSERT(AGENT_FROM_ID(msg->srcLocation) == ID_AGENT_CE);
                        // The CE informs first but we need to wait for
                        // its children so we don't have it check-in first
                        cePolicy->rlSwitch.checkedIn = 0;
                    } else {
                        // XEs or CEs that "respond" to the barrier are fully checked in
                        cePolicy->rlSwitch.checkedIn = 1;
                    }
                    // Record the shudown code
                    self->shutdownCode = PD_MSG_FIELD_I(errorCode);

                    //Release pending responses from the scheduler
                    RESULT_ASSERT(self->schedulers[0]->fcts.update(self->schedulers[0], OCR_SCHEDULER_UPDATE_PROP_SHUTDOWN), ==, 0);
                    self->fcts.switchRunlevel(self, RL_USER_OK, RL_TEAR_DOWN | RL_REQUEST | RL_BARRIER | RL_FROM_MSG | cePolicy->rlSwitch.pdStatus);
                    // This will cause the worker to switch out of USER_OK and, on its next
                    // loop iteration, will drop back out in the switchRunlevel function (in RL_USER_OK
                    // bring up).
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "RL_NOTIFY: child check in (0x%lx), count was %d\n",
                            msg->srcLocation, cePolicy->rlSwitch.checkedIn);
                    // Hard to check for the correct RL match without a race and/or locks
                    // We just check for the barrierState
                    ASSERT(cePolicy->rlSwitch.barrierState <= RL_BARRIER_STATE_CHILD_WAIT);
                    // We strip out the case where a child CE informs us of a shutdown
                    // after we know about it from another (non child) CE.
                    if(PD_MSG_FIELD_I(properties) & RL_RESPONSE)
                        ++(cePolicy->rlSwitch.checkedIn);
                }
            } else {
                // This could be an answer from a CE which knows about shutdown
                // If we know about shutdown, we ignore, otherwise, we inform ourself on
                // the shutdown
                if(cePolicy->rlSwitch.barrierState == RL_BARRIER_STATE_UNINIT &&
                   (PD_MSG_FIELD_I(runlevel) == RL_USER_OK) && (PD_MSG_FIELD_I(properties) & RL_TEAR_DOWN)) {
                    DPRINTF(DEBUG_LVL_VERB, "RL_NOTIFY: initial shutdown notification from non-child with code %u\n",
                            PD_MSG_FIELD_I(errorCode));
                    self->shutdownCode = PD_MSG_FIELD_I(errorCode);
                    ASSERT(cePolicy->rlSwitch.barrierRL == RL_USER_OK);
                    ASSERT(cePolicy->rlSwitch.checkedIn == 0); // No other XE or child CE should have notified us
                    // Not our child so we stay at 0
                    cePolicy->rlSwitch.checkedIn = 0;

                    //Release pending responses from the scheduler
                    RESULT_ASSERT(self->schedulers[0]->fcts.update(self->schedulers[0], OCR_SCHEDULER_UPDATE_PROP_SHUTDOWN), ==, 0);
                    self->fcts.switchRunlevel(self, RL_USER_OK, RL_TEAR_DOWN | RL_REQUEST | RL_BARRIER | RL_FROM_MSG | cePolicy->rlSwitch.pdStatus);
                    // This will cause the worker to switch out of USER_OK and, on its next
                    // loop iteration, will drop back out in the switchRunlevel function (in RL_USER_OK
                    // bring up).
                } else {
                    // else ignore
                    DPRINTF(DEBUG_LVL_INFO, "IGNORE 1: runlevel: %u, properties: %u, rlSwitch.barrierState: %u, rlSwitch.barrierRL: %u, rlSwitch.oldBarrierRL: %u\n",
                            PD_MSG_FIELD_I(runlevel), PD_MSG_FIELD_I(properties), cePolicy->rlSwitch.barrierState, cePolicy->rlSwitch.barrierRL,
                            cePolicy->rlSwitch.oldBarrierRL);
                }
            }
        } else {
            // The CE should not directly receive shutdown messages
            // so RL_FROM_MSG should always be set
            ASSERT(0);
        }
        // ocrPolicyDomainCe_t * cePolicy = (ocrPolicyDomainCe_t *)self;
        // if (cePolicy->shutdownMode == false) {
        //     cePolicy->shutdownMode = true;
        // }
        // if (msg->srcLocation == self->myLocation) {
        //     msg->destLocation = self->parentLocation;
        //     if (self->myLocation != self->parentLocation)
        //         while(self->fcts.sendMessage(self, msg->destLocation, msg, NULL, BLOCKING_SEND_MSG_PROP)) hal_pause();
        //     self->fcts.switchRunlevel(self, RL_USER_OK, 0);
        // } else {
        //     if (msg->type & PD_MSG_REQUEST) {
        //         cePolicy->shutdownCount++;
        //         // This triggers the shutdown of the machine
        //         ASSERT(!(msg->type & PD_MSG_RESPONSE));
        //         ASSERT(msg->srcLocation != self->myLocation);
        //         if (self->shutdownCode == 0)
        //             self->shutdownCode = PD_MSG_FIELD_I(errorCode);
        //         DPRINTF (DEBUG_LVL_INFO, "MSG_SHUTDOWN REQ %lx from Agent 0x%lx; shutdown %u/%u\n", msg->msgId,
        //             msg->srcLocation, cePolicy->shutdownCount, cePolicy->shutdownMax);
        //     } else {
        //         ASSERT(msg->type & PD_MSG_RESPONSE);
        //         DPRINTF (DEBUG_LVL_INFO, "MSG_SHUTDOWN RESP %lx from Agent 0x%lx; shutdown %u/%u\n", msg->msgId,
        //             msg->srcLocation, cePolicy->shutdownCount, cePolicy->shutdownMax);
        //     }
        // }
        // if (cePolicy->shutdownCount == cePolicy->shutdownMax) {
        //     self->workers[0]->fcts.switchRunlevel(self->workers[0], self, RL_USER_OK,
        //                                          0, RL_REQUEST | RL_BARRIER, NULL, 0);
        //     cePolicy->shutdownCount++;
        //     ocrShutdown();
        // }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SAL_PRINT: {
        DPRINTF(DEBUG_LVL_WARN, "CE PD does not yet implement PRINT (FIXME)\n");
        ASSERT(0);
    }

    case PD_MSG_SAL_READ: {
        DPRINTF(DEBUG_LVL_WARN, "CE PD does not yet implement READ (FIXME)\n");
        ASSERT(0);
    }

    case PD_MSG_SAL_WRITE: {
        DPRINTF(DEBUG_LVL_WARN, "CE PD does not yet implement WRITE (FIXME)\n");
        ASSERT(0);
    }

    default:
        // Not handled
        DPRINTF(DEBUG_LVL_WARN, "Unknown message type 0x%x\n",
                msg->type & PD_MSG_TYPE_ONLY);
        ASSERT(0);
    }
    DPRINTF(DEBUG_LVL_INFO, "Done processing message 0x%lx\n", msg);
    return returnCode;
}

u8 cePdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {
    ocrMsgHandle_t thandle;
    ocrMsgHandle_t *pthandle = &thandle;
    // We hijack properties here. If it's 1, we don't try to resend multiple times
    // (ie: it's OK if the message does not get sent)
    u8 status;
    u8 sendMultiple = (properties == 0);
    properties = properties & ~0x1;

    // For responses to XEs about work, we deguidify here because the XEs can't
    // always deguidify (so we make their life easy
    if(AGENT_FROM_ID(target) != ID_AGENT_CE) {
        u32 type = message->type & PD_MSG_TYPE_ONLY;
#define PD_MSG message
        switch(type) {
        case PD_MSG_COMM_TAKE:
        {
#define PD_TYPE PD_MSG_COMM_TAKE
            u32 i = 0;
            for(i = 0; i < PD_MSG_FIELD_IO(guidCount); ++i) {
                localDeguidify(self, &(PD_MSG_FIELD_IO(guids[i])), NULL);
            }
#undef PD_TYPE
            break;
        }
        case PD_MSG_SCHED_GET_WORK:
        {
#define PD_TYPE PD_MSG_SCHED_GET_WORK
            if(PD_MSG_FIELD_IO(schedArgs.kind) == OCR_SCHED_WORK_EDT_USER) {
                localDeguidify(self, &(PD_MSG_FIELD_IO(schedArgs.OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt)), NULL);
            }
#undef PD_TYPE
            break;
        }
        default:
            ;
        }
#undef PD_MSG
    }
    status = self->commApis[0]->fcts.sendMessage(self->commApis[0], target, message,
                                                 handle, properties);
    while(sendMultiple && status != 0) {
        self->commApis[0]->fcts.initHandle(self->commApis[0], pthandle);
        status = self->fcts.pollMessage(self, &pthandle);
        if(status == 0 || status == POLL_MORE_MESSAGE) {
            // We never use persistent messages so this should always be NULL
            ASSERT(pthandle->msg == NULL);
            ASSERT(pthandle == &thandle);
            RESULT_ASSERT(self->fcts.processMessage(self, pthandle->response, true), ==, 0);
            pthandle->destruct(pthandle);
        }
        status = self->commApis[0]->fcts.sendMessage(self->commApis[0], target, message,
                                                     handle, properties);
    }
    return status;
}

u8 cePdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
    return self->commApis[0]->fcts.pollMessage(self->commApis[0], handle);
}

u8 cePdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
    return self->commApis[0]->fcts.waitMessage(self->commApis[0], handle);
}

void* cePdMalloc(ocrPolicyDomain_t *self, u64 size) {
    void* result;
    u64 engineIndex = getEngineIndex(self, self->myLocation);
    u64 prescription = PRESCRIPTION;
    u64 allocatorHints;  // Allocator hint
    u64 levelIndex; // "Level" of the memory hierarchy, taken from the "prescription" to pick from the allocators array the allocator to try.
    s8 allocatorIndex;
    do {
        allocatorHints = (prescription & PRESCRIPTION_HINT_MASK) ?
            (OCR_ALLOC_HINT_NONE) :              // If the hint is non-zero, pick this (e.g. for tlsf, tries "remnant" pool.)
            (OCR_ALLOC_HINT_REDUCE_CONTENTION);  // If the hint is zero, pick this (e.g. for tlsf, tries a round-robin-chosen "slice" pool.)
        prescription >>= PRESCRIPTION_HINT_NUMBITS;
        levelIndex = prescription & PRESCRIPTION_LEVEL_MASK;
        prescription >>= PRESCRIPTION_LEVEL_NUMBITS;
        allocatorIndex = self->allocatorIndexLookup[engineIndex*NUM_MEM_LEVELS_SUPPORTED+levelIndex]; // Lookup index of allocator to use for requesting engine (aka agent) at prescribed memory hierarchy level.
        if ((allocatorIndex < 0) ||
            (allocatorIndex >= self->allocatorCount) ||
            (self->allocators[allocatorIndex] == NULL)) continue;  // Skip this allocator if it doesn't exist.
        result = self->allocators[allocatorIndex]->fcts.allocate(self->allocators[allocatorIndex], size, allocatorHints);
        if (result) {
            return result;
        }
    } while (prescription != 0);

    DPRINTF(DEBUG_LVL_WARN, "cePdMalloc returning NULL for size %ld\n", (u64) size);
    return NULL;
}

void cePdFree(ocrPolicyDomain_t *self, void* addr) {
    allocatorFreeFunction(addr);
}

ocrPolicyDomain_t * newPolicyDomainCe(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrParamList_t *perInstance) {

    ocrPolicyDomainCe_t * derived = (ocrPolicyDomainCe_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainCe_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;

    ASSERT(base);

#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, base, statsObject, perInstance);
#else
    factory->initialize(factory, base, perInstance);
#endif

    return base;
}

void initializePolicyDomainCe(ocrPolicyDomainFactory_t * factory, ocrPolicyDomain_t* self,
#ifdef OCR_ENABLE_STATISTICS
                              ocrStats_t *statObject,
#endif
                              ocrParamList_t *perInstance) {
    u64 i;
#ifdef OCR_ENABLE_STATISTICS
    self->statsObject = statsObject;
#endif

    initializePolicyDomainOcr(factory, self, perInstance);
    self->neighborCount = ((paramListPolicyDomainCeInst_t*)perInstance)->neighborCount;
    ocrPolicyDomainCe_t* derived = (ocrPolicyDomainCe_t*) self;
    //TODO by convention this should be set to NULL and initialized in one of the runlevel
    derived->xeCount = ((paramListPolicyDomainCeInst_t*)perInstance)->xeCount;
    self->allocatorIndexLookup = (s8 *) runtimeChunkAlloc((derived->xeCount+1) * NUM_MEM_LEVELS_SUPPORTED, PERSISTENT_CHUNK);
    for (i = 0; i < ((derived->xeCount+1) * NUM_MEM_LEVELS_SUPPORTED); i++) {
        self->allocatorIndexLookup[i] = -1;
    }
    //TODO by convention this should be set to NULL and initialized in one of the runlevel
    derived->ceCommTakeActive = (bool*)runtimeChunkAlloc(sizeof(bool) *  self->neighborCount, PERSISTENT_CHUNK);

    // Initialize the runlevel switch structures
    derived->rlSwitch.checkedInCount = 0; // We be initialized later
    derived->rlSwitch.checkedIn = 0;
    derived->rlSwitch.properties = 0;
    derived->rlSwitch.barrierRL = RL_GUID_OK; // First barrier that we have to do
    derived->rlSwitch.barrierState = RL_BARRIER_STATE_UNINIT;
    derived->rlSwitch.informOtherPDs = false;
    derived->rlSwitch.informedParent = false;

    derived->nextVictimNeighbor = 0;
    derived->nextVictimThrottle = 0;
}

static void destructPolicyDomainFactoryCe(ocrPolicyDomainFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrPolicyDomainFactory_t * newPolicyDomainFactoryCe(ocrParamList_t *perType) {
    ocrPolicyDomainFactory_t* base = (ocrPolicyDomainFactory_t*) runtimeChunkAlloc(sizeof(ocrPolicyDomainFactoryCe_t), NONPERSISTENT_CHUNK);
    ASSERT(base); // Check allocation

    // Set factory's methods
#ifdef OCR_ENABLE_STATISTICS
    base->instantiate = FUNC_ADDR(ocrPolicyDomain_t*(*)(ocrPolicyDomainFactory_t*,ocrStats_t*,
                                  ocrCost_t *,ocrParamList_t*), newPolicyDomainCe);
    base->initialize = FUNC_ADDR(void(*)(ocrPolicyDomainFactory_t*,ocrPolicyDomain_t*,
                                         ocrStats_t*,ocrCost_t *,ocrParamList_t*), initializePolicyDomainCe);
#endif
    base->instantiate = &newPolicyDomainCe;
    base->initialize = &initializePolicyDomainCe;
    base->destruct = &destructPolicyDomainFactoryCe;

    // Set future PDs' instance  methods
    base->policyDomainFcts.destruct = FUNC_ADDR(void(*)(ocrPolicyDomain_t*), cePolicyDomainDestruct);
    base->policyDomainFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrRunlevel_t, u32), cePdSwitchRunlevel);
    base->policyDomainFcts.processMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*,ocrPolicyMsg_t*,u8), cePolicyDomainProcessMessage);

    base->policyDomainFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrLocation_t,
                                                          ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                                   cePdSendMessage);
    base->policyDomainFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), cePdPollMessage);
    base->policyDomainFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), cePdWaitMessage);

    base->policyDomainFcts.pdMalloc = FUNC_ADDR(void*(*)(ocrPolicyDomain_t*,u64), cePdMalloc);
    base->policyDomainFcts.pdFree = FUNC_ADDR(void(*)(ocrPolicyDomain_t*,void*), cePdFree);
#ifdef OCR_ENABLE_STATISTICS
    base->policyDomainFcts.getStats = FUNC_ADDR(ocrStats_t*(*)(ocrPolicyDomain_t*),ceGetStats);
#endif

    return base;
}

#endif /* ENABLE_POLICY_DOMAIN_CE */
