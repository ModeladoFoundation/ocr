/* Copyright (c) 2012, Rice University

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1.  Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   2.  Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided
   with the distribution.
   3.  Neither the name of Intel Corporation
   nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific
   prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#ifndef __OCR_TASK_EVENT_H_
#define __OCR_TASK_EVENT_H_

#include "ocr-guid.h"
#include "ocr-edt.h"


/*******************************************
 * Dependence Registration
 ******************************************/
void registerDependence(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot);

/*******************************************
 *          OCR Event declarations
 ******************************************/

/*
 * Event's function pointers typedef
 */
struct ocr_event_struct;
typedef void (*event_destruct_fct)(struct ocr_event_struct* event);
typedef ocrGuid_t (*event_get_fct)(struct ocr_event_struct* event);
typedef void (*event_satisfy_fct)(struct ocr_event_struct* event, ocrGuid_t db, int slot);

/*! \brief Abstract class to represent OCR events function pointers
 *
 *  This class provides the interface for the underlying implementation to conform.
 */
typedef struct ocr_event_fcts_struct {
    /*! \brief Virtual destructor for the Event interface
     */
    event_destruct_fct destruct;

    /*! \brief Interface to get the GUID of the entity that satisfied an event.
     *  \return GUID of the entity that satisfied this event
     */
    event_get_fct get;

    /*! \brief Interface to satisfy the event
     *  \param[in]  db  GUID to satisfy this event
     *  \param[in]  w   GUID of the Worker instance satisfying this event
     */
    event_satisfy_fct satisfy;
} ocr_event_fcts_t;

/*! \brief Abstract class to represent OCR events.
 *
 *  This class provides the interface for the underlying implementation to conform.
 *  Events can be satisfied once with a GUID, can be polled for what GUID satisfied the event,
 *  and can be registered to by a task.
 */
typedef struct ocr_event_struct {
    ocrGuid_t guid; /**< GUID for this event */
    /*! \brief Holds function pointer to the event interface
     */
    ocr_event_fcts_t * fct_ptrs;
} ocr_event_t;


/*******************************************
 *       OCR Event Lists declarations
 ******************************************/

/*! \brief EventList linked list's node constructor
 *  \param[in]  pEvent  Event pointer to wrap to a linked list node
 *  \return A Node instance wrapping an Event to a linked list node
 */
typedef struct event_list_node_struct_t {
    /*! Public Event member of the linked list node wrapper */
    ocr_event_t *event;
    /*! Public next Event member of the linked list node wrapper */
    struct event_list_node_struct_t *next;
} event_list_node_t;

/*
 * Event list's function pointers typedef
 */
struct event_list_struct_t;
typedef void (*event_list_enlist_fct)( struct event_list_struct_t* list, ocrGuid_t event_guid );

/*! \brief A linked list data structure to list OCR events.
 *
 *  The runtime implementer or the end user may utilize this class to build a dynamic
 *  linked list of concrete Event implementation GUIDs to create dependence lists for a Task.
 */
typedef struct event_list_struct_t {
    /*! \brief Append an Event guid to the linked list
     *  \param[in]  event_guid  GUID of the event to be appended
     *
     *  This function extracts the Event object from the given guid, creates a linked list node
     *  wrapper by a Node instance, adds the node object to the tail of the linked list, and
     *  increments the size
     */
    event_list_enlist_fct enlist;
    /*! Public member to denote the current size of the linked list object */
    size_t size;
    /*! Public head and tail members of the linked list */
    event_list_node_t *head, *tail;
} event_list_t;

/*! \brief Default constructor for event list
 * Initializes an empty list
 */
event_list_t* event_list_constructor ();

/*! \brief Virtual destructor for the EventList linked list data structure
 *  \param[inout] list The list to be freed (the 'list' pointer is invalid after this call)
 */
void event_list_destructor ( event_list_t* list );

/*
 * OCR event factories declarations
 */
struct ocr_event_factory_struct;
typedef ocrGuid_t (*event_fact_create_fct) (struct ocr_event_factory_struct* factory, ocrEventTypes_t eventType, bool takesArg);
typedef void (*event_fact_destruct_fct)(struct ocr_event_factory_struct* factory);

