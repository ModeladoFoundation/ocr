/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __POLICY_DOMAIN_TASKS_H_
#define __POLICY_DOMAIN_TASKS_H_

#include "debug.h"
#include "ocr-config.h"
#include "ocr-policy-domain.h"
#include "ocr-worker.h"
#include "utils/arrayDeque.h"

#ifndef PDEVT_SCRATCH_BYTES
#define PDEVT_SCRATCH_BYTES 1024 /**< Default number of bytes in a continuation's scratch area */
#endif

#ifndef PDEVT_MERGE_SIZE
#define PDEVT_MERGE_SIZE 4 /**< Default number of events in a merge event */
#endif

#ifndef PDEVT_LIST_SIZE
#define PDEVT_LIST_SIZE 4 /**< Default number of events in a list event */
#endif

#ifndef PDST_NODE_SIZE
#define PDST_NODE_SIZE 64 /**< Default number of elements per strand table node. */
#endif

#ifndef PDPROCESS_MAX_COUNT
#define PDPROCESS_MAX_COUNT 32 /**< Process at most this many strands per table and call to pdProcessStrands */
#endif

#ifndef PDST_ACTION_COUNT
#define PDST_ACTION_COUNT 4 /**< Number of actions per arrayDeque chunk */
#endif

// Certain things need to be powers of 2 (for the arrayDeque basically)
COMPILE_ASSERT((PDEVT_LIST_SIZE & 1) == 0);
COMPILE_ASSERT((PDEVT_MERGE_SIZE & 1) == 0);
COMPILE_ASSERT((PDST_ACTION_COUNT & 1) == 0);

/* Optimization flags (these will go away once we figure out what to use */

/* Define this if you want the insertions
 * to be spread out */
#define MT_OPTI_SPREAD

/* Define this if you want the consumers
 * to look in a spread out manner */
#define MT_OPTI_SPREAD2

/* Define this if you want the initial table
 * to be created as 2 levels initially */
#define MT_OPTI_2LEVEL

/* Define this if you want to elide the table
 * lock in some cases */
#define MT_OPTI_LOCKTABLE

/* Define this if you want to limit contention
 * among consumers */
// WARNING: Seems to cause weird timing issues and crashes in some cases
// needs to be looked at in a bit more detail
//#define MT_OPTI_CONTENTIONLIMIT

/**< Each strand can determine who should process it next (for example, workers or
 * communication workers). This can be managed using different strand tables EXCEPT if
 * the actions on a given strand need to be processed by different entities. For example,
 * a worker needs to send a message that needs to be processed by a comm worker and the
 * response needs to be processed by a regular worker. Add other types here. Note that this
 * adds a u64 value to each pdStrandTableNode_t
 */
#define NP_WORK  0
#define NP_COMM  1
#define NP_COUNT 2

// For now, the implementation is limited to 8 types of NP_*.
 COMPILE_ASSERT(NP_COUNT <= 8);
// We use a 64-bit bitvector to keep track of status so this needs to be properly sized
COMPILE_ASSERT(PDST_NODE_SIZE <= 64);

struct _pdTask_t;

/***************************************/
/********* pdEvent_t definition ********/
/***************************************/

/* Values for the property fields for a pdEvent_t:
 * The bit structure is as follows (32 bits):
 * Bit 0-15:  Type of pdEvent_t; this determines the sub-type of event
 *            Of those bits, the bottom 4 bits encode basic types of
 *            events and the upper bits encode sub-types
 * Bit 16-31: Properties/status bits (defined below).
 */
#define PDEVT_TYPE_MASK    0xFFFF    /**< Mask to extract the type of evt */
#define PDEVT_BASE_TYPE_MASK 0xF
#define PDEVT_READY        0x10000   /**< Bit indicating the event is ready. Events are
                                      * created as not ready */
#define PDEVT_GC           0x20000   /**< Bit indicating that the event should be
                                      * garbage collected if it is the curEvent
                                      * of a strand that is being destroyed (TODO: Do we still need
                                      * this if we reference count the event) */
#define PDEVT_DESTROY_DEEP 0x40000   /**< Bit indicating that a deep destruction
                                      * should occur when this event is destroyed.
                                      * This will, for example, attempt to free
                                      * any pointed-to structures */

struct _pdStrand_t;

/**< Base type indicating a control event */
#define PDEVT_TYPE_BASE_CONTROL 0x0001

/**< Basic event type that is a purely control event */
#define PDEVT_TYPE_CONTROL (PDEVT_TYPE_BASE_CONTROL | (1<<4))
/**
 *
 * @brief Base type for runtime micro-events
 *
 * An event is essentially ready or non-ready and optionally contains a payload.
 * If an event is not ready and the user needs to ensure that it is ready, a slot
 * from one of the strands table will be grabbed and this event will be parked in
 * that table. Its "pointer" will encode the fact that it is in the strands table
 * using the bottom three bits:
 *     - 000: actual pointer
 *     - PDSTT_EVT : in event strands table
 *     - PDSTT_COMM: in communication strands table
 */
typedef struct _pdEvent_t {
    u32 properties;                 /**< Type and other properties for this event. This includes
                                     * if the event is "ready" or not. */
    u32 refCount;                   /**< Reference counter for this event */
    struct _pdStrand_t *strand;     /**< Strand that contains us currently */
} pdEvent_t;

