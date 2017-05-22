
#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_GASNET

#include <assert.h>

#include "ocr-policy-domain.h"

#include "splay-tree.h"

#include "gasnet-comm-platform.h"
#include "gasnet-policy-domain.h"


/*
 * @brief comparison function between policy domains
 *
 * this function is used by splay tree
 */
static int PD_compare(PDQueue_t *n1, PDQueue_t *n2) {
    return n1->platform - n2->platform;
}

// ---------------------------------
// splay tree declaration for policy domain
// ---------------------------------

// root for the splay tree
SPLAY_HEAD(PDQueueHead_s, PDQueue_s) root = SPLAY_INITIALIZER(root);

// prototype of the tree
SPLAY_PROTOTYPE(PDQueueHead_s, PDQueue_s, link, PD_compare);

// definition of the tree
SPLAY_GENERATE(PDQueueHead_s, PDQueue_s, link, PD_compare);

/*
 * @brief Get a comm platform from the database.
 */
static PDQueue_t* getPDQueue(ocrCommPlatformGasnet_t *self) {
    PDQueue_t item = {.platform = (ocrCommPlatformGasnet_t*)self, .counter=0};
    PDQueue_t *current = (struct PDQueue_s*) SPLAY_FIND(PDQueueHead_s, &root, &item);
    return current;
}


// ---------------------------------
// exported API
// ---------------------------------

/*
 * @brief Get either the current free or register policy domain
 *        or the "most" free policy domains.
 */
ocrCommPlatformGasnet_t * getCommPlatform() {
    PDQueue_t *current = SPLAY_ROOT(&root);
    ocrCommPlatformGasnet_t *platform = current->platform;
    ASSERT(platform != NULL);

    // it is tolearable to have a race condition for the counter since
    // it isn't really important at the moment
    current->counter = 0;  // mark that we'll use this platform

    return platform;
}

/*
 * @brief register a policy domain that is looking for a work
 *
 * This function will increase the counter of the PD to
 * mark that the PD is increasingly desperate for a work
 */
void pdLookingForWork(ocrCommPlatformGasnet_t* self) {
    // the PD was looking for a work, this means it is available
    // we accumulate the number of "visit" of this PD to track how
    // many the PD is waiting
    PDQueue_t *current = getPDQueue( self );
    current->counter++;
}

/*
 * @brief register a new policy domain
 */
void addCommPlatform(ocrCommPlatform_t * self) {
    ocrPolicyDomain_t *pd = self->pd;

    // store into the tree
    PDQueue_t * item = (PDQueue_t *) pd->fcts.pdMalloc(pd, sizeof(PDQueue_t));
    ADebug(AllocDebugAllPD, "gasnet-policy-domain.c/addCommPlatform() "
           "pdMalloc(%ld), addr=%p\n", sizeof(PDQueue_t), item);
    item->platform = (ocrCommPlatformGasnet_t*)self;
    item->counter  = 0;

    // make sure the PD is not in the database, otherwise it must be a bug
    RESULT_ASSERT((struct PDQueue_s*) SPLAY_FIND(PDQueueHead_s, &root, item), ==, NULL);

    // insert the current pd in the database
    SPLAY_INSERT(PDQueueHead_s, &root, item);
}

#endif /* ENABLE_COMM_PLATFORM_GASNET */
