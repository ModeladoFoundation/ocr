/**
 * @brief Basic types used throughout the OCR library (runtime only types)
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_RUNTIME_TYPES_H__
#define __OCR_RUNTIME_TYPES_H__

#include "ocr-types.h"
#include "utils/profiler/profiler.h"

#define INVALID_LOCATION ((u64)(-1))

/* Summary of property flags visible to the user */
#define EDT_PROP_ALL  ((u16) 0x3)
#define EVT_PROP_ALL  ((u16) 0x1)
#define DB_PROP_ALL   ((u16) 0x70)
#define GUID_PROP_ALL ((u16) 0x700)

// Mask for runtime properties on GUIDs
#define GUID_RT_PROP_ALL    ((u16) 0x7000)
// The GUID value passed is valid
#define GUID_PROP_ISVALID   ((u16) 0x1000)
// The GP should record the passed GUID
#define GUID_PROP_TORECORD  ((u16) 0x2000)
//Warning: 0x4000 is used by DB_PROP_IGNORE_WARN. No space left.

/* Run-level support */
typedef enum _ocrRunlevels_t {
    RL_CONFIG_PARSE, /**< Configuration has been parsed; RT structures exist */
    RL_NETWORK_OK,   /**< Intra-PD communication is possible; PDs are known */
    RL_PD_OK,        /**< PDs are up. One capable module per PD */
    RL_MEMORY_OK,    /**< On startup, barrier before here. Memory allocators are up
                        pdMalloc() is functional */
    RL_GUID_OK,      /**< The global naming is operational; PDs know not to step
                        on each other's toes */
    RL_COMPUTE_OK,   /**< All other inert modules are brought up; bring up workers */
    RL_USER_OK,      /**< One PD starts mainEDT; others are waiting for work */
    RL_MAX           /**< Not a runlevel. Internal marker */
} ocrRunlevel_t;

typedef u8 phase_t;

// BUG #583: Do we need to explicitly expand this
// to the subcomponents (platform/target for example)
typedef enum _ocrRLPhaseComponents_t {
    RL_PHASE_COMMAPI,
    RL_PHASE_ALLOCATOR,
    RL_PHASE_GUIDPROVIDER,
    RL_PHASE_SCHEDULER,
    RL_PHASE_WORKER,
    RL_PHASE_MAX
} ocrRLPhaseComponents_t;

/* Flags for runlevels */
#define RL_REQUEST     0x1  /**< Only used in policy message: request to change run-level */
#define RL_RESPONSE    0x2  /**< Only used in policy message: in the case of a RL_BARRIER message, respond that RL transitioned */
#define RL_RELEASE     0x4  /**< Only used in policy message: in the case of a RL_BARRIER message, release from the barrier */
#define RL_ASYNC       0x10  /**< Set if the caller can proceed even if the runlevel change is not complete */
#define RL_BARRIER     0x20  /**< Set if the caller should wait for the callee to return from the runlevel change */
#define RL_BRING_UP    0x100  /**< Set if this is for OCR bring-up */
#define RL_TEAR_DOWN   0x200  /**< Set if this is for OCR tear-down */
#define RL_LEGACY      0x400  /**< Set when we run in legacy mode */
#define RL_PD_MASTER   0x1000 /**< Set if the thread is the first capable thread of a PD */
#define RL_NODE_MASTER 0x3000 /**< Set if the thread is the first on the node (potentially starts multiple PDs);
                                * implies RL_PD_MASTER */
#define RL_BLESSED     0x4000 /**< Set if the worker is the blessed worker that should initialize mainEdt */
#define RL_FROM_MSG    0x8000 /**< Set if the transition came from another PD or was triggered internally
                                 (as opposed to being called directly using switchRunlevel on the PD).
                                 This implies that the switchRunlevel PD call should return and not wait
                                 for the transition*/

struct _ocrObject_t;
struct _pdEvent_t;

