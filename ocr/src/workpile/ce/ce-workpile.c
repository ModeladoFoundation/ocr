/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKPILE_CE

#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-workpile.h"
#include "workpile/ce/ce-workpile.h"


/******************************************************/
/* OCR-CE WorkPile                                    */
/******************************************************/

void ceWorkpileDestruct ( ocrWorkpile_t * base ) {
    ocrWorkpileCe_t* derived = (ocrWorkpileCe_t*) base;
    derived->deque->destruct(base->pd, derived->deque);
    runtimeChunkFree((u64)base, NULL);
}


u8 ceWorkpileSwitchRunlevel(ocrWorkpile_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
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
    case RL_MEMORY_OK: {
            ocrWorkpileCe_t* derived = (ocrWorkpileCe_t*)self;
            derived->deque = newDeque(self->pd, (void *) NULL_GUID, WORK_STEALING_DEQUE);
        }
        break;
    case RL_GUID_OK:
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

ocrFatGuid_t ceWorkpilePop(ocrWorkpile_t * base, ocrWorkPopType_t type,
                           ocrCost_t *cost) {

    ocrWorkpileCe_t* derived = (ocrWorkpileCe_t*) base;
    ocrFatGuid_t fguid;
    switch(type) {
    case POP_WORKPOPTYPE:
        fguid.guid = (ocrGuid_t)derived->deque->popFromHead(derived->deque, 0);
        break;
    default:
        ASSERT(0);
    }
    fguid.metaDataPtr = NULL_GUID;
    return fguid;
}

void ceWorkpilePush(ocrWorkpile_t * base, ocrWorkPushType_t type,
                    ocrFatGuid_t g ) {
    ocrWorkpileCe_t* derived = (ocrWorkpileCe_t*) base;
    derived->deque->pushAtTail(derived->deque, (void *)(g.guid), 0);
}

ocrWorkpile_t * newWorkpileCe(ocrWorkpileFactory_t * factory, ocrParamList_t *perInstance) {
    ocrWorkpile_t* derived = (ocrWorkpile_t*) runtimeChunkAlloc(sizeof(ocrWorkpileCe_t), NULL);

    factory->initialize(factory, derived, perInstance);
    return derived;
}

void initializeWorkpileCe(ocrWorkpileFactory_t * factory, ocrWorkpile_t* self, ocrParamList_t * perInstance) {
    initializeWorkpileOcr(factory, self, perInstance);
}

/******************************************************/
/* OCR-CE WorkPile Factory                            */
/******************************************************/

void destructWorkpileFactoryCe(ocrWorkpileFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrWorkpileFactory_t * newOcrWorkpileFactoryCe(ocrParamList_t *perType) {
    ocrWorkpileFactory_t* base = (ocrWorkpileFactory_t*)runtimeChunkAlloc(sizeof(ocrWorkpileFactoryCe_t), NULL);

    base->instantiate = &newWorkpileCe;
    base->initialize = &initializeWorkpileCe;
    base->destruct = &destructWorkpileFactoryCe;

    base->workpileFcts.destruct = FUNC_ADDR(void (*)(ocrWorkpile_t*), ceWorkpileDestruct);
    base->workpileFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrWorkpile_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                         phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), ceWorkpileSwitchRunlevel);
    base->workpileFcts.pop = FUNC_ADDR(ocrFatGuid_t (*)(ocrWorkpile_t*, ocrWorkPopType_t, ocrCost_t*), ceWorkpilePop);
    base->workpileFcts.push = FUNC_ADDR(void (*)(ocrWorkpile_t*, ocrWorkPushType_t, ocrFatGuid_t), ceWorkpilePush);
    return base;
}
#endif /* ENABLE_WORKPILE_CE */
