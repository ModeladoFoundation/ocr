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

#ifndef __OCR_EVENT_H__
#define __OCR_EVENT_H__

#include "ocr-guid.h"
#include "ocr-edt.h"
#include "ocr-utils.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

/*******************************************
 * Dependence Registration
 ******************************************/
void registerDependence(ocrGuid_t signalerGuid, ocrGuid_t waiterGuid, int slot);

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/
typedef struct _paramListEventFact_t {
    ocrParamList_t base;
} paramListEventFact_t;

/****************************************************/
/* OCR EVENT                                        */
/****************************************************/

typedef struct _ocrEvent_t ocrEvent_t;
/*! \brief Abstract class to represent OCR events function pointers
 *
 *  This class provides the interface for the underlying implementation to conform.
 */
typedef struct _ocrEventFcts_t {

    /*! \brief Virtual destructor for the Event interface
     */
    void (*destruct)(ocrEvent_t* self);

    /*! \brief Interface to get the GUID of the entity that satisfied an event.
     *  \return GUID of the entity that satisfied this event
     */
    ocrGuid_t (*get) (ocrEvent_t* self, u32 slot);

    /*! \brief Interface to satisfy the event
     *  \param[in]  db   GUID to satisfy this event with (or NULL_GUID)
     *  \param[in]  slot Input slot for this event (for latch events for example)
     */
    void (*satisfy)(ocrEvent_t* self, ocrGuid_t db, u32 slot);
} ocrEventFcts_t;

/*! \brief Abstract class to represent OCR events.
 *
 *  This class provides the interface for the underlying implementation to conform.
 *  Events can be satisfied once with a GUID, can be polled for what GUID satisfied the event,
 *  and can be registered to by a task.
 */
typedef struct _ocrEvent_t {
    ocrGuid_t guid; /**< GUID for this event */
    #ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcess_t statProcess;
    #endif
    /*! \brief Holds function pointer to the event interface
     */
    ocrEventFcts_t *fctPtrs;
} ocrEvent_t;


/****************************************************/
/* OCR EVENT FACTORY                                */
/****************************************************/

/*! \brief Abstract factory class to create OCR events.
 *
 *  This class provides an interface to create Event instances with a non-static create function
 *  to allow runtime implementers to choose to have state in their derived ocrEventFactory_t classes.
 */
typedef struct _ocrEventFactory_t {
    /*! \brief Instantiates an Event and returns its corresponding GUID
     *  \return Event metadata for the instantiated event
     */
    ocrEvent_t* (*instantiate)(struct _ocrEventFactory_t* factory, ocrEventTypes_t eventType,
        bool takesArg);

    /*! \brief Virtual destructor for the ocrEventFactory_t interface
     */
    void (*destruct)(struct _ocrEventFactory_t* factory);

    ocrEventFcts_t singleFcts; /**< Functions for non-latch events */
    ocrEventFcts_t latchFcts;  /**< Functions for latch events */
} ocrEventFactory_t;

/*******************************************
 *       OCR Event Lists declarations
 ******************************************/

/*! \brief EventList linked list's node constructor
 *  \param[in]  pEvent  Event pointer to wrap to a linked list node
 *  \return A Node instance wrapping an Event to a linked list node
 */
typedef struct event_list_node_struct_t {
    /*! Public Event member of the linked list node wrapper */
    ocrEvent_t *event;
    /*! Public next Event member of the linked list node wrapper */
    struct event_list_node_struct_t *next;
} event_list_node_t;

// TODO: Move this to HC specific stuff
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
    u64 size;
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

#endif /* __OCR_EVENT_H_ */
