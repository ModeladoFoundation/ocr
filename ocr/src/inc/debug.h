/**
 * @brief Some debug utilities for OCR
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __DEBUG_H__
#define __DEBUG_H__

#include "ocr-hal.h"
#include "ocr-sal.h"
#include "ocr-task.h"
#include "ocr-worker.h"

#ifdef OCR_TRACE_BINARY
#include "utils/tracer/trace-callbacks.h"
extern void doTrace(u64 location, u64 wrkr, ocrGuid_t taskGuid, ...);
#endif

#ifdef OCR_DEBUG
/**
 * @brief No debugging messages are printed
 *
 * Note that you can still be in debugging mode
 * and have no messages printed as this would allow
 * all ASSERTs to be checked
 */
#define DEBUG_LVL_NONE      0
#define OCR_DEBUG_0_STR "NONE"

/**
 * @brief Only warnings are printed
 */
#define DEBUG_LVL_WARN      1
#define OCR_DEBUG_1_STR "WARN"

/**
 * @brief Warnings and informational
 * messages are printed
 *
 * Default debug level if nothing is specified
 * for #OCR_DEBUG_LVL (compile time)
 */
#define DEBUG_LVL_INFO      2
#define OCR_DEBUG_2_STR "INFO"

/**
 * @brief Warnings, informational
 * messages and verbose messages are printed
 */
#define DEBUG_LVL_VERB      3
#define OCR_DEBUG_3_STR "VERB"

/**
 * @brief Everything is printed
 */
#define DEBUG_LVL_VVERB     4
#define OCR_DEBUG_4_STR "VVERB"

#ifndef OCR_DEBUG_LVL
/**
 * @brief Debug level
 *
 * This controls the verbosity of the
 * debug messages in debug mode
 */
#define OCR_DEBUG_LVL DEBUG_LVL_INFO
#endif /* OCR_DEBUG_LVL */

/**
 * @brief Debug mask
 *
 * The debug levels above only use 3 bits of a
 * larger mask.  Mask values start at 0x00000008.
 */
extern u64 Debug_Mask;
extern char * pd_type_to_str(int type);
#define DEBUG_MSK_MSGTRACE 8
#define OCR_DEBUG_8_STR "MSG"
#define DEBUG_MSK_EDTSTATS 16
#define OCR_DEBUG_16_STR "EDTSTAT"

#ifdef OCR_DEBUG_ALLOCATOR
#define OCR_DEBUG_ALLOCATOR 1
#else
#define OCR_DEBUG_ALLOCATOR 0
#endif

#define OCR_DEBUG_ALLOCATOR_STR "ALLOC"
#ifndef DEBUG_LVL_ALLOCATOR
#define DEBUG_LVL_ALLOCATOR OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_API
#define OCR_DEBUG_API 1
#else
#define OCR_DEBUG_API 0
#endif

#define OCR_DEBUG_API_STR "API"
#ifndef DEBUG_LVL_API
#define DEBUG_LVL_API OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_COMP_PLATFORM
#define OCR_DEBUG_COMP_PLATFORM 1
#else
#define OCR_DEBUG_COMP_PLATFORM 0
#endif

#define OCR_DEBUG_COMP_PLATFORM_STR "COMP-PLAT"
#ifndef DEBUG_LVL_COMP_PLATFORM
#define DEBUG_LVL_COMP_PLATFORM OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_COMM_PLATFORM
#define OCR_DEBUG_COMM_PLATFORM 1
#else
#define OCR_DEBUG_COMM_PLATFORM 0
#endif

#define OCR_DEBUG_COMM_PLATFORM_STR "COMM-PLAT"
#ifndef DEBUG_LVL_COMM_PLATFORM
#define DEBUG_LVL_COMM_PLATFORM OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_COMM_API
#define OCR_DEBUG_COMM_API 1
#else
#define OCR_DEBUG_COMM_API 0
#endif

