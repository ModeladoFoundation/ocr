/**
 * @brief System Abstraction Layer
 * Not quite hardware ops, not quite runtime, e.g. printf().
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_SAL_H__
#define __OCR_SAL_H__

#include "ocr-config.h"
#include "ocr-types.h"

struct _ocrPolicyDomain_t;
struct _ocrWorker_t;
struct _ocrTask_t;
struct _ocrPolicyMsg_t;
void getCurrentEnv(struct _ocrPolicyDomain_t** pd, struct _ocrWorker_t** worker,
                   struct _ocrTask_t **task, struct _ocrPolicyMsg_t* msg);


u32 SNPRINTF(char * buf, u32 size, const char * fmt, ...) __attribute__((__format__ (__printf__, 3, 4)));;
u32 ocrPrintf(const char * fmt, ...) __attribute__((__format__ (__printf__, 1, 2)));;
u32 PRINTF(const char * fmt, ...) __attribute__((__format__ (__printf__, 1, 2)));;
void _ocrAssert(bool val, const char* str, const char* file, u32 line);
u64 salGetTime();

#if defined(SAL_FSIM_XE)
#include "sal/fsim-xe/ocr-sal-fsim-xe.h"
#elif defined(SAL_FSIM_CE)
#include "sal/fsim-ce/ocr-sal-fsim-ce.h"
#elif defined(SAL_LINUX)
#include "sal/linux/ocr-sal-linux.h"
#else
#error "Unknown SAL layer"
#endif /* Switch of SAL_LAYER */

#endif /* __OCR_SAL_H__ */
