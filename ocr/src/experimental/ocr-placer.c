/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"

#ifdef ENABLE_EXTENSION_AFFINITY

#include "debug.h"

#include "extensions/ocr-hints.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "experimental/ocr-platform-model.h"
#include "experimental/ocr-placer.h"
#include "extensions/ocr-affinity.h"

//
// Internal Placement API
//

//BUG #476 - This code is being deprecated

//Part of policy-domain debug messages
#define DEBUG_TYPE POLICY

// Location Placer Implementation
// NOTE: This implementation is not the fruit of any thinking
//
// Assumptions:
// - neighbors are locations described as ranks [0:N[
// - neighbors contains all ranks but self
// - placer's affinities array represents all the PD locations and is sorted by rank id

ocrPlacer_t * createLocationPlacer(ocrPolicyDomain_t *pd) {
    ocrLocationPlacer_t * placer = pd->fcts.pdMalloc(pd, sizeof(ocrLocationPlacer_t));
    ADebug(AllocDebugAllPD, "ocr-placer.c/createLocationPlacer() "
           "pdMalloc(%ld), addr=%p\n", sizeof(ocrLocationPlacer_t), placer);
    placer->lock = INIT_LOCK;
    placer->edtLastPlacementIndex = 0;
    return (ocrPlacer_t *) placer;
}

void destroyLocationPlacer(ocrPolicyDomain_t *pd) {
    pd->fcts.pdFree(pd, pd->placer);
    // Necessary for the PD implementation to avoid placement
    pd->placer = NULL;
}

u8 suggestLocationPlacement(ocrPolicyDomain_t *pd, ocrLocation_t curLoc, ocrPlatformModelAffinity_t * model, ocrLocationPlacer_t * placer, ocrPolicyMsg_t * msg) {
    u64 msgType = (msg->type & PD_MSG_TYPE_ONLY);
    // Incoming messages have been directed to the current location. Don't try to place them again.

    // Only try to place message the current policy domain is generating (i.e. from executing user code)
    if ((msg->srcLocation == curLoc) && (msg->destLocation == curLoc) && (model != NULL) && (model->pdLocAffinities != NULL)) {
        // Check if we need to place the DB/EDTs
        bool doAutoPlace = false;
        switch(msgType) {
            case PD_MSG_WORK_CREATE:
            {
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                if (PD_MSG_FIELD_I(workType) == EDT_USER_WORKTYPE) {
                    doAutoPlace = true;
                    if (PD_MSG_FIELD_I(hint) != NULL_HINT) {
                        // Don't auto-place and extract the affinity GUID from the hings
                        ocrHint_t *hint = PD_MSG_FIELD_I(hint);
                        u64 hintValue = 0ULL;
                        if ((ocrGetHintValue(hint, OCR_HINT_EDT_AFFINITY, &hintValue) == 0) && (hintValue != 0)) {
                            ocrGuid_t affGuid;
    #if GUID_BIT_COUNT == 64
                            affGuid.guid = hintValue;
    #elif GUID_BIT_COUNT == 128
                            affGuid.upper = 0ULL;
                            affGuid.lower = hintValue;
    #endif
                            ASSERT(!ocrGuidIsNull(affGuid));
                            affinityToLocation(&(msg->destLocation), affGuid);
                            doAutoPlace = false;
                        }
                    }
                } else { // For runtime EDTs, always local
                    doAutoPlace = false;
                }
#undef PD_MSG
#undef PD_TYPE
            break;
            }
            case PD_MSG_DB_CREATE:
            {
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
                // For now a DB is always created where the current EDT executes unless
                // it has an affinity specified (i.e. no auto-placement)
                if (PD_MSG_FIELD_I(hint) != NULL_HINT) {
                    ocrHint_t *hint = PD_MSG_FIELD_I(hint);
                    u64 hintValue = 0ULL;
                    if ((ocrGetHintValue(hint, OCR_HINT_DB_AFFINITY, &hintValue) == 0) && (hintValue != 0)) {
                        ocrGuid_t affGuid;
#if GUID_BIT_COUNT == 64
                        affGuid.guid = hintValue;
#elif GUID_BIT_COUNT == 128
                        affGuid.upper = 0ULL;
                        affGuid.lower = hintValue;
#endif
                        ASSERT(!ocrGuidIsNull(affGuid));
                        affinityToLocation(&(msg->destLocation), affGuid);
                        return 0;
                    }
                }
#undef PD_MSG
#undef PD_TYPE
            break;
            }
            default:
                // Fall-through
            break;
        }
        // Auto placement
        if (doAutoPlace) {
            hal_lock(&(placer->lock));
            u32 placementIndex = placer->edtLastPlacementIndex;
            ocrGuid_t pdLocAffinity = model->pdLocAffinities[placementIndex];
            placer->edtLastPlacementIndex++;
            if (placer->edtLastPlacementIndex == model->pdLocAffinitiesSize) {
                placer->edtLastPlacementIndex = 0;
            }
            hal_unlock(&(placer->lock));
            affinityToLocation(&(msg->destLocation), pdLocAffinity);
            DPRINTF(DEBUG_LVL_VVERB,"Auto-Placement for msg %p, type 0x%"PRIx64", at location %"PRId32"\n",
                    msg, msgType, (u32)placementIndex);
        }
    }
    return 0;
}

#endif /* ENABLE_EXTENSION_AFFINITY */
