/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_SCHEDULER_OBJECT_H_
#define __OCR_SCHEDULER_OBJECT_H_

#include "ocr-types.h"
#include "ocr-runtime-types.h"
#include "ocr-runtime-hints.h"
#include "ocr-guid-kind.h"
#include "utils/ocr-utils.h"

struct _ocrSchedulerObjectFactory_t;
struct _ocrPolicyMsg_t;

/****************************************************/
/* OCR SCHEDULER OBJECT PROPERTIES AND KINDS        */
/****************************************************/

/*!
 * The schedulerObject properties are based on specific usage
 * types. The usage types are encoded in the LS 4 bits.
 * Properties for each usage follows this way with
 * recursive bit encoding on specific types.
 */
#define OCR_SCHEDULER_OBJECT_PROP_TYPE                      0xF
#define OCR_SCHEDULER_OBJECT_PROP_INSERT                    0x1
#define OCR_SCHEDULER_OBJECT_PROP_REMOVE                    0x2
#define OCR_SCHEDULER_OBJECT_PROP_ITERATE                   0x3
#define OCR_SCHEDULER_OBJECT_PROP_COUNT                     0x4
#define OCR_SCHEDULER_OBJECT_PROP_MAPPING                   0x5

// Insert
#define SCHEDULER_OBJECT_INSERT_KIND                        0x0F1   // Determines the kind of insertion taking place (before or after or replace)
#define SCHEDULER_OBJECT_INSERT_BEFORE                      0x011   // Insert new element before the position indicated by the POSITION property
#define SCHEDULER_OBJECT_INSERT_AFTER                       0x021   // Insert new element after the position indicated by the POSITION property
#define SCHEDULER_OBJECT_INSERT_INPLACE                     0x031   // Replace(swap) the current element with the new element at the position indicated by the POSITION property
#define SCHEDULER_OBJECT_INSERT_POSITION                    0xF01   // Determines the position of insertion (POSITION property)
#define SCHEDULER_OBJECT_INSERT_POSITION_HEAD               0x101   // Insert position is head(begin) of the scheduler object
#define SCHEDULER_OBJECT_INSERT_POSITION_TAIL               0x201   // Insert position is tail(end) of the scheduler object
#define SCHEDULER_OBJECT_INSERT_POSITION_ITERATOR           0x301   // Insert position is as pointed by the iterator

// Remove
#define SCHEDULER_OBJECT_REMOVE_HEAD                        0x12    // Remove element from the head(begin) of the scheduler object
#define SCHEDULER_OBJECT_REMOVE_TAIL                        0x22    // Remove element from the tail(end) of the scheduler object
#define SCHEDULER_OBJECT_REMOVE_ITERATOR                    0x32    // Remove element from the position indicated by the iterator

// Iterate
#define SCHEDULER_OBJECT_ITERATE_CURRENT                    0x13    // Dereference the iterator at current position
                                                                    // - [out] iterator->data holds dereferenced value (refreshed on every call)
#define SCHEDULER_OBJECT_ITERATE_HEAD                       0x23    // Move iterator to the head(begin) of the scheduler object
                                                                    // - [out] iterator->data points to value at head after move
#define SCHEDULER_OBJECT_ITERATE_TAIL                       0x33    // Move iterator to the tail(end) of the scheduler object
                                                                    // - [out] iterator->data points to value at tail after move
#define SCHEDULER_OBJECT_ITERATE_NEXT                       0x43    // Move iterator to the next position in the scheduler object
                                                                    // - [out] iterator->data points to value at position after move
#define SCHEDULER_OBJECT_ITERATE_PREV                       0x53    // Move iterator to the previous position in the scheduler object
                                                                    // - [out] iterator->data points to value at position after move
#define SCHEDULER_OBJECT_ITERATE_SEARCH_KEY                 0x63    // Search for an element using a key
                                                                    // - [in/out] iterator->data points to key on input, value on output
#define SCHEDULER_OBJECT_ITERATE_SEARCH_DATA_U64            0x73    // Search for an element that matches the first 64-bits of data content
                                                                    // - [in/out] iterator->data points to u64 key on input, value on output

