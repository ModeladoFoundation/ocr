
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocr.h"
#include "utils/tracer/tracer.h"
#include "utils/tracer/trace-events.h"
#include "ocr-perfmon.h"

#define IDX_OFFSET OCR_TRACE_TYPE_EDT
#define MAX_FCT_PTR_LENGTH 16
#define SYS_CALL_COMMAND_LENGTH 256
#define BIN_PATH_LENGTH 128

void translateObject(ocrTraceObj_t *trace);

int main(int argc, char *argv[]){

    if(argc < 2){
        printf("Error Usage: TODO\n");
        printf("\n-------- Incorrect Input ---------\n");
        printf("Usage: %s <filename>  optional : <application binary>\n\n", argv[0]);
        return 1;
    }

    char *fname = argv[1];

    ocrTraceObj_t *trace = malloc(sizeof(ocrTraceObj_t));

    //Read each trace record, and decode
    int i;
    for(i=1; i < argc; i++){
        FILE *f = fopen(argv[i], "r");
        if(f == NULL){
            printf("Error:  Unable to open provided trace binary\n");
            return 1;
        }

        while(fread(trace, sizeof(ocrTraceObj_t), 1, f)){
            translateObject(trace);
        }
        fclose(f);
    }
    return 0;
}

void genericPrint(bool evtType, ocrTraceType_t ttype, ocrTraceAction_t action,
                  u64 location, u64 workerId, u64 timestamp, ocrGuid_t parent){

    printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" |  TIMESTAMP: %"PRIu64" | TYPE: %s | ACTION: %s\n",
            evt_type[evtType], location, workerId, GUIDA(parent), timestamp, obj_type[ttype-IDX_OFFSET], action_type[action]);

    return;
}

void translateObject(ocrTraceObj_t *trace){
    ocrTraceType_t  ttype = trace->typeSwitch;
    ocrTraceAction_t action =  trace->actionSwitch;
    u64 timestamp = trace->time;
    u64 location = trace->location;
    u64 workerId = trace->workerId;
    bool evtType = trace->eventType;
    ocrGuid_t parent = trace->parent;
    switch(trace->typeSwitch){

    case OCR_TRACE_TYPE_EDT:

        switch(trace->actionSwitch){

            case OCR_ACTION_CREATE:
            {
#if !defined(OCR_ENABLE_SIMULATOR)
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskCreate, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: CREATE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid));
#else
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskCreate, trace, taskGuid);
                u32 depc = TRACE_FIELD(TASK, taskCreate, trace, depc);
                u32 paramc = TRACE_FIELD(TASK, taskCreate, trace, paramc);
                u64 *paramv = TRACE_FIELD(TASK, taskCreate, trace, paramv);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: CREATE | GUID: "GUIDF" | DEPC: %"PRId64" | PARAMC: %"PRId64" | PARAMV:",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), depc, paramc);
                u32 i;
                if(paramc > 0){
                    for(i = 0; i < paramc; i++){
                        printf(" [%"PRId32"] 0x%"PRIx64"", i, paramv[i]);
                    }
                    printf("\n");
                }else{
                    printf(" | EMPTY\n");
                }
#endif
                break;
            }
            case OCR_ACTION_TEMPLATE_CREATE:
            {
                ocrGuid_t templateGuid = TRACE_FIELD(TASK, taskTemplateCreate, trace, templateGuid);
                ocrEdt_t funcPtr = TRACE_FIELD(TASK, taskTemplateCreate,trace, funcPtr);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF"  | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: TEMPLATE_CREATE | GUID: "GUIDF" | FUNC_PTR: 0x%"PRIx64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(templateGuid), (u64)funcPtr);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDestroy, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_RUNNABLE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskReadyToRun, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF"  | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: RUNNABLE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid));
                break;
            }
            case OCR_ACTION_SCHEDULED:
            {
                ocrGuid_t curTask = TRACE_FIELD(TASK, taskScheduled, trace, taskGuid);
                deque_t *deq = TRACE_FIELD(TASK, taskScheduled, trace, deq);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: SCHEDULED | GUID: "GUIDF" | DEQUE: %p\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(curTask), deq);
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDepSatisfy, trace, taskGuid);
                ocrGuid_t satisfyee = TRACE_FIELD(TASK, taskDepSatisfy, trace, satisfyee);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: DEP_SATISFY | GUID: "GUIDF" | SATISFYEE_GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), GUIDA(satisfyee));

                break;
            }
            case OCR_ACTION_ADD_DEP:
            {
                ocrGuid_t src = TRACE_FIELD(TASK, taskDepReady, trace, src);
                ocrGuid_t dest = TRACE_FIELD(TASK, taskDepReady, trace, dest);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: ADD_DEP | SRC: "GUIDF" | DEST: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(src), GUIDA(dest));
                break;
            }
            case OCR_ACTION_EXECUTE:
            {

#if !defined(OCR_ENABLE_SIMULATOR)
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskExeBegin, trace, taskGuid);
                ocrEdt_t funcPtr = TRACE_FIELD(TASK, taskExeBegin, trace, funcPtr);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: EXECUTE | GUID: "GUIDF" | FUNC_PTR: 0x%"PRIx64"\n",
                       evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), (u64)funcPtr);

