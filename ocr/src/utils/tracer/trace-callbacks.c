/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "utils/tracer/trace-callbacks.h"
#include "utils/tracer/tracer.h"

#ifdef ENABLE_WORKER_SYSTEM

#include "ocr-policy-domain.h"
#include "worker/hc/hc-worker.h"

void __attribute__ ((weak)) traceTaskCreate(u64 location, bool evtType, ocrTraceType_t objType,
                                            ocrTraceAction_t actionType, u64 workerId,
                                            u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                            ocrEdt_t fctPtr){

    //TRACING CALLBACKS - Task Create
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskCreate, tr, taskGuid) = edtGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskRunnable(u64 location, bool evtType, ocrTraceType_t objType,
                                              ocrTraceAction_t actionType, u64 workerId,
                                              u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                              ocrEdt_t fctPtr){

    //TRACING CALLBACKS - Task Runnable
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskReadyToRun, tr, taskGuid) = edtGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskAddDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                                   ocrTraceAction_t actionType, u64 workerId,
                                                   u64 timestamp, ocrGuid_t parent, ocrGuid_t src, ocrGuid_t dest){

    //TRACING CALLBACKS - Task Add Dependence
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskDepReady, tr, src) = src;
    TRACE_FIELD(TASK, taskDepReady, tr, dest) = dest;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskSatisfyDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                                       ocrTraceAction_t actionType, u64 workerId,
                                                       u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                                       ocrGuid_t satisfyee){

    //TRACING CALLBACKS - Task Satisfy Dependence
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskDepSatisfy, tr, taskGuid) = edtGuid;
    TRACE_FIELD(TASK, taskDepSatisfy, tr, satisfyee) = satisfyee;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskExecute(u64 location, bool evtType, ocrTraceType_t objType,
                                            ocrTraceAction_t actionType, u64 workerId,
                                            u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                            ocrEdt_t funcPtr){

    //TRACING CALLBACKS - Task Execute
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskExeBegin, tr, taskGuid) = edtGuid;
    TRACE_FIELD(TASK, taskExeBegin, tr, funcPtr) = funcPtr;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskFinish(u64 location, bool evtType, ocrTraceType_t objType,
                                            ocrTraceAction_t actionType, u64 workerId,
                                            u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid){

    //TRACING CALLBACKS - Task Finish
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskExeEnd, tr, taskGuid) = edtGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskShift(u64 location, bool evtType, ocrTraceType_t objType,
                                            ocrTraceAction_t actionType, u64 workerId,
                                            u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                            ocrEdt_t funcPtr, bool shiftFrom){

    //TRACING CALLBACKS - Task Shift
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskExeShift, tr, taskGuid) = edtGuid;
    TRACE_FIELD(TASK, taskExeShift, tr, funcPtr) = funcPtr;
    TRACE_FIELD(TASK, taskExeShift, tr, shiftFrom) = shiftFrom;
    PUSH_TO_TRACE_DEQUE();
    return;
}

u8 __attribute__ ((weak)) getTaskPlacement(ocrGuid_t edtGuid, u32 * location) {
    return 1;
}

