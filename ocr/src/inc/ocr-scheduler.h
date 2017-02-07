/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_SCHEDULER_H__
#define __OCR_SCHEDULER_H__

#include "ocr-runtime-types.h"
#include "ocr-runtime-hints.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-guid-kind.h"
#include "ocr-scheduler-object.h"
#include "ocr-scheduler-heuristic.h"

struct _ocrPolicyDomain_t;

#define OCR_SCHED_ARG_NAME(name) _arg_##name
#define OCR_SCHED_ARG_FIELD(type) data.OCR_SCHED_ARG_NAME(type)

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/

typedef struct _paramListSchedulerFact_t {
    ocrParamList_t base;
} paramListSchedulerFact_t;

typedef struct _paramListSchedulerInst_t {
    ocrParamList_t base;
} paramListSchedulerInst_t;


/****************************************************/
/* OCR SCHEDULER INFO                               */
/****************************************************/

typedef enum {
    OCR_SCHEDULER_OP_GET_WORK,                      /**< Operation to find task(s) for the worker to execute (worker-scheduler operation)
                                                     **  The call from the worker is blocking and this call does not return without a response.
                                                     **/

    OCR_SCHEDULER_OP_NOTIFY,                        /**< Operation to notify scheduler about a new guid or event (typically used by other runtime modules)
                                                     **  This op is used in a one-way notification message and does not expect a response.
                                                     **/

    OCR_SCHEDULER_OP_TRANSACT,                      /**< Operation where schedulers transact a scheduler object (scheduler-scheduler operation)
                                                     **  This op is used with a property flags called request, transact and done to establish a
                                                     **  handshaking protocol. Some schedulers may directly use a transact without a request when
                                                     **  they have an implicit agreement between themselves.
                                                     **/

    OCR_SCHEDULER_OP_ANALYZE,                       /** Operation where schedulers analyze state (no scheduler objects are transferred) and help setup transactions
                                                     ** Schedulers collectively participate in negotiations to decide who gets to execute what and when.
                                                     ** All transaction are typically setup once analysis is done.
                                                     **/
    NUM_SCHEDULER_OPS,
} ocrSchedulerOp_t;

typedef struct _ocrSchedulerOpArgs_t {
    ocrLocation_t location;                         /* Location of the calling context */
    u32 heuristicId;                                /* Scheduler heuristic to invoke */
} ocrSchedulerOpArgs_t;

/* Scheduler worker task related arguments */
typedef enum {
    OCR_SCHED_WORK_EDT_USER,                        /* User-created EDT */
    OCR_SCHED_WORK_COMM,                            /* Runtime created communication task */
} ocrSchedWorkKind;

typedef union _ocrSchedWorkData_t {
    struct {
        ocrFatGuid_t edt;                           /* For user EDTs, count is always 1 */
        bool discardAffinity;                       /* If true, the request is for any work, not just work affinitized for me */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_WORK_EDT_USER);
    struct {
        ocrFatGuid_t *guids;
        u32 guidCount;
    } OCR_SCHED_ARG_NAME(OCR_SCHED_WORK_COMM);
} ocrSchedWorkData_t;

typedef struct _ocrSchedulerOpWorkArgs_t {
    ocrSchedulerOpArgs_t base;
    ocrSchedWorkKind kind;                          /* Kind of worker task */
    ocrSchedWorkData_t data;                        /* Worker task related data */
} ocrSchedulerOpWorkArgs_t;

/* Scheduler notify related arguments */
typedef enum {
    //Note: PROCESS_MSG is intentionally vague for now so that there's an easy way to experiment
    //      with what it would actually make sense for the scheduler to look at.
    OCR_SCHED_NOTIFY_PRE_PROCESS_MSG,               /* Notify scheduler before a PD message will be processed */
    OCR_SCHED_NOTIFY_POST_PROCESS_MSG,              /* Notify scheduler after a PD message has been processed */
    OCR_SCHED_NOTIFY_DB_CREATE,                     /* BUG #920 Cleanup - Notify scheduler that a DB is created */
    OCR_SCHED_NOTIFY_DB_ACQUIRE,                    /* BUG #920 Cleanup - Notify scheduler that a DB is acquired by an EDT */
    OCR_SCHED_NOTIFY_DB_RELEASE,                    /* BUG #920 Cleanup - Notify scheduler that a DB is released by an EDT */
    OCR_SCHED_NOTIFY_DB_DESTROY,                    /* BUG #920 Cleanup - Notify scheduler that a DB is destroyed */
    OCR_SCHED_NOTIFY_EDT_CREATE,                    /* BUG #920 Cleanup - Notify scheduler that an EDT is created */
    OCR_SCHED_NOTIFY_EDT_ENABLED,                   /* Notify scheduler that an EDT is enabled after being created in deferred model */
    OCR_SCHED_NOTIFY_EDT_SATISFIED,                 /* Notify scheduler that an EDT is fully satisfied */
    OCR_SCHED_NOTIFY_EDT_READY,                     /* Notify scheduler that an EDT is ready to execute */
    OCR_SCHED_NOTIFY_EDT_DONE,                      /* BUG #920 Cleanup - Notify scheduler that an EDT is done executing */
    OCR_SCHED_NOTIFY_COMM_READY,                    /* Notify scheduler that a communication task is ready to execute */
} ocrSchedNotifyKind;

