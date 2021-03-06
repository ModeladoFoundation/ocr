/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_HC

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-db.h"
#include "extensions/ocr-hints.h"
#include "ocr-policy-domain.h"
#include "ocr-policy-domain-tasks.h"
#include "ocr-sysboot.h"
#include "ocr-runtime-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#ifdef ENABLE_EXTENSION_LABELING
#include "experimental/ocr-labeling-runtime.h"
#endif

#include "utils/profiler/profiler.h"
#include "utils/ocr-utils.h"

#include "policy-domain/hc/hc-policy.h"
#include "allocator/allocator-all.h"

//BUG #204: cloning: hack to support edt templates, and pause\resume
#include "task/hc/hc-task.h"
#include "event/hc/hc-event.h"
#include "workpile/hc/hc-workpile.h"

#ifdef ENABLE_RESILIENCY
#include "worker/hc/hc-worker.h"
#endif

#ifdef SHOW_BINDING_INFO
#include <unistd.h> // For gethostname
#include "ocr-comp-platform.h"
#include "comp-platform/platform-binding-info.h"
#endif

// Currently required to find out if self is the blessed PD
#include "extensions/ocr-affinity.h"
#include "ocr-errors.h"

#define DEBUG_TYPE POLICY

#define DBG_LVL_MDEVT   DEBUG_LVL_VERB

extern ocrObjectFactory_t * resolveObjectFactory(ocrPolicyDomain_t *pd, ocrGuidKind kind);

#ifdef SHOW_BINDING_INFO
static void printBindingInfo(ocrPolicyDomain_t * pd) {
    char hostname[256];
    gethostname(hostname,255);
    printf("[PD=%"PRIu64",host=%s,binding=", pd->myLocation, hostname);
    u64 i = 0;
    u64 j = 0;
    u64 k = 0;
    u64 ubi = pd->workerCount;
    if (ubi > 0) {
        s32 min = 0;
        s32 max = 0;
        while (i < ubi) {
            ocrWorker_t * worker = pd->workers[i];
            j = 0;
            u64 ubj = worker->computeCount;
            while (j < ubj) {
                ocrCompTarget_t * target = worker->computes[j];
                k = 0;
                u64 ubk = target->platformCount;
                while (k < ubk) {
                    ocrCompPlatform_t * platform = target->platforms[k];
                    bindingInfo_t bindingInfo;
                    u8 res = getCompPlatformBindingInfo(platform, &bindingInfo);
                    if (!res) { // binding info available
                        s32 offset = bindingInfo.offset;
                        if ((i | j | k) == 0) {
                            min = offset; max = offset;
                        } else {
                            // Detect last printout to correctly format the output
                            if ((i == (ubi-1)) && (j == (ubj-1)) && (k == (ubk-1))) {
                                if ((i | j | k) != 0) {
                                    if (offset != (max+1)) {
                                        printf("%d,",max);
                                        printf("%d]\n",offset);
                                    } else {
                                        printf("%d-%d]\n",min,offset);
                                    }
                                } else {
                                    printf("%d]\n",offset);
                                }
                            } else {
                                if (offset != (max+1)) { // end of consecutive sequence
                                    if (max == min) { // singleton
                                        printf("%d,",max);
                                    } else { // range
                                        printf("%d-%d,",min,max);
                                    }
                                    max = min = offset;
                                } else {
                                    max = offset;
                                }
                            }
                        }
                    }
                    k++;
                }
                j++;
            }
            i++;
        }
    }
}
#endif

// Utility function to enqueue a waiter when the metadata is being fetch
// Impl will most likely move to runtime events
static u64 enqueueMdProxyWaiter(ocrPolicyDomain_t * pd, MdProxy_t * mdProxy, ocrPolicyMsg_t * msg) {
    ocrAssert(msg->type & PD_MSG_REQUEST);
        MdProxyNode_t * node = (MdProxyNode_t *) pd->fcts.pdMalloc(pd, sizeof(MdProxyNode_t));
    // This is fugly. acquire we know are on the stack so require copies. Others should be incoming
    if ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE) {
        u64 msgSz = ocrPolicyMsgGetMsgBaseSize(msg, /*isIn=*/true);
        // Make a copy of the message since this is an asynchronous context
        ocrPolicyMsg_t * msgCpy = (ocrPolicyMsg_t *) pd->fcts.pdMalloc(pd, msgSz);
        msgCpy->bufferSize = msgSz;
        msgCpy->usefulSize = msgSz;
        ocrPolicyMsgMarshallMsg(msg, msgSz, (u8*) msgCpy, MARSHALL_DUPLICATE);
        node->msg = msgCpy;
    } else {
        node->msg = msg;
    }
    u64 newValue = (u64) node;
    bool notSucceed = true;
    do {
        MdProxyNode_t * head = mdProxy->queueHead;
        if (head == REG_CLOSED) { // registration is closed
            break;
        }
        node->next = head;
        u64 curValue = (u64) head;
        u64 oldValue = hal_cmpswap64((u64*) &(mdProxy->queueHead), curValue, newValue);
        notSucceed = (oldValue != curValue);
    } while(notSucceed);

    if (notSucceed) { // registration has closed
        pd->fcts.pdFree(pd, node);
        // There must be a fence between the head CAS and the 'ptr'
        // assignment in the code setting that resolve the metadata pointer
        u64 val = (u64) mdProxy->ptr;
        ocrAssert(val != 0);
        return val;
    }
    return 0;
}

//NOTE: TG compiles a different implementation
u8 resolveRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t * fatGuid,
                                ocrPolicyMsg_t * msg, bool isBlocking, bool fetch) {
    // Check if known locally
    u64 val;
    MdProxy_t * mdProxy = NULL;
    //getVal - resolve
    u8 res = pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], fatGuid->guid, &val, NULL, (fetch ? MD_FETCH : MD_PROXY), &mdProxy);
#if defined(ENABLE_EXTENSION_DISTRIBUTED_LABELED)
    if ((val == 0) || (val == ((u64)-1))) {
        // Checking for -1 addresses the race stemming from local worker
        // threads competing to create a reduction event. They compete first
        // on the proxy creation, however some caller cannot create the instance
        // (incoming msg). Hence, there's a second level of competition casing the
        // proxy's mdPtr to -1 to win the right to allocate the event
        // Catching this here, allows to prevent any incoming MD_COMM messages to be
        // processed before the proxy ptr is valid.
#else
    if (val == 0) {
#endif
        //TODO-STUFF getVal could return EPERM and val!=0
        //Should look at BT see who's depending on that EPERM
        if (res == OCR_EPERM) {
            return OCR_EPERM;
        }
#ifdef OCR_ASSERT
        ocrGuidKind guidKind;
        RESULT_ASSERT(pd->guidProviders[0]->fcts.getKind(pd->guidProviders[0], fatGuid->guid, &guidKind), == , 0);
        // The reduction event may not be have been created in the current PD yet.
        // That's why getVal may return no reference (val == 0) but return res == 0.
        bool check = (res == OCR_EPEND);
#ifdef ENABLE_EXTENSION_COLLECTIVE_EVT
        check = check || ((res == 0) && (guidKind == OCR_GUID_EVENT_COLLECTIVE));
#endif
        ocrAssert(check);
#endif
        // The metadata is absent and the GUID provider is fetching it
        if (isBlocking) {
            // Busy-wait and return only when the metadata is resolved
            DPRINTF(DEBUG_LVL_VVERB,"Resolving metadata: enter busy-wait for blocking call\n");
            do {
                // Let the scheduler know we are blocked
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
                msg.type = PD_MSG_MGT_MONITOR_PROGRESS | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                PD_MSG_FIELD_I(monitoree) = NULL;
                PD_MSG_FIELD_IO(properties) = (0 | MONITOR_PROGRESS_COMM);
                RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
#undef PD_MSG
#undef PD_TYPE
                //getVal - resolve
                pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], fatGuid->guid, &val, NULL, MD_LOCAL, NULL);
#if defined(ENABLE_EXTENSION_DISTRIBUTED_LABELED)
            } while ((val == 0) || (val == ((u64)-1)));
#else
            } while(val == 0);
#endif
            DPRINTF(DEBUG_LVL_VVERB,"Resolving metadata: exit busy-wait for blocking call\n");
        } else {
            ocrAssert(msg != NULL);
            // Enqueue itself on the MD proxy, the caller will be rescheduled for execution.
            // Remember that we're still competing with the fetch operation's completion.
            // So we may actually not succeed enqueuing ourselves and be able to read the MD pointer.
            val = enqueueMdProxyWaiter(pd, mdProxy, msg);
            // Warning: At this point we cannot access the msg pointer anymore.
            // This code becomes concurrent with the continuation being invoked, possibly destroying 'msg'.
            //TODO that sounds like a bug for red event no ?
        }
    }
#if defined(ENABLE_EXTENSION_DISTRIBUTED_LABELED)
    // if reach here with -1 it means there a race between the enqueue operation
    // and properly setting the MD's metaDataPtr before CASing the waiter queue
    ASSERT(val != ((u64)-1));
#endif
    fatGuid->metaDataPtr = (void *) val;
    return ((val) ? 0 : OCR_EPEND);
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

// Callback from the capable modules
// val contains worker id on lower 16 bits and RL on next 16 bits
void hcWorkerCallback(ocrPolicyDomain_t *self, u64 val) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)self;
    DPRINTF(DEBUG_LVL_VERB, "Got check-in from worker %"PRIu64" for RL %"PRIu64"\n", val & 0xFFFF, (u64)(val >> 16));
    // Read these now and fence because on TEAR_DOWN as soon
    // as checkedIn reaches zero the master thread falls-through
    // and write to rlSwitch.
    ocrRunlevel_t runlevel = rself->rlSwitch.runlevel;
    s8 nextPhase = rself->rlSwitch.nextPhase;
    u32 properties = rself->rlSwitch.properties;
    hal_fence();

    u64 oldVal, newVal;
    do {
        oldVal = rself->rlSwitch.checkedIn;
        newVal = hal_cmpswap64(&(rself->rlSwitch.checkedIn), oldVal, oldVal - 1);
    } while(oldVal != newVal);
    if(oldVal == 1) {
        // This means we managed to set it to 0
        DPRINTF(DEBUG_LVL_VVERB, "All workers checked in, moving to the next stage: RL %"PRIu32"; phase %"PRId32"\n",
                runlevel, nextPhase);
        if(properties & RL_FROM_MSG) {
            // We need to re-enter switchRunlevel
            if((properties & RL_BRING_UP) &&
               (nextPhase == RL_GET_PHASE_COUNT_UP(self, runlevel))) {
                // Switch to the next runlevel
                ++rself->rlSwitch.runlevel;
                rself->rlSwitch.nextPhase = 0;
            }
            if((properties & RL_TEAR_DOWN) && (nextPhase == -1)) {
                // Switch to the next runlevel (going down)
                --rself->rlSwitch.runlevel;
                rself->rlSwitch.nextPhase = RL_GET_PHASE_COUNT_DOWN(self, rself->rlSwitch.runlevel) - 1;
                hal_fence(); // for LEGACY MASTER loop probably not needed
            }
            // Ok to read cached value here since in this case, the previous
            // 'if' couldn't have updated next phase to zero.
            if(runlevel == RL_COMPUTE_OK && nextPhase == 0) {
                // In this case, we do not re-enter the switchRunlevel because the master worker
                // will drop out of its computation (at some point) and take over
                DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER thread will pick up for switch to RL_COMPUTE_OK\n");
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "Re-entering switchRunlevel with RL %"PRIu32"; phase %"PRIu32"; prop 0x%"PRIx32"\n",
                        rself->rlSwitch.runlevel, rself->rlSwitch.nextPhase, rself->rlSwitch.properties);
                RESULT_ASSERT(self->fcts.switchRunlevel(self, rself->rlSwitch.runlevel, rself->rlSwitch.properties), ==, 0);
            }
        } else { // else, some thread is already in switchRunlevel and will be unblocked
            DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER thread will continue\n");
        }
    }
}

// Function to cause run-level switches in this PD
u8 hcPdSwitchRunlevel(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, u32 properties) {
    s32 j, k=0;
    phase_t i=0, curPhase, phaseCount;
    u64 maxCount;

    u8 toReturn = 0;
    u32 origProperties = properties;
    u32 masterWorkerProperties = 0;

#define GET_PHASE(counter) curPhase = (properties & RL_BRING_UP)?counter:(phaseCount - counter - 1)

    ocrPolicyDomainHc_t* rself = (ocrPolicyDomainHc_t*)policy;
    // Check properties
    u32 amNodeMaster = (properties & RL_NODE_MASTER) == RL_NODE_MASTER;
#ifdef OCR_ASSERT
    u32 amPDMaster = properties & RL_PD_MASTER;
#endif
    u32 fromPDMsg = properties & RL_FROM_MSG;
    properties &= ~RL_FROM_MSG; // Strip this out from the rest; only valuable for the PD
    masterWorkerProperties = properties;
    properties &= ~RL_NODE_MASTER;

    if(!(fromPDMsg)) {
        // RL changes called directly through switchRunlevel should
        // only transition until PD_OK. After that, transitions should
        // occur using policy messages
        ocrAssert(amNodeMaster || (runlevel <= RL_PD_OK));
        // If this is direct function call, it should only be a request
        ocrAssert((properties & RL_REQUEST) && !(properties & (RL_RESPONSE | RL_RELEASE)))
    }

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
            // For RL_CONFIG_PARSE, we set it to 2 on bring up because on the first
            // phase, modules can register their desire for the number of phases per runlevel
            // and, on the second phase, they can make sure they got what they wanted (or
            // adapt to what they did get)
            policy->phasesPerRunlevel[RL_CONFIG_PARSE][0] = (1<<4) + 2;
            phaseCount = 2;
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
            DPRINTF(DEBUG_LVL_WARN, "RL_CONFIG_PARSE(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, curPhase, toReturn);
        }

        break;
    }
    case RL_NETWORK_OK:
    {
        // In this single PD implementation, nothing specific to do (just pass it down)
        // In general, this is when you setup communication
        phaseCount = ((policy->phasesPerRunlevel[RL_NETWORK_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_NETWORK_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, curPhase, toReturn);
        }
        break;
    }
    case RL_PD_OK:
    {
        // In this single PD implementation for x86, there is nothing specific to do
        // In general, you need to:
        //     - if not amNodeMaster, start a worker for this PD
        //     - that worker (or the master one) needs to then transition all inert modules to PD_OK
        phaseCount = ((policy->phasesPerRunlevel[RL_PD_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;

        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }

#ifdef ENABLE_EXTENSION_PAUSE
        if((!toReturn) && (properties & RL_BRING_UP)) {
            //BUG #583: is it important to do that at the first phase or ?
            // if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
            registerSignalHandler();
            ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)policy;
            //Initialize pause/query/resume variables
            rself->pqrFlags.runtimePause = false;
            rself->pqrFlags.pauseCounter = 0;
            rself->pqrFlags.pausingWorker = -1;
        }
#endif

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_PD_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, curPhase, toReturn);
        }
        break;
    }
    case RL_MEMORY_OK:
    {
        phaseCount = ((policy->phasesPerRunlevel[RL_MEMORY_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, masterWorkerProperties);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, j==0?masterWorkerProperties:properties, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_MEMORY_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, curPhase, toReturn);
        }
        break;
    }
    case RL_GUID_OK:
    {
        // In the general case (with more than one PD), in this step and on bring-up:
        //     - send messages to all neighboring PDs to transition to this state
        //     - do local transition
        //     - wait for responses from neighboring PDs
        //     - report back to caller (if RL_FROM_MSG)
        //     - send release message to neighboring PDs
        // If this is RL_FROM_MSG, the above steps may occur in multiple steps (ie: you
        // don't actually "wait" but rather re-enter this function on incomming
        // messages from neighbors. If not RL_FROM_MSG, you do block.

        // This is also the first stage at which we can allocate the microtask tables
        // Memory has been brought up so we have all allocators up and running (including
        // the forthcoming slab allocator)

        if(properties & RL_BRING_UP) {
            // On BRING_UP, bring up GUID provider
            // We assert that there are two phases. The first phase is mostly to bring
            // up the GUID provider and the last phase is to actually get GUIDs for
            // the various components if needed
            phaseCount = policy->phasesPerRunlevel[RL_GUID_OK][0] & 0xF;
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
                ocrAssert(0);
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
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, j==0?masterWorkerProperties:properties, NULL, 0);
                }

            }
        } else {
            phaseCount = policy->phasesPerRunlevel[RL_GUID_OK][0] >> 4;
            maxCount = policy->workerCount;
            for(i = 0; i < phaseCount; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);
                for(j = 0; j < maxCount; ++j) {
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
            DPRINTF(DEBUG_LVL_WARN, "RL_GUID_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, i-1, toReturn);
        }
        break;
    }

    case RL_COMPUTE_OK:
    {
        // At this stage, we have a memory to use so we can create the placer
        // This phase is the first one creating capable modules (workers) apart from myself
        if(properties & RL_BRING_UP) {

#ifdef ENABLE_EXTENSION_PERF
#define MAX_EDT_TEMPLATES 64
            u32 k;
            policy->taskPerfs = newBoundedQueue(policy, MAX_EDT_TEMPLATES);
            policy->myNodeStats = policy->fcts.pdMalloc(policy, sizeof(u64)*NODE_PERF_MAX);
            for(k = 0; k<NODE_PERF_MAX; k++) policy->myNodeStats[k] = 0;
            policy->bestNodeStats = policy->fcts.pdMalloc(policy, sizeof(u64)*NODE_PERF_MAX);
            for(k = 0; k<NODE_PERF_MAX; k++) policy->bestNodeStats[k] = NODE_STATS_MAX;
            policy->bestNodes = policy->fcts.pdMalloc(policy, sizeof(u64)*NODE_PERF_MAX);
            for(k = 0; k<NODE_PERF_MAX; k++) policy->bestNodes[k] = UNINIT_NODE_ID;
#endif
#ifdef LOAD_BALANCING_TEST
            policy->migrationCount = 0;
#endif

            phaseCount = policy->phasesPerRunlevel[RL_COMPUTE_OK][0] & 0xF;
            maxCount = policy->workerCount;
            for(i = rself->rlSwitch.nextPhase; i < phaseCount; ++i) {
                if(RL_IS_FIRST_PHASE_UP(policy, RL_COMPUTE_OK, i)) {
                    guidify(policy, (u64)policy, &(policy->fguid), OCR_GUID_POLICY);
                    // To be deprecated
                    policy->placer = createLocationPlacer(policy);
                    // Create and initialize the platform model (work in progress)
                    policy->platformModel = createPlatformModelAffinity(policy);
#ifdef ENABLE_RESILIENCY
                    u64 calTime = 0;
                    if (policy->commApiCount != 0)
                        calTime = policy->commApis[0]->syncCalTime;
                    if (calTime == 0) {
                        ocrAssert(policy->neighborCount == 0);
                        calTime = salGetCalTime();
                    }
                    ocrPolicyDomainHc_t *derived = (ocrPolicyDomainHc_t *)policy;
                    derived->calTime = calTime;
#endif
                }
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);

                // Setup the resume RL switch structure (in the synchronous case, used as
                // the counter we wait on)
                rself->rlSwitch.checkedIn = maxCount;
                rself->rlSwitch.runlevel = RL_COMPUTE_OK;
                rself->rlSwitch.nextPhase = i + 1;
                rself->rlSwitch.properties = origProperties;
                hal_fence();

                // Worker 0 is considered the capable one by convention
                // Still need to find out if the current PD is the "blessed" with mainEdt execution
                ocrGuid_t affinityMasterPD;
                u64 count = 0;
                // There should be a single master PD
                ocrAssert(!ocrAffinityCount(AFFINITY_PD_MASTER, &count) && (count == 1));
                ocrAffinityGet(AFFINITY_PD_MASTER, &count, &affinityMasterPD);
                ocrLocation_t masterLocation;
                affinityToLocation(&masterLocation, affinityMasterPD);
                u16 blessed = ((policy->myLocation == masterLocation) ? RL_BLESSED : 0);
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, masterWorkerProperties | blessed,
                    &hcWorkerCallback, RL_COMPUTE_OK << 16);

                for(j = 1; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, &hcWorkerCallback, (RL_COMPUTE_OK << 16) | j);
                }
                if(!fromPDMsg) {
                    // Here we need to block because when we return from the function, we need to have
                    // transitioned
                    DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: synchronous switch to RL_COMPUTE_OK phase %"PRId32" ... will block\n", i);
                    while(rself->rlSwitch.checkedIn)
                        ;
                    ocrAssert(rself->rlSwitch.checkedIn == 0);
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: asynchronous switch to RL_COMPUTE_OK phase %"PRId32"\n", i);
                    // We'll continue this from hcWorkerCallback
                    break; // Break out of the loop
                }
            }
        } else {
            // Tear down
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_COMPUTE_OK);
            // On bring down, we need to have at least two phases:
            //     - one where we actually stop the workers (asynchronously)
            //     - one where we join the workers (to clean up properly)
            ocrAssert(phaseCount > 1);
            maxCount = policy->workerCount;

            // We do something special for the last phase in which we only have
            // one worker (all others should no longer be operating
            if(RL_IS_LAST_PHASE_DOWN(policy, RL_COMPUTE_OK, rself->rlSwitch.nextPhase)) {
                ocrAssert(!fromPDMsg); // This last phase is done synchronously
                ocrAssert(amPDMaster); // Only master worker should be here
                toReturn |= helperSwitchInert(policy, runlevel, rself->rlSwitch.nextPhase, masterWorkerProperties);
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, rself->rlSwitch.nextPhase,
                    masterWorkerProperties | RL_BLESSED, NULL, 0);
                for(j = 1; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, rself->rlSwitch.nextPhase, properties, NULL, 0);
                }
