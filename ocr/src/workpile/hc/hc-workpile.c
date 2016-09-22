/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKPILE_HC

#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "workpile/hc/hc-workpile.h"


/******************************************************/
/* OCR-HC WorkPile                                    */
/******************************************************/

void hcWorkpileDestruct ( ocrWorkpile_t * base ) {
    runtimeChunkFree((u64)base, PERSISTENT_CHUNK);
}

u8 hcWorkpileSwitchRunlevel(ocrWorkpile_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                            phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {

    u8 toReturn = 0;

    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    switch(runlevel) {
    case RL_CONFIG_PARSE:
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    case RL_NETWORK_OK:
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP)
            self->pd = PD;
        break;
    case RL_MEMORY_OK:
        break;
    case RL_GUID_OK:
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_GUID_OK, phase)) {
            // Does this need to move up in RL_MEMORY_OK?
            ocrWorkpileHc_t* derived = (ocrWorkpileHc_t*)self;
            // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
            void * nullVal = (void *) NULL_GUID.guid;
#elif GUID_BIT_COUNT == 128
            void * nullVal =  (void *) NULL_GUID.lower;
#endif
            // By convention we stash the deque type here else default
            // Need the u64 cast otherwise icc complains about loosing significant bits
            ocrDequeType_t type = (derived->deque != NULL) ? ((ocrDequeType_t) ((u64) derived->deque)) : WORK_STEALING_DEQUE;
            derived->deque = newDeque(self->pd, nullVal, type);
        }
        if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_GUID_OK, phase)) {
            ocrWorkpileHc_t* derived = (ocrWorkpileHc_t*) self;
            derived->deque->destruct(PD, derived->deque);
        }
        break;
    case RL_COMPUTE_OK:
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
                // We get a GUID for ourself
                guidify(self->pd, (u64)self, &(self->fguid), OCR_GUID_WORKPILE);
            }
        } else {
            // Tear-down
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
                msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(guid) = self->fguid;
                PD_MSG_FIELD_I(properties) = 0;
                toReturn |= self->pd->fcts.processMessage(self->pd, &msg, false);
                self->fguid.guid = NULL_GUID;
#undef PD_MSG
#undef PD_TYPE
            }
        }
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

ocrFatGuid_t hcWorkpilePop(ocrWorkpile_t * base, ocrWorkPopType_t type,
                           ocrCost_t *cost) {

    ocrWorkpileHc_t* derived = (ocrWorkpileHc_t*) base;
    ocrFatGuid_t fguid;
    switch(type) {
    case POP_WORKPOPTYPE:
        // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
        fguid.guid.guid = (u64)derived->deque->popFromTail(derived->deque, 0);
#elif GUID_BIT_COUNT == 128
        fguid.guid.lower = (u64)derived->deque->popFromTail(derived->deque, 0);
        fguid.guid.upper = 0x0;
#endif
        break;
    case STEAL_WORKPOPTYPE:
        // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
        fguid.guid.guid = (u64)derived->deque->popFromHead(derived->deque, 1);
#elif GUID_BIT_COUNT == 128
        fguid.guid.lower = (u64)derived->deque->popFromHead(derived->deque, 1);
        fguid.guid.upper = 0x0;
#endif
        break;
    default:
        ASSERT(0);
    }
    fguid.metaDataPtr = NULL;
    return fguid;
}

void hcWorkpilePush(ocrWorkpile_t * base, ocrWorkPushType_t type,
                    ocrFatGuid_t g ) {
    ocrWorkpileHc_t* derived = (ocrWorkpileHc_t*) base;
    // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
    void * pushVal = (void *)(g.guid.guid);
#elif GUID_BIT_COUNT == 128
    void * pushVal = (void *)(g.guid.lower);
#endif
    if (type == PUSH_WORKPUSHTYPE) {
        derived->deque->pushAtTail(derived->deque, pushVal, 0);
    } else {
        ASSERT(type == PUSH_WORKPUSHBACKTYPE);
        ASSERT(derived->deque->pushAtHead != NULL);
        derived->deque->pushAtHead(derived->deque, (void *)pushVal, 0);
    }
}

ocrWorkpile_t * newWorkpileHc(ocrWorkpileFactory_t * factory, ocrParamList_t *perInstance) {
    ocrWorkpile_t* derived = (ocrWorkpile_t*) runtimeChunkAlloc(sizeof(ocrWorkpileHc_t), PERSISTENT_CHUNK);
    factory->initialize(factory, derived, perInstance);
    return derived;
}

void initializeWorkpileHc(ocrWorkpileFactory_t * factory, ocrWorkpile_t* self, ocrParamList_t * perInstance) {
    initializeWorkpileOcr(factory, self, perInstance);
    ocrWorkpileHc_t* derived = (ocrWorkpileHc_t*)self;
    derived->deque = NULL;
    if (perInstance) {
        paramListWorkpileHcInst_t * perInstanceType = (paramListWorkpileHcInst_t *) perInstance;
        // Contract to remember the type for the initialization
        // without storing it as an extra entry in the struct.
        derived->deque = (deque_t *) perInstanceType->dequeType;
    }
}

/******************************************************/
/* OCR-HC WorkPile Factory                            */
/******************************************************/

void destructWorkpileFactoryHc(ocrWorkpileFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrWorkpileFactory_t * newOcrWorkpileFactoryHc(ocrParamList_t *perType) {
    ocrWorkpileFactory_t* base = (ocrWorkpileFactory_t*)runtimeChunkAlloc(sizeof(ocrWorkpileFactoryHc_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newWorkpileHc;
    base->initialize = &initializeWorkpileHc;
    base->destruct = &destructWorkpileFactoryHc;

    base->workpileFcts.destruct = FUNC_ADDR(void (*) (ocrWorkpile_t *), hcWorkpileDestruct);
    base->workpileFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorkpile_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), hcWorkpileSwitchRunlevel);
    base->workpileFcts.pop = FUNC_ADDR(ocrFatGuid_t (*)(ocrWorkpile_t*, ocrWorkPopType_t, ocrCost_t *), hcWorkpilePop);
    base->workpileFcts.push = FUNC_ADDR(void (*)(ocrWorkpile_t*, ocrWorkPushType_t, ocrFatGuid_t), hcWorkpilePush);
    return base;
}
#endif /* ENABLE_WORKPILE_HC */
