/**
 * @brief OCR internal API to GUID management
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_GUID_END_H__
#define __OCR_GUID_END_H__

#if !defined(__GUID_END_MARKER__)
#error "Do not include ocr-guid-end.h yourself"
#endif

#include "debug.h"
#include "ocr-event.h"
#include "ocr-policy-domain.h"

#include "ocr-types.h"
#include "ocr-guid-functions.h"

/**
 *@brief utility function to get the location of a GUID
 */
static inline u8 guidLocation(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t guid,
                              ocrLocation_t* locationRes) {

    u8 returnCode = 0;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_INFO

    msg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = guid;
    PD_MSG_FIELD_I(properties) = LOCATION_GUIDPROP;
    returnCode = pd->fcts.processMessage(pd, &msg, true);

    if(returnCode == 0)
        *locationRes = PD_MSG_FIELD_O(location);

    return returnCode;
#undef PD_MSG
#undef PD_TYPE
}

static inline u8 guidKind(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t guid,
                          ocrGuidKind* kindRes) {

    u8 returnCode = 0;
    PD_MSG_STACK(msg);
    getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_INFO

    msg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid) = guid;
    PD_MSG_FIELD_I(properties) = KIND_GUIDPROP;
    returnCode = pd->fcts.processMessage(pd, &msg, true);

    if(returnCode == 0)
        *kindRes = PD_MSG_FIELD_O(kind);

    return returnCode;
#undef PD_MSG
#undef PD_TYPE
}


static inline u8 guidify(struct _ocrPolicyDomain_t * pd, u64 val,
                         ocrFatGuid_t * guidRes, ocrGuidKind kind) {
    u8 returnCode = 0;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);

    ocrAssert(ocrGuidIsNull(guidRes->guid) || ocrGuidIsUninitialized(guidRes->guid));

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_CREATE

    msg.type = PD_MSG_GUID_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = (void*)val;
    PD_MSG_FIELD_IO(guid.guid) = NULL_GUID;
    PD_MSG_FIELD_I(size) = 0;
    PD_MSG_FIELD_I(kind) = kind;
    PD_MSG_FIELD_I(targetLoc) = pd->myLocation;
    PD_MSG_FIELD_I(properties) = GUID_PROP_TORECORD;

    returnCode = pd->fcts.processMessage(pd, &msg, true);

    if(returnCode == 0) {
        *guidRes = PD_MSG_FIELD_IO(guid);
        ocrAssert((u64)(guidRes->metaDataPtr) == val);
    }
    return returnCode;
#undef PD_MSG
#undef PD_TYPE
}

static inline u8 deguidify(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t *res,
                           ocrGuidKind* kindRes) {

    u32 properties = 0;
    u8 returnCode = 0;
    if(kindRes)
        properties |= KIND_GUIDPROP;
    if(res->metaDataPtr == NULL)
        properties |= RMETA_GUIDPROP;

    if(properties) {
        PD_MSG_STACK(msg);
        getCurrentEnv(&pd, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_INFO
        msg.type = PD_MSG_GUID_INFO | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_IO(guid) = *res;
        PD_MSG_FIELD_I(properties) = properties;
        returnCode = pd->fcts.processMessage(pd, &msg, true);

        if(returnCode) {
            res->metaDataPtr = NULL;
            return returnCode;
        }

        if(!(res->metaDataPtr)) {
            res->metaDataPtr = PD_MSG_FIELD_IO(guid.metaDataPtr);
        }
        if(kindRes) {
            *kindRes = PD_MSG_FIELD_O(kind);
        }
#undef PD_MSG
#undef PD_TYPE
        return returnCode;
    }
    return 0;
}

static inline bool isDatablockGuid(ocrPolicyDomain_t *pd, ocrFatGuid_t guid) {
    if (ocrGuidIsNull(guid.guid)) {
        return false;
    }

    ocrGuidKind kind = OCR_GUID_NONE;
    if(guidKind(pd, guid, &kind) == 0)
        return kind == OCR_GUID_DB;
    return false;
}

static inline bool isEventGuid(ocrPolicyDomain_t *pd, ocrFatGuid_t guid) {
    if (ocrGuidIsNull(guid.guid)) {
        return false;
    }

    ocrGuidKind kind = OCR_GUID_NONE;
    if(guidKind(pd, guid, &kind) == 0)
        return (kind & OCR_GUID_EVENT);
    return false;
}

static inline bool isEdtGuid(ocrPolicyDomain_t *pd, ocrFatGuid_t guid) {
    if (ocrGuidIsNull(guid.guid)) {
        return false;
    }

    ocrGuidKind kind = OCR_GUID_NONE;
    if(guidKind(pd, guid, &kind) == 0)
        return kind == OCR_GUID_EDT;
    return false;
}

static inline ocrEventTypes_t eventType(ocrPolicyDomain_t *pd, ocrFatGuid_t guid) {
    if(!guid.metaDataPtr) {
        RESULT_ASSERT(deguidify(pd, &guid, NULL), ==, 0);
    }
    // We now have a R/O copy of the event
    return (((ocrEvent_t*)guid.metaDataPtr)->kind);
}

#endif /* __OCR_GUID_END_H__ */