#ifdef ENABLE_EXTENSION_PERF
                ocrPerfCounters_t *counters;
#ifdef OCR_ENABLE_SIMULATOR
                char fname[64];
                sprintf(fname, "output_%ld", (u64)policy->myLocation);
                FILE *fpSim = fopen(fname, "w");
#endif
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
                char filename[16];
                sprintf(filename, "PerfNode%ld", policy->myLocation);
                FILE *fp = fopen(filename, "w");
#endif
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
                fprintf(fp, "EDT\tCount\tHW_CYCLES\tL1_HITS\tL1_MISS\tFLOAT_OPS\tEDT_CREATES\tDB_TOTAL\tDB_CREATES\tDB_DESTROYS\tEVT_SATISFIES\tMask\n");
#else
#ifdef ENABLE_EXTENSION_PERF_KNL
                // TODO: Auto-generate these based on inc/ocr-perfmon.h
                ocrPrintf("EDT\tCount\tHW_CYCLES\tHBM_TRAFFIC\tOFFCORE_TRAFFIC\tEDT_CREATES\tDB_TOTAL\tDB_CREATES\tDB_DESTROYS\tEVT_SATISFIES\tMask\n");
#else
                ocrPrintf("EDT\tCount\tHW_CYCLES\tL1_HITS\tL1_MISS\tFLOAT_OPS\tEDT_CREATES\tDB_TOTAL\tDB_CREATES\tDB_DESTROYS\tEVT_SATISFIES\tMask\n");
#endif /*ENABLE_EXTENSION_PERF_KNL*/
#endif /*OCR_ENABLE_SIMULATOR*/
                while(!queueIsEmpty(policy->taskPerfs)) {
                    u32 i;
                    counters = queueRemoveLast(policy->taskPerfs);
#ifdef OCR_ENABLE_SIMULATOR
                    fprintf(fpSim, "%p\t%"PRId32"\t", counters->edt, counters->count);
                    for(i = 0; i < PERF_MAX; i++) fprintf(fpSim, "%"PRId64"\t", counters->stats[i].average);
                    fprintf(fpSim, "%"PRIx32"\n", counters->steadyStateMask);
#endif
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
                    fprintf(fp, "%p\t%"PRId32"\t", counters->edt, counters->count);
#else
                    ocrPrintf("%p\t%"PRId32"\t", counters->edt, counters->count);
#endif
                    for(i = 0; i < PERF_MAX; i++)
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
                        fprintf(fp, "%"PRId64"\t", counters->stats[i].average);
#else
                        ocrPrintf("%"PRId64"\t", counters->stats[i].average);
#endif
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
                    fprintf(fp, "%"PRIx32"\n", counters->steadyStateMask);
#else
                    ocrPrintf("%"PRIx32"\n", counters->steadyStateMask);
#endif
                    policy->fcts.pdFree(policy, counters);
                }
#ifdef OCR_ENABLE_SIMULATOR
                fclose(fpSim);
#endif
                queueDestroy(policy->taskPerfs);
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
                fprintf(fp, "Node stats:\t"); for(i = 0; i < NODE_PERF_MAX; i++) fprintf(fp, "%ld\t", policy->myNodeStats[i]);
                fclose(fp);
#else
                ocrPrintf("Node stats:\t"); for(i = 0; i < NODE_PERF_MAX; i++) ocrPrintf("%ld\t", policy->myNodeStats[i]); ocrPrintf("\n");
#endif
                policy->fcts.pdFree(policy, policy->myNodeStats);
                policy->fcts.pdFree(policy, policy->bestNodes);
                policy->fcts.pdFree(policy, policy->bestNodeStats);
#endif
#ifdef LOAD_BALANCING_TEST
                ocrPrintf("Total migrations on %ld = %d\n", policy->myLocation, policy->migrationCount);
#endif
#ifdef SHOW_BINDING_INFO
                printBindingInfo(policy);
#endif
                //to be deprecated
                destroyLocationPlacer(policy);
                destroyPlatformModelAffinity(policy);

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

#ifdef ENABLE_RESILIENCY
                ocrPolicyDomainHc_t *derived = (ocrPolicyDomainHc_t *)policy;
                ocrAssert(derived->prevCheckpointName == NULL);
                salRemovePdCheckpoint(derived->currCheckpointName);
#endif
            } else { // Tear-down RL_USER_OK not last phase

                for(i = rself->rlSwitch.nextPhase; i > 0; --i) {
                    toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);

                    // Setup the resume RL switch structure (in the synchronous case, used as
                    // the counter we wait on)
                    rself->rlSwitch.checkedIn = maxCount;
                    rself->rlSwitch.runlevel = RL_COMPUTE_OK;
                    rself->rlSwitch.nextPhase = i - 1;
                    rself->rlSwitch.properties = origProperties;
                    hal_fence();

                    // Worker 0 is considered the capable one by convention
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, masterWorkerProperties | RL_BLESSED,
                        &hcWorkerCallback, RL_COMPUTE_OK << 16);

                    for(j = 1; j < maxCount; ++j) {
                        toReturn |= policy->workers[j]->fcts.switchRunlevel(
                            policy->workers[j], policy, runlevel, i, properties, &hcWorkerCallback, (RL_COMPUTE_OK << 16) | j);
                    }
                    if(!fromPDMsg) {
                        ocrAssert(0); // Always from a PD message since it is from a shutdown message
                    } else {
                        DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: asynchronous switch from RL_COMPUTE_OK phase %"PRId32"\n", i);
                        // We'll continue this from hcWorkerCallback
                        break;
                    }
                }
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_COMPUTE_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, i-1, toReturn);
        }
        break;
    }
    case RL_USER_OK:
    {
        if(properties & RL_BRING_UP) {
            // This branch is only taken in legacy mode the second time RL_USER_OK is entered
            if (rself->rlSwitch.legacySecondStart) {
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, masterWorkerProperties | RL_BLESSED, NULL, 0);
                // When I drop out of this, I should be in RL_COMPUTE_OK at phase 0
                // wait for everyone to check in so that I can continue shutting down
                DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker LEGACY mode, dropped out... waiting for others to complete RL\n");

                //Warning: here make sure the code in hcWorkerCallback reads all the rlSwitch
                //info BEFORE decrementing checkedIn. Otherwise there's a race on rlSwitch
                //between the following code and hcWorkerCallback.
                while(rself->rlSwitch.checkedIn != 0)
                    ;

                ocrAssert(rself->rlSwitch.runlevel == RL_COMPUTE_OK && rself->rlSwitch.nextPhase == 0);
                DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker LEGACY mode, wrapping up shutdown\n");
                // We complete the RL_COMPUTE_OK stage which will bring us down to RL_MEMORY_OK which will
                // get wrapped up by the outside code
                rself->rlSwitch.properties &= ~RL_FROM_MSG;
                toReturn |= policy->fcts.switchRunlevel(policy, rself->rlSwitch.runlevel,
                                                        rself->rlSwitch.properties | RL_NODE_MASTER | RL_PD_MASTER);
                return toReturn;
            }
            // BRING_UP is called twice in RL_LEGACY mode, record we've seen the first call.
            rself->rlSwitch.legacySecondStart = true;
#ifdef ENABLE_RESILIENCY
            rself->timestamp = salGetTime();
            DPRINTF(DEBUG_LVL_INFO, "PD worker count: %"PRIu64" Compute worker count: %"PRIu32"\n", policy->workerCount, rself->computeWorkerCount);
#endif
            // Register properties here to allow tear down to read special flags set on bring up
            rself->rlSwitch.properties = properties;
            phaseCount = RL_GET_PHASE_COUNT_UP(policy, RL_USER_OK);
            maxCount = policy->workerCount;
#ifdef OCR_ENABLE_SIMULATOR
            policy->pdTime = 0;
            policy->slowestWorker = policy->workerCount;
#endif
            for(i = 0; i < phaseCount - 1; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, masterWorkerProperties | RL_BLESSED, NULL, 0);
                for(j = 1; j < maxCount; ++j) {
                    // We start them in an async manner but don't need any callback (ie: we
                    // don't care if they have really started) since there is no bring-up barrier)
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
            }
            if(i == phaseCount - 1) { // Tests if we did not break out earlier with if(toReturn)
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);
                for(j = 1; j < maxCount; ++j) {
                    // We start them in an async manner but don't need any callback (ie: we
                    // don't care if they have really started) since there is no bring-up barrier)
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
                // Always do the capable worker last in this case (it will actualy start doing something useful)
                rself->rlSwitch.runlevel = RL_USER_OK;
                rself->rlSwitch.nextPhase = phaseCount;
                if (properties & RL_LEGACY) {
                    // In legacy mode the master worker just do its setup and falls-through
                    DPRINTF(DEBUG_LVL_VVERB, "Starting PD_MASTER worker in LEGACY mode\n");
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, masterWorkerProperties | RL_BLESSED, NULL, 0);
                } else {
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, masterWorkerProperties | RL_BLESSED, NULL, 0);
                    // When I drop out of this, I should be in RL_COMPUTE_OK at phase 0
                    // wait for everyone to check in so that I can continue shutting down
                    DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker dropped out... waiting for others to complete RL\n");

                    //Warning: here make sure the code in hcWorkerCallback reads all the rlSwitch
                    //info BEFORE decrementing checkedIn. Otherwise there's a race on rlSwitch
                    //between the following code and hcWorkerCallback.
                    while(rself->rlSwitch.checkedIn != 0)
                        ;

                    ocrAssert(rself->rlSwitch.runlevel == RL_COMPUTE_OK && rself->rlSwitch.nextPhase == 0);
                    DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker wrapping up shutdown\n");
                    // We complete the RL_COMPUTE_OK stage which will bring us down to RL_MEMORY_OK which will
                    // get wrapped up by the outside code
                    rself->rlSwitch.properties &= ~RL_FROM_MSG;
                    toReturn |= policy->fcts.switchRunlevel(policy, rself->rlSwitch.runlevel,
                                                            rself->rlSwitch.properties | (amNodeMaster?RL_NODE_MASTER:RL_PD_MASTER));
                }
            }
        } else { // Tear down
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_USER_OK);
            maxCount = policy->workerCount;
            for(i = rself->rlSwitch.nextPhase; i >= 0; --i) {
                toReturn |= helperSwitchInert(policy, runlevel, i, masterWorkerProperties);

                // Setup the resume RL switch structure (in the synchronous case, used as
                // the counter we wait on)
                rself->rlSwitch.checkedIn = maxCount;
                rself->rlSwitch.runlevel = RL_USER_OK;
                rself->rlSwitch.nextPhase = i - 1;
                rself->rlSwitch.properties = origProperties;
                hal_fence();

                // Worker 0 is considered the capable one by convention
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, masterWorkerProperties | RL_BLESSED,
                    &hcWorkerCallback, RL_USER_OK << 16);

                for(j = 1; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, &hcWorkerCallback, (RL_USER_OK << 16) | j);
                }
                if(!fromPDMsg) {
                    ocrAssert(0); // It should always be from a PD MSG since it is an asynchronous shutdown
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: asynchronous switch from RL_USER_OK phase %"PRId32"\n", i);
                    // We'll continue this from hcWorkerCallback
                    break;
                }
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_USER_OK(%"PRId32") phase %"PRId32" failed: %"PRId32"\n", origProperties, i-1, toReturn);
        }
        break;
    }
    default:
        // Unknown runlevel
        ocrAssert(0);
    }
    return toReturn;
}

void hcPolicyDomainDestruct(ocrPolicyDomain_t * policy) {
    // Destroying instances
    u64 i = 0;
    u64 maxCount = 0;
    //BUG #583: should transform all these to stop RL_DEALLOCATE

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

    // Destruct factories
    // Not all factories might have been used in the config file,
    // so only destroy them if they were instantiated.
    maxCount = policy->factoryCount;
    for(i = 0; i < maxCount; ++i) {
        if(policy->factories[i])
            policy->factories[i]->destruct(policy->factories[i]);
    }

    // Destroy these last in case some of the other destructs make use of them
    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        policy->guidProviders[i]->fcts.destruct(policy->guidProviders[i]);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        policy->allocators[i]->fcts.destruct(policy->allocators[i]);
    }

    // Destroy self
    runtimeChunkFree((u64)policy->workers, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->commApis, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->schedulers, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->allocators, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->factories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->guidProviders, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->schedulerObjectFactories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy, PERSISTENT_CHUNK);
}

static void localDeguidify(ocrPolicyDomain_t *self, ocrFatGuid_t *guid) {
    START_PROFILE(pd_hc_localDeguidify);
    ocrAssert(self->guidProviderCount == 1);
    if((guid->metaDataPtr == NULL) && !(ocrGuidIsNull(guid->guid)) && !(ocrGuidIsUninitialized(guid->guid))) {
        //getVal - resolve
        self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid->guid,
                                            (u64*)(&(guid->metaDataPtr)), NULL, MD_LOCAL, NULL);
    }
    RETURN_PROFILE();
}

static u8 hcMemAlloc(ocrPolicyDomain_t *self, ocrFatGuid_t* allocator, u64 size,
                     ocrMemType_t memType, void** ptr, u32 prescription) {
    void* result;
    u64 idx = (prescription<self->allocatorCount)?prescription:0;
    ocrAssert (memType == GUID_MEMTYPE || memType == DB_MEMTYPE);
#ifdef OCR_MONITOR_ALLOCATOR
    u64 starttime = 0;
    OCR_TOOL_TRACE_GETTIME(starttime);
#endif
    result = self->allocators[idx]->fcts.allocate(self->allocators[idx], size, 0);
#ifdef OCR_MONITOR_ALLOCATOR
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_ALLOCATOR, OCR_ACTION_ALLOCATE, traceAlloc, starttime, (u64)OCR_ALLOC_MEMALLOC, size, (u64)memType, result);
#endif
    if (result) {
        *ptr = result;
        *allocator = self->allocators[idx]->fguid;
        return 0;
    } else {
        DPRINTF(DEBUG_LVL_WARN, "hcMemAlloc returning NULL for size %"PRId64"\n", (u64) size);
        return OCR_ENOMEM;
    }
}

static u8 hcMemUnAlloc(ocrPolicyDomain_t *self, ocrFatGuid_t* allocator,
                       void* ptr, ocrMemType_t memType) {
#ifdef OCR_MONITOR_ALLOCATOR
    u64 starttime = 0;
    OCR_TOOL_TRACE_GETTIME(starttime);
#endif
    allocatorFreeFunction(ptr);
#ifdef OCR_MONITOR_ALLOCATOR
    ocrWorker_t *worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // If the current worker->curTask is the same as the memory
    // being unalloced, tracing will fault.
    if (worker && worker->curTask == ptr)
        worker->curTask = NULL;
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_ALLOCATOR, OCR_ACTION_DEALLOCATE, traceDealloc, starttime, (u64)OCR_ALLOC_MEMUNALLOC, ptr);
#endif
    return 0;
}

/**
 * Checks validity of EDT create arguments and create an instance
 */
static u8 createEdtHelper(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                      ocrFatGuid_t  edtTemplate, u32 *paramc, u64* paramv,
                      u32 *depc, u32 properties, ocrHint_t *hint,
                      ocrFatGuid_t * outputEvent, ocrTask_t * currentEdt,
                      ocrFatGuid_t parentLatch, ocrWorkType_t workType) {
    ocrTaskTemplate_t *taskTemplate = (ocrTaskTemplate_t*)edtTemplate.metaDataPtr;
    DPRINTF(DEBUG_LVL_VVERB, "Creating EDT with template GUID "GUIDF" (%p) (paramc=%"PRId32"; depc=%"PRId32")"
            " and have paramc=%"PRId32"; depc=%"PRId32"\n", GUIDA(edtTemplate.guid), edtTemplate.metaDataPtr,
            taskTemplate->paramc, taskTemplate->depc, *paramc, *depc);
    // Check that
    // 1. EDT doesn't have "default" as parameter count if the template
    //    was created with "unknown" as parameter count
    // 2. EDT has "default" as parameter count only if the template was created
    //    with a valid parameter count
    // 3. If neither of the above, the EDT & template both agree on the parameter count
    ocrAssert(((taskTemplate->paramc == EDT_PARAM_UNK) && *paramc != EDT_PARAM_DEF) ||
           (taskTemplate->paramc != EDT_PARAM_UNK && (*paramc == EDT_PARAM_DEF ||
                   taskTemplate->paramc == *paramc)));
    // Check that
    // 1. EDT doesn't have "default" as dependence count if the template
    //    was created with "unknown" as dependence count
    // 2. EDT has "default" as dependence count only if the template was created
    //    with a valid dependence count
    // 3. If neither of the above, the EDT & template both agree on the dependence count
    ocrAssert(((taskTemplate->depc == EDT_PARAM_UNK) && *depc != EDT_PARAM_DEF) ||
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
        DPRINTF(DEBUG_LVL_WARN,"error: EDT paramc set to %"PRId32" but paramv is NULL\n", *paramc);
        ocrAssert(false);
        return OCR_EINVAL;
    }
    if((*paramc == 0) && (paramv != NULL)) {
        DPRINTF(DEBUG_LVL_WARN,"error: EDT paramc set to zero but paramv not NULL\n");
        ocrAssert(false);
        return OCR_EINVAL;
    }

    //Setup task parameters
    paramListTask_t taskparams;
    taskparams.workType = workType;

    u8 returnCode = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->instantiate(
                           (ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]), guid,
                           edtTemplate, *paramc, paramv,
                           *depc, properties, hint, outputEvent, currentEdt,
                           parentLatch, (ocrParamList_t*)(&taskparams));
    if(returnCode && returnCode != OCR_EGUIDEXISTS) {
        DPRINTF(DEBUG_LVL_WARN, "unable to create EDT, instantiate returnCode is %"PRIx32"\n", returnCode);
        ocrAssert(false);
    }

    return returnCode;
}

static u8 createEdtTemplateHelper(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                              ocrEdt_t func, u32 paramc, u32 depc, const char* funcName) {
    ocrTaskTemplate_t *base = ((ocrTaskTemplateFactory_t*)(self->factories[self->taskTemplateFactoryIdx]))->instantiate(
                                  (ocrTaskTemplateFactory_t*)(self->factories[self->taskTemplateFactoryIdx]), func, paramc, depc, funcName, NULL);
    (*guid).guid = getObjectField(base, guid);
    (*guid).metaDataPtr = base;
    return 0;
}

static u8 createEventHelper(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                        ocrEventTypes_t type, u32 properties, ocrParamList_t * paramList) {
    return ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->instantiate(
        (ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]), guid, type, properties, paramList);
}

#ifdef REG_ASYNC_SGL
static u8 convertDepAddToSatisfy(ocrPolicyDomain_t *self, ocrFatGuid_t dbGuid,
                                 ocrFatGuid_t destGuid, u32 slot, ocrDbAccessMode_t mode, bool sync) {
#else
static u8 convertDepAddToSatisfy(ocrPolicyDomain_t *self, ocrFatGuid_t dbGuid,
                                 ocrFatGuid_t destGuid, u32 slot, bool sync) {
#endif
    PD_MSG_STACK(msg);
    ocrTask_t *curTask = NULL;
    getCurrentEnv(NULL, NULL, &curTask, &msg);
    ocrFatGuid_t currentEdt;
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST | ((sync) ? PD_MSG_REQ_RESPONSE : 0);
    PD_MSG_FIELD_I(satisfierGuid.guid) = curTask?curTask->guid:NULL_GUID;
    PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = curTask;
    PD_MSG_FIELD_I(guid) = destGuid;
    PD_MSG_FIELD_I(payload) = dbGuid;
    PD_MSG_FIELD_I(currentEdt) = currentEdt;
    PD_MSG_FIELD_I(slot) = slot;
#ifdef REG_ASYNC_SGL
    PD_MSG_FIELD_I(mode) = mode;
#endif
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_PROPAGATE(self->fcts.processMessage(self, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

#ifdef OCR_ENABLE_STATISTICS
static ocrStats_t* hcGetStats(ocrPolicyDomain_t *self) {
    return self->statsObject;
}
#endif

//Notify scheduler of policy message before it is processed
static inline void hcSchedNotifyPreProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg) {
    //Hard-coded for now, ideally scheduler should register interests
    bool eligible = ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) ||
                  ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_CREATE);
    if ((msg->type & PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER) || !eligible)
        return;
    ocrSchedulerOpNotifyArgs_t notifyArgs;
    notifyArgs.base.location = msg->srcLocation;
    notifyArgs.kind = OCR_SCHED_NOTIFY_PRE_PROCESS_MSG;
    notifyArgs.OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG).msg = msg;
    //Ignore the return code here
    self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
            self->schedulers[0], (ocrSchedulerOpArgs_t*) &notifyArgs, NULL);
    msg->type |= PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER;
}