#define PDEVT_TYPE_COMMSTATUS (PDEVT_TYPE_BASE_CONTROL | (2<<4))
typedef struct _pdEventCommStatus_t {
    pdEvent_t base;
    u32 properties;     /**< Status encoded by this event, one of COMM_STATUS_* */
} pdEventCommStatus_t;

/**< Base type indicating the event contains
 * a message */
#define PDEVT_TYPE_BASE_MSG 0x0002

/**< Basic event type that contains a policy message */
#define PDEVT_TYPE_MSG (PDEVT_TYPE_BASE_MSG | (1<<4))

/**< Basic event containing simply a policy message pointer */
typedef struct _pdEventMsg_t {
    pdEvent_t base;
    ocrPolicyMsg_t *msg;    /**< Payload for the event. These are the
                             * "arguments" when passed to a function. */
    ocrTask_t *ctx;         /**< Context for executing this message */
    struct _pdTask_t *continuation; /**< Current continuation to execute (cached version
                             * from the strands table) */
    u32 properties;         /**< Additional properties field (COMM_) */
} pdEventMsg_t;

#define PDEVT_TYPE_MSGINLINE (PDEVT_TYPE_BASE_MSG | (2<<4))
/**< Basic event containing an inline policy message */
typedef struct _pdEventMsgInline_t {
    pdEvent_t base;
    ocrPolicyMsg_t msg;     /**< Payload; this is an inline message (no separate
                                 allocation needed) */
    ocrTask_t *ctx;         /**< Context for executing this message */
    u32 properties;         /**< Additional properties */
} pdEventMsgInline_t;

/**< Base type for merges of events; a merge contains several
 * events and is only ready when all its sub-events are also ready.
 */
#define PDEVT_TYPE_BASE_MERGE 0x0003

/**< Basic event type for a merge of events  */
#define PDEVT_TYPE_MERGE (PDEVT_TYPE_BASE_MERGE | (1<<4))
typedef struct _pdEventMerge_t {
    pdEvent_t base;
    u32 count;                          /**< Number of events in this list */
    u32 countReady;                     /**< Number that are ready */
    arrayDeque_t events;                /**< Events that are merged */
    lock_t lock;                        /**< The arrayDeque_t is not thread safe so this protects adding stuff */
} pdEventMerge_t;

/**< Base type indicating the event contains callback function to processEvent */
#define PDEVT_TYPE_BASE_FCT 0x0005

/**< Basic event type that contains callback function into processEvent */
#define PDEVT_TYPE_FCT (PDEVT_TYPE_BASE_FCT | (1<<4))

/**< Basic event for processEvent callbacks */
typedef struct _pdEventFct_t {
    pdEvent_t base;
    ocrTask_t *ctx;         /**< Context for executing this message */
    struct _pdTask_t *continuation; /**< Current continuation to execute (cached
                                      *  version from the strands table) */
    u32 id;                 /**< Continuation id */
} pdEventFct_t;


/***************************************/
/******** pdAction_t definition ********/
/***************************************/


/* Values for the property fields for a pdAction_t:
 * The bit structure is as follows (32 bits):
 * Bit 0-15: Type of pdAction_t; this determines the sub-type of action
 *           Of those bits, the bottom 4 bits encode basic types of actions
 *           and the upper bits encode sub-types
 * Bit 16-23: Need-processing type (NP_*)
 * Bit 24-31: Properties/status bits (defined below).
 */
#define PDACT_TYPE_MASK    0xFFFF    /**< Mask to extract the type of action */
#define PDACT_BASE_TYPE_MASK 0xF

#define PDACT_NPTYPE_MASK  0xFF0000
#define PDACT_NPTYPE_SHIFT 16

/**< Base type indicating a control action (no
 * real action but used internally by the MT system */
#define PDACT_TYPE_BASE_CONTROL 0x0001

/**
 * @brief Base type for runtime micro-action
 *
 * An action is what the runtime does once an event becomes
 * ready. Actions are processed sequentially for each event but there is no
 * implied ordering between actions tied to different events. Actions can
 * be something as simple as satisfying another event or as complex as running
 * a continuation
 *
 * Properties on an action indicate if:
 *  - TBD
 *
 * An action can be of different type but can also be a specially encoded value
 * which encodes a common action (to avoid the cost of memory allocation). This
 * differentiation happens on the bottom 3 bits: for a regular action (a pointer),
 * those three bits are 0. Other special values currently supported:
 * 001:  Calls the processEvent call passing the event to it.
 *       This allows the very quick encoding of a mechanism to do a callback or
 *       a deferred call (setup an event, enqueue this simple action and let the micro
 *       task scheduler deal with it at some later point) to processEvent
 *       pdEvent_t as an argument and 0 as the PC argument
 * 010:  Send a message out of the policy domain
 * 111: Extended action, the next 5 bits encode the action type:
 *      - 00001: Satisfy another event:
 *                  - Slot ID is bits 32-63
 *                  - Bit 8 will be 1 if the pdEvent_t needs to be copied as opposed
 *                    to just moved
 *      - 00002: Notify a parent merge event that this event is ready
 * An event is essentially ready or non-ready and optionally contains a payload.
 * If an event is not ready and the user needs to ensure that it is ready, a slot
 * from one of the strands table will be grabbed and this event will be parked in
 * that table. Its "pointer" will encode the fact that it is in the strands table
 * using the bottom three bits:
 *     - 000: actual pointer
 *     - PDSTT_EVT : in event strands table
 *     - PDSTT_COMM: in communication strands table
 */

