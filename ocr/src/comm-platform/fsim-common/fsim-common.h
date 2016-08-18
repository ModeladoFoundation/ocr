/**
 * @brief Common defines for FSim communication
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __FSIM_COMMON_H__
#define __FSIM_COMMON_H__

#include "ocr-config.h"
#if defined(ENABLE_COMM_PLATFORM_CE) || defined(ENABLE_COMM_PLATFORM_XE)

#include "ocr-policy-domain.h"
#include "ocr-types.h"

/**< Data that is shipped around to communicate between CE and XE */
typedef struct {
    volatile u64 status;    /**< One of the FSIM_COMM_* values */
    u64 size;               /**< Size of the message at addr */
    ocrPolicyMsg_t *addr;   /**< Message being shipped around */
    ocrPolicyMsg_t *laddr;  /**< Local address (for book-keeping) */
} fsimCommSlot_t;

#define MAX_NUM_XE 8
#define MSG_QUEUE_OFFT  (0x0)
#define MSG_QUEUE_SIZE  (0x20)



/* Values for the status bits for communication buffers */
// The buffer is free
#define FSIM_COMM_FREE_BUFFER  0x0ULL
// The buffer is being written to
#define FSIM_COMM_RSVRD_BUFFER 0x1ULL
// The buffer is full with the address of a message
// to be marshalled
#define FSIM_COMM_FULL_BUFFER  0x2ULL
// The buffer needs to be cleaned-up (something needs to be freed)
#define FSIM_COMM_CLEANUP_BUFFER 0x4ULL
// The buffer is being read
#define FSIM_COMM_READING_BUFFER 0x8ULL

#endif /* ENABLE_COMM_PLATFORM_X/CE */
#endif
