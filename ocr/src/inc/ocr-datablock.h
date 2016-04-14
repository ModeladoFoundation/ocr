/**
 * @brief OCR interface to the datablocks
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_DATABLOCK_H__
#define __OCR_DATABLOCK_H__

#include "ocr-allocator.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "ocr-runtime-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif


/****************************************************/
/* OCR PARAMETER LISTS                              */
/****************************************************/

typedef struct _paramListDataBlockFact_t {
    ocrParamList_t base;    /**< Base class */
} paramListDataBlockFact_t;

typedef struct _paramListDataBlockInst_t {
    ocrParamList_t base;    /**< Base class */
    ocrGuid_t allocator;    /**< Allocator that created this data-block */
    ocrGuid_t allocPD;      /**< Policy-domain of the allocator */
    u64 size;               /**< data-block size */
    void* ptr;              /**< Initial location for the data-block */
    u32 properties;         /**< Properties for the data-block */
} paramListDataBlockInst_t;


/****************************************************/
/* OCR DATABLOCK                                    */
/****************************************************/

struct _ocrDataBlock_t;

typedef struct _ocrDataBlockFcts_t {
    /**
     * @brief Destroys a data-block
     *
     * Again, this does not perform any de-allocation
     * but makes the meta-data invalid and informs
     * the GUID system that the GUID for this data-block
     * is no longer used
     *
     * @param self          Pointer for this data-block
     * @return 0 on success and an error code on failure
     */
    u8 (*destruct)(struct _ocrDataBlock_t *self);

    /**
     * @brief Acquires the data-block for an EDT
     *
     * This call registers a user (the EDT) for the data-block
     *
     * @param[in] self          Pointer for this data-block
     * @param[out] ptr          Returns the pointer to use to access the data
     * @param[in] edt           EDT seeking registration
     *                          Must be fully resolved
     * @param[in] destLoc       Destination location for the acquire
     *                          May be different than the EDT location at this time
     * @param[in] edtSlot       EDT slot the DB is acquired for. Can be EDT_NO_SLOT
     *                          when not applicable. For example when acquiring
     *                          a datablock for runtime usage)
     * @param[in] isInternal    True if this is an acquire implicitly
     *                          done by the runtime at EDT launch
     * @param[in] properties    Any additional properties for the acquire call
     * @return 0 on success or the following error code:
     *
     *
     * @note Multiple acquires for the same EDT have no effect BUT
     * the DB should only be freed ONCE
     */
    u8 (*acquire)(struct _ocrDataBlock_t *self, void** ptr, ocrFatGuid_t edt, ocrLocation_t destLoc,
                  u32 edtSlot, ocrDbAccessMode_t mode, bool isInternal, u32 properties);

    /**
     * @brief Releases a data-block previously acquired
     *
     * @param self          Pointer for this data-block
     * @param edt           EDT seeking to de-register from the data-block.
     *                      Must be fully resolved
     * @param srcLoc        Source location that initiated the release
     * @param isInternal    True if matching an internal acquire
     * @return 0 on success and an error code on failure (see ocr-db.h)
     *
     * @note No need to match one-to-one with acquires. One release
     * releases any and all previous acquires
     */
    u8 (*release)(struct _ocrDataBlock_t *self, ocrFatGuid_t edt, ocrLocation_t srcLoc, bool isInternal);

    /**
     * @brief Requests that the block be freed when possible
     *
     * This call will return true if the free request was successful.
     * This does not mean that the block was actually freed (other
     * EDTs may be using it), just that the block will be freed
     * at some point
     *
     * @param self          Pointer to this data-block
     * @param edt           EDT seeking to free the data-block
     *                      Must be fully resolved
     * @param srcLoc        Location requesting the free
     * @param properties    Properties of the free (runtime free, require release, etc..)
     * @return 0 on success and an error code on failure (see ocr-db.h)
     */
    u8 (*free)(struct _ocrDataBlock_t *self, ocrFatGuid_t edt, ocrLocation_t srcLoc, u32 properties);

    /**
     * @brief Register a "waiter" (aka a dependence) on the data-block
     *
     * The waiter is waiting on 'slot' for this data-block.
     *
     * @param[in] self          Pointer to this data-block
     * @param[in] waiter        EDT/Event to register as a waiter
     * @param[in] slot          Slot the waiter is waiting on
     * @param[in] isDepAdd      True if this call is part of adding a dependence.
     *                          False if due to a standalone call.
     * @return 0 on success and a non-zero code on failure
     * @note For DBs, this is mostly a "hint" to inform the data-block
     * of where it is going to be needed at some point
     */
    u8 (*registerWaiter)(struct _ocrDataBlock_t *self, ocrFatGuid_t waiter, u32 slot,
                         bool isDepAdd);

    /**
     * @brief Unregisters a "waiter" (aka a dependence) on the data-block
     *
     * Note again that if a waiter is registered multiple times (for multiple
     * slots), you will need to unregister it multiple time as well.
     *
     * @param[in] self          Pointer to this data-block
     * @param[in] waiter        EDT/Event to register as a waiter
     * @param[in] slot          Slot the waiter is waiting on
     * @param[in] isDepRem      True if part of removing a dependence
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*unregisterWaiter)(struct _ocrDataBlock_t *self, ocrFatGuid_t waiter, u32 slot,
                           bool isDepRem);

    /**
     * @brief Set user hints for the data-block
     *
     * The data-block implementation chooses which hint properties will be set.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this data-block
     * @param[in] hint        Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*setHint)(struct _ocrDataBlock_t* self, ocrHint_t *hint);

    /**
     * @brief Get user hints from the data-block
     *
     * The data-block implementation chooses which hint properties will be gotten.
     * Other properties in the user hint will be ignored.
     *
     * @param[in] self        Pointer to this data-block
     * @param[in/out] hint    Pointer to the user hint object
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getHint)(struct _ocrDataBlock_t* self, ocrHint_t *hint);

    /**
     * @brief Get runtime hints from the DB
     *
     * The hints structure is an array of u64 values,
     * starting with the mask and followed by the
     * hint values.
     *
     * @param[in] self        Pointer to this datablock
     * @return pointer to hint structure
     */
    ocrRuntimeHint_t* (*getRuntimeHint)(struct _ocrDataBlock_t* self);
} ocrDataBlockFcts_t;

