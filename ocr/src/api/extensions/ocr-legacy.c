/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_EXTENSION_LEGACY

#include "debug.h"
#include "extensions/ocr-legacy.h"
#include "ocr-errors.h"
#include "ocr-hal.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "ocr-worker.h"

#include "utils/profiler/profiler.h"

#pragma message "LEGACY extension is experimental and may not be supported on all platforms"

#define DEBUG_TYPE API

extern void freeUpRuntime(bool, u8*);
extern void bringUpRuntime(ocrConfig_t *ocrConfig);

void ocrLegacyInit(ocrGuid_t *legacyContext, ocrConfig_t * ocrConfig) {
    START_PROFILE(api_ocrLegacyInit);
    // Bug #492: legacyContext is ignored
    ASSERT(ocrConfig);
    if(ocrConfig->iniFile == NULL)
        PRINTF("ERROR: Set OCR_CONFIG to point to OCR configuration file\n");
    ASSERT(ocrConfig->iniFile);

    bringUpRuntime(ocrConfig);

    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    // Transition to USER_OK in RL_LEGACY mode so that the current thread
    // switches to USER_OK but do not try to start its worker loop.
    DPRINTF(DEBUG_LVL_INFO, "ocrLegacyInit calls switchRunlevel RL_USER_OK | RL_BRING_UP | RL_LEGACY\n");
    RESULT_ASSERT(
        pd->fcts.switchRunlevel(pd, RL_USER_OK, RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER | RL_LEGACY),
        ==, 0);
    DPRINTF(DEBUG_LVL_INFO, "ocrLegacyInit switchRunlevel RL_USER_OK | RL_BRING_UP returned\n");
    RETURN_PROFILE();
}

u8 ocrLegacyFinalize(ocrGuid_t legacyContext, bool runUntilShutdown) {
    START_PROFILE(api_ocrLegacyFinalize);
    // Bug #492: legacyContext is ignored
    ocrPolicyDomain_t *pd = NULL;
    u8 returnCode, otherReturnCode;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if(runUntilShutdown) {
        // Here, we are in COMPUTE_OK. We just need to transition to USER_OK
        // which will start mainEdt
        // Here we should enter the worker loop in non-legacy mode so that
        // the worker goes into its work loop. Do NOT set RL_LEGACY here
        // but the runtime figures out it's the second call to BRING_UP.
        // This code may happen before, simultaneous or after ocrShutdown is called
        DPRINTF(DEBUG_LVL_INFO, "ocrLegacyFinalize calls switchRunlevel RL_USER_OK | RL_BRING_UP\n");
        RESULT_ASSERT(
            pd->fcts.switchRunlevel(pd, RL_USER_OK, RL_REQUEST | RL_ASYNC | RL_BRING_UP | RL_NODE_MASTER),
            ==, 0);
        DPRINTF(DEBUG_LVL_INFO, "ocrLegacyFinalize switchRunlevel RL_USER_OK | RL_BRING_UP returned\n");
        returnCode = pd->shutdownCode;
        freeUpRuntime(true, &otherReturnCode);
    } else {
        // This mode just tears down the runtime (does not wait for an ocrShutdown() call)
        // This is useful in a program that calls OCR from time to time:
        // ocrLegacyInit()
        //  calls to ocrLegacySpawnOCR() and ocrLegacyBlockProgress
        // ocrLegacyFinalize()
        returnCode = pd->shutdownCode;
        freeUpRuntime(false, &otherReturnCode);
    }
    RETURN_PROFILE(returnCode);
}

u8 ocrLegacySpawnOCR(ocrGuid_t* handle, ocrGuid_t finishEdtTemplate, u64 paramc, u64* paramv,
                     u64 depc, ocrGuid_t* depv, ocrGuid_t legacyContext) {

    START_PROFILE(api_ocrLegacySpawnOCR);
    // Bug #492: legacyContext is ignored
    // Bug #494: a thread is lost since this function spins
    ocrGuid_t edtGuid;
    ocrGuid_t stickyGuid;
    ocrGuid_t outputEventGuid;
    ocrGuid_t depv0;
    u32 i;
    ocrEventCreate(&stickyGuid, OCR_EVENT_STICKY_T, true);
    // OCR codes do not have access to any data from heap/stack so things need to
    // be marshalled in. We also rely on depc >= 1 to "hold-off" the start of the EDT
    // and properly setup the dependence chain
    ASSERT(depc >= 1);

    for(i=0; i < depc; ++i) {
        // Verify that all dependences are known
        ASSERT(!(ocrGuidIsUninitialized(depv[i])));
    }

    // Hold off the start by saying that one dependence is not satisfied
    depv0 = depv[0];
    depv[0] = UNINITIALIZED_GUID;
    ocrEdtCreate(&edtGuid, finishEdtTemplate, paramc, paramv,
                 depc, depv, EDT_PROP_FINISH, NULL_HINT, &outputEventGuid);
    ocrAddDependence(outputEventGuid, stickyGuid, 0, DB_DEFAULT_MODE);
    // Actually start the OCR EDT
    ocrAddDependence(depv0, edtGuid, 0, DB_DEFAULT_MODE);
    *handle = stickyGuid;
    RETURN_PROFILE(0);
}