// Count
#define SCHEDULER_OBJECT_COUNT_IMMEDIATE                    0x004   // Count the number of elements in immediate level
#define SCHEDULER_OBJECT_COUNT_RECURSIVE                    0x014   // Count the number of elements recursively for all elements
#define SCHEDULER_OBJECT_COUNT_EDT                          0x104   // Count only EDT elements
#define SCHEDULER_OBJECT_COUNT_DB                           0x204   // Count only DB elements
#define SCHEDULER_OBJECT_COUNT_RUNTIME_EDT                  0x304   // Count only Runtime EDT elements

//Mapping
#define SCHEDULER_OBJECT_CREATE_IF_ABSENT                   0x15

typedef enum {
    OCR_SCHEDULER_OBJECT_MAPPING_POTENTIAL,    /* schedulerObjects are potentially mapped to certain locations or policy domains but not placed yet. Potential mappings can change. */
    OCR_SCHEDULER_OBJECT_MAPPING_MAPPED,       /* mappings are identified and they are currently in the process of being placed at specific locations */
    OCR_SCHEDULER_OBJECT_MAPPING_UNMAPPED,     /* scheduler object is no longer mapped to the location */
    OCR_SCHEDULER_OBJECT_MAPPING_PINNED,       /* schedulerObjects are already placed at specific locations */
    OCR_SCHEDULER_OBJECT_MAPPING_RELEASED,     /* scheduler object has been released by the resource that acquired it */
    OCR_SCHEDULER_OBJECT_MAPPING_WORKER,       /* schedulerObjects are mapped to a specific worker in current PD location */
    OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED,
} ocrSchedulerObjectMappingKind;

/*! \brief OCR schedulerObject kinds
 *   This table should be extended whenever a new kind of schedulerObject is implemented.
 *   LS   4 Bits (0-3) : Allocation type (chunk alloc/pd malloc etc)
 *   Next 4 Bits (4-7) : Scheduler object class (singleton, aggregate, specialized)
 *   Remaining Bits    : Scheduler object type
 */
typedef enum {
    OCR_SCHEDULER_OBJECT_UNDEFINED                         =0x000,

    //Scheduler object allocation:
    //    Scheduler objects can be either allocated during bringup
    //    using runtimeChunkAlloc or at runtime using pdMalloc.
    //    Depending on the allocation, the appropriate free should
    //    be called. These bits indicate which allocation was used.
    OCR_SCHEDULER_OBJECT_ALLOC_CONFIG                      =0x001,
    OCR_SCHEDULER_OBJECT_ALLOC_PD                          =0x002,

    //singleton schedulerObjects:
    //    These are the basic OCR object types handled by the scheduler.
    //    They do not have their own schedulerObject factories and hence they do not
    //    support the functions in ocrSchedulerObjectFcts_t.
    //    They are managed as part of aggregate schedulerObjects.
    OCR_SCHEDULER_OBJECT_SINGLETON                         =0x010,
    OCR_SCHEDULER_OBJECT_VOIDPTR                           =0x110,
    OCR_SCHEDULER_OBJECT_EDT                               =0x210,
    OCR_SCHEDULER_OBJECT_DB                                =0x310,
    OCR_SCHEDULER_OBJECT_RUNTIME_EDT                       =0x410,

    //aggregate schedulerObjects:
    //    These schedulerObjects can hold other schedulerObjects, both singleton and aggregate.
    //    They typically hold their elements in a uniform and structured collection.
    //    They can reused for multiple scheduler heuristics.
    //    These have associated schedulerObject factories and support ocrSchedulerObjectFcts_t.
    OCR_SCHEDULER_OBJECT_AGGREGATE                         =0x020,
    OCR_SCHEDULER_OBJECT_DEQUE                             =0x120,
    OCR_SCHEDULER_OBJECT_ARRAY                             =0x220, // BUG #613: No implementation of this yet
    OCR_SCHEDULER_OBJECT_LIST                              =0x320,
    OCR_SCHEDULER_OBJECT_MAP                               =0x420,
    OCR_SCHEDULER_OBJECT_WST                               =0x520,
    OCR_SCHEDULER_OBJECT_PR_WSH                            =0x620,
    OCR_SCHEDULER_OBJECT_BIN_HEAP                          =0x720,

    //specialized schedulerObjects:
    //    These schedulerObjects can hold other schedulerObjects, both singleton and aggregate.
    //    They typically hold aggregate scheduler objects specialized for a specific heuristic.
    //    They are usually not reused across multiple scheduler heuristics.
    //    These have associated schedulerObject factories and support ocrSchedulerObjectFcts_t.
    OCR_SCHEDULER_OBJECT_SPECIALIZED                       =0x030,
    OCR_SCHEDULER_OBJECT_PDSPACE                           =0x130,
    OCR_SCHEDULER_OBJECT_DBSPACE                           =0x330,
    OCR_SCHEDULER_OBJECT_DBTIME                            =0x430,
} ocrSchedulerObjectKind;

