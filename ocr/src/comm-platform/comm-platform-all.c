/*
* This file is subject to the license agreement located in the file LICENSE
* and cannot be distributed without it. This notice cannot be
* removed or modified.
*/

#include "debug.h"
#include "ocr-policy-domain.h"

#include "comm-platform/comm-platform-all.h"

const char * commplatform_types[] = {
#ifdef ENABLE_COMM_PLATFORM_NULL
    "None",
#endif
#ifdef ENABLE_COMM_PLATFORM_CE
    "CE",
#endif
#ifdef ENABLE_COMM_PLATFORM_XE
    "XE",
#endif
#ifdef ENABLE_COMM_PLATFORM_CE_PTHREAD
    "CePthread",
#endif
#ifdef ENABLE_COMM_PLATFORM_XE_PTHREAD
    "XePthread",
#endif
#ifdef ENABLE_COMM_PLATFORM_MPI
    "MPI",
#endif
#ifdef ENABLE_COMM_PLATFORM_MPI_PROBE
    "MPI_PROBE",
#endif
#ifdef ENABLE_COMM_PLATFORM_GASNET
    "GASNet",
#endif
    NULL
};

#define DEBUG_TYPE COMM_PLATFORM

#define STRINGIFY(x) #x
char * pd_type_to_str(int type) {
    switch(type) {
        case PD_MSG_INVAL: return STRINGIFY(PD_MSG_INVAL);
        case PD_MSG_DB_OP: return STRINGIFY(PD_MSG_DB_OP);
        case PD_MSG_DB_CREATE: return STRINGIFY(PD_MSG_DB_CREATE);
        case PD_MSG_DB_DESTROY: return STRINGIFY(PD_MSG_DB_DESTROY);
        case PD_MSG_DB_ACQUIRE: return STRINGIFY(PD_MSG_DB_ACQUIRE);
        case PD_MSG_DB_RELEASE: return STRINGIFY(PD_MSG_DB_RELEASE);
        case PD_MSG_DB_FREE: return STRINGIFY(PD_MSG_DB_FREE);
        case PD_MSG_MEM_OP: return STRINGIFY(PD_MSG_MEM_OP);
        case PD_MSG_MEM_ALLOC: return STRINGIFY(PD_MSG_MEM_ALLOC);
        case PD_MSG_MEM_UNALLOC: return STRINGIFY(PD_MSG_MEM_UNALLOC);
        case PD_MSG_WORK_OP: return STRINGIFY(PD_MSG_WORK_OP);
        case PD_MSG_WORK_CREATE: return STRINGIFY(PD_MSG_WORK_CREATE);
        case PD_MSG_WORK_EXECUTE: return STRINGIFY(PD_MSG_WORK_EXECUTE);
        case PD_MSG_WORK_DESTROY: return STRINGIFY(PD_MSG_WORK_DESTROY);
        case PD_MSG_EDTTEMP_OP: return STRINGIFY(PD_MSG_EDTTEMP_OP);
        case PD_MSG_EDTTEMP_CREATE: return STRINGIFY(PD_MSG_EDTTEMP_CREATE);
        case PD_MSG_EDTTEMP_DESTROY: return STRINGIFY(PD_MSG_EDTTEMP_DESTROY);
        case PD_MSG_EVT_OP: return STRINGIFY(PD_MSG_EVT_OP);
        case PD_MSG_EVT_CREATE: return STRINGIFY(PD_MSG_EVT_CREATE);
        case PD_MSG_EVT_DESTROY: return STRINGIFY(PD_MSG_EVT_DESTROY);
        case PD_MSG_EVT_GET: return STRINGIFY(PD_MSG_EVT_GET);
        case PD_MSG_GUID_OP: return STRINGIFY(PD_MSG_GUID_OP);
        case PD_MSG_GUID_CREATE: return STRINGIFY(PD_MSG_GUID_CREATE);
        case PD_MSG_GUID_INFO: return STRINGIFY(PD_MSG_GUID_INFO);
        case PD_MSG_GUID_METADATA_CLONE: return STRINGIFY(PD_MSG_GUID_METADATA_CLONE);
        case PD_MSG_GUID_RESERVE: return STRINGIFY(PD_MSG_GUID_RESERVE);
        case PD_MSG_GUID_UNRESERVE: return STRINGIFY(PD_MSG_GUID_UNRESERVE);
        case PD_MSG_GUID_DESTROY: return STRINGIFY(PD_MSG_GUID_DESTROY);
        case PD_MSG_METADATA_COMM: return STRINGIFY(PD_MSG_METADATA_COMM);
        case PD_MSG_SCHED_OP: return STRINGIFY(PD_MSG_SCHED_OP);
        case PD_MSG_SCHED_GET_WORK: return STRINGIFY(PD_MSG_SCHED_GET_WORK);
        case PD_MSG_SCHED_NOTIFY: return STRINGIFY(PD_MSG_SCHED_NOTIFY);
        case PD_MSG_SCHED_TRANSACT: return STRINGIFY(PD_MSG_SCHED_TRANSACT);
        case PD_MSG_SCHED_ANALYZE: return STRINGIFY(PD_MSG_SCHED_ANALYZE);
        case PD_MSG_SCHED_UPDATE: return STRINGIFY(PD_MSG_SCHED_UPDATE);
        case PD_MSG_COMM_TAKE: return STRINGIFY(PD_MSG_COMM_TAKE);
        case PD_MSG_COMM_GIVE: return STRINGIFY(PD_MSG_COMM_GIVE);
        case PD_MSG_DEP_OP: return STRINGIFY(PD_MSG_DEP_OP);
        case PD_MSG_DEP_ADD: return STRINGIFY(PD_MSG_DEP_ADD);
        case PD_MSG_DEP_REGSIGNALER: return STRINGIFY(PD_MSG_DEP_REGSIGNALER);
        case PD_MSG_DEP_REGWAITER: return STRINGIFY(PD_MSG_DEP_REGWAITER);
        case PD_MSG_DEP_SATISFY: return STRINGIFY(PD_MSG_DEP_SATISFY);
        case PD_MSG_DEP_UNREGSIGNALER: return STRINGIFY(PD_MSG_DEP_UNREGSIGNALER);
        case PD_MSG_DEP_UNREGWAITER: return STRINGIFY(PD_MSG_DEP_UNREGWAITER);
        case PD_MSG_DEP_DYNADD: return STRINGIFY(PD_MSG_DEP_DYNADD);
        case PD_MSG_DEP_DYNREMOVE: return STRINGIFY(PD_MSG_DEP_DYNREMOVE);
        case PD_MSG_SAL_OP: return STRINGIFY(PD_MSG_SAL_OP);
        case PD_MSG_SAL_PRINT: return STRINGIFY(PD_MSG_SAL_PRINT);
        case PD_MSG_SAL_READ: return STRINGIFY(PD_MSG_SAL_READ);
        case PD_MSG_SAL_WRITE: return STRINGIFY(PD_MSG_SAL_WRITE);
        case PD_MSG_SAL_TERMINATE: return STRINGIFY(PD_MSG_SAL_TERMINATE);
        case PD_MSG_MGT_OP: return STRINGIFY(PD_MSG_MGT_OP);
        case PD_MSG_MGT_REGISTER: return STRINGIFY(PD_MSG_MGT_REGISTER);
        case PD_MSG_MGT_UNREGISTER: return STRINGIFY(PD_MSG_MGT_UNREGISTER);
        case PD_MSG_MGT_MONITOR_PROGRESS: return STRINGIFY(PD_MSG_MGT_MONITOR_PROGRESS);
        case PD_MSG_MGT_RL_NOTIFY: return STRINGIFY(PD_MSG_MGT_RL_NOTIFY);
        case PD_MSG_HINT_OP: return STRINGIFY(PD_MSG_HINT_OP);
        case PD_MSG_HINT_SET: return STRINGIFY(PD_MSG_HINT_SET);
        case PD_MSG_HINT_GET: return STRINGIFY(PD_MSG_HINT_GET);
        case PD_MSG_RESILIENCY_OP: return STRINGIFY(PD_MSG_RESILIENCY_OP);
        case PD_MSG_RESILIENCY_NOTIFY: return STRINGIFY(PD_MSG_RESILIENCY_NOTIFY);
        case PD_MSG_RESILIENCY_MONITOR: return STRINGIFY(PD_MSG_RESILIENCY_MONITOR);
        case PD_MSG_RESILIENCY_CHECKPOINT: return STRINGIFY(PD_MSG_RESILIENCY_CHECKPOINT);
        case PD_MSG_REQUEST: return STRINGIFY(PD_MSG_REQUEST);
        case PD_MSG_RESPONSE: return STRINGIFY(PD_MSG_RESPONSE);
        case PD_MSG_REQ_RESPONSE: return STRINGIFY(PD_MSG_REQ_RESPONSE);
        case PD_MSG_RESPONSE_OVERRIDE: return STRINGIFY(PD_MSG_RESPONSE_OVERRIDE);
        case PD_MSG_LOCAL_PROCESS: return STRINGIFY(PD_MSG_LOCAL_PROCESS);
        case PD_MSG_DEFERRABLE: return STRINGIFY(PD_MSG_DEFERRABLE);
        default: return "ERROR";
    }
}