// BUG #605
// This is a placeholder for something that identifies a memory,
// a compute node and a policy domain. Whatever else this becomes in the future, it
// includes the "engine index" in the low order eight bits.  Irrelevant for other
// platforms, this is needed by TG where it provides the block-based index for which
// processor or memory is identified:  0 == CE, 1-8 == XE; additional bits allow for
// a differnt number of XE's in other potential TG hardware family members.
typedef u64 ocrLocation_t;
#define UNDEFINED_LOCATION ((u64)-1)

#define UNINITIALIZED_NEIGHBOR_INDEX ((u64)-1)

/**
 * @brief Base type for OCR Objects.
 *
 */
typedef struct _ocrObject_t {
    ocrGuid_t guid;
    u32 fctId;              /**< Factory ID for this object */
} ocrObject_t;

#define getObjectField(self, name) (((ocrObject_t *)(self))->name)

#define setObjectField(self, name, value) (((ocrObject_t *)(self))->name = value)

/**
 * @brief Functions common to all objects
 */
typedef struct _ocrObjectFcts_t {
    /**
     * @brief Implement all potentially asynchronous processing on
     * objects
     *
     * With micro-tasks, user-defined processing functions are
     * always in the form (object, in/out event, continuation index).
     * This function is therefore defined for all objects when they may
     * need to do something that is potentially blocking (for example, acquiring
     * a data-block may involve fetching the data).
     *
     * @param[in] self          Pointer to this object
     * @param[in/out] event     Event to process (on input). On output, contains
     *                          the "response" of this function. Both event and *event
     *                          should be non-NULL.
     * @param[in] idx           Position in the code to resume (for continuations). Set
     *                          to 0 to start at the beginning of the function.
     * @return 0 on success and an error code otherwise
     *
     * @note If prior to the call *event has value ptr, the event pointed to by ptr should
     * not be considered valid anymore. If you need to keep both the event passed in and
     * the event returned, make a copy of the event passed-in first. This is to keep
     * in line with the fact that if this call was an individual action, the
     * source event would be destroyed if another one was returned
     */
    u8 (*processEvent)(struct _ocrObject_t* self, struct _pdEvent_t** event, u32 idx);
} ocrObjectFcts_t;

/**
 * @brief Base type for OCR Object factories.
 *
 * This will contain basic information for all
 * object factories (that create templates, EDTs,
 * events, data-blocks, etc.)
 */
typedef struct _ocrObjectFactory_t {
    ocrObjectFcts_t fcts;   /**< Functions for this object factory */

    /**
     * @brief Destructor for this factory
     * @details Destroys this factory. This function is pure virtual and is implemented by the derived class
     *
     * @param factory[in] Factory to destroy
     * @return Void
     */
    void (*destruct)(struct _ocrObjectFactory_t *factory);
    u8 (*clone)(struct _ocrObjectFactory_t * factory, ocrGuid_t, ocrObject_t ** mdPtr);
    u8 (*serialize)(struct _ocrObjectFactory_t * factory, ocrGuid_t guid, ocrObject_t * src, u64 * mode, ocrLocation_t destLocation, void ** destBuffer, u64 * destSize);
    u8 (*deserialize)(struct _ocrObjectFactory_t * factory, ocrGuid_t evtGuid, ocrObject_t ** dest, u64 mode, void * srcBuffer, u64 srcSize);
    // The size of the metadata if it was to be serialized at the time of the call
    u8 (*mdSize)(ocrObject_t *dest, u64 mode, u64 * size);
} ocrObjectFactory_t;

// Metadata Management
#define ocrObjectOperation_t u32
#define MD_DIR_PULL 1
#define MD_DIR_PUSH 2

#define MD_CLONE 1
#define MD_MOVE  2

/**
 * @brief Memory region "tags"
 *
 * A tag indicates the type of the memory region in the
 * mem-target or mem-platform. Other tags may be added
 * as needed.
 * @see chunk() tag() queryTag()
 * @note Must be sequential and have MAX_TAG for rangeTracker to work
 */
