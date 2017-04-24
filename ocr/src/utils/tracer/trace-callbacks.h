/**
 * @brief OCR interface to callback functions for OCR tracing framework
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __TRACE_CALLBACKS_H__
#define __TRACE_CALLBACKS_H__

#include "ocr-config.h"
#ifdef ENABLE_WORKER_SYSTEM

#include "ocr-runtime-types.h"
#include "ocr-types.h"


/**
 * @brief Callback function for tracing task creations
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT bieng created
 */



void traceTaskCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid);


/**
 * @brief Callback function for tracing task destructions
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT bieng destroyed
 */

void traceTaskDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid);


/**
 * @brief Callback function for tracing when tasks become runnable
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT that became runnable
 */

void traceTaskRunnable(u64 location, bool evtType, ocrTraceType_t objType,
                       ocrTraceAction_t actionType, u64 workerId,
                       u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid);


/**
 * @brief Callback function for tracing when dependences are added to tasks
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param src           Source GUID of the dependence bieng added
 * @param dest          Destination GUID of the dependence being added
 */

void traceTaskAddDependence(u64 location, bool evtType, ocrTraceType_t objType,
                            ocrTraceAction_t actionType, u64 workerId,
                            u64 timestamp, ocrGuid_t parent, ocrGuid_t src,
                            ocrGuid_t dest);


/**
 * @brief Callback function for tracing task dependence satisfacations
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT being satisfied
 * @param satisfyee     GUID of OCR object satisfying the EDTs dependence
 */

void traceTaskSatisfyDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                ocrTraceAction_t actionType, u64 workerId,
                                u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                                ocrGuid_t satisfyee);


/**
 * @brief Callback function for tracing when tasks begin execution
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT beginning execution
 * @param fctPtr        Function pointer of code associated with the EDT execution
 */

void traceTaskExecute(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                      ocrEdt_t fctPtr);


/**
 * @brief Callback function for tracing when tasks complete execution
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT finishing execution
 */

void traceTaskFinish(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid);


/**
 * @brief Callback function for tracing when tasks acquire data
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT acquiring a datablock
 * @param dbGuid        GUID of datablock being acquired
 * @param dbSize        Size of datablock being acquired
 */

void traceTaskDataAcquire(u64 location, bool evtType, ocrTraceType_t objType,
                          ocrTraceAction_t actionType, u64 workerId,
                          u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                          ocrGuid_t dbGuid, u64 dbSize);


/**
 * @brief Callback function for tracing when tasks release data
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT releasing a datablock
 * @param dbGuid        GUID of datablock being released
 * @param dbSize        Size of datablock being released
 */

void traceTaskDataRelease(u64 location, bool evtType, ocrTraceType_t objType,
                          ocrTraceAction_t actionType, u64 workerId,
                          u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                          ocrGuid_t dbGuid, u64 dbSize);


/**
 * @brief Callback function for tracing event creations
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param eventGuid     GUID of OCR event being created
 */

void traceEventCreate(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid);


/**
 * @brief Callback function for tracing event destructions
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param eventGuid     GUID of OCR event being destroyed
 */

void traceEventDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                       ocrTraceAction_t actionType, u64 workerId,
                       u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid);


/**
 * @brief Callback function for tracing event dependence satisfactions
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param eventGuid     GUID of OCR event bieng satisfied
 * @param satisfyee     GUID of OCR object responisble for satisfying the event's dependence
 */

void traceEventSatisfyDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                 ocrTraceAction_t actionType, u64 workerId,
                                 u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid,
                                 ocrGuid_t satisfyee);


/**
 * @brief Callback function for tracing when dependences are added to events
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param src           Source GUID of the dependence bieng added
 * @param dest          Destination GUID of the dependence being added
 */

void traceEventAddDependence(u64 location, bool evtType, ocrTraceType_t objType,
                             ocrTraceAction_t actionType, u64 workerId,
                             u64 timestamp, ocrGuid_t parent, ocrGuid_t src,
                             ocrGuid_t dest);


/**
 * @brief Callback function for tracing datablock creations
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param dbGuid        GUID of datablock being created
 * @param dbSize        Size of datablock being created
 */

void traceDataCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid,
                     u64 dbSize);


/**
 * @brief Callback function for tracing datablock destructions
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param dbGuid        GUID of datablock bieng destroyed
 */

void traceDataDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid);

/**
 * @brief Callback function for tracing beginning of allocator request
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param startTime     Time when allocation started
 * @param callFunc      Identifier of function that called allocate
 * @param memSize       Size of memory request
 * @param memHint       Hint for memory request
 * @param memPtr        Pointer to memory allocated
 */

void traceAlloc(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent,
                      u64 startTime, u64 callFunc, u64 memSize, u64 memHint, void *memPtr);

/**
 * @brief Callback function for tracing alloc end and dealloc begin
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param startTime     Time when allocation started
 * @param callFunc      Identifier of function that called allocate
 * @param memPtr        Pointer to memory allocated
 */

void traceDealloc(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent,
                      u64 startTime, u64 callFunc, void *memPtr);

#endif /* ENABLE_WORKER_SYSTEM */
#endif //__TRACE_CALLBACKS_H__
