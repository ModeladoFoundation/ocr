/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "debug.h"
#include "ocr-config.h"

#ifdef ENABLE_COST_MAPPER_HC

#include "ocr-sysboot.h"
#include "ocr-policy-domain.h"
#include "ocr-component.h"
//#include "component/hc/hc-component.h"
#include "component/component-all.h"
#include "cost-mapper/cost-mapper-all.h"
#include "hc-cost-mapper.h"

ocrCostMapper_t* newCostMapperHc(ocrCostMapperFactory_t * factory, ocrParamList_t *perInstance) {
    ocrCostMapper_t* base = (ocrCostMapper_t*) runtimeChunkAlloc(sizeof(ocrCostMapperHc_t), NULL);
    base->fguid.guid = UNINITIALIZED_GUID;
    base->fguid.metaDataPtr = base;
    base->fcts = factory->fcts;
    base->componentState = NULL;
    return base;
}

void destructCostMapperFactoryHc(ocrCostMapperFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

void hcCostMapperDestruct(ocrCostMapper_t *self) {
}

void hcCostMapperBegin(ocrCostMapper_t *self, ocrPolicyDomain_t * PD) {
}

void hcCostMapperStart(ocrCostMapper_t *self, ocrPolicyDomain_t * PD) {
    ocrCostMapperHc_t * hcCostMapper = (ocrCostMapperHc_t*)self;
    ocrComponentFactory_t * hcCompStateFact = PD->componentFactories[componentWst_id];
    ocrFatGuid_t hints = {NULL_GUID, NULL};
    hcCostMapper->base.componentState = hcCompStateFact->instantiate(hcCompStateFact, hints, 0);
}

void hcCostMapperStop(ocrCostMapper_t *self) {
}

void hcCostMapperFinish(ocrCostMapper_t *self) {
}

u8 hcCostMapperTake(ocrCostMapper_t *self, ocrLocation_t source, ocrFatGuid_t *component, ocrFatGuid_t hints, ocrFatGuid_t *costs, ocrFatGuid_t *mappings, u32 properties) {
    ocrComponent_t *state = self->componentState;
    state->fcts.remove(state, source, component, hints, properties);
    return 0;
}

u8 hcCostMapperGive(ocrCostMapper_t *self, ocrLocation_t source, ocrFatGuid_t component, ocrFatGuid_t hints, u32 properties) {
    ocrComponent_t *state = self->componentState;
    state->fcts.insert(state, source, component, hints, properties);
    return 0;
}

ocrCostMapperFactory_t * newOcrCostMapperFactoryHc(ocrParamList_t *perType) {
    ocrCostMapperFactory_t* base = (ocrCostMapperFactory_t*) runtimeChunkAlloc(
        sizeof(ocrCostMapperFactoryHc_t), (void *)1);

    base->instantiate = &newCostMapperHc;
    base->destruct = &destructCostMapperFactoryHc;

    base->fcts.begin = FUNC_ADDR(void (*)(ocrCostMapper_t*, ocrPolicyDomain_t*), hcCostMapperBegin);
    base->fcts.start = FUNC_ADDR(void (*)(ocrCostMapper_t*, ocrPolicyDomain_t*), hcCostMapperStart);
    base->fcts.stop = FUNC_ADDR(void (*)(ocrCostMapper_t*), hcCostMapperStop);
    base->fcts.finish = FUNC_ADDR(void (*)(ocrCostMapper_t*), hcCostMapperFinish);
    base->fcts.destruct = FUNC_ADDR(void (*)(ocrCostMapper_t*), hcCostMapperDestruct);
    base->fcts.take = FUNC_ADDR(u8 (*)(ocrCostMapper_t*, ocrLocation_t, ocrFatGuid_t*, ocrFatGuid_t, ocrFatGuid_t*, ocrFatGuid_t*, u32), hcCostMapperTake);
    base->fcts.give = FUNC_ADDR(u8 (*)(ocrCostMapper_t*, ocrLocation_t, ocrFatGuid_t, ocrFatGuid_t, u32), hcCostMapperGive);

    return base;
}

#endif /* ENABLE_COST_MAPPER_HC */