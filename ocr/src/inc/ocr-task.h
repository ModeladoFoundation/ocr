/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_TASK_H__
#define __OCR_TASK_H__

#include "ocr-edt.h"
#include "ocr-runtime-types.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-runtime-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#ifdef ENABLE_EXTENSION_PERF
#include "ocr-perfmon.h"
#endif

#ifdef OCR_ENABLE_EDT_NAMING
#ifndef OCR_EDT_NAME_SIZE
#define OCR_EDT_NAME_SIZE 32
#endif
#endif

//Runtime defined properties for EDT create (upper 16 bits)
#define EDT_PROP_RT_HINT_ALLOC  0x10000 //Hint variable is runtime allocated

struct _ocrTask_t;
struct _ocrTaskTemplate_t;

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/
typedef struct _paramListTaskFact_t {
    ocrParamList_t base;
} paramListTaskFact_t;

typedef struct _paramListTaskTemplateFact_t {
    ocrParamList_t base;
} paramListTaskTemplateFact_t;

typedef struct _paramListTask_t {
    ocrParamList_t base;
    ocrWorkType_t workType;
} paramListTask_t;


/****************************************************/
/* OCR TASK TEMPLATE                                */
/****************************************************/

/** @brief Abstract class to represent OCR task template functions
 *
 *  This class provides the interface to call operations on task
 *  templates
 */