//Notify scheduler of policy message after it is processed
static inline void hcSchedNotifyPostProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg) {
    //Hard-coded for now, ideally scheduler should register interests
    bool eligible = ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_WORK_CREATE) ||
                  ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_CREATE);
    if (!(msg->type & PD_MSG_REQ_POST_PROCESS_SCHEDULER) || !eligible)
        return;
    ocrSchedulerOpNotifyArgs_t notifyArgs;
    notifyArgs.base.location = msg->srcLocation;
    notifyArgs.kind = OCR_SCHED_NOTIFY_POST_PROCESS_MSG;
    notifyArgs.OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_POST_PROCESS_MSG).msg = msg;
    RESULT_ASSERT(self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
                    self->schedulers[0], (ocrSchedulerOpArgs_t*) &notifyArgs, NULL), ==, 0);
    msg->type &= ~PD_MSG_REQ_POST_PROCESS_SCHEDULER;
}

#ifdef ENABLE_RESILIENCY

static void checkinWorkerForCheckpoint(ocrPolicyDomain_t *self, ocrWorker_t *worker) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    DPRINTF(DEBUG_LVL_VERB, "Worker checking in for checkpoint...\n");
    hal_fence();
    worker->stateOfCheckpoint = 1;
    u32 oldVal = hal_xadd32(&rself->checkpointWorkerCounter, 1);
    if(oldVal == (rself->computeWorkerCount - 1)) {
        DPRINTF(DEBUG_LVL_VERB, "All workers checked in for checkpoint...\n");
        ocrAssert(rself->quiesceComms == 0);
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
        rself->quiesceComms = 1;
#endif
        while (rself->quiesceComms != 0 && rself->stateOfCheckpoint != 0)
            ;
        hal_fence();
        if (rself->stateOfCheckpoint != 0) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
            msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
            msg.destLocation = 0;
            PD_MSG_FIELD_I(properties) = OCR_CHECKPOINT_PD_READY;
            RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        } else {
            rself->quiesceComms = 0;
        }
    }
}

static void startPdCheckpoint(ocrPolicyDomain_t *self) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    DPRINTF(DEBUG_LVL_VERB, "PD checkpoint start...\n");
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ocrAssert(worker->id != 0);
    ocrAssert(worker->resiliencyMaster == 0);
    worker->resiliencyMaster = 1;
    hal_fence();
    ocrAssert(rself->resiliencyInProgress == 0);
    rself->resiliencyInProgress = 1;
    hal_fence();

    //First quiesce all comms
    ocrAssert(rself->quiesceComms == 0);
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
    rself->quiesceComms = 1;
#endif
    while (rself->quiesceComms != 0 && rself->stateOfCheckpoint != 0)
        ;
    hal_fence();

    //Next quiesce all the other comp workers
    rself->quiesceComps = 1;
    while (rself->quiesceComps != rself->computeWorkerCount && rself->stateOfCheckpoint != 0)
        ;
    hal_fence();

    //Abort if checkpoint got canceled
    if (rself->stateOfCheckpoint == 0) {
        rself->resiliencyInProgress = 0;
        rself->commStopped = 0;
        rself->quiesceComms = 0;
        rself->quiesceComps = 0;
        worker->resiliencyMaster = 0;
        return;
    }

    //No turning back:
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
    ocrAssert(rself->quiesceComms == 0 && rself->commStopped != 0);
#endif
    ocrAssert(self->schedulers[0]->fcts.count(self->schedulers[0], SCHEDULER_OBJECT_COUNT_RUNTIME_EDT) == 0);

    char *chkptName = NULL;
    u64 chkptSize = 0;
    self->guidProviders[0]->fcts.getSerializationSize(self->guidProviders[0], &chkptSize);
    DPRINTF(DEBUG_LVL_VERB, "PD checkpoint size: %lu\n", chkptSize);

    u8 *buffer = salCreatePdCheckpoint(&chkptName, chkptSize);
    ocrAssert(rself->prevCheckpointName == NULL);
    rself->prevCheckpointName = rself->currCheckpointName;
    rself->currCheckpointName = chkptName;
    DPRINTF(DEBUG_LVL_VERB, "PD checkpoint buffer: %p\n", buffer);

    self->guidProviders[0]->fcts.serialize(self->guidProviders[0], buffer);
    DPRINTF(DEBUG_LVL_VERB, "PD serialized...\n");

#ifdef ENABLE_CHECKPOINT_VERIFICATION
    DPRINTF(DEBUG_LVL_VERB, "Starting PD reset ...\n");
    self->guidProviders[0]->fcts.reset(self->guidProviders[0]);
    self->schedulers[0]->fcts.update(self->schedulers[0], OCR_SCHEDULER_UPDATE_PROP_RESET);
    DPRINTF(DEBUG_LVL_VERB, "PD reset done!\n");

    DPRINTF(DEBUG_LVL_VERB, "Starting PD deserialize from checkpoint ...\n");
    self->guidProviders[0]->fcts.deserialize(self->guidProviders[0], buffer);
    self->guidProviders[0]->fcts.fixup(self->guidProviders[0]);
    DPRINTF(DEBUG_LVL_VERB, "Resuming PD from checkpoint ...\n");
#endif

    RESULT_ASSERT(salClosePdCheckpoint(buffer, chkptSize), ==, 0);
    DPRINTF(DEBUG_LVL_VERB, "PD checkpoint completed!\n");
    hal_fence();

    rself->resiliencyInProgress = 0;
    worker->resiliencyMaster = 0;
    hal_fence();

    //Release the comms
    rself->commStopped = 0;
    hal_fence();

    //Release the comps
    rself->quiesceComps = 0;
    hal_fence();

    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
    msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
    msg.destLocation = 0;
    PD_MSG_FIELD_I(properties) = OCR_CHECKPOINT_PD_DONE;
    RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
}

static void resumePdAfterCheckpoint(ocrPolicyDomain_t *self) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    if (rself->prevCheckpointName != NULL && rself->stateOfCheckpoint != 0) {
        RESULT_ASSERT(salRemovePdCheckpoint(rself->prevCheckpointName), ==, 0);
        rself->prevCheckpointName = NULL;
    }
    hal_fence();
    u64 curTime = salGetTime();
    DPRINTF(DEBUG_LVL_VERB, "Total checkpoint time... %lu nsecs\n", (curTime - rself->timestamp));
    rself->timestamp = curTime;
    hal_fence();
    rself->resumeAfterCheckpoint = rself->stateOfCheckpoint;
}

static void checkinPdForCheckpoint(ocrPolicyDomain_t *self) {
    ocrAssert(self->myLocation == 0);
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    u32 oldVal = hal_xadd32(&rself->checkpointPdCounter, 1);
    ocrAssert(oldVal >= 0 && oldVal <= self->neighborCount);
    if (oldVal == self->neighborCount) {
        DPRINTF(DEBUG_LVL_VERB, "All PDs checked in for checkpoint...\n");
        int i;
        for ( i = 1; i <= self->neighborCount; i++ ) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
            msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
            msg.destLocation = i;
            PD_MSG_FIELD_I(properties) = OCR_CHECKPOINT_PD_START;
            RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        }
        startPdCheckpoint(self);
    }
}

static void checkoutPdFromCheckpoint(ocrPolicyDomain_t *self) {
    ocrAssert(self->myLocation == 0);
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    u32 oldVal = hal_xadd32(&rself->checkpointPdCounter, -1);
    if (oldVal == 1) {
        DPRINTF(DEBUG_LVL_VERB, "All PDs checked out from checkpoint...\n");
        int i;
        for ( i = 1; i <= self->neighborCount; i++ ) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
            msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
            msg.destLocation = i;
            PD_MSG_FIELD_I(properties) = OCR_CHECKPOINT_PD_RESUME;
            RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        }
        RESULT_ASSERT(salSetPdCheckpoint(rself->currCheckpointName), ==, 0);
        resumePdAfterCheckpoint(self);
    }
}

bool doCheckpointResume(ocrPolicyDomain_t *self) {
    ocrAssert(self->myLocation == 0);
    bool resumeFromCheckpoint = salCheckpointExistsResumeQuery();

    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)self;
    ocrAssert(rself->stateOfCheckpoint == 0);
    rself->stateOfRestart = resumeFromCheckpoint;
    rself->initialCheckForRestart = 1;
    DPRINTF(DEBUG_LVL_VERB, "We are %sresuming from checkpoint...\n", resumeFromCheckpoint ? "" : "not ");

    u32 i;
    for ( i = 1; i <= self->neighborCount; i++ ) {
        PD_MSG_STACK(msg);
        getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
        msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
        msg.destLocation = i;
        PD_MSG_FIELD_I(properties) = resumeFromCheckpoint ? OCR_RESTART_PD_TRUE : OCR_RESTART_PD_FALSE;
        RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
    }
    return resumeFromCheckpoint;
}

static void startPdRestart(ocrPolicyDomain_t *self) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    DPRINTF(DEBUG_LVL_VERB, "PD restart begin...\n");
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ocrAssert(worker->id != 0);
    ocrAssert(worker->resiliencyMaster == 0);
    worker->resiliencyMaster = 1;
    hal_fence();
    ocrAssert(rself->resiliencyInProgress == 0);
    rself->resiliencyInProgress = 1;
    hal_fence();

    //First quiesce all comms
    ocrAssert(rself->quiesceComms == 0);
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
    rself->quiesceComms = 1;
#endif
    while (rself->quiesceComms != 0 && rself->stateOfRestart != 0)
        ;
    hal_fence();

    //Next quiesce all the other comp workers
    rself->quiesceComps = 1;
    while (rself->quiesceComps != rself->computeWorkerCount && rself->stateOfRestart != 0)
        ;
    hal_fence();

    //Abort if Restart got canceled
    if (rself->stateOfRestart == 0) {
        ocrAssert(rself->quiesceComps != rself->computeWorkerCount);
        worker->resiliencyMaster = 0;
        rself->resiliencyInProgress = 0;
        rself->commStopped = 0;
        rself->quiesceComms = 0;
        rself->quiesceComps = 0;
        return;
    }

    //No turning back:
    ocrAssert(self->schedulers[0]->fcts.count(self->schedulers[0], SCHEDULER_OBJECT_COUNT_RUNTIME_EDT) == 0);

    char *chkptName = salGetCheckpointName();
    ocrAssert(chkptName != NULL);
    u64 chkptSize = 0;
    u8 *buffer = salOpenPdCheckpoint(chkptName, &chkptSize);
    ocrAssert(buffer != NULL);
    rself->prevCheckpointName = NULL;
    rself->currCheckpointName = chkptName;
    DPRINTF(DEBUG_LVL_VERB, "PD checkpoint buffer: %p\n", buffer);

    DPRINTF(DEBUG_LVL_VERB, "Starting PD reset ...\n");
    self->guidProviders[0]->fcts.reset(self->guidProviders[0]);
    self->schedulers[0]->fcts.update(self->schedulers[0], OCR_SCHEDULER_UPDATE_PROP_RESET);
    DPRINTF(DEBUG_LVL_VERB, "PD reset done!\n");

    DPRINTF(DEBUG_LVL_VERB, "Starting PD deserialize from checkpoint ...\n");
    self->guidProviders[0]->fcts.deserialize(self->guidProviders[0], buffer);
    self->guidProviders[0]->fcts.fixup(self->guidProviders[0]);
    DPRINTF(DEBUG_LVL_VERB, "Resuming PD from checkpoint ...\n");

    RESULT_ASSERT(salClosePdCheckpoint(buffer, chkptSize), ==, 0);

    DPRINTF(DEBUG_LVL_VERB, "PD restart completed...\n");

    hal_fence();

    rself->resiliencyInProgress = 0;
    worker->resiliencyMaster = 0;
    rself->commStopped = 0;

    hal_fence();

    rself->quiesceComps = 0;

    hal_fence();

    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
    msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
    msg.destLocation = 0;
    PD_MSG_FIELD_I(properties) = OCR_RESTART_PD_DONE;
    RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
}

static void checkinWorkerForRestart(ocrPolicyDomain_t *self, ocrWorker_t *worker) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    DPRINTF(DEBUG_LVL_VERB, "Worker checking in for restart...\n");
    hal_fence();
    worker->stateOfRestart = 1;
    u32 oldVal = hal_xadd32(&rself->restartWorkerCounter, 1);
    if(oldVal == (rself->computeWorkerCount - 1)) {
        DPRINTF(DEBUG_LVL_VERB, "All workers checked in for restart...\n");
        ocrAssert(rself->quiesceComms == 0);
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
        rself->quiesceComms = 1;
#endif
        while (rself->quiesceComms != 0 && rself->stateOfRestart != 0)
            ;
        hal_fence();
        if (rself->stateOfRestart != 0) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
            msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
            msg.destLocation = 0;
            PD_MSG_FIELD_I(properties) = OCR_RESTART_PD_READY;
            RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        } else {
            rself->quiesceComms = 0;
        }
    }
}

static void checkinPdForRestart(ocrPolicyDomain_t *self) {
    ocrAssert(self->myLocation == 0);
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    u32 oldVal = hal_xadd32(&rself->restartPdCounter, 1);
    ocrAssert(oldVal >= 0 && oldVal <= self->neighborCount);
    if (oldVal == self->neighborCount) {
        DPRINTF(DEBUG_LVL_VERB, "All PDs checked in for restart...\n");
        int i;
        for ( i = 1; i <= self->neighborCount; i++ ) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
            msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
            msg.destLocation = i;
            PD_MSG_FIELD_I(properties) = OCR_RESTART_PD_START;
            RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        }
        startPdRestart(self);
    }
}

static void checkoutPdFromRestart(ocrPolicyDomain_t *self) {
    ocrAssert(self->myLocation == 0);
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
    u32 oldVal = hal_xadd32(&rself->restartPdCounter, -1);
    if (oldVal == 1) {
        DPRINTF(DEBUG_LVL_VERB, "All PDs checked out from restart...\n");
        int i;
        for ( i = 1; i <= self->neighborCount; i++ ) {
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
            msg.type = PD_MSG_RESILIENCY_CHECKPOINT | PD_MSG_REQUEST;
            msg.destLocation = i;
            PD_MSG_FIELD_I(properties) = OCR_RESTART_PD_RESUME;
            RESULT_ASSERT(self->fcts.processMessage(self, &msg, false), ==, 0);
#undef PD_MSG
#undef PD_TYPE
        }
        hal_fence();
        rself->resumeAfterRestart = 1;
    }
}

#endif

#ifdef ENABLE_OCR_API_DEFERRABLE
// Need a strand table
//  => Think this one can go as a per worker
// Each deferred call is represented by a pdEvent and the succession
// of API calls made in the EDT user code forms a chain.
//
// For each EDT need to maintain the head of the chain and the last inserted
//
// Then on each deferred call:
// - marshall the msg
// - create a pd event for msg
// - Chain the last event for that EDT with the newly created
// - get a new strand
// - enqueue action process message
// - unlock the strand

static void setDeferredReturnDetail(ocrPolicyMsg_t * msg, u8 returnDetail) {
    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_EVT_CREATE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EVT_CREATE
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
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
    case PD_MSG_EDTTEMP_CREATE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_EDTTEMP_CREATE
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
    case PD_MSG_DB_CREATE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_CREATE
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
    case PD_MSG_DEP_DYNREMOVE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
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
    case PD_MSG_DB_RELEASE:
    {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_DB_RELEASE
        PD_MSG_FIELD_O(returnDetail) = returnDetail;
#undef PD_MSG
#undef PD_TYPE
    break;
    }
    default:
    ocrAssert("Unhandled message type in setReturnDetail");
    break;
    }
}

static pdEvent_t * createDeferredMT(ocrPolicyDomain_t * pd, ocrPolicyMsg_t * msg) {
    pdEvent_t * pdEvent;
    RESULT_ASSERT(pdCreateEvent(pd, &pdEvent, PDEVT_TYPE_MSG, 0), ==, 0);
    pdEvent->properties |= PDEVT_GC | PDEVT_DESTROY_DEEP;
    ((pdEventMsg_t *) pdEvent)->msg = msg;
    DPRINTF(DEBUG_LVL_VERB, "Created micro-task for deferred API call: %p\n", pdEvent);
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    // The event will be marked ready whenever the previous MT is completed.
    // This is to the exception of the first deferred event that's made ready
    // when the EDT user code has finished executing. (Can do in parallel in future)
#else
    //TODO-MT-DEFERRED: I was assuming the pdEvent would be eligible for scheduling if
    //marked ready. Is this currently working because the pdCreateEvent doesn't reserve in the table ?
    RESULT_ASSERT(pdMarkReadyEvent(pd, pdEvent), ==, 0);
#endif
    return pdEvent;
}

static ocrPolicyMsg_t * hcPdDeferredMarshall(ocrPolicyDomain_t *pd, ocrPolicyMsg_t *msg) {
    u64 baseSize = 0, marshalledSize = 0;
    ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, 0);
    u64 fullMsgSize = baseSize + marshalledSize;
    ocrPolicyMsg_t * msgCopy = (ocrPolicyMsg_t *) pd->fcts.pdMalloc(pd, fullMsgSize);
    initializePolicyMessage(msgCopy, fullMsgSize);
    ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)msgCopy, MARSHALL_DUPLICATE);
    return msgCopy;
}

#define DEFERRED_MSG_QUEUE_SIZE_DEFAULT 4

static void hcPdDeferredRecord(ocrPolicyDomain_t *pd, ocrPolicyMsg_t *msg) {
    ocrTask_t *curTask = NULL;
    getCurrentEnv(NULL, NULL, &curTask, NULL);
    ocrAssert(curTask != NULL);
    ocrTaskHc_t * hcTask = (ocrTaskHc_t *) curTask;
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
#else
    if (hcTask->evts == NULL) {
        hcTask->evts = newBoundedQueue(pd, DEFERRED_MSG_QUEUE_SIZE_DEFAULT);
    }
    if (queueIsFull(hcTask->evts)) {
        hcTask->evts = queueDoubleResize(hcTask->evts, /*freeOld=*/true);
    }
#endif
    pdEvent_t * pdEvent = createDeferredMT(pd, msg);
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    pdStrand_t * tailStrand;
    RESULT_ASSERT(pdGetNewStrand(pd, &tailStrand, pd->strandTables[PDSTT_EVT], pdEvent, 0 /*unused*/), ==, 0);
    pdAction_t * processAction = pdGetProcessMessageAction(msg->destLocation == pd->myLocation?NP_WORK:NP_COMM);
    RESULT_ASSERT(pdEnqueueActions(pd, tailStrand, 1, &processAction, false/*clear hold*/), ==, 0);
    RESULT_ASSERT(pdUnlockStrand(tailStrand), ==, 0);
    if (hcTask->evtHead == NULL) {
        hcTask->evtHead = pdEvent;
    } else {
        // Chain the previous event to the new one
        pdStrand_t * oldTailStrand = hcTask->tailStrand;
        pdAction_t* satisfyAction = pdGetMarkReadyAction(pdEvent);
        RESULT_ASSERT(pdLockStrand(oldTailStrand, 0), ==, 0);
        RESULT_ASSERT(pdEnqueueActions(pd, oldTailStrand, 1, &satisfyAction, true/*clear hold*/), ==, 0);
        RESULT_ASSERT(pdUnlockStrand(oldTailStrand), ==, 0);
    }
    hcTask->tailStrand = tailStrand;
#else
    // This is to be substituted to chaining MT
    queueAddLast(hcTask->evts, (void *) pdEvent);
#endif
}

static u8 getDeferredGuid(ocrPolicyDomain_t * pd, ocrGuid_t * guid, ocrGuidKind kind, ocrLocation_t targetLocation) {
    ocrPolicyMsg_t msgGuid;
    getCurrentEnv(NULL, NULL, NULL, &msgGuid);
#define PD_MSG (&msgGuid)
#define PD_TYPE PD_MSG_GUID_CREATE
    msgGuid.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    // We allocate everything in the meta-data to keep things simple
    PD_MSG_FIELD_I(size) = 0;
    PD_MSG_FIELD_I(kind) = kind;
    PD_MSG_FIELD_I(targetLoc) = targetLocation;
    PD_MSG_FIELD_I(properties) = 0; // Not valid and do not record
    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msgGuid, true));
    ocrAssert(PD_MSG_FIELD_IO(guid.metaDataPtr) == NULL);
    // Set-up base structures
    *guid = PD_MSG_FIELD_IO(guid.guid);
    return PD_MSG_FIELD_O(returnDetail);