#define SCHEDULER_OBJECT_TYPE(kind)                (kind & ~0xF)
#define SCHEDULER_OBJECT_CLASS(kind)               (kind & 0xF0)
#define IS_SCHEDULER_OBJECT_TYPE_SINGLETON(kind)   ((kind & 0xF0) == OCR_SCHEDULER_OBJECT_SINGLETON)
#define IS_SCHEDULER_OBJECT_TYPE_AGGREGATE(kind)   ((kind & 0xF0) == OCR_SCHEDULER_OBJECT_AGGREGATE)
#define IS_SCHEDULER_OBJECT_TYPE_SPECIALIZED(kind) ((kind & 0xF0) == OCR_SCHEDULER_OBJECT_SPECIALIZED)
#define IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(kind) ((kind & 0xF) == OCR_SCHEDULER_OBJECT_ALLOC_CONFIG)
#define IS_SCHEDULER_OBJECT_PD_ALLOCATED(kind)     ((kind & 0xF)  == OCR_SCHEDULER_OBJECT_ALLOC_PD)
#define IS_SCHEDULER_OBJECT_TYPE_RUNTIME(kind)     ((kind == OCR_SCHEDULER_OBJECT_RUNTIME_EDT) /*|| (kind == ...)*/)

/****************************************************/
/* OCR SCHEDULER OBJECT                             */
/****************************************************/

/*! \brief OCR schedulerObject data structures.
 */
typedef struct _ocrSchedulerObject_t {
    ocrFatGuid_t guid;                                      /**< GUID for this schedulerObject
                                                                 The metadata ptr points to the allocation of the scheduler object.
                                                                 For singleton schedulerObjects this field is used to carry the
                                                                 element guid and its metadata ptr */
    ocrSchedulerObjectKind kind;                            /**< Kind of schedulerObject */
    u32 fctId;                                              /**< ID determining factory; Not used for singleton schedulerObjects. */
    ocrLocation_t loc;                                      /**< Current location mapping for this schedulerObject.
                                                                 The mapping can be updated by the scheduler
                                                                 during the lifetime of the schedulerObject. */
    ocrSchedulerObjectMappingKind mapping;                  /**< Mapping kind */
} ocrSchedulerObject_t;

/****************************************************/
/* OCR SCHEDULER OBJECT ITERATOR                    */
/****************************************************/

/*! \brief OCR schedulerObject Iterator data structure
 */
typedef struct _ocrSchedulerObjectIterator_t {
    ocrSchedulerObject_t *schedObj;                         /* The scheduler object to iterate.
                                                               This can be updated to reuse the iterator for another scheduler object.
                                                               If this pointer is updated, the next call to "iterate" will first reset
                                                               the iterator to the initial state before performing the iterate operation. */
    void *data;                                             /* In/Out data assocciated with each iterate call.
                                                               Check the SCHEDULER_OBJECT_ITERATE_* properties above. */
    u32 fctId;                                              /* Factory Id for the iterator. It should match the scheduler object factory Id. */
} ocrSchedulerObjectIterator_t;