#define PDACT_TYPE_EMPTY (PDACT_TYPE_BASE_CONTROL | (1<<4))
typedef struct _pdAction_t {
    u32 properties;         /**< Type and other properties for this action. This includes
                             the type of the action */
} pdAction_t;


/**< Base type defining that the action to take is the execution of
 * a function call */
#define PDACT_TYPE_BASE_TASK 0x0002

#define PDACT_TYPE_CONTINUATION (PDACT_TYPE_BASE_TASK | (1<<4))
/**< A "continuation" task (or simply a callback) */
typedef struct _pdTask_t {
    pdAction_t base;
    struct _pdEvent_t* (*callback)(struct _pdEvent_t*, u32); /**< Callback to call */
    u32 targetJump;                   /**< Jump point in the callback */

    /* Context for the callback */
    char scratch[PDEVT_SCRATCH_BYTES];/**< Scratch space */
    char* scratchPtr;                 /**< Pointer of the next free space in the scratch space */
    pdEvent_t* evtCtx;                /**< Events that need to be restored when jumping back in (this is a list event) */
} pdTask_t;


/***************************************/
/********* pdStrand_t definition *******/
/***************************************/


#define PDST_TYPE_MASK      0xFF     /**< Type of strand */
#define PDST_FREE           0x0      /**< Slot is free to use */
#define PDST_WAIT           0x3000   /**< The strand is not ready */
#define PDST_WAIT_EVT       0x1000   /**< Slot is occupied and the event is not ready */
#define PDST_WAIT_ACT       0x2000   /**< Slot is occupied, the event is satisfied and has
                                          actions that need to be taken (signal to the runtime
                                          to process those actions) */

#define PDST_HOLD           0x30000  /**< Hold the event in the table */
#define PDST_UHOLD          0x10000  /**< User has an explicit hold on the slot */
#define PDST_RHOLD          0x20000  /**< Runtime has a hold on the slot */

#define PDST_MODIFIED       0x40000  /**< Will be true if the strand has had events
                                          enqueued while being processed. This is used
                                          to properly resolve events */

#define PDST_EVENT_ENCODE(strand, table) ((((strand)->index)<<3) | (table))

struct _pdStrandTableNode_t;
struct _pdStrandTable_t;

#define PDST_TYPE_BASIC 0x1
/**
 * @brief A simple strand of execution
 *
 * A strand of execution is a "recipe" to process an event. When an event becomes
 * ready, the recipe is executed in order
 */
typedef struct _pdStrand_t {
    pdEvent_t* curEvent;    /**< Event currently pointed to by this slot */
    struct _pdStrandTableNode_t *parent; /**< Parent of this strand. Note that the slot
                                          number is index & ((1ULL<<6)-1) */
    struct _pdStrandTable_t *containingTable; /**< Table this strand is in */
    u64 index;              /**< Index into the table */
    ocrWorker_t* processingWorker;  /**< Worker that is processing this strand
                                         This has implications on locking among other things */
    ocrTask_t* contextTask; /**< EDT that was active/live when the strand was created.
                                 This is restored when the actions on the strand are processed */
    arrayDeque_t *actions;  /**< Deque of actions to perform once event is ready. This
                             * implementation is a variable size array */
    arrayDeque_t *bufferedActions; /**< Deque containing actions that are buffered when
                                    * the strand is locked */
    lock_t bufferedLock; /**< Lock for the buffered actions structure */
    u32 properties;/**< Properties and status of this slot. */
    lock_t lock;
    u8 bufferedHoldClear; /**< Will be true if the actions in bufferedActions should clear
                            the hold once they are merged */
} pdStrand_t;


#define PDST_TYPE_COMM 0x2
/**< Strand that includes an additional communication buffer for a communication
 backend to use */
typedef struct _pdStrandComm_t {
    pdStrand_t base;
    u64 bufferSize;
    void* buffer;
} pdStrandComm_t;

typedef struct _pdStrandTableNode_t {
    lock_t lock;            /**< Lock on the status bits */
    u64 nodeFree;           /**< Bit will be 1 if slot/node contains a free slot */
    u64 nodeNeedsProcess[NP_COUNT];   /**< Bit will be 1 if slot/node needs to be processed (event ready and actions pending) */
    u64 nodeReady;          /**< Bit will be 1 if slot/node has ready slot (event ready, no actions to process
                                 [TODO: currently not used for search -- remove?] */
    u64 lmIndex;            /**< Bit 0: 1 if leaf node; 0 otherwise. Other bits:
                               index of the left-most strand in this subtree */
    union {
        pdStrand_t* slots[PDST_NODE_SIZE];
        struct _pdStrandTableNode_t* nodes[PDST_NODE_SIZE];
    } data;
    struct _pdStrandTableNode_t *parent; /**< Parent of this node */
    u32 parentSlot;         /**< Slot position in our parent */
} pdStrandTableNode_t;