typedef union _ocrSchedNotifyData_t {
    struct {
        struct _ocrPolicyMsg_t * msg;
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_PRE_PROCESS_MSG);
    struct {
        struct _ocrPolicyMsg_t * msg;
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_POST_PROCESS_MSG);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this db guid */
        ocrDataBlockType_t dbType;                  /* Type of datablock being created */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_DB_CREATE);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this db guid */
        ocrFatGuid_t edtGuid;                       /* EDT which is acquiring the DB */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_DB_ACQUIRE);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this db guid */
        ocrFatGuid_t edtGuid;                       /* EDT which is releasing the DB */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_DB_RELEASE);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this db guid */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_DB_DESTROY);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this edt guid */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_EDT_CREATE);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified this edt is enabled after created in deferred model */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_EDT_ENABLED);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this edt guid */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_EDT_SATISFIED);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this edt guid */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_EDT_READY);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this edt guid */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_EDT_DONE);
    struct {
        ocrFatGuid_t guid;                          /* Scheduler is notified about this communication guid */
    } OCR_SCHED_ARG_NAME(OCR_SCHED_NOTIFY_COMM_READY);
} ocrSchedNotifyData_t;

typedef struct _ocrSchedulerOpNotifyArgs_t {
    ocrSchedulerOpArgs_t base;
    ocrSchedNotifyKind kind;                        /* Kind of notify */
    ocrSchedNotifyData_t data;                      /* Notify op related data */
    u32 properties;                                 /* Properties of this op */
} ocrSchedulerOpNotifyArgs_t;

/* Scheduler transaction related arguments */
typedef enum {
    OCR_TRANSACT_PROP_REQUEST   =0x1,
    OCR_TRANSACT_PROP_TRANSFER  =0x2,
    OCR_TRANSACT_PROP_DONE      =0x4,
} ocrSchedulerTransactProp;

typedef struct _ocrSchedulerOpTransactArgs_t {
    ocrSchedulerOpArgs_t base;
    ocrSchedulerTransactProp properties;            /* Transact properties */
    struct _ocrSchedulerObject_t schedObj;          /* The scheduler object element transacted */
} ocrSchedulerOpTransactArgs_t;

/* Scheduler analysis related arguments:
 * Analyze msgs are lightweight msgs intended for example,
 * to setup a heavyweight transfer of a scheduler object or
 * to keep distributed scheduler state consistent. During
 * analyze no scheduler object is transferred between PDs.
 */
typedef enum {
    OCR_SCHED_ANALYZE_CREATE,                       /* Property for creating a scheduler object in another scheduler */
    OCR_SCHED_ANALYZE_DESTROY,                      /* Property for destroying a scheduler object in another scheduler */
    OCR_SCHED_ANALYZE_REQUEST,                      /* Property for requesting data from another scheduler (get) */
    OCR_SCHED_ANALYZE_RESPONSE,                     /* Property for responding to a request from another scheduler (get response) */
    OCR_SCHED_ANALYZE_DONE,                         /* Property for notifying a scheduler that the response from a request has been processed */
    OCR_SCHED_ANALYZE_UPDATE,                       /* Property for updating the data in another scheduler (set) */
    OCR_SCHED_ANALYZE_ACK,                          /* Property to acknowledge successful update of data (positive set response) */
    OCR_SCHED_ANALYZE_NACK,                         /* Property to acknowledge unsuccessful update of data (negative set response) */
} ocrSchedulerAnalyzeProp;

typedef enum {
    OCR_SCHED_ANALYZE_SPACETIME_EDT,
    OCR_SCHED_ANALYZE_SPACETIME_DB,
} ocrSchedAnalyzeKind;