typedef enum {
    RESERVED_TAG   = 0, /**< Non-usable chunk of memory */
    NON_USER_TAG    = 1, /**< Regions that are non user managed (text, bss for example */
    USER_FREE_TAG   = 2, /**< Regions that are free to be used by higher levels */
    USER_USED_TAG   = 3, /**< Regions that the higher levels have used */
    MAX_TAG         = 4  /**< Marker of the end of the tags */
} ocrMemoryTag_t;

/**
 * @brief "Capabilities" for comp-target and comp-platform
 *
 * Computation resources can have capabilities that allow minimal
 * support for heterogeneous infrastructure.
 */
typedef enum {
    COMP_CAPABILITY = 0x1, /**< Node is capable of computation */
    COMM_CAPABILITY = 0x2, /**< Node is capable of communicating with other nodes */
    MAX_CAPABILITY  = 0x3  /**< OR of all the previous capabilities */
} ocrComputeCapability_t;

/**
 * @brief Properties for the sendMessage calls
 * indicating the usage model of the call by the caller
 *
 * These flags allow the backing comm-platform to make
 * assumptions about the message being passed in. In
 * particular, the comm-platform can make assumptions
 * about whether the storage for a message is kept around
 * after the call
 */
typedef enum {
    TWOWAY_MSG_PROP          = 0x1,   /**< A "response" is expected for
                                       * this message */
    PERSIST_MSG_PROP         = 0x2,   /**< The input message is guaranteed to be
                                       * valid until *after* a successful poll/wait */
    PENDING_MSG_PROP         = 0x4,   /**< Message not processed yet*/
    BLOCKING_SEND_MSG_PROP   = 0x8,   /**< comm-layer will not return until
                                           message is sent successfully */
    ASYNC_MSG_PROP           = 0x10,  /**< Asynchronous msg processing */
    PRIO1_MSG_PROP           = 0x100, /**< Lowest priority message */
    PRIO2_MSG_PROP           = 0x200, /**< Higher priority message */
    PRIO3_MSG_PROP           = 0x400, /**< Highest priority message */
} ocrMsgBehaviorProp_t;

// Defines communication behavior properties to be used as the
// upper 16 bits of the u32 property flag of send/recv/poll API calls.
#define COMM_PROP_BEHAVIOR_SIZE 16
#define COMM_PROP_BEHAVIOR_OFFSET   16
// Marshalling mode (ocrMarshallMode_t) is the first 8 bits of the behavior prop
#define COMM_BEHAVIOR_MARSHALL_MASK 0x00FF0000
#define COMM_PROP_MARSHALL_OFFSET   COMM_PROP_BEHAVIOR_OFFSET

// Extracts marshalling information from a u32 communication property
#define GET_PROP_U8_MARSHALL(commProperty) ((u8) ((commProperty & COMM_BEHAVIOR_MARSHALL_MASK) >> COMM_PROP_MARSHALL_OFFSET))

// MT Comm constants
#define COMM_ONE_WAY 0x1      /**< Message is only being sent with no response expected */
#define COMM_STACK_MSG 0x2    /**< Message is stack allocated */


/**
 * @brief Status of messages at the comm-platform level
 *
 * @note Not all implementations support all states. The mandatory
 * supported ones are marked. Others are present mostly
 * for optimization reasons
 */
typedef enum {
    MSG_NORMAL   = 0x00100, /**< (MANDATORY) The message is normal (no further details) */
    MSG_ERR      = 0x10100, /**< (MANDATORY) An error occured sending the message */
    MSG_ACCEPTED = 0x00200, /**< The message has been accepted by the comm-platform */
    MSG_SEND_ERR = 0x10400, /**< THe message had an error being sent */
    MSG_SEND_OK  = 0x00400, /**< The message has been sent successfully */
    MSG_RECV_OK  = 0x00800, /**< The message has been received by the target successfully */
    MSG_RECV_ERR = 0x10800, /**< The message had an error being received by the target */
} ocrMsgStatus_t;

/**
 * @brief Status of the message handle
 *
 * @note Not all implementations support all states. The
 * mandatory supported ones are marked. Others are present
 * mostly for optimization reasons
 */