/* The following constants are used as the last 3 bits of an event "pointer".
 * An event is only a true event if those bits are 0, otherwise, it encodes an
 * index (in the upper bits) in a table (indicated by these three bits
 */
#define PDSTT_EVT  0b1      /**< ID of the event strands table */
#define PDSTT_COMM 0b10     /**< ID of the communication strands table */
#define PDSTT_LAST 0b10     /**< Should be equal to last strands table */

/**< A strand table. This is where all events that are being waited on will
 be stored */
typedef struct _pdStrandTable_t {
    // This data-structure is a tree with an arity of PDST_NODE_SIZE
    // The tree is not always full though
    pdStrandTableNode_t *head;    /**< Root of the tree */
    u32 levelCount;             /**< Number of levels; 0 means empty */
    lock_t lock;
#ifdef MT_OPTI_CONTENTIONLIMIT
    s32 consumerCount[NP_COUNT]; /**< Counter determining if a consumer should enter
                                      This counter is initially 0 and incremented when
                                      new work needs to be processed. It is decremented when
                                      a consuming thread enters. This limits contention on
                                      internal data-structure where there is little work
                                      to be found */
#endif
} pdStrandTable_t;


/***************************************/
/***** pdEvent_t related functions *****/
/***************************************/


/**
 * @brief Creates a slab-allocated event of the given type.
 *
 * A pointer to the event is returned. If reserveInTable is not zero, the event
 * is also inserted directly in a strands table (PDSTT_EVT -> event table; PDSTT_COMM -> comm
 * table and marked as held by the user.
 * A created event is always non-ready.
 *
 * @param[in] pd                Pointer to this policy domain. Can be NULL
 *                              but the call will have to resolve it using getCurrentEnv
 * @param[out] event            Returns the newly allocated event. The event is initially non-ready
 * @param[in] type              Type of event to create. See PDEVT_TYPE_MASK
 * @param[in] reserveInTable    If non-zero, hold a slot for the event in the
 *                              strands table (sets PDST_UHOLD). This value should
 *                              be PDSTT_EVT or PDSTT_COMM for now (event or comm)
 * @return a status code:
 *  - 0: indicates success
 *  - OCR_EINVAL indicates that type or reserveInTable is invalid. Note that if
 *    reserveInTable is invalid, an event will still be created and returned. Check
 *    *event to see if it is non-NULL
 *  - OCR_ENOMEM indicates that there was no memory to create the event
 *
 *  @todo Do we need another call where events could be created/initialized using a callback
 *  This would allow for more event extensions and allow users to plug in
 *  whatever they want in it. I don't have a good use case for it right now
 *  so leaving that part out
 */
u8 pdCreateEvent(ocrPolicyDomain_t *pd, pdEvent_t **event, u32 type, u8 reserveInTable);

/**
 * @brief Grabs a reference on an already existing event
 *
 * Events are reference counted and this increments the count by 1.
 * The event passed in must be a real event (not a fake pointer-like event).
 *
 * To prevent any leaks, the following must hold:
 * #referenceEvent + 1 = #destroyEvent
 *
 * The +1 is the creation call which increments the reference count by 1
 *
 * @param[in] pd    Pointer to this policy domain. Can be NULL but the call
 *                  will resolve it with getCurrentEnv
 * @param[in] event Event to reference (valid pointer)
 *
 * @return a status code:
 *   - 0: indicates success
 *   - OCR_EINVAL indicates that the event is invalid
 */
u8 pdReferenceEvent(ocrPolicyDomain_t *pd, pdEvent_t *event);

/**
 * @brief Destroys and frees an event if its reference count is 0
 *
 * Events are managed in a reference counted manner so the event
 * will only be destroyed if its reference count reaches 0. In
 * all other cases, this call decrements the reference count.
 *
 * This call will do a deep free if PDEVT_DESTROY_DEEP is set
 * on the event's property.
 *
 * The associated strand for the event is not freed. Care
 * must be taken to make sure the strand will be properly
 * removed automatically if the event is destroyed (ie:
 * that nothing will try to use a destroyed event). A strand
 * is automatically removed if there are no more associated actions
 * and there is no HOLD.
 * This call does do some sanity
 * check and warns in case something seems wrong
 *
 * @param[in] pd        Policy domain to use. Can be NULL but this
 *                      means getCurrentEnv will be used to resolve
 *                      the PD.
 * @param[in] event     Event to destroy and free
 * @return a status code:
 *          - 0 if everything was successful
 *          - OCR_EINVAL if the event is invalid either because:
 *                       - it was ready but had pending actions (likely race)
 *                       - it did not have a valid refCount
 */
u8 pdDestroyEvent(ocrPolicyDomain_t *pd, pdEvent_t *event);