#define OCR_DEBUG_COMM_API_STR "COMM-API"
#ifndef DEBUG_LVL_COMM_API
#define DEBUG_LVL_COMM_API OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_COMM_WORKER
#define OCR_DEBUG_COMM_WORKER 1
#else
#define OCR_DEBUG_COMM_WORKER 0
#endif

#define OCR_DEBUG_COMM_WORKER_STR "COMM-WORK"
#ifndef DEBUG_LVL_COMM_WORKER
#define DEBUG_LVL_COMM_WORKER OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_COMP_TARGET
#define OCR_DEBUG_COMP_TARGET 1
#else
#define OCR_DEBUG_COMP_TARGET 0
#endif

#define OCR_DEBUG_COMP_TARGET_STR "COMP-TARG"
#ifndef DEBUG_LVL_COMP_TARGET
#define DEBUG_LVL_COMP_TARGET OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_DATABLOCK
#define OCR_DEBUG_DATABLOCK 1
#else
#define OCR_DEBUG_DATABLOCK 0
#endif

#define OCR_DEBUG_DATABLOCK_STR "DB"
#ifndef DEBUG_LVL_DATABLOCK
#define DEBUG_LVL_DATABLOCK OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_EVENT
#define OCR_DEBUG_EVENT 1
#else
#define OCR_DEBUG_EVENT 0
#endif

#define OCR_DEBUG_EVENT_STR "EVT"
#ifndef DEBUG_LVL_EVENT
#define DEBUG_LVL_EVENT OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_GUID
#define OCR_DEBUG_GUID 1
#else
#define OCR_DEBUG_GUID 0
#endif

#define OCR_DEBUG_GUID_STR "GUID"
#ifndef DEBUG_LVL_GUID
#define DEBUG_LVL_GUID OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_INIPARSING
#define OCR_DEBUG_INIPARSING 1
#else
#define OCR_DEBUG_INIPARSING 0
#endif

#define OCR_DEBUG_INIPARSING_STR "INI-PARSING"
#ifndef DEBUG_LVL_INIPARSING
#define DEBUG_LVL_INIPARSING OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_MACHINE
#define OCR_DEBUG_MACHINE 1
#else
#define OCR_DEBUG_MACHINE 0
#endif

#define OCR_DEBUG_MACHINE_STR "MACHINE-DESC"
#ifndef DEBUG_LVL_MACHINE
#define DEBUG_LVL_MACHINE OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_MEM_PLATFORM
#define OCR_DEBUG_MEM_PLATFORM 1
#else
#define OCR_DEBUG_MEM_PLATFORM 0
#endif

#define OCR_DEBUG_MEM_PLATFORM_STR "MEM-PLAT"
#ifndef DEBUG_LVL_MEM_PLATFORM
#define DEBUG_LVL_MEM_PLATFORM OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_MEM_TARGET
#define OCR_DEBUG_MEM_TARGET 1
#else
#define OCR_DEBUG_MEM_TARGET 0
#endif

#define OCR_DEBUG_MEM_TARGET_STR "MEM-TARG"
#ifndef DEBUG_LVL_MEM_TARGET
#define DEBUG_LVL_MEM_TARGET OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_POLICY
#define OCR_DEBUG_POLICY 1
#else
#define OCR_DEBUG_POLICY 0
#endif

#define OCR_DEBUG_POLICY_STR "POLICY"
#ifndef DEBUG_LVL_POLICY
#define DEBUG_LVL_POLICY OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_MICROTASKS
#define OCR_DEBUG_MICROTASKS 1
#else
#define OCR_DEBUG_MICROTASKS 0
#endif

#define OCR_DEBUG_MICROTASKS_STR "MICROTASKS"
#ifndef DEBUG_LVL_MICROTASKS
#define DEBUG_LVL_MICROTASKS OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_SAL
#define OCR_DEBUG_SAL 1
#else
#define OCR_DEBUG_SAL 0
#endif

#define OCR_DEBUG_SAL_STR "SAL"
#ifndef DEBUG_LVL_SAL
#define DEBUG_LVL_SAL OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_SCHEDULER
#define OCR_DEBUG_SCHEDULER 1
#else
#define OCR_DEBUG_SCHEDULER 0
#endif

