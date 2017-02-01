/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-task.h"
#include "utils/ocr-utils.h"
#include "ocr-policy-domain-tasks.h"

/* Use a separate type of debug-type just for micro-tasks */
#define DEBUG_TYPE MICROTASKS

/***************************************/
/********** Internal functions *********/
/***************************************/

// Macros to help with code factoring
#define CHECK_MALLOC(expr, cleanup) do {    \
    if((expr) == NULL) {            \
        DPRINTF(DEBUG_LVL_WARN, "Cannot allocate memory for \"" #expr "\"\n");   \
        toReturn = OCR_ENOMEM;      \
        cleanup;                    \
        ASSERT(false);              \
        goto _END_FUNC;             \
    }                               \
} while(0);

// WARNING: CHECK_RESULT looks for the result to be 0. So it works the opposite
// of an assert. If result is 0, all will be good, otherwise it will error out.
// This is meant to check the return value of functions
#define CHECK_RESULT(expr, cleanup, newcode) do {   \
    if((expr) != 0) {               \
        DPRINTF(DEBUG_LVL_WARN, "Error in check \"" #expr "\" on line %"PRIu32"; aborting\n", __LINE__);   \
        cleanup;                    \
        newcode;                    \
        ASSERT(false);              \
        goto _END_FUNC;             \
    }                               \
} while(0);

#define CHECK_RESULT_T(expr, cleanup, newcode) CHECK_RESULT(!(expr), cleanup, newcode)

#define RETURN goto _END_FUNC;

#define END_LABEL(label) label: __attribute__((unused));


#define PROPAGATE_UP_TREE(__node, __parent, __npidx, __cond, __actions) do { \
    while (((__parent) != NULL) && (__cond)) {                       \
        hal_lock(&((__parent)->lock));                               \
        DPRINTF(DEBUG_LVL_VVERB, "BEFORE: %p -> %p [%"PRIu32"] "     \
                "curNode: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"], " \
                "parent: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"]\n", \
                (__node), (__parent), (__node)->parentSlot,          \
                (__npidx),                                           \
                (__node)->nodeFree, (__node)->nodeNeedsProcess[__npidx], \
                (__node)->nodeReady, (__npidx), (__parent)->nodeFree,\
                (__parent)->nodeNeedsProcess[__npidx], (__parent)->nodeReady); \
        __actions;                                                   \
        DPRINTF(DEBUG_LVL_VVERB, "AFTER:  %p -> %p [%"PRIu32"] "     \
                "curNode: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"], " \
                "parent: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"]\n", \
                (__node), (__parent), (__node)->parentSlot,          \
                (__npidx),                                           \
                (__node)->nodeFree, (__node)->nodeNeedsProcess[__npidx], \
                (__node)->nodeReady, (__npidx), (__parent)->nodeFree,\
                (__parent)->nodeNeedsProcess[__npidx], (__parent)->nodeReady); \
        hal_unlock(&((__node)->lock));                               \
        __node = __parent;                                           \
        __parent = (__node)->parent;                                 \
    }                                                                \
    /* Unlock the final one */                                       \
    hal_unlock(&((__node)->lock));                                   \
    } while(0);

#define PROPAGATE_UP_TREE_CHECK_LOCK(__node, __parent, __npidx, __cond, __actions) do { \
    bool _releaseLock = hal_islocked((__node)->lock);                   \
    while (((__parent) != NULL) && (__cond)) {                          \
        hal_lock(&((__parent)->lock));                                  \
        DPRINTF(DEBUG_LVL_VVERB, "BEFORE: %p -> %p [%"PRIu32"] "        \
                "curNode: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"], " \
                "parent: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"]\n", \
                (__node), (__parent), (__node)->parentSlot,             \
                (__npidx),                                              \
                (__node)->nodeFree, (__node)->nodeNeedsProcess[__npidx],\
                (__node)->nodeReady, (__npidx), (__parent)->nodeFree,   \
                (__parent)->nodeNeedsProcess[__npidx], (__parent)->nodeReady); \
        __actions;                                                      \
        DPRINTF(DEBUG_LVL_VVERB, "AFTER: %p -> %p [%"PRIu32"] "         \
                "curNode: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"], " \
                "parent: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"]\n", \
                (__node), (__parent), (__node)->parentSlot,             \
                (__npidx),                                              \
                (__node)->nodeFree, (__node)->nodeNeedsProcess[__npidx],\
                (__node)->nodeReady, (__npidx), (__parent)->nodeFree,   \
                (__parent)->nodeNeedsProcess[__npidx], (__parent)->nodeReady); \
        if(_releaseLock) hal_unlock(&((__node)->lock));                 \
        _releaseLock = true; /* Always lock/unlock up the chain */      \
        __node = __parent;                                              \
        __parent = (__node)->parent;                                    \
    }                                                                   \
    /* Unlock the final one (which can be the first one */              \
    if(_releaseLock) hal_unlock(&((__node)->lock));                     \
    } while(0);

#define PROPAGATE_UP_TREE_NO_UNLOCK(__node, __parent, __npidx, __cond, __actions) do { \
    bool _releaseLock = false;                                          \
    while (((__parent) != NULL) && (__cond)) {                          \
        hal_lock(&((__parent)->lock));                                  \
        DPRINTF(DEBUG_LVL_VVERB, "BEFORE: %p -> %p [%"PRIu32"] "        \
                "curNode: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"], " \
                "parent: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"]\n", \
                (__node), (__parent), (__node)->parentSlot,             \
                (__npidx),                                              \
                (__node)->nodeFree, (__node)->nodeNeedsProcess[__npidx],\
                (__node)->nodeReady, (__npidx), (__parent)->nodeFree,   \
                (__parent)->nodeNeedsProcess[__npidx], (__parent)->nodeReady); \
        __actions;                                                      \
        DPRINTF(DEBUG_LVL_VVERB, "AFTER: %p -> %p [%"PRIu32"] "         \
                "curNode: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"], " \
                "parent: [F;NP[%"PRIu32"];R: 0x%"PRIx64"; 0x%"PRIx64"; 0x%"PRIx64"]\n", \
                (__node), (__parent), (__node)->parentSlot,             \
                (__npidx),                                              \
                (__node)->nodeFree, (__node)->nodeNeedsProcess[__npidx],\
                (__node)->nodeReady, (__npidx), (__parent)->nodeFree,   \
                (__parent)->nodeNeedsProcess[__npidx], (__parent)->nodeReady); \
        if(_releaseLock) hal_unlock(&((__node)->lock));                 \
        _releaseLock = true; /* Always lock/unlock up the chain */      \
        __node = __parent;                                              \
        __parent = (__node)->parent;                                    \
    }                                                                   \
    /* Unlock the final one (which can be the first one) */             \
    if(_releaseLock) hal_unlock(&((__node)->lock));                     \
    } while(0);

// Size of the bit vector we use
#define BV_SIZE 64
#define BV_SIZE_LOG2 6

#define ctz(val) ctz64(val)

// ----- Convenience macros -----

/**< Boolean indicating if a node is a leaf node with
     strands */
#define IS_LEAF_NODE(_idx) ((_idx) & 0x1)

/**< Set the fact that the node is a leaf node */
#define SET_LEAF_NODE(_idx) do { (_idx) |= 0x1; } while(0)

/**< Index value of the left-most strand in the node (either direct
     or as a child of this current node) */
#define LEAF_LEFTMOST_IDX(_idx) ((_idx) >> 1)

/**< Returns the strand table of a "fake" event pointer */
#define EVT_DECODE_ST_TBL(evt) ((evt) & 0x7)
/**< Returns the strand table index of a "fake" event pointer */
#define EVT_DECODE_ST_IDX(evt) ((evt) >> 3)

/**< Encodes an event (reverse of EVT_DECODE) */
#define EVT_ENCODE(sttbl, stidx) (((sttbl) & 0x7) | ((stidx)<<3))


// ----- Action related functions -----

/**
 * @brief Gets the NP_* value for a given action.
 *
 * This can be extracted from the properties flag for regular
 * actions but special care must be used with actions that
 * are directly encoded in the 64bits of the action value
 *
 * @param[in] action        Action to use
 * @return a NP_* value that corresponds to this action
 */
static void _pdActionToNP(u8 *npIdx, pdAction_t* action);

/**
 * @brief Processes and acts on a given action
 *
 * This call will process the action 'action'
 *
 * @param[in] pd            Policy domain to use
 * @param[in] worker        Worker doing the processing
 * @param[in] strand        Strand this action is associated with
 * @param[in] action        Action to process
 * @param[in] properties    Properties (unused for now)
 * @return a status code:
 *    - 0: all went well
 *    - OCR_EINVAL: invalid value for action or properties
 */
static u8 _pdProcessAction(ocrPolicyDomain_t *pd, ocrWorker_t *worker, pdStrand_t *strand,
                           pdAction_t* action, u32 properties);


// ----- Strand related functions -----

/**
 * @brief Destroys (and frees) a table node
 *
 * This function goes down the subtree freeing everything it can
 *
 * @warn This function expects the node to be totally free
 *
 * @param[in] pd        Policy domain to use
 * @param[in] node      Node to destroy
 *
 * @return a status code:
 *     - 0 on success
 *     - OCR_EINVAL if node is NULL
 */
static u8 _pdDestroyStrandTableNode(ocrPolicyDomain_t *pd, pdStrandTableNode_t *node);


#define BLOCK               0x1
/**
 * @brief Attempts to grab a lock on a strand
 *
 * If 'BLOCK' is set, this call will block until the lock can be acquired.
 * Otherwise, the call will attempt once to grab the lock (if it is free) and return
 * if it cannot
 *
 * @param[in] strand        Strand to grab the lock on
 * @param[in] properties    0 or BLOCK
 * @return a status code:
 *     - 0: the lock was acquired
 *     - OCR_EBUSY: the lock could not be acquired (properties does not have BLOCK)
 */
static u8 _pdLockStrand(pdStrand_t *strand, u32 properties);

static inline u8 _pdUnlockStrand(pdStrand_t *strand) {
    if(strand->processingWorker) {
        ocrWorker_t *worker = NULL;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        if(worker == strand->processingWorker) return 0;
    }
    hal_unlock(&(strand->lock));
    return 0;
}

/**
 * @brief Helper function to enqueue actions on a strand's queue
 *
 * This queue may be either the regular actions queue or the bufferedActions one
 * @details [long description]
 *
 * @param[in] pd            Policy domain
 * @param[in/out] queue     Queue to use
 * @param[in] count         Number of actions
 * @param[in] actions       Actions to enqueue
 * @return a status code:
 *      - 0: success
 *      - OCR_ENOMEM: Unable to allocate the queue
 *      - OCR_EFAULT: Other runtime fault
 */
static inline u8 _pdDoEnqueue(ocrPolicyDomain_t *pd, arrayDeque_t **queue, u32 count, pdAction_t** actions) {
#define _END_FUNC doEnqueueEnd
    u8 toReturn = 0;
    if(*queue == NULL) {
        // Create and initialize the actions strand
        CHECK_MALLOC(*queue = (arrayDeque_t*)pd->fcts.pdMalloc(pd, sizeof(arrayDeque_t)), );
        CHECK_RESULT(arrayDequeInit(*queue, PDST_ACTION_COUNT),
                     pd->fcts.pdFree(pd, *queue), toReturn = OCR_EFAULT);
        DPRINTF(DEBUG_LVL_VERB, "Created actions structure @ %p\n", *queue);
    }

    DPRINTF(DEBUG_LVL_VERB, "Going to enqueue %"PRIu32" actions on %p\n",
            count, *queue);
    // At this point, we can enqueue things on the deque
    while(count > 0) {
        DPRINTF(DEBUG_LVL_VVERB, "Pushing action %p\n", *actions);
        arrayDequePushAtTail(*queue, (void*)*actions);
        --count;
        ++actions;
    }

END_LABEL(doEnqueueEnd)
    return toReturn;
#undef _END_FUNC
}

/**
 * @brief Re-queues actions from the bufferedActions array to the regular actions array
 * @details [long description]
 *
 * @param[in] pd        Policy domain
 * @param[in] strand    Strand for which to requeue actions
 *
 * @return a status code:
 *      - 0: success
 *      - Other codes returned by pdEnqueueActions()
 */
static u8 _pdDoRequeue(ocrPolicyDomain_t *pd, pdStrand_t *strand);


#define IS_STRAND        0x1 /**< The node is a strand node */
#define IS_LEAF          0x2 /**< The node is a leaf node */
/**
 * @brief Initialize a strand table node
 *
 * This function initializes a strand table node setting its parent, initializing
 * it to be fully empty and creating the sub-nodes/leafs if numChildrenToInit is non
 * zero. This only applies to leaf nodes. This MUST be zero for non-leaf nodes.
 *
 * @param[in] pd                    Policy domain to use (can be NULL and will be resolved)
 * @param[in] node                  Node to initialize
 * @param[in] parent                Parent of the node or NULL if no parent
 * @param[in] parentSlot            Slot in the parent (ignored if parent is NULL)
 * @param[in] rdepth                Reverse depth of node. Is 0 for leaf nodes and goes up
 *                                  from there
  *                                  the leaf strand. Otherwise, must be 0
 * @param[in] numChildrenToInit     For leaf nodes, create this number of strands.
 *                                  Otherwise, must be 0
 * @param[in] flags                 Flags for creation: IS_LEAF
 *
 * @note This function does not grab any locks but requires a lock on node or exclusive
 * access to it
 *
 * @return a status code:
 *      - 0: successful
 *      - OCR_ENOMEM: insufficient memory
 *      - OCR_EINVAL: invalid numChildrenToInit or lock not held on parent
 */
static u8 _pdInitializeStrandTableNode(ocrPolicyDomain_t *pd, pdStrandTable_t *table, pdStrandTableNode_t *node,
                                       pdStrandTableNode_t *parent, u32 parentSlot,
                                       u32 level, u32 numChildrenToInit, u8 flags);


/**
 * @brief Sets the child of a strand node
 *
 * Sets the child tablenode or strand for a tablenode. There should be no
 * existing child at that index.
 *
 * @param[in] pd                    Policy domain
 * @param[in] parent                Parent to modify
 * @param[in] idx                   Index in the parent
 * @param[in] child                 Either a pdStrand_t or pdStrandTableNode_t
 * @param[in] flags                 Flags (IS_STRAND if the child is a strand)
 *
 * @note This function does not grab any locks but requires a lock on both the
 * parent and child (or ensure non-concurrency of accesses to parent and child)
 *
 * @return a status code:
 *      - 0: successful
 *      - OCR_EACCES: a child already exists at that index
 *      - OCR_EINVAL: child has the wrong parent
 */
static u8 _pdSetStrandNodeAtIdx(ocrPolicyDomain_t *pd, pdStrandTableNode_t *parent,
                                u32 idx, void* child, u8 flags);


/**
 * @brief Frees the strand at index 'index' in 'table'
 *
 * The strand must be locked prior to calling this function.
 *
 * @param[in] pd        Policy domain to use. Must not be NULL
 * @param[in] index     Strand to free
 * @return a status code:
 *      - 0: successful
 *      - OCR_EINVAL: index points to an invalid strand or strand is not locked
 */
static u8 _pdDestroyStrand(ocrPolicyDomain_t* pd, pdStrand_t *strand);


/**
 * @brief Processes a certain number of strands
 *
 * See pdProcessStrands() for detail on the user-facing function. This
 * one just adds a maximum count of strands to process
 *
 *
 * @param[in] pd            Policy domain to use
 * @param[in] processType   Type of strands that you can process
 * @param[in] properties    Properties (currently just PDSTT_EMPTYTABLES)
 * @param[in] count         Maximum number of strands to try to process
 *
 * @return the number of strands processed or -1 if there was an issue
 */
static u32 _pdProcessNStrands(ocrPolicyDomain_t *pd, u32 processType, u32 count, u32 properties);

static inline u32 selectFreeSlot(pdStrandTableNode_t *node, u32 fudge) {
#ifdef MT_OPTI_SPREAD
    // In this case, we are going to try to spread things out by
    // looking for freeslots that also do not have anything to process.
    // This has the effect of spreading out the load and therefore maximizes
    // the number of "consumers" that can go in at a given time.
    // Note that since we do not know the type of work of the inserted strand
    // (and that can also change over time), we can't pick a specific nodeNeedsProcess
    // so we look at both
    // This also favors inserting in the fudgeMask part (ie: the mask is a higher
    // priority than the needProcess vectors)
    ASSERT(NP_COUNT == 2); // We deal only with hardcoded NP_COUNT for now

    // The fudgeMask is the part of nodeFree that we look at.
    u64 fudgeMask = 0ULL;
    ASSERT(node->nodeFree); // We should at least have some free slot, otherwise the while
                            // loop will run forever
    while(true) {
        // Divide into 4 chunks
        fudgeMask = (0xFFFFULL)<<((fudge & 0x3)<<4);
        ++fudge; // The next time around, we will look at another part of the vector
        // This is the ideal solution (free and no-one needs processing)
        DPRINTF(DEBUG_LVL_VERB, "Looking for free slot with freeSlot:0x%"PRIx64", np[0]:0x%"PRIx64
            ", np[1]:0x%"PRIx64" with mask 0x%"PRIx64"\n",
            node->nodeFree, node->nodeNeedsProcess[0], node->nodeNeedsProcess[1], fudgeMask);

        u64 startVal = node->nodeFree & fudgeMask;
        if(startVal == 0ULL) continue;
        u64 val = startVal & ~node->nodeNeedsProcess[0] & ~node->nodeNeedsProcess[1];

        if(val == 0ULL) {
            DPRINTF(DEBUG_LVL_VVERB, "Overly constrained slots\n");
            // We didn't find anything, so we need to look at one of the less
            // constrained ways
            // Figure out which order to do this in; we look to maximize the one
            // that has least amount of processing
            if(popcnt64(node->nodeNeedsProcess[0] & fudgeMask) > popcnt64(node->nodeNeedsProcess[1] & fudgeMask)) {
                // Here, we look at nodeNeedsProcess[1] first
                DPRINTF(DEBUG_LVL_VVERB, "Prioritizing np[1]\n");
                val = startVal & ~node->nodeNeedsProcess[1];
                val = val==0ULL?(startVal & ~node->nodeNeedsProcess[0]):val;
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "Prioritizing np[0]\n");
                val = startVal & ~node->nodeNeedsProcess[0];
                val = val==0ULL?(startVal & ~node->nodeNeedsProcess[1]):val;
            }
            if(val == 0) {
                val = startVal;
            }
        }
        ASSERT(val); // At this point, we should have something
        return ctz(val);
    }
#else
    // Default behavior is simply to use ctz and figure out the first one that is free
    return ctz(node->nodeFree);
#endif
}

static inline u32 selectProcessSlot(pdStrandTableNode_t *node, u32 npIdx, u32 fudge) {
#ifdef MT_OPTI_SPREAD2
    // In this case, we are going to try to spread things out by
    // looking for needsProcess slots in various chunks

    // The fudgeMask is the part of needsProcess that we look at.
    u64 fudgeMask = 0ULL;
    u64 val = 0ULL;
    ASSERT(node->nodeNeedsProcess[npIdx]); // We should at least have some slot
    while(true) {
        // Divide into 8 chunks
        fudgeMask = (0xFFULL)<<((fudge & 0x7)<<3);
        ++fudge; // The next time around, we will look at another part of the vector
        val = node->nodeNeedsProcess[npIdx] & fudgeMask;

        if(val != 0ULL) break;
    }
    ASSERT(val); // At this point, we should have something
    return ctz(val);
#else
    // Default behavior is simply to use ctz and figure out the first one that needs
    // to be processed
    return ctz(node->nodeNeedsProcess[npIdx]);
#endif
}
/***************************************/
/***** pdEvent_t related functions *****/
/***************************************/


u8 pdCreateEvent(ocrPolicyDomain_t *pd, pdEvent_t **event, u32 type, u8 reserveInTable) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdCreateEvent(pd:%p, event**:%p [%p], type:%"PRIu32", table:%"PRIu32")\n",
            pd, event, *event, type, reserveInTable);