/****************************************************/
/* OCR SCHEDULER OBJECT ACTIONS                     */
/****************************************************/

typedef enum {
    OCR_SCHEDULER_OBJECT_ACTION_INSERT,
    OCR_SCHEDULER_OBJECT_ACTION_REMOVE,
} ocrSchedulerObjectActionKind;

/**
 * When a scheduler invokes a scheduler heuristic during any operation,
 * the scheduler heuristic returns a set of actions to the scheduler
 * to be performed on the schedulerObjects. The scheduler heuristic does not
 * modify the schedulerObjects but leaves it to the scheduler to do it.
 */
typedef struct _ocrSchedulerObjectAction_t {
    ocrSchedulerObjectActionKind actionKind;
    u32 properties;
    void *tracker;                                          /* used to track traversal progress */
    union {
        struct {
            ocrSchedulerObject_t *schedulerObject;          /* schedulerObject to insert into */
            ocrSchedulerObject_t *el;                       /* element to insert */
            u32 properties;                                 /* insert properties */
        } insert;
        struct {
            ocrSchedulerObject_t *schedulerObject;          /* schedulerObject to remove from */
            ocrSchedulerObjectKind schedulerObjectKind;     /* kind of element to remove */
            u32 count;                                      /* number of elements to remove */
            ocrSchedulerObject_t *dst;                      /* schedulerObject to hold the removed elements */
            ocrSchedulerObject_t *el;                       /* (optional) specific elements to remove */
            u32 properties;                                 /* remove properties */
        } remove;
    } args;
} ocrSchedulerObjectAction_t;

typedef struct _ocrSchedulerObjectActionSet_t {
    u32 actionCount;                                        /* Number of actions */
    ocrSchedulerObjectAction_t *actions;                    /* Array of actions */
} ocrSchedulerObjectActionSet_t;

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/

typedef struct _paramListSchedulerObjectFact_t {
    ocrParamList_t base;
} paramListSchedulerObjectFact_t;

typedef struct _paramListSchedulerObject_t {
    ocrParamList_t base;
    bool config;                                            /* Indicates if scheduler object is instantiated during runtime bringup through the config file */
    bool guidRequired;                                      /* If object will stay local in the PD, then guid is not required */
} paramListSchedulerObject_t;

/****************************************************/
/* OCR SCHEDULER OBJECT FUNCTIONS                   */
/****************************************************/

