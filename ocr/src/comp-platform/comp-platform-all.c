/*
* This file is subject to the license agreement located in the file LICENSE
* and cannot be distributed without it. This notice cannot be
* removed or modified.
*/

#include "comp-platform/comp-platform-all.h"
#include "comp-platform/platform-binding-info.h"
#include "ocr-errors.h"
#include "debug.h"

const char * compplatform_types[] = {
#ifdef ENABLE_COMP_PLATFORM_PTHREAD
    "pthread",
#endif
#ifdef ENABLE_COMP_PLATFORM_FSIM
    "fsim",
#endif
    NULL,
};

ocrCompPlatformFactory_t *newCompPlatformFactory(compPlatformType_t type, ocrParamList_t *typeArg) {
    switch(type) {
#ifdef ENABLE_COMP_PLATFORM_PTHREAD
    case compPlatformPthread_id:
        return newCompPlatformFactoryPthread(typeArg);
#endif
#ifdef ENABLE_COMP_PLATFORM_FSIM
    case compPlatformFsim_id:
        return newCompPlatformFactoryFsim(typeArg);
#endif
    default:
        ASSERT(0);
        return NULL;
    };
}

void initializeCompPlatformOcr(ocrCompPlatformFactory_t * factory, ocrCompPlatform_t * self, ocrParamList_t *perInstance) {
    self->fcts = factory->platformFcts;
}

u8 getCompPlatformBindingInfo(ocrCompPlatform_t * platform, bindingInfo_t * bindingInfo) {
#ifdef ENABLE_COMP_PLATFORM_PTHREAD
    ocrCompPlatformPthread_t * pthreadPlatform = (ocrCompPlatformPthread_t *) platform;
    *bindingInfo = pthreadPlatform->bindingInfo;
    return 0;
#else
    return OCR_ENOTSUP;
#endif
}