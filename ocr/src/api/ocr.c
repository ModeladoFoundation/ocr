/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-sal.h"
#include "ocr-types.h"
#include "ocr-errors.h"

#define DEBUG_TYPE API

static void ocrShutdownInternal(u8 errorCode) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER ocrShutdown()\n");
    ocrPolicyDomain_t *pd = NULL;
    PD_MSG_STACK(msg);
    ocrPolicyMsg_t * msgPtr = &msg;
    ocrTask_t * curTask;
    getCurrentEnv(&pd, NULL, &curTask, msgPtr);
#define PD_MSG msgPtr
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
    msgPtr->type = PD_MSG_MGT_RL_NOTIFY | PD_MSG_REQUEST;
#ifdef ENABLE_OCR_API_DEFERRABLE
    if (!errorCode) {
        tagDeferredMsg(msgPtr, curTask);
    }
#endif
#ifdef ENABLE_DEFERRED_MSGS
    msgPtr->type |= PD_MSG_IS_DEFERRABLE;
#endif
    PD_MSG_FIELD_I(runlevel) = RL_COMPUTE_OK;
    PD_MSG_FIELD_I(properties) = RL_REQUEST | RL_BARRIER | RL_TEAR_DOWN;
    PD_MSG_FIELD_I(errorCode) = errorCode;
    u8 returnCode __attribute__((unused)) = pd->fcts.processMessage(pd, msgPtr, true);
    if (returnCode == OCR_EDEFERRED) returnCode = 0; //Ignore return code for deferred
    ASSERT((returnCode == 0));
#undef PD_MSG
#undef PD_TYPE
}

void ocrShutdown() {
    START_PROFILE(api_ocrShutdown);
    ocrShutdownInternal(0);
    RETURN_PROFILE();
}

void ocrAbort(u8 errorCode) {
    START_PROFILE(api_ocrAbort);
    ocrShutdownInternal(errorCode);
    RETURN_PROFILE();
}

u64 getArgc(void* dbPtr) {
    START_PROFILE(api_getArgc);
    DPRINTF(DEBUG_LVL_INFO, "ENTER getArgc(dbPtr=%p)\n", dbPtr);
    DPRINTF(DEBUG_LVL_INFO, "EXIT getArgc -> %"PRIu64"\n", ((u64*)dbPtr)[0]);
    RETURN_PROFILE(((u64*)dbPtr)[0]);

}

char* getArgv(void* dbPtr, u64 count) {
    START_PROFILE(api_getArgv);
    DPRINTF(DEBUG_LVL_INFO, "ENTER getArgv(dbPtr=%p, count=%"PRIu64")\n", dbPtr, count);
    u64* dbPtrAsU64 = (u64*)dbPtr;
    ASSERT(count < dbPtrAsU64[0]); // We can't ask for more args than total
    u64 offset = dbPtrAsU64[1 + count];
    DPRINTF(DEBUG_LVL_INFO, "EXIT getArgv -> %s\n", ((char*)dbPtr) + offset);
    RETURN_PROFILE(((char*)dbPtr) + offset);
}

