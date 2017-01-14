/**
 * @brief XE communication platform
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
#ifndef __COMM_PLATFORM_XE_H__
#define __COMM_PLATFORM_XE_H__

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_XE

#include "ocr-comm-platform.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "comm-platform/fsim-common/fsim-common.h"


// Temporary buffers
// This is to work around an issue where the XE needs to call
// pdMalloc to store either a message that is too big to send or
// a message that is incomming. If there is not enough space in the
// L1, this will trigger a message to the CE and therefore cause a deadlock
// in communication (XE receiving message so incoming XE buffer full, CE tries to
// reply to second message (getting memory) but can't respond because incoming XE buffer
// full)
#ifdef OCR_HACK_DB_MOVE
// The size of a message has padding in this case so we don't need as much buffer
// If we use too much buffer, it makes the blob too big
#define XE_HACK_BUFFER_COUNT 1
#define XE_HACK_BUFFER_SIZE (sizeof(ocrPolicyMsg_t))
#else
#define XE_HACK_BUFFER_COUNT 2
#define XE_HACK_BUFFER_SIZE (sizeof(ocrPolicyMsg_t)*XE_HACK_BUFFER_COUNT)
#endif

typedef struct {
    ocrCommPlatformFactory_t base;
} ocrCommPlatformFactoryXe_t;

typedef struct {
    ocrCommPlatform_t base;

    u64 N;                      // Agent bits from our tuple
    fsimCommSlot_t * rq;        // CE stage for this XE

    ocrPolicyMsg_t inBuffer[XE_HACK_BUFFER_COUNT];
	ocrPolicyMsg_t outBuffer[XE_HACK_BUFFER_COUNT];
	bool inBufferFree;
} ocrCommPlatformXe_t;

typedef struct {
    paramListCommPlatformInst_t base;
} paramListCommPlatformXe_t;

extern ocrCommPlatformFactory_t* newCommPlatformFactoryXe(ocrParamList_t *perType);

#endif /* ENABLE_COMM_PLATFORM_XE */
#endif /* __COMM_PLATFORM_XE_H__ */
