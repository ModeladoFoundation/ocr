/**
 * @brief Configuration for the OCR runtime
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_CONFIG_H__
#define __OCR_CONFIG_H__

// Constants used in the runtime

// Define this if building the PD builder program
// If this is defined, this will exclude everything
// that does not contribute to building the policy domain
//#define ENABLE_BUILDER_ONLY

// Allocator
#define ENABLE_ALLOCATOR_TLSF
#define ENABLE_ALLOCATOR_SIMPLE
#define ENABLE_ALLOCATOR_QUICK
#define ENABLE_ALLOCATOR_MALLOCPROXY

// Comm-api
#define ENABLE_COMM_API_DELEGATE
#define ENABLE_COMM_API_SIMPLE

// Comm-platform
#define ENABLE_COMM_PLATFORM_NULL
#define ENABLE_COMM_PLATFORM_MPI
#define ENABLE_COMM_PLATFORM_MPI_PROBE

// Comp-platform
#define ENABLE_COMP_PLATFORM_PTHREAD

// Comp-target
#define ENABLE_COMP_TARGET_PASSTHROUGH

// Datablock
#define ENABLE_DATABLOCK_REGULAR
#define ENABLE_DATABLOCK_LOCKABLE

// Event
#define ENABLE_EVENT_HC

// External things (mostly needed by the INI parser)
#define ENABLE_EXTERNAL_DICTIONARY
#define ENABLE_EXTERNAL_INIPARSER

// GUID provider
#define ENABLE_GUID_PTR
#define ENABLE_GUID_COUNTED_MAP
#define ENABLE_GUID_LABELED

// Hints
#define ENABLE_HINTS

// HAL layer to use
#define HAL_X86_64

// SAL layer to use
#define SAL_LINUX

// Mem-platform
#define ENABLE_MEM_PLATFORM_MALLOC

// Mem-target
#define ENABLE_MEM_TARGET_SHARED

// Policy domain
#define ENABLE_POLICY_DOMAIN_HC
#define ENABLE_POLICY_DOMAIN_HC_DIST

// Scheduler
#define ENABLE_SCHEDULER_HC
#define ENABLE_SCHEDULER_HC_COMM_DELEGATE
#define ENABLE_SCHEDULER_BLOCKING_SUPPORT
#define ENABLE_SCHEDULER_COMMON

// Scheduler Heuristic
#define ENABLE_SCHEDULER_HEURISTIC_NULL
#define ENABLE_SCHEDULER_HEURISTIC_HC
#define ENABLE_SCHEDULER_HEURISTIC_HC_COMM_DELEGATE
#define ENABLE_SCHEDULER_HEURISTIC_PLACEMENT_AFFINITY
#define ENABLE_SCHEDULER_HEURISTIC_ST
#define ENABLE_SCHEDULER_HEURISTIC_PRIORITY
#define ENABLE_SCHEDULER_HEURISTIC_STATIC

// Scheduler Objects
#define ENABLE_SCHEDULER_OBJECT_NULL
#define ENABLE_SCHEDULER_OBJECT_WST
#define ENABLE_SCHEDULER_OBJECT_DEQ
#define ENABLE_SCHEDULER_OBJECT_LIST
#define ENABLE_SCHEDULER_OBJECT_MAP
#define ENABLE_SCHEDULER_OBJECT_PDSPACE
#define ENABLE_SCHEDULER_OBJECT_DBSPACE
#define ENABLE_SCHEDULER_OBJECT_DBTIME
#define ENABLE_SCHEDULER_OBJECT_PR_WSH
#define ENABLE_SCHEDULER_OBJECT_BIN_HEAP

// Support for MPIlite blocking operations
#ifndef DISABLE_EXTENSION_BLOCKING_SUPPORT
#define ENABLE_EXTENSION_BLOCKING_SUPPORT
#endif

// Sysboot layer to use
#define ENABLE_SYSBOOT_LINUX

// Task
#define ENABLE_TASK_HC

// Task template
#define ENABLE_TASKTEMPLATE_HC

// Worker
#define ENABLE_WORKER_HC
#define ENABLE_WORKER_HC_COMM
#define ENABLE_WORKER_SYSTEM
#define ENABLE_WORKER_HC_COMM_MT

// Workpile
#define ENABLE_WORKPILE_HC

// Event creation with parameter
#define ENABLE_EXTENSION_PARAMS_EVT

// Counted Events support
#define ENABLE_EXTENSION_COUNTED_EVT

// Channel Events support
#define ENABLE_EXTENSION_CHANNEL_EVT

// OCR legacy support
#define ENABLE_EXTENSION_LEGACY

// Affinity support
#define ENABLE_EXTENSION_AFFINITY

//GUID Labeling
#define ENABLE_EXTENSION_LABELING

// Build pause support
//#define ENABLE_EXTENSION_PAUSE

// Runtime extension support
#define ENABLE_EXTENSION_RTITF

// Performance monitoring
//#define ENABLE_EXTENSION_PERF

// Resiliency
#define ENABLE_RESILIENCY

// FLags enabled for resiliency on x86-mpi
#ifdef ENABLE_RESILIENCY
#define ENABLE_RESILIENCY_CHECKPOINT_RESTART
#define ENABLE_SCHEDULER_RUNTIME_OBJECT_MGMT
//#define ENABLE_CHECKPOINT_VERIFICATION
//#define OCR_CHECKPOINT_INTERVAL   1000000000UL /* 1 second */
//#define OCR_CHECKPOINT_INTERVAL     10000000UL /* 10 miliseconds */
#endif /* Resiliency flags */

#endif /* __OCR_CONFIG_H__ */

