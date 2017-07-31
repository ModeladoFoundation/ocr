/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_EXTENSION_LEGACY_FIBERS

#include "fcontext.h"

//#include "debug.h"
#include "ocr-worker.h"

#define UNREACHABLE ASSERT(!"UNREACHABLE")

static inline void _set_curr_fiber(fcontext_state_t *fiber) {
    ocrWorker_t *worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    worker->curr_fiber = fiber;
}

static inline fcontext_state_t *_get_curr_fiber(void) {
    ocrWorker_t *worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    return worker->curr_fiber;
}

static inline void _fiber_exit(fcontext_state_t *current,
                                         fcontext_t next) {
    fcontext_swap(next, current);
    UNREACHABLE;
}

static inline void _fiber_suspend(fcontext_state_t *current,
                                      fcontext_fn_t transfer_fn,
                                      void *arg) {
    // switching to new context
    fcontext_state_t *fresh_fiber = fcontext_create(transfer_fn);
    _set_curr_fiber(fresh_fiber);
    fcontext_transfer_t swap_data = fcontext_swap(fresh_fiber->context, arg);
    // switched back to this context
    _set_curr_fiber(current);
    // destroy the context that resumed this one since it's now defunct
    // (there are no other handles to it, and it will never be resumed)
    // (NOTE: fresh_fiber might differ from prev_fiber)
    fcontext_state_t *prev_fiber = swap_data.data;
    fcontext_destroy(prev_fiber);
}

static ocrTask_t *_saveCurrentEdt(void) {
    ocrWorker_t *worker;
    ocrTask_t *edt;
    getCurrentEnv(NULL, &worker, &edt, NULL);
    worker->curTask = NULL;
    return edt;
}

static void _restoreCurrentEdt(ocrTask_t *edt) {
    ocrWorker_t *worker;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ASSERT(worker->curTask == NULL);
    worker->curTask = edt;
}

