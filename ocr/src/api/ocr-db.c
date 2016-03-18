/**
* @brief Data-block implementation for OCR
*/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "debug.h"
#include "ocr-allocator.h"
#include "ocr-datablock.h"
#include "ocr-db.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#include "utils/profiler/profiler.h"

#define DEBUG_TYPE API

u8 ocrDbCreate(ocrGuid_t *db, void** addr, u64 len, u16 flags,
               ocrHint_t *hint, ocrInDbAllocator_t allocator) {

    START_PROFILE(api_ocrDbCreate);
    DPRINTF(DEBUG_LVL_INFO, "ENTER ocrDbCreate(*guid="GUIDF", len=%"PRIu64", flags=%"PRIu32""
            ", hint=%p, alloc=%"PRIu32")\n", GUIDA(*db), len, (u32)flags, hint, (u32)allocator);
    PD_MSG_STACK(msg);
    ocrPolicyDomain_t *policy = NULL;
    ocrTask_t *task = NULL;
    u8 returnCode = 0;
    getCurrentEnv(&policy, NULL, &task, &msg);
    //Copy the hints so that the runtime modifications
    //are not reflected back to the user
    ocrHint_t userHint;
    if (hint != NULL_HINT) {
        userHint = *hint;
        hint = &userHint;
    }

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_CREATE
    msg.type = PD_MSG_DB_CREATE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = *db;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_IO(properties) = (u32) flags;
    PD_MSG_FIELD_IO(size) = len;
    PD_MSG_FIELD_I(edt.guid) = task?task->guid:NULL_GUID; // Can happen when non EDT creates the DB
    PD_MSG_FIELD_I(edt.metaDataPtr) = task;
    PD_MSG_FIELD_I(hint) = hint;
    PD_MSG_FIELD_I(dbType) = USER_DBTYPE;
    PD_MSG_FIELD_I(allocator) = allocator;
#ifdef ENABLE_OCR_API_DEFERRABLE
    tagDeferredMsg(&msg, task);
#endif
    returnCode = policy->fcts.processMessage(policy, &msg, true);

    if(returnCode == 0) {
        returnCode = PD_MSG_FIELD_O(returnDetail);
        if(returnCode == 0) {
            *db = PD_MSG_FIELD_IO(guid.guid);
            *addr = PD_MSG_FIELD_O(ptr);
        } else {
            if(returnCode != OCR_EGUIDEXISTS) {
                *db = NULL_GUID;
            }
            // Addr is always NULL unless we were successful in creating it
            *addr = NULL;
        }
    }
#undef PD_MSG
#undef PD_TYPE

#ifdef ENABLE_EXTENSION_PERF
    if(*addr && task)
        task->swPerfCtrs[PERF_DB_CREATES - PERF_HW_MAX] += len;
#endif

    if((!(flags & DB_PROP_NO_ACQUIRE)) &&  task && (returnCode == 0)) {
        // Here we inform the task that we created a DB
        // This is most likely ALWAYS a local message but let's leave the
        // API as it is for now. It is possible that the EDTs move at some point so
        // just to be safe
        //TODO-DEFERRED: Orthogonal but is this assuming too much of the implementation ?
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_DYNADD
        getCurrentEnv(NULL, NULL, NULL, &msg);
        msg.type = PD_MSG_DEP_DYNADD | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(edt.guid) = task->guid;
        PD_MSG_FIELD_I(edt.metaDataPtr) = task;
        PD_MSG_FIELD_I(db.guid) = *db;
        PD_MSG_FIELD_I(db.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(properties) = 0;
#ifdef ENABLE_OCR_API_DEFERRABLE
        tagDeferredMsg(&msg, task);
#endif
        returnCode = policy->fcts.processMessage(policy, &msg, false);
        if(returnCode != 0) {
            DPRINTF(DEBUG_LVL_WARN, "EXIT ocrDbCreate -> %"PRIu32"; Issue registering datablock\n", returnCode);
            RETURN_PROFILE(returnCode);
        }
#undef PD_MSG
#undef PD_TYPE
    } else {
        if(!(flags & (DB_PROP_IGNORE_WARN | DB_PROP_NO_ACQUIRE)) && (returnCode == 0)) {
            DPRINTF(DEBUG_LVL_WARN, "Acquiring DB (GUID: "GUIDF") from outside an EDT ... auto-release will fail\n",
                    GUIDA(*db));
        }
    }
    DPRINTF_COND_LVL(((returnCode != 0) && (returnCode != OCR_EGUIDEXISTS)), DEBUG_LVL_WARN, DEBUG_LVL_INFO,
                     "EXIT ocrDbCreate -> %"PRIu32"; GUID: "GUIDF"; ADDR: %p size: %"PRIu64"\n",
                     returnCode, GUIDA(*db), *addr, len);
    RETURN_PROFILE(returnCode);
}

u8 ocrDbDestroy(ocrGuid_t db) {

    START_PROFILE(api_ocrDbDestroy);
    DPRINTF(DEBUG_LVL_INFO, "ENTER ocrDbDestroy(guid="GUIDF")\n", GUIDA(db));
    PD_MSG_STACK(msg);
    ocrPolicyDomain_t *policy = NULL;
    ocrTask_t *task = NULL;
    getCurrentEnv(&policy, NULL, &task, &msg);
    u8 returnCode = OCR_ECANCELED;
    bool dynRemoved = false;
    if(task) {
        // Here we inform the task that we are going to destroy the DB
        // We check the returnDetail of the operation to find out if the task
        // was using the datablock or not.
        // This is most likely ALWAYS a local message but let's leave the
        // API as it is for now. It is possible that the EDTs move at some point so
        // just to be safe
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        getCurrentEnv(NULL, NULL, NULL, &msg);
        msg.type = PD_MSG_DEP_DYNREMOVE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_I(edt.guid) = task->guid;
        PD_MSG_FIELD_I(edt.metaDataPtr) = task;
        PD_MSG_FIELD_I(db.guid) = db;
        PD_MSG_FIELD_I(db.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(properties) = 0;
#ifdef ENABLE_OCR_API_DEFERRABLE
        tagDeferredMsg(&msg, task);
#endif
        returnCode = policy->fcts.processMessage(policy, &msg, true);
        if(returnCode != 0) {
            DPRINTF(DEBUG_LVL_WARN, "Destroying DB (GUID: "GUIDF") -> %"PRIu32"; Issue unregistering the datablock\n", GUIDA(db), returnCode);
        }
        // If dynRemoved is true, it means the task was using the data-block and we will therefore
        // need to remove it automatically. Otherwise, we won't need to release it
        dynRemoved = (PD_MSG_FIELD_O(returnDetail)==0);
#undef PD_MSG
#undef PD_TYPE
    } else {
        DPRINTF(DEBUG_LVL_WARN, "Destroying DB (GUID: "GUIDF") from outside an EDT ... auto-release will fail\n", GUIDA(db));
    }
    // !task is to allow the legacy interface to destroy a datablock outside of an EDT
    if ((!task) || (returnCode == 0)) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_FREE
        msg.type = PD_MSG_DB_FREE | PD_MSG_REQUEST;
        PD_MSG_FIELD_I(guid.guid) = db;
        PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(srcLoc) = policy->myLocation;
        PD_MSG_FIELD_I(edt.guid) = task?task->guid:NULL_GUID;
        PD_MSG_FIELD_I(edt.metaDataPtr) = task;
        // Tell whether or not the task was using the DB. This is useful
        // to know if the DB actually needs to be released or not.
        // If dynRemoved is true, we will release the data-block. Otherwise, we won't
        PD_MSG_FIELD_I(properties) = dynRemoved ? 0 : DB_PROP_NO_RELEASE;
#ifdef ENABLE_OCR_API_DEFERRABLE
        tagDeferredMsg(&msg, task);
#endif
        returnCode = policy->fcts.processMessage(policy, &msg, false);
        if(returnCode == 0)
            returnCode = PD_MSG_FIELD_O(returnDetail);
#undef PD_MSG
#undef PD_TYPE
    } else {
        DPRINTF(DEBUG_LVL_WARN, "Destroying DB (GUID: "GUIDF") Issue destroying the datablock\n", GUIDA(db));
    }

    DPRINTF_COND_LVL(returnCode, DEBUG_LVL_WARN, DEBUG_LVL_INFO,
                     "EXIT ocrDbDestroy(guid="GUIDF") -> %"PRIu32"\n", GUIDA(db), returnCode);
    RETURN_PROFILE(returnCode);
}

u8 ocrDbRelease(ocrGuid_t db) {

    START_PROFILE(api_ocrDbRelease);
    DPRINTF(DEBUG_LVL_INFO, "ENTER ocrDbRelease(guid="GUIDF")\n", GUIDA(db));
    PD_MSG_STACK(msg);
    ocrPolicyDomain_t *policy = NULL;
    ocrTask_t *task = NULL;
    getCurrentEnv(&policy, NULL, &task, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DB_RELEASE
    msg.type = PD_MSG_DB_RELEASE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
    PD_MSG_FIELD_IO(guid.guid) = db;
    PD_MSG_FIELD_IO(guid.metaDataPtr) = NULL;
    PD_MSG_FIELD_I(srcLoc) = policy->myLocation;
    PD_MSG_FIELD_I(edt.guid) = task?task->guid:NULL_GUID;
    PD_MSG_FIELD_I(edt.metaDataPtr) = task;
    PD_MSG_FIELD_I(ptr) = NULL;
    PD_MSG_FIELD_I(size) = 0;
    PD_MSG_FIELD_I(properties) = 0;
#ifdef ENABLE_OCR_API_DEFERRABLE
    tagDeferredMsg(&msg, task);
#endif
    u8 returnCode = policy->fcts.processMessage(policy, &msg, true);
    if(returnCode == 0) {
        returnCode = PD_MSG_FIELD_O(returnDetail);
    }
#undef PD_MSG
#undef PD_TYPE

    if(task && (returnCode == 0)) {
        // Here we inform the task that we released a DB
        // This is most likely ALWAYS a local message but let's leave the
        // API as it is for now. It is possible that the EDTs move at some point so
        // just to be safe
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        getCurrentEnv(NULL, NULL, NULL, &msg);
        msg.type = PD_MSG_DEP_DYNREMOVE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD_I(edt.guid) = task->guid;
        PD_MSG_FIELD_I(edt.metaDataPtr) = task;
        PD_MSG_FIELD_I(db.guid) = db;
        PD_MSG_FIELD_I(db.metaDataPtr) = NULL;
        PD_MSG_FIELD_I(properties) = 0;
#ifdef ENABLE_OCR_API_DEFERRABLE
        tagDeferredMsg(&msg, task);
#endif
        returnCode = policy->fcts.processMessage(policy, &msg, true);
        if (returnCode != 0) {
            DPRINTF(DEBUG_LVL_WARN, "Releasing DB  -> %"PRIu32"; Issue unregistering DB datablock\n", returnCode);
        }
#undef PD_MSG
#undef PD_TYPE
    } else {
        if (returnCode == 0) {
            DPRINTF(DEBUG_LVL_WARN, "Releasing DB (GUID: "GUIDF") from outside an EDT ... auto-release will fail\n", GUIDA(db));
        }
    }

    DPRINTF_COND_LVL(returnCode, DEBUG_LVL_WARN, DEBUG_LVL_INFO,
                     "EXIT ocrDbRelease(guid="GUIDF") -> %"PRIu32"\n", GUIDA(db), returnCode);
    RETURN_PROFILE(returnCode);
}

u8 ocrDbMalloc(ocrGuid_t guid, u64 size, void** addr) {
    return OCR_EINVAL; /* not yet implemented */
}

u8 ocrDbMallocOffset(ocrGuid_t guid, u64 size, u64* offset) {
    return OCR_EINVAL; /* not yet implemented */
}

u8 ocrDbCopy(ocrGuid_t destination, u64 destinationOffset, ocrGuid_t source,
             u64 sourceOffset, u64 size, u64 copyType, ocrGuid_t *completionEvt) {
    return OCR_EINVAL; /* not yet implemented */
}

u8 ocrDbFree(ocrGuid_t guid, void* addr) {
    return OCR_EINVAL; /* not yet implemented */
}

u8 ocrDbFreeOffset(ocrGuid_t guid, u64 offset) {
    return OCR_EINVAL; /* not yet implemented */
}