#else
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskExeBegin, trace, taskGuid);
                ocrEdt_t funcPtr = TRACE_FIELD(TASK, taskExeBegin, trace, funcPtr);
                u64 depc = TRACE_FIELD(TASK, taskExeBegin, trace, depc);
                u64 paramc = TRACE_FIELD(TASK, taskExeBegin, trace, paramc);
                u64 *paramv = TRACE_FIELD(TASK, taskExeBegin, trace, paramv);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: EXECUTE | GUID: "GUIDF" | FUNC_PTR: 0x%"PRIx64" | DEPC: %"PRId64" | PARAMC: %"PRId64" | PARAMV:",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), (u64)funcPtr, depc, paramc);

                if(paramc > 0){
                    u32 i;
                    for(i = 0; i < paramc; i++){
                        printf(" [%"PRId32"] 0x%"PRIx64"", i, paramv[i]);
                    }
                    printf("\n");
                }else{
                    printf(" | EMPTY\n");
                }
#endif
                break;
            }
            case OCR_ACTION_FINISH:
            {
#if !defined(OCR_ENABLE_SIMULATOR)
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskCreate, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
#else
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskExeEnd, trace, taskGuid);
                u32 count = TRACE_FIELD(TASK, taskExeEnd, trace, count);
                u64 hwCycles = TRACE_FIELD(TASK, taskExeEnd, trace, hwCycles);
                u64 hwCacheRefs = TRACE_FIELD(TASK, taskExeEnd, trace, hwCacheRefs);
                u64 hwCacheMisses = TRACE_FIELD(TASK, taskExeEnd, trace, hwCacheMisses);
                u64 hwFpOps = TRACE_FIELD(TASK, taskExeEnd, trace, hwFpOps);
                u64 swEdtCreates = TRACE_FIELD(TASK, taskExeEnd, trace, swEdtCreates);
                u64 swDbTotal = TRACE_FIELD(TASK, taskExeEnd, trace, swDbTotal);
                u64 swDbCreates = TRACE_FIELD(TASK, taskExeEnd, trace, swDbCreates);
                u64 swDbDestroys = TRACE_FIELD(TASK, taskExeEnd, trace, swDbDestroys);
                u64 swEvtSats = TRACE_FIELD(TASK, taskExeEnd, trace, swEvtSats);
                void *edt = TRACE_FIELD(TASK, taskExeEnd, trace, edt);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: FINISH | GUID: "GUIDF" | FP: %p | COUNT: %"PRId64" | CYCLES: %"PRId64" | CACHE_REFS: %"PRId64" | CACHE_MISSES: %"PRId64" | FP_OPS: %"PRId64" | EDT_CREATES: %"PRId64" | DB_TOTAL: %"PRId64" | DB_CREATES: %"PRId64" | DB_DESTROYS: %"PRId64" | EVT_SATS: %"PRId64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), edt, count, hwCycles, hwCacheRefs, hwCacheMisses, hwFpOps,
                        swEdtCreates, swDbTotal, swDbCreates, swDbDestroys, swEvtSats);