/**
 * @brief "Resolves" an event, converting an event-like pointer to an actual event
 *
 * This call will always return a pointer to an actual event even if the event is
 * not ready (in which case the OCR_EBUSY value is returned). An event is considered
 * ready iff the event is ready AND no actions need to be taken on the event (the strands
 * deque is empty). In that case, the event's pointer is returned and 0 is returned.
 *
 * This call can also be used on a regular event pointer in which case OCR_ENOP is returned if
 * the event is ready and OCR_EBUSY is returned if it is not ready
 *
 * @param[in] pd            Policy domain to use. Can be NULL but this means that getCurrentEnv
 *                          will be used to resolve the PD.
 * @param[in/out] evtValue  Event to resolve. If the event is ready, on output, will
 *                          contain a valid pdEvent_t pointer. If the event is not
 *                          ready, this value will be unchanged
 * @param[out] strand       If non-NULL, on return, will contain the strand this event
 *                          refers to. This is true irrespective of whether or not
 *                          the event is ready and the returned value can therefore be used
 *                          for other strand related functions if the event is not ready (like
 *                          enqueuing)
 * @param[in] clearFwdHold  Set to 1 if this is a forward execution and the event can
 *                          be removed from the strands table if ready.
 *
 * @warning If clearFwdHold is true AND the event is ready AND the GC (garbage collect)
 * flag is set on the event, the evtValue returned will point to an invalid memory
 * location (as the event is garbage collected when the strand is destroyed)
 * @return a status indicating the state of the resolution:
 *          - 0 if the event is ready and has been resolved
 *          - OCR_EBUSY if the event is still not ready
 *          - OCR_EINVAL if evtValue is invalid
 *          - OCR_ENOP if evtValue was already an event pointer
 */
u8 pdResolveEvent(ocrPolicyDomain_t *pd, u64 *evtValue, pdStrand_t **strand, u8 clearFwdHold);


/**
 * @brief "Encodes" an event, converting an actual event to an event-like pointer
 *
 * This is the opposite operation to pdResolveEvent() and is used when the event is going
 * to be accessed concurrently with the processing of its strand.
 *
 * To explain the role of this function a little better, consider the following scenario:
 *   - an event is created and associated with a strand.
 *   - the user enqueues on the strand an action to process said message (action1)
 *   - the user enqueues a second action on the strand to process the result of action1 (action2)
 *   - the user returns the event (a non-ready event here because there are two actions
 *     pending) to the caller
 * A graphical representation of this is as follows:
 *    evt1 -> action1 -> evt2 -> action2 -> evt3
 * evt1 is the initial created event that will be processed by action1 which may return a result
 * (evt2) which will in turn be processed by action2 which returns evt3. evt1/2/3 do not have to
 * be the same events and previous events are automatically destroyed by runtime (ie: at the end
 * of the day, evt3 will be the only one left if it is not garbage collected).
 *
 * At this point, if the caller wants to wait on the event, using the pointer to evt1 would
 * be inccorect because evt1 may have been destroyed because action1 may have finished and
 * produced evt2. What the caller really wants is to wait for action1 and action2 and resolve
 * the event to be evt3 (the result of the entire chain). This call will therefore convert evt1
 * into a pseudo event-like pointer that will actually always point to the result of the entire
 * strand (so evt3 when it is ready).
 *
 * @note The event needs to be part of a strand for this call to succeed
 * @warning Be wary of race conditions. This call should be made *BEFORE* the processing of the
 * strand can begin. In the example above, this can be anytime the strand is initially locked (no
 * processing happens while the strand is locked). Or, if the event is not initially ready,
 * anytime before the event becomes ready (no actions will be processed while the event is not
 * ready).
 *
 * @param[in] pd            Policy domain to use. Can be NULL but this means that getCurrentEnv
 *                          will be used to resolve the PD.
 * @param[in/out] evtValue  Event to encode. If already encoded, OCR_ENOP will be returned
 * @return a status indicating the status of the encoding:
 *          - 0 if the event has been successfully encoded
 *          - OCR_EINVAL if evtValue is invalid (most likely not part of a strand)
 *          - OCR_ENOP if evtValue was already an event pointer
 */
u8 pdEncodeEvent(ocrPolicyDomain_t *pd, u64 *evtValue);

/**
 * @brief Marks the event as ready and updates internal data-structures
 *
 * The runtime makes no assumptions as to what it means for an event to
 * be ready and instead relies on the user to tell it when an event is ready.
 * However, the user should not flip the ready bit in the property field by him/herself
 * and should instead use this call to do so. The readying of an event has
 * internal consequences for the micro-task scheduler and these changes need
 * to be properly propagated. This call takes care of that.
 *
 * This call expects the event to be NON-READY and will fail if the event
 * is not in that state.
 *
 * @param[in] pd    Policy domain to use. Can be NULL.
 * @param[in] event Event to mark as ready
 * @return a status indicating the status of the satisfaction:
 *     - 0: everything went OK
 *     - OCR_EINVAL: event is not a valid event
 */
u8 pdMarkReadyEvent(ocrPolicyDomain_t *pd, pdEvent_t *event);

/**
 * @brief Marks the event as no longer ready and updates internal data-structures
 *
 * This makes the event 'not ready' and is used if an event is being reused (for
 * example at various points in a strand).
 *
 * @warn There is a natural race between this call and the automatic deletion of
 * events by the runtime (when the strand they are in is fully ready). The user
 * is responsible for making sure this cannot happen. A few rules should be followed:
 *     - If the event is not part of a strand, there is no issue since the
 *       event will not be freed.
 *     - If the event is part of a strand, make sure that there are either
 *       actions left and/or a hold (RHOLD or UHOLD is exercised on the strand)
 * This call expects the event to be READY and will fail if the event
 * is not in that state.
 *
 * @param[in] pd    Policy domain to use. Can be NULL.
 * @param[in] event Event to mark as non-ready
 * @return a status indicating the status of the change:
 *     - 0: everything went OK
 *     - OCR_EINVAL: event is not a valid event
 */
