/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __HC_TASK_H__
#define __HC_TASK_H__

#include "ocr-config.h"
#if defined(ENABLE_TASK_HC) || defined(ENABLE_TASKTEMPLATE_HC)

#ifndef OCR_MAX_MULTI_SLOT
#define OCR_MAX_MULTI_SLOT 1
#endif

#include "hc/hc.h"
#include "ocr-hal.h"
#include "ocr-task.h"
#include "utils/ocr-utils.h"
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
#include "ocr-policy-domain-tasks.h"
#endif

#ifdef ENABLE_HINTS
/**< The number of hint properties supported by this implementation
 * The properties supported are specified in the hc-task.c file. */
#define OCR_HINT_COUNT_EDT_HC   6
#else
#define OCR_HINT_COUNT_EDT_HC   0
#endif

#ifdef ENABLE_TASKTEMPLATE_HC
/*! \brief Event Driven Task(EDT) template implementation
 */
typedef struct {
    ocrTaskTemplate_t base;
    ocrRuntimeHint_t hint;
} ocrTaskTemplateHc_t;

typedef struct {
    ocrTaskTemplateFactory_t baseFactory;
} ocrTaskTemplateFactoryHc_t;

ocrTaskTemplateFactory_t * newTaskTemplateFactoryHc(ocrParamList_t* perType, u32 factoryId);
#endif /* ENABLE_TASKTEMPLATE_HC */

#ifdef ENABLE_TASK_HC

#ifdef ENABLE_OCR_API_DEFERRABLE
#include "utils/queue.h"
#endif

/*! \brief Event Driven Task(EDT) implementation for OCR Tasks
 */
typedef struct {
    ocrTask_t base;

#if !(defined(REG_ASYNC) || defined(REG_ASYNC_SGL))
    lock_t lock;
#endif
    u32 frontierSlot; /**< Slot of the execution frontier
                           This excludes once events */
    u32 slotSatisfiedCount; /**< Number of slots satisfied */
    regNode_t * signalers; /**< Does not grow, set once when the task is created */
    ocrGuid_t* unkDbs;     /**< Contains the list of DBs dynamically acquired (through DB create) */
    u32 countUnkDbs;       /**< Count in unkDbs */
    u32 maxUnkDbs;         /**< Maximum number in unkDbs */
    ocrEdtDep_t * resolvedDeps; /**< List of satisfied dependences */
    u64 doNotReleaseSlots[OCR_MAX_MULTI_SLOT];
    ocrRuntimeHint_t hint;
#ifdef ENABLE_OCR_API_DEFERRABLE
#ifdef ENABLE_OCR_API_DEFERRABLE_MT
    pdEvent_t * evtHead;
    pdStrand_t * tailStrand;
#else
    Queue_t * evts;
#endif
#endif
#ifdef ENABLE_DEFERRED_MSGS
    ocrGuid_t completionEvt;
#endif
} ocrTaskHc_t;

#define HC_TASK_PARAMV_PTR(edt)     ((u64*)(((u64)edt) + sizeof(ocrTaskHc_t)))
#define HC_TASK_DEPV_PTR(edt)       ((regNode_t*)(((u64)edt) + sizeof(ocrTaskHc_t) + (((ocrTask_t *) edt)->paramc)*sizeof(u64)))
#define HC_TASK_DEPV_END(edt)       ((u64)HC_TASK_DEPV_PTR(edt) + (((ocrTask_t *) edt)->depc) * sizeof(regNode_t))
#define HC_TASK_HINT_PTR(edt)       ((((ocrTask_t*)edt)->flags & OCR_TASK_FLAG_USES_HINTS) ? (u64*)HC_TASK_DEPV_END(edt) : NULL)
#define HC_TASK_HINT_END(edt)       ((u64)((((ocrTask_t*)edt)->flags & OCR_TASK_FLAG_USES_HINTS) ? HC_TASK_DEPV_END(edt) + OCR_HINT_COUNT_EDT_HC * sizeof(u64) : HC_TASK_DEPV_END(edt)))
#define HC_TASK_SCHED_OBJ_PTR(edt)  ((((ocrTask_t*)edt)->flags & OCR_TASK_FLAG_USES_SCHEDULER_OBJECT) ? (u64*)HC_TASK_HINT_END(edt) : NULL)
#define HC_TASK_SCHED_OBJ_END(edt)  ((u64)((((ocrTask_t*)edt)->flags & OCR_TASK_FLAG_USES_SCHEDULER_OBJECT) ? HC_TASK_HINT_END(edt) + sizeof(u64) : HC_TASK_HINT_END(edt)))

#define HC_TASK_SIZE(edt)                                                                                           \
    (sizeof(ocrTaskHc_t) +                                                                                          \
    (((ocrTask_t *) edt)->paramc) * sizeof(u64) +                                                                   \
    (((ocrTask_t *) edt)->depc) * sizeof(regNode_t) +                                                               \
    (((((ocrTask_t *) edt)->flags & OCR_TASK_FLAG_USES_HINTS) != 0) ? OCR_HINT_COUNT_EDT_HC * sizeof(u64) : 0) +    \
    (((((ocrTask_t *) edt)->flags & OCR_TASK_FLAG_USES_SCHEDULER_OBJECT) != 0) ? sizeof(u64) : 0));

typedef struct {
    ocrTaskFactory_t baseFactory;
} ocrTaskFactoryHc_t;

ocrTaskFactory_t * newTaskFactoryHc(ocrParamList_t* perType, u32 factoryId);
#endif /* ENABLE_TASK_HC */

#endif /* ENABLE_TASK_HC or ENABLE_TASKTEMPLATE_HC */
#endif /* __HC_TASK_H__ */
