#include "ocr-config.h"
#ifdef ENABLE_WORKER_SYSTEM

#include <stdarg.h>

#include "ocr-sal.h"
#include "utils/ocr-utils.h"
#include "ocr-types.h"
#include "utils/tracer/tracer.h"
#include "worker/hc/hc-worker.h"


bool isDequeFull(deque_t *deq){
    if(deq == NULL) return false;
    s32 head = deq->head;
    s32 tail = deq->tail;
    if(tail == (INIT_DEQUE_CAPACITY + head)){
        return true;
    }else{
        return false;
    }
}

bool isSystem(ocrPolicyDomain_t *pd){
    u32 idx = (pd->workerCount)-1;
    ocrWorker_t *wrkr = pd->workers[idx];
    if(wrkr->type == SYSTEM_WORKERTYPE){
        return true;
    }else{
        return false;
    }
}

bool isSupportedTraceType(bool evtType, ocrTraceType_t ttype, ocrTraceAction_t atype){
    //Hacky sanity check to ensure va_arg got valid trace info if provided.
    //return true if supported (list will expand as more trace types become needed/supported)
    return ((ttype >= OCR_TRACE_TYPE_EDT && ttype < OCR_TRACE_TYPE_MAX) &&
            (atype >= OCR_ACTION_CREATE  && atype < OCR_ACTION_MAX) &&
            (evtType == true || evtType == false));
}