#define _END_FUNC createEventEnd
    ASSERT(event); // Cannot call if you don't want the event back.
    u8 toReturn = 0;
    if (pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }
    u64 sizeToAllocate = 0;
    *event = NULL;
    // Figure out the base type
    switch (type & PDEVT_TYPE_MASK) {
        case PDEVT_TYPE_CONTROL:
            sizeToAllocate = sizeof(pdEvent_t);
            break;
        case PDEVT_TYPE_COMMSTATUS:
            sizeToAllocate = sizeof(pdEventCommStatus_t);
            break;
        case PDEVT_TYPE_MSG:
            sizeToAllocate = sizeof(pdEventMsg_t);
            break;
        case PDEVT_TYPE_MSGINLINE:
            sizeToAllocate = sizeof(pdEventMsgInline_t);
            break;
        case PDEVT_TYPE_MERGE:
            sizeToAllocate = sizeof(pdEventMerge_t);
            break;
        case PDEVT_TYPE_FCT:
            sizeToAllocate = sizeof(pdEventFct_t);
            break;
        default:
            DPRINTF(DEBUG_LVL_WARN, "PD Event type 0x%"PRIu32" not known\n", type);
            return OCR_EINVAL;
    }
    /* BUG #899: replace with proper slab allocator */
    CHECK_MALLOC(*event = (pdEvent_t*)pd->fcts.pdMalloc(pd, sizeToAllocate), );
    DPRINTF(DEBUG_LVL_VERB, "Allocated event of size %"PRIu64" -> ptr: %p\n",
            sizeToAllocate, *event);

    // Initialize base aspect
    (*event)->properties = type;
    (*event)->strand = NULL;
    (*event)->refCount = 1;

    // TODO: Type specific initialization here
    switch (type) {
    case PDEVT_TYPE_CONTROL:
        break;
    case PDEVT_TYPE_COMMSTATUS: {
        pdEventCommStatus_t *t = (pdEventCommStatus_t*)(*event);
        t->properties = 0;
        break;
    }
    case PDEVT_TYPE_MSG: {
        pdEventMsg_t *t = (pdEventMsg_t*)(*event);
        t->msg = NULL;
        getCurrentEnv(NULL, NULL, &(t->ctx), NULL);
        t->continuation = NULL;
        t->properties = 0;
        break;
    }
    case PDEVT_TYPE_MSGINLINE: {
        pdEventMsgInline_t *t = (pdEventMsgInline_t*)(*event);
        getCurrentEnv(NULL, NULL, &(t->ctx), &(t->msg));
        t->msg.bufferSize = sizeof(ocrPolicyMsg_t);
        t->properties = 0;
        break;
    }

    case PDEVT_TYPE_MERGE: {
        pdEventMerge_t *t = (pdEventMerge_t*)(*event);
        arrayDequeInit(&(t->events), PDEVT_MERGE_SIZE);
        t->count = 1; // We start at 1 to prevent a race when adding. pdCloseMerge will increment countReady as needed
        t->countReady = 0;
        t->lock = INIT_LOCK;
        break;
    }
    case PDEVT_TYPE_FCT: {
        pdEventFct_t *t = (pdEventFct_t*)(*event);
        t->ctx = NULL;
        getCurrentEnv(NULL, NULL, &(t->ctx), NULL);
        t->continuation = NULL;
        break;
    }
    default:
        break;
    }

    // Deal with insertion into the strands table if needed
    pdStrandTable_t *stTable = NULL;
    if(reserveInTable) {
        if(reserveInTable <= PDSTT_LAST) {
            DPRINTF(DEBUG_LVL_VERB, "Reserving slot in table %"PRIu32"\n", reserveInTable);
            // This means it is PDSTT_COMM or PDSTT_EVT so we look for the proper table
            stTable = pd->strandTables[reserveInTable - 1];
            pdStrand_t* myStrand = NULL;
            CHECK_RESULT(
                toReturn |= pdGetNewStrand(pd, &myStrand, stTable, *event, PDST_UHOLD),
                pd->fcts.pdFree(pd, *event),);
            DPRINTF(DEBUG_LVL_VERB, "Event %p has index %"PRIu64"\n", *event, (*event)->strand->index);
            // This assert failure indicates a coding issue in the runtime
            RESULT_ASSERT(pdUnlockStrand(myStrand), ==, 0);
        } else {
            DPRINTF(DEBUG_LVL_WARN, "Invalid value for reserveInTable: %"PRIu32"\n", reserveInTable);
            return OCR_EINVAL;
        }
    }

END_LABEL(createEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdCreateEvent -> %"PRIu32"; event: %p; event->strand->index: %"PRIu64"\n",
            toReturn, *event, (*event)->strand?(*event)->strand->index:0);
return toReturn;
#undef _END_FUNC
}

u8 pdReferenceEvent(ocrPolicyDomain_t *pd, pdEvent_t *event) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdReferenceEvent(pd:%p, event*:%p)\n", pd, event);
#define _END_FUNC referenceEventEnd
    u8 toReturn = 0;

    // If this fails, this means that the event is not a real one
    CHECK_RESULT((u64)event & 0x7, , toReturn = OCR_EINVAL);

    u32 oldVal __attribute__((unused)) = hal_xadd32(&(event->refCount), 1);

    CHECK_RESULT(oldVal,
        DPRINTF(DEBUG_LVL_WARN, "POSSIBLE RACE; event counter reached 0 before increment for %p\n", event),
        );

END_LABEL(referenceEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdReferenceEvent -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

u8 pdDestroyEvent(ocrPolicyDomain_t *pd, pdEvent_t *event) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdDestroyEvent(pd:%p, event*:%p)\n", pd, event);
#define _END_FUNC destroyEventEnd

    ASSERT(event);
    u8 toReturn = 0;
    if (pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    // Make sure we have at least one reference
    CHECK_RESULT(event->refCount, , toReturn = OCR_EINVAL);

    u32 oldRefCount = hal_xadd32(&(event->refCount), (u32)-1);
    if(oldRefCount != 1) {
        DPRINTF(DEBUG_LVL_VERB, "Event %p has pre-decrement refCount of %"PRIu32" -- not destroying\n",
            event, oldRefCount);
        RETURN;
    }
    // Here the event is going to be destroyed
    ASSERT(oldRefCount == 1 && event->refCount == 0);
    if(event->strand) {
        DPRINTF(DEBUG_LVL_VERB, "Strand associated with event is %p\n", event->strand);
        RESULT_ASSERT(_pdLockStrand(event->strand, BLOCK), ==, 0);
        CHECK_RESULT_T((event->strand->properties & PDST_WAIT) != PDST_WAIT_ACT,
                       pdUnlockStrand(event->strand), toReturn = OCR_EINVAL);
        event->strand->curEvent = NULL;
        if((event->strand->properties & PDST_HOLD) || (event->strand->properties & PDST_WAIT)) {
            DPRINTF(DEBUG_LVL_WARN, "Destroying event will leave stranded strand; properties "
                    "on strand %p are 0x%"PRIx32"\n", event->strand, event->strand->properties);
        }
        pdUnlockStrand(event->strand);
    }

    // Do any regular free stuff
    switch(event->properties & PDEVT_TYPE_MASK) {
    case PDEVT_TYPE_CONTROL:
        break;
    case PDEVT_TYPE_COMMSTATUS:
        break;
    case PDEVT_TYPE_MSG:
        break;
    case PDEVT_TYPE_MSGINLINE:
        break;
    case PDEVT_TYPE_MERGE: {
        // We basically have to "un-reference" each event that we contain
        // so that they may be freed up as well.
        pdEventMerge_t *t = (pdEventMerge_t*)event;
        pdEvent_t *insideEvt = NULL;
        while(t->count) {
            RESULT_ASSERT(arrayDequePopFromHead(&(t->events), (void**)&insideEvt), ==, 0);
            if(insideEvt && (((u64)insideEvt & 0x7) == 0)) {
                // If the event exists and is a real event
                RESULT_ASSERT(pdDestroyEvent(pd, insideEvt), ==, 0);
            }
            --t->count;
        }
        break;
    }
    case PDEVT_TYPE_FCT:
        break;
    default:
        DPRINTF(DEBUG_LVL_WARN, "Unknown event type 0x%"PRIx32"\n", event->properties & PDEVT_TYPE_MASK);
        ASSERT(0);
    }

    // Do any additional "deep-free" stuff
    if((event->properties & PDEVT_DESTROY_DEEP) != 0) {
        DPRINTF(DEBUG_LVL_VERB, "Deep free of event requested\n");
        switch(event->properties & PDEVT_TYPE_MASK) {
        case PDEVT_TYPE_CONTROL:
            break;
        case PDEVT_TYPE_COMMSTATUS:
            break;
        case PDEVT_TYPE_MSG:
        {
            pdEventMsg_t *t = (pdEventMsg_t*)(event);
            if(t->msg) {
                DPRINTF(DEBUG_LVL_VVERB, "Freeing message %p in event\n", t->msg);
                pd->fcts.pdFree(pd, t->msg);
            }
            break;
        }
        case PDEVT_TYPE_MSGINLINE:
            break;
        case PDEVT_TYPE_MERGE:
            break;
        case PDEVT_TYPE_FCT:
            break;
        default:
            DPRINTF(DEBUG_LVL_WARN, "Unknown event type 0x%"PRIx32"\n", event->properties & PDEVT_TYPE_MASK);
            ASSERT(0);
        }
    }

    pd->fcts.pdFree(pd, event);

END_LABEL(destroyEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdDestroyEvent -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

u8 pdResolveEvent(ocrPolicyDomain_t *pd, u64 *evtValue, pdStrand_t **strand, u8 clearFwdHold) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdResolveEvent(pd:%p, evtValue*:%p [0x%"PRIx64"], strand*:%p [%p], clearHold:%"PRIu32")\n",
            pd, evtValue, *evtValue, strand, strand?*strand:NULL, clearFwdHold);
#define _END_FUNC resolveEventEnd

    u8 toReturn = 0;
    if(pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    pdStrand_t* myStrand = NULL;
    u8 stTableIdx = EVT_DECODE_ST_TBL(*evtValue);

    if (stTableIdx) {
        DPRINTF(DEBUG_LVL_VERB, "Event = (table %"PRIu32", idx: %"PRIu64")\n",
                stTableIdx, EVT_DECODE_ST_IDX(*evtValue));
        // This is a pointer in a strands table
        pdStrandTable_t *stTable = NULL;
        if(stTableIdx < PDSTT_LAST) {
            stTable = pd->strandTables[stTableIdx-1];
        } else {
            DPRINTF(DEBUG_LVL_WARN, "Invalid event value: %"PRIu32" does not represent "
                    "a valid table (from event value 0x%"PRIx64")\n",
                    stTableIdx, *evtValue);
            return OCR_EINVAL;
        }
        DPRINTF(DEBUG_LVL_VERB, "Looking in table %p\n", stTable);
        CHECK_RESULT(toReturn |= pdGetStrandForIndex(pd, &myStrand, stTable,
                                                     EVT_DECODE_ST_IDX(*evtValue)),,);

        // Here, we managed to get the strand properly
        // We do a lock-free check first because we want to not block even
        // if the strand is being processed (only long lock)
        if((myStrand->properties & PDST_WAIT) == 0) {
            // We have a good chance here; there is no action so unlikely it is
            // being processed
            if(_pdLockStrand(myStrand, 0) == 0) {
                DPRINTF(DEBUG_LVL_VVERB, "Event 0x%"PRIx64" -> strand %p (props: 0x%"PRIx32")\n",
                    *evtValue, myStrand, myStrand->properties);
                ASSERT(hal_islocked(&(myStrand->lock)));
                if((myStrand->properties & PDST_WAIT) == 0) {
                    // Event is ready
                    // The following assert ensures that the event in the slot has
                    // the slot's index. Failure indicates a runtime error
                    ASSERT(myStrand->index == (*evtValue));
                    DPRINTF(DEBUG_LVL_VERB, "Event 0x%"PRIx64" -> %p\n",
                        *evtValue, myStrand->curEvent);
                    *evtValue = (u64)(myStrand->curEvent);
                    // The event is already referenced (because it is in the strand)
                    if(clearFwdHold) {
                        myStrand->properties &= ~PDST_RHOLD;
                    }
                    if((myStrand->properties & PDST_HOLD) == 0) {
                        DPRINTF(DEBUG_LVL_VVERB, "Freeing strand %p [idx %"PRIu64"] after resolution\n",
                            myStrand, myStrand->index);
                        RESULT_ASSERT(_pdDestroyStrand(pd, ((pdEvent_t*)evtValue)->strand), ==, 0);
                    } else {
                        RESULT_ASSERT(pdUnlockStrand(myStrand), ==, 0);
                    }
                }
            } else {
                // Here, we are just going to pretend the event is not ready
                // and return that
                DPRINTF(DEBUG_LVL_VVERB, "Event 0x%"PRIx64" -> strand %p; could not lock strand\n",
                    *evtValue, myStrand);
                toReturn = OCR_EBUSY;
            }
        } else {
            DPRINTF(DEBUG_LVL_VVERB, "Event 0x%"PRIx64" -> strand %p (props: 0x%"PRIx32") is not ready\n",
                *evtValue, myStrand, myStrand->properties);
            toReturn = OCR_EBUSY;
        }

        if(strand)
            *strand = myStrand;
    } else {
        DPRINTF(DEBUG_LVL_VERB, "Event 0x%"PRIx64" is already a pointer\n", *evtValue);
        pdEvent_t *evt = (pdEvent_t*)(*evtValue);
        // By default return OCR_ENOP unless we find the event is not ready
        toReturn = OCR_ENOP;

        // Easy to get the strand here
        if(strand)
            *strand = evt->strand;
        if(evt->properties & PDEVT_READY) {
            ocrWorker_t *curWorker;
            getCurrentEnv(NULL, &curWorker, NULL, NULL);
            if(evt->strand) {
                if(evt->strand->processingWorker == curWorker) {
                    // This means that the event is being resolved by the thread processing
                    // the strand
                    if(evt->strand->properties & PDST_MODIFIED) {
                        DPRINTF(DEBUG_LVL_VERB, "Event %p is being processed and requires strand re-processing\n", evt);
                        toReturn = OCR_EBUSY;
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Event %p is being processed and is ready\n", evt);
                        if(clearFwdHold) {
                            evt->strand->properties &= ~PDST_RHOLD;
                        }
                    }
                } else {
                    // Check to make sure the strand is ready
                    // We again do not lock right away
                    if((evt->strand->properties & PDST_WAIT) == 0) {
                        // Now we try to grab the lock
                        if(_pdLockStrand(evt->strand, 0) == 0) {
                            // Success in locking
                            DPRINTF(DEBUG_LVL_VVERB, "Event %p has strand %p (props: 0x%"PRIx32")\n",
                                evt, evt->strand, evt->strand->properties);
                            ASSERT(hal_islocked(&(evt->strand->lock)));
                            if((evt->strand->properties & PDST_WAIT) != 0) {
                                // Event is not fully ready, there is some stuff left to process
                                DPRINTF(DEBUG_LVL_VERB, "Event %p is ready but strand is not\n", evt);
                                toReturn = OCR_EBUSY;
                            } else {
                                if(clearFwdHold) {
                                    evt->strand->properties &= ~PDST_RHOLD;
                                }
                            }
                            RESULT_ASSERT(pdUnlockStrand(evt->strand), ==, 0);
                        } else {
                            DPRINTF(DEBUG_LVL_VVERB, "Event %p has strand %p but could not lock it\n",
                                evt, evt->strand);
                            toReturn = OCR_EBUSY;
                        }
                    } else {
                        DPRINTF(DEBUG_LVL_VVERB, "Event %p has strand %p (props: 0x%"PRIx32") not ready\n",
                            evt, evt->strand, evt->strand->properties);
                    }
                }
            }
        } else {
            // event is not ready so we don't even need to check the eventual strand
            DPRINTF(DEBUG_LVL_VERB, "Event %p is NOT ready\n", evt);
            toReturn = OCR_EBUSY;
        }

    }
END_LABEL(resolveEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdResolveEvent -> %"PRIu32"; event: 0x%"PRIx64" strand: %p [%p]\n",
            toReturn, *evtValue, strand, strand?*strand:NULL);
    return toReturn;
#undef _END_FUNC
}

u8 pdEncodeEvent(ocrPolicyDomain_t *pd, u64 *evtValue) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdEncodeEvent(pd:%p, evtValue*:%p [0x%"PRIx64"])\n",
            pd, evtValue, *evtValue);
#define _END_FUNC encodeEventEnd

    u8 toReturn = 0;
    if(pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }
    u8 stTableIdx = EVT_DECODE_ST_TBL(*evtValue);
    if (stTableIdx) {
        // Event is already encoded, do nothing
        toReturn = OCR_ENOP;
    } else {
        pdEvent_t *evt = (pdEvent_t*)(*evtValue);
        if(evt->strand) {
            // This could be made more efficient but we won't have many tables anyways
            u32 tblIdx = 0;
            u32 stIdx = evt->strand->index;
            for(tblIdx=0; tblIdx<=PDSTT_LAST; tblIdx++) {
                if(pd->strandTables[tblIdx] == evt->strand->containingTable)
                    break;
            }
            if(tblIdx > PDSTT_LAST) {
                // Somehow we didn't find the table which is weird and probably a runtime error...
                DPRINTF(DEBUG_LVL_WARN, "Event %p (strand %p) is not contained in a valid table (%p)\n",
                    evt, evt->strand, evt->strand->containingTable);
                toReturn = OCR_EINVAL;
            } else {
                *evtValue = EVT_ENCODE(tblIdx, stIdx);
            }
        } else {
            DPRINTF(DEBUG_LVL_WARN, "Event %p is not part of a strand -- cannot encode\n", evt);
            toReturn = OCR_EINVAL;
        }
    }

END_LABEL(encodeEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdEncodeEvent -> %"PRIu32"; event: 0x%"PRIx64"\n",
            toReturn, *evtValue);
    return toReturn;
#undef _END_FUNC
}

u8 pdMarkReadyEvent(ocrPolicyDomain_t *pd, pdEvent_t *evt) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdMarkReadyEvent(pd:%p, evt:%p)\n",
            pd, evt);