typedef enum {
    HDL_NORMAL        = 0x00100, /**< (MANDATORY) No error conditions */
    HDL_ERR           = 0x10100, /**< (MANDATORY) Unspecified error */
    HDL_SEND_ERR      = 0x10200, /**< Outgoing message could not be sent */
    HDL_SEND_OK       = 0x00200, /**< Outgoing message was sent properly */
    HDL_SEND_RECV_OK  = 0x00201, /**< Outgoing message was acknowledged as received
                              * by the target */
    HDL_SEND_RECV_ERR = 0x10201, /**< Outgoing message was sent but an error
                              * occured on the receiving end */
    HDL_RECV_ERR      = 0x10400, /**< An error occured on the incoming response */
    HDL_RESPONSE_OK   = 0x00400  /**< (MANDATORY) Handle has a ready response */

} ocrMsgHandleStatus_t;

/**
 * @brief Types of data-blocks allocated by the runtime
 *
 * OCR provides support for "runtime" data-blocks which
 * may have more restrictive rules than regular data-blocks.
 *
 * @warn This functionality is not currently used and may be
 * stripped out but it is here in case we need it later
 */
typedef enum {
    USER_DBTYPE     = 0x1,
    RUNTIME_DBTYPE  = 0x2,
    MAX_DBTYPE      = 0x3
} ocrDataBlockType_t;

/** @brief Special property that removes the warning
 * for the acquire/create */
//This is clashing with the RT/GUID mask. No space left anywhere else.
#define DB_PROP_IGNORE_WARN ((u16)(0x4000))

/**
 * @brief Type of memory allocated/unallocated
 * by MEM_ALLOC and MEM_UNALLOC
 *
 * The PD_MSG_MEM_ALLOC and PD_MSG_MEM_UNALLOC
 * messages allocate and de-allocate chunks of
 * memory. This indicates what these chunks of
 * memories are used for
 */
typedef enum {
    DB_MEMTYPE      = 0x1, /**< Memory used for a data-block */
    GUID_MEMTYPE    = 0x2, /**< Memory used for the metadata of an object */
    MAX_MEMTYPE     = 0x3
} ocrMemType_t;

/**
 * @brief Types of work "scheduled" by the runtime
 *
 * For now this is EDT but other type of computation
 * may be used (runtime EDTs, communication tasks, etc)
 *
 * @warn This functionality is not currently used and may be
 * stripped out but it is here in case we need it later
 */
typedef enum {
    EDT_USER_WORKTYPE    = 0x1,
    EDT_RT_WORKTYPE = 0x2,
    MAX_WORKTYPE    = 0x3
} ocrWorkType_t;

typedef enum {
    CREATED_EDTSTATE    = 0x1, /**< EDT created */
    ALLDEPS_EDTSTATE    = 0x2, /**< EDT has all dependences added */
    PARTIAL_EDTSTATE    = 0x3, /**< EDT has at least one dependence that is satisfied */
    ALLSAT_EDTSTATE     = 0x4, /**< EDT has all dependences satisfied */
    ALLACQ_EDTSTATE     = 0x5, /**< EDT has DB dependences acquired */
    RUNNING_EDTSTATE    = 0x6, /**< EDT is executing */
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
    RESCHED_EDTSTATE    = 0x7, /**< EDT needs to be rescheduled */
#endif
    REAPING_EDTSTATE    = 0x8  /**< EDT finished executing and is cleaning up */
} ocrEdtState_t;


/**
 * @brief Identifier to represent 'none' of an EDT slots.
 */
#define EDT_SLOT_NONE ((u32)-1)

/**
 * @brief Type of pop from workpiles.
 *
 * There are multiple ways to steal from a workpile
 * (traditionally the usual pop and also the steal).
 * This enum allows us to expand this in the future
 */
typedef enum {
    POP_WORKPOPTYPE     = 0x1,
    STEAL_WORKPOPTYPE   = 0x2,
    MAX_WORKPOPTYPE     = 0x3
} ocrWorkPopType_t;

/**
 * @brief Type of push to workpiles
 *
 * @see ocrWorkPopType_t
 */
