/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_EXTENSION_AFFINITY

#include "debug.h"
#include "extensions/ocr-affinity.h"
#include "ocr-policy-domain.h"
#include "experimental/ocr-platform-model.h"
#include "ocr-errors.h"

#include "utils/profiler/profiler.h"

#pragma message "AFFINITY extension is experimental and may not be supported on all platforms"

//Part of policy-domain debug messages
#define DEBUG_TYPE POLICY

//
// Affinity group API
//

// These allow to handle the simple case where the caller request a number
// of affinity guids and does the mapping. i.e. assign some computation
// and data to each guid.

u8 ocrAffinityCount(ocrAffinityKind kind, u64 * count) {
#if defined(TG_CE_TARGET)
    // CE should not ask anything
    *count = 0;
    return 0;
#endif
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_API_AFFINITY, OCR_ACTION_GET_COUNT);
    if(kind == AFFINITY_SIM)
        return 0;

    START_PROFILE(api_ocrAffinityCount)
    ocrPolicyDomain_t *pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    // If no platformModel, we know nothing so just say that we are the only one
    if(pd->platformModel == NULL) {
        *count = 1;
        RETURN_PROFILE(0);
    }

    if (kind == AFFINITY_PD) {
        //BUG #606/#VV4 Neighbors/affinities: this is assuming each PD knows about every other PDs
        //Need to revisit that when we have a better idea of what affinities are
        *count = (pd->neighborCount + 1);
    } else if ((kind == AFFINITY_PD_MASTER) || (kind == AFFINITY_CURRENT) || (kind == AFFINITY_GUID)) {
        // Change this implementation if 'AFFINITY_CURRENT' cardinality can be > 1
        *count = 1;
    } else {
        ASSERT(false && "Unknown affinity kind");
    }
    RETURN_PROFILE(0);
}

u8 ocrAffinityQuery(ocrGuid_t guid, u64 * count, ocrGuid_t * affinities) {
#if defined(TG_CE_TARGET)
    // CE should not ask anything
    if(count)
        *count = 0;
    affinities[0] = NULL_GUID;
    return 0;
#endif
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_API_AFFINITY, OCR_ACTION_QUERY);
    START_PROFILE(api_ocrAffinityQuery);
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPlatformModelAffinity_t * platformModel = ((ocrPlatformModelAffinity_t*)pd->platformModel);
    if(platformModel == NULL) {
        if (count != NULL) {
            ASSERT(*count > 0);
            *count = 1;
        }
        affinities[0] = NULL_GUID;
        RETURN_PROFILE(0);
    } else {
        if (count != NULL) {
            ASSERT(*count > 0);
            *count = 1;
        }
        if (ocrGuidIsNull(guid)) {
            affinities[0] = platformModel->pdLocAffinities[platformModel->current];
            RETURN_PROFILE(0);
        }
        ocrLocation_t loc = 0;
        ocrFatGuid_t fatGuid = {.guid=guid, .metaDataPtr=NULL};
        guidLocation(pd, fatGuid, &loc);
        //Current implementation doesn't store affinities of GUIDs
        //So we resolve the affinity of the GUID by looking up its
        //location and use that to index into the affinity array.
        //NOTE: Shortcoming is that it assumes location are integers.
#ifdef TG_XE_TARGET
        u32 loc2 = locationToIdx(loc);
        ASSERT((loc2) < platformModel->pdLocAffinitiesSize);
        affinities[0] = platformModel->pdLocAffinities[loc2];
#else
        ASSERT(((u32)loc) < platformModel->pdLocAffinitiesSize);
        affinities[0] = platformModel->pdLocAffinities[(u32)loc];
#endif
    }
    RETURN_PROFILE(0);
}


//BUG #606/#VV4 Neighbors/affinities:  This call returns affinities with identical mapping across PDs.
u8 ocrAffinityGet(ocrAffinityKind kind, u64 * count, ocrGuid_t * affinities) {
#if defined(TG_CE_TARGET)
    // CE should not ask anything
    ASSERT(count && (*count) == 0);
    return 0;
#endif
    START_PROFILE(api_ocrAffinityGet);
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPlatformModelAffinity_t * platformModel = ((ocrPlatformModelAffinity_t*)pd->platformModel);
    if(platformModel == NULL) {
        ASSERT(*count > 0);
        *count = 1;
        affinities[0] = NULL_GUID;
        RETURN_PROFILE(0);
    }

    if (kind == AFFINITY_PD) {
        ASSERT(*count <= (pd->neighborCount + 1));
        u64 i = 0;
        while(i < *count) {
            affinities[i] = platformModel->pdLocAffinities[i];
            i++;
        }
    } else if (kind == AFFINITY_PD_MASTER) {
        //BUG #610 Master PD: This should likely come from the INI file
        affinities[0] = platformModel->pdLocAffinities[0];
    } else if (kind == AFFINITY_CURRENT) {
        affinities[0] = platformModel->pdLocAffinities[platformModel->current];
    } else {
        ASSERT(false && "Unknown affinity kind");
    }
    RETURN_PROFILE(0);
}

u8 ocrAffinityGetAt(ocrAffinityKind kind, u64 idx, ocrGuid_t * affinity) {
#if defined(TG_CE_TARGET)
    // CE should not ask anything
    *affinity = NULL_GUID;
    return 0;
#endif
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_API_AFFINITY, OCR_ACTION_GET_AT);
    START_PROFILE(api_ocrAffinityGetAt);
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    ocrPlatformModelAffinity_t * platformModel = ((ocrPlatformModelAffinity_t*)pd->platformModel);
    if(platformModel == NULL) {
        affinity[0] = NULL_GUID;
        RETURN_PROFILE(OCR_EINVAL);
    }
    if (kind == AFFINITY_PD) {
        if (idx > (pd->neighborCount + 1)) {
            ASSERT(false && "error: ocrAffinityGetAt index is out of bounds");
            RETURN_PROFILE(OCR_EINVAL);
        }
        affinity[0] = platformModel->pdLocAffinities[idx];
    } else if (kind == AFFINITY_PD_MASTER) {
        //BUG #610 Master PD: This should likely come from the INI file
        affinity[0] = platformModel->pdLocAffinities[0];
    } else if (kind == AFFINITY_CURRENT) {
        affinity[0] = platformModel->pdLocAffinities[platformModel->current];
    } else {
        ASSERT(false && "Unknown affinity kind");
    }
    RETURN_PROFILE(0);
}

u8 ocrAffinityGetCurrent(ocrGuid_t * affinity) {
#if defined(TG_CE_TARGET)
    // CE should not ask anything
    *affinity = NULL_GUID;
    return 0;
#endif
    OCR_TOOL_TRACE(true, OCR_TRACE_TYPE_API_AFFINITY, OCR_ACTION_GET_CURRENT);
    u64 count = 1;
    return ocrAffinityGet(AFFINITY_CURRENT, &count, affinity);
}

u64 ocrAffinityToHintValue(ocrGuid_t affinity) {
#if GUID_BIT_COUNT == 64
    return affinity.guid;
#elif GUID_BIT_COUNT == 128
    return affinity.lower;
#else
#error Unknown GUID type
#endif
}

#endif /* ENABLE_EXTENSION_AFFINITY */