#undef PD_MSG
#undef PD_TYPE
}

// Returns true if the operation has been deferred. Otherwise the PD must process the message.
static u8 hcPdDeferredProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {
    // ocrPolicyDomainHc_t * dself = (ocrPolicyDomainHc_t *) self;

    // Note: Here we loose the 'isBlocking' argument value but it's ok because in this implementation
    // of deferred all the messages are rooted in the user API which doesn't support asynchrony anyway.
    ocrAssert(msg->type & PD_MSG_DEFERRABLE);

    // Disable the deferrable flag so that next time we do process the messages fully.
    msg->type &= (~PD_MSG_DEFERRABLE);

#define PD_MSG msg
    switch(msg->type & PD_MSG_TYPE_ONLY) {
        case PD_MSG_DB_CREATE: {
#define PD_TYPE PD_MSG_DB_CREATE
            //O(returnDetail)
            //O(ptr)
            //IO(guid)
            //TODO-deferred for NO_ACQUIRE we could actually defer
            if (PD_MSG_FIELD_IO(properties) & DB_PROP_NO_ACQUIRE) {
                ocrAssert(false && "Implement deferred DB create in DB_PROP_NO_ACQUIRE"); // Just as a reminder
            } else {
                return OCR_EPERM;
            }
            // Note there is a subsequent PD_MSG_DEP_DYNADD in the user/rt interface.
            // It's ok to not do dynadd immediately as the 'uses' for that information
            // will be deferred too. Hence, the operation's side-effects will be seen
            // in the same order.
#undef PD_TYPE
        break;
        }
        case PD_MSG_DEP_DYNADD:
        case PD_MSG_DEP_DYNREMOVE: {
            //TODO-DEFERRED: As currently setup in the ocrDbDestroy of the user/rt interface, we need
            // to get the output of DYN_REMOVE to feed it as an input parameter to DB_FREE.
            // Two possible approaches:
            // 1) Forbid DYNs to be deferred so that the accounting is done properly and DB_FREE is
            //    called with the proper parameter. Wrt resiliency it's ok because those messages do not have
            //    side-effects visible outside the EDT.
            // 2) Another approach would be to have a deferred processing of DYN_REMOVE that in deferred mode
            //    also does a subsequent DB_FREE
            return OCR_EPERM; //TODO: do minimal work and defer
        }
        case PD_MSG_DB_RELEASE: {
            // Similar issue to ocrDbDestroy. Depending on the output of DB_RELEASE, the user/rt interface
            // does a DYN_REMOVE. However, here we really must defer the call as it's expensive in the
            // current implementation.
            // So go with approach 2) mentioned above to invoke PD_MSG_DEP_DYNREMOVE after the facts
        break;
        }
        case PD_MSG_WORK_CREATE: {
#define PD_TYPE PD_MSG_WORK_CREATE
            //O(returnDetail)
            //IO(guid)
            //IO(paramc)
            //IO(paramv)
            //IO(outputEvent)
            //TODO-DEFERRED: Limitations EDTs: Proposal to remove EDT_PARAM_DEF from OCR User APIs
            if ((PD_MSG_FIELD_IO(paramc) == EDT_PARAM_DEF) || (PD_MSG_FIELD_IO(depc) == EDT_PARAM_DEF)) {
                //TODO-DEFERRED: This is going to be an issue if I've deferred the template creation then
                //I can't issue this call since the other end will try to resolve the template and it doesn't exists.
                DPRINTF(DEBUG_LVL_WARN, "[DFRD] Warning: EDT creation cannot be deferred because of EDT_PARAM_DEF being used\n");
                return OCR_EPERM;
            }
            if(PD_MSG_FIELD_I(properties) & GUID_PROP_IS_LABELED) {
                if(ocrGuidIsUninitialized(PD_MSG_FIELD_IO(outputEvent.guid))) {
                    DPRINTF(DEBUG_LVL_WARN, "Labeled EDTs cannot request an output event\n");
                    ocrAssert(0);
                    setDeferredReturnDetail(msg, OCR_EINVAL);
                    return OCR_EINVAL;
                }
                if(PD_MSG_FIELD_I(properties) & EDT_PROP_FINISH) {
                    DPRINTF(DEBUG_LVL_WARN, "[DFRD] Labeled EDT has the FINISH property -- call cannot be deferred\n");
                    return OCR_EPERM;
                }
                if(!ocrGuidIsNull(PD_MSG_FIELD_I(parentLatch.guid))) {
                    DPRINTF(DEBUG_LVL_WARN, "[DFRD] Labeled EDT is in a FINISH scope -- call cannot be deferred\n");
                    DPRINTF(DEBUG_LVL_WARN, "Labeled EDTs in a finish scope are dangerous and will only be registered by the winner of the creation\n");
                    return OCR_EPERM;
                }
            }
            // Only generate GUIDs for two-way EDTs
            if (msg->type & PD_MSG_REQ_RESPONSE) {
                ocrGuid_t edtGuid;
                getDeferredGuid(self, &edtGuid, OCR_GUID_EDT, msg->destLocation);
                PD_MSG_FIELD_IO(guid.guid) = edtGuid;
                PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
                ocrGuid_t oeGuid = NULL_GUID;
                if (ocrGuidIsUninitialized(PD_MSG_FIELD_IO(outputEvent.guid))) {
                    ocrEventTypes_t evtType = OCR_GUID_EVENT_ONCE;
                    getDeferredGuid(self, &oeGuid, evtType, msg->destLocation);
                    PD_MSG_FIELD_IO(outputEvent.guid) = oeGuid;
                    PD_MSG_FIELD_IO(outputEvent.metaDataPtr) = NULL;
                }
                PD_MSG_FIELD_I(properties) |= (GUID_PROP_ISVALID); // Record the GUID is valid for subsequent processMessage
                DPRINTF(DEBUG_LVL_VVERB, "[DFRD] Creating deferred EDT (GUID: "GUIDF") (OE-GUID: "GUIDF") msg->destLocation=%"PRIu64"\n", GUIDA(edtGuid), GUIDA(oeGuid), (u64)msg->destLocation);
            }
#undef PD_TYPE
        break;
        }
        case PD_MSG_EVT_CREATE: {
#define PD_TYPE PD_MSG_EVT_CREATE
            //O(returnDetail)
            //IO(guid)
            //TODO-DEFERRED: need to think about labelled
            ocrGuid_t evtGuid;
            ocrGuidKind kind = eventTypeToGuidKind(PD_MSG_FIELD_I(type));
            getDeferredGuid(self, &evtGuid, kind, msg->destLocation);
            PD_MSG_FIELD_IO(guid.guid) = evtGuid;
            PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(properties) |= (GUID_PROP_ISVALID); // Record the GUID is valid for subsequent processMessage
#undef PD_TYPE
        break;
        }
        case PD_MSG_EDTTEMP_CREATE: {
#define PD_TYPE PD_MSG_EDTTEMP_CREATE
            //O(returnDetail)
            //IO(guid)
            //TODO-DEFERRED see current limitation for EDT_PARAM_DEF
            return OCR_EPERM; //TODO: do minimal work and defer
#undef PD_TYPE
        break;
        }
    }
#undef PD_MSG
    // Just defer the call
    ocrPolicyMsg_t * msgCopy = hcPdDeferredMarshall(self, msg);
    hcPdDeferredRecord(self, msgCopy);
    DPRINTF(DEBUG_LVL_VVERB, "[DFRD] Creating copy msg %p of original %p\n", msgCopy, msg);
    //TODO-DEFERRED: for now keep returning zero but we may want to get an error code.
    //If we do so, we'll need to update the user/rt interface too
    setDeferredReturnDetail(msg, 0);
    return 0;
}
#endif

u8 hcPolicyDomainProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {
    START_PROFILE(pd_hc_ProcessMessage);
    u8 returnCode = 0;

    // This assert checks the call's parameters are correct
    // - Synchronous processMessage calls always deal with a REQUEST.
    // - Asynchronous message processing allows for certain type of message
    //   to have a RESPONSE processed.
    ocrAssert(((msg->type & PD_MSG_REQUEST) && !(msg->type & PD_MSG_RESPONSE))
        || ((msg->type & PD_MSG_RESPONSE) && ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE)));

    // The message buffer size should always be greater or equal to the
    // max of the message in and out sizes, otherwise a write on the message
    // as a response overflows.
#ifdef OCR_ASSERT
    if (msg->type & PD_MSG_REQ_RESPONSE) {
        u64 baseSizeIn = ocrPolicyMsgGetMsgBaseSize(msg, true);
        u64 baseSizeOut = ocrPolicyMsgGetMsgBaseSize(msg, false);
        ocrAssert(((baseSizeIn < baseSizeOut) && (msg->bufferSize >= baseSizeOut)) || (baseSizeIn >= baseSizeOut));
    }
#endif

    //TODO-DEFERRED:
#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
    if (msg->type & PD_MSG_DEFERRABLE) {
        return hcPdDeferredProcessMessage(self, msg, isBlocking);
    }
#else
    if(msg->type & PD_MSG_DEFERRABLE) {
        returnCode = hcPdDeferredProcessMessage(self, msg, isBlocking);
        // OCR_EPERM means drop-through and continue processing (can't defer)
        // 0 means call was deferred
        // Other error codes should be returned as usual
        if(returnCode != OCR_EPERM)
            return returnCode;
        returnCode = 0;
    }
#endif
#endif

    hcSchedNotifyPreProcessMessage(self, msg);

    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_DB_CREATE: {
        START_PROFILE(pd_hc_DbCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        // BUG #584: Add properties whether DB needs to be acquired or not
        // This would impact where we do the PD_MSG_MEM_ALLOC for ex
        // For now we deal with both USER and RT dbs the same way
        ocrAssert(PD_MSG_FIELD_I(dbType) == USER_DBTYPE || PD_MSG_FIELD_I(dbType) == RUNTIME_DBTYPE);

        // We do not acquire a data-block in two cases:
        //  - it was created with a labeled-GUID in non "trust me" mode. This is because it would be difficult
        //    to handle cases where both EDTs create it but only one acquires it (particularly
        //    in distributed case
        //  - if the user does not want to acquire the data-block (DB_PROP_NO_ACQUIRE)
        bool doNotAcquireDb = PD_MSG_FIELD_IO(properties) & DB_PROP_NO_ACQUIRE;
        doNotAcquireDb |= ((PD_MSG_FIELD_IO(properties) & GUID_PROP_CHECK) == GUID_PROP_CHECK);
        doNotAcquireDb |= ((PD_MSG_FIELD_IO(properties) & GUID_PROP_BLOCK) == GUID_PROP_BLOCK);
        ocrFatGuid_t tEdt = PD_MSG_FIELD_I(edt);
        #define PRESCRIPTION 0x10LL
        //TODO-MD-DBNOACQ: 'no acquire' flag is handled upstream by forwarding the msg directly to the recipient PD
        ocrDataBlockFactory_t * dbFactory = (ocrDataBlockFactory_t*) self->factories[self->datablockFactoryIdx];
        void * ptr = NULL; // request memory to be allocated
        PD_MSG_FIELD_O(returnDetail) = dbFactory->instantiate(dbFactory, &(PD_MSG_FIELD_IO(guid)),
                                                        self->allocators[0]->fguid, self->fguid,
                                                        PD_MSG_FIELD_IO(size), &ptr,
                                                        PD_MSG_FIELD_I(hint), PD_MSG_FIELD_IO(properties), NULL);
        if(PD_MSG_FIELD_O(returnDetail) == 0) {
            ocrDataBlock_t *db = PD_MSG_FIELD_IO(guid.metaDataPtr);
            if(db == NULL) {
                DPRINTF(DEBUG_LVL_WARN, "DB Create failed for size %"PRIx64"\n", PD_MSG_FIELD_IO(size));
            } else {
                DPRINTF(DEBUG_LVL_VERB, "Creating a datablock of size %"PRIu64" @ %p (GUID: "GUIDF") (edt GUID: "GUIDF")\n",
                        db->size, db->ptr, GUIDA(db->guid), GUIDA(tEdt.guid));
                OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_DATABLOCK, OCR_ACTION_CREATE, traceDataCreate, db->guid, db->size);
            }
            ocrAssert(db);
            if(doNotAcquireDb) {
                DPRINTF(DEBUG_LVL_INFO, "Not acquiring DB since disabled by property flags\n");
                PD_MSG_FIELD_O(ptr) = NULL;
            } else {
                ocrAssert(db->fctId == ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->factoryId);
                PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.acquire(
                    db, &(PD_MSG_FIELD_O(ptr)), tEdt, self->myLocation, EDT_SLOT_NONE, DB_MODE_RW, !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE),
                    (u32) DB_MODE_RW);
                // Set the default mode in the response message for the caller
                PD_MSG_FIELD_IO(properties) |= DB_MODE_RW;
            }
        } else {
            // Couldn't create the datablock
            PD_MSG_FIELD_O(ptr) = NULL;
            if (!(PD_MSG_FIELD_IO(properties) & GUID_PROP_IS_LABELED)) {
                DPRINTF(DEBUG_LVL_WARN, "PD_MSG_DB_CREATE returning NULL for size %"PRId64"\n", (u64) PD_MSG_FIELD_IO(size));
                DPRINTF(DEBUG_LVL_WARN, "*** WARNING : OUT-OF-MEMORY ***\n");
                DPRINTF(DEBUG_LVL_WARN, "*** Please increase sizes in *ALL* MemPlatformInst, MemTargetInst, AllocatorInst sections.\n");
                DPRINTF(DEBUG_LVL_WARN, "*** Same amount increasing is recommended.\n");
            }
        }
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_DESTROY: {
        // Should never ever be called. The user calls free and internally
        // this will call whatever it needs (most likely PD_MSG_MEM_UNALLOC)
        // This would get called when DBs move for example
        ocrAssert(0);
        break;
    }

    case PD_MSG_DB_ACQUIRE: {
        START_PROFILE(pd_hc_DbAcquire);
        if (msg->type & PD_MSG_REQUEST) {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_ACQUIRE
            localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
            //BUG #273 rely on the call to set the fatguid ptr to NULL and not crash if edt acquiring is not local
            localDeguidify(self, &(PD_MSG_FIELD_IO(edt)));
            ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr));
            ocrAssert(isDatablockGuid(self, PD_MSG_FIELD_IO(guid)));
            ocrAssert(db != NULL);
            ocrAssert(db->fctId == ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->factoryId);
            if (msg->type & PD_MSG_REQ_RESPONSE) {
                PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.acquire(
                    db, &(PD_MSG_FIELD_O(ptr)), PD_MSG_FIELD_IO(edt), PD_MSG_FIELD_IO(destLoc), PD_MSG_FIELD_IO(edtSlot),
                    (ocrDbAccessMode_t) (PD_MSG_FIELD_IO(properties) & (u32)DB_ACCESS_MODE_MASK),
                    !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE), PD_MSG_FIELD_IO(properties));
                //BUG #273 db: modify the acquire call if we agree on changing the api
                PD_MSG_FIELD_O(size) = db->size;
                // conserve acquire's msg properties and add the DB's one.
                //BUG #273: This is related to bug #273
                PD_MSG_FIELD_IO(properties) |= db->flags;
                // Acquire message can be asynchronously responded to
                if (PD_MSG_FIELD_O(returnDetail) == OCR_EBUSY) {
                    // Processing not completed
                    returnCode = OCR_EPEND;
                } else {
                    // Something went wrong in dbAcquire
                    if (PD_MSG_FIELD_O(returnDetail) != 0) {
                        DPRINTF(DEBUG_LVL_WARN, "DB Acquire failed for guid "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
                    }
                    ocrAssert(PD_MSG_FIELD_O(returnDetail) == 0);
                    DPRINTF(DEBUG_LVL_INFO, "DB guid "GUIDF" of size %"PRIu64" acquired by EDT "GUIDF"\n",
                            GUIDA(db->guid), db->size, GUIDA(PD_MSG_FIELD_IO(edt.guid)));

                    OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_DATA_ACQUIRE, traceTaskDataAcquire, PD_MSG_FIELD_IO(edt.guid),
                                db->guid, db->size);
                    msg->type &= ~PD_MSG_REQUEST;
                    msg->type |= PD_MSG_RESPONSE;
                }
            } else {
                //TODO-MD-DBRTACQ
                // This is re-processing an acquire that was gated on the MD being brought in.
                // In the eventuality the access is granted, there's still no calling context alive
                // to process the response. Hence, the MD impl will issue a response to be processed
                // by the policy-domain.
                PD_MSG_FIELD_IO(properties) |= DB_PROP_ASYNC_ACQ;
                void * ptr;
                ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.acquire(
                    db, &ptr, PD_MSG_FIELD_IO(edt), PD_MSG_FIELD_IO(destLoc), PD_MSG_FIELD_IO(edtSlot),
                    (ocrDbAccessMode_t) (PD_MSG_FIELD_IO(properties) & (u32)DB_ACCESS_MODE_MASK),
                    !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE), PD_MSG_FIELD_IO(properties));
            }
#undef PD_MSG
#undef PD_TYPE
        } else {
            ocrAssert(msg->type & PD_MSG_RESPONSE);
            // asynchronous callback on acquire, reading response
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_ACQUIRE
            ocrFatGuid_t edtFGuid = PD_MSG_FIELD_IO(edt);
            ocrFatGuid_t dbFGuid = PD_MSG_FIELD_IO(guid);
            u32 edtSlot = PD_MSG_FIELD_IO(edtSlot);
            ocrAssert(edtSlot != EDT_SLOT_NONE); //BUG #190
            localDeguidify(self, &edtFGuid);
            // At this point the edt MUST be local as well as the acquire's message DB ptr
            ocrTask_t* task = (ocrTask_t*) edtFGuid.metaDataPtr;
#ifdef TG_STAGING
            PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.dependenceResolved(task, dbFGuid.guid, PD_MSG_FIELD_O(ptr), edtSlot, PD_MSG_FIELD_O(size));
#else
            PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.dependenceResolved(task, dbFGuid.guid, PD_MSG_FIELD_O(ptr), edtSlot);
#endif
            OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_DATA_ACQUIRE, traceTaskDataAcquire, PD_MSG_FIELD_IO(edt.guid), dbFGuid.guid, PD_MSG_FIELD_O(size));
#undef PD_MSG
#undef PD_TYPE
        }
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_RELEASE: {
        // Call the appropriate release function
        START_PROFILE(pd_hc_DbRelease);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_RELEASE
        localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
        localDeguidify(self, &(PD_MSG_FIELD_I(edt)));
        ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr));
        ocrAssert(isDatablockGuid(self, PD_MSG_FIELD_IO(guid)));
        ocrAssert(db != NULL);
        ocrAssert(db->fctId == ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->factoryId);
        ocrGuid_t edtGuid __attribute__((unused)) =  PD_MSG_FIELD_I(edt.guid);
        PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.release(
            db, PD_MSG_FIELD_I(edt), PD_MSG_FIELD_I(srcLoc), !!(PD_MSG_FIELD_I(properties) & DB_PROP_RT_ACQUIRE));
        DPRINTF(DEBUG_LVL_INFO, "DB guid "GUIDF" of size %"PRIu64" released by EDT "GUIDF"\n",
                GUIDA(db->guid), db->size, GUIDA(edtGuid));
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_DATA_RELEASE, traceTaskDataRelease, edtGuid, db->guid, db->size);

