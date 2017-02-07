/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_SAL_FSIM_XE_H__
#define __OCR_SAL_FSIM_XE_H__

#include "ocr-hal.h"
#include "xe-abi.h"

/**
 * @brief Function to drive the runlevel changes on boot-up as well
 * as the runlevel changes on tear-down
 *
 * This function will be called by tgkrnl to start the PD for the XE
 *
 * @param[in] pd    Pointer to the policy domain to start
 */
void salPdDriver(void* pd);

extern u32 salPause(bool isBlocking);

extern ocrGuid_t salQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags);

extern void salResume(u32 flag);

extern void salInjectFault(void);

#define sal_abort() hal_abort()

#define sal_exit(x) hal_exit(x)

#ifdef TG_GDB_SUPPORT
extern void __xeDoAssert(const char* fn, u32 ln);

#define sal_assert(x, fn, ln) do { if(!(x)) {                   \
            PRINTF("ASSERT FAILUTRE XE at line %"PRId32" in '%s'\n", (int)(ln), fn); \
            __xeDoAssert(fn, ln);                                       \
            __asm__ __volatile__ __attribute__((noreturn)) (    \
                "lea %0, %0\n\t"                                \
                "alarm %2\n\t"                                  \
                :                                               \
                : "{r" XE_ASSERT_FILE_REG_STR "}" (fn),         \
                  "{r" XE_ASSERT_LINE_REG_STR "}" (ln),         \
                  "L" (XE_ASSERT_ERROR)                         \
                : "r" XE_ASSERT_FILE_REG_STR);                  \
        } } while(0)
#else
#define sal_assert(x, fn, ln) do { if(!(x)) {                           \
            __asm__ __volatile__ __attribute__((noreturn)) (            \
                "lea %0, %0\n\t"                                        \
                "alarm %2\n\t"                                          \
                :                                                       \
                : "{r" XE_ASSERT_FILE_REG_STR "}" (fn),                 \
                  "{r" XE_ASSERT_LINE_REG_STR "}" (ln),                 \
                  "L" (XE_ASSERT_ERROR)                                 \
                : "r" XE_ASSERT_FILE_REG_STR);                          \
        } } while(0)
#endif

#define sal_print(msg, len) __asm__ __volatile__(                   \
        "lea %0, %0\n\t"                                            \
        "alarm %2\n\t"                                              \
        :                                                           \
        : "{r" XE_CONOUT_MSG_REG_STR "}" (msg),                     \
          "{r" XE_CONOUT_LEN_REG_STR "}" (len),                     \
          "L" (XE_CONOUT_ALARM)                                     \
        : "r" XE_CONOUT_MSG_REG_STR)

#ifdef ENABLE_EXTENSION_PERF

typedef struct _salPerfCounter {
    u64 perfVal;
} salPerfCounter;

u64 salPerfInit(salPerfCounter* perfCtr);
u64 salPerfStart(salPerfCounter* perfCtr);
u64 salPerfStop(salPerfCounter* perfCtr);
u64 salPerfShutdown(salPerfCounter *perfCtr);

#endif

#endif /* __OCR_SAL_FSIM_XE_H__ */