//Create a trace object subject to trace type, and push to HC worker's deque, to be processed by system worker.
void populateTraceObject(u64 location, bool evtType, ocrTraceType_t objType, ocrTraceAction_t actionType,
                                u64 workerId, u64 timestamp, ocrGuid_t parent, va_list ap){

    switch(objType){

    case OCR_TRACE_TYPE_EDT:

        switch(actionType){

            case OCR_ACTION_CREATE:
            {
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrEdt_t func = va_arg(ap, ocrEdt_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, func);
                break;
            }
            case OCR_ACTION_TEMPLATE_CREATE:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void*); // discard traceFunc
                ocrGuid_t guid = va_arg(ap, ocrGuid_t);
                ocrEdt_t funcPtr = va_arg(ap, ocrEdt_t);
                TRACE_FIELD(TASK, taskTemplateCreate, tr, templateGuid) = guid;
                TRACE_FIELD(TASK, taskTemplateCreate, tr, funcPtr) = funcPtr;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t guid = va_arg(ap, ocrGuid_t);
                ocrEdt_t funcPtr = va_arg(ap, ocrEdt_t);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, guid, funcPtr);
#endif
                break;
            }

            case OCR_ACTION_DESTROY:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrEdt_t func = va_arg(ap, ocrEdt_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, func);
                break;
            }
            case OCR_ACTION_RUNNABLE:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrEdt_t func = va_arg(ap, ocrEdt_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, func);
                break;
            }
            case OCR_ACTION_SCHEDULED:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                deque_t *deq = va_arg(ap, deque_t *);

                TRACE_FIELD(TASK, taskScheduled, tr, taskGuid) = curTask;
                TRACE_FIELD(TASK, taskScheduled, tr, deq) = deq;
                PUSH_TO_TRACE_DEQUE();
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrGuid_t satisfyee = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, satisfyee);
                break;
            }
            case OCR_ACTION_ADD_DEP:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t src = va_arg(ap, ocrGuid_t);
                ocrGuid_t dest = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, src, dest);
                break;
            }
            case OCR_ACTION_EXECUTE:
            {
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrEdt_t func = va_arg(ap, ocrEdt_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, func);
                break;
            }
            case OCR_ACTION_FINISH:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid);
                break;
            }
            case OCR_ACTION_SHIFT:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrEdt_t func = va_arg(ap, ocrEdt_t);
                bool shiftFrom = (bool)va_arg(ap, int);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, func, shiftFrom);
                break;
            }
            case OCR_ACTION_DATA_ACQUIRE:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrGuid_t dbGuid = va_arg(ap, ocrGuid_t);
                u64 dbSize = va_arg(ap, u64);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, dbGuid, dbSize);
                break;
            }

            case OCR_ACTION_DATA_RELEASE:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrGuid_t dbGuid = va_arg(ap, ocrGuid_t);
                u64 dbSize = va_arg(ap, u64);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, dbGuid, dbSize);
                break;
            }

            default:
                break;

        }
        break;

    case OCR_TRACE_TYPE_EVENT:

        switch(actionType){

            case OCR_ACTION_CREATE:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t eventGuid = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, eventGuid);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t eventGuid = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, eventGuid);
                break;
            }
            case OCR_ACTION_ADD_DEP:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t src = va_arg(ap, ocrGuid_t);
                ocrGuid_t dest = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, src, dest);
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t eventGuid = va_arg(ap, ocrGuid_t);
                ocrGuid_t satisfyee = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, eventGuid, satisfyee);
                break;
            }
            default:
                break;
        }

        break;

    case OCR_TRACE_TYPE_MESSAGE:

        switch(actionType){

            case OCR_ACTION_END_TO_END:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrLocation_t src = va_arg(ap, ocrLocation_t);
                ocrLocation_t dst = va_arg(ap, ocrLocation_t);
                u64 usefulSize = va_arg(ap, u64);
                u64 marshTime = va_arg(ap, u64);
                u64 sendTime = va_arg(ap, u64);
                u64 rcvTime = va_arg(ap, u64);
                u64 unMarshTime = va_arg(ap, u64);
                u64 type = va_arg(ap, u64);

                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, src) = src;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, dst) = dst;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, usefulSize) = usefulSize;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, marshTime) = marshTime;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, sendTime) = sendTime;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, rcvTime) = rcvTime;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, unMarshTime) = unMarshTime;
                TRACE_FIELD(MESSAGE, msgEndToEnd, tr, type) = type;
                PUSH_TO_TRACE_DEQUE();
                break;
            }

            default:
                break;
        }

        break;

    case OCR_TRACE_TYPE_DATABLOCK:

        switch(actionType){

            case OCR_ACTION_CREATE:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t dbGuid = va_arg(ap, ocrGuid_t);
                u64 dbSize = va_arg(ap, u64);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, dbGuid, dbSize);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t dbGuid = va_arg(ap, ocrGuid_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, dbGuid);
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_WORKER:

        switch(actionType){

            case OCR_ACTION_WORK_REQUEST:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                TRACE_FIELD(EXECUTION_UNIT, exeWorkRequest, tr, placeHolder) = NULL;
                PUSH_TO_TRACE_DEQUE();
                break;
            }
            case OCR_ACTION_WORK_TAKEN:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                deque_t *deq = va_arg(ap, deque_t *);

                TRACE_FIELD(EXECUTION_UNIT, exeWorkTaken, tr, foundGuid) = curTask;
                TRACE_FIELD(EXECUTION_UNIT, exeWorkTaken, tr, deq) = deq;
                PUSH_TO_TRACE_DEQUE();
                break;
            }

            default:
                break;

        }

        break;

    case OCR_TRACE_TYPE_SCHEDULER:

        switch(actionType){

            case OCR_ACTION_SCHED_MSG_SEND:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(SCHEDULER, schedMsgSend, tr, taskGuid) = curTask;
                PUSH_TO_TRACE_DEQUE();
                break;
            }

            case OCR_ACTION_SCHED_MSG_RCV:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(SCHEDULER, schedMsgRcv, tr, taskGuid) = curTask;
                PUSH_TO_TRACE_DEQUE();
                break;
            }

            case OCR_ACTION_SCHED_INVOKE:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(SCHEDULER, schedInvoke, tr, taskGuid) = curTask;
                PUSH_TO_TRACE_DEQUE();
                break;
            }

            default:
                break;

        }

        break;

    case OCR_TRACE_TYPE_API_EDT:

        switch(actionType){

            case OCR_ACTION_CREATE:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                va_arg(ap, ocrGuid_t); // discard edtGuid
                ocrGuid_t templateGuid = va_arg(ap, ocrGuid_t);
                u32 paramc = va_arg(ap, u32);
                u64 *paramvIn = va_arg(ap, u64 *);
                u64 paramvOut[MAX_PARAMS];
                if(paramc > 0 && paramvIn != NULL){
                    memcpy(paramvOut, paramvIn, sizeof(paramvOut));
                }
                u32 depc = va_arg(ap, u32);
                TRACE_FIELD(API_EDT, simEdtCreate, tr, templateGuid) = templateGuid;
                TRACE_FIELD(API_EDT, simEdtCreate, tr, paramc) = paramc;
                memcpy(TRACE_FIELD(API_EDT, simEdtCreate, tr, paramv), paramvOut, sizeof(paramvOut));
                TRACE_FIELD(API_EDT, simEdtCreate, tr, depc) = depc;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t edtGuid = va_arg(ap, ocrGuid_t); // discard edtGuid
                ocrGuid_t templateGuid = va_arg(ap, ocrGuid_t);
                u32 paramc = va_arg(ap, u32);
                u64 * paramv = va_arg(ap, u64 *);
                u32 depc = va_arg(ap, u32);
                ocrGuid_t * depv = va_arg(ap, ocrGuid_t *);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, edtGuid,
                        templateGuid, paramc, paramv, depc, depv);