#define _END_FUNC markReadyEventEnd

    u8 toReturn = 0;
    if(pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    CHECK_RESULT_T(evt != NULL, , toReturn = OCR_EINVAL);
    CHECK_RESULT_T((evt->properties & PDEVT_READY) == 0, , toReturn = OCR_EINVAL);

    evt->properties |= PDEVT_READY;
    if(evt->strand != NULL) {
        DPRINTF(DEBUG_LVL_VERB, "Event has strand %p -> going to update\n", evt->strand);
        // First grab the lock on the strand
        pdStrand_t *strand = evt->strand;
        u32 stIdx = strand->index & ((1<<BV_SIZE_LOG2) - 1);
        pdStrandTableNode_t *curNode = strand->parent;
        ASSERT(curNode);

        // Lock the strand
        RESULT_ASSERT(_pdLockStrand(strand, BLOCK), ==, 0);

        // This should be the case since the event was not ready yet
        ASSERT((strand->properties & PDST_WAIT_EVT) != 0);
        strand->properties &= ~PDST_WAIT_EVT;

        // Here, we only propagate if we are not the processing worker
        // If we ARE the processing worker, this will happen when the strand is
        // done being processed so we can save time here
        ocrWorker_t *worker = NULL;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        if(worker != strand->processingWorker) {
            // The following things can happen here
            //     - there are actions so this strand needs processing
            //     - there are no actions -> this strand becomes ready (destroyed or kept around)
            bool propagateReady = false, propagateNP = false, didFree = false;
            u32 npIdxCounter = 0;
            u8 npIdx[NP_COUNT] = {0};
            hal_lock(&(curNode->lock));
            if ((strand->properties & PDST_WAIT_ACT) != 0) {
                // Get the NP index for the first action to do
                pdAction_t *tAct = NULL;
                RESULT_ASSERT(arrayDequePeekFromHead(strand->actions, (void**)&(tAct)), ==, 0);
                _pdActionToNP(npIdx, tAct);
                while(npIdx[npIdxCounter]) {
                    u8 tIdx = npIdx[npIdxCounter] - 1;
                    DPRINTF(DEBUG_LVL_VERB, "Strand %p has waiting actions -> setting NP[%"PRIu32"]\n", strand, tIdx);
                    // We have pending actions, making this a NP node
                    propagateNP |= curNode->nodeNeedsProcess[tIdx] == 0ULL;
                    curNode->nodeNeedsProcess[tIdx] |= (1ULL<<stIdx);
#ifdef MT_OPTI_CONTENTIONLIMIT
                    u32 t __attribute__((unused)) = hal_xadd32(&(strand->containingTable->consumerCount[tIdx]), 1);
                    DPRINTF(DEBUG_LVL_VVERB, "Incrementing consumerCount[%"PRIu32"] @ table %p (markReadyEvent); was %"PRId32"\n",
                        tIdx, strand->containingTable, t);
#endif
                    ++npIdxCounter;
                }
            } else {
                DPRINTF(DEBUG_LVL_VERB, "Strand %p is fully ready\n", strand);
                propagateReady = curNode->nodeReady == 0ULL;
                curNode->nodeReady |= (1ULL<<stIdx);
            }

            if((strand->properties & PDST_WAIT) == 0) {
                  // Strand is ready; we either free it or keep it there due to a hold
                if ((strand->properties & PDST_HOLD) == 0) {
                    // We can free the strand now
                    DPRINTF(DEBUG_LVL_VERB, "(POSSIBLE RACE) Freeing strand %p [idx %"PRIu64"] after making event ready\n",
                    strand, strand->index);
                    // We unset the nodeReady bit to prevent unecessary propagation
                    curNode->nodeReady &= ~(1ULL<<stIdx);
                    hal_unlock(&(curNode->lock));
                    RESULT_ASSERT(_pdDestroyStrand(pd, strand), ==, 0);
                    ASSERT(!propagateNP);
                    propagateReady = false; // No need to change this since we freed the node
                    didFree = true;
                } else {
                    DPRINTF(DEBUG_LVL_VERB, "Strand %p is ready but has a hold -- leaving as is\n",
                            strand);
                    pdUnlockStrand(strand);
                }
            } else {
                pdUnlockStrand(strand);
            }

            // We still hold lock on curNode EXCEPT if didFree
            if (propagateReady || propagateNP) {
                ASSERT(!didFree);
                DPRINTF(DEBUG_LVL_VERB, "Propagating properties: ready: %"PRIu32"; np: %"PRIu32"\n",
                        propagateReady, propagateNP);

                pdStrandTableNode_t *parent = curNode->parent;
                ASSERT(hal_islocked(&(curNode->lock)));
                // We flipped nodeReady from 0 to 1; to up until we see a 1
                // We flipped nodeNeedsProcessing from 0 to 1; same as above
                u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                if(npIdx[1]) {
                    DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                }
                PROPAGATE_UP_TREE(curNode, parent, tNpIdx,
                    propagateReady || propagateNP, {
                    if (propagateReady) {
                        propagateReady = parent->nodeReady == 0ULL;
                        parent->nodeReady |= (1ULL<<curNode->parentSlot);
                    }
                    if (propagateNP) {
                        propagateNP = false;
                        u32 i;
                        for(i=0; i<NP_COUNT; ++i) {
                            if(npIdx[i]) { // npIdx[i] == 0 means nothing to propagate at that position
                                if(parent->nodeNeedsProcess[npIdx[i]-1] == 0ULL) {
                                    parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                    propagateNP = true;
                                } else {
                                    parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                    npIdx[i] = 0;
                                }
                            }
                        }
                    }
                });
            } else {
                if(!didFree)
                    hal_unlock(&(curNode->lock));
            }
        } else {
            DPRINTF(DEBUG_LVL_VERB, "Skipping propagation as worker is processing worker\n");
            // No need to unlock because it is a NOP when we are the processing worker
        }
    }
END_LABEL(markReadyEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdMarkReadyEvent -> %"PRIu32"\n",
            toReturn);
    return toReturn;
#undef _END_FUNC
}

u8 pdMarkWaitEvent(ocrPolicyDomain_t *pd, pdEvent_t *evt) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdMarkWaitEvent(pd:%p, evt:%p)\n",
            pd, evt);
#define _END_FUNC markWaitEventEnd

    u8 toReturn = 0;
    if(pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    CHECK_RESULT_T(evt != NULL, , toReturn = OCR_EINVAL);
    CHECK_RESULT_T((evt->properties & PDEVT_READY) == PDEVT_READY, , toReturn = OCR_EINVAL);

    evt->properties &= ~PDEVT_READY;
    if(evt->strand != NULL) {
        DPRINTF(DEBUG_LVL_VERB, "Event has strand %p -> going to update\n", evt->strand);
        // First grab the lock on the strand
        pdStrand_t *strand = evt->strand;
        u32 stIdx = strand->index & ((1<<BV_SIZE_LOG2) - 1);
        pdStrandTableNode_t *curNode = strand->parent;
        ASSERT(curNode);

        // Lock the strand
        RESULT_ASSERT(_pdLockStrand(strand, BLOCK), ==, 0);

        // This should be the case since the event was ready prior to this
        ASSERT((strand->properties & PDST_WAIT_EVT) == 0);
        strand->properties |= PDST_WAIT_EVT;

        // Here, we only propagate if we are not the processing worker
        // If we ARE the processing worker, this will happen when the strand is
        // done being processed so we can save time here
        ocrWorker_t *worker = NULL;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        if(worker != strand->processingWorker) {
            // The following things can happen here
            //     - there are actions so this strand no longer needs processing
            //     - there are no actions -> this strand is no longer ready
            bool propagateReady = false, propagateNP = false;
            // Need Processing index (what's the next work to do)
            u32 npIdxCounter = 0;
            u8 npIdx[NP_COUNT] = {0};
            hal_lock(&(curNode->lock));
            if ((strand->properties & PDST_WAIT_ACT) != 0) {
                DPRINTF(DEBUG_LVL_VERB, "(POSSIBLE RACE) Strand %p has waiting actions\n", strand);
                pdAction_t *tAct = NULL;
                RESULT_ASSERT(arrayDequePeekFromHead(strand->actions, (void**)&(tAct)), ==, 0);
                _pdActionToNP(npIdx, tAct);
                while(npIdx[npIdxCounter]) {
                    u8 tIdx = npIdx[npIdxCounter] - 1;
                    curNode->nodeNeedsProcess[tIdx] &= ~(1ULL<<stIdx);
                    propagateNP |= curNode->nodeNeedsProcess[tIdx] == 0ULL;

#ifdef MT_OPTI_CONTENTIONLIMIT
                    u32 t __attribute__((unused)) = hal_xadd32(&(strand->containingTable->consumerCount[tIdx]), -1);
                    DPRINTF(DEBUG_LVL_VVERB, "Decrementing consumerCount[%"PRIu32"] @ table %p (markEventWait); was %"PRId32"\n",
                        tIdx, strand->containingTable, t);
#endif
                    ++npIdxCounter;
                }
            } else {
                DPRINTF(DEBUG_LVL_VERB, "Strand %p is no longer ready\n", strand);
                // If we are still around, it means we have an active hold
                ASSERT(strand->properties & PDST_HOLD);
                curNode->nodeReady &= ~(1ULL<<stIdx);
                propagateReady = curNode->nodeReady == 0ULL;
            }

            // Not quite sure how to recover from this one. If this fails, it means
            // we couldn't enqueue buffered actions
            RESULT_ASSERT(pdUnlockStrand(strand), ==, 0);

            // We still hold lock on curNode
            if (propagateReady || propagateNP) {
                DPRINTF(DEBUG_LVL_VERB, "Propagating properties: ready: %"PRIu32"; np: %"PRIu32"\n",
                    propagateReady, propagateNP);

                pdStrandTableNode_t *parent = curNode->parent;
                ASSERT(hal_islocked(&(curNode->lock)));
                // We flipped nodeReady from 1 to 0; to up until we see a sibbling
                // We flipped nodeNeedsProcessing from 1 to 0; same as above
                u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                if(npIdx[1]) {
                    DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                }
                PROPAGATE_UP_TREE(curNode, parent, tNpIdx,
                  propagateReady || propagateNP, {
                    if (propagateReady) {
                        parent->nodeReady &= ~(1ULL<<curNode->parentSlot);
                        propagateReady = parent->nodeReady == 0ULL;
                    }
                    if (propagateNP) {
                        propagateNP = false;
                        u32 i;
                        for(i=0; i<NP_COUNT; ++i) {
                            if(npIdx[i]) {
                                parent->nodeNeedsProcess[npIdx[i]-1] &= ~(1ULL<<curNode->parentSlot);
                                if(parent->nodeNeedsProcess[npIdx[i]-1] == 0ULL) {
                                    propagateNP = true;
                                } else {
                                    npIdx[i] = 0;
                                }
                            }
                        }
                    }
                });
            } else {
                hal_unlock(&(curNode->lock));
            }
        } else {
            DPRINTF(DEBUG_LVL_VERB, "Skipping propagation as worker is processing worker\n");
            // No need to unlock as we are the processing worker and an unlock is a NOP
        }
    }
END_LABEL(markWaitEventEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdMarkWaitEvent -> %"PRIu32"\n",
            toReturn);
    return toReturn;
#undef _END_FUNC
}

u8 pdAddEventToMerge(ocrPolicyDomain_t *pd, u64 *position, pdEventMerge_t *mergeEvent, pdEvent_t *event) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdAddEventToMerge(pd:%p, position*:%p, mergeEvent*:%p, event*:%p)\n",
            pd, position, mergeEvent, event);
#define _END_FUNC addEventToMerge

    u8 toReturn = 0;
    if(pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    // Check if the event is real and ready
    if(event && (((u64)event & 0x7) == 0)) {
        if(event->properties & PDEVT_READY) {
            // Grab a reference on it since we are adding it to here
            CHECK_RESULT(pdReferenceEvent(pd, event), , toReturn = OCR_EINVAL);
        } else {
            // The event is not ready, we error out
            DPRINTF(DEBUG_LVL_WARN, "Event passed in pdAddEventToMerge is not ready\n");
            CHECK_RESULT(1, , toReturn = OCR_EINVAL);
        }
    } else {
        if(event) {
            DPRINTF(DEBUG_LVL_WARN, "Considering event as NULL in pdAddEventToMerge\n");
            event = NULL;
        }
    }

    // Now insert it in the queue of events and get the proper position for it
    u32 returnedPosition = 0;
    hal_lock(&(mergeEvent->lock));
    arrayDequePushAtTail(&(mergeEvent->events), event);
    returnedPosition = mergeEvent->count++ - 1; // -1 because the initial count is 1
    ASSERT(returnedPosition + 1 == arrayDequeSize(&(mergeEvent->events)));
    hal_unlock(&(mergeEvent->lock));

    if(event) {
        // If the event is initially ready, we will increment the ready count as well
        u32 newCountReady = hal_xadd32(&(mergeEvent->countReady), 1);
        if(newCountReady + 1 == mergeEvent->count) {
            DPRINTF(DEBUG_LVL_VERB, "Last event satisfied (on addition) -- triggering merged event %p\n", mergeEvent);
            RESULT_ASSERT(pdMarkReadyEvent(pd, (pdEvent_t*)mergeEvent), ==, 0);
        }
    }
    if(position)
        *position = returnedPosition;

END_LABEL(addEventToMerge)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdAddEventToMerge -> %"PRIu32" position: %"PRIu64"\n",
            toReturn, position?*position:0);
    return toReturn;
#undef _END_FUNC
}

u8 pdCloseMerge(ocrPolicyDomain_t *pd, pdEventMerge_t *mergeEvent) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdCloseMerge(pd:%p, mergeEvent*:%p)\n",
            pd, mergeEvent);
#define _END_FUNC closeMerge

    u8 toReturn = 0;

    u32 newCountReady = hal_xadd32(&(mergeEvent->countReady), 1);
    if(newCountReady + 1 == mergeEvent->count) {
        DPRINTF(DEBUG_LVL_VERB, "Last event satisfied (on close) -- triggering merged event %p\n", mergeEvent);
        RESULT_ASSERT(pdMarkReadyEvent(pd, (pdEvent_t*)mergeEvent), ==, 0);
    }

END_LABEL(closeMerge)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdCloseMerge -> %"PRIu32"\n",
            toReturn);
    return toReturn;
#undef _END_FUNC
}

/***************************************/
/***** pdAction_t related functions ****/
/***************************************/

// TODO: I can encode difference between strand and current object instead of
// address of current object so that it is easier for me to use the full 64 bits
// and not be too constrained...

/* Macros defining the special encoding of pdAction_t* */
// TODO: PROCESS_MESSAGE and PROCESS_EVENT will be merged when
// policy domains also become ocrObject_t (this will come
// in a subsequent patch)
// Main type is on 3 bits
#define PDACTION_ENC_SIZE              3
#define PDACTION_ENC_PROCESS_MESSAGE   0b001
// Revisit next two
#define PDACTION_ENC_PROCESS_EVENT     0b010
#define PDACTION_ENC_PROCESS_EVENT_MOD 0b011
#define PDACTION_ENC_EXTEND            0b111

// Extended "type" values are on 8 bits
#define PDACTION_ENCEXT_SIZE                    8
/**< Mark a strand ready (strand encoded using strand table and index) */
#define PDACTION_ENCEXT_CTRL_BASE               0x001
#define PDACTION_ENCEXT_CTRL_MARK_READY         0x001
/**< Mark a strand ready (strand encoded as an offset) */
#define PDACTION_ENCEXT_CTRL_MARK_READY_DIFF    0x011
/**< Satisfy a merge event */
#define PDACTION_ENCEXT_CTRL_SATISFY_MERGE      0x101

// Convenience functions
#define PDACTION_DECEXT_TYPE(_type, _value) do {    \
        u64 __v = (_value);                         \
        _type = (__v >> PDACTION_ENC_SIZE) & ((1ULL<<PDACTION_ENCEXT_SIZE) - 1);\
    } while(0);

#define PDACTION_1ARG_SIZE 53
#define PDACTION_ENCEXT_1ARG(_arg1, _type) (((_arg1) << (PDACTION_ENC_SIZE + PDACTION_ENCEXT_SIZE)) | ((_type)<<PDACTION_ENC_SIZE) | PDACTION_ENC_EXTEND)

#define PDACTION_2ARG_SIZE1 16
#define PDACTION_2ARG_SIZE2 37
#define PDACTION_ENCEXT_2ARG(_arg1, _arg2, _type) (((_arg1)<<(PDACTION_ENC_SIZE + PDACTION_ENCEXT_SIZE + PDACTION_2ARG_SIZE1)) | ((_arg2)<<(PDACTION_ENC_SIZE + PDACTION_ENCEXT_SIZE)) | ((_type)<<PDACTION_ENC_SIZE) | PDACTION_ENC_EXTEND)

#define PDACTION_DECEXT_1ARG(_arg1, _type, _value) do {         \
        u64 __v = (_value);                                     \
        _type = (__v >> PDACTION_ENC_SIZE) & ((1ULL<<PDACTION_ENCEXT_SIZE) - 1); \
        __v >>= PDACTION_ENC_SIZE + PDACTION_ENCEXT_SIZE;       \
        _arg1 = __v & ((1ULL<<PDACTION_1ARG_SIZE) - 1);         \
    } while(0);

#define PDACTION_DECEXT_2ARG(_arg1, _arg2, _type, _value) do {  \
        u64 __v = (_value);                                     \
        _type = (__v >> PDACTION_ENC_SIZE) & ((1ULL<<PDACTION_ENCEXT_SIZE) - 1); \
        __v >>= PDACTION_ENC_SIZE + PDACTION_ENCEXT_SIZE;       \
        _arg2 = __v & ((1ULL<<PDACTION_2ARG_SIZE1) - 1);        \
        _arg1 = (__v >> PDACTION_2ARG_SIZE1);                   \
    } while(0);
// Check bits

/**
 * @brief Encodes a differential object (to save on bits)
 *
 * The idea is that _curStrand and whatever object we want to
 * point to more than likely live in a space that is smaller than 64 bits
 * so we encode the difference to be able to encode more stuff
 * within the 64 bits of an action
 *
 * @param[in] curStrand     Current strand (base to encode from)
 * @param[in] otherObj      Object to encode
 * @param[in] maxBits       Maximum number of bits that can be used
 *
 * @return A signed offset on maxBits bits
 */
inline static u64 PDACTION_GETDIFF(u64 curStrand, u64 otherObj, u32 maxBits) {
    if(otherObj > curStrand) {
        // Check that the value is actually OK
        u64 diff = otherObj - curStrand;
        ASSERT((diff>>(maxBits-1)) == 0);
        return diff;
    } else {
        // We encode over 64-11=53 bits.
        u64 diff = curStrand - otherObj;
        // Check that the value actually fits
        ASSERT((diff>>(maxBits-1)) == 0);
        diff |= 1ULL<<(maxBits-1); // Make negative
        return diff;
    }
}

/**
 * @brief Reverse of PDACTION_GETDIFF
 *
 * This decodes an offset and returns the 64 bit address
 * that was encoded
 *
 * @param[in] curStrand Current strand (base to decode from)
 * @param[in] encoded Value to decode
 *
 * @return 64 bit address of the object (cast as appropriate)
 */
inline static u64 PDACTION_FROMDIFF(u64 curStrand, u64 encoded, u32 maxBits) {
    u64 diff = encoded & (((1ULL<<(maxBits-1))-1));
    if(diff & (1ULL<<(maxBits-1))) {
        // This was a negative offset so the returned value is SMALLER than curStrand
        return curStrand - diff;
    } else {
        return curStrand + diff;
    }
}

pdAction_t* pdGetProcessMessageAction(u32 workType) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetProcessMessageAction(%"PRIu32")\n", workType);
    ASSERT(workType == NP_COMM || workType == NP_WORK);
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdGetProcessMessageAction -> action:0x%"PRIx64"\n", (u64)(((u64)workType<<3) | PDACTION_ENC_PROCESS_MESSAGE));
    return (pdAction_t*)(((u64)workType<<3) | PDACTION_ENC_PROCESS_MESSAGE);
}

pdAction_t* pdGetProcessEventAction(ocrObject_t* object) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetProcessEventAction(object: %p)\n", object);
    ASSERT(((u64)object & 0x7) == 0);
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdGetProcessEventAction -> action:0x%"PRIx64"\n", (u64)(((u64)object) | PDACTION_ENC_PROCESS_EVENT));
    return (pdAction_t*)(((u64)object) | PDACTION_ENC_PROCESS_EVENT);
}

/*
pdAction_t* pdGetModuleProcessEventAction(ocrModule_t* module) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetModuleProcessEventAction(module: %p)\n", module);
    ASSERT(((u64)module & 0x7) == 0);
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdGetModuleProcessEventAction -> action:0x%"PRIx64"\n", (u64)(((u64)module) | PDACTION_ENC_PROCESS_EVENT_MOD));
    return (pdAction_t*)(((u64)object) | PDACTION_ENC_PROCESS_EVENT_MOD);
}
*/
pdAction_t* pdGetMarkReadyAction(pdStrand_t *curStrand, pdEvent_t *event) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetMarkReadyAction(curStrand*:%p, event*:%p)\n", curStrand, event);

    // Some sanity checks. Any failure here means that the API is being misused
    // These should probably be changed to returning NULL but I am pretty sure no one will
    // check :)
    ASSERT(curStrand);
    ASSERT(event);

    u64 evtValue = (u64)event;
    u64 returnedAction = 0;
    u8 stTableIdx = EVT_DECODE_ST_TBL(evtValue);
    if(stTableIdx) {
        u64 stIdx = EVT_DECODE_ST_IDX(evtValue);
        DPRINTF(DEBUG_LVL_VVERB, "Event = (table %"PRIu32", idx: %"PRIu64")\n",
                stTableIdx, stIdx);
        returnedAction = PDACTION_ENCEXT_2ARG(stIdx, stTableIdx, PDACTION_ENCEXT_CTRL_MARK_READY);
    } else {
        // We can get the strand directly so we just code that directly
        // I don't think it makes sense to mark an event ready that is not in a strand
        // so we assert for now
        ASSERT(event->strand);
        ASSERT(((u64)(event->strand) & 0x7) == 0);
        DPRINTF(DEBUG_LVL_VVERB, "Direct marking of strand %p\n", event->strand);
        returnedAction = PDACTION_ENCEXT_1ARG(PDACTION_GETDIFF((u64)curStrand, (u64)event->strand, PDACTION_1ARG_SIZE),
            PDACTION_ENCEXT_CTRL_MARK_READY_DIFF);
    }
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdGetMarkReadyAction -> action:0x%"PRIx64"\n", returnedAction);
    return (pdAction_t*)returnedAction;
}

pdAction_t* pdGetSatisfyMergeAction(pdStrand_t *curStrand, pdEventMerge_t *mergeEvent, u64 position) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetSatisfyMergeAction(curStrand*:%p, mergeEvent*:%p, position:%"PRIu64")\n",
        curStrand, mergeEvent, position);

    // Sanity checks again
    ASSERT(curStrand);
    ASSERT(mergeEvent);
    ASSERT((position >> PDACTION_2ARG_SIZE1) == 0);

    u64 returnedAction = PDACTION_ENCEXT_2ARG(PDACTION_GETDIFF((u64)curStrand, (u64)mergeEvent, PDACTION_2ARG_SIZE2),
        position, PDACTION_ENCEXT_CTRL_SATISFY_MERGE);

    DPRINTF(DEBUG_LVL_INFO, "EXIT pdGetSatisfyMergeAction -> action:0x%"PRIx64"\n", returnedAction);
    return (pdAction_t*)returnedAction;
}