#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_FREE: {
        // Call the appropriate free function
        START_PROFILE(pd_hc_DbFree);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_FREE
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        localDeguidify(self, &(PD_MSG_FIELD_I(edt)));
        ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
        ocrAssert(isDatablockGuid(self, PD_MSG_FIELD_I(guid)));
        ocrAssert(db != NULL);
        ocrAssert(db->fctId == ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->factoryId);
        ocrAssert(!(msg->type & PD_MSG_REQ_RESPONSE));
        //Save a copy of the DB guid for DPRINTF() and tracing before the free call
        ocrGuid_t dbGuid = PD_MSG_FIELD_I(guid).guid;
        PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.free(
            db, PD_MSG_FIELD_I(edt), PD_MSG_FIELD_I(srcLoc), PD_MSG_FIELD_I(properties));
        if(PD_MSG_FIELD_O(returnDetail)!=0)
            DPRINTF(DEBUG_LVL_WARN, "DB Free failed for guid "GUIDF"\n", GUIDA(dbGuid));
        else{
            DPRINTF(DEBUG_LVL_INFO,
                    "DB guid: "GUIDF" Destroyed\n", GUIDA(dbGuid));
            OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_DATABLOCK, OCR_ACTION_DESTROY, traceDataDestroy, dbGuid);

        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        ocrAssert(!(msg->type & PD_MSG_REQ_RESPONSE));
        // msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MEM_ALLOC: {
        START_PROFILE(pd_hc_MemAlloc);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MEM_ALLOC
        u64 tSize = PD_MSG_FIELD_I(size);
        ocrMemType_t tMemType = PD_MSG_FIELD_I(type);
        u32 properties = PD_MSG_FIELD_I(properties);
        PD_MSG_FIELD_O(allocatingPD.metaDataPtr) = self;
        PD_MSG_FIELD_O(returnDetail) = hcMemAlloc(
            self, &(PD_MSG_FIELD_O(allocator)), tSize,
            tMemType, &(PD_MSG_FIELD_O(ptr)), properties);
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MEM_UNALLOC: {
        START_PROFILE(pd_hc_MemUnalloc);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MEM_UNALLOC
        // This was set but it is in. Transforming into ASSERT but disabling for now
        // because I think we still have issues with allocator code
        //ASSERT(PD_MSG_FIELD_I(allocatingPD.metaDataPtr) == self);
        ocrFatGuid_t allocatorGuid = PD_MSG_FIELD_I(allocator);
        PD_MSG_FIELD_O(returnDetail) = hcMemUnAlloc(
            self, &allocatorGuid, PD_MSG_FIELD_I(ptr), PD_MSG_FIELD_I(type));
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }
    case PD_MSG_METADATA_COMM:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_METADATA_COMM
        ocrGuid_t guid = PD_MSG_FIELD_I(guid);
        // All of the pull/push requests are subject to brokering
        ocrGuidKind guidKind;
        self->guidProviders[0]->fcts.getKind(self->guidProviders[0], guid, &guidKind);

        // - Resolve the factory pointer (from the kind and factoryId)
        u32 factoryId = PD_MSG_FIELD_I(factoryId);
        ocrObjectFactory_t * factory = self->factories[factoryId];
        bool shouldProcess = (guidKind == OCR_GUID_DB);
#ifdef ENABLE_EXTENSION_COLLECTIVE_EVT
        shouldProcess |= (guidKind == OCR_GUID_EVENT_COLLECTIVE);