#endif
                break;
            }
            case OCR_ACTION_DATA_ACQUIRE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDataAcquire, trace, taskGuid);
                ocrGuid_t dbGuid = TRACE_FIELD(TASK, taskDataAcquire, trace, dbGuid);
                u64 dbSize = TRACE_FIELD(TASK, taskDataAcquire, trace, dbSize);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64"  | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: DB_ACQUIRE | GUID: "GUIDF" | DB_GUID: "GUIDF" | DB_SIZE: %"PRIu64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), GUIDA(dbGuid), dbSize);

                break;
            }

            case OCR_ACTION_DATA_RELEASE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDataRelease, trace, taskGuid);
                ocrGuid_t dbGuid = TRACE_FIELD(TASK, taskDataRelease, trace, dbGuid);
                u64 dbSize = TRACE_FIELD(TASK, taskDataRelease, trace, dbSize);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: DB_RELEASE | GUID: "GUIDF" | DB_GUID: "GUIDF" | DB_SIZE: %"PRIu64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(taskGuid), GUIDA(dbGuid), dbSize);

                break;
            }

            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_EVENT:

        switch(trace->actionSwitch){

            case OCR_ACTION_CREATE:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(EVENT, eventCreate, trace, eventGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(EVENT, eventDestroy, trace, eventGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(EVENT, eventDepSatisfy, trace, eventGuid);
                ocrGuid_t satisfyee = TRACE_FIELD(EVENT, eventDepSatisfy, trace, satisfyee);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EVENT | ACTION: DEP_SATISFY | GUID: "GUIDF" | SATISFYEE_GUID "GUIDF"\n",
                       evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(eventGuid), GUIDA(satisfyee));
                break;
            }

            case OCR_ACTION_ADD_DEP:
            {
                ocrGuid_t src = TRACE_FIELD(EVENT, eventDepAdd, trace, src);
                ocrGuid_t dest = TRACE_FIELD(EVENT, eventDepAdd, trace, dest);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: EVENT | ACTION: ADD_DEP | SRC: "GUIDF" | DEST: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(src), GUIDA(dest));
                break;
            }

            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_MESSAGE:

        switch(trace->actionSwitch){

            case OCR_ACTION_END_TO_END:
            {
                ocrLocation_t src = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, src);
                ocrLocation_t dst = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, dst);
                u64 usefulSize = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, usefulSize);
                u64 marshTime = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, marshTime);
                u64 sendTime = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, sendTime);
                u64 rcvTime = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, rcvTime);
                u64 unMarshTime = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, unMarshTime);
                u64 type = TRACE_FIELD(MESSAGE, msgEndToEnd, trace, type);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: MESSAGE | ACTION: END_TO_END | SRC: 0x%"PRIu32" | DEST: 0x%"PRIu32" | SIZE: %"PRIu64" | MARSH: %"PRIu64" | SEND: %"PRIu64" | RCV: %"PRIu64" | UMARSH: %"PRIu64" | TYPE: 0x%"PRIx64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, src, dst, usefulSize, marshTime, sendTime, rcvTime, unMarshTime, type);

                break;
            }

            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_DATABLOCK:

        switch(trace->actionSwitch){

            case OCR_ACTION_CREATE:
            {
                ocrGuid_t dbGuid = TRACE_FIELD(DATA, dataCreate, trace, dbGuid);
                u64 dbSize = TRACE_FIELD(DATA, dataCreate, trace, dbSize);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: DATABLOCK | ACTION: CREATE | GUID: "GUIDF" | SIZE: %"PRIu64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(dbGuid), dbSize);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t dbGuid = TRACE_FIELD(DATA, dataDestroy, trace, dbGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_WORKER:

        switch(trace->actionSwitch){

            case OCR_ACTION_WORK_REQUEST:
            {
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: WORKER | ACTION: REQUEST\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp);
                break;
            }

            case OCR_ACTION_WORK_TAKEN:
            {
                ocrGuid_t curTask = TRACE_FIELD(EXECUTION_UNIT, exeWorkTaken, trace, foundGuid);
                deque_t *deq = TRACE_FIELD(EXECUTION_UNIT, exeWorkTaken, trace, deq);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: WORKER | ACTION: TAKEN | GUID: "GUIDF" | DEQUE: %p\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(curTask), deq);
                break;
            }

            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_SCHEDULER:

        switch(trace->actionSwitch){

            case OCR_ACTION_SCHED_MSG_SEND:
            {
                ocrGuid_t curTask = TRACE_FIELD(SCHEDULER, schedMsgSend, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: SCHEDULER | ACTION: MSG_SEND | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(curTask));
                break;
            }

            case OCR_ACTION_SCHED_MSG_RCV:
            {
                ocrGuid_t curTask = TRACE_FIELD(SCHEDULER, schedMsgRcv, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: SCHEDULER | ACTION: MSG_RECEIVE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(curTask));
                break;
            }

            case OCR_ACTION_SCHED_INVOKE:
            {
                ocrGuid_t curTask = TRACE_FIELD(SCHEDULER, schedInvoke, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: SCHEDULER | ACTION: INVOKE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(curTask));
                break;
            }

            default:
                break;

        }
        break;

    case OCR_TRACE_TYPE_ALLOCATOR:

        switch(trace->actionSwitch){

            case OCR_ACTION_ALLOCATE:
            {
                u64 startTime = TRACE_FIELD(ALLOCATOR, memAlloc, trace, startTime);
                u64 callFunc = TRACE_FIELD(ALLOCATOR, memAlloc, trace, callFunc);
                u64 memSize = TRACE_FIELD(ALLOCATOR, memAlloc, trace, memSize);
                u64 memHint = TRACE_FIELD(ALLOCATOR, memAlloc, trace, memHint);
                void *memPtr = TRACE_FIELD(ALLOCATOR, memAlloc, trace, memPtr);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TASK: "GUIDF" | TIMESTAMP: %"PRIu64" | STARTTIME: %"PRIu64" | TYPE: ALLOCATOR | ACTION: ALLOCTATE | FUNC:  %"PRIu64" | MEMSIZE: %"PRIu64" | MEMHINT: %"PRIx64" | MEMPTR: %p\n",
                        evt_type[evtType], location, workerId, NULL_GUID, timestamp, startTime, callFunc, memSize, memHint, memPtr);
                break;
            }

            case OCR_ACTION_DEALLOCATE:
            {
                u64 startTime = TRACE_FIELD(ALLOCATOR, memDealloc, trace, startTime);
                u64 callFunc = TRACE_FIELD(ALLOCATOR, memAlloc, trace, callFunc);
                void *memPtr = TRACE_FIELD(ALLOCATOR, memDealloc, trace, memPtr);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TASK: "GUIDF" | TIMESTAMP: %"PRIu64" | STARTTIME: %"PRIu64" | TYPE: ALLOCATOR | ACTION: DEALLOCATE | FUNC:  %"PRIu64" | MEMPTR: %p\n",
                        evt_type[evtType], location, workerId, NULL_GUID, timestamp, startTime, callFunc, memPtr);
                break;
            }

            default:
                break;

        }
        break;

