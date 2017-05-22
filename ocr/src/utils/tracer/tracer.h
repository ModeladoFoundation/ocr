#ifndef __TRACER_H__
#define __TRACER_H__

#include "ocr-config.h"
#include "ocr-runtime-types.h"
#include "ocr-types.h"
#include "ocr-perfmon.h"

#include "utils/deque.h"

#ifdef ENABLE_WORKER_SYSTEM

#include <stdarg.h>

#define TRACE_TYPE_NAME(ID) _type_##ID
#define _TRACE_FIELD_FULL(ttype, taction, obj, field) obj->type._type_##ttype.action.taction.field
#define TRACE_FIELD(type, action, traceObj, field) _TRACE_FIELD_FULL(type, action, (traceObj), field)

#ifndef MAX_PARAMS
#define MAX_PARAMS 32
#endif

#ifndef MAX_DEPS
#define MAX_DEPS 32
#endif

bool isDequeFull(deque_t *deq);
bool isSystem(ocrPolicyDomain_t *pd);
bool isSupportedTraceType(bool evtType, ocrTraceType_t ttype, ocrTraceAction_t atype);
void populateTraceObject(u64 location, bool evtType, ocrTraceType_t objType, ocrTraceAction_t actionType,
                                u64 workerId, u64 timestamp, ocrGuid_t parent, va_list ap);


extern __thread bool inside_trace;

/* Macros to condense and simplify the packing of trace objects */
#define INIT_TRACE_OBJECT()                                                 \
                                                                            \
    ocrPolicyDomain_t *pd = NULL;                                           \
    ocrWorker_t *worker = NULL;                                             \
    getCurrentEnv(&pd, &worker, NULL, NULL);                                \
    inside_trace = true;                                                    \
    ocrTraceObj_t *tr = pd->fcts.pdMalloc(pd, sizeof(ocrTraceObj_t));       \
    inside_trace = false;                                                   \
    ADebug(AllocDebugAllPD, "tracer.h/INIT_TRACE_OBJECT: pdMalloc(%ld), "   \
           "addr=%p\n", sizeof(ocrTraceObj_t), tr);                         \
                                                                            \
    tr->typeSwitch = objType;                                               \
    tr->actionSwitch = actionType;                                          \
    tr->workerId = workerId;                                                \
    tr->location = location;                                                \
    tr->time = timestamp;                                                   \
    tr->parent = parent;                                                    \
    tr->eventType = evtType;

#define PUSH_TO_TRACE_DEQUE()                                                                           \
    if(worker != NULL){                                                                                 \
        while(isDequeFull(((ocrWorkerHc_t*)worker)->sysDeque)){                                         \
            hal_pause();                                                                                \
        }                                                                                               \
                                                                                                        \
        ((ocrWorkerHc_t *)worker)->sysDeque->pushAtTail(((ocrWorkerHc_t *)worker)->sysDeque, tr, 0);    \
    }

//TODO: Add comment descriptions for new trace fields

/*
 * Data structure for trace objects.
 */