#endif
        if (shouldProcess) {
            // Rely on the DB's MD 'process' implementation. The call may be asynchronous.
            u64 val = 0;
            self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid, &val, NULL, MD_LOCAL, NULL);
            ocrObject_t * mdPtr = (ocrObject_t *) val;             // ASSERT(val != ((u64)0xffffffffffffffff));
            // This is potentially asynchronous as the MD may not be able to carry out the operation immediately
            returnCode = factory->process(factory, guid, mdPtr, msg);
            // If pending we return here as it may indicate the msg is now
            // being used by the MD and can be potentially concurrently freed
            if (returnCode == OCR_EPEND) {
                RETURN_PROFILE(OCR_EPEND);
            }
        } else {
            // rely on serialize/deserialize that are synchronously called here
            if (PD_MSG_FIELD_I(direction) == MD_DIR_PULL) {
                ocrAssert(msg->srcLocation != msg->destLocation);
                // If pulling, the MD must exist in the GP
                u64 val = 0;
                self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid, &val, NULL, MD_LOCAL, NULL);
                ocrObject_t * src = (ocrObject_t *) val;
                ocrAssert(src != NULL);
                //TODO-MD-SIZE
                //There may be a need for asynchrony here too as the MD may not be able to accomodate the pull
                //request immediately. Additionally decoupling size and serialize may be tricky since the md
                //size to serialize is function of both the mode, direction, operations and its arguments.
                u64 mode = PD_MSG_FIELD_I(mode);
                u64 mdSize;
                factory->mdSize(src, mode, &mdSize);
                // Creating a response message to store a serialized copy of the MD.
                // TODO: Wasting some space with sizeof(ocrPolicyMsg_t), get the real size for COMM message ?
                // TODO: Also we may want to reuse the request whenever possible, especially if mdSize == 0
                // Use allocPolicyMsg so that alignment is coherent with marshalling code
                u64 msgSize = (mdSize + sizeof(ocrPolicyMsg_t));
                ocrPolicyMsg_t * response = (ocrPolicyMsg_t *) allocPolicyMsg(self, &msgSize);
                initializePolicyMessage(response, msgSize);
    #undef PD_MSG
    #undef PD_TYPE
    #define PD_MSG response
    #define PD_TYPE PD_MSG_METADATA_COMM
                void * destBuffer = (void *) (&PD_MSG_FIELD_I(payload));
    #undef PD_MSG
    #undef PD_TYPE
                // Note: the goal of destSize is to be able to provide a pre-allocated
                // buffer to avoid copying the metadata into a message later on.
                //TODO-MD-SER-SIZE: This is a little ill-defined because if the buffer is too small,
                // the serialize doesn't know it needs to account for the message header.
                u64 destSize = mdSize;
                factory->serialize(factory, guid, src, &mode, msg->srcLocation, &destBuffer, &destSize);
                ocrAssert(destSize == mdSize);
                // Two scenarios are possible here:
                // A) We can process the message synchronously and set up the response.
                //    If the caller is the distributed policy domain it can send the response right away.
                // B) Asynchronous processing: return error code PEND / or continuation. At some point
                //    the message is processed fully and the response buffer is ready. Looking at the
                //    src/dst we can determine if the message needs to be sent out or the response
                //    locally processed.
                // => Currently only support scenario 'A'
    #define PD_MSG msg
    #define PD_TYPE PD_MSG_METADATA_COMM
                ocrLocation_t srcLocation = msg->srcLocation;
                ocrLocation_t destLocation = msg->destLocation;
                // Set the response message into the request's response field
                //TODO-MD-PD-SEND
                //- What the 'response' field is trying to solve is how an independent asynchronous
                //response can be sent from this PD implementation. i.e. we would need to have a
                //send here for the response to be sent out.
                //- Writing to the request's 'response' field is borderline since
                //the pull message is one-way hc-dist-policy is not supposed to look at field
                //once this processMessage returns.
                PD_MSG_FIELD_I(response) = response;
    #undef PD_MSG
    #undef PD_TYPE
                // Setup the response header message
                response->srcLocation = destLocation;
                response->destLocation = srcLocation;
                response->type = PD_MSG_METADATA_COMM | PD_MSG_REQUEST;
                ocrAssert(response->srcLocation != response->destLocation);
    #define PD_MSG response
    #define PD_TYPE PD_MSG_METADATA_COMM
                PD_MSG_FIELD_I(guid) = guid;
                PD_MSG_FIELD_I(direction) = MD_DIR_PUSH;
                PD_MSG_FIELD_I(op) = 0; /*ocrObjectOperation_t*/
                PD_MSG_FIELD_I(mode) = mode;
                PD_MSG_FIELD_I(factoryId) = factoryId;
                PD_MSG_FIELD_I(sizePayload) = mdSize;
                PD_MSG_FIELD_I(response) = NULL;
                PD_MSG_FIELD_I(mdPtr) = NULL;
                //PD_MSG_FIELD_I(payload) has already been written to by the call to serialize
    #undef PD_MSG
    #undef PD_TYPE
            } else { /*MD_DIR_PUSH*/
#define PD_MSG msg
#define PD_TYPE PD_MSG_METADATA_COMM
                ocrAssert(PD_MSG_FIELD_I(direction) == MD_DIR_PUSH);
                // Receiving a metadata update
                ocrGuid_t guid = PD_MSG_FIELD_I(guid);
                ocrGuidKind guidKind;
                self->guidProviders[0]->fcts.getKind(self->guidProviders[0], guid, &guidKind);
                if (guidKind == OCR_GUID_EDT) {
                    DPRINTF(DEBUG_LVL_VVERB, "Processing incoming MD_COMM PUSH for OCR_GUID_EDT\n");
                    // Currently only support MD_MOVE for EDTs that are translated into a PUSH operation
    #ifdef OCR_ASSERT
                    u64 val = 0;
                    self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid, &val, NULL, MD_LOCAL, NULL);
                    ocrAssert(val == 0);
                    ocrAssert(PD_MSG_FIELD_I(direction) == MD_DIR_PUSH);
    #endif
                    // - Resolve the factory pointer
                    ocrObjectFactory_t * factory = self->factories[self->taskFactoryIdx];
                    ocrObject_t * mdPtr = NULL;
                    factory->deserialize(factory, guid, &mdPtr, PD_MSG_FIELD_I(mode), (void *) &PD_MSG_FIELD_I(payload), (u64) PD_MSG_FIELD_I(sizePayload));
                    ocrAssert((mdPtr != NULL) && "error: PD_MSG_METADATA_COMM deserialize operation failed");
                    // NOTE: Implementation ensures there's a single message generated for the initial clone
                    // so that this registration is not concurrent with others for the same GUID
                    DPRINTF(DEBUG_LVL_VVERB, "registerGuid called after deserialize\n");
                    // Notify the scheduler of the EDT move
                    ocrPolicyDomain_t *pd = NULL;
                    PD_MSG_STACK(msgNotify);
                    getCurrentEnv(&pd, NULL, NULL, &msgNotify);
        #undef PD_MSG
        #undef PD_TYPE
        #define PD_MSG (&msgNotify)
        #define PD_TYPE PD_MSG_SCHED_NOTIFY
                    msgNotify.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
                    PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_EDT_SATISFIED;
                    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_SATISFIED).guid.guid = guid;
                    PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_SATISFIED).guid.metaDataPtr = mdPtr;
                    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msgNotify, false));
                    u8 res = PD_MSG_FIELD_O(returnDetail);
                    if (res) {
                        ocrAssert(res == OCR_ENOP);
                        ocrTask_t * task = (ocrTask_t *) mdPtr;
#ifdef TG_STAGING
                        RESULT_ASSERT(((ocrTaskFactory_t *)pd->factories[task->fctId])->fcts.dependenceResolved(task, NULL_GUID, NULL, EDT_SLOT_NONE, 0), ==, 0);
#else
                        RESULT_ASSERT(((ocrTaskFactory_t *)pd->factories[task->fctId])->fcts.dependenceResolved(task, NULL_GUID, NULL, EDT_SLOT_NONE), ==, 0);
#endif
                    }
        #undef PD_MSG
        #undef PD_TYPE
                } else {
    #define PD_MSG msg
    #define PD_TYPE PD_MSG_METADATA_COMM
    #ifdef OCR_ASSERT
                    ocrGuid_t guid = PD_MSG_FIELD_I(guid);
                    ocrGuidKind guidKind;
                    self->guidProviders[0]->fcts.getKind(self->guidProviders[0], guid, &guidKind);
                    ocrAssert(guidKind & OCR_GUID_EVENT);
    #endif
                    DPRINTF(DBG_LVL_MDEVT, "md: receive MD_DIR_PUSH mode=%"PRIu64" for "GUIDF"\n", PD_MSG_FIELD_I(mode), GUIDA(PD_MSG_FIELD_I(guid)));
                    MdProxy_t * proxy = NULL;
                    u64 val = 0;
                    // Get the metadata pointer
                    u8 retCode = self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid, &val, NULL, MD_LOCAL, &proxy);
                    // ASSERT(retCode == 0); => This can be EPEND if the push message we're receiving is the metadata to be stored as 'val'
                    ocrObject_t * dest = (void *) val;
                    // Note: 'dest' is NULL means it's an initial clone. We have nothing to deserialize too so the
                    // deserialize code has to allocate memory to deserialize the payload to.
                    factory->deserialize(factory, guid, &dest, PD_MSG_FIELD_I(mode), (void *) &PD_MSG_FIELD_I(payload), (u64) PD_MSG_FIELD_I(sizePayload));
                    //If the ptr is null we don't know about the MD and install it in the PD
                    //Proxy can be null when deserialize receives metadata update for an object whose current PD is home.
                    if (proxy == NULL) {
                        ocrLocation_t cmpLoc;
                        ocrAssert(self->guidProviders[0]->fcts.getLocation(self->guidProviders[0], guid, &cmpLoc) == 0);
                        ocrAssert(cmpLoc == self->myLocation);
                    }
                    if (proxy && (proxy->ptr == ((u64) 0))) {
                        ocrAssert((dest != NULL) && "error: PD_MSG_METADATA_COMM deserialize operation failed");
                        // NOTE: Implementation ensures there's a single message generated for the initial clone
                        // so that this registration is not concurrent with others for the same GUID
                        retCode = self->guidProviders[0]->fcts.registerGuid(self->guidProviders[0], guid, (u64) dest);
                        ocrAssert(retCode == 0);
                    }
    #undef PD_MSG
    #undef PD_TYPE
                } // push edt or other
            } // end PULL/PUSH
        }// end kind if/else
        break;
    }
    case PD_MSG_WORK_CREATE: {
        START_PROFILE(pd_hc_WorkCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
        localDeguidify(self, &(PD_MSG_FIELD_I(templateGuid)));
        if(PD_MSG_FIELD_I(templateGuid.metaDataPtr) == NULL)
            DPRINTF(DEBUG_LVL_WARN, "Invalid template GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(templateGuid.guid)));
        ocrAssert(PD_MSG_FIELD_I(templateGuid.metaDataPtr) != NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(currentEdt)));
        localDeguidify(self, &(PD_MSG_FIELD_I(parentLatch)));

#ifdef ENABLE_EXTENSION_PERF
        ocrTask_t *curEdt = PD_MSG_FIELD_I(currentEdt).metaDataPtr;
        if(curEdt) curEdt->swPerfCtrs[PERF_EDT_CREATES - PERF_HW_MAX]++;
#endif
        ocrFatGuid_t *outputEvent = &(PD_MSG_FIELD_IO(outputEvent));
        if((PD_MSG_FIELD_I(workType) != EDT_USER_WORKTYPE) && (PD_MSG_FIELD_I(workType) != EDT_RT_WORKTYPE)) {
            // This is a runtime error and should be reported as such
            DPRINTF(DEBUG_LVL_WARN, "Invalid worktype %"PRIx32"\n", PD_MSG_FIELD_I(workType));
        }

        ocrAssert((PD_MSG_FIELD_I(workType) == EDT_USER_WORKTYPE) || (PD_MSG_FIELD_I(workType) == EDT_RT_WORKTYPE));
        u32 depc = PD_MSG_FIELD_IO(depc); // intentionally read before processing
        ocrFatGuid_t * depv = PD_MSG_FIELD_I(depv);
        ocrHint_t *hint = PD_MSG_FIELD_I(hint);
        u32 properties = PD_MSG_FIELD_I(properties) | GUID_PROP_TORECORD;
        u8 returnCode = createEdtHelper(
                self, &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(templateGuid),
                &(PD_MSG_FIELD_IO(paramc)), PD_MSG_FIELD_I(paramv), &(PD_MSG_FIELD_IO(depc)),
                properties, hint, outputEvent, (ocrTask_t*)(PD_MSG_FIELD_I(currentEdt).metaDataPtr),
                PD_MSG_FIELD_I(parentLatch), PD_MSG_FIELD_I(workType));

        if ((properties & EDT_PROP_RT_HINT_ALLOC) && (msg->srcLocation == self->myLocation)) {
            self->fcts.pdFree(self, hint);
        }
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            PD_MSG_FIELD_O(returnDetail) = returnCode;
        }
        ocrAssert((returnCode == 0) || (returnCode == OCR_EGUIDEXISTS));
#ifndef EDT_DEPV_DELAYED
        if ((depv != NULL)) {
            ocrAssert(returnCode == 0);
            ocrAssert(depc != EDT_PARAM_DEF);
            ocrGuid_t destination = PD_MSG_FIELD_IO(guid).guid;
            u32 i = 0;
            ocrTask_t * curEdt = NULL;
            getCurrentEnv(NULL, NULL, &curEdt, NULL);
            ocrFatGuid_t curEdtFatGuid = {.guid = curEdt ? curEdt->guid : NULL_GUID, .metaDataPtr = curEdt};
            while(i < depc) {
                if(!(ocrGuidIsUninitialized(depv[i].guid))) {
                    // We only add dependences that are not UNINITIALIZED_GUID
                    PD_MSG_STACK(msgAddDep);
                    getCurrentEnv(NULL, NULL, NULL, &msgAddDep);
                #undef PD_MSG
                #undef PD_TYPE
                    //NOTE: Could systematically call DEP_ADD but it's faster to disambiguate
                    //      NULL_GUID here instead of having DEP_ADD find out and do a satisfy.
#ifdef REG_ASYNC
                #define PD_MSG (&msgAddDep)
                #define PD_TYPE PD_MSG_DEP_ADD
                        msgAddDep.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
                        PD_MSG_FIELD_I(source.guid) = depv[i].guid;
                        PD_MSG_FIELD_I(source.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(dest.guid) = destination;
                        PD_MSG_FIELD_I(dest.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(slot) = i;
                        PD_MSG_FIELD_IO(properties) = DB_DEFAULT_MODE;
                        PD_MSG_FIELD_I(currentEdt) = curEdtFatGuid;
                #undef PD_MSG
                #undef PD_TYPE
#else
                    if(!(ocrGuidIsNull(depv[i].guid))) {
                #define PD_MSG (&msgAddDep)
                #define PD_TYPE PD_MSG_DEP_ADD
                        msgAddDep.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
                        PD_MSG_FIELD_I(source.guid) = depv[i].guid;
                        PD_MSG_FIELD_I(source.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(dest.guid) = destination;
                        PD_MSG_FIELD_I(dest.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(slot) = i;
                        PD_MSG_FIELD_IO(properties) = DB_DEFAULT_MODE;
                        PD_MSG_FIELD_I(currentEdt) = curEdtFatGuid;
                #undef PD_MSG
                #undef PD_TYPE
                    } else {
                      //Handle 'NULL_GUID' case here to avoid overhead of
                      //going through dep_add and end-up doing the same thing.
                #define PD_MSG (&msgAddDep)
                #define PD_TYPE PD_MSG_DEP_SATISFY
                        msgAddDep.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
                        PD_MSG_FIELD_I(satisfierGuid.guid) = curEdtFatGuid.guid;
                        PD_MSG_FIELD_I(guid.guid) = destination;
                        PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(payload.guid) = NULL_GUID;
                        PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(slot) = i;
#ifdef REG_ASYNC_SGL
                        PD_MSG_FIELD_I(mode) = DB_DEFAULT_MODE;
#endif
                        PD_MSG_FIELD_I(properties) = 0;
                        PD_MSG_FIELD_I(currentEdt) = curEdtFatGuid;
                #undef PD_MSG
                #undef PD_TYPE
                    }
#endif /*!EDT_DEPV_DELAYED*/
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                    u8 toReturn __attribute__((unused)) = self->fcts.processMessage(self, &msgAddDep, true);
                    ocrAssert(!toReturn);
                }
                ++i;
            }
        }
#endif
        // For asynchronous edt creation
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            msg->type &= ~PD_MSG_REQUEST;
            msg->type |= PD_MSG_RESPONSE;
        }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_WORK_EXECUTE: {
        ocrAssert(0); // Not used for this PD
        break;
    }

    case PD_MSG_WORK_DESTROY: {
        START_PROFILE(pd_hc_WorkDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrTask_t *task = (ocrTask_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        if(task == NULL)
            DPRINTF(DEBUG_LVL_WARN, "Invalid task, guid "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(guid.guid)));
        ocrAssert(task);
        ocrAssert(task->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.destruct(task);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EDTTEMP_CREATE: {
        START_PROFILE(pd_hc_EdtTempCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EDTTEMP_CREATE
#if defined(OCR_ENABLE_EDT_NAMING) || defined(OCR_TRACE_BINARY)
            const char* edtName = PD_MSG_FIELD_I(funcName);
#else
            const char* edtName = "";
#endif
        PD_MSG_FIELD_O(returnDetail) = createEdtTemplateHelper(
            self, &(PD_MSG_FIELD_IO(guid)),
            PD_MSG_FIELD_I(funcPtr), PD_MSG_FIELD_I(paramc),
            PD_MSG_FIELD_I(depc), edtName);

        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EDTTEMP_DESTROY: {
        START_PROFILE(pd_hc_EdtTempDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrTaskTemplate_t *tTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
        ocrAssert(tTemplate->fctId == ((ocrTaskTemplateFactory_t*)(self->factories[self->taskTemplateFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrTaskTemplateFactory_t*)(self->factories[self->taskTemplateFactoryIdx]))->fcts.destruct(tTemplate);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= (~PD_MSG_REQUEST);
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_CREATE: {
        START_PROFILE(pd_hc_EvtCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_CREATE
        ocrParamList_t * paramList = NULL;
#ifdef ENABLE_EXTENSION_PARAMS_EVT
        if (PD_MSG_FIELD_I(params) != NULL) {
            // Forcefully convert ocrEventParams_t to ocrParamList_t
            paramList = (ocrParamList_t *) PD_MSG_FIELD_I(params);
        }
#endif
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
        if (isBlocking == false) {
            u32 returnDetail = createEventHelper(
                self, &(PD_MSG_FIELD_IO(guid)),
                PD_MSG_FIELD_I(type), (PD_MSG_FIELD_I(properties) | GUID_PROP_TORECORD), paramList);
            if (returnDetail == OCR_EGUIDEXISTS) {
                RETURN_PROFILE(OCR_EPEND);
            } else {
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                msg->type &= ~PD_MSG_REQUEST;
                msg->type |= PD_MSG_RESPONSE;
            }
        } else {
#endif
        PD_MSG_FIELD_O(returnDetail) = createEventHelper(
            self, &(PD_MSG_FIELD_IO(guid)),
            PD_MSG_FIELD_I(type), (PD_MSG_FIELD_I(properties) | GUID_PROP_TORECORD), paramList);
            msg->type &= ~PD_MSG_REQUEST;
            msg->type |= PD_MSG_RESPONSE;
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
        }
#endif

#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_DESTROY: {
        START_PROFILE(pd_hc_EvtDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrEvent_t *evt = (ocrEvent_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        ocrAssert(evt->fctId == ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].destruct(evt);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= (~PD_MSG_REQUEST);
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
        // For mpilite long running EDTs to handle blocking destroy of labeled events
        if (msg->type & PD_MSG_REQ_RESPONSE)
            msg->type |= PD_MSG_RESPONSE;
#endif
        EXIT_PROFILE;

        break;
    }

    case PD_MSG_EVT_GET: {
        START_PROFILE(pd_hc_EvtGet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_GET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        if (PD_MSG_FIELD_I(guid.metaDataPtr) != NULL) {
            ocrEvent_t * evt = (ocrEvent_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
            ocrAssert(evt->fctId == ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->factoryId);
            PD_MSG_FIELD_O(data) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].get(evt);
            // There's no way to check if this call has been
            // successful without changing the 'get' signature
            PD_MSG_FIELD_O(returnDetail) = 0;
        } else {
            // Hack for BUG #865 This is for labeled GUIDs that are remote.
            // If they are not created yet, return ERROR_GUID.
            PD_MSG_FIELD_O(data.guid) = ERROR_GUID;
            PD_MSG_FIELD_O(data.metaDataPtr) = NULL;
            PD_MSG_FIELD_O(returnDetail) = 0;
        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_CREATE: {
        START_PROFILE(pd_hc_GuidCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_CREATE
        if(PD_MSG_FIELD_I(size) != 0) {
            // Here we need to create a metadata area as well
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.createGuid(
                self->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(targetLoc), PD_MSG_FIELD_I(properties));
            // This returnDetail is OCR_EGUIDEXISTS
        } else {
            // Here we just need to associate a GUID
            ocrGuid_t temp;
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getGuid(
                self->guidProviders[0], &temp, (u64)PD_MSG_FIELD_IO(guid.metaDataPtr),
                PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(targetLoc), GUID_PROP_TORECORD);
            PD_MSG_FIELD_IO(guid.guid) = temp;
        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_INFO: {
        START_PROFILE(pd_hc_GuidInfo);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_INFO
        localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
        if(PD_MSG_FIELD_I(properties) & KIND_GUIDPROP) {
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getKind(
                self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(kind)));
            if(PD_MSG_FIELD_O(returnDetail) == 0)
                PD_MSG_FIELD_O(returnDetail) = KIND_GUIDPROP
                    | WMETA_GUIDPROP | RMETA_GUIDPROP;
        } else if (PD_MSG_FIELD_I(properties) & LOCATION_GUIDPROP) {
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getLocation(
                self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(location)));
            if(PD_MSG_FIELD_O(returnDetail) == 0)
                PD_MSG_FIELD_O(returnDetail) = LOCATION_GUIDPROP
                    | WMETA_GUIDPROP | RMETA_GUIDPROP;
        } else {
            PD_MSG_FIELD_O(returnDetail) = WMETA_GUIDPROP | RMETA_GUIDPROP;
        }

#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_METADATA_CLONE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
        ocrFatGuid_t fatGuid = PD_MSG_FIELD_IO(guid);
        ocrGuidKind kind = OCR_GUID_NONE;
        guidKind(self, fatGuid, &kind);
        //TODO-MD-EAGER: Here we need to handle the use case that we want
        // to push a MD to a remote destination/clone/non-coherent
        // so we would call the factory clone with the right arguments and
        // it will trigger the push operation
        //TODO-MD-EAGER: Who's piloting the CLONE|NON_COHERENT ?
        // 1) We get a request to do it this way
        // 2) The MD figures it out. For instance given the operation that requests the clone,
        //    I should do coherent or non-coherent ? => Actually doesn't work because why would
        //    we call clone if there was no intent.
        if (HAS_MD_CLONE(PD_MSG_FIELD_I(type)) && HAS_MD_COHERENT(PD_MSG_FIELD_I(type))) {
            //TODO-MD These should go to their respective factories
            ocrAssert(msg->type & PD_MSG_REQ_RESPONSE);
            switch(kind) {
                case OCR_GUID_EDT_TEMPLATE:
                    localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
                    //These won't support flat serialization
#ifdef OCR_ENABLE_STATISTICS
                    ocrAssert(false && "no statistics support in distributed edt templates");
#endif
                    PD_MSG_FIELD_O(size) = sizeof(ocrTaskTemplateHc_t) + (sizeof(u64) * OCR_HINT_COUNT_EDT_HC);
                    PD_MSG_FIELD_O(returnDetail) = 0;
                    break;
                case OCR_GUID_AFFINITY:
                    localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
                    PD_MSG_FIELD_O(size) = sizeof(ocrAffinity_t);
                    PD_MSG_FIELD_O(returnDetail) = 0;
                    break;
#ifdef ENABLE_EXTENSION_LABELING
                case OCR_GUID_GUIDMAP:
                    localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
                    ocrGuidMap_t * map = (ocrGuidMap_t *) PD_MSG_FIELD_IO(guid.metaDataPtr);
                    PD_MSG_FIELD_O(size) =  ((sizeof(ocrGuidMap_t) + sizeof(s64) - 1) & ~(sizeof(s64)-1)) + map->numParams*sizeof(s64);
                    PD_MSG_FIELD_O(returnDetail) = 0;
                    break;
#endif
                default:
                    ocrAssert(false && "Unsupported GUID kind cloning");
                    PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            }
            msg->type &= ~PD_MSG_REQUEST;
            msg->type |= PD_MSG_RESPONSE;
        } else {
#ifdef OCR_ASSERT
            // Check for currently supported scenarios
            if (HAS_MD_MOVE(PD_MSG_FIELD_I(type))) {
                //TODO-MD The EDT is destroyed by the caller:
                // Should it be done here instead, as part of the move ?
                ocrAssert((kind == OCR_GUID_EDT) && "Only support metadata move of EDTs");
            } else {
                ocrAssert(HAS_MD_CLONE(PD_MSG_FIELD_I(type)) && HAS_MD_NON_COHERENT(PD_MSG_FIELD_I(type)));
                ocrAssert((kind == OCR_GUID_DB) && "Only support metadata push of DBs");
            }
            ocrAssert(!(msg->type & PD_MSG_REQ_RESPONSE));
#endif
            // In clone mode: I invoke the factory because I don't have any other handle to
            // this particular instance. It may be completely remote and unknown about.
            // In move, I have both the factory and an instance that I can call.
            // Move is about transferring the metadata from this PD to another PD.
            // It involves creating a LL MD message of a certain size, serialize the MD,
            // and send the message to the destination.
            ocrObjectFactory_t * factory = resolveObjectFactory(self, kind);
            ocrLocation_t dstLoc = PD_MSG_FIELD_I(dstLocation);
            ocrAssert(self->myLocation != dstLoc);
            // Trigger the movement, the 'type' field encodes the move semantic
            if (kind == OCR_GUID_DB) {
                char * readPtr = (char *) &PD_MSG_FIELD_I(addPayload);
                u32 waitersCount = ((u32*)readPtr)[0];
                ocrAssert(waitersCount == 1);
                readPtr+=sizeof(u32);
                void * waitersPtr = (void *) readPtr;
                RESULT_ASSERT(((ocrDataBlockFactory_t *)factory)->fcts.cloneAndSatisfy(factory, fatGuid.guid, NULL, dstLoc,
                                                                        PD_MSG_FIELD_I(type), waitersCount, waitersPtr), ==, OCR_EPEND);
            } else {
                RESULT_ASSERT(factory->clone(factory, fatGuid.guid, NULL, dstLoc, PD_MSG_FIELD_I(type)), ==, 0);

            }
        }
#undef PD_MSG
#undef PD_TYPE
        break;
    }
    case PD_MSG_GUID_RESERVE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_RESERVE
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.guidReserve(
            self->guidProviders[0], &(PD_MSG_FIELD_O(startGuid)), &(PD_MSG_FIELD_O(skipGuid)),
            PD_MSG_FIELD_I(numberGuids), PD_MSG_FIELD_I(guidKind), PD_MSG_FIELD_I(properties));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    case PD_MSG_GUID_UNRESERVE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_UNRESERVE
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.guidUnreserve(
            self->guidProviders[0], PD_MSG_FIELD_I(startGuid), PD_MSG_FIELD_I(skipGuid),
            PD_MSG_FIELD_I(numberGuids));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }
    case PD_MSG_GUID_DESTROY: {
        START_PROFILE(pd_hc_GuidDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.releaseGuid(
            self->guidProviders[0], PD_MSG_FIELD_I(guid), PD_MSG_FIELD_I(properties) & 1);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_GET_WORK: {
        START_PROFILE(pd_hc_Sched_Work);
        ocrFatGuid_t *fguid = NULL;
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_GET_WORK
        ocrSchedulerOpWorkArgs_t *taskArgs = &PD_MSG_FIELD_IO(schedArgs);
        taskArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_GET_WORK].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)taskArgs, NULL);

        if (taskArgs->kind == OCR_SCHED_WORK_EDT_USER) {
            PD_MSG_FIELD_O(factoryId) = 0; //taskHc_id;
            fguid = &(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
            localDeguidify(self, fguid);
        }
#undef PD_MSG
#ifdef ENABLE_EXTENSION_PERF
        ocrTask_t *task = (ocrTask_t *)(fguid)?fguid->metaDataPtr:NULL;
        if (task)
        {
            u32 k;
            ocrPerfCounters_t *taskCtrs = NULL;
            for (k = 0; k < queueGetSize(self->taskPerfs); k++)
                if(((ocrPerfCounters_t*)queueGet(self->taskPerfs, k))->edt == task->funcPtr) break;

            if(k<queueGetSize(self->taskPerfs)) {
                taskCtrs = (ocrPerfCounters_t *)queueGet(self->taskPerfs, k);

                // Update only if we have accumulated a sizable number of EDTs
                if (taskCtrs->count > COUNT_THRESHOLD) {
                    self->myNodeStats[NODE_PERF_ALLOC_PRESSURE] -= (taskCtrs->stats[PERF_DB_CREATES].average -
                                                                    taskCtrs->stats[PERF_DB_DESTROYS].average);
                    self->myNodeStats[NODE_PERF_PROGRESS] -= (taskCtrs->stats[PERF_EDT_CREATES].average +
                                                                    taskCtrs->stats[PERF_EVT_SATISFIES].average);
                    self->myNodeStats[NODE_PERF_LOAD] -= taskCtrs->stats[PERF_HW_CYCLES].average;
                    self->myNodeStats[NODE_PERF_FP_LOAD] -= taskCtrs->stats[PERF_FLOAT_OPS].average;
                    self->myNodeStats[NODE_PERF_CACHE] -= taskCtrs->stats[PERF_L1_HITS].average;
                    self->myNodeStats[NODE_PERF_MEM] -= taskCtrs->stats[PERF_L1_MISSES].average;

                    u32 j;
                    for(j = 0; j < NODE_PERF_MAX; j++) if(((s64)self->myNodeStats[j])<0) self->myNodeStats[j] = 0;
                }
            }
            self->myNodeStats[NODE_PERF_EDTS]--;
            //hal_xadd64(&self->myNodeStats[NODE_PERF_EDTS], -1); // Can be simple add w/ loss of accuracy
        }
#endif
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_NOTIFY: {
        START_PROFILE(pd_hc_Sched_Notify);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_NOTIFY
        ocrSchedulerOpNotifyArgs_t *notifyArgs = &PD_MSG_FIELD_IO(schedArgs);
#ifdef OCR_MONITOR_SCHEDULER
        if(PD_MSG_FIELD_IO(schedArgs).kind == OCR_SCHED_NOTIFY_EDT_READY){
            ocrGuid_t taskGuid = PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.guid;
            OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_SCHEDULER, OCR_ACTION_SCHED_MSG_RCV, taskGuid);
        }
#endif
#ifdef ENABLE_EXTENSION_PERF
        if(PD_MSG_FIELD_IO(schedArgs).kind == OCR_SCHED_NOTIFY_EDT_READY){
            u32 k;
            ocrPerfCounters_t *taskCtrs = NULL;
            ocrTask_t *task = PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_EDT_READY).guid.metaDataPtr;
            for (k = 0; k < queueGetSize(self->taskPerfs); k++)
                if(((ocrPerfCounters_t*)queueGet(self->taskPerfs, k))->edt == task->funcPtr) break;
            if(k<queueGetSize(self->taskPerfs)) {
                taskCtrs = (ocrPerfCounters_t *)queueGet(self->taskPerfs, k);

                if(taskCtrs->count > COUNT_THRESHOLD) {
                    self->myNodeStats[NODE_PERF_ALLOC_PRESSURE] += (taskCtrs->stats[PERF_DB_CREATES].average -
                                                                    taskCtrs->stats[PERF_DB_DESTROYS].average);
                    self->myNodeStats[NODE_PERF_PROGRESS] += (taskCtrs->stats[PERF_EDT_CREATES].average +
                                                                    taskCtrs->stats[PERF_EVT_SATISFIES].average);
                    self->myNodeStats[NODE_PERF_LOAD] += taskCtrs->stats[PERF_HW_CYCLES].average;
                    self->myNodeStats[NODE_PERF_FP_LOAD] += taskCtrs->stats[PERF_FLOAT_OPS].average;
                    self->myNodeStats[NODE_PERF_CACHE] += taskCtrs->stats[PERF_L1_HITS].average;
                    self->myNodeStats[NODE_PERF_MEM] += taskCtrs->stats[PERF_L1_MISSES].average;
                }
            }
            self->myNodeStats[NODE_PERF_EDTS]++;
            //hal_xadd64(&self->myNodeStats[NODE_PERF_EDTS], 1);
        }
#endif
        notifyArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)notifyArgs, NULL);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_TRANSACT: {
        START_PROFILE(pd_hc_Sched_Transact);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_TRANSACT
        ocrSchedulerOpTransactArgs_t *transactArgs = &PD_MSG_FIELD_IO(schedArgs);
        transactArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_TRANSACT].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)transactArgs, NULL);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        if (msg->type & PD_MSG_REQ_RESPONSE)
            msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_ANALYZE: {
        START_PROFILE(pd_hc_Sched_Analyze);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_ANALYZE
        ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = &PD_MSG_FIELD_IO(schedArgs);
        analyzeArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_ANALYZE].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)analyzeArgs, NULL);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        if (msg->type & PD_MSG_REQ_RESPONSE)
            msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_ADD: {
        START_PROFILE(pd_hc_AddDep);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_ADD
        // We first get information about the source and destination
        ocrGuidKind srcKind, dstKind;
        //NOTE: In distributed the metaDataPtr is set to NULL_GUID since
        //the guid provider doesn't fetch remote metaDataPtr yet. It's ok
        //(but fragile) because the HC event/task does not try to use it
        //Querying the kind through the PD's interface should be ok as it's
        //the problem of the guid provider to give this information
        u8 resolveCode = 1;
        if (ocrGuidIsNull(PD_MSG_FIELD_I(source.guid))) {
            srcKind = OCR_GUID_NONE;
        } else {
            if (ENABLE_EVENT_MDC) {
                RESULT_ASSERT(self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_I(source.guid), &srcKind), == , 0);
            }
            // Second, check if MDC is on
            if (MDC_SUPPORT_EVT(srcKind)) {
                DPRINTF(DBG_LVL_MDEVT, "event-md: PD_MSG_DEP_ADD resolving remote source "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(source.guid)));
                resolveCode = resolveRemoteMetaData(self, &PD_MSG_FIELD_I(source), NULL, true, true);
            }
            // When the GP doesn't support the cloning it falls back on regular GUID lookup.
            // So far this only applies to labelled GUIDs since we do not handle their metadata-cloning.
            if (resolveCode) {
                ocrAssert((resolveCode == 1) || (resolveCode == OCR_EPERM));
                //getVal - resolve
                self->guidProviders[0]->fcts.getVal(
                    self->guidProviders[0], PD_MSG_FIELD_I(source.guid),
                    (u64*)(&(PD_MSG_FIELD_I(source.metaDataPtr))), &srcKind, MD_LOCAL, NULL);
                DPRINTF(DBG_LVL_MDEVT, "event-md: PD_MSG_DEP_ADD local source "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(source.guid)));
            }
        }

        resolveCode = 1;
        if (ENABLE_EVENT_MDC) {
            RESULT_ASSERT(self->guidProviders[0]->fcts.getKind(self->guidProviders[0], PD_MSG_FIELD_I(dest.guid), &dstKind), == , 0);
        }
        if (MDC_SUPPORT_EVT(dstKind)) {
            DPRINTF(DBG_LVL_MDEVT, "event-md: PD_MSG_DEP_ADD resolving remote dest "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(dest.guid)));
            resolveCode = resolveRemoteMetaData(self, &PD_MSG_FIELD_I(dest), NULL, true, true);
        }
        if (resolveCode) {
            ocrAssert((resolveCode == 1) || (resolveCode == OCR_EPERM));
            //getVal - resolve
            self->guidProviders[0]->fcts.getVal(
                self->guidProviders[0], PD_MSG_FIELD_I(dest.guid),
                (u64*)(&(PD_MSG_FIELD_I(dest.metaDataPtr))), &dstKind, MD_LOCAL, NULL);
            DPRINTF(DBG_LVL_MDEVT, "event-md: PD_MSG_DEP_ADD local dest "GUIDF"\n", GUIDA(PD_MSG_FIELD_I(dest.guid)));
        }
        ocrFatGuid_t src = PD_MSG_FIELD_I(source);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        ocrDbAccessMode_t mode = (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK); //lower bits is the mode //BUG 550: not pretty
        u32 slot = PD_MSG_FIELD_I(slot);
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
        u32 sslot = PD_MSG_FIELD_I(sslot);
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
#if defined(XP_CHANNEL_EVT_NONFIFO) || defined(COMMWRK_PROCESS_SATISFY_CHANNEL_ONLY)
        bool sync = false;
#else
        // Channel needs to be synchronous to ensure ordering of multiple satisfy issued in a row
        bool sync = (srcKind == OCR_GUID_EVENT_CHANNEL);
#endif
#else
        bool sync = false;
#endif
        if (srcKind == OCR_GUID_NONE) {
            //NOTE: Handle 'NULL_GUID' case here to be safe although
            //we've already caught it in ocrAddDependence for performance
            // This is equivalent to an immediate satisfy
#ifdef REG_ASYNC_SGL
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot, mode, sync);
#else
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot, sync);
#endif
#ifdef REG_ASYNC // In addition need to do the signaler registration to get the mode
            if (dstKind == OCR_GUID_EDT) {
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGSIGNALER
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST;
                // Registers sourceGuid (signaler) onto destGuid
                PD_MSG_FIELD_I(signaler) = src;
                PD_MSG_FIELD_I(dest) = dest;
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
                PD_MSG_FIELD_I(sslot) = sslot;
#endif
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(mode) = DB_MODE_NULL;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            }
#endif
        } else if (srcKind == OCR_GUID_DB) {
            if (dstKind & OCR_GUID_EVENT) {
#ifdef REG_ASYNC_SGL
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot, mode, sync);
#else
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot, sync);
#endif
            } else {
                // NOTE: We could use convertDepAddToSatisfy since adding a DB dependence
                // is equivalent to satisfy. However, we want to go through the register
                // function to make sure the access mode is recorded.
                if(dstKind != OCR_GUID_EDT)
                    DPRINTF(DEBUG_LVL_WARN, "Attempting to add a DB dependence to dest of kind %"PRIx32" "
                                            "that's neither EDT nor Event\n", dstKind);
                ocrAssert(dstKind == OCR_GUID_EDT);
#ifdef REG_ASYNC_SGL
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot, mode, sync);
#else
#ifdef REG_ASYNC
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot, sync);
#endif
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGSIGNALER
#ifdef REG_ASYNC
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST;
#else
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
#endif
                // Registers sourceGuid (signaler) onto destGuid
                PD_MSG_FIELD_I(signaler) = src;
                PD_MSG_FIELD_I(dest) = dest;
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
                PD_MSG_FIELD_I(sslot) = sslot;
#endif
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(mode) = mode;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            #undef PD_MSG
            #undef PD_TYPE
#endif
            }
        } else {
            if(!(srcKind & OCR_GUID_EVENT)) {
                DPRINTF(DEBUG_LVL_WARN, "Attempting to add a dependence with a GUID of type 0x%"PRIx32", "
                                        "expected Event\n", srcKind);
            }
            ocrAssert(srcKind & OCR_GUID_EVENT);
            // We are handling the following dependences (event, event|edt) and do
            // things differently depending on the type of events:
            // (non-persistent event, edt)  => REG_SIGNALER (event on edt), then REG_WAITER (edt on event)
            // (persistent event, edt)      => REG_SIGNALER (event on edt, edt does late registration)
            // ((any) event, (any) event)   => REG_WAITER
            //
            // Are we revealing too much of the underlying implementation here ?
            //
            // We omit counted events here since it won't be destroyed until the addDependence happens.
#ifdef XP_CHANNEL_EVT_NONFIFO
            bool srcIsNonPersistent = ((srcKind == OCR_GUID_EVENT_ONCE) ||
                                        (srcKind == OCR_GUID_EVENT_LATCH));
#else
            bool srcIsNonPersistent = ((srcKind == OCR_GUID_EVENT_ONCE) ||
                                        (srcKind == OCR_GUID_EVENT_LATCH) ||
                                        (srcKind == OCR_GUID_EVENT_CHANNEL));
#endif

#ifdef REG_ASYNC_SGL
            bool needPullMode = false;
#else
            // The registration is always necessary when the destination is an EDT.
            // It allows to record the mode of the dependence as well as the type of
            // event the EDT should be expecting
            bool needPullMode = (dstKind == OCR_GUID_EDT);
#endif

            // NOTE: Important to do the signaler registration before the waiter one
            // when the dependence is of the form (non-persistent event, edt)
            // Otherwise there's a race between the once event being destroyed and
            // the edt processing the registerSignaler call (which may read into the
            // destroyed event metadata).
            if(needPullMode) {
                ASSERT_BLOCK_BEGIN(dstKind == OCR_GUID_EDT);
                DPRINTF(DEBUG_LVL_WARN, "Runtime error expect REGSIGNALER dest to be an EDT GUID\n");
                ASSERT_BLOCK_END
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
                // 'Pull' registration left with persistent event as source and EDT as destination
#ifdef REG_ASYNC_SGL
            #undef PD_MSG
            #undef PD_TYPE
#endif
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGSIGNALER
#ifdef REG_ASYNC
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST;
#else
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
#endif
                // Registers sourceGuid (signaler) onto destGuid
                PD_MSG_FIELD_I(signaler) = src;
                PD_MSG_FIELD_I(dest) = dest;
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
                PD_MSG_FIELD_I(sslot) = sslot;
#endif
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(mode) = mode;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;
                DPRINTF(DEBUG_LVL_INFO,
                        "Dependence added (src: "GUIDF", dest: "GUIDF") -> %"PRIu32"\n", GUIDA(src.guid), GUIDA(dest.guid), returnCode);
#if !defined(ENABLE_EXTENSION_MULTI_OUTPUT_SLOT) && defined(OCR_TRACE_BINARY)
                u32 sslot = 0;
#endif
                OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_ADD_DEP, traceTaskAddDependence, src.guid, dest.guid, sslot, slot, mode);
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            #undef PD_MSG
            #undef PD_TYPE
            }
#if defined (REG_ASYNC) || (REG_ASYNC_SGL) // New registration always does register the edt on the event here (eager instead of lazy through signaler)
            bool needPushMode = true;
#else
            // 'Push' registration when source is non-persistent and/or destination is another event.
            bool needPushMode = (srcIsNonPersistent || (dstKind & OCR_GUID_EVENT));
#endif
            if (needPushMode) {
#ifdef REG_ASYNC_SGL_DEBUG
                DPRINTF(DEBUG_LVL_WARN, "taskGuid="GUIDF" PD_MSG_DEP_REGWAITER needPushMode\n", GUIDA(dest.guid));
#endif
                //OK if srcKind is at current location
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGWAITER
                // Registration with non-persistent events is two-way
                // to enforce message ordering constraints.
#if defined (REG_ASYNC) || (REG_ASYNC_SGL)
                registerMsg.type = PD_MSG_DEP_REGWAITER | PD_MSG_REQUEST;
                // Want the message to be blocking if src is non persistent or it is a channel
                // event and we want to ensure ordering is repected
                if (srcIsNonPersistent) {
                     registerMsg.type |= PD_MSG_REQ_RESPONSE;
                }
#else
                registerMsg.type = PD_MSG_DEP_REGWAITER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
#endif
                // Registers destGuid (waiter) onto sourceGuid
                PD_MSG_FIELD_I(waiter) = dest;
                PD_MSG_FIELD_I(dest) = src;
#ifdef REG_ASYNC_SGL
                PD_MSG_FIELD_I(mode) = mode;
#endif
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
                PD_MSG_FIELD_I(sslot) = sslot;
#endif
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
#if defined (REG_ASYNC) || (REG_ASYNC_SGL)
                u8 returnDetail = ((srcIsNonPersistent) && (returnCode == 0)) ? PD_MSG_FIELD_O(returnDetail) : returnCode;
#else
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;
#endif
                DPRINTF(DEBUG_LVL_INFO,
                        "Dependence added (src: "GUIDF", dest: "GUIDF") -> %"PRIu32"\n", GUIDA(src.guid),
                        GUIDA(dest.guid), returnCode);
#if !defined(ENABLE_EXTENSION_MULTI_OUTPUT_SLOT) && defined(OCR_TRACE_BINARY)
                u32 sslot = 0;
#endif
                OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EVENT, OCR_ACTION_ADD_DEP, traceEventAddDependence, src.guid, dest.guid, sslot, slot, mode);
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            #undef PD_MSG
            #undef PD_TYPE
            }
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_REGSIGNALER: {
        START_PROFILE(pd_hc_RegSignaler);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_REGSIGNALER
        // We first get information about the signaler and destination
        ocrGuidKind signalerKind, dstKind;
        //NOTE: In distributed the metaDataPtr is set to NULL_GUID since
        //the guid provider doesn't fetch remote metaDataPtr yet. It's ok
        //(but fragile) because the HC event/task does not try to use it
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(signaler.guid),
            (u64*)(&(PD_MSG_FIELD_I(signaler.metaDataPtr))), &signalerKind, MD_LOCAL, NULL);
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(dest.guid),
            (u64*)(&(PD_MSG_FIELD_I(dest.metaDataPtr))), &dstKind, MD_LOCAL, NULL);

        ocrFatGuid_t signaler = PD_MSG_FIELD_I(signaler);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        bool isAddDep = PD_MSG_FIELD_I(properties);
        u32 slot = PD_MSG_FIELD_I(slot);
        ocrDbAccessMode_t mode = PD_MSG_FIELD_I(mode);
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
        u32 sslot = PD_MSG_FIELD_I(sslot);
#else
#ifdef OCR_TRACE_BINARY
        u32 sslot = 0;
#endif
#endif
        if (dstKind & OCR_GUID_EVENT) {
            ocrAssert(false && "We never register signaler on an event");
            ocrEvent_t *evt = (ocrEvent_t*)(dest.metaDataPtr);
            ocrAssert(evt->fctId == ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->factoryId);
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
            PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].registerSignaler(
                evt, sslot, signaler, slot, mode, isAddDep);
#else
            PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].registerSignaler(
                evt, signaler, slot, mode, isAddDep);
#endif
        } else if (dstKind == OCR_GUID_EDT) {
            DPRINTF(DEBUG_LVL_VERB, "Add Dep to EDT (GUID: "GUIDF")\n", GUIDA(dest.guid));
            ocrTask_t *edt = (ocrTask_t*)(dest.metaDataPtr);
            ocrAssert(edt->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
            PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.registerSignaler(
                edt, signaler, slot, mode, isAddDep);
        } else {
            DPRINTF(DEBUG_LVL_WARN, "Attempt to register signaler on %"PRIx32" which is not of type EDT or Event\n", dstKind);
            ocrAssert(0); // No other things we can register signalers on
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
        DPRINTF(DEBUG_LVL_INFO,
                "Dependence added (src: "GUIDF", dest: "GUIDF") -> %"PRIu32"\n", GUIDA(signaler.guid),
                GUIDA(dest.guid), returnCode);
        OCR_TOOL_TRACE(false, OCR_TRACE_TYPE_EDT, OCR_ACTION_ADD_DEP, traceTaskAddDependence, signaler.guid, dest.guid, sslot, slot, mode);

#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            msg->type |= PD_MSG_RESPONSE;
        }
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_REGWAITER: {
        START_PROFILE(pd_hc_RegWaiter);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_REGWAITER
        // We first get information about the signaler and destination
        ocrGuidKind dstKind;
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(dest.guid),
            (u64*)(&(PD_MSG_FIELD_I(dest.metaDataPtr))), &dstKind, MD_LOCAL, NULL);
        ocrFatGuid_t waiter = PD_MSG_FIELD_I(waiter);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        ocrAssert(PD_MSG_FIELD_I(dest.metaDataPtr) != NULL);
        bool isAddDep = PD_MSG_FIELD_I(properties);
        if (dstKind & OCR_GUID_EVENT) {
            ocrEvent_t *evt = (ocrEvent_t*)(dest.metaDataPtr);
#ifdef OCR_ASSERT
#ifdef ENABLE_EXTENSION_COLLECTIVE_EVT
            if ((dstKind != OCR_GUID_EVENT_COLLECTIVE) && (!MDC_SUPPORT_EVT(dstKind))) {
#endif
            ocrLocation_t tmpL;
            self->guidProviders[0]->fcts.getLocation(
                self->guidProviders[0], dest.guid, &tmpL);
            ocrAssert(tmpL == self->myLocation);
#ifdef ENABLE_EXTENSION_COLLECTIVE_EVT
            } else {
                ocrAssert(PD_MSG_FIELD_I(dest.metaDataPtr) != NULL);
            }
#endif
#endif
            ocrAssert(evt->fctId == ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->factoryId);
            // Warning: A counted-event can be destroyed by this call
#ifdef REG_ASYNC_SGL
#ifdef ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
            PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].registerWaiter(
                evt, PD_MSG_FIELD_I(sslot), waiter, PD_MSG_FIELD_I(slot), isAddDep, PD_MSG_FIELD_I(mode));
#else
            PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].registerWaiter(
                evt, waiter, PD_MSG_FIELD_I(slot), isAddDep, PD_MSG_FIELD_I(mode));