/*! \brief Abstract factory class to create OCR events.
 *
 *  This class provides an interface to create Event instances with a non-static create function
 *  to allow runtime implementers to choose to have state in their derived ocr_event_factory classes.
 */
typedef struct ocr_event_factory_struct {
    /*! \brief Creates an concrete implementation of an Event and returns its corresponding GUID
     *  \return GUID of the concrete Event that is created by this call
     */
    event_fact_create_fct create;

    /*! \brief Virtual destructor for the ocr_event_factory interface
     */
    event_fact_destruct_fct destruct;

    ocr_event_fcts_t * event_fct_ptrs_single;
    ocr_event_fcts_t * event_fct_ptrs_latch;
} ocr_event_factory;


/*******************************************
 *        OCR Tasks declarations
 ******************************************/

/*
 * OCR tasks factories declarations
 */
struct ocr_task_factory_struct;
typedef ocrGuid_t (*task_fact_create_fct) ( struct ocr_task_factory_struct* factory, ocrEdt_t fctPtr, u32 paramc, u64 * params, void** paramv, u16 properties, size_t l_size, ocrGuid_t * outputEvent);
typedef void (*task_fact_destruct_fct)(struct ocr_task_factory_struct* factory);

// Fwd declaration
struct ocr_task_fcts_struct_t;

/*! \brief Abstract factory class to create OCR tasks.
 *
 *  This class provides an interface to create Task instances with a non-static create function
 *  to allow runtime implementers to choose to have state in their derived TaskFactory classes.
 */
typedef struct ocr_task_factory_struct {
    /*! \brief Creates an concrete implementation of a Task and returns its corresponding GUID
     *  \param[in]  routine A user defined function that represents the computation this Task encapsulates.
     *  \param[in]  worker_id   The Worker instance creating this Task instance
     *  \return GUID of the concrete Task that is created by this call
     *
     *  The signature of the interface restricts the user computation that can be assigned to a task as follows.
     *  The user defined computation should take a vector of GUIDs and its size as their inputs, which may be
     *  the GUIDs used to satisfy the Events enlisted in the dependence list.
     *
     */
    task_fact_create_fct create;

    /*! \brief Virtual destructor for the TaskFactory interface
     */
    task_fact_destruct_fct destruct;

    struct ocr_task_fcts_struct_t * task_fct_ptrs;
} ocr_task_factory;

/*
 * OCR tasks function pointers typedefs
 */
struct ocr_task_struct_t;
typedef void (*task_destruct_fct) ( struct ocr_task_struct_t* base );
typedef void (*task_execute_fct) ( struct ocr_task_struct_t* base );
typedef void (*task_schedule_fct) ( struct ocr_task_struct_t* base, ocrGuid_t wid );

/*! \brief Abstract class to represent OCR tasks function pointers
 *
 *  This class provides the interface to call operations on task
 */
typedef struct ocr_task_fcts_struct_t {
    /*! \brief Virtual destructor for the Task interface
     */
    task_destruct_fct destruct;
    /*! \brief Interface to execute the underlying computation of a task
     */
    task_execute_fct execute;
    task_schedule_fct schedule;
} ocr_task_fcts_t;


// ELS runtime size is one to support finish-edt
// ELS_USER_SIZE is defined by configure
#define ELS_RUNTIME_SIZE 1
#define ELS_SIZE (ELS_RUNTIME_SIZE + ELS_USER_SIZE)

/*! \brief Abstract class to represent OCR tasks.
 *
 *  This class provides the interface for the underlying implementation to conform.
 *  OCR tasks can be executed and can have their synchronization frontier furthered by Events.
 */
typedef struct ocr_task_struct_t {
    ocrGuid_t guid; /**< GUID for this task (EDT) */
    u32 paramc;
    u64 * params;
    void ** paramv;
    ocrGuid_t outputEvent; // Event to notify when the EDT is done
    ocrGuid_t els[ELS_SIZE];
    /*! \brief Holds function pointer to the task interface
     */
    ocr_task_fcts_t * fct_ptrs;
} ocr_task_t;

#endif /* __OCR_TASK_EVENT_H_ */
