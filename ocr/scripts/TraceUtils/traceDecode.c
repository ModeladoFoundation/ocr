
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocr.h"
#include "utils/tracer/tracer.h"
#include "utils/tracer/trace-events.h"

#define IDX_OFFSET OCR_TRACE_TYPE_EDT
#define MAX_FCT_PTR_LENGTH 16
#define SYS_CALL_COMMAND_LENGTH 256
#define BIN_PATH_LENGTH 128

void translateObject(ocrTraceObj_t *trace, int lineCount, long *fctPtrs);
int getLineCount(FILE *fname);
void readFctPtrsFromBinary(char *binaryPath, int lineCount, long *fctPtrs);
bool fctPtrExists(long lookup, int lineCount, long *fctPtrs);

int main(int argc, char *argv[]){

    if(argc < 2){
        printf("Error Usage: TODO\n");
        printf("\n-------- Incorrect Input ---------\n");
        printf("Usage: %s <filename>  optional : <application binary>\n\n", argv[0]);
        return 1;
    }

    char *fname = argv[1];

    /*TODO Keep this around for now.  Will go away once processRequestEdts go away
    char *binaryPath;
    //If binary path provided, store
    if(argc == 3){
        char *binname = argv[2];
        binaryPath = malloc(strlen(binname)+1);
        strcpy(binaryPath, binname);
    }else{
        binaryPath = "";
    }
    */

    ocrTraceObj_t *trace = malloc(sizeof(ocrTraceObj_t));

    int lineCount = 0;

    /*TODO Keep this around for now.  Will go away once processRequestEdts go away
    if(binaryPath != ""){
        char *sysCommand = malloc(SYS_CALL_COMMAND_LENGTH);
        char path[BIN_PATH_LENGTH];
        strcpy(sysCommand, "nm ");
        strcat(sysCommand, binaryPath);

        //Call nm on optionally provided application binary.
        FILE *fp = popen(sysCommand, "r");
        free(sysCommand);
        if(fp == NULL){
            printf("Error: Unable to open application binary, reverting to default options\n");
        }else{
            lineCount = getLineCount(fp);
            pclose(fp);
        }
    }


    long *fctPtrs = NULL;
    //This indicates that an application binary was provided, and we will differentatie processRequestEdts
    if(lineCount > 0){
        fctPtrs = (long *)malloc(lineCount*(sizeof(long)));
        readFctPtrsFromBinary(binaryPath, lineCount, fctPtrs);
    }
    */

    long *fctPtrs = NULL;

    //Read each trace record, and decode
    int i;
    for(i=1; i < argc; i++){
        FILE *f = fopen(argv[i], "r");
        if(f == NULL){
            printf("Error:  Unable to open provided trace binary\n");
            return 1;
        }

        while(fread(trace, sizeof(ocrTraceObj_t), 1, f)){
            translateObject(trace, lineCount, fctPtrs);
        }
        fclose(f);
    }
    return 0;
}

void genericPrint(bool evtType, ocrTraceType_t ttype, ocrTraceAction_t action,
                  u64 location, u64 workerId, u64 timestamp, ocrGuid_t self, ocrGuid_t parent){

    if(!(ocrGuidIsNull(parent))){
        printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: %s | ACTION: %s | GUID: "GUIDF" | PARENT: "GUIDF"\n",
                evt_type[evtType], location, workerId, timestamp, obj_type[ttype-IDX_OFFSET], action_type[action], GUIDA(self), GUIDA(parent));
    }else{

        printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: %s | ACTION: %s | GUID: "GUIDF"\n",
                evt_type[evtType], location, workerId, timestamp, obj_type[ttype-IDX_OFFSET], action_type[action], GUIDA(self));
    }
    return;
}

