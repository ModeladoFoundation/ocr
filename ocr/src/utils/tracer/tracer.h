#ifndef __TRACER_H__
#define __TRACER_H__

#include "ocr-config.h"
#include "ocr-runtime-types.h"
#include "ocr-types.h"

#include "utils/deque.h"

#ifdef ENABLE_WORKER_SYSTEM

#include <stdarg.h>

#define TRACE_TYPE_NAME(ID) _type_##ID

#define _TRACE_FIELD_FULL(ttype, taction, obj, field) obj->type._type_##ttype.action.taction.field
#define TRACE_FIELD(type, action, traceObj, field) _TRACE_FIELD_FULL(type, action, (traceObj), field)

/*
 * Data structure for trace objects.  Not yet fully populated
 * as not all trace coverage is needed/supported yet.
 *
 */


bool isDequeFull(deque_t *deq);
bool isSystem(ocrPolicyDomain_t *pd);
bool isSupportedTraceType(bool evtType, ocrTraceType_t ttype, ocrTraceAction_t atype);
void populateTraceObject(u64 location, bool evtType, ocrTraceType_t objType, ocrTraceAction_t actionType,
                                u64 workerId, u64 timestamp, ocrGuid_t parent, va_list ap);

//FIXME: Smith-Waterman specific hard coded
//TODO Define these generically

#define MAX_PARAMS 6
#define MAX_DEPS 6
#define INIT_TRACE_OBJECT()                                                 \
                                                                            \
    ocrPolicyDomain_t *pd = NULL;                                           \
    ocrWorker_t *worker = NULL;                                             \
    getCurrentEnv(&pd, &worker, NULL, NULL);                                \
    ocrTraceObj_t *tr = pd->fcts.pdMalloc(pd, sizeof(ocrTraceObj_t));       \
                                                                            \
    tr->typeSwitch = objType;                                               \
    tr->actionSwitch = actionType;                                          \
    tr->workerId = workerId;                                                \
    tr->location = location;                                                \
    tr->time = timestamp;                                                   \
    tr->parent = parent;                                                    \
    tr->eventType = evtType;

#define PUSH_TO_TRACE_DEQUE()                                                                       \
    while(isDequeFull(((ocrWorkerHc_t*)worker)->sysDeque)){                                         \
        hal_pause();                                                                                \
    }                                                                                               \
                                                                                                    \
    ((ocrWorkerHc_t *)worker)->sysDeque->pushAtTail(((ocrWorkerHc_t *)worker)->sysDeque, tr, 0);

//TODO: Add comment descriptions for new trace fields
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
                    ocrGuid_t templateGuid;
                    ocrEdt_t funcPtr;
                }taskTemplateCreate;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of created task*/
                    ocrGuid_t parentID;             /*GUID of parent creating cur EDT*/
                    u32 depc;
                    u32 paramc;
                    u64 paramv[MAX_PARAMS];
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
                    u64 depc;
                    u64 paramc;
                    u64 paramv[MAX_PARAMS];
                }taskExeBegin;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task completing */
                    void *placeHolder;              /* future TODO: define useful fields*/
                    void *edt;
                    u32 count;
                    u64 hwCycles;
                    u64 hwCacheRefs;
                    u64 hwCacheMisses;
                    u64 hwFpOps;
                    u64 swEdtCreates;
                    u64 swDbTotal;
                    u64 swDbCreates;
                    u64 swDbDestroys;
                    u64 swEvtSats;
                }taskExeEnd;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task being destroyed */
                }taskDestroy;

                struct{
                    ocrGuid_t taskGuid;             /* GUID of task shifted */
                    ocrEdt_t funcPtr;               /* Function ptr to function associated with the task */
                    bool shiftFrom;                 /* true if shifting FROM task, false if shifting BACK */
                }taskExeShift;

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
                    ocrGuid_t templateGuid;
                    u32 paramc;
                    u64 paramv[MAX_PARAMS];
                    u32 depc;
                    ocrGuid_t depv[MAX_DEPS];
                }simEdtCreate;

                struct{
                    ocrEdt_t funcPtr;
                    u32 paramc;
                    u32 depc;
                }simEdtTemplateCreate;

            }action;

        } TRACE_TYPE_NAME(API_EDT);

        struct{
            union{
                struct{
                    ocrEventTypes_t eventType;
                }simEventCreate;

                struct{
                    ocrGuid_t eventGuid;
                    ocrGuid_t dataGuid;
                }simEventSatisfy;

                struct{
                    ocrGuid_t source;
                    ocrGuid_t destination;
                    u32 slot;
                    ocrDbAccessMode_t accessMode;
                }simEventAddDep;

            }action;

        } TRACE_TYPE_NAME(API_EVENT);

        struct{
            union{
                struct{
                    u64 len;
                }simDbCreate;

                struct{
                    ocrGuid_t guid;
                }simDbRelease;

                struct{
                    ocrGuid_t guid;
                }simDbDestroy;

            }action;

        } TRACE_TYPE_NAME(API_DATABLOCK);

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
                    ocrLocation_t src;
                    ocrLocation_t dst;
                    u64 usefulSize;
                    u64 marshTime;
                    u64 sendTime;
                    u64 rcvTime;
                    u64 unMarshTime;
                    u64 type;
                }msgEndToEnd;

            }action;

        } TRACE_TYPE_NAME(MESSAGE);


    }type;
}ocrTraceObj_t;

#endif /* ENABLE_WORKER_SYSTEM */
void doTrace(u64 location, u64 wrkr, ocrGuid_t taskGuid, ...);

#endif