#ifdef OCR_ENABLE_SIMULATOR
    case OCR_TRACE_TYPE_API_EDT:

        switch(trace->actionSwitch){

            case OCR_ACTION_CREATE:
            {

                ocrGuid_t templateGuid = TRACE_FIELD(API_EDT, simEdtCreate, trace, templateGuid);
                u32 paramc = TRACE_FIELD(API_EDT, simEdtCreate, trace, paramc);
                u32 depc = TRACE_FIELD(API_EDT, simEdtCreate, trace, depc);

                u64 *paramv = TRACE_FIELD(API_EDT, simEdtCreate, trace, paramv);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_EDT | ACTION: CREATE | TEMPLATE_GUID: "GUIDF" | DEPC: %"PRId32" | PARAMC: %"PRId32" | PARAMV:",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(templateGuid), depc, paramc);
                u32 i;
                if(paramc > 0){
                   for(i = 0; i < paramc; i++){
                        printf(" [%"PRId32"] 0x%"PRIx64"", i, paramv[i]);
                    }
                    printf("\n");
                }else{
                    printf(" | EMPTY\n");
                }

                break;
            }
            case OCR_ACTION_TEMPLATE_CREATE:
            {

                ocrEdt_t funcPtr = TRACE_FIELD(API_EDT, simEdtTemplateCreate, trace, funcPtr);
                u32 paramc = TRACE_FIELD(API_EDT, simEdtTemplateCreate, trace, paramc);
                u32 depc = TRACE_FIELD(API_EDT, simEdtTemplateCreate, trace, depc);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_EDT | ACTION: TEMPLATE_CREATE | FUNC_PTR: 0x%"PRIx64" | DEPC: %"PRId32" | PARAMC: %"PRId32"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, funcPtr, depc, paramc);
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_API_EVENT:

        switch(trace->actionSwitch){

            case OCR_ACTION_CREATE:
            {
                ocrEventTypes_t eventType = TRACE_FIELD(API_EVENT, simEventCreate, trace, eventType);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_EVENT | ACTION: CREATE | EVT_TYPE: %"PRId32"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, eventType);
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(API_EVENT, simEventSatisfy, trace, eventGuid) = eventGuid;
                ocrGuid_t dataGuid = TRACE_FIELD(API_EVENT, simEventSatisfy, trace, dataGuid) = dataGuid;

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_EVENT | ACTION: SATISFY | EVT_GUID: "GUIDF" | DATA_GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(eventGuid), GUIDA(dataGuid));
                break;
            }
            case OCR_ACTION_ADD_DEP:
            {

                ocrGuid_t src = TRACE_FIELD(API_EVENT, simEventAddDep, trace, source);
                ocrGuid_t dest = TRACE_FIELD(API_EVENT, simEventAddDep, trace, destination);
                u32 slot = TRACE_FIELD(API_EVENT, simEventAddDep, trace, slot);
                ocrDbAccessMode_t accessMode = TRACE_FIELD(API_EVENT, simEventAddDep, trace, accessMode);

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_EVENT | ACTION: ADD_DEP | SRC: "GUIDF" | DEST: "GUIDF" | SLOT: %"PRId32" | ACCESS_MODE: 0x%"PRIx64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(src), GUIDA(dest), slot, accessMode);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(API_EVENT, simEventDestroy, trace, eventGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_EVENT | ACTION: DESTROY | EVT_GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(eventGuid));
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_API_DATABLOCK:

        switch(trace->actionSwitch){

            case OCR_ACTION_CREATE:
            {
                u64 size = TRACE_FIELD(API_DATABLOCK, simDbCreate, trace, len);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_DATABLOCK | ACTION: CREATE | SIZE: %"PRId64"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, size);
                break;
            }
            case OCR_ACTION_DATA_RELEASE:
            {
                ocrGuid_t dbGuid = TRACE_FIELD(API_DATABLOCK, simDbRelease, trace, guid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_DATABLOCK | ACTION: DATA_RELEASE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(dbGuid));
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t dbGuid = TRACE_FIELD(API_DATABLOCK, simDbDestroy, trace, guid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | EDT: "GUIDF" | TIMESTAMP: %"PRIu64" | TYPE: API_DATABLOCK | ACTION: DESTROY | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, GUIDA(parent), timestamp, GUIDA(dbGuid));
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_API_AFFINITY:

        switch(trace->actionSwitch){
            case OCR_ACTION_GET_CURRENT:
            {
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_GET_AT:
            {
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_GET_COUNT:
            {
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_QUERY:
            {
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            default:
                break;
        }
        break;

    case OCR_TRACE_TYPE_API_HINT:

        switch(trace->actionSwitch){
            case OCR_ACTION_INIT:
            {
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }
            case OCR_ACTION_SET_VAL:
            {
                genericPrint(evtType, ttype, action, location, workerId, timestamp, parent);
                break;
            }

            default:
                break;
        }
        break;

#endif
    default:
        break;
    }
}