typedef struct _ocrSchedulerObjectFcts_t {

    /** @brief Create a new schedulerObject at runtime
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] params       Params for the create
     *
     *  @returns the pointer to the instantiated schedulerObject
     */
    ocrSchedulerObject_t* (*create)(struct _ocrSchedulerObjectFactory_t *fact, ocrParamList_t *params);

    /** @brief Dynamic destruction of schedulerObjects
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Pointer to the schedulerObject to be destroyed
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*destroy)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self);

    /** @brief Insert an element into this(self) schedulerObject
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Pointer to this schedulerObject
     *  @param[in] element      SchedulerObject to insert
     *  @param[in] iterator     Position to insert into (depends on properties)
     *  @param[in] properties   Properties of the operation
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*insert)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties);

    /** @brief Removes an element from this(self) schedulerObject
     *
     *  This will remove an element from this schedulerObject set.
     *
     *  @param[in] fact          Pointer to this schedulerObject factory
     *  @param[in] self          Pointer to this schedulerObject (schedulerObject to remove from)
     *  @param[in] kind          Kind of schedulerObject to remove
     *  @param[in] count         Number of elements to remove
     *  @param[in] dst           SchedulerObject that will hold removed elements
     *  @param[in] iterator      Position to remove from (depends on properties)
     *  @param[in] properties    Properties of the operation
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*remove)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties);

    /** @brief Get the number of elements
     *
     *  This will return the number of elements in the schedulerObject
     *  The implementation may expose this as a function that
     *  simply responds as empty/non-empty.
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Pointer to this schedulerObject
     *  @param[in] properties   Properties for the count.
     *
     *  Counting Process Properties:
     *  if properties is 0 or SCHEDULER_OBJECT_COUNT_IMMEDIATE,
     *      then this function returns the immediate level element count.
     *  if SCHEDULER_OBJECT_COUNT_RECURSIVE is defined,
     *      then, the total recursive count from all schedulerObject elements is returned.
     *      i.e. count elements in elements that are schedulerObjects
     *
     *  Counting Element Kind Properties:
     *  if SCHEDULER_OBJECT_COUNT_EDT is defined,
     *      then, specifically count only EDTs
     *  if SCHEDULER_OBJECT_COUNT_DB is defined,
     *      then, specifically count only DBs
     *
     *  Properties can be created by OR-ing the process properties and the element kind properties.
     *
     *  @return count, the number of elements in this schedulerObject
     */
    u64 (*count)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties);

    //Iterator specific API

    /** @brief Create a scheduler object iterator
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Scheduler object to iterate over
     *                          (If NULL, then ensure iterator->schedObj is set before calling "iterate")
     *  @param[in] properties   Properties of the operation
     *
     *  @return iterator object on success and a NULL value on failure
     */
    ocrSchedulerObjectIterator_t* (*createIterator)(struct _ocrSchedulerObjectFactory_t * factory, ocrSchedulerObject_t *self, u32 properties);

    /** @brief Destroy a scheduler object iterator
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] iterator     Iterator object to destroy
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*destroyIterator)(struct _ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator);

    /** @brief Iterate over the scheduler object
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Scheduler object to iterate over
     *  @param[in] iterator     Iterator object (iterator->schedObj cannot be NULL)
     *  @param[in] properties   Properties of the operation
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*iterate)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties);

    //Location specific API

    /** @brief Get the schedulerObjects mapped to a specific location
     *
     *  Based on kind, these may be schedulerObjects that are "mapped"
     *  to potentially execute or be pinned to a specific location.
     *  The scheduler may update the location of a mapped schedulerObject
     *  multiple times during the lifetime of the schedulerObject but the
     *  location of a pinned schedulerObject remains constant until a
     *  "done" call is made to the scheduler for that schedulerObject.
     *
     *  If multiple schedulerObjects are found to be present, then a new
     *  schedulerObject is created and returned such that it encapsulates
     *  all those schedulerObjects.
     *
     *  If no schedulerObject is found then by default NULL is returned.
     *  However, if the properties flag is SCHEDULER_OBJECT_CREATE_IF_ABSENT,
     *  then an empty schedulerObject is created and returned.
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Pointer to this schedulerObject
     *  @param[in] kind         Kind of schedulerObject to get
     *  @param[in] loc          Location to query with
     *  @param[in] mapping      Mapping kind
     *  @param[in] properties   Properties of the operation
     *                          Can be 0 or SCHEDULER_OBJECT_CREATE_IF_ABSENT
     *
     *  @return pointer to schedulerObject on success and NULL on failure
     */
    ocrSchedulerObject_t* (*getSchedulerObjectForLocation)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties);

    /** @brief Set the location this schedulerObject is mapped to
     *
     *  @param[in] self         Pointer to this schedulerObject
     *  @param[in] loc          Location mapping
     *  @param[in] mapping      Mapping kind
     *  @param[in] factories    All the schedulerObject factories
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*setLocationForSchedulerObject)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping);

    //Action set specific API

    /** @brief Create an action set for the scheduler object
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] self         Scheduler object for the action set
     *  @param[in] properties   Properties of the operation
     *
     *  @return iterator object on success and a NULL value on failure
     */
    ocrSchedulerObjectActionSet_t* (*createActionSet)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count);

    /** @brief Destroy a scheduler object iterator
     *
     *  @param[in] fact         Pointer to this schedulerObject factory
     *  @param[in] actionSet    Action set to destroy
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*destroyActionSet)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet);

    //Other API

    /**
     * @brief Switch runlevel
     *
     * @param[in] self         Pointer to this object
     * @param[in] PD           Policy domain this object belongs to
     * @param[in] runlevel     Runlevel to switch to
     * @param[in] phase        Phase for this runlevel
     * @param[in] properties   Properties (see ocr-runtime-types.h)
     * @param[in] callback     Callback to call when the runlevel switch
     *                         is complete. NULL if no callback is required
     * @param[in] val          Value to pass to the callback
     *
     * @return 0 if the switch command was successful and a non-zero error
     * code otherwise. Note that the return value does not indicate that the
     * runlevel switch occured (the callback will be called when it does) but only
     * that the call to switch runlevel was well formed and will be processed
     * at some point
     */
    u8 (*switchRunlevel)(ocrSchedulerObject_t *self, struct _ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(struct _ocrPolicyDomain_t*, u64), u64 val);

    /** @brief Get marshall size of the scheduler object for policy message
     *
     *  Scheduler objects need to be marshalled into the policy message
     *  during scheduler transact and analyze operations across PDs.
     *
     *  @param[in] fact             Pointer to this schedulerObject factory
     *  @param[in] msg              Policy msg for marshalling
     *  @param[out] marshalledSize  Size of buffer required to marshall scheduler object
     *  @param[in] properties       Properties of the operation
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*ocrPolicyMsgGetMsgSize)(struct _ocrSchedulerObjectFactory_t *fact, struct _ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties);

    /** @brief Marshall scheduler object into policy message
     *
     *  @param[in] fact             Pointer to this schedulerObject factory
     *  @param[in] msg              Policy msg for marshalling
     *  @param[in] buffer           Buffer to marshall into
     *  @param[in] properties       Properties of the operation
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*ocrPolicyMsgMarshallMsg)(struct _ocrSchedulerObjectFactory_t *fact, struct _ocrPolicyMsg_t *msg, u8 *buffer, u32 properties);

    /** @brief Unmarshall scheduler object into policy message
     *
     *  @param[in] fact             Pointer to this schedulerObject factory
     *  @param[in] msg              Policy msg for marshalling
     *  @param[in] buffer           Buffer to marshall into
     *  @param[in] properties       Properties of the operation
     *
     *  @return 0 on success and a non-zero value on failure
     */
    u8 (*ocrPolicyMsgUnMarshallMsg)(struct _ocrSchedulerObjectFactory_t *fact, struct _ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties);

