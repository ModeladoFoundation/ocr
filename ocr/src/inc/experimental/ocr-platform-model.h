/**
 * @brief Platform Model (converting affinities to locations)
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_PLATFORM_MODEL_H__
#define __OCR_PLATFORM_MODEL_H__

#include "ocr-config.h"

#include "ocr-types.h"
#include "ocr-runtime-types.h"

typedef struct _ocrPlatformModel_t {
} ocrPlatformModel_t;

// Affinity data-structure, tailored to location placer
typedef struct _ocrAffinity_t {
    ocrLocation_t place;
} ocrAffinity_t;

typedef struct _ocrPlatformModelAffinity_t {
    ocrPlatformModel_t base;
    u64 pdLocAffinitiesSize; /**< Count of available locations */
    u32 current;
    ocrGuid_t * pdLocAffinities;
} ocrPlatformModelAffinity_t;

struct _ocrPolicyDomain_t;

u8 affinityToLocation(ocrLocation_t* result, ocrGuid_t affinityGuid);
ocrPlatformModel_t * createPlatformModelAffinity(struct _ocrPolicyDomain_t * pd);
void destroyPlatformModelAffinity(struct _ocrPolicyDomain_t * pd);

#endif /* __OCR_PLATFORM_MODEL_H__ */