typedef enum {
    PUSH_WORKPUSHTYPE       = 0x1,
    PUSH_WORKPUSHBACKTYPE   = 0x2,
    MAX_WORKPUSHTYPE        = 0x3
} ocrWorkPushType_t;

/**
 * @brief Type of worker
 *
 * This is used in workers as well as comp-* to determine what types
 * of workers the specific instance of the platform supports.
 * The distinction between SINGLE and MASTER is mostly to distinguish
 * between machines that require a single point of entry/exit (such as
 * a process with multiple threads that must fall back to only one thread
 * to properly exit) and others that can have multiple "main" entry points
 * (MPI nodes for example).
 */
typedef enum {
    SINGLE_WORKERTYPE   = 0x1, /**< Single worker (starts/stops by itself) */
    MASTER_WORKERTYPE   = 0x2, /**< Master worker (responsible for starting/stopping others) */
    SLAVE_WORKERTYPE    = 0x3, /**< Slave worker (started/stopped by a master worker) */
    SYSTEM_WORKERTYPE   = 0x4, /**< System worker (started/stopped by a master worker) */
    MAX_WORKERTYPE      = 0x5
} ocrWorkerType_t;

typedef struct {
    ocrGuid_t guid;
    void* metaDataPtr;
} ocrFatGuid_t;

typedef enum {
    KIND_GUIDPROP       = 0x1, /**< Request kind of the GUID */
    LOCATION_GUIDPROP   = 0x2, /**< Request location of the GUID */
    WMETA_GUIDPROP      = 0x4, /**< Request the metadata of the GUID in write mode
                                * Also used when returning the GUID to indicate if
                                * the returned meta data can be written to */
    RMETA_GUIDPROP      = 0x8, /**< Request the metadata of the GUID in read mode */
    CMETA_GUIDPROP      = 0x10, /**< Indicates that the metadata returned is a copy (R/O
                                * and should be freed with pdFree) */
} ocrGuidInfoProp_t;

typedef enum { // Coded on 8 bits maximum
    MONITOR_PROGRESS_COMM  = 0x1, /**< Monitor a communication completion */
    MONITOR_PROGRESS_EVENT = 0x2, /**< Monitor an event completion */
    MAX_MONITOR_PROGRESS   = 0x3
} ocrMonitorProgress_t;

/**
 * @brief Returned by the pollMessage function in
 * either the comp-target and comp-platform to indicate
 * that no message was available */
#define POLL_NO_MESSAGE            0x1 // first bit indicates no message
#define POLL_NO_OUTGOING_MESSAGE   0x3 // no outgoing in queue
#define POLL_NO_INCOMING_MESSAGE   0x5 // no incoming in queue
/**
 * @brief Indicates that a message was returned and available
 * and that more messages are available
 */
#define POLL_MORE_MESSAGE 0x4
/**
 * @brief AND the return code of pollMessage with this
 * mask to get any real error codes
 */
#define POLL_ERR_MASK     0xF0

/**
 * @brief Some useful macros
 */
#define RESULT_PROPAGATE(expression)            \
    do {                                        \
        u8 __result = (expression);             \
        if(__result) return (__result);         \
    } while(0);

#define RESULT_PROPAGATE2(expression, returnValue)  \
    do {                                            \
        u8 __result = (expression);                 \
        if(__result) return (returnValue);          \
    } while(0);

#define RESULT_PROPAGATE_PROF(expression)       \
    do {                                        \
        u8 __result = (expression);             \
        if(__result) RETURN_PROFILE(__result);  \
    } while(0);

#define RESULT_PROPAGATE2_PROF(expression, returnValue) \
    do {                                                \
        u8 __result = (expression);                     \
        if(__result) RETURN_PROFILE(returnValue);       \
    } while(0);


/**
 * @brief Static assertion check macro
 */
#define ocrStaticAssert(e) extern char (*ocrStaticAssert(void))[sizeof(char[1-2*!(e)])]

#endif /* __OCR_RUNTIME_TYPES_H__ */