void translateObject(ocrTraceObj_t *trace, int lineCount, long *fctPtrs){

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
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskCreate, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, taskGuid, parent);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDestroy, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, taskGuid, NULL_GUID);
                break;
            }
            case OCR_ACTION_RUNNABLE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskReadyToRun, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, taskGuid, NULL_GUID);
                break;
            }
            case OCR_ACTION_SCHEDULED:
            {
                ocrGuid_t curTask = TRACE_FIELD(TASK, taskScheduled, trace, taskGuid);
                deque_t *deq = TRACE_FIELD(TASK, taskScheduled, trace, deq);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: SCHEDULED | GUID: "GUIDF" | DEQUE: %p\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(curTask), deq);
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDepSatisfy, trace, taskGuid);
                ocrGuid_t satisfyee = TRACE_FIELD(TASK, taskDepSatisfy, trace, satisfyee);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: DEP_SATISFY | GUID: "GUIDF" | SATISFYEE_GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(taskGuid), GUIDA(satisfyee));
                break;
            }
            case OCR_ACTION_ADD_DEP:
            {
                ocrGuid_t src = TRACE_FIELD(TASK, taskDepReady, trace, src);
                ocrGuid_t dest = TRACE_FIELD(TASK, taskDepReady, trace, dest);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: ADD_DEP | SRC: "GUIDF" | DEST: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(src), GUIDA(dest));
                break;
            }
            case OCR_ACTION_EXECUTE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskExeBegin, trace, taskGuid);
                ocrEdt_t funcPtr = TRACE_FIELD(TASK, taskExeBegin, trace, funcPtr);

                if(fctPtrs != NULL){

                    if(!(fctPtrExists((long)funcPtr, lineCount, fctPtrs))){
                        printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: EXECUTE | GUID "GUIDF" | FUNC_PTR: <processRequestEdt>\n",
                                evt_type[0], location, workerId, timestamp, GUIDA(taskGuid));
                        break;
                    }
                }

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: EXECUTE | GUID: "GUIDF" | FUNC_PTR: 0x%"PRIx64"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(taskGuid), (u64)funcPtr);
                break;
            }
            case OCR_ACTION_FINISH:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskExeEnd, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, taskGuid, NULL_GUID);
                break;
            }
            case OCR_ACTION_SHIFT:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskExeShift, trace, taskGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, taskGuid, NULL_GUID);
                break;
            }
            case OCR_ACTION_DATA_ACQUIRE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDataAcquire, trace, taskGuid);
                ocrGuid_t dbGuid = TRACE_FIELD(TASK, taskDataAcquire, trace, dbGuid);
                u64 dbSize = TRACE_FIELD(TASK, taskDataAcquire, trace, dbSize);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: DB_ACQUIRE | GUID: "GUIDF" | DB_GUID: "GUIDF" | DB_SIZE: %"PRIu64"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(taskGuid), GUIDA(dbGuid), dbSize);

                break;
            }

            case OCR_ACTION_DATA_RELEASE:
            {
                ocrGuid_t taskGuid = TRACE_FIELD(TASK, taskDataRelease, trace, taskGuid);
                ocrGuid_t dbGuid = TRACE_FIELD(TASK, taskDataRelease, trace, dbGuid);
                u64 dbSize = TRACE_FIELD(TASK, taskDataRelease, trace, dbSize);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EDT | ACTION: DB_RELEASE | GUID: "GUIDF" | DB_GUID: "GUIDF" | DB_SIZE: %"PRIu64"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(taskGuid), GUIDA(dbGuid), dbSize);

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
                genericPrint(evtType, ttype, action, location, workerId, timestamp, eventGuid, parent);
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(EVENT, eventDestroy, trace, eventGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, eventGuid, NULL_GUID);
                break;
            }
            case OCR_ACTION_SATISFY:
            {
                ocrGuid_t eventGuid = TRACE_FIELD(EVENT, eventDepSatisfy, trace, eventGuid);
                ocrGuid_t satisfyee = TRACE_FIELD(EVENT, eventDepSatisfy, trace, satisfyee);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EVENT | ACTION: DEP_SATISFY | GUID: "GUIDF" | SATISFYEE_GUID "GUIDF"\n",
                       evt_type[evtType], location, workerId, timestamp, GUIDA(eventGuid), GUIDA(satisfyee));
                break;
            }

            case OCR_ACTION_ADD_DEP:
            {
                ocrGuid_t src = TRACE_FIELD(EVENT, eventDepAdd, trace, src);
                ocrGuid_t dest = TRACE_FIELD(EVENT, eventDepAdd, trace, dest);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: EVENT | ACTION: ADD_DEP | SRC: "GUIDF" | DEST: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(src), GUIDA(dest));
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

                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: MESSAGE | ACTION: END_TO_END | SRC: 0x%u | DEST: 0x%u | SIZE: %"PRIu64" | MARSH: %"PRIu64" | SEND: %"PRIu64" | RCV: %"PRIu64" | UMARSH: %"PRIu64" | TYPE: 0x%"PRIx64"\n",
                        evt_type[evtType], location, workerId, timestamp, src, dst, usefulSize, marshTime, sendTime, rcvTime, unMarshTime, type);


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
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: DATABLOCK | ACTION: CREATE | GUID: "GUIDF" | SIZE: %"PRIu64" | PARENT: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(dbGuid), dbSize, GUIDA(parent));
                break;
            }
            case OCR_ACTION_DESTROY:
            {
                ocrGuid_t dbGuid = TRACE_FIELD(DATA, dataDestroy, trace, dbGuid);
                genericPrint(evtType, ttype, action, location, workerId, timestamp, dbGuid, NULL_GUID);
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
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: WORKER | ACTION: REQUEST\n",
                        evt_type[evtType], location, workerId, timestamp);
                break;
            }

            case OCR_ACTION_WORK_TAKEN:
            {
                ocrGuid_t curTask = TRACE_FIELD(EXECUTION_UNIT, exeWorkTaken, trace, foundGuid);
                deque_t *deq = TRACE_FIELD(EXECUTION_UNIT, exeWorkTaken, trace, deq);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: WORKER | ACTION: TAKEN | GUID: "GUIDF" | DEQUE: %p\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(curTask), deq);
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
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: SCHEDULER | ACTION: MSG_SEND | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(curTask));
                break;
            }

            case OCR_ACTION_SCHED_MSG_RCV:
            {
                ocrGuid_t curTask = TRACE_FIELD(SCHEDULER, schedMsgRcv, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: SCHEDULER | ACTION: MSG_RECEIVE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(curTask));
                break;
            }

            case OCR_ACTION_SCHED_INVOKE:
            {
                ocrGuid_t curTask = TRACE_FIELD(SCHEDULER, schedInvoke, trace, taskGuid);
                printf("[TRACE] U/R: %s | PD: 0x%"PRIx64" | WORKER_ID: %"PRIu64" | TIMESTAMP: %"PRIu64" | TYPE: SCHEDULER | ACTION: INVOKE | GUID: "GUIDF"\n",
                        evt_type[evtType], location, workerId, timestamp, GUIDA(curTask));
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

void readFctPtrsFromBinary(char *binaryPath, int lineCount, long *fctPtrs){
    char *sysCommand = malloc(SYS_CALL_COMMAND_LENGTH);
    char path[BIN_PATH_LENGTH];
    strcpy(sysCommand, "nm -nr ");
    strcat(sysCommand, binaryPath);

    FILE *fp = popen(sysCommand, "r");
    free(sysCommand);
    if(fp == NULL){
        //Should not ever happen, as this check is already made in callee.
        printf("Error:  Unable to open application binary\n");
        exit(0);
    }

    int i;
    //Scan over binary file and store function pointer values in an array
    //for faster lookup later
    for(i = 0; i < lineCount; i++){
        long fctPtr;
        if(feof(fp))
            return;
        fscanf(fp, "%lx%*[^\n]", &fctPtr);
        fctPtrs[i] = fctPtr;
    }
}

bool fctPtrExists(long lookup, int lineCount, long *fctPtrs){
    int i;
    //Loop over function pointer array to see if lookup value exists
    for(i = 0; i < lineCount; i++){
        if(fctPtrs[i] == lookup){
            return true;
        }
    }

    return false;

}

int getLineCount(FILE* f){
    if(f == NULL)
        return 0;

    int c = 0;
    int count = 0;
    //Iterate through binary file to get line count for later malloc
    while(!feof(f)){
        c = fgetc(f);
        if(c == '\n'){
            count++;
        }
    }
    return count;
}