typedef union _ocrSchedAnalyzeData_t {
    union {
        struct {
            u32 depc;
            ocrEdtDep_t *depv;
        } req;
        struct {
            ocrLocation_t space;                    /* Scheduler location where task will be scheduled */
            u64 time;                               /* Scheduler time when task will be scheduled */
        } resp;
    } OCR_SCHED_ARG_NAME(OCR_SCHED_ANALYZE_SPACETIME_EDT);
    union {
        struct {
            u64 dbSize;                             /* Size of DB */
            u64 time;                               /* Time at DB location */
            u64 count;                              /* Number of EDTs accessing the DB */
        } create;
        struct {
            u64 srcTime;                            /* Time at source location from where DB is requested */
            u64 dstTime;                            /* Time at source location from where DB is requested */
        } req;
        struct {
            u64 dbSize;                             /* Size of DB */
            ocrLocation_t srcLoc;                   /* Location where DB is currently placed */
            u64 srcTime;                            /* Time at DB's current location */
            u64 dstTime;                            /* Time at DB's destination location */
        } update;
        struct {
            u64 dbSize;                             /* Size of DB */
            u64 time;                               /* Scheduler time when at DB */
            u64 edtDoneCount;                       /* Number of EDTs that have accessed this DB during this space and time */
            bool free;                              /* DB has been freed by user */
        } done;
    } OCR_SCHED_ARG_NAME(OCR_SCHED_ANALYZE_SPACETIME_DB);
} ocrSchedAnalyzeData_t;

typedef struct _ocrSchedulerOpAnalyzeArgs_t {
    ocrSchedulerOpArgs_t base;
    ocrGuid_t guid;                                 /* Guid to analyze */
    ocrSchedulerAnalyzeProp properties;             /* Analyze properties */
    ocrSchedAnalyzeKind kind;                       /* Kind of analysis (heuristic defined) */
    ocrSchedAnalyzeData_t data;                     /* Analyze op related data */
} ocrSchedulerOpAnalyzeArgs_t;

/****************************************************/
/* OCR SCHEDULER PROPERTIES                         */
/****************************************************/

#define OCR_SCHEDULER_UPDATE_PROP_NONE                  0x0
#define OCR_SCHEDULER_UPDATE_PROP_IDLE                  0x1
#define OCR_SCHEDULER_UPDATE_PROP_SHUTDOWN              0x2
#define OCR_SCHEDULER_UPDATE_PROP_RESET                 0x3

/****************************************************/
/* OCR SCHEDULER                                    */
/****************************************************/

struct _ocrScheduler_t;
struct _ocrPolicyCtx_t;
struct _ocrMsgHandle_t;