typedef struct {

    ocrTraceType_t  typeSwitch;
    ocrTraceAction_t actionSwitch;

    u64 time;               /* Timestamp for event*/
    u64 workerId;           /* Worker where event occured*/
    u64 location;           /* PD where event occured*/
    ocrGuid_t parent;       /* GUID of parent task where trace action took place*/
    bool eventType;         /* TODO: make this more descriptive than bool*/
    unsigned char **blob;   /* TODO: Carry generic blob*/

    union{ /*type*/

        struct{ /* Task (EDT) */
            union{
                struct{
                    ocrGuid_t templateGuid;         /* GUID of EDT template */
                    ocrEdt_t funcPtr;               /* function pointer associated with EDT template */
                }taskTemplateCreate;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of created task*/
                    ocrGuid_t parentID;             /* GUID of parent creating cur EDT*/
                    u32 depc;                       /* Number of dependencies associated with task */
                    u32 paramc;                     /* Number of paramaters associated with task */
                    u64 paramv[MAX_PARAMS];         /* List of paramaters associated with task */
                }taskCreate;

                struct{
                    ocrGuid_t src;                  /* Source GUID of dependence being added */
                    ocrGuid_t dest;                 /* Destination GUID of dependence being added*/
                }taskDepReady;

                struct{
                    ocrGuid_t taskGuid;             /* Guid of task being satisfied */
                    ocrGuid_t satisfyee;            /* Guid of object satisfying the dependecy */
                }taskDepSatisfy;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of runnable task */
                }taskReadyToRun;

                 struct{
                    ocrGuid_t taskGuid;             /* GUID of scheduled task */
                    deque_t *deq;
                }taskScheduled;

                struct{
                    u32 whyDelay;                   /* TODO: define this... may not be needed/useful */
                    ocrGuid_t taskGuid;             /* GUID of task executing */
                    ocrEdt_t funcPtr;               /* Function ptr to current function associated with the task */
                    u64 depc;                       /* Number of dependencies associated with task */
                    u64 paramc;                     /* Number of paramaters associated with task */
                    u64 paramv[MAX_PARAMS];         /* List of paramaters associated with task */
                }taskExeBegin;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task completing */
                    void *edt;                      /* Pointer to EDT */
                    u32 count;                      /* Number of times this EDT has executed */
                    u64 hwCycles;                   /* Perf counter: total clock cycles */
                    u64 hwCacheRefs;                /* Perf counter: L1 hits */
                    u64 hwCacheMisses;              /* Perf counter: L1 misses */
                    u64 hwFpOps;                    /* Perf counter: Floating pointer operations */
                    u64 swEdtCreates;               /* Soft counter: Number of ocrEdtCreate calls */
                    u64 swDbTotal;                  /* Soft counter: Total memory footprint of datablocks */
                    u64 swDbCreates;                /* Soft counter: Number of ocrDbCreate calls */
                    u64 swDbDestroys;               /* Soft counter: Number if ocrDbDestroy calls */
                    u64 swEvtSats;                  /* Soft counter: Number of ocrEventSatisfy calls */
                }taskExeEnd;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task being destroyed */
                }taskDestroy;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task acquiring the datablock */
                    ocrGuid_t dbGuid;               /* GUID of datablock being acquired */
                    u64 dbSize;                     /* Size of Datablock being acquired */
                }taskDataAcquire;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task releasing the datablock */
                    ocrGuid_t dbGuid;               /* GUID of datablock being released */
                    u64 dbSize;                     /* Size of Datablock being released */
                }taskDataRelease;

            }action;

        } TRACE_TYPE_NAME(TASK);

        struct{ /* Data (DB) */
            union{
                struct{
                    ocrGuid_t dbGuid;               /* GUID of datablock being created */
                    u64 dbSize;                     /* Size of DB in bytes */
                }dataCreate;

                struct{
                    void *memID;                    /* TODO: define type for memory ID */
                }dataSize;

                struct{
                    ocrLocation_t src;              /* Data source location */
                }dataMoveFrom;

                struct{
                    ocrLocation_t dest;             /* Data destination location */
                }dataMoveTo;

                struct{
                    ocrGuid_t duplicateID;          /* GUID of new DB when copied */
                }dataReplicate;

                struct{
                    ocrGuid_t dbGuid;               /* GUID of datablock being destroyed */
                }dataDestroy;

            }action;

        } TRACE_TYPE_NAME(DATA);

        struct{ /* Allocator */
            union{
                struct{
                    u64 startTime;                  /* Time when allocation started */
                    u64 callFunc;                   /* Identifier of function calling allocate */
                    u64 memSize;                    /* Size of memory in bytes */
                    u64 memHint;                    /* Hint for allocator */
                    void *memPtr;                   /* Pointer to memory allocated */
                }memAlloc;

                struct{
                    u64 startTime;                  /* Time when deallocation started */
                    u64 callFunc;                   /* Identifier of function calling allocate */
                    void *memPtr;                   /* Pointer to memory allocated */
                }memDealloc;

            }action;

        } TRACE_TYPE_NAME(ALLOCATOR);

        struct{ /* Event (OCR module) */
            union{
                struct{
                    ocrGuid_t eventGuid;            /* GUID of event being created */
                }eventCreate;

                struct{
                    ocrGuid_t src;                  /* Source GUID of dependence being added */
                    ocrGuid_t dest;                 /* Destination GUID of dependence being added */
                }eventDepAdd;

                struct{
                    ocrGuid_t eventGuid;            /* GUID of event being satisfied */
                    ocrGuid_t satisfyee;            /* GUID of object satisfying the dependence */
                }eventDepSatisfy;

                struct{
                    void *placeHolder;              /* TODO: Define values.  What trigger? */
                }eventTrigger;

                struct{
                    ocrGuid_t eventGuid;            /* GUID of event being destroyed */
                }eventDestroy;

            }action;

        } TRACE_TYPE_NAME(EVENT);

        struct{ /* Execution Unit (workers) */
            union{
                struct{
                    ocrLocation_t location;         /* Location worker belongs to (PD) */
                }exeUnitStart;

                struct{
                    ocrLocation_t location;         /* Location after work shift */
                }exeUnitMigrate;

                struct{
                    void *placeHolder;              /* TODO: Define values.  May not be needed */
                }exeUnitDestroy;

                struct{
                    void *placeHolder;              /* TODO: Define values.  May not be needed*/
                }exeWorkRequest;

                struct{
                    ocrGuid_t foundGuid;            /* GUID of EDT popped from deque for execution*/
                    deque_t *deq;
                }exeWorkTaken;


            }action;

        } TRACE_TYPE_NAME(EXECUTION_UNIT);

        struct{ /* OCR scheduler module */
            union{
                struct{
                    ocrGuid_t taskGuid;             /* GUID of task en route to being scheduled */
                }schedMsgSend;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task en route to being scheduled */
                }schedMsgRcv;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task en route to being scheduled */
                }schedInvoke;

            }action;
        } TRACE_TYPE_NAME(SCHEDULER);

        struct{
            union{
                struct{
                    ocrGuid_t templateGuid;         /* GUID of template on API entry */
                    u32 paramc;                     /* Number of params associated with EDT on API entry */
                    u64 paramv[MAX_PARAMS];         /* List of params associated with EDT on API entry */
                    u32 depc;                       /* Number of dependencies associated with EDT on API entry */
                    ocrGuid_t depv[MAX_DEPS];       /* List of dependencies associated with EDT on API entry */
                }simEdtCreate;

                struct{
                    ocrEdt_t funcPtr;               /* Function pointer associated with task template on API entry */
                    u32 paramc;                     /* Number of paramaters associated with task template on API entry */
                    u32 depc;                       /* Number of dependencies associated with task template on API entry */
                }simEdtTemplateCreate;

            }action;

        } TRACE_TYPE_NAME(API_EDT);

        struct{
            union{
                struct{
                    ocrEventTypes_t eventType;      /* Type of event bieng created on API entry */
                }simEventCreate;

                struct{
                    ocrGuid_t eventGuid;            /* GUID of the event bieng satisfied on API entry */
                    ocrGuid_t dataGuid;             /* GUID of object satisfying the event on API entry */
                }simEventSatisfy;

                struct{
                    ocrGuid_t source;                   /* OCR object having dependence added on API entry */
                    ocrGuid_t destination;              /* OCR object bieng depended on by source on API entry */
                    u32 slot;                           /* Slot number of event where dependence being added on API entry */
                    ocrDbAccessMode_t accessMode;       /* Access mode of associated dependence on API entry */
                }simEventAddDep;

                struct{
                    ocrGuid_t eventGuid;            /* GUID of event being destroyed on API entry */
                }simEventDestroy;

            }action;

        } TRACE_TYPE_NAME(API_EVENT);

        struct{
            union{
                struct{
                    u64 len;                        /* Size of datablock being created on API entry */
                }simDbCreate;

                struct{
                    ocrGuid_t guid;                 /* GUID of datablock being released on API entry */
                }simDbRelease;

                struct{
                    ocrGuid_t guid;                 /* GUID of datablock being destroyed on API entry */
                }simDbDestroy;

            }action;

        } TRACE_TYPE_NAME(API_DATABLOCK);


        struct{
            union{
                struct{
                    void *placeHolder;              /* TODO: define necessary field if simulation of Affinity calls becomes necessary */
                }simAffinityGetCurrent;

                struct{
                    void *placeHolder;              /* TODO: define necessary field if simulation of Affinity calls becomes necessary */
                }simAffinityGetAt;

                struct{
                    void *placeHolder;              /* TODO: define necessary field if simulation of Affinity calls becomes necessary */
                }simAffinityGetCount;

                struct{
                    void *placeHolder;              /* TODO: define necessary field if simulation of Affinity calls becomes necessary */
                }simAffinityQuery;

            }action;

        } TRACE_TYPE_NAME(API_AFFINITY);


        struct{
            union{
                struct{
                    void *placeHolder;              /* TODO: define necessary field if simulation of hint calls becomes necessary */
                }simHintInit;

                struct{
                    void *placeHolder;              /* TODO: define necessary field if simulation of hint calls becomes necessary */
                }simHintSetValue;

            }action;

        } TRACE_TYPE_NAME(API_HINT);


