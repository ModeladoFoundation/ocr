/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"

#ifdef ENABLE_EXTENSION_AFFINITY

#include "debug.h"

#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "experimental/ocr-platform-model.h"
#include "experimental/ocr-placer.h"
#include "extensions/ocr-affinity.h"

//
// Internal Placement API
//

//Part of policy-domain debug messages
#define DEBUG_TYPE POLICY

// Assumptions:
// - neighbors are locations described as ranks [0:N[
// - neighbors contains all ranks but self
// - placer's affinities array represents all the PD locations and is sorted by rank id

//
// Begin platform model based on affinities
//

//TODO defined in hc-dist-policy for now but should go away
#ifdef ENABLE_POLICY_DOMAIN_HC_DIST
extern u8 resolveRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t * fatGuid,
                                ocrPolicyMsg_t * msg, bool isBlocking);
#else
static u8 resolveRemoteMetaData(ocrPolicyDomain_t * pd, ocrFatGuid_t * fatGuid,
                                ocrPolicyMsg_t * msg, bool isBlocking) {
    u64 val;
    u8 res __attribute__((unused)) = pd->guidProviders[0]->fcts.getVal(pd->guidProviders[0], fatGuid->guid, &val, NULL, MD_LOCAL, NULL);
    ASSERT(val != 0);
    ASSERT(res == 0);
    fatGuid->metaDataPtr = (void *) val;
    return 0;
}
#endif

u8 affinityToLocation(ocrLocation_t* result, ocrGuid_t affinityGuid) {
    ocrFatGuid_t fguid;
    fguid.guid = affinityGuid;
    ocrPolicyDomain_t * pd;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    resolveRemoteMetaData(pd, &fguid, NULL, true);
    ASSERT((fguid.metaDataPtr != NULL) && "ERROR: cannot deguidify affinity GUID");
    *result = ((ocrAffinity_t *) fguid.metaDataPtr)->place;
    return 0;
}


ocrPlatformModel_t * createPlatformModelAffinity(ocrPolicyDomain_t *pd) {
    ocrPlatformModelAffinity_t * model = pd->fcts.pdMalloc(pd, sizeof(ocrPlatformModelAffinity_t));
    u64 countAff = pd->neighborCount + 1;
    model->pdLocAffinities = NULL;
    model->pdLocAffinitiesSize = countAff;
    model->pdLocAffinities = pd->fcts.pdMalloc(pd, sizeof(ocrGuid_t)*countAff);
    // Returns an array of affinity where each affinity maps to a PD.
    // The array is ordered by PD's location (rank in mpi/gasnet)
    u64 i=0;
    for(i=0; i < pd->neighborCount; i++) {
        ASSERT(pd->neighbors[i] < countAff);
        ocrFatGuid_t fguid;
        pd->guidProviders[0]->fcts.createGuid(pd->guidProviders[0], &fguid, sizeof(ocrAffinity_t), OCR_GUID_AFFINITY, pd->myLocation, GUID_PROP_NONE);
        ((ocrAffinity_t*)fguid.metaDataPtr)->place = pd->neighbors[i];
        model->pdLocAffinities[pd->neighbors[i]] = fguid.guid;
    }
    // Do current PD
    model->current = (u32)pd->myLocation;
    ocrFatGuid_t fguid;
    pd->guidProviders[0]->fcts.createGuid(pd->guidProviders[0], &fguid, sizeof(ocrAffinity_t), OCR_GUID_AFFINITY, pd->myLocation, GUID_PROP_NONE);
    ((ocrAffinity_t*)fguid.metaDataPtr)->place = pd->myLocation;
    model->pdLocAffinities[model->current] = fguid.guid;

    for(i=0; i < countAff; i++) {
        DPRINTF(DEBUG_LVL_VVERB,"affinityGuid[%"PRId32"]="GUIDF"\n",
                (u32)i, GUIDA(model->pdLocAffinities[i]));
    }

    return (ocrPlatformModel_t *) model;
}


void destroyPlatformModelAffinity(ocrPolicyDomain_t *pd) {
    ocrPlatformModelAffinity_t * model = (ocrPlatformModelAffinity_t *) (pd->platformModel);
    u64 i=0;
    PD_MSG_STACK(msg);
    getCurrentEnv(NULL, NULL, NULL, &msg);
    for(i=0; i < pd->neighborCount; i++) {
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
      msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
      PD_MSG_FIELD_I(guid.guid) = model->pdLocAffinities[pd->neighbors[i]];
      PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
      PD_MSG_FIELD_I(properties) = 1; // Free metadata
      pd->fcts.processMessage(pd, &msg, false);
#undef PD_MSG
#undef PD_TYPE
    }

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
      msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
      PD_MSG_FIELD_I(guid.guid) = model->pdLocAffinities[model->current];
      PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
      PD_MSG_FIELD_I(properties) = 1; // Free metadata
      pd->fcts.processMessage(pd, &msg, false);
#undef PD_MSG
#undef PD_TYPE

    pd->fcts.pdFree(pd, model->pdLocAffinities);
    pd->fcts.pdFree(pd, model);
    pd->platformModel = NULL;
}

//
// End platform model based on affinities
//

#else

#include "debug.h"

#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "experimental/ocr-platform-model.h"
#include "experimental/ocr-placer.h"
#include "extensions/ocr-affinity.h"

// Stubs
u8 affinityToLocation(ocrLocation_t* result, ocrGuid_t affinityGuid) {

    return OCR_ENOTSUP;
}

#endif /* ENABLE_EXTENSION_AFFINITY */
