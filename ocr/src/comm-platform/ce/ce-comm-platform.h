/**
 * @brief CE communication platform
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
#ifndef __COMM_PLATFORM_CE_H__
#define __COMM_PLATFORM_CE_H__

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_CE

#include "ocr-comm-platform.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "comm-platform/fsim-common/fsim-common.h"


/* CE-CE communication buffers */
/* Number of messages that other CEs can send to me */
#define OUTSTANDING_CE_MSGS   16
/* Number of messages I can send at once (to other CEs). We need
 * a few because when we need to deal with barriers, it is debilitating to not be
 * able to quickly send out all requests for barriers */
#define OUTSTANDING_CE_SEND   4
#define MSG_CE_ADDR_OFFT      ((u64)(MSG_QUEUE_OFFT + MAX_NUM_XE*MSG_QUEUE_SIZE))
#define MSG_CE_RECV_BUF_OFFT  ((u64)(MSG_CE_ADDR_OFFT + OUTSTANDING_CE_MSGS*sizeof(u64)))
// Bit twidling to ensure sizeof(ocrPolicyMsg_t) does not make things unaligned
#define MSG_CE_SEND_BUF_OFFT  ((u64)((MSG_CE_RECV_BUF_OFFT + sizeof(ocrPolicyMsg_t) + 7) & ~0x7ULL))


typedef struct {
    ocrCommPlatformFactory_t base;
} ocrCommPlatformFactoryCe_t;

typedef struct {
    ocrCommPlatform_t base;
    fsimCommSlot_t* rq[MAX_NUM_XE];      // Remote stages for this block's XEs
    fsimCommSlot_t* lq[MAX_NUM_XE];      // Local stages for this block's XEs
    u64 pollq;                           // Round-robin queue to poll next
} ocrCommPlatformCe_t;

typedef struct {
    paramListCommPlatformInst_t base;
} paramListCommPlatformCe_t;

extern ocrCommPlatformFactory_t* newCommPlatformFactoryCe(ocrParamList_t *perType);

#endif /* ENABLE_COMM_PLATFORM_CE */
#endif /* __COMM_PLATFORM_CE_H__ */
