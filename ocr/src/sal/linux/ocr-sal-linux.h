/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_SAL_LINUX_H__
#define __OCR_SAL_LINUX_H__

#include "ocr-hal.h"
#include "ocr-perfmon.h"

#include <assert.h>
#include <stdio.h>

#ifdef ENABLE_EXTENSION_PERF
#include <unistd.h>
#include <asm/unistd.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#endif

void sig_handler(u32 sigNum);

extern u32 salPause(bool isBlocking);

extern ocrGuid_t salQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags);

extern void salResume(u32 flag);

extern void salInjectFault(void);

extern void registerSignalHandler();

#define sal_abort()   hal_abort()

#define sal_exit(x)   hal_exit(x)

#define sal_assert(x, f, l)  assert(x)

#define sal_print(msg, len)   printf("%s", msg)

#ifdef ENABLE_EXTENSION_PERF

typedef struct _salPerfCounter {
    u64 perfVal;
    u32 perfFd;
    u32 perfType;
    u64 perfConfig;
    struct perf_event_attr perfAttr;
} salPerfCounter;

u64 salPerfInit(salPerfCounter* perfCtr);
u64 salPerfStart(salPerfCounter* perfCtr);
u64 salPerfStop(salPerfCounter* perfCtr);
u64 salPerfShutdown(salPerfCounter *perfCtr);
#endif

#endif /* __OCR_SAL_LINUX_H__ */