#define OCR_DEBUG_SCHEDULER_STR "SCHED"
#ifndef DEBUG_LVL_SCHEDULER
#define DEBUG_LVL_SCHEDULER OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_SCHEDULER_HEURISTIC
#define OCR_DEBUG_SCHEDULER_HEURISTIC 1
#else
#define OCR_DEBUG_SCHEDULER_HEURISTIC 0
#endif

#define OCR_DEBUG_SCHEDULER_HEURISTIC_STR "SCHED-HEURISTIC"
#ifndef DEBUG_LVL_SCHEDULER_HEURISTIC
#define DEBUG_LVL_SCHEDULER_HEURISTIC OCR_DEBUG_LVL
#endif

#ifdef OCR_DEBUG_SCHEDULER_OBJECT
#define OCR_DEBUG_SCHEDULER_OBJECT 1
#else
#define OCR_DEBUG_SCHEDULER_OBJECT 0
#endif

#define OCR_DEBUG_SCHEDULER_OBJECT_STR "SCHED-OBJECT"
#ifndef DEBUG_LVL_SCHEDULER_OBJECT
#define DEBUG_LVL_SCHEDULER_OBJECT OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_STATS
#define OCR_DEBUG_STATS 1
#else
#define OCR_DEBUG_STATS 0
#endif

#define OCR_DEBUG_STATS_STR "STATS"
#ifndef DEBUG_LVL_STATS
#define DEBUG_LVL_STATS OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_SYNC
#define OCR_DEBUG_SYNC 1
#else
#define OCR_DEBUG_SYNC 0
#endif

#define OCR_DEBUG_SYNC_STR "SYNC"
#ifndef DEBUG_LVL_SYNC
#define DEBUG_LVL_SYNC OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_SYSBOOT
#define OCR_DEBUG_SYSBOOT 1
#else
#define OCR_DEBUG_SYSBOOT 0
#endif

#define OCR_DEBUG_SYSBOOT_STR "SYSBOOT"
#ifndef DEBUG_LVL_SYSBOOT
#define DEBUG_LVL_SYSBOOT OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_TASK
#define OCR_DEBUG_TASK 1
#else
#define OCR_DEBUG_TASK 0
#endif

#define OCR_DEBUG_TASK_STR "EDT"
#ifndef DEBUG_LVL_TASK
#define DEBUG_LVL_TASK OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_WORKER
#define OCR_DEBUG_WORKER 1
#else
#define OCR_DEBUG_WORKER 0
#endif

#define OCR_DEBUG_WORKER_STR "WORKER"
#ifndef DEBUG_LVL_WORKER
#define DEBUG_LVL_WORKER OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_WORKPILE
#define OCR_DEBUG_WORKPILE 1
#else
#define OCR_DEBUG_WORKPILE 0
#endif

#define OCR_DEBUG_WORKPILE_STR "WORKPILE"
#ifndef DEBUG_LVL_WORKPILE
#define DEBUG_LVL_WORKPILE OCR_DEBUG_LVL
#endif


#ifdef OCR_DEBUG_UTIL
#define OCR_DEBUG_UTIL 1
#else
#define OCR_DEBUG_UTIL 0
#endif

#define OCR_DEBUG_UTIL_STR "UTIL"
#ifndef DEBUG_LVL_UTIL
#define DEBUG_LVL_UTIL OCR_DEBUG_LVL
#endif


// Build system must define 'OCR_ASSERT' for assertions to work

#define DO_DEBUG_TYPE(type, level) \
    if(OCR_DEBUG_##type  && level <= DEBUG_LVL_##type) {


/* This weird macro allows for placing debug messages
   in different sections. This is useful when you are constrained
   in memory
*/
#ifdef OCR_DPRINTF_SECTION

#define OCR_DPRINTF_SECT_STR_INT(sect) #sect
#define OCR_DPRINTF_SECT_STR(sect) OCR_DPRINTF_SECT_STR_INT(sect)

#define DPRINTF_STR(str) (__extension__({                               \
                static const __attribute__((__section__(OCR_DPRINTF_SECT_STR(OCR_DPRINTF_SECTION)))) char __c[] = (str); \
                (const char *)&__c;                                     \
            }))