/***************************************/
/***** pdStrand_t related functions ****/
/***************************************/

u8 pdInitializeStrandTable(ocrPolicyDomain_t* pd, pdStrandTable_t *table,
                           u32 properties) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdInitializeStrandTable(pd:%p, table:%p, props:0x%"PRIx32")\n",
            pd, table, properties);
#define _END_FUNC initializeStrandTableEnd
    u8 toReturn = 0;

    CHECK_RESULT_T(table != NULL, , toReturn |= OCR_EINVAL);

    table->levelCount = 0;
    table->head = NULL;
    table->lock = INIT_LOCK;

END_LABEL(initializeStrandTableEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdInitializeStrandTable -> %"PRIu32"\n",
            toReturn);
    return toReturn;
#undef _END_FUNC

}

u8 pdDestroyStrandTable(ocrPolicyDomain_t* pd, pdStrandTable_t *table,
                           u32 properties) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdDestroyStrandTable(pd:%p, table:%p, props:0x%"PRIx32")\n",
            pd, table, properties);
#define _END_FUNC destroyStrandTableEnd
    u8 toReturn = 0;

    CHECK_RESULT_T(table != NULL, , toReturn |= OCR_EINVAL);
    hal_lock(&(table->lock));
    if (table->head != NULL) {
        // If the head exists, make sure all nodes are marked as free
        hal_lock(&(table->head->lock));
        CHECK_RESULT_T(table->head->nodeFree == ~0ULL, {
                hal_unlock(&(table->head->lock));
                hal_unlock(&(table->lock));
            }, toReturn |= OCR_EINVAL);
        hal_unlock(&(table->head->lock));
        // At this point, we hold the lock on the table and unless something is
        // fishy, since the table is empty, nothing else can be happening in
        // parallel. We will therefore free stuff happily.
        _pdDestroyStrandTableNode(pd, table->head);
        pd->fcts.pdFree(pd, table->head);
    } else {
        DPRINTF(DEBUG_LVL_VERB, "Freeing NULL table\n");
    }
    hal_unlock(&(table->lock));

END_LABEL(destroyStrandTableEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdDestroyStrandTable -> %"PRIu32"\n",
            toReturn);
    return toReturn;
#undef _END_FUNC
}

// NOTE on the lock order:
// - If you want to hold multiple locks, you must hold
//   the lock for the child FIRST and then acquire the lock of your parent.

u8 pdGetNewStrand(ocrPolicyDomain_t *pd, pdStrand_t **returnStrand, pdStrandTable_t *table,
                  pdEvent_t* event, u32 properties) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetNewStrand(pd:%p, strand**:%p [%p], table:%p)\n",
            pd, returnStrand, *returnStrand, table);
#define _END_FUNC getNewStrandEnd

    // The general algorithm here is:
    //   - start at the head of the table and go down paths that have potential free
    //     slots that could be used.
    //     - if multiple possibilities exist, we use a "fudgeFactor" to determine
    //       which way to go. This fudge factor is composed of two parts:
    //         - the ID of the worker (so each worker looks in different parts)
    //         - the "round" the worker is doing (basically, everytime it "fails" to find
    //           a free slot because it competed with someone else, it will try again)
    //   - Once a slot is found, it is initialized and returned

    // This function follows the following basic structure:
    //   - Is the table empty => if so, create an initial level in the table (or two
    //     depending on whether MT_OPTI_2LEVEL is used)
    //   - Is the table completely full (ie: no chance of finding any free slot) =>
    //     if so, insert an additional level *above* the existing head
    //   - If neither of these conditions hold, loop around until you find an empty
    //     slot by going down branches of the table/tree; the looping occurs if two
    //     workers somehow want to pick the same slot (rare since they start at different
    //     places)

    u8 toReturn = 0;
    *returnStrand = NULL;
    ocrWorker_t *worker = NULL;
    ocrTask_t *task;
    getCurrentEnv(&pd, &worker, &task, NULL);

    // We check if the properties are sane
    // This makes sure that no other bits than those allowed are set
    CHECK_RESULT(properties & ~(PDST_UHOLD), , toReturn |= OCR_EINVAL);

    pdStrandTableNode_t *leafToUse = NULL;
    // Fudge factor is initialized using the worker ID to make each worker look
    // in different places
    u32 fudgeFactor = (u32)(worker->id);
    u32 cachedLevelCount;
    u32 curLevel;

    // The while true is looping over whether we found a slot. We will break out once we have one
    while(true) {
        hal_lock(&(table->lock));
        // Look for a leaf to use
        leafToUse = NULL;
        curLevel = 1;
        cachedLevelCount = table->levelCount; // To be able to release the lock earlier

        // Is the table empty?
        if (table->levelCount == 0) {
            // If level is 0, it means that the table should be empty
            // We need to initialize it.
#ifdef MT_OPTI_2LEVEL
            //To maximize parallelism, we always build the
            // initial table with 2 levels so that during processing, workers never keep
            // the lock on the top-level node.
            ASSERT(table->head == NULL);
            pdStrandTableNode_t *tempHead = NULL; // Use a temporary because with the MT_OPTI_TABLELOCK, we don't
            // grab a lock when reading so can't write directly to head
            DPRINTF(DEBUG_LVL_VERB, "Table %p: empty -- adding non-leaf level 1 and one level 2 leaf\n",
                    table);
            // See BUG #899: this should be slab allocated
            // First allocate a non-leaf head node
            CHECK_MALLOC(tempHead = (pdStrandTableNode_t*)pd->fcts.pdMalloc
                        (pd, sizeof(pdStrandTableNode_t)), {hal_unlock(&(table->lock));});

            // We don't initialize any sub nodes. This is also always a non-leaf node
            CHECK_RESULT(
                toReturn |=_pdInitializeStrandTableNode(pd, table, tempHead, NULL, 0, 0, 0, 0),
                {pd->fcts.pdFree(pd, tempHead); hal_unlock(&(table->lock));},);

            // Now initialize the first leaf; other leaves will be created on demand
            CHECK_MALLOC(
                leafToUse = (pdStrandTableNode_t*)pd->fcts.pdMalloc(pd, sizeof(pdStrandTableNode_t)),
                {pd->fcts.pdFree(pd, tempHead); hal_unlock(&(table->lock))});

            // Set the table statistics
            table->levelCount = 2;
            curLevel = cachedLevelCount = 2;
            hal_lock(&(tempHead->lock)); // Lock needs to be held in initializeStrandTableNode
            CHECK_RESULT(
                toReturn |= _pdInitializeStrandTableNode(pd, table, leafToUse, tempHead, 0, 0, PDST_NODE_SIZE, IS_LEAF),
                {pd->fcts.pdFree(pd, leafToUse); pd->fcts.pdFree(pd, tempHead); hal_unlock(&(table->lock));},);

            // An error here indicates a runtime logic error
            RESULT_ASSERT(_pdSetStrandNodeAtIdx(pd, tempHead, 0, leafToUse, 0), ==, 0);
            hal_unlock(&(tempHead->lock));
            table->head = tempHead;
            hal_unlock(&(table->lock));

            hal_lock(&(leafToUse->lock));
            // Lock held here: leafToUse
            break; // We don't need to try anything anymore
#else
            ASSERT(table->head == NULL);
            DPRINTF(DEBUG_LVL_VERB, "Table %p: empty -- adding level 1\n",
                    table);
            pdStrandTableNode_t *tempHead = NULL; // Use a temporary because with the MT_OPTI_TABLELOCK, we don't
            // grab a lock when reading so can't write directly to head
            // See BUG #899: this should be slab allocated
            CHECK_MALLOC(tempHead = (pdStrandTableNode_t*)pd->fcts.pdMalloc
                         (pd, sizeof(pdStrandTableNode_t)), hal_unlock(&(table->lock)));
            CHECK_RESULT(
                toReturn |= _pdInitializeStrandTableNode(pd, table, tempHead, NULL, 0, 0, PDST_NODE_SIZE, IS_LEAF),
                         {hal_unlock(&(table->lock)); pd->fcts.pdFree(pd, tempHead);},);
            DPRINTF(DEBUG_LVL_VERB, "Table %p: added head %p\n", table, tempHead);
            table->levelCount = 1;
            cachedLevelCount = 1;
            leafToUse = tempHead;
            table->head = tempHead;
            hal_unlock(&(table->lock));
            hal_lock(&(leafToUse->lock)); // Need lock to be able to read nodeFree later in a race-free manner
                                            // (otherwise, another thread may grab the head)
            // Lock held here: leafToUse
            break;
#endif
        } else {
            // In this case, there is at least something in the table, go down the tree
            // to find something that is free. If nothing is found, we will create a new
            // leaf node
            // The common case is for something to exist and not re-needing the table->lock
            // lock to insert something so we release it here and will re-aquire if needed.
            pdStrandTableNode_t *curNode = table->head;
            hal_unlock(&(table->lock));

            hal_lock(&(curNode->lock)); // Need lock to check curNode->nodeFree

            // Is the table completely full?
            if(curNode->nodeFree == 0ULL) {
                // If the table is completely full, we create a new level

                // We don't really need the lock until later so we release for now, do
                // all our other stuff and then will re-grab when we link everything up.
                hal_unlock(&(curNode->lock));

                // No locks held here
                DPRINTF(DEBUG_LVL_VERB, "Table %p: fully loaded -- adding level %"PRIu32"\n",
                        table, cachedLevelCount + 1);
                pdStrandTableNode_t *newNode = NULL;
                // See BUG #899: this should be slab allocated
                CHECK_MALLOC(newNode = (pdStrandTableNode_t*)pd->fcts.pdMalloc
                             (pd, sizeof(pdStrandTableNode_t)), );

                // We don't initialize any sub nodes. This is also always a non-leaf node
                CHECK_RESULT(
                    toReturn |=_pdInitializeStrandTableNode(pd, table, newNode, NULL, 0, cachedLevelCount, 0, 0),
                    {pd->fcts.pdFree(pd, newNode); },);

                DPRINTF(DEBUG_LVL_VVERB, "Table %p: level 1 will now be %p (from %p)\n",
                        table, newNode, curNode);
                // We grab the lock on the table, check if curNode is still the same as
                // the head (to prevent two producers from creating a new head) and link
                // everything up.
                hal_lock(&(newNode->lock));
                hal_lock(&(table->lock));
                if(curNode == table->head) {
                    // We can proceed
                    hal_lock(&(curNode->lock));
                    // We need to "update" curNode to add the new parent
                    curNode->parent = newNode;
                    curNode->parentSlot = 0;
                    RESULT_ASSERT(_pdSetStrandNodeAtIdx(pd, newNode, 0, curNode, 0), ==, 0);
                    cachedLevelCount = ++table->levelCount;
                    table->head = newNode;
                    hal_unlock(&(table->lock));
                    // Check to make sure the state is still consistent
                    ASSERT((newNode->nodeFree & 1ULL) == (curNode->nodeFree != 0ULL));
#ifdef OCR_ASSERT
                    {
                        u32 i = 0;
                        for(; i < NP_COUNT; ++i) {
                            ASSERT((newNode->nodeNeedsProcess[i] & 1ULL) == (curNode->nodeNeedsProcess[i] != 0ULL));
                        }
                    }
#endif
                    ASSERT((newNode->nodeReady & 1ULL) == (curNode->nodeReady != 0ULL));

                    hal_unlock(&(curNode->lock));
                    // We hold newNode's lock. Set curNode to that so we will hold only curNode's lock
                    curNode = newNode;
                } else {
                    // Something switched so we don't change the head, we will restart looking
                    hal_unlock(&(table->lock));
                    pd->fcts.pdFree(pd, newNode);
                    continue;
                }
                // Lock held here: curNode
#ifndef MT_OPTI_2LEVEL
            } else if(table->levelCount == 1) {
                DPRINTF(DEBUG_LVL_VERB, "Table %p has one level with free space (%p)\n",
                        table, curNode);
                // If we have some free room and only one level, we know what to use
                // since a one level table always only has a leaf node
                leafToUse = curNode;
                // Lock held here: curNode/leafToUse
#endif
            } else {
#ifdef MT_OPTI_2LEVEL
                ASSERT(table->levelCount > 1); // We should never have a level of only 1
#endif
                DPRINTF(DEBUG_LVL_VERB, "Proceeding down table with curNode %p\n",
                        curNode);
            }

            // At this point, we hold the lock on curNode and leafToUse (if set)

            // At this point, the table has room (most likely). We start going down it
            // We have a good shot of having room in the table (but maybe not because
            // another thread may be competing with us)
            bool breakOut = false;
            while(!breakOut && (leafToUse == NULL)) {
                if(curNode->nodeFree == 0ULL) {
                    DPRINTF(DEBUG_LVL_VERB, "CurNode %p at level curLevel %"PRIu32" has no slots -- restarting\n",
                        curNode, curLevel);
                    hal_unlock(&(curNode->lock));
                    breakOut = true;
                    continue;
                }
                ASSERT(curLevel < cachedLevelCount); // We never go all the way to the leaf
                // Increase fudge factor if we are going to go back around.
                u32 freeSlot = selectFreeSlot(curNode, fudgeFactor++);

                pdStrandTableNode_t **node = &(curNode->data.nodes[freeSlot]);
                DPRINTF(DEBUG_LVL_VERB, "Found tentative free slot %"PRIu32" [%p] at level %"PRIu32" [%p]\n",
                        freeSlot, *node, curLevel, curNode);
                if (*node == NULL) {
                    // The node does not exist; this happens because we lazily create table nodes
                    // See BUG #899: This should be slab allocated
                    pdStrandTableNode_t *t = NULL;
                    CHECK_MALLOC(
                        t = (pdStrandTableNode_t*)pd->fcts.pdMalloc(pd, sizeof(pdStrandTableNode_t)),
                                                                    hal_unlock(&(curNode->lock)));
                    // If we are at the penultimate level, create a leaf node, otherwise
                    // create a regular one
                    if (curLevel == cachedLevelCount - 1) {
                        DPRINTF(DEBUG_LVL_VERB, "Initializing leaf-node %p at level %"PRIu32"\n",
                                t, curLevel+1);
                        CHECK_RESULT(
                            toReturn |= _pdInitializeStrandTableNode(pd, table, t, curNode,
                                                                     freeSlot, 0,
                                                                     PDST_NODE_SIZE, IS_LEAF),
                                     {pd->fcts.pdFree(pd, *node); hal_unlock(&(curNode->lock));},);
                        // An error here indicates a runtime logic error
                        RESULT_ASSERT(_pdSetStrandNodeAtIdx(pd, curNode, freeSlot, t, 0), ==, 0);
                        // In this case, we just created the node so we can reasonably grab the
                        // lock on it (instead of doing trylock). This breaks the usual
                        // order but will not cause a deadlock
                        leafToUse = *node = t;
                        hal_lock(&(leafToUse->lock));
                        hal_unlock(&(curNode->lock));
                        // Lock held here: leafToUse
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Initializing intermediate-node %p at level %"PRIu32"\n",
                                t, curLevel+1);
                        CHECK_RESULT(
                            toReturn |= _pdInitializeStrandTableNode(pd, table, t, curNode,
                                                                     freeSlot, cachedLevelCount - curLevel,
                                                                     0, 0),
                            {pd->fcts.pdFree(pd, t); hal_unlock(&(curNode->lock));},);
                        // An error here indicates a runtime logic error
                        RESULT_ASSERT(_pdSetStrandNodeAtIdx(pd, curNode, freeSlot, t, 0), ==, 0);
                        // Same logic as above, we grab the lock out of order but it will not
                        // cause a deadlock. We can avoid the try lock scenario here.
                        hal_lock(&(t->lock));
                        *node = t;
                        hal_unlock(&(curNode->lock));
                        curNode = t;
                        // Lock held here: curNode (the new one)
                    }
                } else {
                    pdStrandTableNode_t *tentativeChild = *node;
                    // Release the parent, try to grab the child. We use a trylock because
                    // others can be competing and we want to go somewhere else if there is
                    // contention here
                    hal_unlock(&(curNode->lock));
                    if(hal_trylock(&(tentativeChild->lock)) == 0) {
                        // Whouhou, we are in
                        if(curLevel == cachedLevelCount - 1) {
                            leafToUse = tentativeChild;
                        } else {
                            curNode = tentativeChild;
                        }
                    } else {
                        breakOut = true;
                        continue;
                    }
                }
                ++curLevel;
            }
            if(breakOut) {
                ASSERT(leafToUse == NULL);
                continue;
            }
            ASSERT(leafToUse != NULL);
            break;
        }
    }
    // At this point:
    //  - we hold the lock on leafToUse (and nothing else)
    //  - there is room to add a strand
    // In this implementation, the leaf nodes are always fully initialized
    // so any child will exist. This can be changed by changing the parameters
    // to _pdInitializeStrandTableNode if needed (for example, only create all of
    // them for the first leaf node and then don't create any or just half).

    // We should have room in our leaf
    ASSERT(leafToUse);
    ASSERT(leafToUse->nodeFree);
    ASSERT(curLevel == cachedLevelCount); // We should be at the leaf level

#ifdef MT_OPTI_2LEVEL
    ASSERT(curLevel > 1); // We never have just one level
#endif

    u32 freeSlot = selectFreeSlot(leafToUse, fudgeFactor);

    pdStrand_t *strand = leafToUse->data.slots[freeSlot];
    DPRINTF(DEBUG_LVL_VERB, "Found free strand %"PRIu32" [%p] at leaf level %"PRIu32" [%p]\n",
            freeSlot, strand, curLevel, leafToUse);

    // All strands should be initialized here.
    ASSERT(strand);

    // The strand should be free
    RESULT_ASSERT(_pdLockStrand(strand, 0), ==, 0);

    strand->curEvent = event;
    if(event) {
        RESULT_ASSERT(pdReferenceEvent(pd, event), ==, 0)
    }
    strand->actions = NULL;
    strand->bufferedActions = NULL;
    strand->processingWorker = NULL;
    strand->contextTask = task;
    strand->properties |= PDST_RHOLD |
        (((event->properties & PDEVT_READY) != 0)?0:PDST_WAIT_EVT);
    strand->properties |= properties;
    DPRINTF(DEBUG_LVL_VVERB, "Strand %p: event: %p | actions: %p | props: 0x%"PRIx32"\n",
            strand, strand->curEvent, strand->actions, strand->properties);
    ASSERT(hal_islocked(&(strand->lock)));
    // Now set the value for the event
    event->strand = strand;
    *returnStrand = strand;

    // Now set the proper bits in the bit vectors and go up the chain
    // and update their bits if needed as well
    leafToUse->nodeFree &= ~(1ULL<<freeSlot);

    // If this was the last free slot, we need to
    // update our parent to say that we no longer have
    // any free slots
    u8 propagateFree = leafToUse->nodeFree == 0ULL;
    u8 propagateReady = false;

    // After inserting an event in a new strand, there
    // is definitely no way for the event to need processing
    // (which requires actions to process)

    // It can be ready though
    if((strand->properties & PDST_WAIT) == 0) {
        propagateReady = leafToUse->nodeReady == 0ULL;
        leafToUse->nodeReady |= (1ULL<<freeSlot);
    }

    pdStrandTableNode_t *curNode = leafToUse;
    pdStrandTableNode_t *parent = leafToUse->parent;
    ASSERT(hal_islocked(&(curNode->lock)));
    // We propagate only until we have nothing left to change
    PROPAGATE_UP_TREE(curNode, parent, 0, propagateReady || propagateFree, {
            if(propagateFree) {
                // We say that we no longer have free slots and
                // we only propagate if this was also our parent's
                // last free slot
                // The following assert should always be true because
                // even if there is another thread adding free slots (through
                // the destruction of strands), we don't release curNode (old parent) between
                // the time we check the condition to set propagateFree and the time
                // we come to check the ASSERT
                ASSERT(curNode->nodeFree == 0ULL);
                parent->nodeFree &= ~(1ULL<<curNode->parentSlot);
                propagateFree = parent->nodeFree == 0ULL;
            }

            if (propagateReady) {
                propagateReady = parent->nodeReady == 0ULL;
                parent->nodeReady |= (1ULL << curNode->parentSlot);
            }
        });
    ASSERT(hal_islocked(&(strand->lock)));