#endif
                break;
            }
            case OCR_ACTION_TEMPLATE_CREATE:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                ocrEdt_t funcPtr = va_arg(ap, ocrEdt_t);
                u32 paramc = va_arg(ap, u32);
                u32 depc = va_arg(ap, u32);
                TRACE_FIELD(API_EDT, simEdtTemplateCreate, tr, funcPtr) = funcPtr;
                TRACE_FIELD(API_EDT, simEdtTemplateCreate, tr, paramc) = paramc;
                TRACE_FIELD(API_EDT, simEdtTemplateCreate, tr, depc) = depc;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrEdt_t funcPtr = va_arg(ap, ocrEdt_t);
                u32 paramc = va_arg(ap, u32);
                u32 depc = va_arg(ap, u32);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, funcPtr, paramc, depc);
#endif
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_API_EVENT:

        switch(actionType){

            case OCR_ACTION_CREATE:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                ocrEventTypes_t eventType = va_arg(ap, ocrEventTypes_t);
                TRACE_FIELD(API_EVENT, simEventCreate, tr, eventType) = eventType;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrEventTypes_t eventType = va_arg(ap, ocrEventTypes_t);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, eventType);
#endif
                break;
            }
            case OCR_ACTION_SATISFY:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                ocrGuid_t eventGuid = va_arg(ap, ocrGuid_t);
                ocrGuid_t dataGuid = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(API_EVENT, simEventSatisfy, tr, eventGuid) = eventGuid;
                TRACE_FIELD(API_EVENT, simEventSatisfy, tr, dataGuid) = dataGuid;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t eventGuid = va_arg(ap, ocrGuid_t);
                ocrGuid_t dataGuid = va_arg(ap, ocrGuid_t);
                u32 slot = va_arg(ap, u32);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, eventGuid,
                        dataGuid, slot);
#endif
                break;
            }
            case OCR_ACTION_ADD_DEP:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                ocrGuid_t source = va_arg(ap, ocrGuid_t);
                ocrGuid_t dest = va_arg(ap, ocrGuid_t);
                u32 slot = va_arg(ap, u32);
                ocrDbAccessMode_t accessMode = va_arg(ap, ocrDbAccessMode_t);
                TRACE_FIELD(API_EVENT, simEventAddDep, tr, source) = source;
                TRACE_FIELD(API_EVENT, simEventAddDep, tr, destination) = dest;
                TRACE_FIELD(API_EVENT, simEventAddDep, tr, slot) = slot;
                TRACE_FIELD(API_EVENT, simEventAddDep, tr, accessMode) = accessMode;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t source = va_arg(ap, ocrGuid_t);
                ocrGuid_t dest = va_arg(ap, ocrGuid_t);
                u32 slot = va_arg(ap, u32);
                ocrDbAccessMode_t accessMode = va_arg(ap, ocrDbAccessMode_t);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent,
                        source, dest, slot, accessMode);
#endif
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_API_DATABLOCK:

        switch(actionType){
            case OCR_ACTION_CREATE:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                va_arg(ap, ocrGuid_t); // discard dbGuid
                u64 len = va_arg(ap, u64);
                TRACE_FIELD(API_DATABLOCK, simDbCreate, tr, len) = len;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t dbGuid = va_arg(ap, ocrGuid_t);
                u64 len = va_arg(ap, u64);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent,
                        dbGuid, len);
#endif
                break;
            }
            case OCR_ACTION_DATA_RELEASE:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                ocrGuid_t guid = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(API_DATABLOCK, simDbRelease, tr, guid) = guid;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t guid = va_arg(ap, ocrGuid_t);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, guid);
#endif
                break;
            }
            case OCR_ACTION_DESTROY:
            {
#ifdef OCR_ENABLE_SIMULATOR
                INIT_TRACE_OBJECT();
                va_arg(ap, void *); // discard traceFunc
                ocrGuid_t guid = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(API_DATABLOCK, simDbDestroy, tr, guid) = guid;
                PUSH_TO_TRACE_DEQUE();
#else
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t guid = va_arg(ap, ocrGuid_t);
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, guid);
#endif
                break;
            }

            default:
                break;
        }
        break;
    default:
        break;
    }

}

void doTrace(u64 location, u64 wrkr, ocrGuid_t parent, ...){
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);

    //First check if a system worker is configured.  If not, return and do nothing;
    if((pd == NULL) || !(isSystem(pd))) return;

    u64 timestamp = salGetTime();

    va_list ap;
    va_start(ap, parent);

    //Retrieve event type and action of trace. By convention in the order below.
    bool evtType = va_arg(ap, u32);
    ocrTraceType_t objType = va_arg(ap, ocrTraceType_t);
    ocrTraceAction_t actionType = va_arg(ap, ocrTraceAction_t);

    //If no valid additional tracing info found return to normal DPRINTF
    if(!(isSupportedTraceType(evtType, objType, actionType))){
        va_end(ap);
        return;
    }
    populateTraceObject(location, evtType, objType, actionType, wrkr, timestamp, parent, ap);
    va_end(ap);

}
#endif /* ENABLE_WORKER_SYSTEM */
