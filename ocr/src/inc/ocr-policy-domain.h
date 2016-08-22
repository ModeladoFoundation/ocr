/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef OCR_POLICY_DOMAIN_H_
#define OCR_POLICY_DOMAIN_H_

#include "ocr-allocator.h"
#include "ocr-comm-api.h"
#include "ocr-datablock.h"
#include "ocr-event.h"
#include "ocr-guid.h"
#include "ocr-scheduler.h"
#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif
#include "ocr-task.h"
#include "ocr-types.h"
#include "ocr-worker.h"

#include "experimental/ocr-platform-model.h"
#include "experimental/ocr-placer.h"

#include "utils/queue.h"

struct _pdEvent_t;

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/

typedef struct _paramListPolicyDomainFact_t {
    ocrParamList_t base;
} paramListPolicyDomainFact_t;

typedef struct _paramListPolicyDomainInst_t {
    ocrParamList_t base;
    ocrLocation_t location;
} paramListPolicyDomainInst_t;


/******************************************************/
/* RL macros                                          */
/******************************************************/
#define RL_ENSURE_PHASE_UP(pd, rl, comp, val) do {                      \
        u8 _t = (pd)->phasesPerRunlevel[rl][comp];                     \
        if((_t & 0xF) < (val)) _t = (_t & 0xF0) + (val);       \
        (pd)->phasesPerRunlevel[rl][comp] = _t;                         \
    } while(0)

#define RL_ENSURE_PHASE_DOWN(pd, rl, comp, val) do {                    \
        u32 _t = (pd)->phasesPerRunlevel[rl][comp];                     \
        if((_t >> 4) < (val)) _t = ((val) << 4) + (_t & 0xF);      \
        (pd)->phasesPerRunlevel[rl][comp] = _t;                         \
    } while(0)

// Get number of phases when going up for a given runlevel
#define RL_GET_PHASE_COUNT_UP(pd, rl) ((pd)->phasesPerRunlevel[rl][0] & 0xF)

// Get number of phases when going down for a given runlevel
#define RL_GET_PHASE_COUNT_DOWN(pd, rl) ((pd)->phasesPerRunlevel[rl][0] >> 4)

#define RL_IS_LAST_PHASE_UP(pd, rl, phase) ((RL_GET_PHASE_COUNT_UP(pd, rl) - 1) == (phase))
#define RL_IS_LAST_PHASE_DOWN(pd, rl, phase) (phase == 0)

#define RL_IS_FIRST_PHASE_UP(pd_, rl, phase)  (phase == 0)
#define RL_IS_FIRST_PHASE_DOWN(pd, rl, phase) ((RL_GET_PHASE_COUNT_DOWN(pd, rl) - 1) == (phase))


/******************************************************/
/* OCR POLICY DOMAIN INTERFACE                        */
/******************************************************/

// WARNING: Keep this list in sync with ocr-policy-msg-list.h

/**< Invalid message */
#define PD_MSG_INVAL            0

/* Bit structure for types of message:
 *     - Bottom 12 bits encode whether the message is about computation,
 *       memory, etc (001, 002, 004, ...)
 *     - Next 4 bits indicate the operation ID (sequential number 1, 2, 3...)
 *     - Next 2 bits indicate the number of ocrFatGuid_t in the in/out part
 *       of the message
 *     - Next 3 bits indicate the number of ocrFatGuid_t in the in part of the
 *       message
 *     - Next 3 bits indicate the number of ocrFatGuid_t in the out part of the
 *       message
 *     - Top 8 bits contain flags such as query/response, etc.
 *     - 0x100000 and 0x200000 indicate query/response
 */
/**< AND with this and if result non-null, movable-memory operation.
 * Generally, these will be DBs but it could be something else too
 * (runtime DBs, etc). For pure memory chunks (directly out
 * of allocators), use PD_MSG_MEM_OP
 */
#define PD_MSG_DB_OP            0x001
/**< Create a memory area (allocate) */
#define PD_MSG_DB_CREATE        0x00051001
/**< Destroy a DB (orginates from PD<->PD) */
#define PD_MSG_DB_DESTROY       0x00082001
/**< Acquires a DB */
#define PD_MSG_DB_ACQUIRE       0x00023001
/**< Releases a DB */
#define PD_MSG_DB_RELEASE       0x00054001
/**< Frees a DB (the last free may trigger a destroy) */
#define PD_MSG_DB_FREE          0x00085001

/**< AND with this and if the result non-null, memory chunks
 * related operation (goes directly to allocators) */
#define PD_MSG_MEM_OP                 0x002
#define PD_MSG_MEM_ALLOC              0x00401002
/**< De-allocates a chunk of memory (through an allocator).
 * This is called internally */
#define PD_MSG_MEM_UNALLOC            0x00082002

/**< AND with this and if result non-null, work/task related operation.
 * Generally, these will be EDTs but it could be something else too
 */
#define PD_MSG_WORK_OP          0x004
/**< Create an EDT */
#define PD_MSG_WORK_CREATE      0x000E1004
/**< Execute this EDT (originates from PD<->PD) */
#define PD_MSG_WORK_EXECUTE     0x00042004
/**< Destroy an EDT (originates from PD<->PD) */
#define PD_MSG_WORK_DESTROY     0x00083004

/**< AND with this and if result non-null, EDT-template related operation */
#define PD_MSG_EDTTEMP_OP       0x008
/**< Create an EDT template */
#define PD_MSG_EDTTEMP_CREATE   0x00051008
/**< Destroy an EDT template */
#define PD_MSG_EDTTEMP_DESTROY  0x00082008

/**< AND with this and if result non-null, Event related operation */
#define PD_MSG_EVT_OP           0x010
/**< Create an event */
#define PD_MSG_EVT_CREATE       0x00051010
/**< Destroy an event */
#define PD_MSG_EVT_DESTROY      0x00082010
/**< Get the entity that satisfied the event (originates from PD<->PD) */
#define PD_MSG_EVT_GET          0x00243010

/**< AND with this and if result non-null, GUID related operations */
#define PD_MSG_GUID_OP          0x020
/**< Either associate a known u64 value with a created GUID OR
 * create a GUID metadata and associate that address with a GUID */
#define PD_MSG_GUID_CREATE      0x00011020

/**< Gets information about the GUID */
#define PD_MSG_GUID_INFO        0x00012020

/**< Request a copy of the GUID's metadata */
#define PD_MSG_GUID_METADATA_CLONE  0x00013020

/**< Reserve GUID space for labeled GUIDs */
#define PD_MSG_GUID_RESERVE    0x00004020

/**< Remove the reservation for GUID space */
#define PD_MSG_GUID_UNRESERVE   0x00005020

/**< Release the GUID (destroy the association between the u64
 * value and the GUID and optionally destroy the associated
 * metadata */
#define PD_MSG_GUID_DESTROY     0x00046020

/**< Low-level messages to push and pull metadata across policy-domains
 */
#define PD_MSG_METADATA_COMM    0x00007020

/**< AND with this and if result non-null, GUID distribution related
 * operation (taking/giving EDTs, DBs, events, etc)
 */
#define PD_MSG_SCHED_OP          0x040
/**< Find tasks for the worker to execute */
#define PD_MSG_SCHED_GET_WORK    0x00001040
/**< Notify a scheduler of a new guid or event */
#define PD_MSG_SCHED_NOTIFY      0x00002040
/**< Transaction of a scheduler object between schedulers */
#define PD_MSG_SCHED_TRANSACT    0x00003040
/**< Negotiations between schedulers to setup transactions */
#define PD_MSG_SCHED_ANALYZE     0x00004040
/**< Update scheduler state */
#define PD_MSG_SCHED_UPDATE      0x00005040

/**< Request for a GUID (ie: the caller wants the callee
 * to give it the GUID(s) requested (pull model) */
#define PD_MSG_COMM_TAKE        0x00006040
/**< Request for a GUID to be put (ie: the caller wants the
 * callee to accept the GUID(s) given (push model) */
#define PD_MSG_COMM_GIVE        0x00007040

/**< AND with this and if result non-null, dependence related
 * operation
 */
#define PD_MSG_DEP_OP           0x080
/**< Add a dependence. This will call registerSignaler and registerWaiter
 * on both sides of the dependence as appropriate*/
#define PD_MSG_DEP_ADD          0x000c1080

