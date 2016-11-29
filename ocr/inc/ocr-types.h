/**
 * @brief Basic types used throughout the OCR library
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_TYPES_H__
#define __OCR_TYPES_H__

#include <stddef.h>
#include <inttypes.h>

/**
 * @defgroup OCRTypes Types and constants used in OCR
 *
 * @brief Collection of types and constants used throughout
 * the OCR API
 * @{
 *
 * @defgroup OCRTypesGeneral General types and constants
 *
 * @{
 */
typedef uint64_t u64; /**< 64-bit unsigned integer */
typedef uint32_t u32; /**< 32-bit unsigned integer */
typedef uint16_t u16; /**< 16-bit unsigned integer */
typedef uint8_t  u8;  /**< 8-bit unsigned integer */
typedef int64_t  s64; /**< 64-bit signed integer */
typedef int32_t  s32; /**< 32-bit signed integer */
typedef int8_t   s8;  /**< 8-bit signed integer */

#define NULL_HINT ((ocrHint_t *)0x0)

#ifdef OCR_ENABLE_128_BIT_GUID

#define GUID_BIT_COUNT 128
typedef struct {
    intptr_t lower;
    intptr_t upper;
} ocrGuid_t;

#else

#define GUID_BIT_COUNT 64
typedef struct {
    intptr_t guid;
} ocrGuid_t;

#endif

// See impl notes
ocrGuid_t addValueToGuid(ocrGuid_t input, u64 value);

/* Defined vals for 64-bit GUIDs */
#if GUID_BIT_COUNT == 64

#define NULL_GUID_INITIALIZER {.guid = 0x0}
#define NULL_GUID ((ocrGuid_t)NULL_GUID_INITIALIZER)

#define UNINITIALIZED_GUID_INITIALIZER {.guid = -2}
#define UNINITIALIZED_GUID ((ocrGuid_t)UNINITIALIZED_GUID_INITIALIZER)

#define ERROR_GUID_INITIALIZER {.guid = -1}
#define ERROR_GUID ((ocrGuid_t)ERROR_GUID_INITIALIZER)

/* Defined vals for 128-bit GUIDs */
#elif GUID_BIT_COUNT == 128

#define NULL_GUID_INITIALIZER {.lower = 0x0, .upper = 0x0}
#define NULL_GUID ((ocrGuid_t)NULL_GUID_INITIALIZER)

#define UNINITIALIZED_GUID_INITIALIZER {.lower = -2, .upper = -2}
#define UNINITIALIZED_GUID ((ocrGuid_t)UNINITIALIZED_GUID_INITIALIZER)

#define ERROR_GUID_INITIALIZER {.lower = -1, .upper = -1}
#define ERROR_GUID ((ocrGuid_t)ERROR_GUID_INITIALIZER)

#endif

#ifdef __MACH__
#include <stdbool.h>
#else

/* boolean support in C */
#ifndef __cplusplus
#define true 1
#define TRUE 1
#define false 0
#define FALSE 0
typedef u8 bool;
#endif /* __cplusplus */

#ifdef __cplusplus
#define TRUE true
#define FALSE false
#endif /* __cplusplus */

#endif


/**
 * @}
 *
 * @defgroup OCRTypesDB Types and constants associated with data blocks
 * @{
 */

/**
 * @brief Allocators that can be used to allocate
 * within a data block
 *
 * Data blocks can be used as heaps and allocators can be
 * defined for these heaps. This enum lists the possible
 * allocators.
 */
typedef enum {
    NO_ALLOC = 0 /**< No allocation is possible with the data block */
} ocrInDbAllocator_t;

/**
 * @brief Data block access modes
 *
 * These are the modes with which an EDT can access a data block. OCR currently
 * supports five modes:
 * - None: This is useful to convert a data-dependence into a control-dependence,
 *   when an EDT doesn't expect a datablock but it may be passed along the event.
 * - Read Only (RO): The EDT is stating that it will only read from the data block.
 *   In this mode, the runtime guarantees that the data block seen by the EDT
 *   is not modified by other concurrent EDTs (in other words, the data block
 *   does not change "under you". Any violation of the "no-write" contract
 *   by the program will result in undefined behavior
 *   (the write may or may not be visible to other EDTs depending on the
 *   implementation and specific runtime conditions).
 * - Non-coherent read (NCR): This mode is exactly the same as RO
 *   except that the runtime does not guarantee that the data block will
 *   not change.
 * - Read-write (RW) (default mode): The EDT is stating that it may
 *   or may not write to the data block. The user is responsible for
 *   synchronizing between EDTs that could potentially write to the same
 *   data block concurrently.
 * - Exclusive write (EW): The EDT requires that it be the only one accessing
 *   the data block. The runtime will not schedule any other EDT that accesses
 *   the same data block in EW or ITW mode concurrently.
 *   This can limit parallelism.
 */