#endif
#else
            PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].registerWaiter(
                evt, waiter, PD_MSG_FIELD_I(slot), isAddDep);
#endif
        } else {
            if (dstKind != OCR_GUID_DB)
                DPRINTF(DEBUG_LVL_WARN, "Attempting to add a dependence to a GUID of type %"PRIx32", expected DB\n", dstKind);
            ocrAssert(dstKind == OCR_GUID_DB);
            // When an EDT want to register to a DB, for instance to get EW access.
            ocrDataBlock_t *db = (ocrDataBlock_t*)(dest.metaDataPtr);
            ocrAssert(db->fctId == ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->factoryId);
            // Warning: A counted-event can be destroyed by this call
            PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.registerWaiter(
                db, waiter, PD_MSG_FIELD_I(slot), isAddDep);
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            msg->type |= PD_MSG_RESPONSE;
        }
        EXIT_PROFILE;
        break;
    }


    case PD_MSG_DEP_SATISFY: {
        START_PROFILE(pd_hc_Satisfy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_SATISFY
        ocrGuidKind dstKind;
#ifdef ENABLE_EXTENSION_PAUSE
        ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
#endif
#ifdef ENABLE_EXTENSION_PERF
        if(!ocrGuidIsNull(PD_MSG_FIELD_I(satisfierGuid.guid))) {
            self->guidProviders[0]->fcts.getVal(
                self->guidProviders[0], PD_MSG_FIELD_I(satisfierGuid.guid),
                (u64*)(&(PD_MSG_FIELD_I(satisfierGuid.metaDataPtr))), &dstKind, MD_LOCAL, NULL);
            ocrTask_t * curEdt = PD_MSG_FIELD_I(satisfierGuid).metaDataPtr;
            if(curEdt) curEdt->swPerfCtrs[PERF_EVT_SATISFIES - PERF_HW_MAX]++;
         }
#endif
        PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(guid.guid),
            (u64*)(&(PD_MSG_FIELD_I(guid.metaDataPtr))), &dstKind, MD_LOCAL, NULL);
        //TODO I think there's a MD cloning issue here. The dstKind can be remote. See test edtReturnEvent.c
        ocrAssert(PD_MSG_FIELD_I(guid.metaDataPtr) != NULL);
        ocrFatGuid_t dst = PD_MSG_FIELD_I(guid);
        if(dstKind & OCR_GUID_EVENT) {
            ocrEvent_t *evt = (ocrEvent_t*)(dst.metaDataPtr);
            ocrAssert(evt->fctId == ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->factoryId);
            PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->fcts[evt->kind].satisfy(
                evt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot));
#ifdef ENABLE_EXTENSION_PAUSE
            rself->pqrFlags.prevDb = PD_MSG_FIELD_I(payload).guid;
#endif
        } else {
            if(dstKind == OCR_GUID_EDT) {
                ocrTask_t *edt = (ocrTask_t*)(dst.metaDataPtr);
                ocrAssert(edt->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
#ifdef REG_ASYNC_SGL
                PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.satisfyWithMode(
                    edt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot), PD_MSG_FIELD_I(mode));
#else
                PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.satisfy(
                    edt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot));
#endif
#ifdef ENABLE_EXTENSION_PAUSE
                rself->pqrFlags.prevDb = PD_MSG_FIELD_I(payload).guid;
#endif
            } else {
                DPRINTF(DEBUG_LVL_WARN, "Attempting to satisfy a GUID of type %"PRIx32", expected EDT\n", dstKind);
                PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
                ocrAssert(0); // We can't satisfy anything else
            }
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
#undef PD_MSG
#undef PD_TYPE
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            msg->type |= PD_MSG_RESPONSE;
        }
