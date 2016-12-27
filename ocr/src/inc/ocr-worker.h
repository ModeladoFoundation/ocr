/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_WORKER_H__
#define __OCR_WORKER_H__

#include "ocr-comp-target.h"
#include "ocr-runtime-types.h"
#include "ocr-scheduler.h"
#include "ocr-types.h"
#include "ocr-hal.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

struct _ocrPolicyDomain_t;
struct _ocrPolicyMsg_t;
struct _ocrMsgHandle_t;

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/

typedef struct _paramListWorkerFact_t {
    ocrParamList_t base;
} paramListWorkerFact_t;

typedef struct _paramListWorkerInst_t {
    ocrParamList_t base;
    u64 workerId;
} paramListWorkerInst_t;

/******************************************************/
/* OCR WORKER                                         */
/******************************************************/

struct _ocrWorker_t;
struct _ocrTask_t;

typedef struct _ocrWorkerFcts_t {
    void (*destruct)(struct _ocrWorker_t *self);

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
    u8 (*switchRunlevel)(struct _ocrWorker_t* self, struct _ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(struct _ocrPolicyDomain_t*,u64), u64 val);

    /** @brief Run Worker
     */
    void* (*run)(struct _ocrWorker_t *self);

    /**
     * @brief Allows the worker to request another EDT/task
     * to execute
     *
     * This is used by some implementations that support/need
     * context switching during the execution of an EDT
     * @warning This API is a WIP and should not be relied on at
     * the moment (see usage in HC's blocking scheduler)
     */
    void* (*workShift)(struct _ocrWorker_t *self);

    /** @brief Check if Worker is still running
     *  @return true if the Worker is running, false otherwise
     */
    bool (*isRunning)(struct _ocrWorker_t *self);

    /** @brief Prints the location of this worker
     *
     *  The location field should be allocated (stack or heap) by the caller
     *  and be able to hold 32 characters at least.
     */
    void (*printLocation)(struct _ocrWorker_t *self, char* location);

} ocrWorkerFcts_t;

// Relies on runlevel and phases to be encoded on a maximum of 4 bits
#define GET_STATE(rl, phase) (((rl) << 4) | (phase))
#define GET_STATE_RL(state) ((state) >> 4)
#define GET_STATE_PHASE(state) ((state) & 0xF)

typedef struct _ocrWorker_t {
    ocrFatGuid_t fguid;
    struct _ocrPolicyDomain_t *pd;
    ocrLocation_t location;  //BUG #605 Locations spec: need this ?
    ocrWorkerType_t type;
    u8 amBlessed; // BUG #583: Clean-up runlevels; maybe merge in type?
    u64 id; //Worker id as indicated in runtime config
    // Workers are capable modules so
    // part of their runlevel processing happens asynchronously
    // This provides a convenient location to save
    // what is needed to do this
    // The state encodes the RL (top 4 bits) and the phase (bottom 4 bits)
    volatile u8 curState, desiredState;
    // BUG #583: Clean up runlevels; Do we need something with RL properties?
    void (*callback)(struct _ocrPolicyDomain_t*, u64);
    u64 callbackArg;

#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcess_t *statProcess;
#endif

    ocrCompTarget_t **computes; /**< Compute node(s) associated with this worker */
    u64 computeCount;           /**< Number of compute node(s) associated */
    //TODO-DEFERRED, I don't want this to be volatile
    struct _ocrTask_t * volatile curTask; /**< Currently executing task */

    ocrWorkerFcts_t fcts;

#ifdef OCR_MONITOR_SCHEDULER
    bool isSeeking;
#endif
#ifdef ENABLE_RESILIENCY
    volatile u32 stateOfCheckpoint;
    volatile u32 stateOfRestart;
    volatile u32 resiliencyMaster;
    volatile u32 isIdle;
    volatile u32 edtDepth;
    volatile ocrEdtDep_t *activeDepv;
    lock_t notifyLock;
#endif
} ocrWorker_t;


/****************************************************/
/* OCR WORKER FACTORY                               */
/****************************************************/

typedef struct _ocrWorkerFactory_t {
    ocrWorker_t* (*instantiate) (struct _ocrWorkerFactory_t * factory,
                                 ocrParamList_t *perInstance);
    void (*initialize) (struct _ocrWorkerFactory_t * factory, struct _ocrWorker_t * worker, ocrParamList_t *perInstance);
    void (*destruct)(struct _ocrWorkerFactory_t * factory);
    ocrWorkerFcts_t workerFcts;
} ocrWorkerFactory_t;

void initializeWorkerOcr(ocrWorkerFactory_t * factory, ocrWorker_t * self, ocrParamList_t *perInstance);

#endif /* __OCR_WORKER_H__ */
