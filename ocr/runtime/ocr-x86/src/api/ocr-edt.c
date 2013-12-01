/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "debug.h"
#include "ocr-edt.h"
#include "ocr-policy-domain-getter.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime.h"

u8 ocrEventCreate(ocrGuid_t *guid, ocrEventTypes_t eventType, bool takesArg) {
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrPolicyCtx_t *context = getCurrentWorkerContext();
    pd->createEvent(pd, guid,eventType, takesArg, context);
    return 0;
}

u8 ocrEventDestroy(ocrGuid_t eventGuid) {
    ocrEvent_t * event = NULL;
    deguidify(getCurrentPD(), eventGuid, (u64*)&event, NULL);
    event->fctPtrs->destruct(event);
    return 0;
}

u8 ocrEventSatisfySlot(ocrGuid_t eventGuid, ocrGuid_t dataGuid /*= INVALID_GUID*/, u32 slot) {
    ASSERT(eventGuid != NULL_GUID);
    ocrEvent_t * event = NULL;
    deguidify(getCurrentPD(), eventGuid, (u64*)&event, NULL);
    event->fctPtrs->satisfy(event, dataGuid, slot);
    return 0;
}

u8 ocrEventSatisfy(ocrGuid_t eventGuid, ocrGuid_t dataGuid /*= INVALID_GUID*/) {
    return ocrEventSatisfySlot(eventGuid, dataGuid, 0);
}

u8 ocrEdtTemplateCreate_internal(ocrGuid_t *guid, ocrEdt_t funcPtr, u32 paramc, u32 depc, const char* funcName) {
#ifdef OCR_ENABLE_EDT_NAMING
    ASSERT(funcName);
#endif
    ocrPolicyDomain_t *pd = getCurrentPD();
    ocrPolicyCtx_t *context = getCurrentWorkerContext();
    pd->createEdtTemplate(pd, guid, funcPtr, paramc, depc, funcName, context);

    return 0;
}

u8 ocrEdtTemplateDestroy(ocrGuid_t guid) {
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrTaskTemplate_t * taskTemplate = NULL;
    deguidify(pd, guid, (u64*)&taskTemplate, NULL);
    taskTemplate->fctPtrs->destruct(taskTemplate);
    return 0;
}

u8 ocrEdtCreate(ocrGuid_t* edtGuid, ocrGuid_t templateGuid,
                u32 paramc, u64* paramv, u32 depc, ocrGuid_t *depv,
                u16 properties, ocrGuid_t affinity, ocrGuid_t *outputEvent) {

    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrTaskTemplate_t *taskTemplate = NULL;
    deguidify(pd, templateGuid, (u64*)&taskTemplate, NULL);
    // TODO: Move this to runtime checks with error returned
    ASSERT(((taskTemplate->paramc == EDT_PARAM_UNK) && paramc != EDT_PARAM_DEF) ||
           (taskTemplate->paramc != EDT_PARAM_UNK && (paramc == EDT_PARAM_DEF || taskTemplate->paramc == paramc)));
    ASSERT(((taskTemplate->depc == EDT_PARAM_UNK) && depc != EDT_PARAM_DEF) ||
           (taskTemplate->depc != EDT_PARAM_UNK && (depc == EDT_PARAM_DEF || taskTemplate->depc == depc)));

    if(paramc == EDT_PARAM_DEF) {
        paramc = taskTemplate->paramc;
    }
    if(depc == EDT_PARAM_DEF) {
        depc = taskTemplate->depc;
    }

    // If paramc are expected, double check paramv is not NULL
    ASSERT((paramc > 0) ? (paramv != NULL) : true);

    ocrPolicyCtx_t *context = getCurrentWorkerContext();
    pd->createEdt(pd, edtGuid, taskTemplate, paramc, paramv, depc,
                  properties, affinity, outputEvent, context);

    // If guids dependences were provided, add them now
    if(depv != NULL) {
        ASSERT(depc != 0);
        u32 i = 0;
        while(i < depc) {
            ocrAddDependence(depv[i], *edtGuid, i, DB_DEFAULT_MODE);
            i++;
        }
    }
    return 0;
}

u8 ocrEdtDestroy(ocrGuid_t edtGuid) {
    ocrPolicyDomain_t * pd = getCurrentPD();
    ocrTask_t * task = NULL;
    deguidify(pd, edtGuid, (u64*)&task, NULL);
    task->fctPtrs->destruct(task);
    return 0;
}

// TODO: Pass down the mode information!!
u8 ocrAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot,
                    ocrDbAccessMode_t mode) {
    registerDependence(source, destination, slot);
    return 0;
}