#endif
        msg->type &= ~PD_MSG_REQUEST;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_UNREGSIGNALER: {
        //Not implemented: see #521, #522
        ocrAssert(0);
        break;
    }

    case PD_MSG_DEP_UNREGWAITER: {
        //Not implemented: see #521, #522
        ocrAssert(0);
        break;
    }

    case PD_MSG_DEP_DYNADD: {
        START_PROFILE(pd_hc_DynAdd);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNADD
        // Check to make sure that the EDT is only doing this to
        // itself
        // Also, this should only happen when there is an actual EDT
        if((curTask==NULL) || (!(ocrGuidIsEq(curTask->guid, PD_MSG_FIELD_I(edt.guid)))))
            DPRINTF(DEBUG_LVL_WARN, "Attempting to notify a missing/different EDT, GUID="GUIDF"\n", GUIDA(PD_MSG_FIELD_I(edt.guid)));
        ocrAssert(curTask && (ocrGuidIsEq(curTask->guid, PD_MSG_FIELD_I(edt.guid))));
        ocrAssert(curTask->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.notifyDbAcquire(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        // msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_DYNREMOVE: {
        START_PROFILE(pd_hc_DynRemove);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        // Check to make sure that the EDT is only doing this to itself
        // Also, this should only happen when there is an actual EDT
        if ((curTask==NULL) || (!(ocrGuidIsEq(curTask->guid, PD_MSG_FIELD_I(edt.guid)))))
            DPRINTF(DEBUG_LVL_WARN, "Attempting to notify a missing/different EDT, GUID="GUIDF"\n", GUIDA(PD_MSG_FIELD_I(edt.guid)));
        ocrAssert(curTask && (ocrGuidIsEq(curTask->guid, PD_MSG_FIELD_I(edt.guid))));
        ocrAssert(curTask->fctId == ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->factoryId);
        PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.notifyDbRelease(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SAL_PRINT: {
        ocrAssert(0);
        break;
    }

    case PD_MSG_SAL_READ: {
        ocrAssert(0);
        break;
    }

    case PD_MSG_SAL_WRITE: {
        ocrAssert(0);
        break;
    }

    case PD_MSG_SAL_TERMINATE: {
        ocrAssert(0);
        break;
    }

    case PD_MSG_MGT_REGISTER: {
        ocrAssert(0);
        break;
    }

    case PD_MSG_MGT_UNREGISTER: {
        // Only one PD at this time
        ocrAssert(0);
        break;
    }

    case PD_MSG_MGT_RL_NOTIFY: {
#define PD_MSG msg
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
        if(PD_MSG_FIELD_I(properties) & RL_FROM_MSG) {
            // This should not happen here as we only have one PD
            ocrAssert(0);
        } else {
            // This is from user code so it should be a request to shutdown
            ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)self;
            // Set up the switching for the next phase
            ocrAssert(rself->rlSwitch.runlevel == RL_USER_OK && rself->rlSwitch.nextPhase == RL_GET_PHASE_COUNT_UP(self, RL_USER_OK));
            // We want to transition down now
            rself->rlSwitch.nextPhase = RL_GET_PHASE_COUNT_DOWN(self, RL_USER_OK) - 1;
            ocrAssert(PD_MSG_FIELD_I(properties) & RL_TEAR_DOWN);
            ocrAssert(PD_MSG_FIELD_I(runlevel) & RL_COMPUTE_OK);
            self->shutdownCode = PD_MSG_FIELD_I(errorCode);
#ifdef ENABLE_RESILIENCY
            rself->shutdownInProgress = 1;
#endif
            u8 returnCode __attribute__((unused)) = self->fcts.switchRunlevel(
                              self, RL_USER_OK, RL_TEAR_DOWN | RL_ASYNC | RL_REQUEST | RL_FROM_MSG);
            ocrAssert(returnCode == 0);
        }
        msg->type &= ~PD_MSG_REQUEST;
        break;
#undef PD_MSG
#undef PD_TYPE
    }

    case PD_MSG_MGT_MONITOR_PROGRESS: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
        // Delegate to scheduler
        PD_MSG_FIELD_IO(properties) = self->schedulers[0]->fcts.monitorProgress(self->schedulers[0],
                                                                                (ocrMonitorProgress_t) PD_MSG_FIELD_IO(properties) & 0xFF, PD_MSG_FIELD_I(monitoree));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    case PD_MSG_HINT_SET: {
        START_PROFILE(pd_hc_HintSet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_HINT_SET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrFatGuid_t fatGuid = PD_MSG_FIELD_I(guid);
        ocrGuidKind kind = OCR_GUID_NONE;
        guidKind(self, fatGuid, &kind);
        switch(PD_MSG_FIELD_I(hint->type)) {
        case OCR_HINT_EDT_T:
            {
                if (kind == OCR_GUID_EDT_TEMPLATE) {
                    ocrTaskTemplate_t* taskTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = ((ocrTaskTemplateFactory_t*)(self->factories[self->taskTemplateFactoryIdx]))->fcts.setHint(taskTemplate, PD_MSG_FIELD_I(hint));
                } else {
                    ocrAssert(kind == OCR_GUID_EDT);
                    ocrTask_t *task = (ocrTask_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.setHint(task, PD_MSG_FIELD_I(hint));
                }
            }
            break;
        case OCR_HINT_DB_T:
            {
                ocrAssert(kind == OCR_GUID_DB);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.setHint(db, PD_MSG_FIELD_I(hint));
            }
            break;
        case OCR_HINT_EVT_T:
            {
                ocrAssert(kind & OCR_GUID_EVENT);
                ocrEvent_t *evt = (ocrEvent_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->commonFcts.setHint(evt, PD_MSG_FIELD_I(hint));
            }
            break;
        case OCR_HINT_GROUP_T:
        default:
            ocrAssert(0);
            PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            break;
        }
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_HINT_GET: {
        START_PROFILE(pd_hc_HintGet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_HINT_GET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrFatGuid_t fatGuid = PD_MSG_FIELD_I(guid);
        ocrGuidKind kind = OCR_GUID_NONE;
        guidKind(self, fatGuid, &kind);
        switch(PD_MSG_FIELD_IO(hint->type)) {
        case OCR_HINT_EDT_T:
            {
                if (kind == OCR_GUID_EDT_TEMPLATE) {
                    ocrTaskTemplate_t* taskTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = ((ocrTaskTemplateFactory_t*)(self->factories[self->taskTemplateFactoryIdx]))->fcts.getHint(taskTemplate, PD_MSG_FIELD_IO(hint));
                } else {
                    ocrAssert(kind == OCR_GUID_EDT);
                    ocrTask_t *task = (ocrTask_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = ((ocrTaskFactory_t*)(self->factories[self->taskFactoryIdx]))->fcts.getHint(task, PD_MSG_FIELD_IO(hint));
                }
            }
            break;
        case OCR_HINT_DB_T:
            {
                ocrAssert(kind == OCR_GUID_DB);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = ((ocrDataBlockFactory_t*)(self->factories[self->datablockFactoryIdx]))->fcts.getHint(db, PD_MSG_FIELD_IO(hint));
            }
            break;
        case OCR_HINT_EVT_T:
            {
                ocrAssert(kind & OCR_GUID_EVENT);
                ocrEvent_t *evt = (ocrEvent_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = ((ocrEventFactory_t*)(self->factories[self->eventFactoryIdx]))->commonFcts.getHint(evt, PD_MSG_FIELD_IO(hint));
            }
            break;
        case OCR_HINT_GROUP_T:
        default:
            ocrAssert(0);
            PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            break;
        }
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_RESILIENCY_NOTIFY: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_RESILIENCY_NOTIFY
#ifdef ENABLE_RESILIENCY
        ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
        ocrAssert(rself->fault == 0);
        rself->faultArgs = PD_MSG_FIELD_I(faultArgs);
        hal_fence();
        bool faultInjected = false;
        switch(rself->faultArgs.kind) {
        case OCR_FAULT_DATABLOCK_CORRUPTION:
            {
                ocrFatGuid_t *dbGuid = &(rself->faultArgs.OCR_FAULT_ARG_FIELD(OCR_FAULT_DATABLOCK_CORRUPTION).db);
                ocrAssert(!ocrGuidIsNull(dbGuid->guid) && dbGuid->metaDataPtr == NULL);
                localDeguidify(self, dbGuid);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(dbGuid->metaDataPtr);
                if((db->flags & DB_PROP_SINGLE_ASSIGNMENT) != 0)
                    faultInjected = true;
            }
            break;
        default:
            // Not handled
            ocrAssert(0);
            return OCR_EFAULT;
        }
        if (faultInjected) {
            rself->fault = 1;
            PD_MSG_FIELD_O(returnDetail) = 0;
        } else {
            PD_MSG_FIELD_O(returnDetail) = 1;
        }
#else
        PD_MSG_FIELD_O(returnDetail) = 0;
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    case PD_MSG_RESILIENCY_MONITOR: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_RESILIENCY_MONITOR
#ifdef ENABLE_RESILIENCY
        ocrWorker_t * worker;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
        if (rself->shutdownInProgress == 0) {
            bool doMonitorFault = false;
            bool doMonitorCheckpoint = false;
            ocrResiliencyMonitorProp prop = PD_MSG_FIELD_I(properties);
            PD_MSG_FIELD_O(returnDetail) = 0;
            switch(prop) {
            case OCR_RESILIENCY_MONITOR_FAULT:
                {
                    doMonitorFault = true;
                }
                break;
            case OCR_RESILIENCY_MONITOR_CHECKPOINT:
                {
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
                    doMonitorCheckpoint = true;
#endif
                }
                break;
            case OCR_RESILIENCY_MONITOR_DEFAULT:
                {
                    doMonitorFault = true;
#ifdef ENABLE_RESILIENCY_CHECKPOINT_RESTART
                    doMonitorCheckpoint = true;
#endif
                }
                break;
            default:
                // Not handled
                ocrAssert(0);
                break;
            }

            if (doMonitorFault && rself->fault != 0) { //Fault detected

                // Read fault data:
                PD_MSG_FIELD_O(faultArgs) = rself->faultArgs;

                // Checkin:
                u32 oldVal = hal_xadd32(&rself->faultMonitorCounter, 1);

                if (oldVal == 0) {
                    DPRINTF(DEBUG_LVL_WARN, "Fault detected! Waiting for recovery...\n");
                }

                DPRINTF(DEBUG_LVL_INFO, "Worker %"PRIu64" checked in...\n", worker->id);

                // Recovery:
                if(oldVal == (rself->computeWorkerCount - 1)) {
                    // The last worker to arrive executes the recovery code
                    // This ensures shutdown does not start during recovery
                    ocrAssert(rself->faultMonitorCounter == rself->computeWorkerCount);
                    RESULT_ASSERT((hal_cmpswap32(&rself->recover, 0, 1)), ==, 0);
                    //Now recover from fault
                    switch(rself->faultArgs.kind) {
                    case OCR_FAULT_DATABLOCK_CORRUPTION:
                        {
                            ocrFatGuid_t dbGuid = rself->faultArgs.OCR_FAULT_ARG_FIELD(OCR_FAULT_DATABLOCK_CORRUPTION).db;
                            ocrAssert(!ocrGuidIsNull(dbGuid.guid) && dbGuid.metaDataPtr != NULL);
                            DPRINTF(DEBUG_LVL_WARN, "Fault kind: OCR_FAULT_DATABLOCK_CORRUPTION (db="GUIDF")\n", GUIDA(dbGuid.guid));
                            ocrDataBlock_t *db = (ocrDataBlock_t*)(dbGuid.metaDataPtr);
                            hal_memCopy(db->ptr, db->bkPtr, db->size, 0);
                        }
                        break;
                    default:
                        // Not handled
                        ocrAssert(0);
                        return OCR_EFAULT;
                    }
                    rself->faultArgs.kind = OCR_FAULT_NONE;
                    RESULT_ASSERT((hal_cmpswap32(&rself->fault, 1, 0)), ==, 1);
                } else {
                    // Others wait for recovery
                    while(rself->fault != 0 && rself->shutdownInProgress == 0)
                        ;
                }

                // Checkout:
                oldVal = hal_xadd32(&rself->faultMonitorCounter, -1);
                DPRINTF(DEBUG_LVL_INFO, "Worker %"PRIu64" checked out...\n", worker->id);

                if (oldVal == 1) {
                    ocrAssert(rself->faultMonitorCounter == 0);
                    RESULT_ASSERT((hal_cmpswap32(&rself->recover, 1, 0)), ==, 1);
                    DPRINTF(DEBUG_LVL_WARN, "Fault recovery completed\n");
                } else {
                    while(rself->recover != 0)
                        ;
                }

                // Resume:
                PD_MSG_FIELD_O(returnDetail) = OCR_EFAULT;
            }

            if (doMonitorCheckpoint) {
                if (worker->id == 1 &&
                    rself->stateOfCheckpoint == 0 &&
                    rself->stateOfRestart == 0 &&
                    rself->initialCheckForRestart != 0)
                {
                    u64 curTime = salGetTime();
                    u64 prevTime = rself->timestamp;
                    if ((curTime - prevTime) > rself->checkpointInterval) {
                        if (prevTime > 0) {
                            rself->timestamp = curTime;
                            DPRINTF(DEBUG_LVL_VERB, "Ready to checkpoint...\n");
                            rself->stateOfCheckpoint = 1;
                        }
                    }
                } else if (rself->stateOfCheckpoint != 0 &&
                           worker->stateOfCheckpoint == 0 &&
                           worker->isIdle)
                {
                    checkinWorkerForCheckpoint(self, worker);
                } else if (rself->stateOfRestart != 0 &&
                           worker->stateOfRestart == 0 &&
                           worker->isIdle)
                {
                    checkinWorkerForRestart(self, worker);
                } else if (rself->quiesceComps != 0 && worker->isIdle) {
                    hal_xadd32(&rself->quiesceComps, 1);
                    DPRINTF(DEBUG_LVL_VERB, "Waiting at quiesceComps...\n");
                    while (rself->quiesceComps != 0)
                        ;
                    DPRINTF(DEBUG_LVL_VERB, "Checking out of quiesceComps...\n");
                } else if (rself->resumeAfterCheckpoint != 0) {
                    ocrAssert(worker->stateOfCheckpoint != 0);
                    worker->stateOfCheckpoint = 0;
                    u32 oldVal = hal_xadd32(&rself->checkpointWorkerCounter, -1);
                    ocrAssert(oldVal > 0 && oldVal <= rself->computeWorkerCount);
                    if (oldVal == 1) {
                        DPRINTF(DEBUG_LVL_VERB, "Resuming after checkpoint...\n");
                        rself->stateOfCheckpoint = 0;
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Waiting to resume after checkpoint...\n");
                        while (rself->stateOfCheckpoint != 0)
                            ;
                    }
                    if (hal_cmpswap32(&rself->resumeAfterCheckpoint, 1, 0) == 1) {
                        ocrAssert(rself->checkpointWorkerCounter == 0);
                    }
                    ocrAssert(rself->resumeAfterCheckpoint == 0);
                } else if (rself->resumeAfterRestart != 0) {
                    ocrAssert(worker->stateOfRestart != 0);
                    worker->stateOfRestart = 0;
                    u32 oldVal = hal_xadd32(&rself->restartWorkerCounter, -1);
                    ocrAssert(oldVal > 0 && oldVal <= rself->computeWorkerCount);
                    if (oldVal == 1) {
                        DPRINTF(DEBUG_LVL_VERB, "Resuming after restart...\n");
                        rself->timestamp = salGetTime();
                        hal_fence();
                        rself->stateOfRestart = 0;
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Waiting to resume after restart...\n");
                        while (rself->stateOfRestart != 0)
                            ;
                    }
                    if (hal_cmpswap32(&rself->resumeAfterRestart, 1, 0) == 1) {
                        ocrAssert(rself->restartWorkerCounter == 0);
                    }
                    ocrAssert(rself->resumeAfterRestart == 0);
                } else if (rself->stateOfCheckpoint == 0 &&
                           worker->stateOfCheckpoint != 0) {
                    //Checkpoint got canceled
                    worker->stateOfCheckpoint = 0;
                    u32 oldVal = hal_xadd32(&rself->checkpointWorkerCounter, -1);
                    ocrAssert(oldVal > 0 && oldVal <= rself->computeWorkerCount);
                }
            }
        } else {
            //When shutting down, reset the resiliency state
            rself->stateOfCheckpoint = 0;
            rself->stateOfRestart = 0;
            PD_MSG_FIELD_O(returnDetail) = 0;
        }
#else
        PD_MSG_FIELD_O(returnDetail) = 0;
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    case PD_MSG_RESILIENCY_CHECKPOINT: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_RESILIENCY_CHECKPOINT
#ifdef ENABLE_RESILIENCY
        ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
        ocrCheckpointProp prop = PD_MSG_FIELD_I(properties);
        switch(prop) {
        case OCR_CHECKPOINT_PD_READY:
            {
                if (!rself->shutdownInProgress && !rself->stateOfRestart)
                    checkinPdForCheckpoint(self);
                break;
            }
        case OCR_CHECKPOINT_PD_START:
            {
                startPdCheckpoint(self);
                break;
            }
        case OCR_CHECKPOINT_PD_DONE:
            {
                checkoutPdFromCheckpoint(self);
                break;
            }
        case OCR_CHECKPOINT_PD_RESUME:
            {
                resumePdAfterCheckpoint(self);
                break;
            }
        case OCR_RESTART_PD_TRUE:
            {
                ocrAssert(self->myLocation != 0);
                ocrAssert(salCheckpointExists());
                rself->stateOfRestart = 1;
                hal_fence();
                rself->initialCheckForRestart = 1;
                rself->stateOfCheckpoint = 0;
                break;
            }
        case OCR_RESTART_PD_FALSE:
            {
                ocrAssert(self->myLocation != 0);
                rself->initialCheckForRestart = 1;
                break;
            }
        case OCR_RESTART_PD_READY:
            {
                ocrAssert(rself->stateOfRestart);
                checkinPdForRestart(self);
                break;
            }
        case OCR_RESTART_PD_START:
            {
                startPdRestart(self);
                break;
            }
        case OCR_RESTART_PD_DONE:
            {
                checkoutPdFromRestart(self);
                break;
            }
        case OCR_RESTART_PD_RESUME:
            {
                rself->resumeAfterRestart = 1;
                break;
            }
        default:
            // Not handled
            ocrAssert(0);
        }
#else
        PD_MSG_FIELD_O(returnDetail) = 0;
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    default:
        // Not handled
        ocrAssert(0);
    }

    hcSchedNotifyPostProcessMessage(self, msg);

    if ((msg->type & PD_MSG_RESPONSE) && (msg->type & PD_MSG_REQ_RESPONSE)) {
        // response is issued:
        // flip required response bit
        msg->type &= ~PD_MSG_REQ_RESPONSE;
        // flip src and dest locations
        ocrLocation_t src = msg->srcLocation;
        msg->srcLocation = msg->destLocation;
        msg->destLocation = src;
    } // when (!PD_MSG_REQ_RESPONSE) we were processing an asynchronous processMessage's RESPONSE

    // This code is not needed but just shows how things would be handled (probably
    // done by sub-functions)
    if(isBlocking && (msg->type & PD_MSG_REQ_RESPONSE)) {
        ocrAssert((msg->type & PD_MSG_RESPONSE) || ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_METADATA_COMM));
        // If we were blocking and needed a response
        // we need to make sure there is one
    }

    RETURN_PROFILE(returnCode);
}

u8 hcPdProcessEvent(ocrPolicyDomain_t* self, pdEvent_t **evt, u32 idx) {
    // Simple version to test out micro tasks for now. This just executes a blocking
    // call to the regular process message and returns NULL
    ocrAssert(idx == 0);
    ocrAssert(((*evt)->properties & PDEVT_TYPE_MASK) == PDEVT_TYPE_MSG);
    pdEventMsg_t *evtMsg = (pdEventMsg_t*)*evt;
    ocrWorker_t * worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    // Check if we need to restore a context in which the MT is supposed to execute.
    // Can typically happen in deferred execution where the EDT user code is done
    // but there still is pending OCR operations in the form of MT to execute.
    ocrTask_t * curTask = worker->curTask;
    if (evtMsg->ctx) {
        worker->curTask = evtMsg->ctx;
    }
    ocrPolicyMsg_t * msg = evtMsg->msg;
    DPRINTF(DEBUG_LVL_VERB, "hcPdProcessEvent executing msg of type 0x%"PRIx64"\n",
            (u64)(msg->type & PD_MSG_TYPE_ONLY));
    hcPolicyDomainProcessMessage(self, msg, true);
    worker->curTask = curTask;
    *evt = NULL;
    return 0;
}



u8 hcPdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {
    ocrAssert(0);
    return 0;
}

u8 hcPdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
    ocrAssert(0);
    return 0;
}

u8 hcPdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
    ocrAssert(0);
    return 0;
}

void* hcPdMalloc(ocrPolicyDomain_t *self, u64 size) {
    START_PROFILE(pd_hc_PdMalloc);
#ifdef NANNYMODE_SYSALLOC
    // Just try in the first allocator
    void* toReturn = malloc(size);
    ocrAssert(toReturn != NULL);
#else
    // Just try in the first allocator
    void* toReturn = NULL;
#ifdef OCR_MONITOR_ALLOCATOR
    u64 starttime = 0;
    OCR_TOOL_TRACE_GETTIME(starttime);
#endif
    toReturn = self->allocators[0]->fcts.allocate(self->allocators[0], size, OCR_ALLOC_HINT_PDMALLOC);
#ifdef OCR_MONITOR_ALLOCATOR
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_ALLOCATOR, OCR_ACTION_ALLOCATE, traceAlloc, starttime, (u64)OCR_ALLOC_PDMALLOC, size, (u64)OCR_ALLOC_HINT_PDMALLOC, toReturn);
#endif
    if(toReturn == NULL)
        DPRINTF(DEBUG_LVL_WARN, "Failed PDMalloc for size %"PRIx64"\n", size);
    ocrAssert(toReturn != NULL);
#endif
    RETURN_PROFILE(toReturn);
}

void hcPdFree(ocrPolicyDomain_t *self, void* addr) {
    START_PROFILE(pd_hc_PdFree);
#ifdef NANNYMODE_SYSALLOC
    // Just try in the first allocator
    free(addr);
#else
#ifdef OCR_MONITOR_ALLOCATOR
    u64 starttime = 0;
    OCR_TOOL_TRACE_GETTIME(starttime);
#endif
    // May result in leaks but better than the alternative...
    allocatorFreeFunction(addr);
#ifdef OCR_MONITOR_ALLOCATOR
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_ALLOCATOR, OCR_ACTION_DEALLOCATE, traceDealloc, starttime, (u64)OCR_ALLOC_PDFREE, addr);
#endif
#endif
    RETURN_PROFILE();
}

ocrPolicyDomain_t * newPolicyDomainHc(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrParamList_t *perInstance) {

    ocrPolicyDomainHc_t * derived = (ocrPolicyDomainHc_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainHc_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;
    ocrAssert(base);
#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, base, statsObject, perInstance);
#else
    factory->initialize(factory, base, perInstance);
#endif

    return base;
}

void initializePolicyDomainHc(ocrPolicyDomainFactory_t * factory, ocrPolicyDomain_t* self,
#ifdef OCR_ENABLE_STATISTICS
                              ocrStats_t *statsObject,
#endif
                              ocrParamList_t *perInstance) {
#ifdef OCR_ENABLE_STATISTICS
    self->statsObject = statsObject;
#endif

    initializePolicyDomainOcr(factory, self, perInstance);

    ocrPolicyDomainHc_t* derived = (ocrPolicyDomainHc_t*) self;
    derived->rlSwitch.legacySecondStart = false;
#ifdef ENABLE_RESILIENCY
    derived->faultArgs.kind = OCR_FAULT_NONE;
    derived->shutdownInProgress = 0;
    derived->stateOfCheckpoint = 0;
    derived->stateOfRestart = 0;
    derived->initialCheckForRestart = 0;
    derived->resiliencyInProgress = 0;
    derived->resumeAfterCheckpoint = 0;
    derived->resumeAfterRestart = 0;
    derived->quiesceComms = 0;
    derived->quiesceComps = 0;
    derived->commStopped = 0;
    derived->fault = 0;
    derived->recover = 0;
    derived->computeWorkerCount = 0;
    derived->faultMonitorCounter = 0;
    derived->checkpointWorkerCounter = 0;
    derived->checkpointPdCounter = 0;
    derived->restartWorkerCounter = 0;
    derived->restartPdCounter = 0;
    derived->checkpointInterval = OCR_CHECKPOINT_INTERVAL;
    derived->timestamp = 0;
    derived->calTime = 0;
    derived->currCheckpointName = NULL;
    derived->prevCheckpointName = NULL;
#endif
}

static void destructPolicyDomainFactoryHc(ocrPolicyDomainFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrPolicyDomainFactory_t * newPolicyDomainFactoryHc(ocrParamList_t *perType) {
    ocrPolicyDomainFactory_t* base = (ocrPolicyDomainFactory_t*) runtimeChunkAlloc(sizeof(ocrPolicyDomainFactoryHc_t), NONPERSISTENT_CHUNK);
    // Set factory's methods
#ifdef OCR_ENABLE_STATISTICS
    base->instantiate = FUNC_ADDR(ocrPolicyDomain_t*(*)(ocrPolicyDomainFactory_t*,ocrStats_t*,
                                  ocrCost_t *,ocrParamList_t*), newPolicyDomainHc);
    base->initialize = FUNC_ADDR(void(*)(ocrPolicyDomainFactory_t*,ocrPolicyDomain_t*,
                                         ocrStats_t*,ocrCost_t *,ocrParamList_t*), initializePolicyDomainHc);
#endif

    base->instantiate = &newPolicyDomainHc;
    base->initialize = &initializePolicyDomainHc;
    base->destruct = &destructPolicyDomainFactoryHc;

    // Set future PDs' instance  methods
    base->policyDomainFcts.destruct = FUNC_ADDR(void(*)(ocrPolicyDomain_t*), hcPolicyDomainDestruct);
    base->policyDomainFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrRunlevel_t, u32), hcPdSwitchRunlevel);
    base->policyDomainFcts.processMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*,ocrPolicyMsg_t*,u8), hcPolicyDomainProcessMessage);
    base->policyDomainFcts.processEvent = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, pdEvent_t**, u32), hcPdProcessEvent);

    base->policyDomainFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrLocation_t, ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                         hcPdSendMessage);
    base->policyDomainFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcPdPollMessage);
    base->policyDomainFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcPdWaitMessage);

    base->policyDomainFcts.pdMalloc = FUNC_ADDR(void*(*)(ocrPolicyDomain_t*,u64), hcPdMalloc);
    base->policyDomainFcts.pdFree = FUNC_ADDR(void(*)(ocrPolicyDomain_t*,void*), hcPdFree);
#ifdef OCR_ENABLE_STATISTICS
    base->policyDomainFcts.getStats = FUNC_ADDR(ocrStats_t*(*)(ocrPolicyDomain_t*),hcGetStats);
#endif

    return base;
}

#endif /* ENABLE_POLICY_DOMAIN_HC */
