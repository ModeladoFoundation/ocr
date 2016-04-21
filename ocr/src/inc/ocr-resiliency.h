/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef OCR_RESILIENCY_H_
#define OCR_RESILIENCY_H_

#define OCR_FAULT_ARG_NAME(name) _arg_##name
#define OCR_FAULT_ARG_FIELD(kind) data.OCR_FAULT_ARG_NAME(kind)

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

#endif /* OCR_RESILIENCY_H_ */