u8 pdMarkWaitEvent(ocrPolicyDomain_t *pd, pdEvent_t *event);

/**
 * @brief Add an event to a merge event
 *
 * A merge event contains several events and will
 * be marked as ready when all the events that it contains are
 * also marked as ready.
 *
 * To add events to the merge event, use this function. Once
 * all events have been added, you need to call pdCloseMerge().
 * This is to prevent a race condition between the adding of
 * events and the parallel satisfaction of other events that
 * may have already been added.
 *
 * @note If the event to add is not ready, NULL can be passed in as the last
 * parameter and the event will be properly set by the action of satisfying
 * a merge event. If the event is passed in and is a real event, it should be
 * ready. If it is not a real event, it will be considered the same as NULL.
 *
 * @param[in] pd          Policy domain to use. Can be NULL
 * @param[out] position   Position this event was added in. This is
 *                        needed to create the appropriate action to satisfy
 *                        this event
 * @param[in] mergeEvent  Merge event to modify
 * @param[in] event       Event to add if already ready or NULL otherwise
 * @return a status code indicating the status of the change:
 *     - 0: everything went OK
 *     - OCR_EINVAL: invalid mergeEvent or event
 *     - OCR_ENOMEM: not enough memory to add the event
 */
u8 pdAddEventToMerge(ocrPolicyDomain_t *pd, u64 *position, pdEventMerge_t *mergeEvent, pdEvent_t *event);

/**
 * @brief Indicates that no more events will be added to a merge event
 *
 * When initially created, the merge event is prevented from being
 * satisfied until pdCloseMerge is called. This prevents a race
 * condition between the adding of events and the concurrent satisfaction
 * of previously added events
 * @details [long description]
 *
 * @param[in] pd          Policy domain to use. Can be NULL
 * @param[in] mergeEvent  Merge event to close
 *
 * @return a status code indicating the status of the change:
 *     - 0: everything went OK
 *     - OCR_EINVAL: invalid mergeEvent
 */
u8 pdCloseMerge(ocrPolicyDomain_t *pd, pdEventMerge_t *mergeEvent);

/***************************************/
/***** pdAction_t related functions ****/
/***************************************/

/**
 * @brief Creates an action that is a simple callback
 *
 * This action will simply execute the function processEvent, passing
 * it:
 *  - the current policy domain
 *  - the ready event in the strand
 *  - the value 0 signifying the start of the function (if this is a continuable function
 *
 * @param[in] workType   Type of worker that can process this message (NP_COMM or NP_WORK)
 * @return A pdAction_t pointer encoding the action. This can be used in pdEnqueueAction()
 *         to add this action to a strand. Note that the "pointer" returned may not be
 *         a true pointer and should not be used directly
 */
pdAction_t* pdGetProcessMessageAction(u32 workType);


pdAction_t* pdGetProcessEventAction(ocrObject_t* object);


/**
 * @brief Creates an action that will satisfy the given event
 *
 * This action will switch the event from not being ready
 * to being ready. If the event has actions waiting to
 * execute, those will be available to be processed.
 *
 * @warning This should NOT be used on events that are
 * being waited on as part of a pdEventMerge_t event
 * and instead the pdGetSatisfyMergeAction() should be used
 *
 * @warning Events to satisfy MUST be part of a strand (this kind
 * of subsumes the previous case but calling that one out
 * explicitly)
 *
 * @note Making an event ready does not mean the event
 * will resolve because actions may still need to be
 * processed
 *
 * @param[in] curStrand Pointer to the current strand (the one on which this
 *                      action is being enqueued)
 * @param[in] event     Pointer (either resolved or not) to
 *                      the event to make ready
 * @return A pdAction_t pointer encoding the action. This can be used in pdEnqueueAction()
 *         to add this action to a strand. Note that the "pointer" returned may not
 *         be a true pointer and should not be used directly
 */
pdAction_t* pdGetMarkReadyAction(pdStrand_t *curStrand, pdEvent_t *event);

/**
 * @brief Creates an action that will mark as ready the event in
 * position 'position' of the merge event
 *
 * This call is similar to pdGetMarkReadyAction() but for events
 * that are within a merge event. It is assumed that the event at position
 * in the merge event is the current strand's event. In other words, this will
 * replace the event in the merged event with the one at this point in the strand.
 *
 * @param[in] curStrand   Pointer to the current strand (the one on which this
 *                        action is enqueued)
 * @param[in] mergeEvent  Merge event to satisfy. This event must be a resolved event
 * @param[in] position    Position to satisfy (this is given by the pdAddEventToMerge() function)
 * @return [description]
 */
pdAction_t* pdGetSatisfyMergeAction(pdStrand_t *curStrand, pdEventMerge_t *mergeEvent, u64 position);

/***************************************/
/***** pdStrand_t related functions ****/
/***************************************/