END_LABEL(getNewStrandEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdGetNewStrand -> %"PRIu32" [strand: %p]\n",
            toReturn, *returnStrand);
    return toReturn;
#undef _END_FUNC
}


u8 pdGetStrandForIndex(ocrPolicyDomain_t* pd, pdStrand_t **returnStrand, pdStrandTable_t* table,
                       u64 index) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdGetStrandForIndex(pd:%p, strand**:%p [%p], table:%p, idx:%"PRIu64")\n",
            pd, returnStrand, *returnStrand, table, index);
#define _END_FUNC getStrandForIndex

    u8 toReturn = 0;
    *returnStrand = NULL;

    if (pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    // We will just go down the tree until we find the strand we need
    // The lock is required here to make sure we read a consistent
    // head and level.
    hal_lock(&(table->lock));
    u32 maxLevel = table->levelCount;
    pdStrandTableNode_t *curNode = table->head;
    u32 curIndex;
    hal_unlock(&(table->lock));
    if (maxLevel == 0) {
        DPRINTF(DEBUG_LVL_WARN, "Table empty; index %"PRIu64" not found\n", index);
        CHECK_RESULT_T(false, , toReturn = OCR_EINVAL);
    }
    u32 curLevel = 1;
#define _LVL_MASK ((1ULL<<BV_SIZE_LOG2) - 1)
    if (index > (1ULL<<(maxLevel*BV_SIZE_LOG2))) {
        DPRINTF(DEBUG_LVL_WARN, "Table has only %"PRIu32" levels; cannot contain %"PRIu64"\n",
                maxLevel, index);
        CHECK_RESULT_T(false, , toReturn = OCR_EINVAL);
    }
    // At this point, we are pretty sure that the index is valid in the table
    while (curLevel < maxLevel) {
        curIndex = (index >> (BV_SIZE_LOG2*(maxLevel-curLevel))) & _LVL_MASK;
        curNode = curNode->data.nodes[curIndex];
        ++curLevel;
    }
    // Now extract the strand at the last level
    *returnStrand = curNode->data.slots[index & _LVL_MASK];

    // This makes sure that the strand actually has the proper index
    ASSERT((*returnStrand)->index == index);

END_LABEL(getStrandForIndex)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdStrandForIndex -> %"PRIu32" [strand: %p]\n",
            toReturn, *returnStrand);
    return toReturn;
#undef _LVL_MASK
#undef _END_FUNC
}


u8 pdEnqueueActions(ocrPolicyDomain_t *pd, pdStrand_t* strand, u32 actionCount,
                    pdAction_t** actions, u8 clearFwdHold) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdEnqueueActions(pd:%p, strand:%p, count:%"PRIu32", actions**:%p [%p], clearHold:%"PRIu32")\n",
            pd, strand, actionCount, actions, actions?*actions:NULL, clearFwdHold);
#define _END_FUNC enqueueActionsEnd

    u8 toReturn = 0;

    if (pd == NULL) {
        getCurrentEnv(&pd, NULL, NULL, NULL);
    }

    // Basic sanity checks
    // !(actionCount == 0 || actions != NULL)
    CHECK_RESULT_T(actionCount == 0 || actions != NULL, , toReturn = OCR_EINVAL);

    u8 needUnlock = 0; // Will be 1 for the regular lock or 2 for the buffered one
    if(hal_islocked(&(strand->lock))) {
        DPRINTF(DEBUG_LVL_VERB, "Strand is locked -- regular enqueue\n");
        toReturn = _pdDoEnqueue(pd, &(strand->actions), actionCount, actions);
        CHECK_RESULT(toReturn, ,);
    } else {
        if(hal_trylock(&(strand->lock))) {
            DPRINTF(DEBUG_LVL_VERB, "Strand was not locked but successfully locked -- regular enqueue\n");
            toReturn = _pdDoEnqueue(pd, &(strand->actions), actionCount, actions);
            CHECK_RESULT(toReturn, hal_unlock(&(strand->lock)),);
            needUnlock = 1;
        } else {
            DPRINTF(DEBUG_LVL_VERB, "Strand was not locked and could not acquire -- buffered enqueue\n");
            hal_lock(&(strand->bufferedLock));
            toReturn = _pdDoEnqueue(pd, &(strand->bufferedActions), actionCount, actions);
            CHECK_RESULT(toReturn, hal_unlock(&(strand->bufferedLock)), );
            needUnlock = 2;
        }
    }

    // At this point, we enqueued the actions.

    if(needUnlock < 2) {
        // Basically we enqueued normally
        ASSERT(hal_islocked(&(strand->lock)));

        u32 npIdxCounter = 0;
        u8 npIdx[NP_COUNT];
        _pdActionToNP(npIdx, actions[0]);

        if (arrayDequeSize(strand->actions) == actionCount) {
            // This means that no actions were pending
            DPRINTF(DEBUG_LVL_VVERB, "Strand %p had no actions [props: 0x%"PRIx32"] -> setting WAIT_ACT\n",
                strand, strand->properties);
            ASSERT((strand->properties & PDST_WAIT_ACT) == 0);
            strand->properties |= PDST_WAIT_ACT;
            DPRINTF(DEBUG_LVL_VVERB, "Strand %p [props: 0x%"PRIx32"]\n", strand,
                strand->properties);

            if((strand->properties & PDST_WAIT_EVT) == 0) {
                // Check if the event was also ready. If that's
                // the case, we need to switch to being *not* ready
                // We need to propagate that back up the tree
                pdStrandTableNode_t *curNode = strand->parent;
                ASSERT(curNode);
                pdStrandTableNode_t *parent = curNode->parent;
                u32 stIdx = strand->index & ((1<<BV_SIZE_LOG2)-1);
                bool propagateReady = false, propagateNP = false;

                hal_lock(&(curNode->lock));
                // We need processing
                while(npIdx[npIdxCounter]) {
                    u8 tIdx = npIdx[npIdxCounter] - 1;
                    propagateNP |= curNode->nodeNeedsProcess[tIdx] == 0ULL;
                    curNode->nodeNeedsProcess[tIdx] |= (1ULL<<stIdx);
#ifdef MT_OPTI_CONTENTIONLIMIT
                    u32 t __attribute__((unused)) = hal_xadd32(&(strand->containingTable->consumerCount[tIdx]), 1);
                    DPRINTF(DEBUG_LVL_VVERB, "Incrementing consumerCount[%"PRIu32"] @ table %p (enqueueActions); was %"PRId32"\n",
                        tIdx, strand->containingTable, t);
#endif
                    ++npIdxCounter;
                }

                // We are no longer ready
                ASSERT((curNode->nodeReady & (1ULL<<stIdx)) != 0);
                curNode->nodeReady &= ~(1ULL<<stIdx);
                propagateReady = (curNode->nodeReady == 0ULL);
                ASSERT(hal_islocked(&(curNode->lock)));

                // In this case, we flipped:
                // NP from 0 to 1 (stop when we see a 1)
                // Ready from 1 to 0 (stop when sibblings have ready nodes)
                u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                if(npIdx[1]) {
                    DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                }
                PROPAGATE_UP_TREE(
                    curNode, parent, tNpIdx,
                    propagateReady || propagateNP, {
                        if (propagateReady) {
                            parent->nodeReady &= ~(1ULL<<curNode->parentSlot);
                            propagateReady = parent->nodeReady == 0ULL;
                        }
                        if (propagateNP) {
                            propagateNP = false;
                            u32 i;
                            for(i=0; i<NP_COUNT; ++i) {
                            if(npIdx[i]) { // npIdx[i] == 0 means nothing to propagate at that position
                                if(parent->nodeNeedsProcess[npIdx[i]-1] == 0ULL) {
                                    parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                    propagateNP = true;
                                } else {
                                    parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                    npIdx[i] = 0;
                                }
                            }
                        }
                    }
                });
            }
        }

        if (clearFwdHold) {
            DPRINTF(DEBUG_LVL_VERB, "Clearing fwd hold on %p [props: 0x%"PRIx32"]\n",
                strand, strand->properties);
            if ((strand->properties & PDST_RHOLD) == 0) {
                DPRINTF(DEBUG_LVL_WARN, "Clearing non-existant hold on %p [props: 0x%"PRIx32"] -- possible race!!\n",
                    strand, strand->properties);
            }
            strand->properties &= ~PDST_RHOLD;
        }
        if(needUnlock)
            hal_unlock(&(strand->lock));
    } else {
        // We did not grab the lock. Just check the clearing of the hold and buffer that if necessary
        if(clearFwdHold) {
            if(strand->bufferedHoldClear) {
                DPRINTF(DEBUG_LVL_WARN, "Clearning non-existant buffer hold on %p -- possible race!!\n", strand);
            }
            strand->bufferedHoldClear = true;
        }
        hal_unlock(&(strand->bufferedLock));
    }
END_LABEL(enqueueActionsEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdEnqueueActions -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}


u8 pdReplaceCurrentAction(ocrPolicyDomain_t *pd, pdStrand_t* strand, pdAction_t *action) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdReplaceCurrentAction(pd:%p, strand:%p, action*:%p)\n",
            pd, strand, action);
#define _END_FUNC replaceCurrentActionEnd

    u8 toReturn = 0;

    ocrWorker_t *curWorker = NULL;
    if (pd == NULL) {
        getCurrentEnv(&pd, &curWorker, NULL, NULL);
    } else {
        getCurrentEnv(NULL, &curWorker, NULL, NULL);
    }

    // Basic sanity checks
    // action != NULL && action != NULL
    CHECK_RESULT_T(action != NULL && strand != NULL, , toReturn = OCR_EINVAL);

    // A lock should be held while we enqueue actions. Make sure it is. If this
    // fails, most likely an internal runtime error
    ASSERT(hal_islocked(&(strand->lock)));

    // We should be the worker processing the strand
    CHECK_RESULT_T(curWorker == strand->processingWorker, , toReturn = OCR_EINVAL);

    // We need to have strand->actions that exists. If not, this is probably a runtime
    // error again
    ASSERT(strand->actions);

    DPRINTF(DEBUG_LVL_VERB, "Going to enqueue at HEAD on queue %p\n", strand->actions);
    DPRINTF(DEBUG_LVL_VVERB, "Pushing at head action %p\n", action);
    arrayDequePushAtHead(strand->actions, (void*)action);

    // No state update needs to happen since it will happen after we are done processing stuff
    // This is called if we are in pdProcessAction.

END_LABEL(replaceCurrentActionEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdReplaceCurrentAction -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}


u8 pdLockStrand(pdStrand_t *strand, bool doTry) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdLockStrand(strand:%p, doTry:%"PRIu32")\n",
            strand, doTry);
#define _END_FUNC lockStrandEnd

    u8 toReturn = 0;
    toReturn = _pdLockStrand(strand, doTry?0:BLOCK);

END_LABEL(lockStrandEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdLockStrand -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

u8 pdUnlockStrand(pdStrand_t *strand) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER pdUnlockStrand(strand:%p)\n",
            strand);
#define _END_FUNC unlockStrandEnd

    u8 toReturn = 0;
    CHECK_RESULT_T(hal_islocked(&(strand->lock)), , toReturn = OCR_EINVAL);

    // We figure out if there is a need to check for buffered actions and
    // add them to the action list if that is the case
    // This is a while loop to catch the highly unlikely case where things keep getting buffered
    // while we are adding the previously buffered actions
    if(strand->bufferedActions) {
        ocrPolicyDomain_t *pd = NULL;
        getCurrentEnv(&pd, NULL, NULL, NULL);
        toReturn = _pdDoRequeue(pd, strand);
    }
    _pdUnlockStrand(strand);

END_LABEL(unlockStrandEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT pdUnlockStrand -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}


/***************************************/
/****** Global scheduler functions *****/
/***************************************/


u8 pdProcessStrands(ocrPolicyDomain_t *pd, u32 processType, u32 properties) {

    if(processType >= NP_COUNT) {
        DPRINTF(DEBUG_LVL_WARN, "Invalid value for processType in pdProcessStrands: %"PRIu32"\n",
                processType);
        return OCR_EINVAL;
    }
    if(properties != 0 && properties != PDSTT_EMPTYTABLES) {
        DPRINTF(DEBUG_LVL_WARN, "Invalid value for properties in pdProcessStrands: %"PRIu32"\n",
                processType);
        return OCR_EINVAL;
    }

    if(_pdProcessNStrands(pd, processType, PDPROCESS_MAX_COUNT, properties) != (u32)-1)
        return 0;
    return OCR_EAGAIN;
}

u8 pdProcessNStrands(ocrPolicyDomain_t *pd, u32 processType, u32 *count, u32 properties) {

    if(processType >= NP_COUNT) {
        DPRINTF(DEBUG_LVL_WARN, "Invalid value for processType in pdProcessNStrands: %"PRIu32"\n",
                processType);
        return OCR_EINVAL;
    }
    if(properties != 0) {
        DPRINTF(DEBUG_LVL_WARN, "Invalid value for properties in pdProcessNStrands: %"PRIu32"\n",
                processType);
        return OCR_EINVAL;
    }
    u32 r = _pdProcessNStrands(pd, processType, *count, properties);
    if(r != (u32)-1) {
        *count = r;
        return 0;
    }
    *count = 0;
    return OCR_EAGAIN;
}

