/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef OCR_RESILIENCY_H_
#define OCR_RESILIENCY_H_

#ifdef ENABLE_AMT_RESILIENCE
#define OCR_FAILURE_NONE        0
#define OCR_NODE_FAILURE_SELF   1
#define OCR_NODE_FAILURE_OTHER  2
#define OCR_NODE_SHUTDOWN       3
#endif

#define OCR_FAULT_ARG_NAME(name) _arg_##name
#define OCR_FAULT_ARG_FIELD(kind) data.OCR_FAULT_ARG_NAME(kind)

typedef enum {
    OCR_RESILIENCY_MONITOR_DEFAULT,
    OCR_RESILIENCY_MONITOR_FAULT,
    OCR_RESILIENCY_MONITOR_CHECKPOINT,
} ocrResiliencyMonitorProp;

typedef enum {
    OCR_FAULT_NONE = 0,
    OCR_FAULT_DATABLOCK_CORRUPTION,
} ocrFaultKind;

typedef union _ocrFaultData_t {
    struct {
        ocrFatGuid_t db;
    } OCR_FAULT_ARG_NAME(OCR_FAULT_DATABLOCK_CORRUPTION);
} ocrFaultData_t;

typedef struct _ocrResiliencyFaultArgs {
    ocrFaultKind kind;                        /* Kind of fault */
    ocrFaultData_t data;                      /* Fault related data */
} ocrFaultArgs_t;

typedef enum {
    OCR_CHECKPOINT_PD_READY,
    OCR_CHECKPOINT_PD_START,
    OCR_CHECKPOINT_PD_DONE,
    OCR_CHECKPOINT_PD_RESUME,
    OCR_RESTART_PD_TRUE,
    OCR_RESTART_PD_FALSE,
    OCR_RESTART_PD_READY,
    OCR_RESTART_PD_START,
    OCR_RESTART_PD_DONE,
    OCR_RESTART_PD_RESUME
} ocrCheckpointProp;

#endif /* OCR_RESILIENCY_H_ */