#else
#define DPRINTF_STR(str) (str)
#endif

#ifdef OCR_TRACE_BINARY

#undef OCR_DEBUG_INIPARSING
#define OCR_DEBUG_INIPARSING DEBUG_LVL_NONE
#undef OCR_DEBUG_MACHINE
#define OCR_DEBUG_MACHINE DEBUG_LVL_NONE
//Binary trace enabled, suppressing INIPARSING and MACHINE
//DPRINTFs until bug #829 is addressed.

#if (OCR_DEBUG_LVL==DEBUG_LVL_WARN)
#undef OCR_DEBUG_LVL
#define OCR_DEBUG_LVL DEBUG_LVL_INFO
//Binary trace enabled, overriding debug verbosity to DEBUG_LVL_INFO
#endif

#define DPRINTF_TYPE(type, level, format, ...)
//NO-OP... Suppress DPRINTF console output if tracing is active

#else

#define DPRINTF_TYPE(type, level, format, ...)   do {                   \
    if(OCR_DEBUG_##type && (level <= DEBUG_LVL_##type || level & Debug_Mask)) {                 \
        ocrTask_t *__task = NULL; ocrWorker_t *__worker = NULL;         \
        struct _ocrPolicyDomain_t *__pd = NULL;                         \
        getCurrentEnv(&__pd, &__worker, &__task, NULL);                 \
        ocrGuid_t __taskGuid = __task ? __task->guid : NULL_GUID;       \
        PRINTF(DPRINTF_STR(OCR_DEBUG_##type##_STR "(" OCR_DEBUG_##level##_STR \
                           ") [PD:0x%"PRIx64" W:0x%"PRIx64" EDT:"GUIDF"] " format), \
               __pd?(u64)__pd->myLocation:0,                            \
               __worker?(u64)__worker->id:0,                            \
               GUIDA(__taskGuid), ## __VA_ARGS__);                      \
    } } while(0)

#endif /*OCR_TRACE_BINARY*/