typedef enum {
    DB_MODE_NULL = 0x0,   /**< No mode specified; useful for pure control dependences */
    DB_MODE_CONST= 0x1,   /**< Constant mode */
    DB_MODE_RW   = 0x2,   /**< Read-write (default mode) */
    DB_MODE_EW   = 0x4,   /**< Exclusive write mode */
    DB_MODE_RO   = 0x8    /**< Read-only mode */
} ocrDbAccessMode_t; // Warning: only 5 bits starting at bit 1 (leave bit 0 as 0)

#define DB_DEFAULT_MODE ((ocrDbAccessMode_t)DB_MODE_RW) /**< Default access mode */

#define DB_PROP_NONE       ((u16)0x0) /**< Property for a data block indicating no special behavior */
#define DB_PROP_NO_ACQUIRE ((u16)0x10) /**< Property for a data block indicating that the data-block
                                       *   is just being created but does not need to be acquired
                                       *   at the same time (creation for another EDT)
                                       */

#define DB_PROP_SINGLE_ASSIGNMENT ((u16)0x20) /**< Property for a data block indicating single-assignment
                                               *   i.e. The user guarantees the data block is written once
                                               *   at creation time.
                                               *   @note This property is experimental and not
                                               *   implemented consistently.
                                               */
#define DB_PROP_NO_HINT       ((u16)0x40) /**< Property for a data block indicating no hints can be set on the datablock */

/**
 * @}
 *
 * @defgroup OCRTypesEDT Types and constants associated with EDTs
 *
 * @{
 */

/**
 * @brief Type of values passed to an EDT on each pre-slot
 *
 * An EDT with N pre-slots will receive an array of N elements of this
 * type (its input dependences). Each dependence has the GUID of the
 * data block passed along that pre-slot as well as a pointer
 * to the data in the data block.
 *
 * @note The GUID passed to the EDT is *not* the GUID of the event
 * linked to the pre-slot of the EDT but rather the GUID of the data block
 * that was associated with that event. If no data block was associated, NULL_GUID
 * is passed.
 */
typedef struct {
    ocrGuid_t guid;         /**< GUID of the data block or NULL_GUID */
    void* ptr;              /**< Pointer allowing access to the data block or NULL */
    ocrDbAccessMode_t mode; /**< Runtime reserved (may go away in future) --
                                 The access mode with which the data block has been acquired */
} ocrEdtDep_t;


#define EDT_PROP_NONE    ((u16) 0x0) /**< Property bits indicating a regular EDT */
#define EDT_PROP_FINISH  ((u16) 0x1) /**< Property bits indicating a FINISH EDT */
#define EDT_PROP_NO_HINT ((u16) 0x2) /**< Property bits indicating the EDT does not take hints */
#define EDT_PROP_LONG    ((u16) 0x4) /**< Property bits indicating a long running EDT */
#define EDT_PROP_OEVT_VALID ((u16) 0x8) /** Property bits indicating an already initialized output event */

/**
 * @brief Constant indicating that the number of parameters or dependences
 * to an EDT or EDT template is unknown
 *
 * An EDT is created as an instance of an EDT template. The number of
 * parameters or dependences for the EDT can either be specified when
 * creating the template or when creating the EDT. This constant indicates
 * that the number of parameters or dependences is still unknown (for example,
 * when creating the template).
 *
 * When the EDT is created, the number of parameters and dependences must
 * be known (either specified in the template or the EDT). In other words, you
 * cannot specify the number of parameters or dependences to be EDT_PARAM_UNK
 * in both the creation of the template and the EDT.
 */
#define EDT_PARAM_UNK   ((u32)-1)

/**
 * @brief Constant indicating that the number of parameters or
 * dependences to an EDT is the same as the one specified in its template.
 */
#define EDT_PARAM_DEF   ((u32)-2)

/**
 * @brief Type for an EDT
 *
 * This is the function prototype for all EDTs.
 * @param[in] paramc   Number of non-data block parameters. A parameter is a 64-bit
 *                     value known at the creation time of the EDT
 * @param[in] paramv   Values of the 'paramc' parameters
 * @param[in] depc     Number of dependences. This corresponds to the number of
 *                     pre-slots for the EDT
 * @param[in] depv     GUIDs and pointers to the data blocks passed to this
 *                     EDT on its pre-slots. The GUID may be NULL_GUID if the pre-slot
 *                     was a pure control dependence.
 * @return The GUID of a data block to pass along to the pre-slot of the output
 * event optionally associated with this EDT. NULL_GUID can also be returned.
 **/