/**
 * @brief Initialize a strand table
 *
 * This should be called to initialize a table. This does not allocate any memory
 * but sets up the data-structures to sane values
 *
 * @param[in] pd            Policy domain
 * @param[in] table         Table to initialize
 * @param[in] properties    Unused for now
 * @return a status code:
 *     - 0: successful
 *     - OCR_EINVAL: 'table' is NULL
 */
u8 pdInitializeStrandTable(ocrPolicyDomain_t* pd, pdStrandTable_t *table, u32 properties);


/**
 * @brief Destroy the internal structures of a strand table
 *
 * This should be called to destroy any memory that has been allocated as part of
 * building and using the strand table. This does not free the table itself.
 *
 * @warn The table should be entirely empty (all nodes freed up) prior to calling this
  *
 * @param[in] pd            Policy domain
 * @param[in] table         Table to destroy
 * @param[in] properties    Unused for now
 * @return a status code:
 *     - 0: successful
 *     - OCR_EINVAL: 'table' was not entirely empty
 */
u8 pdDestroyStrandTable(ocrPolicyDomain_t* pd, pdStrandTable_t *table, u32 properties);


/**
 * @brief Gets a new strand from the strand table and inserts the event into it
 *
 * This reserves a new slot in the strand table 'table' and assigns the event to that
 * strand. The action queue is initially empty and the properties for the strand will be:
 *     - PDST_RHOLD
 *     - PDST_READY or PDST_WAIT_EVT depending on whether the event is ready or not
 *
 * The caller may specify *additional* properties on the strand using the
 * 'properties' argument (for example PDST_READY_UHOLD)
 *
 * @note This call will lock the strand created. A call to pdUnlockStrand() is needed
 * to release the lock
 *
 * @param[in] pd            Policy domain to use. If NULL, getCurrentEnv will be used
 *                          to determine policy domain
 * @param[out] strand       Returned strand allocated
 * @param[in] table         Strand table to search
 * @param[in/out] event     Event to store in the strand. On output, the strand will
 *                          be properly updated
 * @param[in] properties    Additional properties to set on the strand
 * @return a status code:
 *      - 0 if successful
 *      - OCR_ENOMEM if there is not enough memory
 *      - OCR_EINVAL if the properties value is insane
 */
u8 pdGetNewStrand(ocrPolicyDomain_t* pd, pdStrand_t **strand, pdStrandTable_t *table,
                  pdEvent_t* event, u32 properties);


/**
 * @brief Gets a strand for the given index
 *
 * This returns the strand in table at index "index". If the strand does not exist,
 * OCR_EINVAL will be returned
 *
 * @note The strand is not locked by this call. This call is also non-blocking
 *
 * @param[in] pd        Policy domain to use. Can be NULL and getCurrentEnv will be used
 * @param[out] strand   Returned strand
 * @param[in] table     Strand table to use
 * @param[in] index     Index into the table
 * @return a status code:
 *      - 0: successful
 *      - OCR_EINVAL: index is invalid (no strand of that index exists)
 */
u8 pdGetStrandForIndex(ocrPolicyDomain_t* pd, pdStrand_t **strand, pdStrandTable_t* table,
                       u64 index);

/**
 * @brief Enqueue an action on the strand
 *
 * This enqueues the action 'action' to the strand at the end of its action
 * queue. If 'clearFwdHold' is 1, this will clear the hold that the runtime holds
 * on a strand when it does not know if other actions are going to be added. This
 * does not mean that other actions can't be added on later but just that there
 * is no longer a race between adding an action and removing a ready strand
 *
 * @note If a lock on the strand is held, the actions will be directly enqueued and
 * the lock is *NOT* released. If a lock on the strand is NOT held a try lock is attempted:
 *   - if successful, the actions are directly enqueued and the lock is released
 *   - if unsuccessful, the actions are buffered in another side structure and will be
 *     merged with the main list of actions at a later point. This mode of operation is
 *     specifically targeted at allowing simultaneous processing of the strand and adding
 *     further actions to the strand. This way, threads that want to add actions to a strand
 *     only have to wait for other threads adding actions to a strand (short) as opposed to
 *     the potentially long time it takes to process the actions already present.
 *
 * @param[in] pd            Policy domain to use. Can be NULL; it will be resolved by
 *                          getCurrentEnv() in that case
 * @param[in/out] strand    Strand to add the action to
 * @param[in] actionCount   Number of actions to enqueue
 * @param[in] actions       List of actions (pointer or specially encoded) to add
 * @param[in] clearFwdHold  1 if the runtime should release its hold. 0 otherwise
 * @return a status indicating the state of the addition:
 *          - 0: successful addition
 *          - OCR_EINVAL: actionCount is 0, actions are invalid
 *          - OCR_ENOMEM: not enough memory
 *          - OCR_EFAULT: Runtime error
 */
u8 pdEnqueueActions(ocrPolicyDomain_t *pd, pdStrand_t* strand, u32 actionCount,
                    pdAction_t** actions, u8 clearFwdHold);