/**< Register a signaler on a waiter. This is called internally by the PD
 * as a result of PD_MSG_DEP_ADD (potentially). DEP_ADD is effectively
 * a REGSIGNALER and a REGWAITER.
 */
#define PD_MSG_DEP_REGSIGNALER  0x00082080

/**< Register a waiter on a signaler. This is called internally by the PD
 * as a result of PD_MSG_DEP_ADD (potentially). DEP_ADD is effectively
 * a REGSIGNALER and a REGWAITER.
 */
#define PD_MSG_DEP_REGWAITER    0x00083080

/**< Satisfy a dependence. A user can satisfy an event but the general
 * case is that a signaler satisfies its waiter(s)
 */
#define PD_MSG_DEP_SATISFY      0x00104080

/**< Unregister a signaler on a waiter. This is called internally
 * when an event is destroyed for example */
#define PD_MSG_DEP_UNREGSIGNALER 0x00085080

/**< Unregister a signaler on a waiter */
#define PD_MSG_DEP_UNREGWAITER  0x00086080

/**< Adds a "dynamic" dependence (creation of a DB mostly) */
#define PD_MSG_DEP_DYNADD       0x00087080

/**< Removes a potential dynamic dependence */
#define PD_MSG_DEP_DYNREMOVE    0x00088080

/**< AND with this and if result non-null, low-level OS operation */
#define PD_MSG_SAL_OP           0x100
/**< Print operation */
#define PD_MSG_SAL_PRINT        0x00001100
/**< Read operation */
#define PD_MSG_SAL_READ         0x00002100
/**< Write operation */
#define PD_MSG_SAL_WRITE        0x00003100
// BUG #611: Potentially provide more operations to support newlib/legacy
/**< Abort/exit the runtime */
#define PD_MSG_SAL_TERMINATE    0x00004100

/**< And with this and if the result is non-null, PD management operations */
#define PD_MSG_MGT_OP           0x200

/**< Register a policy-domain with another
 * one. This registration is one way (ie: the
 * source of this call already knows of the destination
 * since it is sending it a message) */
#define PD_MSG_MGT_REGISTER     0x00001200

/**< Opposite of register */
#define PD_MSG_MGT_UNREGISTER   0x00002200

/**< For a worker to request the policy-domain to monitor an operation progress */
#define PD_MSG_MGT_MONITOR_PROGRESS 0x00003200

/**< Runlevel change notification */
#define PD_MSG_MGT_RL_NOTIFY        0x00004200

/**< AND with this and if result non-null, hint related operation.
 * Generally, these will be calls to set/get user hints
 */
#define PD_MSG_HINT_OP          0x400
/**< Set hint on guid */
#define PD_MSG_HINT_SET      0x00041400
/**< Get hint from guid */
#define PD_MSG_HINT_GET      0x00042400

/**< And this to just get the type of the message (note that the number
 * of ocrFatGuid_t is part of the type as for a given type, this won't change)
 */
#define PD_MSG_TYPE_ONLY        0x00FFFFFFUL

/**< Get just the flags */
#define PD_MSG_META_ONLY        0xFF000000UL

/**< Get just the number of ocrFatGuid_t in the in/out part */
#define PD_MSG_FG_IO_COUNT_ONLY    0x00030000UL

/**< Get just the number of ocrFatGuid_t in the in part */
#define PD_MSG_FG_I_COUNT_ONLY     0x001C0000UL

#define PD_MSG_FG_O_COUNT_ONLY     0x00E00000UL

#define PD_MSG_FG_IO_COUNT_ONLY_GET(__msgtype) ((u32) (__msgtype & PD_MSG_FG_IO_COUNT_ONLY) >> 16)
#define PD_MSG_FG_I_COUNT_ONLY_GET(__msgtype) ((u32) (__msgtype & PD_MSG_FG_I_COUNT_ONLY) >> 18)
#define PD_MSG_FG_O_COUNT_ONLY_GET(__msgtype) ((u32) (__msgtype & PD_MSG_FG_O_COUNT_ONLY) >> 21)

/**< Defines that the message is a query (non-answered) */
#define PD_MSG_REQUEST          0x01000000
/**< Defines that the message is a response */
#define PD_MSG_RESPONSE         0x02000000
/**< Defines if the message requires a return message */
#define PD_MSG_REQ_RESPONSE     0x04000000
/**< Defines a message that is a request
 * but actually a response to another query (ie:
 * the receiver was expecting something else but
 * we changed the response (shutdown for example) */
#define PD_MSG_RESPONSE_OVERRIDE 0x08000000

/**< Defines if/how a message needs to be processed by the scheduler
 * By default, every msg is pre-processed by scheduler by default.
 * However the post-processing is not default and is dependent on
 * the scheduler heuristic that is used.
 * BUG 929 - A generic framework would be useful */
#define PD_MSG_IGNORE_PRE_PROCESS_SCHEDULER 0x10000000 /* Bit used to turn off default pre-process of msgs */
#define PD_MSG_REQ_POST_PROCESS_SCHEDULER   0x20000000 /* Bit to indicate if msg requires post processing */
#define PD_MSG_LOCAL_PROCESS                0x40000000 /* Bit to indicate that the msg should be locally processed by the PD */

// This is mainly used as a mean to tag a message comes from the user/rt interface
// The runtime may or may not allow the deferrability
#define PD_MSG_DEFERRABLE                0x80000000 /* Bit to indicate if msg execution can be deferred until end of EDT */

#define PD_MSG_ARG_NAME_SUB(ID) _data_##ID
#define PD_MSG_STRUCT_NAME(ID) PD_MSG_ARG_NAME_SUB(ID)

#define _PD_MSG_FIELD_FULL_SUB_QUAL(ptr, type, qual, field) ptr->args._data_##type.qual.field
#define _PD_MSG_FIELD_FULL_SUB(ptr, type, field) ptr->args._data_##type.field
#define _PD_MSG_INOUT_STRUCT_SUB(ptr, type) ptr->args._data_##type.inOrOut

#define _PD_MSG_FIELD_FULL_QUAL(ptr, type, qual, field) _PD_MSG_FIELD_FULL_SUB_QUAL((ptr), type, qual, field)
#define _PD_MSG_FIELD_FULL(ptr, type, field) _PD_MSG_FIELD_FULL_SUB((ptr), type, field)

#define _PD_MSG_INOUT_STRUCT(ptr, type) _PD_MSG_INOUT_STRUCT_SUB(ptr, type)

#define PD_MSG_FIELD_IO(field) _PD_MSG_FIELD_FULL(PD_MSG, PD_TYPE, field)
#define PD_MSG_FIELD_I(field) _PD_MSG_FIELD_FULL_QUAL(PD_MSG, PD_TYPE, inOrOut.in, field)
#define PD_MSG_FIELD_O(field) _PD_MSG_FIELD_FULL_QUAL(PD_MSG, PD_TYPE, inOrOut.out, field)
#define PD_MSG_INOUT_STRUCT(ptr) _PD_MSG_INOUT_STRUCT(ptr, PD_TYPE)