typedef ocrGuid_t (*ocrEdt_t)(u32 paramc, u64* paramv,
                              u32 depc, ocrEdtDep_t depv[]);

/**
 * @}
 *
 * @defgroup OCRTypesEvents Types and constants associated with events
 *
 * @{
 */

/**
 * @brief Types of OCR events
 *
 * Each OCR event has a type that is specified at creation.
 * The type of the event determines its behavior, specifically:
 * - its persistency after it triggers its post-slot
 * - its trigger rule
 * - its behavior when satisfied multiple times
 */
typedef enum {
    OCR_EVENT_ONCE_T = 1,/**< A ONCE event simply passes along a satisfaction on its
                          * unique pre-slot to its post-slot. Once all OCR objects
                          * linked to its post-slot have been satisfied, the ONCE event
                          * is automatically destroyed. */
    OCR_EVENT_IDEM_T = 2,/**< An IDEM event simply passes along a satisfaction on its
                          * unique pre-slot to its post-slot. The IDEM event persists
                          * until ocrEventDestroy() is explicitly called on it.
                          * It can only be satisfied once and susequent
                          * satisfactions are ignored (use case: BFS, B&B..) */
    OCR_EVENT_STICKY_T = 3,/**< A STICKY event is identical to an IDEM event except that
                          * multiple satisfactions result in an error
                          */
    OCR_EVENT_LATCH_T = 4,/**< A LATCH event has two pre-slots: a INCR and a DECR.
                          * Each slot is associated with an internal monotonically
                          * increasing counter that starts at 0. On each satisfaction
                          * of one of the pre-slots, the counter for that slot is
                          * incremented by 1. When both counters are equal (and non-zero),
                          * the post-slot of the latch event is triggered.
                          * Any data block passed along its pre-slots is ignored.
                          * A LATCH event has the same persistent as a ONCE event and
                          * is automatically destroyed when its post-slot is triggered.
                          */
#ifdef ENABLE_EXTENSION_COUNTED_EVT
    OCR_EVENT_COUNTED_T = 5,/**< A COUNTED event is a hybrid ONCE/STICKY events. It is
                          * initialized at creation time with a fixed number of expected
                          * dependences. The event can auto-destroy itself but only when
                          * both all dependences have been registered and satisfy has happened.
                          */
#endif
#ifdef ENABLE_EXTENSION_CHANNEL_EVT
    OCR_EVENT_CHANNEL_T = 6, /**< TODO
                          */
#endif
    OCR_EVENT_T_MAX      /**< This is *NOT* an event and is only used to count
                          * the number of event types. Its use is reserved for the
                          * runtime. */
} ocrEventTypes_t;


/**
 * @brief Pre-slots for events
 *
 * Currently, only the LATCH event has more than one pre-slot.
 */
typedef enum {
    OCR_EVENT_LATCH_DECR_SLOT = 0, /**< The decrement slot of a LATCH event */
    OCR_EVENT_LATCH_INCR_SLOT = 1  /**< The increment slot of a LATCH event */
} ocrLatchEventSlot_t;

#define EVT_PROP_NONE      ((u16) 0x0) /**< Property bits indicating a regular event */
#define EVT_PROP_TAKES_ARG ((u16) 0x1) /**< Property bits indicating that the event takes an argument */

/**
 * @}
 *
 * @defgroup OCR Hint types, properties and structure
 *
 * @{
 */

/**
 * ocrHintType_t: is the type of the hint object.
 * It typically denotes the kind of target guid.
 */
typedef enum {
    OCR_HINT_UNDEF_T,     // Undefined hint type
    OCR_HINT_EDT_T,       // Hints for Event Driven Tasks. Can be set to both EDT and EDT template guids.
    OCR_HINT_DB_T,        // Hints for DataBlock guids
    OCR_HINT_EVT_T,       // Hints for Event guids
    OCR_HINT_GROUP_T,     // Hints for Group guids
} ocrHintType_t;

/**
 * ocrHintProp_t: denotes the property kinds that can be set on the hint object.
 * If new hint a property is to be added, then it needs to go into this enum.
 *
 * Note: New hints should be added within the respective type's
 *       START and END fields in the following enum.
 */