/**
 * @brief Replaces the current action with a new one
 *
 * This is used when a function has been "re-entered"/"entered" in a strand
 * after a callback (in other words, a strand was created, an action enqueued and
 * we are now processing that action) and replaces the current action with a new one.
 * This is effectively used to "insert" an action before whatever comes next. For example,
 * if the following actions are enqueued:
 * A -> B -> C and we are now processing A (obviously since actions are processes sequentially),
 * using this function passing D as the action parameter would result in the chain:
 * D -> B -> C
 *
 * @param[in] pd      Policy domain to use. Can be NULL; it will be resolved by
 *                    getCurrentEnv() in that case
 * @param[in] strand  Strand to add the action to
 * @param[in] action  Action to add (pointer or specially encoded)
 * @return a status indicating the state of the addition:
 *          - 0: successful addition
 *          - OCR_EINVAL: invalid action or the current worker is not the one processing the strand
 *          - OCR_ENOMEM: not enough memory to add the action
 *          - OCR_EFAULT: Runtime error
 */
u8 pdReplaceCurrentAction(ocrPolicyDomain_t *pd, pdStrand_t* strand, pdAction_t *action);


/**
 * @brief Grabs a lock on the strand 'strand'
 *
 * This gets a lock on the strand 'strand'. The user can optionally specify that
 * he just wants to *try* to grab the lock in which case this call is non-blocking
 *
 *
 * @param[in] strand    Strand to lock
 * @param[in] doTry     If true, this call is non-blocking and will only attempt to grab
 *                      the lock on the strand
 * @return a status code:
 *      - 0: successful
 *      - OCR_EINVAL: strand is invalid
 *      - OCR_EBUSY: the lock could not be acquire (is doTry is true)
 */
u8 pdLockStrand(pdStrand_t *strand, bool doTry);

/**
 * @brief Unlocks the strand
 *
 * This call should be used after a pdLockStrand or other call that locks the
 * strand (pdGetNewStrand() or pdGetStrandForIndex() for example)
 *
 *
 * @param[in] strand    Strand to unlock
 * @return a status code:
 *      - 0: successful
 *      - OCR_EINVAL: strand is invalid or unlocked already
 *      - status codes from pdEnqueueActions may also be returned in certain circumstances. If
 *        something other than OCR_EINVAL is returned, look at those error codes for their meaning
 */
u8 pdUnlockStrand(pdStrand_t *strand);


/***************************************/
/****** Global scheduler functions *****/
/***************************************/

#define PDSTT_EMPTYTABLES 0x1
#define PDSTT_CLEARHOLD   0x2

/**
 * @brief Main entry into the micro-task processor
 *
 * This function will process a certain number of events as determined by the
 * policy of the micro-task scheduler and the properties passed. This call
 * will not block.
 *
 * @param[in] pd            Policy domain to use
 * @param[in] processType   Type of messages we can process (NP_WORK, NP_COMM, etc.)
 *                          If multiple types can be processed, this call needs
 *                          to be called multiple times
 * @param[in] properties    Properties (currently just PDSTT_EMPTYTABLES)
 * @return a status code:
 *      - 0: successful
 *      - OCR_EINVAL: Invalid value for processType or properties
 *      - OCR_EAGAIN: temporary failure, retry later
 *
 */
u8 pdProcessStrands(ocrPolicyDomain_t *pd, u32 processType, u32 properties);

/**
 * @brief Entry into the micro-task processor for a fixed number of times
 *
 * This function is similar to pdProcessStrands() except that it
 * processes up-to the number of strands specified (it will stop processing
 * if there is nothing left for it to do
 *
 * @param[in] pd            Policy domain to use
 * @param[in] processType   Type of messages we can process (NP_WORK, NP_COMM, etc.)
 *                          If multiple types can be processed, this call needs
 *                          to be called multiple times
 * @param[in/out] count     Maximum number of strands to process; on
 *                          output contains the number of strands actually processed
 * @param[in] properties    Properties (currently unused)
 * @return a status code:
 *      - 0: successful. This does not mean that any strands were processed
 *      - OCR_EINVAL: Invalid value for processType or properties
 *      - OCR_EAGAIN: Temporary failure, retry later
 */
u8 pdProcessNStrands(ocrPolicyDomain_t *pd, u32 processType, u32 *count, u32 properties);

/**
 * @brief Process tasks until the events required are resolved
 *
 * This function will not return until the events specified have been fully
 * resolved. It will continue processing tasks until that happens. This is
 * used to implement a wait on a given event (for example). On return, the
 * event array will contain the resolved events.
 *
 * @param[in] pd            Policy domain to use
 * @param[in] processType   Type of message we can process (NP_WORK, NP_COMM, etc.)
 *                          If multiple types can be processed, this call needs to be
 *                          called multiple times
 * @param[in] count         Number of events to try to resolve
 * @param[in/out] events    Events to resolve. On successful return, contains the
 *                          resolved events
 * @param[in] properties    Properties; currently just PDSTT_CLEARHOLD if
 *                          events in events should be resolved and have their runtime
 *                          hold cleared
 * @return a status code:
 *      - 0: successful
 *      - OCR_EAGAIN: temporary failure, retry later
 * @todo Define properties
 * @note The events you want resolved don't have to be of the type you can
 * process. Other workers may process those other events. The only guarantee
 * of this call is that when it returns, all events in the events array will
 * have been resolved. It does NOT guarantee that this thread will be the one
 * resolving them.
 */
u8 pdProcessResolveEvents(ocrPolicyDomain_t *pd, u32 processType, u32 count,
                          pdEvent_t **events, u32 properties);

#endif /* __POLICY_DOMAIN_TASKS_H__ */