void __attribute__ ((weak)) traceTaskDataAcquire(u64 location, bool evtType, ocrTraceType_t objType,
                                                 ocrTraceAction_t actionType, u64 workerId,
                                                 u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                                 ocrGuid_t dbGuid, u64 dbSize){

    //TRACING CALLBACKS - Task Data Acquire
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskDataAcquire, tr, taskGuid) = edtGuid;
    TRACE_FIELD(TASK, taskDataAcquire, tr, dbGuid) = dbGuid;
    TRACE_FIELD(TASK, taskDataAcquire, tr, dbSize) = dbSize;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskDataRelease(u64 location, bool evtType, ocrTraceType_t objType,
                                                 ocrTraceAction_t actionType, u64 workerId,
                                                 u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                                 ocrGuid_t dbGuid, u64 dbSize){

    //TRACING CALLBACKS - Task Data Release
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskDataRelease, tr, taskGuid) = edtGuid;
    TRACE_FIELD(TASK, taskDataRelease, tr, dbGuid) = dbGuid;
    TRACE_FIELD(TASK, taskDataRelease, tr, dbSize) = dbSize;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTaskDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                                             ocrTraceAction_t actionType, u64 workerId,
                                             u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                             ocrEdt_t fctPtr){

    //TRACING CALLBACKS - Task Destroy
    INIT_TRACE_OBJECT();
    TRACE_FIELD(TASK, taskDestroy, tr, taskGuid) = edtGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceEventCreate(u64 location, bool evtType, ocrTraceType_t objType,
                                             ocrTraceAction_t actionType, u64 workerId,
                                             u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid){

    //TRACING CALLBACKS - Event Create
    INIT_TRACE_OBJECT();
    TRACE_FIELD(EVENT, eventCreate, tr, eventGuid) = eventGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceEventDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                                              ocrTraceAction_t actionType, u64 workerId,
                                              u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid){

    //TRACING CALLBACKS - Event Destroy
    INIT_TRACE_OBJECT();
    TRACE_FIELD(EVENT, eventDestroy, tr, eventGuid) = eventGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceEventSatisfyDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                                        ocrTraceAction_t actionType, u64 workerId,
                                                        u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid,
                                                        ocrGuid_t satisfyee){

    //TRACING CALLBACKS - Event Satisfy
    INIT_TRACE_OBJECT();
    TRACE_FIELD(EVENT, eventDepSatisfy, tr, eventGuid) = eventGuid;
    TRACE_FIELD(EVENT, eventDepSatisfy, tr, satisfyee) = satisfyee;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceEventAddDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                                    ocrTraceAction_t actionType, u64 workerId,
                                                    u64 timestamp, ocrGuid_t parent, ocrGuid_t src,
                                                    ocrGuid_t dest){

    //TRACING CALLBACKS - Event Add Dependence
    INIT_TRACE_OBJECT();
    TRACE_FIELD(EVENT, eventDepAdd, tr, src) = src;
    TRACE_FIELD(EVENT, eventDepAdd, tr, dest) = dest;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceDataCreate(u64 location, bool evtType, ocrTraceType_t objType,
                                            ocrTraceAction_t actionType, u64 workerId,
                                            u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid, u64 dbSize){

    //TRACING CALLBACKS - Data Create
    INIT_TRACE_OBJECT();
    TRACE_FIELD(DATA, dataCreate, tr, dbGuid) = dbGuid;
    TRACE_FIELD(DATA, dataCreate, tr, dbSize) = dbSize;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceDataDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                                             ocrTraceAction_t actionType, u64 workerId,
                                             u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid){

    //TRACING CALLBACKS - Data Destroy
    INIT_TRACE_OBJECT();
    TRACE_FIELD(DATA, dataDestroy, tr, dbGuid) = dbGuid;
    PUSH_TO_TRACE_DEQUE();
    return;
}

void __attribute__ ((weak)) traceTemplateCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t templGuid,
                     ocrEdt_t fctPtr) {
    return;
}

void __attribute__ ((weak)) traceAPITemplateCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrEdt_t fctPtr,
                     u32 paramc, u32 depc) {
    return;
}

void __attribute__ ((weak)) traceAPITaskCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                     ocrGuid_t templGuid, u32 paramc, u64 * paramv,
                     u32 depc, ocrGuid_t * depv) {
    return;
}

void __attribute__ ((weak)) traceAPIAddDependence(u64 location, bool evtType, ocrTraceType_t objType,
                            ocrTraceAction_t actionType, u64 workerId,
                            u64 timestamp, ocrGuid_t parent, ocrGuid_t src,
                            ocrGuid_t dest, u32 slot, ocrDbAccessMode_t mode) {
    return;
}

void __attribute__ ((weak)) traceAPIEventSatisfyDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                 ocrTraceAction_t actionType, u64 workerId,
                                 u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid,
                                 ocrGuid_t satisfyee, u32 slot) {
    return;
}

void __attribute__ ((weak)) traceAPIEventCreate(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrEventTypes_t eventType) {
    return;
}

void __attribute__ ((weak)) traceAPIDataCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid,
                     u64 dbSize) {
    return;
}

void __attribute__ ((weak)) traceAPIDataDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid) {
    return;
}

void __attribute__ ((weak)) traceAPIDataRelease(u64 location, bool evtType, ocrTraceType_t objType,
                          ocrTraceAction_t actionType, u64 workerId,
                          u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid) {
    return;
}

#endif /* ENABLE_WORKER_SYSTEM */