typedef struct ocrTaskTemplateFcts_t {
    /** @brief Virtual destructor for the task template interface
     */
    u8 (*destruct)(struct _ocrTaskTemplate_t* self);

    /**
     * @brief Set user hints for the EDT template
     *
     * The EDT template implementation chooses which hint properties will be set.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this task template
     * @param[in] hint        Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*setHint)(struct _ocrTaskTemplate_t* self, ocrHint_t *hint);

    /**
     * @brief Get user hints from the EDT template
     *
     * The EDT template implementation chooses which hint properties will be gotten.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this task template
     * @param[in/out] hint    Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getHint)(struct _ocrTaskTemplate_t* self, ocrHint_t *hint);

    /**
     * @brief Get runtime hints from the EDT template
     *
     * The hints structure is an array of u64 values,
     * starting with the mask and followed by the
     * hint values.
     *
     * @param[in] self        Pointer to this task template
     * @return pointer to hint structure
     */
    ocrRuntimeHint_t* (*getRuntimeHint)(struct _ocrTaskTemplate_t* self);

#ifdef ENABLE_RESILIENCY
    /**
     * @brief Get the serialization size
     *
     * @param[in] self        Pointer to this task template
     * @param[out] size       Buffer size required to serialize task template
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getSerializationSize)(struct _ocrTaskTemplate_t* self, u64* size);

    /**
     * @brief Serialize task template into buffer
     *
     * @param[in] self        Pointer to this task template
     * @param[in/out] buffer  Buffer to serialize into
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*serialize)(struct _ocrTaskTemplate_t* self, u8* buffer);

    /**
     * @brief Deserialize task template from buffer
     *
     * @param[in] buffer      Buffer to deserialize from
     * @param[out] self       Pointer to deserialized task template
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*deserialize)(u8* buffer, struct _ocrTaskTemplate_t** self);

    /**
     * @brief Fixup task template pointers after deserialization
     *
     * @param[in] self        Pointer to this task template
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*fixup)(struct _ocrTaskTemplate_t* self);

    /**
     * @brief Deallocate task template during PD reset
     *
     * @param[in] self        Pointer to this task template
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*reset)(struct _ocrTaskTemplate_t* self);
#endif
} ocrTaskTemplateFcts_t;

/** @brief Abstract class to represent OCR task templates.
 *
 */
typedef struct _ocrTaskTemplate_t {
    ocrObject_t base;
#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcess_t *statProcess;
#endif
    u32 paramc;             /**< Number of input parameters */
    u32 depc;               /**< Number of dependences */
    // BUG #616: This does not really support things like
    // moving code around and/or different ISAs. Is this
    // going to be a problem...
    ocrEdt_t executePtr;    /**< Function pointer to execute */
#ifdef OCR_ENABLE_EDT_NAMING
    const char name[OCR_EDT_NAME_SIZE];       /**< Name of the EDT */
#endif
#ifdef ENABLE_EXTENSION_PERF
    ocrPerfCounters_t *taskPerfsEntry;   /**< Entry to the PD's taskPerfs queue */
#endif
    u32 fctId;              /**< Functions to manage this template */
} ocrTaskTemplate_t;

/****************************************************/
/* OCR TASK TEMPLATE FACTORY                        */
/****************************************************/

typedef struct _ocrTaskTemplateFactory_t {
    ocrObjectFactory_t base;
    /**
     * @brief Create a task template
     *
     * @param[in] factory     Pointer to this factory
     * @param[in] fctPtr      Function pointer to execute
     * @param[in] paramc      Number of input parameters or EDT_PARAM_UNK or EDT_PARAM_DEF
     * @param[in] depc        Number of DB dependences or EDT_PARAM_UNK or EDT_PARAM_DEF
     * @param[in] fctName     Name of the EDT (for debugging)
     * @param[in] perInstance Instance specific parameters
     */
    ocrTaskTemplate_t* (*instantiate)(struct _ocrTaskTemplateFactory_t * factory, ocrEdt_t fctPtr,
                                      u32 paramc, u32 depc, const char* fctName,
                                      ocrParamList_t *perInstance);

    u32 factoryId;
    ocrTaskTemplateFcts_t fcts;
    u64 *hintPropMap; /**< Mapping hint properties to implementation specific packed array */
} ocrTaskTemplateFactory_t;

/****************************************************/
/* OCR TASK                                         */
/****************************************************/

/**
 * @brief Abstract class to represent OCR tasks function pointers
 *
 * This class provides the interface to call operations on task
 */
typedef struct _ocrTaskFcts_t {
    /**
     * @brief Virtual destructor for the Task interface
     */
    u8 (*destruct)(struct _ocrTask_t* self);

#ifdef REG_ASYNC_SGL
    u8 (*satisfyWithMode)(struct _ocrTask_t* self, ocrFatGuid_t db, u32 slot, ocrDbAccessMode_t mode);
#endif

    /**
     * @brief "Satisfy" an input dependence for this EDT
     *
     * An EDT has input slots that must be satisfied before it
     * is becomes runable. This call satisfies the dependence
     * identified by 'slot'
     *
     * @param[in] self        Pointer to this task
     * @param[in] db          Optional data passed for this dependence
     * @param[in] slot        Slot satisfied
     */
    u8 (*satisfy)(struct _ocrTask_t* self, ocrFatGuid_t db, u32 slot);

    /**
     * @brief Informs the task that the event/db 'src' is linked to its
     * input slot 'slot'
     *
     * registerSignaler where src is a data-block is equivalent to calling
     * satisfy with that same data-block
     *
     * When a dependence is established between an event and
     * a task, registerWaiter will be called on the event
     * and registerSignaler will be called on the task
     *
     * @param[in] self        Pointer to this task
     * @param[in] signaler    GUID of the source (signaler)
     * @param[in] slot        Slot on self that will be satisfied by signaler
     * @param[in] mode        The access mode for the dependence's data
     * @param[in] isDepAdd    True if the registerSignaler is part of the initial
     *                        adding of the dependence. False if this was a
     *                        standalone signaler register.
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*registerSignaler)(struct _ocrTask_t* self, ocrFatGuid_t src, u32 slot,
                           ocrDbAccessMode_t mode, bool isDepAdd);

    /**
     * @brief Informs the task that the event/db 'src' is no longer linked
     * to it on 'slot'
     *
     * This removes a dependence between an event/db and
     * a task
     *
     * @param[in] self        Pointer to this task
     * @param[in] signaler    GUID of the source (signaler)
     * @param[in] slot        Slot on self that will be satisfied by signaler
     * @param[in] isDepRem    True if the unregisterSignaler is part of a
     *                        dependence removal. False if standalone call
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*unregisterSignaler)(struct _ocrTask_t* self, ocrFatGuid_t src, u32 slot,
                             bool isDepRem);

    /**
     * @brief Informs the task that it has acquired a DB that it didn't know
     * about upon entry
     *
     * This happens when the user creates a data-block within an EDT. This
     * call is special in the sense that it will ALWAYS be called within
     * the context/execution of self.
     *
     * @note This call is only called when the data-block acquire
     * is user-triggered from isnide the EDT
     *
     * @param[in] self          Pointer to this task
     * @param[in] db            GUID of the DB
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*notifyDbAcquire)(struct _ocrTask_t* self, ocrFatGuid_t db);

    /**
     * @brief Symmetric call to notifyDbAcquire(): this is called when
     * the user performs a release (could be as part of a free) on
     * ANY data-block while inside the EDT
     *
     * @warning This call may be called with DBs that have not been
     * acquired using notifyDbAcquire (ie: DBs that are dependences.
     * This should not be an error
     *
     * @param[in] self          Pointer to this task
     * @param[in] db            GUID of the DB
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*notifyDbRelease)(struct _ocrTask_t* self, ocrFatGuid_t db);

    /**
     * @brief Executes this EDT
     *
     * This should be called by a worker to execute the EDT's code
     * This call should take care of acquiring any input dependences
     * and notifying any output events if needed
     *
     * @param[in] self        Pointer to this task
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*execute)(struct _ocrTask_t* self);

    /**
     * @brief Sets the a pointer to a datablock dependence the task has.
     *
     * This should be called to set the task's resolved DB dependences pointers
     * after acquisition.
     *
     * @param[in] self        Pointer to this task.
     * @param[in] dbGuid      The guid of the datablock
     * @param[in] localDbPtr  Pointer to a task's dependence DB data pointer
     * @param[in] self        The slot of the dependence.
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*dependenceResolved)(struct _ocrTask_t* self, ocrGuid_t dbGuid, void* localPtr, u32 slot);

    /**
     * @brief Set user hints for the EDT
     *
     * The task implementation chooses which hint properties will be set.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this task
     * @param[in] hint        Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*setHint)(struct _ocrTask_t* self, ocrHint_t *hint);

    /**
     * @brief Get user hints from the EDT
     *
     * The task implementation chooses which hint properties will be gotten.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this task
     * @param[in/out] hint    Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getHint)(struct _ocrTask_t* self, ocrHint_t *hint);

    /**
     * @brief Get runtime hints from the EDT
     *
     * The hints structure is an array of u64 values,
     * starting with the mask and followed by the
     * hint values.
     *
     * @param[in] self        Pointer to this task
     * @return pointer to hint structure
     */
    ocrRuntimeHint_t* (*getRuntimeHint)(struct _ocrTask_t* self);

#ifdef ENABLE_RESILIENCY
    /**
     * @brief Get the serialization size
     *
     * @param[in] self        Pointer to this task
     * @param[out] size       Buffer size required to serialize task
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getSerializationSize)(struct _ocrTask_t* self, u64* size);

    /**
     * @brief Serialize task into buffer
     *
     * @param[in] self        Pointer to this task
     * @param[in/out] buffer  Buffer to serialize into
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*serialize)(struct _ocrTask_t* self, u8* buffer);

    /**
     * @brief Deserialize task from buffer
     *
     * @param[in] buffer      Buffer to deserialize from
     * @param[out] self       Pointer to deserialized task
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*deserialize)(u8* buffer, struct _ocrTask_t** self);

    /**
     * @brief Fixup task pointers after deserialization
     *
     * @param[in] self        Pointer to this task
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*fixup)(struct _ocrTask_t* self);

    /**
     * @brief Deallocate task during PD reset
     *
     * @param[in] self        Pointer to this task
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*reset)(struct _ocrTask_t* self);
#endif
} ocrTaskFcts_t;

#define ELS_RUNTIME_SIZE 0
#ifndef ELS_USER_SIZE
#define ELS_USER_SIZE 4
#endif
#define ELS_SIZE (ELS_RUNTIME_SIZE + ELS_USER_SIZE)

/*! \brief Abstract class to represent OCR tasks.
 *
 *  This class provides the interface for the underlying implementation to conform.
 *  OCR tasks can be executed and can have their synchronization frontier furthered by Events.
 */
typedef struct _ocrTask_t {
    ocrObject_t base;
    ocrGuid_t guid;         /**< GUID for this task (EDT) */
#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcess_t *statProcess;
#endif
    ocrGuid_t templateGuid; /**< GUID for the template of this task */
    ocrEdt_t funcPtr;       /**< Function to execute */
    u64* paramv;            /**< Pointer to the paramaters; should be inside task metadata */
#ifdef OCR_ENABLE_EDT_NAMING
    const char name[OCR_EDT_NAME_SIZE];       /**< Name of the EDT (for debugging purposes */
#endif
    ocrGuid_t outputEvent;  /**< Event to notify when the EDT is done */
    ocrGuid_t finishLatch;  /**< Latch event for this EDT (if this is a finish EDT) */
    ocrGuid_t parentLatch;  /**< Inner-most latch event (not of this EDT) */
    ocrGuid_t els[ELS_SIZE];/**< EDT local storage */
    ocrEdtState_t state;    /**< State of the EDT */
    u32 paramc, depc;       /**< Number of parameters and dependences */
    u32 flags;              /**< Bit flags for the task */
    u32 fctId;
#ifdef ENABLE_EXTENSION_PERF
    ocrPerfCounters_t *taskPerfsEntry;
    u32 swPerfCtrs[PERF_MAX-PERF_HW_MAX];
#endif
} ocrTask_t;

#define OCR_TASK_FLAG_USES_HINTS            0x1 /* Identifies if the task has user hints set */
#define OCR_TASK_FLAG_RUNTIME_EDT           0x2 /* Identifies if the task is a runtime EDT as opposed to a user EDT */
#define OCR_TASK_FLAG_USES_SCHEDULER_OBJECT 0x4 /* BUG #920 Cleanup */
#define OCR_TASK_FLAG_USES_AFFINITY         0x8 /* BUG #921: This should go away once affinity is folded into hints */
#ifdef ENABLE_EXTENSION_BLOCKING_SUPPORT
#define OCR_TASK_FLAG_LONG                  0x10
#endif
#ifdef ENABLE_EXTENSION_PERF
#define OCR_TASK_FLAG_PERFMON_ME            0x20 /* Identifies if perfmon needs to be carried out for this task */
#endif

/****************************************************/
/* OCR TASK FACTORY                                 */
/****************************************************/

/*! \brief Abstract factory class to create OCR tasks.
 *
 *  This class provides an interface to create Task instances with a non-static create function
 *  to allow runtime implementers to choose to have state in their derived TaskFactory classes.
 */
typedef struct _ocrTaskFactory_t {
    ocrObjectFactory_t base;
    /*! \brief Instantiates a Task and returns its corresponding GUID
     *  \param[in]  routine A user defined function that represents the computation this Task encapsulates.
     *  \param[in]  worker_id   The Worker instance creating this Task instance
     *  \return GUID of the concrete Task that is created by this call
     *
     *  The signature of the interface restricts the user computation that can be assigned to a task as follows.
     *  The user defined computation should take a vector of GUIDs and its size as their inputs, which may be
     *  the GUIDs used to satisfy the Events enlisted in the dependence list.
     *
     */
    u8  (*instantiate)(struct _ocrTaskFactory_t * factory, ocrFatGuid_t * edtGuid, ocrFatGuid_t edtTemplate,
                              u32 paramc, u64* paramv, u32 depc, u32 properties,
                              ocrHint_t *hint, ocrFatGuid_t *outputEvent,
                              ocrTask_t *curEdt, ocrFatGuid_t parentLatch,
                              ocrParamList_t *perInstance);

    ocrTaskFcts_t fcts;         /**< Function pointers created instances should use */
    u32 factoryId;              /**< Corresponds to fctId in task */
    u64 *hintPropMap;           /**< Mapping hint properties to implementation specific packed array */
} ocrTaskFactory_t;

#endif /* __OCR_TASK_H__ */