typedef struct _ocrSchedulerOpFcts_t {
    /**
     * @brief Invokes the scheduler for a specific operation
     *
     * The scheduler responds to this call in reactive manner
     * performing the specified operation.
     *
     * @param self[in]           Pointer to this scheduler
     * @param opArgs[in]         Info about specific scheduling task
     * @param hints[in]          Hints for the op
     *
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*invoke)(struct _ocrScheduler_t *self, ocrSchedulerOpArgs_t *opArgs, ocrRuntimeHint_t *hints);

} ocrSchedulerOpFcts_t;

typedef struct _ocrSchedulerFcts_t {
    void (*destruct)(struct _ocrScheduler_t *self);

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
    u8 (*switchRunlevel)(struct _ocrScheduler_t* self, struct _ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(struct _ocrPolicyDomain_t*, u64), u64 val);

    /**
     * @brief Requests EDTs from this scheduler
     *
     * This call requests EDTs from the scheduler. The EDTs are returned in the
     * EDTs array.
     *
     * @param self[in]          Pointer to this scheduler
     * @param count[in/out]     As input contains either:
     *                            - the maximum number of EDTs requested if edts[0] is NULL_GUID
     *                            - the number of EDTs in edts (requested GUIDs). This
     *                              is also the maximum number of EDTs to be returned
     *                          As output, contains the number of EDTs returned
     * @param edts[in/out]      As input contains the GUIDs of the EDTs requested or NULL_GUID.
     *                          As output, contains the EDTs given by the scheduler to the
     *                          caller. Note that the array needs to be allocated by
     *                          the caller and of sufficient size
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*takeEdt)(struct _ocrScheduler_t *self, u32 *count, ocrFatGuid_t *edts);

    /**
     * @brief Gives EDTs to this scheduler
     *
     * This call requests that the scheduler now handles the EDTs passed to it. The
     * scheduler may refuse some of the EDTs passed to it
     *
     * @param self[in]          Pointer to this scheduler
     * @param count[in/out]     As input, contains the number of EDTs passed to the scheduler
     *                          As output, contains the number of EDTs still left in the array
     * @param edts[in/out]      As input, contains the EDTs passed to the scheduler. As output,
     *                          contains the EDTs that have not been accepted by the
     *                          scheduler from 0 to count excluded.
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*giveEdt)(struct _ocrScheduler_t *self, u32 *count, ocrFatGuid_t *edts);

    /**
     * @brief Requests communication handler from this scheduler.
     *
     * This call requests communication handler from the scheduler.
     * The pointers are returned in the handlers array.
     *
     * @param self[in]          Pointer to this scheduler
     * @param count[in/out]     Number of handlers. As input, contains the maximum number
     *                          of handlers to be returned. As output contains the number
     *                          of handlers returned.
     * @param handlers[in/out]  As output, handlers given to the caller by the callee where
     *                          the fatGuid's metaDataPtr contains a pointer to a handler.
     *                          The fatGuid's guid is set to NULL_GUID.
     *                          (array needs to be allocated by the caller and of sufficient size).
     * @param properties[in]    Properties for the take
     *
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*takeComm)(struct _ocrScheduler_t *self, u32 *count, ocrFatGuid_t * handlers, u32 properties);

    /**
     * @brief Gives communication handler to this scheduler.
     *
     * @param self[in]          Pointer to this scheduler
     * @param count[in/out]     As input, contains the number of handlers passed to the scheduler
     *                          As output, contains the number of handlers still left in the array
     * @param handlers[in/out]  As input, contains handlers passed to the scheduler through the fatGuid's
     *                          metaDataPtr. The fatGuid's guid is set to NULL_GUID. As output,
     *                          contains handlers that have not been accepted by the scheduler
     *                          from 0 to count excluded.
     * @param properties[in]    Properties for the give
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*giveComm)(struct _ocrScheduler_t *self, u32 *count, ocrFatGuid_t * handlers, u32 properties);

    /**
     * @brief Ask the scheduler to monitor the progress of an operation
     * BUG #131 helper-mode:
     * @param self[in]          Pointer to this scheduler
     * @param type[in]          The type of the operation
     * @param monitoree[in]     The data-structure associated with the operation
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*monitorProgress)(struct _ocrScheduler_t *self, ocrMonitorProgress_t type, void * monitoree);

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Scheduler 1.0 //
    ////////////////////////////////////////////////////////////////////////////////////////////////////

    /**
     * @brief Proactively update the scheduler state
     *
     * This works as a proactive hook for external modules
     *
     * @param self[in]          Pointer to this scheduler
     * @param properties[in]    Properties of this update
     *
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*update)(struct _ocrScheduler_t *self, u32 properties);

    /**
     * @brief Get count of objects held in the root scheduler object
     *
     * @param self[in]          Pointer to this scheduler
     * @param properties[in]    Properties of this update
     *                          (uses scheduler object properties)
     *
     * @return 0 on success and a non-zero value on failure
     */
    u64 (*count)(struct _ocrScheduler_t *self, u32 properties);

    /**
     * @brief Array of functions for scheduler ops
     *
     * These functions are used by scheduler related
     * policy messages.
     */
    ocrSchedulerOpFcts_t op[NUM_SCHEDULER_OPS];
} ocrSchedulerFcts_t;

struct _ocrWorkpile_t;

/*! \brief Represents OCR schedulers.
 *
 *  Currently, we allow scheduler interface to have work taken from them or given to them
 */
typedef struct _ocrScheduler_t {
    ocrFatGuid_t fguid;
    struct _ocrPolicyDomain_t *pd;
    struct _ocrWorkpile_t **workpiles;
    u64 workpileCount;

    // SchedulerObject Root:
    // Top level schedulerObject that encapsulates all schedulerObjects.
    // SchedulerObject factories are maintained in the policy domain.
    struct _ocrSchedulerObject_t *rootObj;

    // Scheduler Heuristics
    struct _ocrSchedulerHeuristic_t **schedulerHeuristics;
    u32 schedulerHeuristicCount;
    u32 masterHeuristicId;

    ocrSchedulerFcts_t fcts;
} ocrScheduler_t;

/****************************************************/
/* OCR SCHEDULER FACTORY                            */
/****************************************************/

typedef struct _ocrSchedulerFactory_t {
    ocrScheduler_t* (*instantiate) (struct _ocrSchedulerFactory_t * factory,
                                    ocrParamList_t *perInstance);
    void (*initialize) (struct _ocrSchedulerFactory_t * factory, ocrScheduler_t *self, ocrParamList_t *perInstance);
    void (*destruct)(struct _ocrSchedulerFactory_t * factory);

    ocrSchedulerFcts_t schedulerFcts;
} ocrSchedulerFactory_t;

void initializeSchedulerOcr(ocrSchedulerFactory_t * factory, ocrScheduler_t * self, ocrParamList_t *perInstance);

#endif /* __OCR_SCHEDULER_H__ */