// this static function is copied from hc-comm-worker.c
// (and then slightly modified)
// FIXME - unused
static u8 createProcessRequestEdt(ocrPolicyDomain_t * pd, ocrGuid_t templateGuid,
        u64 * paramv, ocrGuid_t * depv) {

    u32 paramc = 1;
    u32 depc = 0;
    u32 properties = GUID_PROP_TORECORD;
    ocrWorkType_t workType = EDT_RT_WORKTYPE;

    START_PROFILE(api_EdtCreate);
    PD_MSG_STACK(msg);
    u8 returnCode = 0;
    ocrTask_t *curEdt = NULL;
    getCurrentEnv(NULL, NULL, &curEdt, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_CREATE
    msg.type = PD_MSG_WORK_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(outputEvent.guid) = NULL_GUID;
    PD_MSG_FIELD_IO(outputEvent.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(paramc) = paramc;
    PD_MSG_FIELD_IO(depc) = depc;
    PD_MSG_FIELD_I(templateGuid.guid) = templateGuid;
    PD_MSG_FIELD_I(templateGuid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(hint) = NULL_HINT;
    // This is a "fake" EDT so it has no "parent"
    PD_MSG_FIELD_I(currentEdt.guid) = NULL_GUID;
    PD_MSG_FIELD_I(currentEdt.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(parentLatch.guid) = NULL_GUID;
    PD_MSG_FIELD_I(parentLatch.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(paramv) = paramv;
    PD_MSG_FIELD_I(depv) = NULL;
    PD_MSG_FIELD_I(workType) = workType;
    PD_MSG_FIELD_I(properties) = properties;
    returnCode = pd->fcts.processMessage(pd, &msg, true);
    if(returnCode) {
        DPRINTF(DEBUG_LVL_VVERB,"fiber-worker: Created processRequest EDT GUID "GUIDF"\n", GUIDA(PD_MSG_FIELD_IO(guid.guid)));
        RETURN_PROFILE(returnCode);
    }

    RETURN_PROFILE(0);
#undef PD_MSG
#undef PD_TYPE
}

static void _fiberWorkerLoop(ocrWorker_t *worker) {
    while (worker->curState == worker->desiredState) {
        worker->fcts.workShift(worker);
        // worker may have changed if the fiber swapped threads
        getCurrentEnv(NULL, &worker, NULL, NULL);
    }
    // If the desired state changed, then we're ready to shut down,
    // so we shoudl swap back to original stack for this worker.
    // (worker may have changed if the fiber swapped threads)
    getCurrentEnv(NULL, &worker, NULL, NULL);
    _fiber_exit(_worker->curr_fiber, _worker->orig_ctx);
    UNREACHABLE;
}

static void _fiberStartEntry(fcontext_transfer_t fiber_data) {
    ocrWorker_t *worker = fiber_data.data;
    worker->orig_ctx = fiber_data.prev_context;
    _fiberWorkerLoop(worker);
    UNREACHABLE;
}

void ocrLegacyFiberStart(ocrWorker_t *worker) {
    _fiber_suspend(NULL, _fiberStartEntry, worker);
}

typedef struct {
    ocrGuid_t event_guid;
    ocrGuid_t db_guid;
} AwaitedGuids;

typedef struct {
    AwaitedGuids *guids;
    fcontext_t ctx;
} ResumeData;

typedef union {
    u64 *u64_ptr;
    ResumeData *params_ptr;
} FiberResumeParamPtr;

static ocrGuid_t fiberResumeEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    FiberResumeParamPtr params;
    params.u64_ptr = paramv;

    // get the GUID of the awaited event's payload
    params.params_ptr->guids->db_guid = depv[0].guid;
    ocrDbRelease(depv[0].guid);

    // resume the suspended fiber (and destroy the current one)
    fcontext_t next = params.params_ptr->ctx;
    _fiber_exit(_get_curr_fiber(), next);

    UNREACHABLE;
    return NULL_GUID;
}

static void _fiberReplaceEntry(fcontext_transfer_t fiber_data) {
    ResumeData resume_data;
    resume_data.guids = fiber_data.data;
    resume_data.ctx = fiber_data.prev_context;

    const u32 param_size = (sizeof(resume_data) + sizeof(u64) - 1) / sizeof(u64);
    FiberResumeParamPtr params;
    params.params_ptr = &resume_data;

    // FIXME - this should be a "runtime" task
    // and must stay within the current policy domain
    ocrGuid_t resumeTemplate, resumeEdt;
    ocrEdtTemplateCreate(&resumeTemplate, fiberResumeEdt, param_size, 1);
    ocrEdtCreate(&resumeEdt, resumeTemplate,
            param_size, params.u64_ptr, 1, NULL,
            EDT_PROP_NONE, NULL_HINT, NULL);
    ocrAddDependence(guids->event_guid, resumeEdt, 0, DB_MODE_RO);

    // do other work until we can resume something (or we shut down)
    _fiberWorkerLoop(fiber_data.data);
    UNREACHABLE;
}

ocrEdtDep_t ocrLegacyFiberSuspendOnEvent(ocrGuid_t event, ocrDbAccessMode_t mode) {
    AwaitedGuids guids;

    // suspend this fiber until the target event is satisfied
    guids.event_guid = event;
    _fiber_suspend(_get_curr_fiber(), _fiberReplaceEntry, &guids);
    // FIXME - need to destroy the killed fiber's EDT (leak)

    ocrEdtDep_t db_result;
    // acquire the target event's playload data block
    // (code copied from ocrLegacyBlockProgress in ocr-legacy.c)
    {
        ocrPolicyDomain_t *pd = NULL;
        ocrFatGuid_t dbResult = {.guid = ERROR_GUID, .metaDataPtr = NULL};
        ocrFatGuid_t currentEdt;
        ocrTask_t *curTask = NULL;
        PD_MSG_STACK(msg);
        getCurrentEnv(&pd, NULL, &curTask, &msg);
        currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
        currentEdt.metaDataPtr = curTask;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_ACQUIRE
        msg.type = PD_MSG_DB_ACQUIRE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid) = dbResult;
        PD_MSG_FIELD_IO(edt) = currentEdt;
        PD_MSG_FIELD_IO(destLoc) = pd->myLocation;
        PD_MSG_FIELD_IO(edtSlot) = EDT_SLOT_NONE;
        PD_MSG_FIELD_IO(properties) = mode;
        u8 returnCode = pd->fcts.processMessage(pd, &msg, true);
        ASSERT((returnCode == 0) && ((returnCode = PD_MSG_FIELD_O(returnDetail)) == 0));
        // store the resulting data block's guid and pointer in our return variable
        db_result.ptr = PD_MSG_FIELD_O(ptr);
        db_result.guid = PD_MSG_FIELD_IO(guid);
#undef PD_TYPE
#undef PD_MSG
    }

    return db_result;
}
#endif /* ENABLE_EXTENSION_LEGACY_FIBERS */
