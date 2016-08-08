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
                //Get var args
                void (*traceFunc)() = va_arg(ap, void *);
                ocrGuid_t taskGuid = va_arg(ap, ocrGuid_t);
                ocrEdt_t func = va_arg(ap, ocrEdt_t);
                //Callback
                traceFunc(location, evtType, objType, actionType, workerId, timestamp, parent, taskGuid, func);
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
                //Get var args
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
            }
                break;

            case OCR_ACTION_SCHED_MSG_RCV:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(SCHEDULER, schedMsgRcv, tr, taskGuid) = curTask;
                PUSH_TO_TRACE_DEQUE();
            }
                break;

            case OCR_ACTION_SCHED_INVOKE:
            {
                //Handle trace object manually.  No callback for this trace event.
                INIT_TRACE_OBJECT();
                ocrGuid_t curTask = va_arg(ap, ocrGuid_t);
                TRACE_FIELD(SCHEDULER, schedInvoke, tr, taskGuid) = curTask;
                PUSH_TO_TRACE_DEQUE();
            }
                break;

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
