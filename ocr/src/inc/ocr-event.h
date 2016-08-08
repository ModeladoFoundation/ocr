/**
 * @brief OCR interface to events
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_EVENT_H__
#define __OCR_EVENT_H__

#include "ocr-edt.h"
#include "ocr-runtime-types.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-runtime-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/

/**
 * @brief Parameter list to create an event factory
 */
typedef struct _paramListEventFact_t {
    ocrParamList_t base;
} paramListEventFact_t;


/****************************************************/
/* OCR EVENT                                        */
/****************************************************/

struct _ocrEvent_t;

/**
 * @brief Abstract class to represent OCR events function pointers
 *
 * This class provides the interface for the underlying implementation to conform with.
 */
typedef struct _ocrEventFcts_t {
    /**
     * @brief Virtual destructor for the Event interface
     * @param[in] self          Pointer to this event
     */
    u8 (*destruct)(struct _ocrEvent_t* self);

    /**
     * @brief Interface to get the GUID of the entity that satisfied an event.
     * @param[in] self          Pointer to this event
     * @return GUID of the entity the event has been satisfied with
     */
    ocrFatGuid_t (*get)(struct _ocrEvent_t* self);

    /**
     * @brief Interface to satisfy the event
     * @param[in] self         Pointer to this event
     * @param[in] db           GUID to satisfy this event with (or NULL_GUID)
     * @param[in] slot         Input slot for this event
     *
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*satisfy)(struct _ocrEvent_t* self, ocrFatGuid_t db, u32 slot);

    /**
     * @brief Register a "signaler" on the event
     *
     * A signaler can either be another event or a data-block. If a data-block,
     * this call is equivalent to calling satisfy.
     *
     * @param[in] self          Pointer to this event
     * @param[in] signaler      GUID of the source (signaler)
     * @param[in] slot          Slot on self that will be satisfied by the signaler
     * @param[in] mode          The access mode for the dependence's data
     * @param[in] isDepAdd      True if this call is part of adding a dependence. False
     *                          if due to a standalone call
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*registerSignaler)(struct _ocrEvent_t *self, ocrFatGuid_t signaler, u32 slot,
                           ocrDbAccessMode_t mode, bool isDepAdd);

    /**
     * @brief Oppositor of registerSignaler()
     *
     * This call will unregister the signaler from the event. Note that
     * the unregistration is per slot (ie: if a signaler is attached to
     * multiple slots, it will need to be unregistered from all slots
     *
     * @param[in] self          Pointer to this event
     * @param[in] signaler      GUID of the source (signaler)
     * @param[in] slot          Slot on self that will be satisfied by the signaler
     * @param[in] isDepRem      True if this call is part of removing a dependence
     * @return 0 on success and a non-zero value on failure
     */
    u8 (*unregisterSignaler)(struct _ocrEvent_t *self, ocrFatGuid_t signaler, u32 slot,
                             bool isDepRem);

    /**
     * @brief Register a "waiter" (aka a dependence) on the event
     *
     * The waiter will be notified on slot 'slot' once this event is satisfid.
     * In other words, the satisfy() function serves to notify the "front" of
     * the event and this call serves to determine what happens at the "back"
     * of the event once the event is satisfied
     *
     * @param[in] self          Pointer to this event
     * @param[in] waiter        EDT/Event to register as a waiter
     * @param[in] slot          Slot to satisfy waiter on once this event
     *                          is satisfied
     * @param[in] isDepAdd      True if this call is part of adding a dependence.
     *                          False if due to a standalone call. This will be
     *                          important for STICKY events for example where
     *                          we only register waiters on their dependence frontier
     * @return 0 on success and a non-zero code on failure
     */
#ifdef REG_ASYNC_SGL
    u8 (*registerWaiter)(struct _ocrEvent_t *self, ocrFatGuid_t waiter, u32 slot,
                         bool isDepAdd, ocrDbAccessMode_t mode);
#else
    u8 (*registerWaiter)(struct _ocrEvent_t *self, ocrFatGuid_t waiter, u32 slot,
                         bool isDepAdd);
#endif
    /**
     * @brief Unregisters a "waiter" (aka a dependence) on the event
     *
     * Note again that if a waiter is registered multiple times (for multiple
     * slots), you will need to unregister it multiple time as well.
     *
     * @param[in] self          Pointer to this event
     * @param[in] waiter        EDT/Event to register as a waiter
     * @param[in] slot          Slot to satisfy waiter on once this event
     *                          is satisfied
     * @param[in] isDepRem      True if part of removing a dependence
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*unregisterWaiter)(struct _ocrEvent_t *self, ocrFatGuid_t waiter, u32 slot,
                           bool isDepRem);
} ocrEventFcts_t;

typedef struct _ocrEventCommonFcts_t {
    /**
     * @brief Set user hints for the Event
     *
     * The event implementation chooses which hint properties will be set.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this event
     * @param[in] hint        Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*setHint)(struct _ocrEvent_t* self, ocrHint_t *hint);

    /**
     * @brief Get user hints from the Event
     *
     * The event implementation chooses which hint properties will be gotten.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this event
     * @param[in/out] hint    Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getHint)(struct _ocrEvent_t* self, ocrHint_t *hint);

    /**
     * @brief Get runtime hints from the Event
     *
     * The hints structure is an array of u64 values,
     * starting with the mask and followed by the
     * hint values.
     *
     * @param[in] self        Pointer to this event
     * @return pointer to hint structure
     */
    ocrRuntimeHint_t* (*getRuntimeHint)(struct _ocrEvent_t* self);

#ifdef ENABLE_RESILIENCY
    /**
     * @brief Get the serialization size
     *
     * @param[in] self        Pointer to this event
     * @param[out] size       Buffer size required to serialize event
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getSerializationSize)(struct _ocrEvent_t* self, u64* size);

    /**
     * @brief Serialize event into buffer
     *
     * @param[in] self        Pointer to this event
     * @param[in/out] buffer  Buffer to serialize into
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*serialize)(struct _ocrEvent_t* self, u8* buffer);

    /**
     * @brief Deserialize event from buffer
     *
     * @param[in] buffer      Buffer to deserialize from
     * @param[out] self       Pointer to deserialized event
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*deserialize)(u8* buffer, struct _ocrEvent_t** self);

    /**
     * @brief Fixup event pointers after deserialization
     *
     * @param[in] self        Pointer to this event
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*fixup)(struct _ocrEvent_t* self);

    /**
     * @brief Deallocate event during PD reset
     *
     * @param[in] self        Pointer to this event
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*reset)(struct _ocrEvent_t* self);
#endif
} ocrEventCommonFcts_t;

/**
 * @brief Abstract class to represent OCR events.
 *
 * Events can be satisfied with a GUID, polled for
 * what GUID satisfied the event and registered to
 * other events or edts.
 */
typedef struct _ocrEvent_t {
    ocrObject_t base;
    ocrGuid_t guid;         /**< GUID for this event */
#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcess_t *statProcess;
#endif
    ocrEventTypes_t kind;  /**< The kind of this event instance */
    u32 fctId;             /**< The functions to use to access this event */
} ocrEvent_t;


/****************************************************/
/* OCR EVENT FACTORY                                */
/****************************************************/

/**
 * @brief events factory
 */
typedef struct _ocrEventFactory_t {
    ocrObjectFactory_t base;
    /** @brief Instantiates an Event and returns its corresponding GUID
     *  @param[in] factory          Pointer to this factory
     *  @param[in] eventType        Type of event to instantiate
     *  @param[in/out] guid         GUID for the object. If GUID_PROP_IS_LABELED is
     *                              specified in properties, guid.guid contains the
     *                              GUID to use. guid.metaDataPtr will contain
     *                              the pointer to the object (an ocrEvent_t*)
     *  @param[in] properties       Properties for the creation. Currently:
     *                              GUID_PROP_* and EVT_PROP_*
     *  @param[in] instanceArg      Arguments specific for this instance
     *  @return 0 on success or an error code on failure:
     *      - OCR_EGUIDEXISTS if the object exists (if GUID_PROP_IS_LABELED and GUID_PROP_CHECK)
     */
    u8 (*instantiate)(struct _ocrEventFactory_t* factory, ocrFatGuid_t *guid,
                      ocrEventTypes_t eventType, u32 properties,
                      ocrParamList_t *instanceArg);

    u32 factoryId;             /**< Factory ID (matches fctId in event */
    ocrEventCommonFcts_t commonFcts; /**< Functions common for all the types of events */
    ocrEventFcts_t fcts[OCR_EVENT_T_MAX]; /**< Functions for specific types of events */
    u64* hintPropMap; /**< Mapping hint properties to implementation specific packed array */
} ocrEventFactory_t;

#endif /* __OCR_EVENT_H_ */
