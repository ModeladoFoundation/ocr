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

// Define this if building the PD builder program
// If this is defined, this will exclude everything
// that does not contribute to building the policy domain
//#define ENABLE_BUILDER_ONLY

#define TG_XE_TARGET

// See bug #556
#define EDT_DEPV_DELAYED

// Allocator
#define ENABLE_ALLOCATOR_SIMPLE
#define ENABLE_ALLOCATOR_QUICK

// CommApi
#define ENABLE_COMM_API_HANDLELESS

// Comm-platform
#define ENABLE_COMM_PLATFORM_XE

// Comp-platform
#define ENABLE_COMP_PLATFORM_FSIM

// Comp-target
#define ENABLE_COMP_TARGET_PASSTHROUGH

// Datablock
#define ENABLE_DATABLOCK_REGULAR
#define ENABLE_DATABLOCK_LOCKABLE

// Event
#define ENABLE_EVENT_HC

// External things (mostly needed by the INI parser)

// GUID provider
#define ENABLE_GUID_PTR
#define ENABLE_GUID_LABELED

// Hints
#define ENABLE_HINTS

// HAL layer to use
#define HAL_FSIM_XE

// Mem-platform
#define ENABLE_MEM_PLATFORM_FSIM

// Mem-target
#define ENABLE_MEM_TARGET_SHARED

// Policy domain
#define ENABLE_POLICY_DOMAIN_XE

// Scheduler
#define ENABLE_SCHEDULER_XE

// Scheduler Heuristic
#define ENABLE_SCHEDULER_HEURISTIC_NULL

// Scheduler Objects
#define ENABLE_SCHEDULER_OBJECT_NULL

// SAL layer to use
#define SAL_FSIM_XE

// XE tool-chain used
#define TOOL_CHAIN_XE

// Sysboot layer to use
#define ENABLE_SYSBOOT_FSIM

// Task
#define ENABLE_TASK_HC

// Task template
#define ENABLE_TASKTEMPLATE_HC

// Worker
#define ENABLE_WORKER_XE

// Workpile
#define ENABLE_WORKPILE_XE

// Extensions
#define ENABLE_EXTENSION_LABELING

// Build OCR legacy support
//#define ENABLE_EXTENSION_LEGACY

// Build pause/resume support
// #define ENABLE_EXTENSION_PAUSE

// Event creation with parameter
#define ENABLE_EXTENSION_PARAMS_EVT

//Counted Events support
#define ENABLE_EXTENSION_COUNTED_EVT

//Channel Events support
#define ENABLE_EXTENSION_CHANNEL_EVT

// Performance monitoring
#define ENABLE_EXTENSION_PERF

#endif /* __OCR_CONFIG_H__ */