// Assumes that all union members start at the start of the union.
// Works for now but may have issues with packing
#define _PD_MSG_SIZE_ALL_SUB(type)                              \
    ((u64)(&(((struct _ocrPolicyMsg_t*)0)->args)) +             \
     sizeof(((struct _ocrPolicyMsg_t*)0)->args._data_##type))
#define _PD_MSG_SIZE_ALL(type) _PD_MSG_SIZE_ALL_SUB(type)
#define PD_MSG_SIZE_ALL _PD_MSG_SIZE_ALL(PD_TYPE)

#define _PD_MSG_SIZE_IN_SUB(type)                                       \
    ((u64)(&(((struct _ocrPolicyMsg_t*)0)->args._data_##type.inOrOut)) + \
     sizeof(((struct _ocrPolicyMsg_t*)0)->args._data_##type.inOrOut.in))
#define _PD_MSG_SIZE_IN(type) _PD_MSG_SIZE_IN_SUB(type)
#define PD_MSG_SIZE_IN _PD_MSG_SIZE_IN(PD_TYPE)

#define _PD_MSG_SIZE_OUT_SUB(type)                                      \
    ((u64)(&(((struct _ocrPolicyMsg_t*)0)->args._data_##type.inOrOut)) + \
     sizeof(((struct _ocrPolicyMsg_t*)0)->args._data_##type.inOrOut.out))
#define _PD_MSG_SIZE_OUT(type) _PD_MSG_SIZE_OUT_SUB(type)
#define PD_MSG_SIZE_OUT _PD_MSG_SIZE_OUT(PD_TYPE)

#define PD_MSG_RESET_FULL_SUB(ptr) \
    do { ptr->destLocation = UNDEF_LOCATION; ptr->msgId = 0; } while(0)
#define PD_MSG_RESET_FULL(ptr) PD_MSG_RESET_FULL_SUB((ptr))
#define PD_MSG_RESET PD_MSG_RESET_FULL(PD_MSG)

struct _ocrPolicyDomain_t;

// Defines a stack ocrPolicyMsg_t and sets the two size fields correctly
// The size fields are defined extremely conservatively. They will be udpated
// in the PD's processMessage (well, usefulSize will)
/**
 * @brief Defines an ocrPolicyMsg_t on the stack and sets usefulSize and bufferSize
 *
 * Use this macro to allocate a ocrPolicyMsg_t on the stack and properly set-up
 * its buffer size
 */
#define PD_MSG_STACK(name) ocrPolicyMsg_t name; \
    name.usefulSize = 0; name.bufferSize = sizeof(ocrPolicyMsg_t); \
    name.srcLocation = name.destLocation = INVALID_LOCATION;

/**
 * @brief Structure describing a "message" that is used to communicate between
 * policy domains in an asynchronous manner
 *
 * Communication between policy domains is always assumed to be asynchronous
 * where the requester (source) does not execute the code of the remote
 * policy domain (it is therefore like a remote procedure call). Specific
 * implementations may choose a synchronous communication but asynchronous
 * communication is assumed.
 *
 * This class defines the "message" that will be sent between policy
 * domains. All messages to/from policy domains are encapsulated
 * in this format (even if synchronous)
 *
 * @warning If modifying this structure, make sure to update
 * the marshalling and unmarshalling functions in policy-domain-all.c
 */
typedef struct _ocrPolicyMsg_t {
    u64 msgId;                      /**< Implementation specific ID identifying
                                     * this message (if required) */
    u64 bufferSize;                 /**< Total size of the buffer containing this message */
    u64 usefulSize;                 /**< Size to transfer (useful payload). This size may be
                                     * modified by the marshalling code (for example, it will
                                     * be updated if elements are marshalled inside this buffer).
                                     * Always less than or equal to bufferSize */
    /*ocrLocation_t origSrcLocation;*/  /**< Original source of the message (in case
                                         * it gets forwarded around). This is currently not used
                                         * but there is example code in the CE PD for this */
    ocrLocation_t srcLocation;      /**< Source of the message
                                     * (location making the request) */
    ocrLocation_t destLocation;     /**< Destination of the message
                                     * (location processing the request) */
    u32 type;                   /**< Type of the message. Also includes if this
                                 * is a request or a response */
#ifdef OCR_MONITOR_NETWORK
    u64 marshTime;
    u64 sendTime;
    u64 rcvTime;
    u64 unMarshTime;
#endif
    u64 msgTime;

    /* The following rules apply to all fields in the message:
     *     - All ocrFatGuid_t are in/out parameters in the sense
     *       that if they come in with only the GUID information
     *       set, they may get fully resolved and the metaDataPtr
     *       may be set properly on return. If on return metaDataPtr is NULL,
     *       it means that the GUID metadata cannot be accessed from the PD
     *     - All 'guid' parameters for the creation of objects are in/out:
     *       If they are not NULL_GUID on input, the runtime will use the GUID
     *       specified for the object.
     *     - The 'properties' field is input only.
     *     - The 'returnDetail' field is output only and on return it contains
     *       the error code of any deeper call the processMessage function makes.
     *       The return value of processMessage is its own error code (ie: not
     *       the internal message's processing)
     */
    union {
        struct {
            ocrFatGuid_t guid;            /**< In/Out: GUID of the created
                                           * memory segment (usually a DB) */
            //BUG #273 See if these need to stay In/Out or if it can move back to in
            u32 properties;               /**< In: Properties for creation */
            u64 size;                     /**< In: Size of the created DB */
            union {
                struct {
                    ocrFatGuid_t edt;             /** In: EDT doing the creation */
                    ocrDataBlockType_t dbType;    /**< In: Type of memory requested */
                    ocrInDbAllocator_t allocator; /**< In: In-DB allocator */
                    ocrHint_t * hint;             /**< In: Hints passed by the user at DB creation time */
                } in;
                struct {
                    void* ptr;                    /**< Out: Address of created DB */
                    u32 returnDetail;             /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DB_CREATE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid;         /**< In: GUID of the DB to destroy */
                    ocrFatGuid_t edt;          /**< In: EDT doing the destruction */
                    u32 properties;            /**< In: properties for the destruction */
                } in;
                struct {
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DB_DESTROY);

        struct {
            // These four parameters should move to IN but they are here
            // due to the implementation of the lockable DB
            //BUG #273: This is reported in bug #273
            ocrFatGuid_t guid;         /**< In: GUID of the DB to acquire */
            ocrFatGuid_t edt;          /**< In: EDT doing the acquire (whenever relevant) */
            ocrLocation_t destLoc;     /**< In: Destination location for the acquire */
            u32 edtSlot;               /**< In: EDT's slot if applicable else EDT_SLOT_NONE */
            u32 properties;            /**< In: Properties for acquire. Bit 0: 1 if runtime acquire */
            union {
                struct {
                } in;
                struct {
                    void* ptr;                 /**< Out: Pointer to the acquired memory */
                    u64 size;                  /**< Out: Size of the acquired memory */
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DB_ACQUIRE);

        struct {
            //BUG #273 This field should also be just IN
            ocrFatGuid_t guid;         /**< In: GUID of the DB to release */
            union {
                struct {
                    ocrFatGuid_t edt;          /**< In: GUID of the EDT doing the release */
                    ocrLocation_t srcLoc;      /**< In: Source location doing the release */
                    void* ptr;                 /**< In: Optionally provide pointer to the released memory */
                    u64 size;                  /**< In: Optionally provide size to the released memory */
                    u32 properties;            /**< In: Properties of the release: Bit 0: 1 if runtime release */
                } in;
                struct {
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DB_RELEASE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid;         /**< In: GUID of the DB to free */
                    ocrFatGuid_t edt;          /**< In: GUID of the EDT doing the free */
                    ocrLocation_t srcLoc;      /**< In: Location emitting the free */
                    u32 properties;            /**< In: Properties of the free */
                } in;
                struct {
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DB_FREE);

        struct {
            union {
                struct {
                    u64 size;                  /**< In: Size of memory chunk to allocate */
                    ocrMemType_t type;         /**< In: Type of memory requested */
                    u32 properties;            /**< In: Properties for the allocation */
                } in;
                struct {
                    ocrFatGuid_t allocatingPD; /**< Out: GUID of the PD that owns the allocator */
                    ocrFatGuid_t allocator;    /**< Out: GUID of the allocator that provided this memory */
                    void* ptr;                 /**< Out: Pointer of the allocated chunk */
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_MEM_ALLOC);

        struct {
            union {
                struct {
                    ocrFatGuid_t allocatingPD; /**< In: GUID of the PD that owns the allocator; not necessarily
                                                * required for all ocrMemType_t (in particular GUID_MEMTYPE) */
                    ocrFatGuid_t allocator;    /**< In: GUID of the allocator that gave the memory; same comment
                                                * as above */
                    void* ptr;                 /**< In: Pointer to the memory to free */
                    ocrMemType_t type;         /**< In: Type of memory to free */
                    u32 properties;            /**< In: Properties for the free */
                } in;
                struct {
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_MEM_UNALLOC);

        struct {
            ocrFatGuid_t guid;         /**< In/Out: GUID of the EDT/Work
                                        * to create */
            ocrFatGuid_t outputEvent;  /**< In/Out: If UNINITIALIZED_GUID on input,
                                        * will contain the output event to wait for for this
                                        * EDT. If NULL_GUID, no event will be created
                                        * and returned. */
            u32 paramc;                /**< In/out: Number of parameters; on out returns real number
                                        * in case of EDT_PARAM_DEF as input for example */
            u32 depc;                  /**< In/out: Number of dependence slots; same comment as above */
            union {
                struct {
                    ocrFatGuid_t templateGuid; /**< In: GUID of the template to use */
                    ocrFatGuid_t parentLatch;  /**< In: Parent latch for EDT */
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is creating work */
                    u64 *paramv;               /**< In: Parameters for this EDT */
                    ocrFatGuid_t * depv;       /**< In: Dependences for this EDT */
                    ocrHint_t * hint;          /**< In: Hints passed by the user at EDT creation time */
                    ocrWorkType_t workType;    /**< In: Type of work to create */
                    u32 properties;            /**< In: properties for the creation */
                } in;
                struct {
                    u32 returnDetail;          /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_WORK_CREATE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid; /**< In: GUID of the EDT to execute */
                    u32 properties;    /**< In: Properties for the execution */
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_WORK_EXECUTE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid; /**< In: GUID of the EDT to destroy */
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is destroying work */
                    u32 properties;    /**< In: properties for the destruction */
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_WORK_DESTROY);

        struct {
            ocrFatGuid_t guid;     /**< In/Out: GUID of the EDT template */
            union {
                struct {
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is creating template */
                    ocrEdt_t funcPtr;      /**< In: Function to execute for this EDT */
                    u32 paramc;            /**< In: Number of parameters for EDT */
                    u32 depc;              /**< In: Number of dependences for EDT */
                    u32 properties;        /**< In: Properties */
#ifdef OCR_ENABLE_EDT_NAMING
                    const char * funcName; /**< In: Debug help: user identifier */
                    u64 funcNameLen;       /**< In: Number of characters (excluding '\0') in funcName */
#endif
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_EDTTEMP_CREATE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid; /**< In: GUID of the EDT template to destroy */
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is destroying the template */
                    u32 properties;    /**< In: properties for the destruction */
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_EDTTEMP_DESTROY);

        struct {
            ocrFatGuid_t guid;    /**< In/Out: GUID of the event to create */
            union {
                struct {
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is creating event */
#ifdef ENABLE_EXTENSION_PARAMS_EVT
                    ocrEventParams_t * params;
#endif
                    u32 properties;       /**< In: Properties for this creation */
                    ocrEventTypes_t type; /**< In: Type of the event created: Bit 0: 1 if event takes an argument */
                } in;
                struct {
                    u32 returnDetail;     /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_EVT_CREATE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid; /**< In: GUID of the event to destroy */
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is destroying event */
                    u32 properties;    /**< In: properties for the destruction */
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_EVT_DESTROY);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid; /**< In: GUID of the event to get the data from */
                    u32 properties;    /**< In: Properties for the get */
                } in;
                struct {
                    ocrFatGuid_t data; /**< Out: GUID of the DB used to satisfy the event */
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_EVT_GET);

        struct {
            ocrFatGuid_t guid; /**< In/Out:
                                *  In: The metaDataPtr field contains the value
                                *  to associate with the GUID or NULL if the metadata
                                *  memory needs to be created
                                *  Out: The guid field contains created GUID */
            union {
                struct {
                    u64 size;          /**< In: If metaDataPtr is NULL on input, contains the
                                        *   size needed to contain the metadata. Otherwise ignored */
                    ocrGuidKind kind;  /**< In: Kind of the GUID to create */
                    ocrLocation_t targetLoc; /**< In: Target location of the GUID to create */
                    u32 properties;    /**< In: Properties for the creation. */
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_GUID_CREATE);

        struct {
            ocrFatGuid_t guid; /**< In/Out:
                                * In: The GUID field should be set to the GUID
                                * whose information is needed
                                * Out: Fully resolved information */
            union {
                struct {
                    u32 properties;    /**< In: Properties for the info. See ocrGuidInfoProp_t */
                } in;
                struct {
                    ocrGuidKind kind;  /**< Out: Contains the type of the GUID */
                    ocrLocation_t location; /**< Out: Contains the location of the GUID */
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_GUID_INFO);

        struct {
            ocrFatGuid_t guid; /**< In/Out:
                                * In: The GUID we request a metadata copy
                                * Out: The GUID and pointer to the cloned metadata */
            union {
                struct {
                } in;
                struct {
                    u64 size;          /**< Out: Size of the metadata that was cloned */
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_GUID_METADATA_CLONE);

        struct {
            union {
                struct {
                    ocrGuid_t guid;    /**< In: The GUID of the metadata */
                    u64 mode;          /**< In: Metadata implementation specific serialization mode */
                    struct _ocrPolicyMsg_t * response; /** TODO: this would move into the handle_t/comm_t */
                    void * mdPtr;      /**< In: The deserialized metadata. This is a placeholder for
                                                when deserialize has been called on the payload,
                                                it is NULL otherwise */
                    u32 factoryId;     /**< In: The factory ID for the metadata */
                    u32 sizePayload;   /**< In: The serialized metadata payload size */
                    ocrObjectOperation_t op; /**< In: Operation that triggered the push/pull */
                    u8 direction;       /**< In: Pull or Push message */
                    char payload;      /**< In: Metadata implementation specific serialized data of size 'sizePayload' */
                } in;
                struct {
                    u32 returnDetail;   /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_METADATA_COMM);

        struct {
            union {
                struct {
                    u64 numberGuids;      /**< In: Number of GUIDs being reserved */
                    ocrGuidKind guidKind; /**< In: GUID type for the GUIDs being reserved */
                } in;
                struct {
                    ocrGuid_t startGuid;  /**< Out: First GUID usable in the reserved range */
                    u64 skipGuid;         /**< Out: Skip value between two consecutive GUIDs in the range */
                    u32 returnDetail;     /**< Out: Return value; 0 on success */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_GUID_RESERVE);

        struct {
            union {
                struct {
                    ocrGuid_t startGuid;  /**< In: First GUID of the range to un-reserve */
                    u64 skipGuid;         /**< In: Skip value between consecutive GUIDs in the range */
                    u64 numberGuids;      /**< In: Number of GUIDs in the range */
                } in;
                struct {
                    u32 returnDetail;     /**< Out: Return value; 0 on success */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_GUID_UNRESERVE);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid; /**< In: GUID to destroy */
                    u32 properties;    /**< In: Properties for the destruction:
                                        * Bit 0: If 1, metadata area is "freed" */
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_GUID_DESTROY);

        struct {
            ocrFatGuid_t *guids; /**< In/Out: GUID(s) of the work/DB/etc taken:
                                 * In (optional): GUID(s) requested. If no GUID
                                 * is specifically requested, guids[0].guid needs to be
                                 * NULL_GUID
                                 * Out: GUID(s) given to the caller
                                 * by the callee */
            u64 extra;           /**< In/Out: Additional information on the take (for eg: function
                                  * pointer to use to execute the returned EDTs
                                  * BUG #586: Could this be moved to OUT ? */
            ocrGuidKind type;    /**< In/Out Kind of GUIDs requested */
            u32 guidCount;       /**< In/Out: Number of GUID(s) in guids.
                                  * In: If guids[0].guid, number of GUIDs requested in
                                  *     guids. If guids[0].guid is NULL_GUID, maximum
                                  *     number of guids requested
                                  * Out: Number of guids returned
                                  */
            union {
                struct {
                    u32 properties;      /**< In: properties for the take; */
                } in;
                struct {
                    u32 returnDetail;    /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
            // BUG #586: Take into account cost/choice heuristic
        } PD_MSG_STRUCT_NAME(PD_MSG_COMM_TAKE);

        struct {
            ocrFatGuid_t *guids; /**< In/Out: GUID(s) of the work/DB/etc given:
                                 * In: GUID(s) the caller wants to hand-off
                                 * to the callee
                                 * Out (optional): GUID(s) NOT accepted
                                 * by callee */
            u32 guidCount;       /**< In/Out: Number of GUID(s) in guids
                                  * In: Number of GUIDs the caller wants to hand-off
                                  * Out: Number of GUIDs rejected (in guids) */
#ifdef ENABLE_HINTS
            u64 **hints;         /**< In/Out: Array of pointers to hints for the guids.
                                  * The array length should be equal to guidCount */
#endif
            union {
                struct {
                    ocrGuidKind type;    /**< In: Kind of GUIDs given */
                    u32 properties;      /**< In: properties for the give */
                } in;
                struct {
                    u32 returnDetail;    /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
            // BUG #586: Take into account cost/choice heuristic
        } PD_MSG_STRUCT_NAME(PD_MSG_COMM_GIVE);

        /* WORKER to SCHEDULER: Scheduling tasks for the worker to execute. These are typically
         *                      messages where the worker blocks for response.
         */
        struct {
            ocrSchedulerOpWorkArgs_t schedArgs;            /**< In/Out: Arguments for worker tasks */
            union {
                struct {
                    u32 properties;                         /**< In: properties for the op */
                } in;
                struct {
                    u32 factoryId;                          /**< Out: Task factory of EDTs returned */
                    u32 returnDetail;                       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SCHED_GET_WORK);

        /* Operation to notify scheduler about a new guid or event (typically used by other runtime modules)  */
        struct {
            ocrSchedulerOpNotifyArgs_t schedArgs;          /**< In/Out: Arguments for the scheduler notify operation */
            union {
                struct {
                    u32 properties;                         /**< In: properties for the op */
                } in;
                struct {
                    u32 returnDetail;                       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SCHED_NOTIFY);

        /* Operation where schedulers transact a scheduler object (scheduler-scheduler operation) */
        struct {
            ocrSchedulerOpTransactArgs_t schedArgs;        /**< In/Out: Arguments for the scheduler transact operation */
            u64 size;                                      /**< In/Out: Size of transacted object */
            union {
                struct {
                    u32 properties;                         /**< In: properties for the op */
                } in;
                struct {
                    u32 returnDetail;                       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SCHED_TRANSACT);

        /* Operation where schedulers analyze (no scheduler objects are transferred) and help setup transactions */
        struct {
            ocrSchedulerOpAnalyzeArgs_t schedArgs;       /**< In/Out: Arguments for the scheduler analyze operation */
            union {
                struct {
                    u32 properties;                         /**< In: properties for the op */
                } in;
                struct {
                    u32 returnDetail;                       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SCHED_ANALYZE);

        /* Operation to update scheduler state at certain times during the runtime  */
        struct {
            union {
                struct {
                    u32 properties;                         /**< In: properties for the op */
                } in;
                struct {
                    u32 returnDetail;                       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SCHED_UPDATE);

        struct {
            //BUG #273 THis is also being used at In/Out
            u32 properties;      /**< In: Properties. Lower 3 bits are access modes */
            union {
                struct {
                    ocrFatGuid_t source; /**< In: Source of the dependence */
                    ocrFatGuid_t dest;   /**< In: Destination of the dependence */
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is adding dep */
                    u32 slot;            /**< In: Slot of dest to connect the dep to */
                } in;
                struct {
                    u32 returnDetail;    /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_ADD);

        struct {
            union {
                struct {
                    ocrFatGuid_t signaler;  /**< In: Signaler to register */
                    ocrFatGuid_t dest;      /**< In: Object to register the signaler on */
                    u32 slot;               /**< In: Slot on dest to register the signaler on */
                    ocrDbAccessMode_t mode; /**< In: Access mode for the dependence's datablock */
                    u32 properties;         /**< In: Properties */
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_REGSIGNALER);

        struct {
            union {
                struct {
                    ocrFatGuid_t waiter;   /**< In: Waiter to register */
                    ocrFatGuid_t dest;     /**< In: Object to register the waiter on */
                    u32 slot;              /**< In: The slot on waiter that will be notified */
#ifdef REG_ASYNC_SGL
                    ocrDbAccessMode_t mode; /**< In: Caching the mode the destination should use when
                                                     it will get satisfied */
#endif
                    u32 properties;        /**< In: Properties */
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_REGWAITER);

        struct {
            union {
                struct {
                    ocrFatGuid_t satisfierGuid; /**< In: GUID of the "satisfier" (usually an EDT or event) */
                    ocrFatGuid_t guid;    /**< In: GUID of the event/task to satisfy */
                    ocrFatGuid_t payload; /**< In: GUID of the "payload" to satisfy the
                                           * event/task with (a DB usually). */
                    ocrFatGuid_t currentEdt;   /**< In: EDT that is satisfying dep */
                    u32 slot;             /**< In: Slot to satisfy the event/task on */
#ifdef REG_ASYNC_SGL
                    ocrDbAccessMode_t mode;
#endif
                    u32 properties;       /**< In: Properties for the satisfaction */
                } in;
                struct {
                    u32 returnDetail;     /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_SATISFY);

        struct {
            union {
                struct {
                    ocrFatGuid_t signaler; /**< In: Signaler to unregister */
                    ocrFatGuid_t dest;     /**< In: Object to unregister the signaler on */
                    u32 slot;              /**< In: Slot on dest to unregister the signaler on */
                    u32 properties;        /**< In: Properties */
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_UNREGSIGNALER);

        struct {
            union {
                struct {
                    ocrFatGuid_t waiter;   /**< In: Waiter to unregister */
                    ocrFatGuid_t dest;     /**< In: Object to unregister the waiter on */
                    u32 slot;              /**< In: The slot on waiter that will be notified */
                    u32 properties;        /**< In: Properties */
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_UNREGWAITER);

        struct {
            union {
                struct {
                    ocrFatGuid_t edt;      /**< In: EDT adding the dynamic dependence (to itself only) */
                    ocrFatGuid_t db;       /**< In: DB dynamically acquired */
                    u32 properties;        /**< In: Properties */
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_DYNADD);

        struct {
            union {
                struct {
                    ocrFatGuid_t edt;      /**< In: EDT removing the dynamic dependence (to itself only) */
                    ocrFatGuid_t db;       /**< In: DB dynamically being removed */
                    u32 properties;        /**< In: Properties */
                } in;
                struct {
                    u32 returnDetail;      /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_DEP_DYNREMOVE);

        struct {
            union {
                struct {
                    const char* buffer;  /**< In: Character string to print (including NULL terminator) */
                    u64 length;          /**< In: Length to print (including NULL terminator) */
                    u32 properties;      /**< In: Properties for the print */
                } in;
                struct {
                    u32 returnDetail;    /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SAL_PRINT);

        struct {
            u64 length;         /**< In/Out: On input contains maximum length
                                 * of buffer, on output, contains length read */
            union {
                struct {
                    u64 inputId;        /**< In: Identifier for where to read from */
                    u32 properties;     /**< In: Properties for the read */
                } in;
                struct {
                    char* buffer;       /**< Out: Buffer to read into */
                    u32 returnDetail;   /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SAL_READ);

        struct {
            u64 length;         /**< In/Out: Number of bytes to write. On return contains
                                 * bytes NOT written */
            union {
                struct {
                    const char* buffer; /**< In: Buffer to write */
                    u64 outputId;       /**< In: Identifier for where to write to */
                    u32 properties;     /**< In: Properties for the write */
                } in;
                struct {
                    u32 returnDetail;   /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SAL_WRITE);

        struct {
            union {
                struct {
                    u64 errorCode;     /**< In: Error code if applicable */
                    u32 properties;    /**< In: For now: 0 if normal exit, 1 if abort, 2 if assert*/
                } in;
                struct {
                    u32 returnDetail;  /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_SAL_TERMINATE);

        struct {
            union {
                struct {
                    ocrRunlevel_t runlevel; /**< In: Runlevel involved */
                    u32 properties;         /**< Properties: OR of the following flags
                                               One of:
                                                 - RL_REQUEST
                                                 - RL_RESPONSE
                                                 - RL_RELEASE
                                               One of:
                                                 - RL_ASYNC
                                                 - RL_BARRIER
                                               One of:
                                                 - RL_BRING_UP
                                                 - RL_TEAR_DOWN
                                            */
                    u8 errorCode;           /**< In switch from RL_USER, used to capture
                                               error code */
                } in;
                struct {
                    u32 returnDetail;       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_MGT_RL_NOTIFY);

        struct {
            union {
                struct {
                    ocrLocation_t loc;      /**< In Location being registered */
                    u32 properties;         /**< In Properties for the registration */
                } in;
                struct {
                    u32 returnDetail;       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_MGT_REGISTER);

        struct {
            union {
                struct {
                    ocrLocation_t neighbor; /**< In: Neighbor unregistering */
                    u32 properties;         /**< In */
                } in;
                struct {
                    u32 returnDetail;       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_MGT_UNREGISTER);

        struct {
            //BUG #273
            u32 properties; /**< In: first eight bits is type of monitoree */
            union {
                struct {
                    void * monitoree; /**< In */
                } in;
                struct {
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_MGT_MONITOR_PROGRESS);

        struct {
            union {
                struct {
                    ocrFatGuid_t guid;      /**< In: Target guid to set hints on */
                    ocrHint_t *hint;        /**< In: Hint to set */
                } in;
                struct {
                    u32 returnDetail;       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_HINT_SET);

        struct {
            ocrHint_t *hint;                /**< InOut: Hints retrieved from guid */
            union {
                struct {
                    ocrFatGuid_t guid;      /**< In: Guid from which hints are retrieved */
                } in;
                struct {
                    u32 returnDetail;       /**< Out: Success or error code */
                } out;
            } inOrOut __attribute__ (( aligned(8) ));
        } PD_MSG_STRUCT_NAME(PD_MSG_HINT_GET);
    } args;
} ocrPolicyMsg_t;

/**
 * @brief Initializes the size fields of ocrPolicyMsg_t
 *
 * Use this call to initialize a heap allocated policy message
 *
 * @param[in] msg         Message to initialize
 * @param[in] bufferSize  Buffer this message is allocated in (bytes)
 */
void initializePolicyMessage(ocrPolicyMsg_t* msg, u64 bufferSize);

/**
 * @brief Structure describing a message "handle" that is
 * used to keep track of the status of a communication.
 *
 * The handle is always created by the communication layer (comm-worker)
 * and destroyed by the user (caller of sendMessage, waitMessage...) using
 * the provided 'destruct' function.
 *
 * The response message will always be contained in handle->response.
 *
 * If 'msg' is non-NULL after a successful poll (handle->status = HDL_RESPONSE_OK),
 * or a wait, this serves as a reminder to the caller that the message
 * was passed in with PERSIST_MESSAGE_PROP (pointer to original message).
 *
 * The handle needs to be freed by the caller using the 'destruct' function.
 *
 * @see sendMessage() in ocr-comm-worker.h for more detail
 */
typedef struct _ocrMsgHandle_t {
    ocrPolicyMsg_t * msg;           /**< The message associated with the communication
                                       the handle represents. */
    ocrPolicyMsg_t * response;      /**< The response (if applicable) */
    ocrMsgHandleStatus_t status;   /**< Status of this handle. See ocrMsgHandleState */
    void (*destruct)(struct _ocrMsgHandle_t * self); /**< Destructor for this
                                                       * instance of the message
                                                       * handle */
    ocrCommApi_t* commApi;         /**< Pointer to the comm API that manages this handle */
    u64 properties;                /**< Implementation defined properties */
} ocrMsgHandle_t;

typedef struct _ocrPolicyDomainFcts_t {
    /**
     * @brief Destroys (and frees any associated memory) this
     * policy domain
     *
     * Call when the policy domain has stopped executing to free
     * any remaining memory.
     *
     * @param self                This policy domain
     */
    void (*destruct)(struct _ocrPolicyDomain_t *self);

    u8 (*switchRunlevel)(struct _ocrPolicyDomain_t* self, ocrRunlevel_t runlevel, u32 properties);

    /**
     * @brief Requests for the handling of the request msg
     *
     * This function can be called either by user code (for when the user code
     * requires runtime services) or internally by the runtime (when a message
     * has been received via CT.pollMessage for example)
     *
     * All code executed by processMessage until it reaches a sendMessage
     * is executed synchronously and inside the same address space.
     *
     * @param[in]     self       This policy domain
     * @param[in/out] msg        Message to process. Response will be
     *                           contained in msg if a response is
     *                           required and isBlocking is true. In all
     *                           cases, the message pointer must be
     *                           freed by the caller
     * @param[in]     isBlocking True if the processing of the message
     *                           need to complete before returning
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*processMessage)(struct _ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg,
                         u8 isBlocking);

    /**
     * @brief Micro-tasks version of processMessage
     *
     * This function can be called either by user code (for when the user code
     * requires runtime services) or internally by the runtime (when a message
     * has been received from another policy domain for example)
     *
     *
     * @param[in]     self       This policy domain
     * @param[in/out] evt        Input event which contains the message
     *                           to process. On output, will contain the
     *                           response in an event. This event may not be ready
     * @param[in]     idx        Position in the code to resume at (if the code
     *                           was interrupted due to a blocking call. Should be
     *                           0 for a direct initial call.
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*processEvent)(struct _ocrPolicyDomain_t *self,
                       struct _pdEvent_t** evt, u32 idx);
    /**
     * @brief Send a message outside of the policy domain.
     * This API can be used by any client of the policy domain and
     * will call into the correct comm-api sendMessage to actually
     * send the message.
     *
     * See ocr-comm-api.h for a detailed description
     * @param[in] self          This policy-domain
     * @param[in] target        Where to send the message
     * @param[in/out] handle    Handle for the message
     * @param[in] properties    Properties for the send
     * @return 0 on success and a non-zero error code
     */
    u8 (*sendMessage)(struct _ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                      ocrMsgHandle_t **handle, u32 properties);

    /**
     * @brief Non-blocking check for incoming messages
     * This API can be used by any client of the policy domain and
     * will call into the correct comm-api pollMessage to actually
     * poll for a message.
     *
     * See ocr-comm-api.h for a detailed description
     * @param[in] self          This policy-domain
     * @param[in/out] handle    Handle for the message
     * @return 0 on success and a non-zero error code
     */
    u8 (*pollMessage)(struct _ocrPolicyDomain_t *self, ocrMsgHandle_t **handle);

    /**
     * @brief Blocking check for incoming messages
     * This API can be used by any client of the policy domain and
     * will call into the correct comm-api waitMessage to actually
     * wait for a message.
     *
     * See ocr-comm-api.h for a detailed description
     * @param[in] self          This policy-domain
     * @param[in/out] handle    Handle for the message
     * @return 0 on success and a non-zero error code
     */
    u8 (*waitMessage)(struct _ocrPolicyDomain_t *self, ocrMsgHandle_t **handle);

    /**
     * @brief MT compatible send a message outside of the policy domain.
     *
     * This API can be used by any client of the policy domain and
     * will call into the correct comm-platform sendMessage to actually
     * send the message.
     *
     * See ocr-comm-platform.h for a detailed description
     * @param[in] self          This policy-domain
     * @param[in/out] inOutEvent Event to send; if the message requires a
     *                          response, the response will be contained in
     *                          the resulting event (the event may not be ready
     *                          on return)
     * @param[out] statusEvent  If needed, will return an event to poll on the status
     *                          of the send of the message
     * @param[in] idx           Position in the code to resume at (should be
     *                          0 for an initial direct call)
     * @return 0 on success and a non-zero error code
     */
    u8 (*sendMessageMT)(struct _ocrPolicyDomain_t* self, struct _pdEvent_t **inOutEvent,
                        struct _pdEvent_t *statusEvent, u32 idx);

    /**
     * @brief MT Non-blocking check for incoming messages that are
     * not responses to previously sent queries
     *
     * This API can be used by any client of the policy domain and
     * will call into the correct comm-platform pollMessage to actually
     * poll for a message.
     *
     * See ocr-comm-platform.h for a detailed description
     * @param[in] self          This policy-domain
     * @param[in/out] outEvent  Event containing message
     * @param[in] idx           Should always be 0
     * @return 0 on success and a non-zero error code
     */
    u8 (*pollMessageMT)(struct _ocrPolicyDomain_t *self, struct _pdEvent_t **outEvent, u32 idx);

    /**
     * @brief MT Blocking check for incoming messages that are
     * not responses to previously sent queries
     *
     * This API can be used by any client of the policy domain and
     * will call into the correct comm-platform pollMessage to actually
     * poll for a message.
     *
     * See ocr-comm-platform.h for a detailed description
     * @param[in] self          This policy-domain
     * @param[in/out] outEvent  Event containing message
     * @param[in] idx           Should always be 0
     * @return 0 on success and a non-zero error code
     */
    u8 (*waitMessageMT)(struct _ocrPolicyDomain_t *self, struct _pdEvent_t **outEvent, u32 idx);


    /**
     * @brief Policy-domain only allocation.
     *
     * Memory allocated with this call can only be used within the
     * policy domain and must be freed using pdFree(). Memory allocated
     * with pdMalloc is meant to be private to the policy domain
     *
     * @param size  Number of bytes to allocate
     * @return A pointer to the allocated space
     */
    void* (*pdMalloc)(struct _ocrPolicyDomain_t *self, u64 size);

    /**
     * @brief Policy-domain only free.
     *
     * Used to free memory allocated with pdMalloc
     *
     * @param addr Address to free
     */
    void (*pdFree)(struct _ocrPolicyDomain_t *self, void* addr);

#ifdef OCR_ENABLE_STATISTICS
    ocrStats_t* (*getStats)(struct _ocrPolicyDomain_t *self);
#endif
} ocrPolicyDomainFcts_t;

// Forward declaration of the strand tables
struct _pdStrandTable_t;

/**
 * @brief A policy domain is OCR's way of dividing up the runtime in scalable
 * chunks
 *
 * A policy domain is considered a 'synchronous' section of the runtime. In
 * other words, if a given worker makes a call into its policy domain, that call
 * will be processed synchronously by that worker (and therefore by the
 * compute target and platform running the worker) as long as it stays within
 * the policy domain. When the call requires processing outside the policy
 * domain, an asynchronous one-way 'message' will be sent to the other policy
 * domain (this is similar to a remote procedure call). Implementation
 * would determine if the RPC call is actually executed asynchronously by
 * another worker or executed synchronously by the same worker but it will
 * always be treated as an asynchronous call. Since the user API is
 * synchronous, if no response is needed, the call into the policy domain
 * can then return to the caller. However, if a response is required, a
 * worker specific function is called informing it that it needs to
 * 'wait-for-network' (or something like that). The exact implementation
 * will determine whether the worker halts or does something else.
 *
 * Each policy domain contains the following:
 *     - 0 or more 'schedulers' which both schedule tasks and place data
 *     - 0 or more 'workers' on which EDTs are executed
 *     - 0 or more 'allocators' from which DBs are allocated/freed
 *
 * A policy domain will directly respond to requests emanating from user
 * code to:
 *     - create EDTs (using ocrEdtCreate())
 *     - create DBs (using ocrDbCreate())
 *     - create GUIDs
 *
 */
typedef struct _ocrPolicyDomain_t {

// Any changes to this struct may cause problems in the symbolic constants inside
// tg-bin-files.h (build/tg-xe or build/tg-ce). To minimize problems, put factory first.
// (The factory in turn contains the pointers to the switchRunlevel functions, which is the relevant
// data for needing careful placement.
// See also xe-policy.h for a third magicly placed value, packedArgsLocation).

    ocrPolicyDomainFcts_t fcts;                 /**< Function pointers */

    ocrFatGuid_t fguid;                         /**< GUID for this policy */

    /* Inert modules */
    u64 commApiCount;                           /**< Number of comm APIs */
    u64 guidProviderCount;                      /**< Number of GUID providers */
    u64 allocatorCount;                         /**< Number of allocators */
    u64 schedulerCount;                         /**< Number of schedulers */

    /* Capable modules */
    u64 workerCount;                            /**< Number of workers */

    /* Factories */
    u64 factoryCount;                           /**< Number of factories (all types) */
    ocrObjectFactory_t **factories;             /**< Factories (all types) */
    // WARNING: Index order must match that in ocr-machine.h in type_enum
    u32 taskFactoryIdx;                         /**< First index of task factories */
    u32 taskTemplateFactoryIdx;                 /**< First index of task template factories */
    u32 datablockFactoryIdx;                    /**< First index of datablock factories */
    u32 eventFactoryIdx;                        /**< First index of event factories */


    u64 schedulerObjectFactoryCount;            /**< Number of schedulerObject factories */

    /* Objects based on counts above */
    ocrCommApi_t    ** commApis;                /**< All the communication interfaces */
    ocrGuidProvider_t ** guidProviders;         /**< GUID generators */
    ocrAllocator_t  ** allocators;              /**< All the allocators */
    ocrScheduler_t  ** schedulers;              /**< All the schedulers */

    ocrWorker_t     ** workers;                 /**< All the workers */

    ocrSchedulerObjectFactory_t **schedulerObjectFactories; /**< All the schedulerObject factories
                                                 * known to this policy domain */

    ocrPlacer_t * placer;                       //BUG #476 - This code is being deprecated

    ocrPlatformModel_t * platformModel;         /**< Platform model (WIP) */

    struct _pdStrandTable_t* strandTables[2];

    Queue_t* taskPerfs;                        /**< Table maintaining performance statistics of each EDT */

    /**
     * @brief Two dimensional array:
     * phasesPerRunLevel[i][j]:
     *     - i: runlevel
     *     - j: component from ocrRLPhaseComponents_t. After RL_CONFIG_PARSE
     *          phase, only j=0 is useful
     * The RL_CONFIG_PARSE always has two phases on bringup
     *     - the first phase all components in the order of
     *       ocrRLPhaseComponents_t are given the opportunity to say
     *       how many phases they need. If multiple components exist
     *       of the same type, this value should be updated to the
     *       maximum among all components of that type
     *     - in the second phase, the components get to see what the
     *       other components provided and this allows them to ensure
     *       a minimum level of compatibility
     * The bottom 32 bits encode the number of phases on BRING_UP
     * and the top 32 bits encode the number of phases on TEAR_DOWN
     * @todo The entire structure is only useful in the RL_CONFIG_PARSE
     * phase but part of it needs to be kept around for the shutdown
     * part. Is there a better way
     * BUG #583
     */
    s8 phasesPerRunlevel[RL_MAX][RL_PHASE_MAX];

    ocrLocation_t myLocation;
    ocrLocation_t parentLocation;
    ocrLocation_t * neighbors;                  /**< Array of neighbor locations */
    u32 neighborCount;                          /**< Number of neighboring policy domains */
    u8 shutdownCode;

    // BUG #605: Location support
    struct _ocrPolicyDomain_t **neighborPDs;
    struct _ocrPolicyDomain_t *parentPD;

    //TODO that looks TG related, should move to implementation
    s8 * allocatorIndexLookup;                  /**< Allocator indices for each block agent, over each of 8 memory levels */
#ifdef OCR_ENABLE_STATISTICS
    ocrStats_t *statsObject;                    /**< Statistics object */
#else
    u64 *junk_statsObject;                      /**< Placeholder, to assure consistency of PD structure length */
#endif

    volatile u64 pdTime;
    volatile u32 slowestWorker;
} ocrPolicyDomain_t;

/****************************************************/
/* OCR POLICY DOMAIN FACTORY                        */
/****************************************************/

typedef struct _ocrPolicyDomainFactory_t {
    /**
     * @brief Create a policy domain
     *
     * Allocates the required space for the policy domain
     * based on the counts passed as arguments. The 'schedulers',
     * 'workers', 'computes', 'workpiles', 'allocators' and 'memories'
     * data-structures must then be properly filled
     *
     * @param factory             This policy domain factory
     * @param configuration       An optional configuration
     * @param schedulerCount      The number of schedulers
     * @param workerCount         The number of workers
     * @param computeCount        The number of compute targets
     * @param workpileCount       The number of workpiles
     * @param allocatorCount      The number of allocators
     * @param memoryCount         The number of memory targets
     * @param taskFactory         The factory to use to generate EDTs
     * @param taskTemplateFactory The factory to use to generate EDTs templates
     * @param dbFactory           The factory to use to generate DBs
     * @param eventFactory        The factory to use to generate events
     * @param contextFactory      The factory to use to generate context
     * @param guidProvider        The provider of GUIDs for this policy domain
     * @param lockFactory         The factory to use to generate locks
     * @param atomicFactory       The factory to use to generate atomics
     * @param queueFactory        The factory to use to generate queues
     */

    ocrPolicyDomain_t * (*instantiate) (struct _ocrPolicyDomainFactory_t *factory,
#ifdef OCR_ENABLE_STATISTICS
                                        ocrStats_t *statsObject,
#endif
                                        ocrParamList_t *perInstance);
    void (*initialize) (struct _ocrPolicyDomainFactory_t *factory, ocrPolicyDomain_t* self,
#ifdef OCR_ENABLE_STATISTICS
                        ocrStats_t *statsObject,
#endif
                        ocrParamList_t *perInstance);
    void (*destruct)(struct _ocrPolicyDomainFactory_t * factory);
    ocrPolicyDomainFcts_t policyDomainFcts;
} ocrPolicyDomainFactory_t;


void initializePolicyDomainOcr(ocrPolicyDomainFactory_t * factory, ocrPolicyDomain_t * self, ocrParamList_t *perInstance);

/****************************************************/
/* UTILITY FUNCTIONS                                */
/****************************************************/

// The u32 property flag of send/recv/poll API calls
// relies on ocrMarshallMode_t to be 8 bits wide.
typedef enum {
    MARSHALL_FULL_COPY = 0x1,
    MARSHALL_DUPLICATE = 0x2,
    MARSHALL_APPEND    = 0x3,
    MARSHALL_ADDL      = 0x4,
    MARSHALL_TYPE      = 0xF,
    MARSHALL_NSADDR    = 0x10,
    MARSHALL_DBPTR     = 0x20,
    MARSHALL_FLAGS     = 0xF0
} ocrMarshallMode_t;

/**
 * @brief Gets the base size and the marshalled size of a message
 * that needs to be sent
 *
 *
 *
 * This call can be used on both a ocrPolicyMsg_t or a buffer
 * containing a marshalled ocrPolicyMsg_t.
 *
 * baseSize is the number of bytes in msg that are useful to transfer
 * for that particular message (looking at the type of message and
 * its direction)
 *
 * marshalledSize is the additional size that needs to be marshalled
 * and transferred for this message to be complete (for example paramv)
 *
 * @note This function uses msg->type to determine if the message is
 * leaving this PD (PD_MSG_REQUEST) in which case IN fields will be used
 * or is a response (PD_MSG_RESPONSE) in which case OUT fields will be used
 *
 * This call modifies msg->usefulSize *only* if msg->usefulSize is 0. In this
 * case, it will set it to 'baseSize'. No other modification of the message
 * happens.
 *
 * @param[in] msg               Message to analyze
 * @param[out] baseSize         Number of bytes at the head of msg that are useful
 * @param[out] marshalledSize   Additional data that needs to be
 *                              fetched/marshalled  (bytes)
 * @param[in] mode              The "flag" part of the marshalling mode
 *                              (MARSHALL_DBPTR and MARSHALL_NSADDR). Non flag bits
 *                              are ignored
 * @return 0 on success and a non-zero error code
 */
u8 ocrPolicyMsgGetMsgSize(struct _ocrPolicyMsg_t* msg, u64 *baseSize,
                          u64* marshalledSize, u32 mode);


// To get the in or out base size
u64 ocrPolicyMsgGetMsgBaseSize(ocrPolicyMsg_t *msg, bool isIn);

/**
 * @brief Marshall everything needed to send the message over to a PD that
 * does not share the same address space
 *
 * This function performs one of four operations depending on the
 * mode. You must specify one of:
 *
 *   - MARSHALL_FULL_COPY: Fully copy and marshall msg into buffer. In
 *                         this case buffer and msg do not overlap. The
 *                         caller also ensures that buffer is at least
 *                         big enough to contain baseSize + marshalledSize bytes as
 *                         returned by ocrPolicyMsgGetMsgSize. buffer->usefulSize will be
 *                         set to baseSize + marshalledSize. Note that the message
 *                         in buffer is not directly usable before an unmarshall
 *                         occurs as the pointers are encoded values.
 *   - MARSHALL_DUPLICATE: This is exactly like MARSHALL_FULL_COPY in the sense
 *                         that msg is fully marshalled into buffer; however,
 *                         buffer is directly usable as the pointers are not
 *                         encoded. In other words, this mode
 *                         allows you to duplicate an ocrPolicyMsg_t fully
 *                         including the first level of pointers. This mode
 *                         should not be used with an associated unmarshall
 *                         buffer->usefulSize will be set to baseSize + marshalledSize
 *   - MARSHALL_APPEND:    In this case (u64)buffer == (u64)msg and
 *                         marshalled values will be appended to the
 *                         end of the useful portion of msg. The caller
 *                         ensures that buffer is at least big enough to
 *                         contain baseSize + marshalledSize bytes as returned by
 *                         ocrPolicyMsgGetMsgSize. msg->usefulSize will be set to
 *                         baseSize + marshalledSize
 *   - MARSHALL_ADDL:      Marshall only needed portions (not already in msg)
 *                         into buffer. The caller ensures that buffer is
 *                         at least big enough to contain marshalledSize
 *                         as returned by ocrPolicyMsgGetMsgSize. msg->usefulSize
 *                         will be set to baseSize
 * and optionally OR it with any of the following:
 *   - MARSHALL_NSADDR:    Marshalling to send to a location that does not share
 *                         the same address space (across MPI ranks for example)
 *   - MARSHALL_DBPTR:     Marshall/unmarshall the ptr* field in ACQUIRE/RELEASE
 *                         calls. To save on message traffic, an acquire/release
 *                         contain the actual data-block if needed (going across
 *                         address spaces for example)
 *
 * Note that in the case of MARSHALL_APPEND and MARSHALL_ADDL, the
 * msg itself is modified to encode offsets and positions for the marshalled
 * values so that it can be unmarshalled by ocrPolicyMsgUnMarshallMsg. In
 * all cases, size is set to what actually needs to be sent over:
 *
 * @param[in/out] msg      Message to marshall from
 * @param[in] baseSize     "Base" size of msg ("baseSize" returned from ocrPolicyMsgGetMsgSize)
 * @param[in/out] buffer   Buffer to marshall to
 * @param[in] mode         One of MARSHALL_FULL_COPY, MARSHALL_APPEND, MARSHALL_ADDL
 * @return 0 on success and a non-zero error code
 */
u8 ocrPolicyMsgMarshallMsg(struct _ocrPolicyMsg_t* msg, u64 baseSize, u8* buffer, u32 mode);

/**
 * @brief Performs the opposite operation to ocrPolicyMsgMarshallMsg
 *
 * The inputs are the mainBuffer (containing the header of the message and
 * an optional additional buffer (obtained using the MARSHALL_ADDL mode).
 *
 * The mode is also a combination of any of MARSHALL_NSADDR and MARSHALL_DBPTR
 * (same significance as for marshall) as well as one of:
 *   - MARSHALL_FULL_COPY: In this case, none of the buffers (mainBuffer,
 *                         addlBuffer, msg) overlap. The entire content
 *                         of the message and any additional structure
 *                         will be put in msg. The caller ensures that
 *                         the actual allocated size of msg is at least
 *                         baseSize + marshalledSize bytes (as returned
 *                         by ocrPolicyMsgGetMsgSize).
 *                         msg->usefulSize will be set to marshalledSize + baseSize
 *   - MARSHALL_APPEND:    In this case, (u64)msg == (u64)mainBuffer. In
 *                         most cases, this will only fix-up pointers but
 *                         may copy from addlBuffer into mainBuffer. For
 *                         example, the mashalling could have been
 *                         done using MARSHALL_ADDL and unmarshalling is
 *                         done with MARSHALL_APPEND.
 *                         The caller ensures that the actual allocated
 *                         size of msg is at least baseSize + marshalledSize bytes (as
 *                         returned by ocrPolicyMsgGetMsgSize).
 *                         msg->usefulSize will be set to baseSize + marshalledSize
 *   - MARSHALL_ADDL:      Marshall the common part of the message into msg
 *                         and use pdMalloc for marshalledSize bytes (one
 *                         or more chunks totalling marshalledSize bytes as
 *                         returned by ocrPolicyMsgGetMsgSize). msg->usefulSize will
 *                         be set to baseSize. In this case, msg may or may not be the
 *                         same as mainBuffer
 *
 * @param[in] mainBuffer   Buffer containing the common part of the message
 * @param[in] addlBuffer   Optional addtional buffer (if message was marshalled
 *                         using MARSHALL_ADDL). NULL otherwise (do not pass in
 *                         garbage!). It is assumed to have marshalledSize of useful
 *                         content
 * @param[out] msg         Message to create from mainBuffer and addlBuffer
 * @param[in] mode         One of MARSHALL_FULL_COPY, MARSHALL_APPEND, MARSHALL_ADDL
 *
 * @return 0 on success and a non-zero error code
 */
u8 ocrPolicyMsgUnMarshallMsg(u8* mainBuffer, u8* addlBuffer,
                             struct _ocrPolicyMsg_t* msg, u32 mode);

#ifdef ENABLE_OCR_API_DEFERRABLE
void tagDeferredMsg(ocrPolicyMsg_t * msg, ocrTask_t * task);
#endif

#define __GUID_END_MARKER__
#include "ocr-guid-end.h"
#undef __GUID_END_MARKER__

#endif /* OCR_POLICY_DOMAIN_H_ */