#ifdef ENABLE_RESILIENCY
    /**
     * @brief Reset scheduler object state
     *
     * Assumes all threads are quiesced except calling thread
     *
     * @param self[in]          Pointer to this scheduler object
     * @param properties[in]    Properties of the reset
     *
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*reset)(struct _ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties);
#endif

} ocrSchedulerObjectFcts_t;

/****************************************************/
/* OCR SCHEDULER OBJECT FACTORY                     */
/****************************************************/

/*! \brief Abstract factory class to create OCR schedulerObjects.
 *
 *  This class provides an interface to create SchedulerObject instances with a non-static create function
 *  to allow runtime implementers to choose to have state in their derived SchedulerObjectFactory classes.
 */
typedef struct _ocrSchedulerObjectFactory_t {
    u32 factoryId;                      /**< Corresponds to fctId in SchedulerObject */
    ocrSchedulerObjectKind kind;        /**< Kind of schedulerObject factory */
    struct _ocrPolicyDomain_t *pd;      /**< Policy domain of the schedulerObject factories */

    /*! \brief Destructor for the SchedulerObjectFactory interface
     */
    void (*destruct)(struct _ocrSchedulerObjectFactory_t * factory);

    /*! \brief Instantiates a SchedulerObject from the factory and returns its corresponding pointer
     *         If it does not recognize the kind, then it returns NULL.
     */
    ocrSchedulerObject_t* (*instantiate)(struct _ocrSchedulerObjectFactory_t * factory, ocrParamList_t *perInstance);

    /*! \brief Common schedulerObject interface functions
     *         These interface functions are supported by all scheduler objects (non-singletons)
     */
    ocrSchedulerObjectFcts_t fcts;
} ocrSchedulerObjectFactory_t;

#endif /* __OCR_SCHEDULER_OBJECT_H_ */