//        struct{
//            union{
//                void *placeHolder;
//            }action;
//
//        } TRACE_TYPE_NAME(API_GUID);


        struct{ /* User-facing custom marker */
            union{
                struct{
                    void *placeHolder;              /* TODO: Define user facing options */
                }userMarkerFlags;

            }action;

        } TRACE_TYPE_NAME(USER_MARKER);


        struct{ /* Runtime facing custom Marker */
            union{
                struct{
                    void *placeHolder;              /* TODO: Define runtime options */
                }runtimeMarkerFlags;

            }action;

        } TRACE_TYPE_NAME(RUNTIME_MARKER);

        struct{
            union{
                struct{
                    ocrLocation_t src;              /* Source policy domain of message */
                    ocrLocation_t dst;              /* Destination policy domain of message */
                    u64 usefulSize;                 /* Size of message contents */
                    u64 marshTime;                  /* Timestamp when message was marshalled */
                    u64 sendTime;                   /* Timestamp when message was sent */
                    u64 rcvTime;                    /* Timestamp when message was received */
                    u64 unMarshTime;                /* Timestamp when message was unmarshalled */
                    u64 type;                       /* Type of message */
                }msgEndToEnd;

            }action;

        } TRACE_TYPE_NAME(MESSAGE);


    }type;
}ocrTraceObj_t;

#endif /* ENABLE_WORKER_SYSTEM */
void doTrace(u64 location, u64 wrkr, ocrGuid_t taskGuid, ...);

#endif

