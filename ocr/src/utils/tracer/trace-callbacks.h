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
 * @param fctPtr        Function pointer of code associated with the EDT
 */



void traceTaskCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                     ocrEdt_t fctPtr);


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
 * @param edtGuid       GUID of EDT being destroyed
 * @param fctPtr        Function pointer of code associated with the EDT
 */

void traceTaskDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                      ocrEdt_t fctPtr);


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
 * @param fctPtr        Function pointer of code associated with the EDT
 */

void traceTaskRunnable(u64 location, bool evtType, ocrTraceType_t objType,
                       ocrTraceAction_t actionType, u64 workerId,
                       u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                       ocrEdt_t fctPtr);


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
 * @brief Callback function for tracing when tasks are shifted
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT being shifted
 * @param fctPtr        Function pointer of code associated with the EDT
 * @param shiftFrom     true if shifting FROM edtGuid, false if shifting
 *                      BACK TO edtGuid
 */

void traceTaskShift(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                     ocrEdt_t fctPtr, bool shiftFrom);


u8 getTaskPlacement(ocrGuid_t edtGuid, u32 * location);

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
 * @brief Callback function for tracing template creation
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param templGuid     GUID of template being created
 * @param fctPtr        Function pointer of code associated with the EDT
 */

void traceTemplateCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t templGuid,
                     ocrEdt_t fctPtr);

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
 * @brief Callback function for tracing template creation at the API-call level
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param fctPtr        Function pointer of code associated with the EDT
 * @param paramc        The number of parameters the template takes
 * @param depc          The number of dependencies the template has
 */

void traceAPITemplateCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrEdt_t fctPtr,
                     u32 paramc, u32 depc);

/**
 * @brief Callback function for tracing task creations at the API-call level
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param edtGuid       GUID of EDT being created
 * @param templGuid     GUID of the EDT's template
 * @param paramc        The number of parameters the EDT takes
 * @param paramv        The parameters to the EDT
 * @param depc          The number of dependencies the EDT has
 * @param depv          The dependencies of the EDT
 */

void traceAPITaskCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t edtGuid,
                     ocrGuid_t templGuid, u32 paramc, u64 * paramv,
                     u32 depc, ocrGuid_t * depv);

/**
 * @brief Callback function for tracing when dependences are added
 *        at the API level.
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
 * @param slot          The pre-slot on dest to which the src is connected
 * @param mode          Access mode with which the dest will access the source
 */

void traceAPIAddDependence(u64 location, bool evtType, ocrTraceType_t objType,
                            ocrTraceAction_t actionType, u64 workerId,
                            u64 timestamp, ocrGuid_t parent, ocrGuid_t src,
                            ocrGuid_t dest, u32 slot, ocrDbAccessMode_t mode);

/**
 * @brief Callback function for tracing event dependence satisfactions
 *        at the API level.
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
 * @param slot          The pre-slot on the event being satisfied by satisfyee
 */

void traceAPIEventSatisfyDependence(u64 location, bool evtType, ocrTraceType_t objType,
                                 ocrTraceAction_t actionType, u64 workerId,
                                 u64 timestamp, ocrGuid_t parent, ocrGuid_t eventGuid,
                                 ocrGuid_t satisfyee, u32 slot);

/**
 * @brief Callback function for tracing event creations at the API level.
 *
 *
 * @param location      Id of policy domain where trace occured
 * @param evtType       Type of trace (true for USER false for RUNTIME)
 * @param objType       Type of ocr object being traced
 * @param actionType    Type of action being traced
 * @param workerId      Id of OCR worker where trace occured
 * @param timestamp     Timestamp when trace occured (ns)
 * @param parent        Parent task executing when trace occured
 * @param eventType     Type of the event being created
 */

void traceAPIEventCreate(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrEventTypes_t eventType);

/**
 * @brief Callback function for tracing datablock creations at the API level.
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

void traceAPIDataCreate(u64 location, bool evtType, ocrTraceType_t objType,
                     ocrTraceAction_t actionType, u64 workerId,
                     u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid,
                     u64 dbSize);

/**
 * @brief Callback function for tracing datablock destructions at the API level.
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

void traceAPIDataDestroy(u64 location, bool evtType, ocrTraceType_t objType,
                      ocrTraceAction_t actionType, u64 workerId,
                      u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid);

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
 * @param dbGuid        GUID of datablock being released
 */

void traceAPIDataRelease(u64 location, bool evtType, ocrTraceType_t objType,
                          ocrTraceAction_t actionType, u64 workerId,
                          u64 timestamp, ocrGuid_t parent, ocrGuid_t dbGuid);

#endif /* ENABLE_WORKER_SYSTEM */
#endif //__TRACE_CALLBACKS_H__