u32 _pdProcessNStrands(ocrPolicyDomain_t *pd, u32 processType, u32 count, u32 properties) {
    DPRINTF(DEBUG_LVL_VERB, "ENTER _pdProcessNStrands(pd:%p, type:%"PRIu32", count:%"PRIu32", props:0x%"PRIx32")\n",
            pd, processType, count, properties);
#define _END_FUNC processNStrandsEnd

    ASSERT(processType < NP_COUNT);
    ASSERT(pd);

    /* In this function, we are currently very dumb and follow a simple algorithm.
     * In the future, this could be extended to having a plug-in model to write
     * what is basically a very fast scheduler for micro-tasks. I don't want to have
     * a full-fledged module model here because this part is performance sensitive
     * and I also do not think there are that many possibilities for such a "simple"
     * scheduler.
     * Ideas considered for the future:
     *    - some sort of callback to determine priorities
     *    - a better determination of how much work to do in one of these calls
     *    - dynamic throttling (related to previous point) or priorities
     *
     * Current algorithm:
     *    - look at all tables in the policy domain and for each table do the following:
     *    - look for strands that require action processing up to X (compile time
     *      constant) times.
     *    - for all such strands, process their action queue until stuck again
     */


    u32 i = 0;
#ifdef MT_OPTI_CONTENTIONLIMIT
    u32 j = 0;
#endif
    u32 processCount = 0;
    u32 curLevel = 1;
    ocrWorker_t *worker = NULL;
    getCurrentEnv(NULL, &worker, NULL, NULL);
    ocrTask_t *savedTask = worker->curTask;

    // Structure to figure out what we can process
    u32 npIdxCounter = 0;
    u8 npIdx[NP_COUNT];

    // We iterate over tables but still look for work of type 'processType'
    // This is because once a strand is created, it cannot move from table to table
    // as its index and table ID are used as handles when things wait on it. However,
    // a strand may have communication work, computation work, etc. so we need to distinguish
    // these two things. It may be that having multiple tables does not bring much and we will
    // reduce to just one table.
    for (; i < PDSTT_LAST; ++i) {
        pdStrandTable_t *table = pd->strandTables[i];
        DPRINTF(DEBUG_LVL_VERB, "Looking at table %p [idx: %"PRIu32"]\n", table, i);
        processCount = 0;
#ifndef MT_OPTI_LOCKTABLE
        hal_lock(&(table->lock));
#endif
        pdStrandTableNode_t *curNode = table->head;
#ifndef MT_OPTI_LOCKTABLE
        hal_unlock(&(table->lock));
#endif
        if (curNode == NULL) {
            DPRINTF(DEBUG_LVL_VVERB, "Table empty -- continuing to next table\n");
            continue; // We don't even have a head so we really don't have much to do
        }
#ifdef MT_OPTI_CONTENTIONLIMIT
        if(table->consumerCount[processType] < 1) {
            DPRINTF(DEBUG_LVL_VVERB, "Limiting contention -- not enough work\n");
            continue;
        }
        {
            u32 contentionCount = hal_xadd32(&(table->consumerCount[processType]), -1);
            if(contentionCount <= 0) {
                // Oops, too many people grabbed things; we re-increment and go-away
                hal_xadd32(&(table->consumerCount[processType]), 1);
                DPRINTF(DEBUG_LVL_VVERB, "Limiting contention (2) -- not enough work\n");
                continue;
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "Entering processing loop; consumerCount[%"PRIu32"] @ table %p was %"PRId32"\n",
                    processType, table, contentionCount);
            }
        }

        s32 changeConsumerCount[NP_COUNT]; // We update the consumer count only once at the end
        for(j = 0; j < NP_COUNT; ++j) changeConsumerCount[j] = 0;
        j = 0;
        changeConsumerCount[processType] = 1; // We offset for the initial -1 when we enter
#endif
        pdStrandTableNode_t *origHead = curNode;
        hal_lock(&(curNode->lock));

        // This is similar to the fudgeFactor for getNewStrand. Same concept
        u32 fudgeFactor = (u32)(worker->id);
        // Continue "forever" if emptytables or just until the maximum count is reached
        while ((properties & PDSTT_EMPTYTABLES) || (processCount < count)) {
            // Go down the tree and see if we have nodeNeedsProcess set anywhere
            // Note that if multiple threads show up, the lock will serialize them
            // and they will each pick a different path ensuring at most 64 way parallelism
            // in this endeavor. This can also happen concurrently with adding new strands and
            // what not
            ASSERT(curNode);
            ASSERT(hal_islocked(&(curNode->lock)));
            DPRINTF(DEBUG_LVL_VERB, "Looking at node %p with children nodeNeedsProcess[%"PRIu32"] = [0x%"PRIx64"]\n",
                curNode, processType, curNode->nodeNeedsProcess[processType]);
            if (curNode->nodeNeedsProcess[processType]) {
                u32 processSlot = selectProcessSlot(curNode, processType, fudgeFactor);
                pdStrandTableNode_t *tentativeChild = NULL;
                if(!IS_LEAF_NODE(curNode->lmIndex)) {
                    // This is not a leaf node so we attempt to go down
                    tentativeChild = curNode->data.nodes[processSlot];
                    ASSERT(tentativeChild);
                    hal_unlock(&(curNode->lock));

                    if(hal_trylock(&(tentativeChild->lock)) == 0) {
                        // We are in, continue down
                        DPRINTF(DEBUG_LVL_VERB, "Going down slot %"PRIu32" to %p\n", processSlot, tentativeChild);
                        curNode = tentativeChild;
                        ++curLevel;
                        continue;
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Could not go down slot %"PRIu32" -- restarting at top\n", processSlot);
                        curNode = origHead;
                        hal_lock(&(curNode->lock));
                        ++fudgeFactor;
                        curLevel = 1;
                        continue;
                    }
                } else {
                    // If we are a leaf, we want to check if this is the last needProcess and if so
                    // we want to go back up and flip the bits properly. We also check for all the
                    // nodeNeedsProcess that have been flipped for this action
                    // Note that without the lock, this is still safe to read the first action
                    // because there is at least one (it needs processing) and even adding to
                    // the tail will not break the head (in this implementation).
                    pdStrand_t *toProcess = curNode->data.slots[processSlot];
                    pdAction_t *curAction = NULL;
                    bool propagateNP = false;
                    ASSERT(arrayDequeSize(toProcess->actions) > 0);
                    RESULT_ASSERT(arrayDequePeekFromHead(toProcess->actions, (void**)&curAction), ==, 0);
                    _pdActionToNP(npIdx, curAction);
#ifdef OCR_ASSERT
                    npIdxCounter = 0;
                    while(npIdx[npIdxCounter]) {
                        if(npIdx[npIdxCounter] -1 == processType) {
                            npIdxCounter = (u32)-1;
                            break;
                        }
                        ++npIdxCounter;
                    }
                    // Somehow we are trying to process something that we shouldn't be. Runtime error most likely
                    if(npIdxCounter != (u32)-1)
                        ASSERT(0);
#endif
                    npIdxCounter = 0;
                    while(npIdx[npIdxCounter]) {
                        u8 tIdx = npIdx[npIdxCounter] - 1;
                        curNode->nodeNeedsProcess[tIdx] &= ~(1ULL<<processSlot);
                        if(curNode->nodeNeedsProcess[tIdx] == 0ULL) {
                            propagateNP = true;
                        } else {
                            npIdx[npIdxCounter] = 0;
                        }
#ifdef MT_OPTI_CONTENTIONLIMIT
                        changeConsumerCount[tIdx] -= 1;
#endif
                        ++npIdxCounter;
                    }

                    pdStrandTableNode_t *t = curNode;
                    pdStrandTableNode_t *parent = curNode->parent;

                    u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                    if(npIdx[1]) {
                        DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                    }
                    PROPAGATE_UP_TREE(curNode, parent, tNpIdx, propagateNP, {
                        propagateNP = false;
                        u32 i;
                        for(i=0; i<NP_COUNT; ++i) {
                            if(npIdx[i]) { // npIdx[i] == 0 means nothing to propagate at that position
                                // The following assert should always be true because
                                // even if something else is modifying nodeNeedsProcess at the same time,
                                // we don't release curNode (old parent) between the time
                                // we check the condition to set propagateNP and the time
                                // we check with the assert
                                ASSERT(curNode->nodeNeedsProcess[npIdx[i]-1] == 0ULL);
                                parent->nodeNeedsProcess[npIdx[i]-1] &= ~(1ULL<<curNode->parentSlot);
                                if(parent->nodeNeedsProcess[npIdx[i]-1] == 0ULL) {
                                    propagateNP = true;
                                } else {
                                    npIdx[i] = 0;
                                }
                            }
                        }
                    });
                    // Reset curNode to the proper value
                    curNode = t;
                }
                // If we are here, we are in a leaf node so we may have found something
                // to process
                // We hold no locks here
#ifdef MT_OPTI_2LEVEL
                ASSERT(curLevel > 1);
#endif
                // These are both safe to read without lock
                ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<processSlot)) == 0);
                pdStrand_t *toProcess = curNode->data.slots[processSlot];
                DPRINTF(DEBUG_LVL_VERB, "Found strand %p in slot %"PRIu32" and level %"PRIu32"\n",
                        toProcess, processSlot, curLevel);

                // Grab the lock on the strand
                RESULT_ASSERT(_pdLockStrand(toProcess, BLOCK), ==, 0);

                // At this point, we found the strand to process so we can actually
                // go and process each action. We hold no locks except on the strand
                // We mark the strand as being processed
                toProcess->processingWorker = worker;

                // We save and set the context of this thread as well so we can
                // use getCurrentEnv as if we were executing the EDT from which we originate
                // First some sanity checks: if the node needed processing, it should be in this state
                worker->curTask = toProcess->contextTask;
                ASSERT((toProcess->properties & PDST_WAIT) == PDST_WAIT_ACT);

                // This outer processing loop is to deal with the case that we have actions
                // buffering up in bufferedActions.
                u8 outerBreak = false;
                while(true) {
                    outerBreak = false;
                    // We loop while the event is ready and there is stuff to do
                    // Note that the actions may make the event not ready thus the importance
                    // of checking every time
                    while (((toProcess->properties & PDST_WAIT_EVT) == 0) &&
                        arrayDequeSize(toProcess->actions)) {

                        pdAction_t *curAction = NULL;
                        bool canProcess = false;
                        RESULT_ASSERT(arrayDequePeekFromHead(toProcess->actions, (void**)&curAction), ==, 0);
                        ASSERT(curAction);
                        npIdxCounter = 0;
                        _pdActionToNP(npIdx, curAction);
                        while(npIdx[npIdxCounter]) {
                            if((npIdx[npIdxCounter] - 1) == processType) {
                                canProcess = true;
                                break;
                            }
                            ++npIdxCounter;
                        }
                        if(canProcess) {
                            pdAction_t *tAction __attribute__((unused)) = NULL;
                            RESULT_ASSERT(arrayDequePopFromHead(toProcess->actions, (void**)&tAction), ==, 0);
                            ASSERT(tAction == curAction);
                            DPRINTF(DEBUG_LVL_VERB, "Processing action %p\n", curAction);
                            RESULT_ASSERT(_pdProcessAction(pd, worker, toProcess, curAction, 0), ==, 0);
                            DPRINTF(DEBUG_LVL_VERB, "Done processing action %p\n", curAction);
                        } else {
                            DPRINTF(DEBUG_LVL_VERB, "Action %p is of the wrong type (needed %"PRIu32") -- leaving as is\n",
                                curAction, processType);
                            // Whatever happens, we can't go further since this action cannot be processed
                            outerBreak = true;
                            break;
                        }
                    }

                    // At this point, we check to see if we have actions that we can add
                    // We check regardless of outerBreak because we will use the cheaper
                    // unlock later on

                    // If we broke out of the inner loop because we couldn't process the action, we still can't
                    // process it now
                    if(outerBreak)
                        break;
                }
                worker->curTask = savedTask;

                toProcess->processingWorker = NULL;
                toProcess->properties &= ~PDST_MODIFIED;

                // Update properties
                hal_lock(&(curNode->lock));
                bool propagateReady = false, propagateNP = false, didFree = false;
                if(arrayDequeSize(toProcess->actions) == 0) {
                    toProcess->properties &= ~(PDST_WAIT_ACT);
                    if((toProcess->properties & PDST_WAIT_EVT) == 0) {
                        DPRINTF(DEBUG_LVL_VERB, "Strand %p now ready\n", toProcess);

                        // Some sanity checks: we should not need processing (since we were just
                        // processing) and we should not be ready (since we needed processing)
                        ASSERT((curNode->nodeReady & (1ULL<<processSlot)) == 0);
                        ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<processSlot)) == 0);

                        // We check if there is a hold on the strand -- if so, we leave in the
                        // strand table and propagate that. Otherwise, we remove it
                        if ((toProcess->properties & PDST_HOLD) == 0) {
                            // We can free the strand now
                            DPRINTF(DEBUG_LVL_VERB, "Freeing strand %p [idx %"PRIu64"] after processing actions\n",
                                    toProcess, toProcess->index);
                            // Unlock the curNode because it will be locked again in _pdDestroyStrand
                            hal_unlock(&(curNode->lock));
                            RESULT_ASSERT(_pdDestroyStrand(pd, toProcess), ==, 0);
                            didFree = true;
                        } else {
                            // Here, we propagate the ready flag only if we are the first ready
                            // node
                            propagateReady = curNode->nodeReady == 0ULL;
                            curNode->nodeReady |= (1ULL<<processSlot);
                            DPRINTF(DEBUG_LVL_VERB, "Strand %p is ready but has a hold -- leaving as is\n",
                                    toProcess);
                        }
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Strand %p is not ready and has no actions\n",
                                toProcess);
                        // We don't have anything to set here
                        ASSERT((curNode->nodeReady & (1ULL<<processSlot)) == 0);
                        ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<processSlot)) == 0);
                        ASSERT((curNode->nodeReady & (1ULL<<processSlot)) == 0);
                    }
                } else {
                    ASSERT(toProcess->properties & PDST_WAIT_ACT);
                    if((toProcess->properties & PDST_WAIT_EVT) == 0) {
                        DPRINTF(DEBUG_LVL_VERB, "Strand %p still has pending actions that need processing\n",
                                toProcess);
                        // We need to check by whom these actions need to be processed; in other words, there
                        // may be actions left because we are not processing actions of this type. We need
                        // to set the proper nodeNeedsProcess flag
                        // We already updated npIdx with the proper types for this action (that's when
                        // we figured out we couldn't process it)
                        npIdxCounter = 0;
                        propagateNP = false;
                        while(npIdx[npIdxCounter]) {
                            u8 tIdx = npIdx[npIdxCounter] - 1;
                            DPRINTF(DEBUG_LVL_VVERB, "Next action can be processed by type %"PRIu32"\n", tIdx);
                            propagateNP |= curNode->nodeNeedsProcess[tIdx] == 0ULL;
                            curNode->nodeNeedsProcess[tIdx] |= (1ULL<<processSlot);
#ifdef MT_OPTI_CONTENTIONLIMIT
                            changeConsumerCount[tIdx] += 1;
#endif
                            ++npIdxCounter;
                        }

                        ASSERT((curNode->nodeReady & (1ULL<<processSlot)) == 0);
                    } else {
                        DPRINTF(DEBUG_LVL_VERB, "Strand %p has pending actions but not ready\n",
                                toProcess);
                        // Nothing to do again since it does not need processing and is not ready
                        ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<processSlot)) == 0);
                        ASSERT((curNode->nodeReady & (1ULL<<processSlot)) == 0);
                    }
                }

                // Holding curNode->lock (except if didFree) and strand lock
                // We can use the cheap unlock because we already checked for buffered actions
                if(!didFree)
                    _pdUnlockStrand(toProcess);

                // Holding lock on curNode->lock EXCEPT if freed strand
                // (in that case, the following if statement is false)
                if (propagateReady || propagateNP) {
                    ASSERT(!didFree);
                    DPRINTF(DEBUG_LVL_VERB, "Propagating properties: ready: %"PRIu32"; np: %"PRIu32"\n",
                            propagateReady, propagateNP);

                    pdStrandTableNode_t *parent = curNode->parent;
                    ASSERT(hal_islocked(&(curNode->lock)));
                    // We flip nodeReady from 0 to 1; to up until we see a 1
                    // We flip nodeNeedsProcessing from 0 to 1; same as above
                    u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                    if(npIdx[1]) {
                        DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                    }
                    PROPAGATE_UP_TREE(
                        curNode, parent, tNpIdx,
                        propagateReady || propagateNP, {
                            if (propagateReady) {
                                propagateReady = parent->nodeReady == 0ULL;
                                parent->nodeReady |= (1ULL<<curNode->parentSlot);
                            }
                            if (propagateNP) {
                                propagateNP = false;
                                u32 i;
                                for(i=0; i<NP_COUNT; ++i) {
                                    if(npIdx[i]) {
                                        if(parent->nodeNeedsProcess[npIdx[i]-1]) {
                                            parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                            propagateNP = true;
                                        } else {
                                            parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                            npIdx[i] = 0;
                                        }
                                    }
                                }
                            }
                        });
                } else {
                    if(!didFree)
                        hal_unlock(&(curNode->lock));
                }

                // We processed a node so we go and look for the next one
                ++processCount;
#ifndef MT_OPTI_LOCKTABLE
                hal_lock(&(table->lock));
#endif
                curNode = table->head;
#ifndef MT_OPTI_LOCKTABLE
                hal_unlock(&(table->lock));
#endif
                ASSERT(curNode); // The table can't empty out from under us
                hal_lock(&(curNode->lock));
            } else {
                // Nothing left to process
                DPRINTF(DEBUG_LVL_VERB, "No actions to process -- breaking out after %"PRIu32"\n",
                        processCount);
                break; // Breaks out of processing loop
            }
        } /* End of while loop in one table */
#ifdef MT_OPTI_CONTENTIONLIMIT
        for(j=0; j<NP_COUNT; ++j) {
            if(changeConsumerCount[j]) {
                DPRINTF(DEBUG_LVL_VVERB, "Changing consumerCount[%"PRIu32"] for table @ %p by %"PRId32"\n",
                        j, table, changeConsumerCount[j]);
                hal_xadd32(&(table->consumerCount[j]), changeConsumerCount[j]);
            }
        }
#endif
        // At the end of the loop, we always hold the lock on curNode so
        // we release it
        hal_unlock(&(curNode->lock));
    } /* End of for loop on tables */

END_LABEL(processNStrandsEnd)
    DPRINTF(DEBUG_LVL_VERB, "EXIT _pdProcessNStrands -> %"PRIu32"\n", processCount);
    return processCount;
#undef _END_FUNC
}

u8 pdProcessResolveEvents(ocrPolicyDomain_t *pd, u32 processType, u32 count, pdEvent_t **events,
                          u32 properties) {
    DPRINTF(DEBUG_LVL_VERB, "ENTER pdProcessResolveEvents(pd:%p, processType:%"PRIu32", count%"PRIu32", events**:%p [%p], props:0x%"PRIx32")\n",
            pd, processType, count, events, events?*events:NULL, properties);
#define _END_FUNC processResolveEventsEnd

    u8 toReturn = 0;

    ocrWorker_t *worker = NULL;

    getCurrentEnv(&pd, &worker, NULL, NULL);
    ocrTask_t *savedTask = worker->curTask;

    // Structure to figure out what we can process
    u32 npIdxCounter = 0;
    u8 npIdx[NP_COUNT];

    bool doClearHold = properties & PDSTT_CLEARHOLD;
    // HACK: For now limit ourselves to 64 so we can track what we resolved easily
    ASSERT(count < 64);
    u64 isNotReady = 0ULL;
    u32 i;

    // Populate things initially; basically check if things are ready and if they are,
    // don't worry about them. This also resolves the event to an event pointer through
    // which we will be able to get a strand
    DPRINTF(DEBUG_LVL_VERB, "Initially resolving events; count is %"PRIu32"\n", count);
    for(i=0; i<count; ++i) {
        if(pdResolveEvent(pd, (u64*)(&(events[i])), NULL, doClearHold) != OCR_EBUSY) {
            // The event is ready
            DPRINTF(DEBUG_LVL_VVERB, "Event %"PRIu32" (@ %p) is initially ready\n",
                    i, events[i]);
        } else {
            DPRINTF(DEBUG_LVL_VVERB, "Event %"PRIu32" (@ %p) is initially not ready -- strand is %p\n",
                    i, events[i], events[i]->strand);
            isNotReady |= (1ULL<<i);
        }
    }
    DPRINTF(DEBUG_LVL_VERB, "Not ready vector is 0x%"PRIx64"\n", isNotReady);

    // The general heuristic here is:
    //   - try to process the ones we want first
    //   - continue trying the ones we want until we no longer flip one (ie: isNotReady is the same)
    //   - go process anything else (pdProcessStrands)
    //   - loop around. For the last step, process more and more at one time (exponential "back-off") if
    //     we still can't get isNotReady to flip
    u32 curEvent = 63;
    u32 strandsCount = 1; // Number of strands to process in the second step
    while(isNotReady) {
        u64 oldIsNotReady = isNotReady;
        // We try to resolve the events we care about for now
        // Look for MSB in the part we haven't looked at yet
        bool doBreak = false;
        while(true) {
            curEvent = fls64(isNotReady & ((1ULL<<curEvent) - 1));

            if(curEvent == 0) {
                // This can be if the vector isNotReady ends with a 1 (in position 0) or if
                // it is fully 0
                if((isNotReady & 1) == 0) break; // Nothing to look at

                if(doBreak) break; // This means we have been here before
                doBreak = true; // Next time we show up here, we break out
                                // since it will mean we are trying the zero position again
            }
            // At this stage, we have a legitimate event to try to resolve at position curEvent
            // First attempt to resolve the event (who knows, it may be ready now)
            if(pdResolveEvent(pd, (u64*)(&(events[curEvent])), NULL, doClearHold) != OCR_EBUSY) {
                // We resolved it without having to do anything, hurray, move along
                DPRINTF(DEBUG_LVL_VERB, "Event %"PRIu32" (@ %p) is now ready\n", curEvent, events[curEvent]);
                isNotReady &= ~(1ULL<<curEvent);
            } else { /* Event not ready */
                if(events[curEvent]->properties & PDEVT_READY) {
                    // We may have a shot
                    pdStrand_t *toProcess = events[curEvent]->strand;
                    DPRINTF(DEBUG_LVL_VERB, "Event %"PRIu32" (@ %p) is ready but has actions -- processing strand %p\n",
                            curEvent, events[curEvent], toProcess);
                    u32 stIdx = toProcess->index & ((1<<BV_SIZE_LOG2) - 1);
                    ASSERT(toProcess); // If we have a ready event but not fully resolved, there
                                       // must be a strand
                    // Lock the strand and try to process things. To do so in a way that won't break
                    // the usual "go-down-the-tree" approach, we lock our parent, check that we need
                    // processing, lock the strand and propagate any relevant information back up the tree
                    pdStrandTableNode_t *curNode = toProcess->parent;
                    hal_lock(&(curNode->lock));
                    // We should be looking at toProcess
                    ASSERT(curNode->data.slots[stIdx] == toProcess);
                    if(curNode->nodeNeedsProcess[processType] & (1ULL<<stIdx)) {
                        DPRINTF(DEBUG_LVL_VVERB, "Strand %p [idx %"PRIu32"] needs processing we can do\n",
                                toProcess, stIdx);
                        pdAction_t *curAction = NULL;
                        bool propagateNP = false;
                        ASSERT(arrayDequeSize(toProcess->actions) > 0);
                        RESULT_ASSERT(arrayDequePeekFromHead(toProcess->actions, (void**)&curAction), ==, 0);
                                            _pdActionToNP(npIdx, curAction);
                        _pdActionToNP(npIdx, curAction);
#ifdef OCR_ASSERT
                        npIdxCounter = 0;
                        while(npIdx[npIdxCounter]) {
                            if(npIdx[npIdxCounter] -1 == processType) {
                                npIdxCounter = (u32)-1;
                                break;
                            }
                            ++npIdxCounter;
                        }
                        // Somehow we are trying to process something that we shouldn't be. Runtime error most likely
                        if(npIdxCounter != (u32)-1)
                            ASSERT(0);
#endif
                        npIdxCounter = 0;
                        while(npIdx[npIdxCounter]) {
                            u8 tIdx = npIdx[npIdxCounter] - 1;
                            curNode->nodeNeedsProcess[tIdx] &= ~(1ULL<<stIdx);
                            if(curNode->nodeNeedsProcess[tIdx] == 0ULL) {
                                propagateNP = true;
                            } else {
                                npIdx[npIdxCounter] = 0;
                            }
#ifdef MT_OPTI_CONTENTIONLIMIT
                            {
                                u32 t __attribute__((unused)) = hal_xadd32(&(toProcess->containingTable->consumerCount[tIdx]), -1);
                                DPRINTF(DEBUG_LVL_VVERB, "Decrementing consumerCount[%"PRIu32"] @ table %p (processResolveEvents); was %"PRId32"\n",
                                    tIdx, toProcess->containingTable, t);
                            }
#endif
                            ++npIdxCounter;
                        }

                        // Go back up the tree; we switch nodeNeedsProcess for our parent if
                        // we have no more nodes to process due to the strand we are currently
                        // processing
                        pdStrandTableNode_t *parentNode = curNode->parent;
                        u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                        if(npIdx[1]) {
                            DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                        }
                        PROPAGATE_UP_TREE(curNode, parentNode, tNpIdx, propagateNP, {
                            propagateNP = false;
                            u32 i;
                            for(i=0; i<NP_COUNT; ++i) {
                                if(npIdx[i]) { // npIdx[i] == 0 means nothing to propagate at that position
                                    // The following assert should always be true because
                                    // even if something else is modifying nodeNeedsProcess at the same time,
                                    // we don't release curNode (old parent) between the time
                                    // we check the condition to set propagateNP and the time
                                    // we check with the assert
                                    ASSERT(curNode->nodeNeedsProcess[npIdx[i]-1] == 0ULL);
                                    parentNode->nodeNeedsProcess[npIdx[i]-1] &= ~(1ULL<<curNode->parentSlot);
                                    if(parentNode->nodeNeedsProcess[npIdx[i]-1] == 0ULL) {
                                        propagateNP = true;
                                    } else {
                                        npIdx[i] = 0;
                                    }
                                }
                            }
                        });

                        // Reset curNode properly
                        curNode = toProcess->parent;
                        // At this point, we have a strand that we can process
                        RESULT_ASSERT(_pdLockStrand(toProcess, BLOCK), ==, 0);

                        toProcess->processingWorker = worker;
                        worker->curTask = toProcess->contextTask;

                        // It should be in this state if we are ready to process it.
                        ASSERT((toProcess->properties & PDST_WAIT) == PDST_WAIT_ACT);
                        // We loop while the event is ready and there is stuff to do
                        // Note that the actions may make the event not ready thus the importance
                        // of checking every time
                        while (((toProcess->properties & PDST_WAIT_EVT) == 0) &&
                               arrayDequeSize(toProcess->actions)) {
                            bool canProcess = false;
                            pdAction_t *curAction = NULL;
                            RESULT_ASSERT(arrayDequePeekFromHead(toProcess->actions, (void**)&curAction),
                                          ==, 0);
                            ASSERT(curAction);
                            npIdxCounter = 0;
                            _pdActionToNP(npIdx, curAction);
                            while(npIdx[npIdxCounter]) {
                                if(npIdx[npIdxCounter] - 1 == processType) {
                                    canProcess = true;
                                    break;
                                }
                                ++npIdxCounter;
                            }
                            if(canProcess) {
                                pdAction_t *tAction __attribute__((unused)) = NULL;
                                RESULT_ASSERT(arrayDequePopFromHead(toProcess->actions, (void**)&tAction),
                                              ==, 0);
                                ASSERT(tAction == curAction);
                                DPRINTF(DEBUG_LVL_VERB, "Processing action %p\n", curAction);
                                RESULT_ASSERT(_pdProcessAction(pd, worker, toProcess, curAction, 0), ==, 0);
                                DPRINTF(DEBUG_LVL_VERB, "Done processing action %p\n", curAction);
                            } else {
                                DPRINTF(DEBUG_LVL_VERB, "Action %p is of the wrong type (need %"PRIu32") -- leaving as is\n",
                                        curAction, processType);
                                break;
                            }
                        }
                        // Done processing the strand; reset values
                        worker->curTask = savedTask;
                        toProcess->processingWorker = NULL;
                        toProcess->properties &= ~PDST_MODIFIED;

                        hal_lock(&(curNode->lock));
                        bool propagateReady = false, didFree = false;
                        propagateNP = false;
                        if(arrayDequeSize(toProcess->actions) == 0) {
                            toProcess->properties &= ~(PDST_WAIT_ACT);
                            if((toProcess->properties & PDST_WAIT_EVT) == 0) {
                                DPRINTF(DEBUG_LVL_VVERB, "Strand %p now ready\n", toProcess);
                                // Some sanity checks: we should not need processing (since we were just
                                // processing) and we should not be ready (since we needed processing)
                                ASSERT((curNode->nodeReady & (1ULL<<stIdx)) == 0);
                                ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<stIdx)) == 0);

                                // Clear isNotReady since we actually have the result now
                                isNotReady &= ~(1ULL<<curEvent);

                                // Set the return value to the proper event
                                events[curEvent] = toProcess->curEvent;

                                // We check if there is a hold on the strand -- if so, we leave in the
                                // strand table and propagate that. Otherwise, we remove it
                                if ((toProcess->properties & PDST_HOLD) == 0) {
                                    // We can free the strand now
                                    DPRINTF(DEBUG_LVL_VERB, "Freeing strand %p [idx %"PRIu64"] after processing actions\n",
                                            toProcess, toProcess->index);
                                    // Unlock the curNode because it will be locked again in _pdDestroyStrand
                                    hal_unlock(&(curNode->lock));
                                    RESULT_ASSERT(_pdDestroyStrand(pd, toProcess), ==, 0);
                                    didFree = true;
                                } else {
                                    // Here, we propagate the ready flag only if we are the first ready
                                    // ndde
                                    propagateReady = curNode->nodeReady == 0ULL;
                                    curNode->nodeReady |= (1ULL<<stIdx);
                                    DPRINTF(DEBUG_LVL_VERB, "Strand %p is ready but has a hold -- leaving as is\n",
                                            toProcess);
                                }
                            } else {
                                DPRINTF(DEBUG_LVL_VVERB, "Strand %p is not ready and has no actions\n",
                                        toProcess);
                                ASSERT((curNode->nodeReady & (1ULL<<stIdx)) == 0);
                                ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<stIdx)) == 0);
                                ASSERT((curNode->nodeReady & (1ULL<<stIdx)) == 0);
                            }
                        } else {
                            ASSERT(toProcess->properties & PDST_WAIT_ACT);
                            if((toProcess->properties & PDST_WAIT_EVT) == 0) {
                                DPRINTF(DEBUG_LVL_VERB, "Strand %p still has pending actions that need processing\n",
                                        toProcess);
                                // We need to check by whom these actions need to be processed; in other words, there
                                // may be actions left because we are not processing actions of this type. We need
                                // to set the proper nodeNeedsProcess flag
                                // We already updated npIdx with the proper types for this action (that's when
                                // we figured out we couldn't process it)
                                propagateNP = false;
                                npIdxCounter = 0;
                                while(npIdx[npIdxCounter]) {
                                    u8 tIdx = npIdx[npIdxCounter] - 1;
                                    DPRINTF(DEBUG_LVL_VVERB, "Next action can be processed by type %"PRIu32"\n", tIdx);
                                    propagateNP |= curNode->nodeNeedsProcess[tIdx] == 0ULL;
                                    curNode->nodeNeedsProcess[tIdx] |= (1ULL<<stIdx);
#ifdef MT_OPTI_CONTENTIONLIMIT
                                    {
                                        u32 t __attribute__((unused)) = hal_xadd32(&(toProcess->containingTable->consumerCount[tIdx]), 1);
                                        DPRINTF(DEBUG_LVL_VVERB, "Incrementing consumerCount[%"PRIu32"] @ table %p (processResolveEvents 2); was %"PRId32"\n",
                                                tIdx, toProcess->containingTable, t);
                                    }
#endif
                                    ++npIdxCounter;
                                }
                                ASSERT((curNode->nodeReady & (1ULL<<stIdx)) == 0);
                            } else {
                                DPRINTF(DEBUG_LVL_VERB, "Strand %p has pending actions but not ready\n",
                                        toProcess);

                                ASSERT((curNode->nodeNeedsProcess[processType] & (1ULL<<stIdx)) == 0);
                                ASSERT((curNode->nodeReady & (1ULL<<stIdx)) == 0);
                            }
                        }

                        // Holding curNode->lock (except if didFree) and strand lock
                        // Unlock the strand; we can use the cheaper unlock because we already checked
                        // for buffered actions
                        _pdUnlockStrand(toProcess);

                        // Holding lock on curNode->lock EXCEPT if freed strand
                        // (in that case, the following if statement is false)
                        if (propagateReady || propagateNP) {
                            ASSERT(!didFree);
                            DPRINTF(DEBUG_LVL_VERB, "Propagating properties: ready: %"PRIu32"; np: %"PRIu32"\n",
                                    propagateReady, propagateNP);

                            pdStrandTableNode_t *parent = curNode->parent;
                            ASSERT(hal_islocked(&(curNode->lock)));
                            u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0;
                            if(npIdx[1]) {
                                DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
                            }
                            // We flip nodeReady from 0 to 1; to up until we see a 1
                            // We flip nodeNeedsProcessing from 0 to 1; same as above
                            PROPAGATE_UP_TREE(
                                curNode, parent, tNpIdx,
                                propagateReady || propagateNP, {
                                    if (propagateReady) {
                                        propagateReady = parent->nodeReady == 0ULL;
                                        parent->nodeReady |= (1ULL<<curNode->parentSlot);
                                    }
                                    if (propagateNP) {
                                        propagateNP = false;
                                        u32 i;
                                        for(i=0; i<NP_COUNT; ++i) {
                                            if(npIdx[i]) {
                                                if(parent->nodeNeedsProcess[npIdx[i]-1]) {
                                                    parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                                    propagateNP = true;
                                                } else {
                                                    parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                                                    npIdx[i] = 0;
                                                }
                                            }
                                        }
                                    }
                                });
                        } else {
                            if(!didFree)
                                hal_unlock(&(curNode->lock));
                        }
                    } else { /* Node does not need processing or cannot be processed by us */
                        hal_unlock(&(curNode->lock));
                        // We attempt to resolve the event again
                        if(pdResolveEvent(pd, (u64*)(&(events[curEvent])), NULL, doClearHold) != OCR_EBUSY) {
                            isNotReady &= ~(1ULL<<curEvent);
                        }
                    }
                } else { /* Event is not PDEVT_READY */
                    DPRINTF(DEBUG_LVL_VVERB, "Event %"PRIu32" (@ %p) is still not ready\n",
                            curEvent, events[curEvent]);
                }
            }
        } /* End of while true over the events that are not ready */
        if(oldIsNotReady != isNotReady) {
            strandsCount = 1;
            DPRINTF(DEBUG_LVL_VVERB, "isNotReady: from 0x%"PRIx64" to 0x%"PRIx64" -- strandsCount: 1\n",
                    oldIsNotReady, isNotReady);
        } else {
            DPRINTF(DEBUG_LVL_VVERB, "No change in isNotReady bitvector (0x%"PRIx64") -- strandsCount: %"PRIu32"\n",
                    isNotReady, strandsCount);
        }
        if(isNotReady) {
            // If we still have non-ready events, we go and do something else for a bit
            _pdProcessNStrands(pd, processType, strandsCount, 0);
            strandsCount *= 2;
        }
    } /* End of while(isNotReady) */

    ASSERT(isNotReady == 0); // We should have all events ready