typedef enum {
    //EDT Hint Properties                   (OCR_HINT_EDT_T)
    OCR_HINT_EDT_PROP_START,                /* This is NOT a hint. Its use is reserved for the runtime */
    OCR_HINT_EDT_PRIORITY,                  /* [u64] : Global priority number of EDT. Higher value is greater priority. */
    OCR_HINT_EDT_SLOT_MAX_ACCESS,           /* [u64] : EDT slot number that contains the DB which is accessed most by the EDT. */
    OCR_HINT_EDT_AFFINITY,                  /* [u64] : Affinitizes an EDT to a guid */
    OCR_HINT_EDT_DISPERSE,                  /* [xxx] : Tells scheduler to schedule EDT away from current location */
    OCR_HINT_EDT_SPACE,                     /* [u64] : Used internally by the runtime for spatial locality of EDTs */
    OCR_HINT_EDT_TIME,                      /* [u64] : Used internally by the runtime for temporal locality of EDTs */
    OCR_HINT_EDT_STATS_HW_CYCLES,           /* [u64] : Inform the simulator runtime of the EDT's perf stats */
    OCR_HINT_EDT_STATS_L1_HITS,             /* [u64] : Inform the simulator runtime of the EDT's perf stats */
    OCR_HINT_EDT_STATS_L1_MISSES,           /* [u64] : Inform the simulator runtime of the EDT's perf stats */
    OCR_HINT_EDT_STATS_FLOAT_OPS,           /* [u64] : Inform the simulator runtime of the EDT's perf stats */
    OCR_HINT_EDT_PROP_END,                  /* This is NOT a hint. Its use is reserved for the runtime */

    //DB Hint Properties                    (OCR_HINT_DB_T)
    OCR_HINT_DB_PROP_START,                 /* This is NOT a hint. Its use is reserved for the runtime */
    OCR_HINT_DB_AFFINITY,                   /* [u64] : DB affinity to a mem level */
    OCR_HINT_DB_NEAR,                       /* [u64] : Prefer near memory if possible */
    OCR_HINT_DB_INTER,                      /* [u64] : Prefer intermediate memory if possible */
    OCR_HINT_DB_FAR,                        /* [u64] : Prefer far memory if possible */
    OCR_HINT_DB_HIGHBW,                     /* [u64] : Prefer high bandwidth memory if possible */
    OCR_HINT_DB_PROP_END,                   /* This is NOT a hint. Its use is reserved for the runtime */

    //EVT Hint Properties                   (OCR_HINT_EVT_T)
    OCR_HINT_EVT_PROP_START,                /* This is NOT a hint. Its use is reserved for the runtime */
    OCR_HINT_EVT_PROP_END,                  /* This is NOT a hint. Its use is reserved for the runtime */

    //GROUP Hint Properties                 (OCR_HINT_GROUP_T)
    OCR_HINT_GROUP_PROP_START,              /* This is NOT a hint. Its use is reserved for the runtime */
    OCR_HINT_GROUP_PROP_END,                /* This is NOT a hint. Its use is reserved for the runtime */

} ocrHintProp_t;

typedef uint64_t ocrHintVal_t;

/**
 * @brief OCR Hint structure
 *
 * Note: This struct requires no modification while adding
 *       new hint properties or removing old ones.
 *
 */
typedef struct {
    ocrHintType_t type; /* The type of hint variable; depends on target kind */
    u64 propMask;       /* The bit mask that reflects all the set properties */
    union {
        u64 propEDT[OCR_HINT_EDT_PROP_END - OCR_HINT_EDT_PROP_START - 1];
        u64 propDB[OCR_HINT_DB_PROP_END - OCR_HINT_DB_PROP_START - 1];
        u64 propEVT[OCR_HINT_EVT_PROP_END - OCR_HINT_EVT_PROP_START - 1];
        u64 propGROUP[OCR_HINT_GROUP_PROP_END - OCR_HINT_GROUP_PROP_START - 1];
    } args;
} ocrHint_t;

/**
 * @brief Pre-defined OCR hint property values
 *
 */

//EDT disperse hints
#define OCR_HINT_EDT_DISPERSE_FAR   0
#define OCR_HINT_EDT_DISPERSE_NEAR  1

/**
 * @brief OCR query types
 *
 * Note: User facing options for queryable sections
 *       of paused runtime.
 */
typedef enum {
    OCR_QUERY_READY_EDTS,
    OCR_QUERY_EVENTS,
    OCR_QUERY_LAST_SATISFIED_DB,
    OCR_QUERY_ALL_EDTS,
} ocrQueryType_t;


/**
 * @brief OCR Trace Object types
 *
 * Note: Types of OCR objects used for tracing
 *       Values selected to avoid collision with
 *       Variable arg values.
 */
