/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"

#ifdef ENABLE_EXTENSION_PAUSE
#include "extensions/ocr-pause.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "ocr-sal.h"

#include "utils/profiler/profiler.h"

u32 ocrPause(bool isBlocking){
    START_PROFILE(api_ocrPause);
    u32 t = salPause(isBlocking);
    RETURN_PROFILE(t);
}

ocrGuid_t ocrQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags){
    START_PROFILE(api_ocrQuery);
    ocrGuid_t ret = salQuery(query, guid, result, size, flags);
    RETURN_PROFILE(ret);
}

void ocrResume(u32 flag){
    START_PROFILE(api_ocrResume);
    salResume(flag);
    RETURN_PROFILE()
}

void ocrInjectFault(){
    START_PROFILE(api_ocrInjectFault);
    salInjectFault();
    RETURN_PROFILE();
}

#endif /* ENABLE_EXTENSION_PAUSE */