#define DPRINTF_TYPE_COND_LVL(type, cond, levelT, levelF, format, ...)  \
    do {                                                                \
        if(cond) {                                                      \
            DPRINTF_TYPE(type, levelT, format, ## __VA_ARGS__);         \
        } else {                                                        \
            DPRINTF_TYPE(type, levelF, format, ## __VA_ARGS__);         \
        }                                                               \
    } while(0)

#else
#define DO_DEBUG_TYPE(level) if(0) {
#define DPRINTF_TYPE(type, level, format, ...)
#define DPRINTF_TYPE_COND_LVL(type, cond, levelT, levelF, format, ...)
#endif /* OCR_DEBUG */

#ifdef OCR_TRACE

#ifdef OCR_TRACE_ALLOCATOR
#define OCR_TRACE_ALLOCATOR 1
#else
#define OCR_TRACE_ALLOCATOR 0
#endif

#ifdef OCR_TRACE_API
#define OCR_TRACE_API 1
#else
#define OCR_TRACE_API 0
#endif

#ifdef OCR_TRACE_COMP_PLATFORM
#define OCR_TRACE_COMP_PLATFORM 1
#else
#define OCR_TRACE_COMP_PLATFORM 0
#endif

#ifdef OCR_TRACE_COMM_PLATFORM
#define OCR_TRACE_COMM_PLATFORM 1
#else
#define OCR_TRACE_COMM_PLATFORM 0
#endif

#ifdef OCR_TRACE_COMM_API
#define OCR_TRACE_COMM_API 1
#else
#define OCR_TRACE_COMM_API 0
#endif

#ifdef OCR_TRACE_COMM_WORKER
#define OCR_TRACE_COMM_WORKER 1
#else
#define OCR_TRACE_COMM_WORKER 0
#endif

#ifdef OCR_TRACE_COMP_TARGET
#define OCR_TRACE_COMP_TARGET 1
#else
#define OCR_TRACE_COMP_TARGET 0
#endif

#ifdef OCR_TRACE_DATABLOCK
#define OCR_TRACE_DATABLOCK 1
#else
#define OCR_TRACE_DATABLOCK 0
#endif

#ifdef OCR_TRACE_EVENT
#define OCR_TRACE_EVENT 1
#else
#define OCR_TRACE_EVENT 0
#endif

#ifdef OCR_TRACE_GUID
#define OCR_TRACE_GUID 1
#else
#define OCR_TRACE_GUID 0
#endif

#ifdef OCR_TRACE_INIPARSING
#define OCR_TRACE_INIPARSING 1
#else
#define OCR_TRACE_INIPARSING 0
#endif

#ifdef OCR_TRACE_MACHINE
#define OCR_TRACE_MACHINE 1
#else
#define OCR_TRACE_MACHINE 0
#endif

#ifdef OCR_TRACE_MEM_PLATFORM
#define OCR_TRACE_MEM_PLATFORM 1
#else
#define OCR_TRACE_MEM_PLATFORM 0
#endif

#ifdef OCR_TRACE_MEM_TARGET
#define OCR_TRACE_MEM_TARGET 1
#else
#define OCR_TRACE_MEM_TARGET 0
#endif

#ifdef OCR_TRACE_POLICY
#define OCR_TRACE_POLICY 1
#else
#define OCR_TRACE_POLICY 0
#endif

#ifdef OCR_TRACE_SCHEDULER
#define OCR_TRACE_SCHEDULER 1
#else
#define OCR_TRACE_SCHEDULER 0
#endif

#ifdef OCR_TRACE_SCHEDULER_HEURISTIC
#define OCR_TRACE_SCHEDULER_HEURISTIC 1
#else
#define OCR_TRACE_SCHEDULER_HEURISTIC 0
#endif

#ifdef OCR_TRACE_SCHEDULER_OBJECT
#define OCR_TRACE_SCHEDULER_OBJECT 1
#else
#define OCR_TRACE_SCHEDULER_OBJECT 0
#endif

#ifdef OCR_TRACE_STATS
#define OCR_TRACE_STATS 1
#else
#define OCR_TRACE_STATS 0
#endif

#ifdef OCR_TRACE_SYNC
#define OCR_TRACE_SYNC 1
#else
#define OCR_TRACE_SYNC 0
#endif

#ifdef OCR_TRACE_SYSBOOT
#define OCR_TRACE_SYSBOOT 1
#else
#define OCR_TRACE_SYSBOOT 0
#endif

#ifdef OCR_TRACE_TASK
#define OCR_TRACE_TASK 1
#else
#define OCR_TRACE_TASK 0
#endif

#ifdef OCR_TRACE_WORKER
#define OCR_TRACE_WORKER 1
#else
#define OCR_TRACE_WORKER 0
#endif

#ifdef OCR_TRACE_WORKPILE
#define OCR_TRACE_WORKPILE 1
#else
#define OCR_TRACE_WORKPILE 0
#endif

#ifdef OCR_TRACE_UTIL
#define OCR_TRACE_UTIL 1
#else
#define OCR_TRACE_UTIL 0
#endif


#define TPRINTF_TYPE(type, format, ...) do {                            \
        if(OCR_TRACE_##type) {                                          \
            ocrTask_t *__task = NULL; ocrWorker_t *__worker = NULL;     \
            struct _ocrPolicyDomain_t *__pd = NULL;                     \
            getCurrentEnv(&__pd, &__worker, &__task, NULL);             \
            ocrGuid_t __taskGuid = __task ? __task->guid : NULL_GUID;   \
            PRINTF(OCR_DEBUG_##type##_STR "(TRACE) "                    \
                   "[PD:0x%"PRIx64" W:0x%"PRIx64" EDT:0x"GUIDF"] " format, \
                   __pd?(u64)__pd->myLocation:0,                        \
                   __worker?(u64)__worker->id:0,                        \
                   GUIDA(__taskGuid), ## __VA_ARGS__);                  \
        }} while(0)

#else
#define TPRINTF_TYPE(type, level, format, ...)
#endif /* OCR_TRACE */

#define DO_DEBUG_TYPE_INT(type, level) DO_DEBUG_TYPE(type, level)
#define DO_DEBUG(level) DO_DEBUG_TYPE_INT(DEBUG_TYPE, level)

#define DPRINTF_TYPE_INT(type, level, format, ...) DPRINTF_TYPE(type, level, format, ## __VA_ARGS__)

#define DPRINTF(level, format, ...) DPRINTF_TYPE_INT(DEBUG_TYPE, level, format, ## __VA_ARGS__)
#define DPRINTF_COND_LVL(cond, levelT, levelF, format, ...) \
    DPRINTF_TYPE_COND_LVL(DEBUG_TYPE, cond, levelT, levelF, format, ## __VA_ARGS__)

#define END_DEBUG }

#define TPRINTF_TYPE_INT(type, format, ...) TPRINTF_TYPE(type, format, ## __VA_ARGS__)
#define TPRINTF(format, ...) TPRINTF_TYPE_INT(DEBUG_TYPE, format, ## __VA_ARGS__)

#ifdef OCR_ASSERT
#define ASSERT(a) do { sal_assert((bool)((a) != 0), __FILE__, __LINE__); } while(0);
#define RESULT_ASSERT(a, op, b) do { sal_assert((a) op (b), __FILE__, __LINE__); } while(0);
#define RESULT_TRUE(a) do { sal_assert((a) != 0, __FILE__, __LINE__); } while(0);
#define ASSERT_BLOCK_BEGIN(cond) if(!(cond)) {
#define ASSERT_BLOCK_END ASSERT(false && "assert block failure"); }
#else
#define ASSERT(a)
#define RESULT_ASSERT(a, op, b) do { a; } while(0);
#define RESULT_TRUE(a) do { a; } while(0);
#define ASSERT_BLOCK_BEGIN(cond) if(0) {
#define ASSERT_BLOCK_END }
#endif /* OCR_ASSERT */

// Some asserts we really should report and abort, overflows etc...
#ifdef OCR_ASSERT_CRITICAL
#define ASSERT_CRITICAL(a) do { sal_assert((bool)((a) != 0), __FILE__, __LINE__); } while(0);
#else
#define ASSERT_CRITICAL(a) ASSERT(a)
#endif


#ifndef VERIFY
#define VERIFY(cond, format, ...)                                       \
    do {                                                                \
        if(!(cond)) {                                                   \
            PRINTF("FAILURE @ '%s:%"PRId32"' " format, __FILE__, __LINE__, ## __VA_ARGS__); \
        } else {                                                        \
            PRINTF("PASSED @ '%s:%"PRId32"' " format, __FILE__, __LINE__, ## __VA_ARGS__); \
        }                                                               \
    } while(0);
#endif

#ifdef OCR_TRACE_BINARY
//Call Tracing Function
#define OCR_TOOL_TRACE(...) do {             \
    ocrTask_t *_task = NULL; ocrWorker_t *_worker = NULL;               \
    struct _ocrPolicyDomain_t *_pd = NULL;                              \
    getCurrentEnv(&_pd, &_worker, &_task, NULL);                        \
    doTrace(_pd?(u64)_pd->myLocation:0,                                 \
            _worker?(u64)_worker->id:0,                                 \
            _task?_task->guid:NULL_GUID,                                \
            ## __VA_ARGS__);                                            \
    } while(0)

#else
//NO-OP
#define OCR_TOOL_TRACE(...)

#endif /* OCR_TRACE_BINARY */


// Support for compile time asserts
#define COMPILE_ASSERT(e) extern char (*COMPILE_ASSERT(void))[sizeof(char[1-2*!(e)])]

// Include this to get the DPRINTF thing to work properly
#ifndef OCR_POLICY_DOMAIN_H_
#include "ocr-policy-domain.h"
#endif

#endif /* __DEBUG_H__ */