END_LABEL(processResolveEventsEnd)
    DPRINTF(DEBUG_LVL_VERB, "EXIT pdProcessResolveEvents -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

/***************************************/
/********** Internal functions *********/
/***************************************/

static void _pdActionToNP(u8 *npIdx, pdAction_t* action) {
    u32 i = 0;
    switch((u64)(action) & 0X7) {
    case 0b000:
        /* An actual action -- read the properties field */
        npIdx[0] = (u8)(((action->properties & PDACT_NPTYPE_MASK) >> PDACT_NPTYPE_SHIFT)+1);
        for(i=1; i<NP_COUNT; ++i) npIdx[i] = 0;
        return;
    case PDACTION_ENC_PROCESS_MESSAGE:
        /* PROCESS_MESSAGE */
        npIdx[0] = (u8)(((u64)action >> 3)+1);
        for(i=1; i<NP_COUNT; ++i) npIdx[i] = 0;
        return;
    case PDACTION_ENC_PROCESS_EVENT: case PDACTION_ENC_PROCESS_EVENT_MOD:
        /* PROCESS_EVENT -- always a computation thing */
        npIdx[0] = (u8)(NP_WORK + 1);
        for(i=1; i<NP_COUNT; ++i) npIdx[i] = 0;
        return;
    case PDACTION_ENC_EXTEND: {
        /* Add extended values here */
        u8 type = 0;
        PDACTION_DECEXT_TYPE(type, (u64)action);
        if(type & PDACTION_ENCEXT_CTRL_BASE) {
            // Anyone can process any of the control events
            for(i=0; i<NP_COUNT; ++i)
                npIdx[i] = i+1;
        } else {
            ASSERT(0); // Not handled yet
            for(i=0; i<NP_COUNT; ++i) npIdx[i] = 0;
        }
        break;
    }
    default:
        DPRINTF(DEBUG_LVL_WARN, "Unknown action type in pdActionToNP: 0x%"PRIx64"\n",
                (u64)(action) & 0x7);
        ASSERT(0);
        for(i=0; i<NP_COUNT; ++i) npIdx[i] = 0;
    }
}

static u8 _pdProcessAction(ocrPolicyDomain_t *pd, ocrWorker_t *worker, pdStrand_t *strand,
                           pdAction_t* action, u32 properties) {

    DPRINTF(DEBUG_LVL_INFO, "ENTER _pdProcessAction(pd:%p, worker:%p, strand:%p, action:%p, props:0x%"PRIx32")\n",
            pd, worker, strand, action, properties);
#define _END_FUNC processActionEnd

    ASSERT(pd && worker);
    // If we are processing, the event should either be ready or NULL as certain
    // actions do not require an event
    ASSERT(strand);
    ASSERT((strand->curEvent == NULL) || (strand->curEvent->properties & PDEVT_READY));

    u8 toReturn = 0;

    u64 actionPtr = (u64)action;

    // Figure out the encoding for the action
    switch (actionPtr & 0x7) {
        case PDACTION_ENC_PROCESS_MESSAGE:
        {
            u8 (*callback)(ocrPolicyDomain_t*, pdEvent_t**, u32) = pd->fcts.processEvent;
            DPRINTF(DEBUG_LVL_VERB, "Action is a callback to processEvent (%p)\n", callback);
            pdEvent_t *curEvent = strand->curEvent;
            toReturn = callback(pd, &curEvent, 0);
            CHECK_RESULT(toReturn,
                {
                    DPRINTF(DEBUG_LVL_WARN, "Callback to processEvent returned error code %"PRIu32"\n", toReturn);
                },);
            DPRINTF(DEBUG_LVL_VVERB, "Callback returned 0\n");
            // Check what was returned and update strand->curEvent if needed
            // TODO: For now, we don't allow an event from another strand to be
            // returned. I have to think if we need to do this
            if(((u64)curEvent) & 0x7) {
                // This is a fake event, it better point to us
                // We only check the index and not the table ID because I don't have it
                // For now this is just for asserting so we ignore but if we need
                // to take this feature further, we'll need to rethink this
                ASSERT(EVT_DECODE_ST_IDX((u64)curEvent) == strand->index);
                curEvent = strand->curEvent; // No change
            }

            if(strand->curEvent != curEvent) {
                if(strand->curEvent) {
                    DPRINTF(DEBUG_LVL_INFO, "Event changed from %p to %p -- freeing old event\n",
                        strand->curEvent, curEvent);
                    // Make sure we don't grab lock on this strand
                    strand->curEvent->strand = NULL;
                    RESULT_ASSERT(pdDestroyEvent(pd, strand->curEvent), ==, 0);
                    strand->curEvent = curEvent;
                } else {
                    DPRINTF(DEBUG_LVL_INFO, "Event chnaged from NULL to %p\n", curEvent);
                    strand->curEvent = curEvent;
                }
                // We reference the new event as well
                RESULT_ASSERT(pdReferenceEvent(pd, strand->curEvent), ==, 0);
            } else {
                // Event did not change, nothing to do
            }
            break;
        }

        case PDACTION_ENC_PROCESS_EVENT:
        {
            // Get the object we have to call processEvent on
            ocrObject_t *object = (ocrObject_t*)((u64)actionPtr & ~0x7);
            u8 (*callback)(ocrObject_t*, pdEvent_t**, u32) = pd->factories[object->fctId]->fcts.processEvent;
            DPRINTF(DEBUG_LVL_VERB, "Action is a callback on object %p (%p)", object, callback);
            pdEvent_t *curEvent = strand->curEvent;
            toReturn = callback(object, &curEvent, 0);
            CHECK_RESULT(toReturn,
                {
                    DPRINTF(DEBUG_LVL_WARN, "Callback to processEvent returned error code %"PRIu32"\n", toReturn);
                },);
            DPRINTF(DEBUG_LVL_VVERB, "Callback returned 0\n");
            // Check what was returned and update strand->curEvent if needed
            // TODO: For now, we don't allow an event from another strand to be
            // returned. I have to think if we need to do this
            if(((u64)curEvent) & 0x7) {
                // This is a fake event, it better point to us
                // We only check the index and not the table ID because I don't have it
                // For now this is just for asserting so we ignore but if we need
                // to take this feature further, we'll need to rethink this
                ASSERT(EVT_DECODE_ST_IDX((u64)curEvent) == strand->index);
                curEvent = strand->curEvent; // No change
            }

            if(strand->curEvent != curEvent) {
                if(strand->curEvent) {
                    DPRINTF(DEBUG_LVL_INFO, "Event changed from %p to %p -- freeing old event\n",
                        strand->curEvent, curEvent);
                    // Make sure we don't grab lock on this strand
                    strand->curEvent->strand = NULL;
                    RESULT_ASSERT(pdDestroyEvent(pd, strand->curEvent), ==, 0);
                    strand->curEvent = curEvent;
                } else {
                    DPRINTF(DEBUG_LVL_INFO, "Event chnaged from NULL to %p\n", curEvent);
                    strand->curEvent = curEvent;
                }
                // We reference the new event as well
                RESULT_ASSERT(pdReferenceEvent(pd, strand->curEvent), ==, 0);
            } else {
                // Event did not change, nothing to do
            }
            break;
        }
/*
        case PDACTION_ENC_PROCESS_EVENT_MOD:
        {
            // Get the object we have to call processEvent on
            ocrModule_t *module = (ocrModule_t*)((u64)actionPtr & ~0x7);
            u8 (*callback)(ocrModule_t*, pdEvent_t**, u32) = pd->factories[object->fctId]->fcts.processEvent;
            DPRINTF(DEBUG_LVL_VERB, "Action is a callback on object %p (%p)", object, callback);
            pdEvent_t *curEvent = strand->curEvent;
            toReturn = callback(object, &curEvent, 0);
            CHECK_RESULT(toReturn,
                {
                    DPRINTF(DEBUG_LVL_WARN, "Callback to processEvent returned error code %"PRIu32"\n", toReturn);
                },);
            DPRINTF(DEBUG_LVL_VVERB, "Callback returned 0\n");
            // Check what was returned and update strand->curEvent if needed
            // TODO: For now, we don't allow an event from another strand to be
            // returned. I have to think if we need to do this
            if(((u64)curEvent) & 0x7) {
                // This is a fake event, it better point to us
                // We only check the index and not the table ID because I don't have it
                // For now this is just for asserting so we ignore but if we need
                // to take this feature further, we'll need to rethink this
                ASSERT(EVT_DECODE_ST_IDX((u64)curEvent) == strand->index);
                curEvent = strand->curEvent; // No change
            }

            if(strand->curEvent != curEvent) {
                if(strand->curEvent) {
                    DPRINTF(DEBUG_LVL_INFO, "Event changed from %p to %p -- freeing old event\n",
                        strand->curEvent, curEvent);
                    // Make sure we don't grab lock on this strand
                    strand->curEvent->strand = NULL;
                    RESULT_ASSERT(pdDestroyEvent(pd, strand->curEvent), ==, 0);
                    strand->curEvent = curEvent;
                } else {
                    DPRINTF(DEBUG_LVL_INFO, "Event chnaged from NULL to %p\n", curEvent);
                    strand->curEvent = curEvent;
                }
                // We reference the new event as well
                RESULT_ASSERT(pdReferenceEvent(pd, strand->curEvent), ==, 0);
            } else {
                // Event did not change, nothing to do
            }
            break;
        }
*/
        case PDACTION_ENC_EXTEND:
        {
            u32 subType = 0;
            PDACTION_DECEXT_TYPE(subType, actionPtr);
            DPRINTF(DEBUG_LVL_VVERB, "Decoding extended action 0x%"PRIu32"\n",
                    subType);
            switch(subType) {
            case PDACTION_ENCEXT_CTRL_MARK_READY:
            {
                // We need to find the strand and the event to make
                // it ready

                u32 stTableIdx;
                u64 stIdx;
                u32 tt __attribute__((unused)); // This is just a trash variable
                PDACTION_DECEXT_2ARG(stIdx, stTableIdx, tt, actionPtr);
                DPRINTF(DEBUG_LVL_VVERB, "Action is make event (table %"PRIu32", idx: %"PRIu64") ready\n",
                        stTableIdx, stIdx);
                pdStrand_t *newStrand = NULL;
                RESULT_ASSERT(pdGetStrandForIndex(pd, &newStrand, pd->strandTables[stTableIdx-1], stIdx), ==, 0);
                DPRINTF(DEBUG_LVL_VVERB, "Found strand %p and event %p\n", newStrand, newStrand->curEvent);
                ASSERT(newStrand && newStrand->curEvent);
                RESULT_ASSERT(pdMarkReadyEvent(pd, newStrand->curEvent), ==, 0);
                break;
            }
            case PDACTION_ENCEXT_CTRL_MARK_READY_DIFF: {
                u64 encodedValue;
                u64 tt __attribute__((unused)); // This is just a trash variable
                PDACTION_DECEXT_1ARG(encodedValue, tt, actionPtr);
                pdStrand_t *newStrand = (pdStrand_t*)(PDACTION_FROMDIFF((u64)strand, encodedValue, PDACTION_1ARG_SIZE));
                DPRINTF(DEBUG_LVL_VVERB, "Decoded strand %p from offset 0x%"PRIx64" off current strand %p\n",
                        newStrand, encodedValue, strand);
                pdEvent_t *evt = newStrand->curEvent;
                DPRINTF(DEBUG_LVL_VERB, "Action is make strand %p ready (evt %p)\n",
                        newStrand, evt);
                ASSERT(evt);
                RESULT_ASSERT(pdMarkReadyEvent(pd, evt), ==, 0);
                break;
            }
            case PDACTION_ENCEXT_CTRL_SATISFY_MERGE: {
                // We need to satisfy a merge event and, if needed, mark it as ready
                u64 encodedValue, position;
                u64 tt __attribute__((unused)); // Trash variable
                PDACTION_DECEXT_2ARG(encodedValue, position, tt, actionPtr);
                pdEventMerge_t *newEvent = (pdEventMerge_t*)(PDACTION_FROMDIFF((u64)strand, encodedValue, PDACTION_2ARG_SIZE2));
                DPRINTF(DEBUG_LVL_VVERB, "Decoded event %p from offset 0x%"PRIx64" off current strand %p and position %"PRIu64"\n",
                    newEvent, encodedValue, strand, position);
                DPRINTF(DEBUG_LVL_VERB, "Action is satisfy merge %p with %p at position %"PRIu64" (totalEvents: %"PRIu32"; satisfyCount: %"PRIu32")\n",
                    newEvent, strand->curEvent, position, newEvent->count, newEvent->countReady);

                // First set the new event
                void** entryValue;
                arrayDequeAtIndex(&(newEvent->events), position, &entryValue);
                *entryValue = (void*)strand->curEvent;
                RESULT_ASSERT(pdReferenceEvent(pd, strand->curEvent), ==, 0); // Increase ref count
                ASSERT(newEvent->count > newEvent->countReady); // This should always be true
                u32 newCountReady = hal_xadd32(&(newEvent->countReady), 1);
                // Both count and countReady are monotonically increasing so this next check
                // is safe and will only be true once (given the usage pattern we are making of it)
                if(newCountReady + 1 == newEvent->count) {
                    DPRINTF(DEBUG_LVL_VERB, "Last event is satisfied -- triggering merged event %p\n", newEvent);
                    CHECK_RESULT(pdMarkReadyEvent(pd, (pdEvent_t*)newEvent), , toReturn = OCR_EINVAL);
                }
            }
            default:
                ASSERT(0);
                break;
            }
            break;
        }
        default:
            /* For now, empty function, we just print something */
            DPRINTF(DEBUG_LVL_INFO, "Pretending to execute action %p\n", action);
            break;
    }


END_LABEL(processActionEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT _pdProcessAction -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

static u8 _pdDestroyStrandTableNode(ocrPolicyDomain_t *pd, pdStrandTableNode_t *node) {

    DPRINTF(DEBUG_LVL_INFO, "ENTER _pdDestroyStrandTableNode(pd:%p, node:%p)\n",
            pd, node);
#define _END_FUNC destroyStrandTableNodeEnd

    u8 toReturn = 0;
    u32 i;

    CHECK_RESULT_T(node != NULL, , toReturn |= OCR_EINVAL);

    // This should not contain anything
    ASSERT(node->nodeFree == ~0ULL);
    bool isLeaf = IS_LEAF_NODE(node->lmIndex);

    for (i=0; i<BV_SIZE; ++i) {
        if(node->data.slots[i] != NULL) {
            if(isLeaf) {
                DPRINTF(DEBUG_LVL_VERB, "Freeing strand %"PRIu32": %p\n",
                        i, node->data.slots[i]);
                pd->fcts.pdFree(pd, node->data.slots[i]);
                node->data.slots[i] = NULL;
            } else {
                DPRINTF(DEBUG_LVL_VERB, "Freeing down sub-node %"PRIu32": %p\n",
                        i, node->data.nodes[i]);
                CHECK_RESULT(toReturn |= _pdDestroyStrandTableNode(pd, node->data.nodes[i]), ,);
                DPRINTF(DEBUG_LVL_VERB, "Freeing sub-node %"PRIu32": %p\n",
                        i, node->data.nodes[i]);
                pd->fcts.pdFree(pd, node->data.nodes[i]);
                node->data.nodes[i] = NULL;
            }
        }
    }

END_LABEL(destroyStrandTableNodeEnd)
        DPRINTF(DEBUG_LVL_INFO, "EXIT _pdDestroyStrandTableNode -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

static u8 _pdLockStrand(pdStrand_t *strand, u32 properties) {

    // If the strand has a processing worker and we are that worker
    // there is no need to lock because the lock was already grabbed when
    // started processing the strand
    if(strand->processingWorker) {
        ocrWorker_t *worker = NULL;
        getCurrentEnv(NULL, &worker, NULL, NULL);
        if(worker == strand->processingWorker)
            return 0;
    }

    if((properties & BLOCK) == 0) {
        if(hal_trylock(&(strand->lock))) {
            return OCR_EBUSY;
        }
    } else {
        hal_lock(&(strand->lock));
    }
    return 0;
}

static u8 _pdDoRequeue(ocrPolicyDomain_t *pd, pdStrand_t *strand) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER _pdDoRequeue(pd:%p, strand:%p)\n",
            pd, strand);
#define _END_FUNC doRequeueEnd

    u8 toReturn = 0;

    // If these fail, there is a runtime error in the MT
    ASSERT(pd)
    ASSERT(strand && hal_islocked(&(strand->lock)));

    // We use a while loop in case there are additions in parallel
    // TODO: There is still a race here because actions can be buffered and
    // not added to the main actions list. This may cause issues but this
    // should be rare enough that for now we can ignore (fingers crossed)
    while(strand->bufferedActions != NULL) {
        arrayDeque_t *bufferedActions = NULL;
        bool bufferedClear = false;
        // OK, there seems to be something for us to do
        DPRINTF(DEBUG_LVL_VERB, "Found buffered actions %p on unlock for strand %p -- adding to regular actions\n",
            strand->bufferedActions, strand);

        hal_lock(&(strand->bufferedLock));
        // We are going to swap bufferedActions with NULL
        bufferedActions = strand->bufferedActions;
        strand->bufferedActions = NULL;
        bufferedClear = strand->bufferedHoldClear;
        strand->bufferedHoldClear = false;
        hal_unlock(&(strand->bufferedLock));

        // At this point, we have taken all the actions that were buffered and all we have to do is add them
        // to the actions queue.
        // This is not done very efficiently (ideally we should do it in one fell swoop) but it works
        ASSERT(bufferedActions);
        u32 count = arrayDequeSize(bufferedActions);
        ASSERT(count >= 1);
        pdAction_t *actionPtr = NULL;
        while(--count > 0) {
            // Only go to the penultimate one so we can set the proper clearFwdHold flag
            RESULT_ASSERT(arrayDequePopFromHead(bufferedActions, (void**)&actionPtr), ==, 0);
            CHECK_RESULT(pdEnqueueActions(pd, strand, 1, &actionPtr, false),
                arrayDequeDestruct(bufferedActions);
                pd->fcts.pdFree(pd, bufferedActions), );
        }
        // Now do the last one
        RESULT_ASSERT(arrayDequePopFromHead(bufferedActions, (void**)&actionPtr), ==, 0);
        CHECK_RESULT(pdEnqueueActions(pd, strand, 1, &actionPtr, bufferedClear),
            arrayDequeDestruct(bufferedActions);
            pd->fcts.pdFree(pd, bufferedActions), );

        // Free buffferedActions
        arrayDequeDestruct(bufferedActions);
        pd->fcts.pdFree(pd, bufferedActions);
    }
END_LABEL(doRequeueEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT _pdDoRequeue -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

static u8 _pdInitializeStrandTableNode(ocrPolicyDomain_t *pd, pdStrandTable_t *table, pdStrandTableNode_t *node,
                                       pdStrandTableNode_t *parent, u32 parentSlot,
                                       u32 rdepth, u32 numChildrenToInit,
                                       u8 flags) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER _pdInitializeStrandTableNode(pd:%p, node:%p, parent:%p, pSlot:%"PRIu32", rdepth:%"PRIu32", childCount:%"PRIu32", flags:0x%"PRIx32")\n",
            pd, node, parent, parentSlot, rdepth, numChildrenToInit, flags);
#define _END_FUNC initializeStrandTableNodeEnd

    // Does not check for lock on node because could be in exclusive access
    // Parent should have lock
    u8 toReturn = 0;
    u32 i = 0;

    ASSERT(pd);

    if (parent) {
        CHECK_RESULT_T(hal_islocked(&(parent->lock)), , toReturn = OCR_EINVAL);
    }

    // Some sanity checks
    ASSERT(node);
    ASSERT((parent == NULL) || parentSlot < BV_SIZE);
    ASSERT(numChildrenToInit <= BV_SIZE);
    if (!(flags & IS_LEAF)) {
        // If not a leaf node, numChildrenToInit should be 0
        CHECK_RESULT_T(numChildrenToInit == 0, , toReturn = OCR_EINVAL);
    } else {
        CHECK_RESULT(rdepth, , toReturn = OCR_EINVAL);
    }

    node->nodeFree = (u64)-1;   // All nodes are free to start with
    for(i=0; i<NP_COUNT; ++i) {
        node->nodeNeedsProcess[i] = 0; // Nothing needs to be processed
    }
    i = 0;
    node->nodeReady = 0;        // Nothing is ready
    // This is parent->lmIndex + parentSlot*BV_SIZE^(rdepth+1)
    node->lmIndex = parent?(parent->lmIndex + (parentSlot << (BV_SIZE_LOG2*(rdepth+1))))<<1:0;
    node->lock = INIT_LOCK;             // No lock for now.
    node->parent = parent;
    node->parentSlot = parent?parentSlot:(u32)-1; // If no parent, put -1

    if (flags & IS_LEAF) {
        // Indicate leaf status
        SET_LEAF_NODE(node->lmIndex);

        // Now take care of the data
        pdStrand_t * slab = NULL;
        if (numChildrenToInit) {
            // BUG #899: should be slab allocated
            // We allocate once so that if it fails, the cleanup is easy :)
            CHECK_MALLOC(slab = (pdStrand_t*)pd->fcts.pdMalloc(pd, sizeof(pdStrand_t)*numChildrenToInit),);
            DPRINTF(DEBUG_LVL_VERB, "Allocated %"PRIu32" strands for node %p\n",
                    numChildrenToInit, node);
        }
        // If we reach here, we allocated OK
        for (i = 0; i < numChildrenToInit; ++i, ++slab) {
            slab->curEvent = NULL;
            slab->actions = NULL;
            slab->bufferedActions = NULL;
            slab->parent = node;
            slab->containingTable = table;
            slab->properties = PDST_FREE;
            slab->lock = INIT_LOCK;
            slab->bufferedLock = INIT_LOCK;
            slab->index = LEAF_LEFTMOST_IDX(node->lmIndex) + (u64)i;
            slab->processingWorker = NULL;
            slab->contextTask =  NULL;
            node->data.slots[i] = slab;
            DPRINTF(DEBUG_LVL_VVERB, "Created strand %"PRIu64" @ %p\n",
                    slab->index, slab);
        }
    }

    // We NULL-ify everything else. Note that this works in all cases because
    // numChildrenToInit will be 0 if not a leaf
    // Continues the previous loop if isLeaf
    for ( ; i < BV_SIZE; ++i) {
        node->data.slots[i] = NULL; // Does not matter if we use slots or nodes
    }


END_LABEL(initializeStrandTableNodeEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT _pdInitializeStrandTableNode -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}


static u8 _pdSetStrandNodeAtIdx(ocrPolicyDomain_t *pd, pdStrandTableNode_t *parent,
                                u32 idx, void* child, u8 flags) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER _pdSetStrandNodeAtIdx(pd:%p, parent:%p, idx:%"PRIu32", child:%p, flags:0x%"PRIx32")\n",
            pd, parent, idx, child, flags);
#define _END_FUNC setStrandNodeAtIdxEnd

    // Does not check for lock on anything since we may have exclusive access
    u8 toReturn = 0;
    u32 i;

    ASSERT(pd);

    // Sanity check
    ASSERT(idx < BV_SIZE);

    // If this fails, it means there is already a child there
    CHECK_RESULT_T(parent->data.slots[idx] == NULL, , toReturn |= OCR_EACCES);

    // If this assert fails, this means a runtime error happened and state is
    // no longer consistent
    ASSERT(parent->nodeFree & (1ULL<<idx));

    // If this fails, the child is invalid
    if (flags & IS_STRAND) {
        CHECK_RESULT_T(((pdStrand_t*)child)->parent == parent, , toReturn |= OCR_EINVAL);
        CHECK_RESULT_T(((pdStrand_t*)child)->index >= LEAF_LEFTMOST_IDX(parent->lmIndex), , toReturn |= OCR_EINVAL);
        CHECK_RESULT_T(((pdStrand_t*)child)->index < LEAF_LEFTMOST_IDX(parent->lmIndex) + BV_SIZE, , toReturn |= OCR_EINVAL);
    } else {
        CHECK_RESULT_T(((pdStrandTableNode_t*)child)->parent == parent, , toReturn |= OCR_EINVAL);
        CHECK_RESULT_T(((pdStrandTableNode_t*)child)->parentSlot == idx, , toReturn |= OCR_EINVAL);
    }

    pdStrandTableNode_t *curNode = NULL;
    bool propagateFree = false, propagateReady = false, propagateNP = false;
    u32 npIdxCounter = 0;
    u8 npIdx[NP_COUNT] = {0}; // npIdx[i] will be set j+1 if we need to propagate NP j
    if (flags & IS_STRAND) {
        parent->data.slots[idx] = child;
        pdStrand_t *strand = (pdStrand_t*)child;
        // Update the flags
        if (!(strand->properties & PDST_FREE)) {
            parent->nodeFree &= ~(1ULL<<idx);
            propagateFree = parent->nodeFree == 0ULL;
        }

        if ((strand->properties & PDST_WAIT) == 0) {
            propagateReady = parent->nodeReady == 0ULL;
            parent->nodeReady |= (1ULL<<idx);
        }

        if ((strand->properties & PDST_WAIT) == PDST_WAIT_ACT) {
            // This means that we only have to wait for actions
            // We need to figure out what the first action is
            pdAction_t *tAct = NULL;
            RESULT_ASSERT(arrayDequePeekFromHead(strand->actions, (void**)&(tAct)), ==, 0);
            _pdActionToNP(npIdx, tAct);
            // We go over this value and set all the bits that *can* process
            // this action. Since this is only 8 bits, we just check by hand
            while(npIdx[npIdxCounter]) {
                u8 tIdx = npIdx[npIdxCounter] - 1;
                propagateNP |= curNode->nodeNeedsProcess[tIdx] == 0ULL;
                curNode->nodeNeedsProcess[tIdx] |= (1ULL<<idx);
#ifdef MT_OPTI_CONTENTIONLIMIT
                u32 t __attribute__((unused)) = hal_xadd32(&(strand->containingTable->consumerCount[tIdx]), 1);
                DPRINTF(DEBUG_LVL_VVERB, "Incrementing consumerCount[%"PRIu32"] @ table %p (setStrandNodeAtIdx); was %"PRId32"\n",
                    tIdx, strand->containingTable, t);
#endif
                ++npIdxCounter;
            }
        }
        curNode = parent;
    } else {
        parent->data.nodes[idx] = child;
        // Update the flags
        pdStrandTableNode_t *childNode = (pdStrandTableNode_t*)child;
        if (childNode->nodeFree == 0ULL) {
            parent->nodeFree &= ~(1ULL<<idx);
            propagateFree = parent->nodeFree == 0ULL;
        }

        if (childNode->nodeReady != 0ULL) {
            propagateReady = parent->nodeReady == 0ULL;
            parent->nodeReady |= 1ULL<<idx;
        }
        for(i=0; i<NP_COUNT; ++i) {
            if (childNode->nodeNeedsProcess[i] != 0ULL) {
                // WARNING: We do not increment consumerCount here because
                // we only insert full trees. If this changes, we may need to update this
                propagateNP |= parent->nodeNeedsProcess[i] == 0ULL;
                parent->nodeNeedsProcess[i] |= 1ULL<<idx;
                npIdx[npIdxCounter++] = i+1;
            }
        }
        curNode = parent;
    }

    parent = curNode->parent;

    // We need to propagate things (possibly)
    // Free bits: we flipped from 1 to 0, see if this makes others 0 (stop when sibblings have free slots)
    // Ready bits: we flipped from 0 to 1, see if this makes others 1 (stop when see 1)
    // NP bits: we flipped from 0 to 1, see if this makes others 1 (stop when see 1)

    // If we have the lock on curNode, we should *not* release it.
    u8 tNpIdx __attribute__((unused)) = npIdx[0]?(npIdx[0]-1):0; // If we don't have propagateNP, we don't want the print to fail
    if(npIdx[1]) {
        DPRINTF(DEBUG_LVL_VVERB, "WARNING: The below status of propagation will not print all NP vectors affected\n");
    }
    PROPAGATE_UP_TREE_NO_UNLOCK(
        curNode, parent, tNpIdx,
        propagateFree || propagateReady || propagateNP, {
            if (propagateFree) {
                parent->nodeFree &= ~(1ULL<<curNode->parentSlot);
                propagateFree = parent->nodeFree == 0ULL;
            }
            if (propagateReady) {
                propagateReady = parent->nodeReady == 0ULL;
                parent->nodeReady |= (1ULL<<curNode->parentSlot);
            }
            if (propagateNP) {
                propagateNP = false;
                u32 i;
                for(i=0; i<NP_COUNT; ++i) {
                    if(npIdx[i]) { // npIdx[i] == 0 means nothing to propagate at that position
                        if(parent->nodeNeedsProcess[npIdx[i]-1] == 0ULL) {
                            parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                            propagateNP = true;
                        } else {
                            parent->nodeNeedsProcess[npIdx[i]-1] |= (1ULL<<curNode->parentSlot);
                            npIdx[i] = 0;
                        }
                    }
                }
            }
        });

END_LABEL(setStrandNodeAtIdxEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT _pdSetStrandNodeAtIdx -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}


static u8 _pdDestroyStrand(ocrPolicyDomain_t* pd, pdStrand_t *strand) {
    DPRINTF(DEBUG_LVL_INFO, "ENTER _pdDestroyStrand(pd:%p, strand:%p)\n",
            pd, strand);
#define _END_FUNC destroyStrandEnd

    u8 toReturn = 0;

    ASSERT(pd);

    // The lock must be held on the strand
    CHECK_RESULT_T(hal_islocked(&(strand->lock)), , toReturn = OCR_EINVAL);

    // We should not be freeing strands that are still going to be used, so a bit
    // of sanity check here
    CHECK_RESULT(strand->properties & PDST_WAIT, , toReturn = OCR_EINVAL);
    if (strand->actions) {
        CHECK_RESULT_T(arrayDequeSize(strand->actions) == 0, , toReturn = OCR_EINVAL);
        arrayDequeDestruct(strand->actions);
        pd->fcts.pdFree(pd, strand->actions);
    }

    if (strand->bufferedActions) {
        CHECK_RESULT_T(arrayDequeSize(strand->bufferedActions) == 0, , toReturn = OCR_EINVAL);
        arrayDequeDestruct(strand->bufferedActions);
        pd->fcts.pdFree(pd, strand->bufferedActions);
    }

    // At this stage, we can free the strand
    if(strand->curEvent && ((strand->curEvent->properties & PDEVT_GC) != 0)) {
        DPRINTF(DEBUG_LVL_VERB, "Event %p garbage collected -- destroying\n", strand->curEvent);
        // Set the strand of the event to NULL so we don't go back into the strand to lock it
        strand->curEvent->strand = NULL;
        pdDestroyEvent(pd, strand->curEvent);
    }
    // Clean up data a bit
    strand->curEvent = NULL;

    // Go up and hold the parent lock so we can propagate the proper information on
    // free slots
    pdStrandTableNode_t *curNode = strand->parent;
    ASSERT(curNode);
    hal_lock(&(curNode->lock));

    hal_unlock(&(strand->lock));

    // Propgate things up. We free a node and it may remove the
    // "ready" flag from it. It can't be NP because none of the
    // wait flags are set

    bool propagateReady = false, propagateFree = false;
    u32 stIdx = strand->index & ((1<<BV_SIZE_LOG2) - 1);
#ifdef OCR_ASSERT
    {
        u32 i;
        for(i=0; i<NP_COUNT; ++i) {
            ASSERT((curNode->nodeNeedsProcess[i] & (1ULL<<stIdx)) == 0);
        }
    }
#endif

    // Only propagate if this is the first free slot we are adding
    propagateFree = curNode->nodeFree == 0ULL;
    curNode->nodeFree |= (1ULL<<stIdx);


    if(curNode->nodeReady & (1ULL<<stIdx)) {
        curNode->nodeReady &= ~(1ULL<<stIdx);
        // Only propagate if we removed the last ready node
        propagateReady = curNode->nodeReady == 0ULL;
    }
    pdStrandTableNode_t *parent = curNode->parent;
    ASSERT(hal_islocked(&(curNode->lock)));
    // We flipped nodeFree from 0 to 1. Propagate until we hit a 1
    // We flipped nodeReady from 1 to 0. Propagate until we find sibblings
    PROPAGATE_UP_TREE(
        curNode, parent, 0,
        propagateFree || propagateReady, {
            if (propagateFree) {
                propagateFree = parent->nodeFree == 0ULL;
                parent->nodeFree |= (1ULL<<curNode->parentSlot);
            }
            if (propagateReady) {
                parent->nodeReady &= ~(1ULL<<curNode->parentSlot);
                propagateReady = parent->nodeReady == 0ULL;
            }
        });
END_LABEL(destroyStrandEnd)
    DPRINTF(DEBUG_LVL_INFO, "EXIT _pdDestroyStrand -> %"PRIu32"\n", toReturn);
    return toReturn;
#undef _END_FUNC
}

