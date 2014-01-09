/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef HC_H_
#define HC_H_

#include "ocr-types.h"

#define SEALED_LIST ((void *) -1)
#define END_OF_LIST NULL

typedef struct _regNode_t {
    ocrGuid_t guid;
    u32 slot;
} regNode_t;

#endif /* HC_H_ */