ocrGuid_t ocrWait(ocrGuid_t outputEvent) {
    START_PROFILE(api_ocrWait);
    //DPRINTF(DEBUG_LVL_WARN, "ocrWait is deprecated -- use ocrLegacyBlockProgress instead\n");
    ocrGuid_t outputGuid;
    if(ocrLegacyBlockProgress(outputEvent, &outputGuid, NULL, NULL, LEGACY_PROP_NONE) == 0) {
        RETURN_PROFILE(outputGuid);
    }
    RETURN_PROFILE(ERROR_GUID);
}

bool ocrLegacyIsChannelReady(ocrGuid_t channelGuid) {
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_EVT_GET
    msg.type = PD_MSG_EVT_GET | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_I(guid.guid) = channelGuid;
    PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
    pd->fcts.processMessage(pd, &msg, true);
    return !ocrGuidIsError(PD_MSG_FIELD_O(data).guid);
#undef PD_TYPE
#undef PD_MSG

}

// Runtime legacy properties 
#define WAIT_CREATE   0x1
#define WAIT_SATISFY  0x2

u8 ocrLegacyBlockProgress(ocrGuid_t evtHandle, ocrGuid_t* guid, void** result, u64* size, u16 properties) {
    START_PROFILE(api_ocrLegacyBlockProgress);
    u16 rtProps = properties;
    if (properties == LEGACY_PROP_NONE) {
        //TODO: logic for PROP_NONE sound wrong
        rtProps = WAIT_SATISFY;
    } else if (properties == LEGACY_PROP_WAIT_FOR_CREATE) {
        rtProps = WAIT_CREATE | WAIT_SATISFY;
    } else if (properties == LEGACY_PROP_WAIT_IF_CREATED) {
        rtProps = WAIT_SATISFY;
    } else if (properties == LEGACY_PROP_CHECK) {
        rtProps = 0;
    }

    ocrPolicyDomain_t *pd = NULL;
    ocrFatGuid_t dbResult = {.guid = ERROR_GUID, .metaDataPtr = NULL};
    ocrFatGuid_t currentEdt;
    ocrTask_t *curTask = NULL;

    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, &curTask, &msg);
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;
#define PD_MSG (&msg)
    ocrLocation_t loc;
    pd->guidProviders[0]->fcts.getLocation(pd->guidProviders[0], evtHandle, &loc);
    // HACK for BUG #865 This should be done as part of MD cloning
    if (loc == pd->myLocation) {
#define PD_TYPE PD_MSG_GUID_INFO
        do {
            getCurrentEnv(NULL, NULL, NULL, &msg);
            msg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_IO(guid.guid) = evtHandle;
            PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
            PD_MSG_FIELD_I(properties) = KIND_GUIDPROP | RMETA_GUIDPROP;
            u8 returnCode = pd->fcts.processMessage(pd, &msg, true);
            //Warning PD_MSG_GUID_INFO returns GUID properties as 'returnDetail', not error code
            if(returnCode != 0) {
                RETURN_PROFILE(returnCode);
            }

            if(PD_MSG_FIELD_IO(guid.metaDataPtr) == NULL) {
                // if ((properties == LEGACY_PROP_NONE) || (properties & LEGACY_PROP_CHECK)) {
                if (!(rtProps & WAIT_CREATE)) {
                    RETURN_PROFILE(OCR_EINVAL);
                }
                // Everytime there's a busy wait loop like this we need to
                // call into the PD to try and make progress on other work
                // For instance: might have to unlock some incoming label create here
                // else if(properties == LEGACY_PROP_WAIT_FOR_CREATE) {
                if (rtProps & WAIT_CREATE) {
                    ocrPolicyDomain_t * pd;
                    PD_MSG_STACK(msg);
                    getCurrentEnv(&pd, NULL, NULL, &msg);
                #undef PD_MSG
                #undef PD_TYPE
                #define PD_MSG (&msg)
                #define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
                    msg.type = PD_MSG_MGT_MONITOR_PROGRESS | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                    //BUG #131 helper-mode: not sure if the caller should register a progress function or if the
                    //scheduler should know what to do for each type of monitor progress
                    PD_MSG_FIELD_IO(properties) = 0;
                    PD_MSG_FIELD_I(monitoree) = NULL;
                    RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
                #undef PD_MSG
                #undef PD_TYPE
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_INFO
                    continue; // Tightest loop to see how this goes
                }
            }
            else {
#ifdef OCR_ASSERT

                ocrEvent_t* eventToYieldFor = (ocrEvent_t *)PD_MSG_FIELD_IO(guid.metaDataPtr);
                ASSERT(eventToYieldFor->kind == OCR_EVENT_STICKY_T ||
                       eventToYieldFor->kind == OCR_EVENT_IDEM_T);
#endif
                break;
            }
        } while(true);
#undef PD_TYPE
#undef PD_MSG
    } // end if local: ensures the event is created locally
    // If it was remote we directly use the EVT_GET operation
    // We can't wait on once/latch events since they could get freed asynchronously
    if (properties & LEGACY_PROP_NONE) {
        rtProps |= WAIT_CREATE;
    }

    bool notDone = true;
    do {
        hal_pause();
        PD_MSG_STACK(msg2);
        getCurrentEnv(NULL, NULL, NULL, &msg2);
#define PD_MSG (&msg2)
#define PD_TYPE PD_MSG_EVT_GET
        msg2.type = PD_MSG_EVT_GET | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_I(guid.guid) = evtHandle;
        PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
        pd->fcts.processMessage(pd, &msg2, true);
        dbResult = PD_MSG_FIELD_O(data);
#undef PD_TYPE
#undef PD_MSG
        // Everytime there's a busy wait loop like this we need to
        // call into the PD to try and make progress on other work
        // For instance: might have to unlock some incoming label create here
        // if ((properties & LEGACY_PROP_CHECK) && ocrGuidIsError(dbResult.guid)) {
        if (!(rtProps & WAIT_CREATE) && ocrGuidIsError(dbResult.guid)) {
            // Event hasn't been created yet
            return OCR_EINVAL;
        }
        // Other cases just wait it out
        notDone = (ocrGuidIsUninitialized(dbResult.guid) || ocrGuidIsError(dbResult.guid));
        if (notDone) {
            if (!(rtProps & WAIT_SATISFY)) {
                return OCR_EINVAL;
            }
            ocrPolicyDomain_t * pd;
            PD_MSG_STACK(msg);
            getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
            msg.type = PD_MSG_MGT_MONITOR_PROGRESS | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            //BUG #131 helper-mode: not sure if the caller should register a progress function or if the
            //scheduler should know what to do for each type of monitor progress
            PD_MSG_FIELD_IO(properties) = 0;
            PD_MSG_FIELD_I(monitoree) = NULL;
            RESULT_PROPAGATE(pd->fcts.processMessage(pd, &msg, true));
#undef PD_MSG
#undef PD_TYPE
        }
    } while(notDone);

    if(!(ocrGuidIsNull(dbResult.guid))) {
        if(dbResult.metaDataPtr == NULL) {
            // We now need to acquire the DB
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
            msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
            PD_MSG_FIELD_IO(guid) = dbResult;
            PD_MSG_FIELD_IO(edt) = currentEdt;
            PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
            PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
            PD_MSG_FIELD_IO(properties) = DB_MODE_RW;
            u8 returnCode = pd->fcts.processMessage(pd, &msg, true);
            if(!((returnCode == 0) && ((returnCode = PD_MSG_FIELD_O(returnDetail)) == 0))) {
                RETURN_PROFILE(returnCode);
            }
            if(result != NULL)
                *result = PD_MSG_FIELD_O(ptr);
            if(size != NULL) {
                //BUG #162
                // Because we do not have the metadata cloned, must read the size from the msg
                *size = PD_MSG_FIELD_O(size);
            }
            dbResult = PD_MSG_FIELD_IO(guid);
#undef PD_TYPE
#undef PD_MSG
        } else {
            if(result != NULL)
                *result = ((ocrDataBlock_t*)(dbResult.metaDataPtr))->ptr;
            if(size != NULL) {
                *size = ((ocrDataBlock_t*)(dbResult.metaDataPtr))->size;
            }
        }
        ASSERT(dbResult.metaDataPtr != NULL);
    } else {
        if(size != NULL) {
            *size = 0;
        }
        if(result != NULL) {
            *result = NULL;
        }
    }

    if(guid != NULL)
        *guid = dbResult.guid;
    RETURN_PROFILE(0);
}

#endif /* ENABLE_EXTENSION_LEGAGY */