/**
 * @brief Internal description of a data-block.
 *
 * This describes the internal representation of
 * a data-block and the meta-data that is associated
 * with it for book-keeping
 **/
typedef struct _ocrDataBlock_t {
    ocrObject_t base;
    ocrGuid_t guid; /**< The guid of this data-block */
#ifdef OCR_ENABLE_STATISTICS
    ocrStatsProcess_t *statProcess;
#endif
    ocrGuid_t allocator;    /**< Allocator that created the data chunk (ptr) */
    ocrGuid_t allocatingPD; /**< Policy domain of the creating allocator */
    u64 size;               /**< Size of the data-block */
    void* ptr;              /**< Current location for this data-block */
    u32 flags;              /**< flags for the data-block, lower 16 bits are info
                                 from user, upper 16 bits is for internal bookeeping */
    u32 fctId;              /**< ID determining which functions to use */
#ifdef ENABLE_RESILIENCY
    void* bkPtr;
    ocrGuid_t singleAssigner;
#endif
} ocrDataBlock_t;

// User DB properties
#define DB_ACCESS_MODE_MASK 0xF /**< Bits encoding the access modes (see inc/ocr-types.h) */

// Runtime DB properties (upper 16 bits of a u32)
#define DB_PROP_RT_ACQUIRE          0x10000 // DB acquired by runtime
#define DB_PROP_RT_OBLIVIOUS        0x20000 // BUG #607 DB RO mode: (Flag is for runtime use)
#define DB_PROP_NO_RELEASE          0x40000 // Indicate a release is not required
#define DB_PROP_RT_PD_ACQUIRE       0x80000 // DB acquired by scheduler for whole PD
#define DB_PROP_RT_PROXY            0x100000// DB metadata instantiated as proxy (workaround for BUG #162)

#define DB_FLAG_RT_FETCH            0x1000000
#define DB_FLAG_RT_WRITE_BACK       0x2000000

/****************************************************/
/* OCR DATABLOCK FACTORY                            */
/****************************************************/

/**
 * @brief data-block factory
 */
typedef struct _ocrDataBlockFactory_t {
    ocrObjectFactory_t base;
    /**
     * @brief Creates a data-block to represent a chunk of memory
     *
     * @param[in] factory       Pointer to this factory
     * @param[in/out] guid      GUID for the object. If GUID_PROP_IS_LABELED is specified in
     *                          properties, guid.guid contains the GUID to use. guid.metaDataPtr will
     *                          contain the pointer to the object (an ocrDataBlock_t)
     * @param[in] allocator     Allocator guid used to allocate memory
     * @param[in] allocPD       Policy-domain of the allocator
     * @param[in] size          data-block size
     * @param[in] ptr           Pointer to the memory to use (created through an allocator)
     * @param[in] hint          Hints provided at time of creation
     * @param[in] properties    Properties for the data-block creation (GUID_PROP_* or DB_PROP_*)
     * @param[in] instanceArg   Arguments specific for this instance
     * @return 0 on success or an error code on failure:
     *    - OCR_EGUIDEXISTS if the object exists (if GUID_PROP_IS_LABELED and GUID_PROP_CHECK)
     **/
    u8 (*instantiate)(struct _ocrDataBlockFactory_t *factory, ocrFatGuid_t *guid,
                      ocrFatGuid_t allocator, ocrFatGuid_t allocPD, u64 size,
                      void* ptr, ocrHint_t *hint, u32 properties, ocrParamList_t *instanceArg);

    u32 factoryId; /**< Corresponds to fctId in DB */
    ocrDataBlockFcts_t fcts; /**< Function pointers created instances should use */
    u64 *hintPropMap; /**< Mapping hint properties to implementation specific packed array */
} ocrDataBlockFactory_t;

#endif /* __OCR_DATABLOCK_H__ */