ocrCommPlatformFactory_t *newCommPlatformFactory(commPlatformType_t type, ocrParamList_t *typeArg) {
    switch(type) {
#ifdef ENABLE_COMM_PLATFORM_NULL
    case commPlatformNull_id:
        return newCommPlatformFactoryNull(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_CE
    case commPlatformCe_id:
        return newCommPlatformFactoryCe(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_XE
    case commPlatformXe_id:
        return newCommPlatformFactoryXe(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_CE_PTHREAD
    case commPlatformCePthread_id:
        return newCommPlatformFactoryCePthread(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_XE_PTHREAD
    case commPlatformXePthread_id:
        return newCommPlatformFactoryXePthread(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_MPI
    case commPlatformMPI_id:
        return newCommPlatformFactoryMPI(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_MPI_PROBE
    case commPlatformMPIProbe_id:
        return newCommPlatformFactoryMPIProbe(typeArg);
#endif
#ifdef ENABLE_COMM_PLATFORM_GASNET
    case commPlatformGasnet_id:
        return newCommPlatformFactoryGasnet(typeArg);
#endif
    default:
        ASSERT(0);
        return NULL;
    };
}

void initializeCommPlatformOcr(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * self, ocrParamList_t *perInstance) {
    self->location = ((paramListCommPlatformInst_t *)perInstance)->location;
    self->fcts = factory->platformFcts;
}