typedef enum {
    OCR_TRACE_TYPE_EDT = 1000,
    OCR_TRACE_TYPE_API_EDT = 1001,
    OCR_TRACE_TYPE_EVENT = 1002,
    OCR_TRACE_TYPE_API_EVENT = 1003,
    OCR_TRACE_TYPE_DATABLOCK = 1004,
    OCR_TRACE_TYPE_API_DATABLOCK = 1005,
    OCR_TRACE_TYPE_MESSAGE = 1006,
    OCR_TRACE_TYPE_WORKER = 1007,
    OCR_TRACE_TYPE_SCHEDULER = 1008,
    OCR_TRACE_TYPE_API_AFFINITY = 1009,
    OCR_TRACE_TYPE_API_HINT = 1010,
    OCR_TRACE_TYPE_MAX = 1012
} ocrTraceType_t;


/**
 * @brief OCR Trace Object actions
 *
 * Note: Types of actions performed by OCR objects
 *       for tracing
 */
typedef enum {
    OCR_ACTION_CREATE,
    OCR_ACTION_TEMPLATE_CREATE,
    OCR_ACTION_DESTROY,
    OCR_ACTION_RUNNABLE,
    OCR_ACTION_SCHEDULED,
    OCR_ACTION_ADD_DEP,
    OCR_ACTION_SATISFY,
    OCR_ACTION_EXECUTE,
    OCR_ACTION_FINISH,
    OCR_ACTION_DATA_ACQUIRE,
    OCR_ACTION_DATA_RELEASE,
    OCR_ACTION_END_TO_END,
    OCR_ACTION_WORK_REQUEST,
    OCR_ACTION_WORK_TAKEN,
    OCR_ACTION_SCHED_MSG_SEND,
    OCR_ACTION_SCHED_MSG_RCV,
    OCR_ACTION_SCHED_INVOKE,
    OCR_ACTION_GET_CURRENT,
    OCR_ACTION_GET_AT,
    OCR_ACTION_GET_COUNT,
    OCR_ACTION_QUERY,
    OCR_ACTION_INIT,
    OCR_ACTION_SET_VAL,
    OCR_ACTION_RANGE_CREATE,
    OCR_ACTION_MAX
} ocrTraceAction_t;

/**
 * @}
 */
/**
 * @defgroup OCRTypesLabels Constants for GUID labels
 *
 * @{
 */
#define GUID_PROP_NONE       ((u16)(0x0))   /**< No specific property */
#define GUID_PROP_IS_LABELED ((u16)(0x100)) /**< Property to indicate the GUID passed as
                                             * argument to the create call is a labeled GUID and
                                             * should not be ignored (the default behavior is
                                             * to ignore the field) */
#define GUID_PROP_CHECK ((u16)(0x300))      /**< Property to indicate that, on creation, the
                                             * runtime should check if the GUID exists already,
                                             * and, if so, return an error code. The default behavior
                                             * is to not check (and therefore potentially create
                                             * the same object multiple times leading to undefined
                                             * behavior). Note that checking will incur
                                             * additional overhead. This implies
                                             * GUID_PROP_IS_LABELED. */
#define GUID_PROP_BLOCK ((u16)(0x500))      /**< Property to indicate that, on creation, the
                                             * runtime should check if the GUID exists already, and
                                             * if so, block until it can be re-created. See
                                             * comments for #GUID_PROP_CHECK
                                             */

                                                  //TODO: this sounds wrong but we need to double check with apps
#define LEGACY_PROP_NONE            ((u16)(0x0)) /**< For ocrLegacyBlockProgress, check if the event has been
                                                  * created. If it is local and has not been created return.
                                                  * If it is remote ahd has not been created wait for creation.
                                                  * In either case wait for it to be satisfied. Returns
                                                  * immediately with OCR_EINVAL if event not created */
#define LEGACY_PROP_WAIT_FOR_CREATE ((u16)(0x1)) /**< For ocrLegacyBlockProgress, wait for the handle to
                                                  * be created */
#define LEGACY_PROP_WAIT_IF_CREATED ((u16)(0x2))  /**< For ocrLegacyBlockProgress, check if the event has been
                                                  * created. If yes behaves as LEGACY_PROP_NONE, it not returns
                                                  * immediately with OCR_EINVAL */
#define LEGACY_PROP_CHECK           ((u16)(0x4))  /**< For ocrLegacyBlockProgress, check if the event has been
                                                  * created. If yes check if it has been satisfied. Returns
                                                  * immediately with OCR_EINVAL in either case */
/**
 * @}
 */

/**
 * @}
 */


#endif /* __OCR_TYPES_H__ */


