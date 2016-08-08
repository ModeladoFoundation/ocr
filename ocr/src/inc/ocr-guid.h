/**
 * @brief OCR internal API to GUID management
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_GUID_H__
#define __OCR_GUID_H__

#include "ocr-runtime-types.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"

#include "ocr-guid-kind.h"

/****************************************************/
/* OCR PARAMETER LISTS                              */
/****************************************************/

/**
 * @brief Parameter list to create a guid provider factory
 */
typedef struct _paramListGuidProviderFact_t {
    ocrParamList_t base;
} paramListGuidProviderFact_t;

/**
 * @brief Parameter list to create a guid provider instance
 */
typedef struct _paramListGuidProviderInst_t {
    ocrParamList_t base;
} paramListGuidProviderInst_t;


/****************************************************/
/* OCR GUID PROVIDER                                */
/****************************************************/

struct _ocrGuidProvider_t;
struct _ocrPolicyDomain_t;

#define MD_FETCH 1
#define MD_LOCAL 0

//
// TODO: this is to be replaced by some form of runtime event
//

#define REG_OPEN 0x1
#define REG_CLOSED 0x0
#define OCR_GUID_MD_PROXY 32

struct _ocrPolicyMsg_t;

typedef struct _MdProxyNode_t {
    //TODO This should be a rt event to allow 'anything' to be a continuation of the md fetch
    struct _ocrPolicyMsg_t * msg;
    struct _MdProxyNode_t * next;
} MdProxyNode_t;

typedef struct {
#ifdef ENABLE_RESILIENCY
    ocrObject_t base;
    u32 numNodes;
#endif
    MdProxyNode_t * queueHead;
    volatile u64 ptr;
} MdProxy_t;

//
// END TODO
//


/**
 * @brief GUID provider function pointers
 *
 * The function pointers are separate from the GUID provider instance to allow
 * for the sharing of function pointers for GUID provider from the same factory
 */
typedef struct _ocrGuidProviderFcts_t {
    /**
     * @brief Destructor equivalent
     *
     * This will free the GUID provider and any
     * memory that it uses
     *
     * @param self          Pointer to this GUID provider
     */
    void (*destruct)(struct _ocrGuidProvider_t* self);

    /**
     * @brief Switch runlevel
     *
     * @param[in] self         Pointer to this object
     * @param[in] PD           Policy domain this object belongs to
     * @param[in] runlevel     Runlevel to switch to
     * @param[in] phase        Phase for this runlevel
     * @param[in] properties   Properties (see ocr-runtime-types.h)
     * @param[in] callback     Callback to call when the runlevel switch
     *                         is complete. NULL if no callback is required
     * @param[in] val          Value to pass to the callback
     *
     * @return 0 if the switch command was successful and a non-zero error
     * code otherwise. Note that the return value does not indicate that the
     * runlevel switch occured (the callback will be called when it does) but only
     * that the call to switch runlevel was well formed and will be processed
     * at some point
     */
    u8 (*switchRunlevel)(struct _ocrGuidProvider_t* self, struct _ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(struct _ocrPolicyDomain_t*, u64), u64 val);
    /**
     * @brief Reserves a GUID range to be used by a user mapping function
     *
     * @param[in] self             GUID provider reserving the GUIDs
     * @param[out] startGuid       Returns the first GUID of the range
     * @param[out] skipGuid        Returns the "step" between valid GUIDs in the range
     * @param[in] numberGuids      Number of GUIDs to reserve
     * @param[in] guidType         Type of GUIDs this range will be used to store
     * @return 0 on success a non-zero error code
     */
    u8 (*guidReserve)(struct _ocrGuidProvider_t *self, ocrGuid_t *startGuid, u64* skipGuid,
                      u64 numberGuids, ocrGuidKind guidType);

    /**
     * @brief Un-reserves a GUID range when no longer needed.
     *
     * Note that this function does not free/unallocate any
     * GUIDs that are active in the range, it un-reserves all non-used
     * GUIDs.
     *
     * @param[in] self            GUID provider un-reserving the GUIDs
     * @param[in] startGuid       First GUID of the range
     * @param[in] skipGuid        "Step" between valid GUIDs in the range
     * @param[in] numberGuids     Number of GUIDs to release
     * @return 0 on success a non-zero error code
     */
    u8 (*guidUnreserve)(struct _ocrGuidProvider_t *self, ocrGuid_t startGuid, u64 skipGuid,
                        u64 numberGuids);

    /**
     * @brief Gets a GUID for an object of kind 'kind'
     * and associates the value val.
     *
     * The GUID provider basically associates a value with the
     * GUID
     *
     * @param[in] self          Pointer to this GUID provider
     * @param[out] guid         GUID returned
     * @param[in] val           Value to be associated
     * @param[in] kind          Kind of the object that will be associated with the GUID
     * @param[in] targetLoc     Location targeted by this GUID (whenever relevant)
     * @param[in] properties    Properties for the GUID generation
     * @return 0 on success or an error code
     */
    u8 (*getGuid)(struct _ocrGuidProvider_t* self, ocrGuid_t* guid, u64 val,
                  ocrGuidKind kind, ocrLocation_t targetLoc, u32 properties);

    /**
     * @brief Create a GUID for an object of kind 'kind'
     * and creates storage of size 'size' associated with the
     * GUID
     *
     * getGuid() will associate an existing 64-bit value to a
     * GUID but createGuid() will create storage of size 'size'
     * and associate the resulting address with a GUID. This
     * is useful to create metadata storage
     *
     *
     * @param[in] self          Pointer to this GUID provider
     * @param[in/out] fguid     GUID returned (with metaDataPtr)
     *                          If properties has GUID_PROP_IS_LABELED
     *                          the fguid.guid field should contain
     *                          the GUID that is requested
     * @param[in] size          Size of the storage to be created
     * @param[in] kind          Kind of the object that will be associated with the GUID
     * @param[in] targetLoc     Location targeted by this GUID (whenever relevant)
     * @param[in] properties    Properties for the creation. Mostly contains stuff
     *                          related to GUID labeling
     * @return 0 on success or an error code:
     *     - OCR_EGUIDEXISTS if GUID_PROP_CHECK is set and the GUID already exists
     */
    u8 (*createGuid)(struct _ocrGuidProvider_t* self, ocrFatGuid_t* fguid,
                     u64 size, ocrGuidKind kind, ocrLocation_t targetLoc, u32 properties);

    /**
     * @brief Resolve the associated value to the GUID 'guid'
     *
     * \param[in] self          Pointer to this GUID provider
     * \param[in] guid          GUID to resolve
     * \param[out] val          Parameter-result for the value to return
     * \param[out] kind         Parameter-result for the GUID's kind. Can be NULL if the user does not care about the kind
     *
     * @return 0 on success or an error code
     */
    u8 (*getVal)(struct _ocrGuidProvider_t* self, ocrGuid_t guid, u64* val, ocrGuidKind* kind, u32 mode, MdProxy_t ** proxy);

    /**
     * @brief Resolve the kind of a GUID
     *
     * \param[in] self          Pointer to this GUID provider
     * \param[in] guid          GUID to get the kind of
     * \param[out] kind         Parameter-result for the GUID's kind.
     * @return 0 on success or an error code
     */
    u8 (*getKind)(struct _ocrGuidProvider_t* self, ocrGuid_t guid, ocrGuidKind* kind);

    /**
     * @brief Resolve the location of a GUID
     *
     * BUG #605 Locations spec: any property regarding location ?
     *  - Is it where guid has been created ?
     *  - Is it where the value is ?
     *  - Can we get different location across calls if associated value is replicated ?
     *
     * \param[in] self          Pointer to this GUID provider
     * \param[in] guid          GUID to get the location of
     * \param[out] location     Parameter-result for the GUID's location
     * @return 0 on success or an error code
     */
    u8 (*getLocation)(struct _ocrGuidProvider_t* self, ocrGuid_t guid, ocrLocation_t* location);

    /**
     * @brief Register a GUID with the GUID provider.
     *
     * This function is meant to register GUIDs created by other providers of the
     * same type 'self' is. Implementers must make sure GUID are generated coherently
     * across provider instances.
     *
     * \param[in] self          Pointer to this GUID provider
     * \param[in] guid          GUID to register
     * \param[in] val           The GUID's associated value
     * @return 0 on success or an error code
     */
    u8 (*registerGuid)(struct _ocrGuidProvider_t* self, ocrGuid_t guid, u64 val);

    /**
     * @brief Unregister a GUID with the GUID provider.
     *
     * This function is meant to unregister GUIDs created by other providers of the
     * same type 'self' is. This call does not deallocate the GUID and its associated metadata.
     *
     * \param[in] self          Pointer to this GUID provider
     * \param[in] guid          GUID to register
     * \param[out] val           The GUID's associated value
     * @return 0 on success or an error code
     */
    u8 (*unregisterGuid)(struct _ocrGuidProvider_t* self, ocrGuid_t guid, u64 ** val);

    /**
     * @brief Releases the GUID
     *
     * Whether the GUID provider will re-issue this same GUID for a different
     * object is implementation dependent.
     *
     * @param[in] self        Pointer to this GUID provider
     * @param[in] guid        GUID to release
     * @param[in] releaseVal  If true, will also "free" the value associated
     *                        with the GUID
     * @return 0 on success or an error code
     */
    u8 (*releaseGuid)(struct _ocrGuidProvider_t *self, ocrFatGuid_t guid, bool releaseVal);

#ifdef ENABLE_RESILIENCY
    /**
     * @brief Get the serialization size
     *
     * @param[in] self        Pointer to this GUID provider
     * @param[out] size       Buffer size required to serialize GUID provider metadata
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*getSerializationSize)(struct _ocrGuidProvider_t *self, u64 * size);

    /**
     * @brief Serialize GUID provider metadata into buffer
     *
     * @param[in] self        Pointer to this GUID provider
     * @param[in/out] buffer  Buffer to serialize into
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*serialize)(struct _ocrGuidProvider_t *self, u8 * buffer);

    /**
     * @brief Deserialize GUID provider from buffer
     *
     * @param[in] self        Pointer to this GUID provider
     * @param[in] buffer      Buffer to deserialize from
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*deserialize)(struct _ocrGuidProvider_t *self, u8 * buffer);

    /**
     * @brief Reset GUID provider user program state by clearing
     * all user program metadata
     *
     * @param[in] self        Pointer to this GUID provider
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*reset)(struct _ocrGuidProvider_t *self);

    /**
     * @brief Fixup GUID provider state after restore
     *
     * @param[in] self        Pointer to this GUID provider
     * @return 0 on success and a non-zero code on failure
     */
    u8 (*fixup)(struct _ocrGuidProvider_t *self);
#endif
} ocrGuidProviderFcts_t;

/**
 * @brief GUIDs Provider for the system
 *
 * GUIDs should be unique and are used to
 * identify and locate objects (and their associated
 * metadata mostly). GUIDs serve as a level of indirection
 * to allow objects to move around in the system and
 * support different address spaces (in the future)
 */
typedef struct _ocrGuidProvider_t {
    ocrObject_t base;
    struct _ocrPolicyDomain_t *pd;  /**< Policy domain of this GUID provider */
    u32 id;                         /**< Function IDs for this GUID provider */
    ocrGuidProviderFcts_t fcts;     /**< Functions for this instance */
} ocrGuidProvider_t;


/****************************************************/
/* OCR GUID PROVIDER FACTORY                        */
/****************************************************/

/**
 * @brief GUID provider factory
 */
typedef struct _ocrGuidProviderFactory_t {
    /**
     * @brief Instantiate a new GUID provider and returns a pointer to it.
     *
     * @param factory       Pointer to this factory
     * @param instanceArg   Arguments specific for this instance
     */
    ocrGuidProvider_t* (*instantiate)(struct _ocrGuidProviderFactory_t *factory, ocrParamList_t* instanceArg);

    /**
     * @brief GUID provider factory destructor
     * @param factory       Pointer to the factory to destroy.
     */
    void (*destruct)(struct _ocrGuidProviderFactory_t *factory);

    u32 factoryId;
    ocrGuidProviderFcts_t providerFcts; /**< Function pointers created instances should use */
} ocrGuidProviderFactory_t;

/****************************************************/
/* OCR GUID CONVENIENCE FUNCTIONS                   */
/****************************************************/

/**
 * @brief Resolve the kind of a guid (could be an event, an edt, etc...)
 * @param[in] pd          Policy domain
 * @param[in] guid        The GUID for which we need the kind
 * @param[out] kindRes    Parameter-result to contain the kind
 */
static inline u8 guidKind(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t guid,
                          ocrGuidKind* kindRes) __attribute__((unused));

/**
 *  @brief Generates a GUID based on the value 'val'.
 *
 *  This does not allocate space for the metadata associated with the GUID
 *  but rather associates a GUID with the value passed in.
 *
 *  @param[in] pd          Policy domain
 *  @param[in] ptr         The pointer for which we want a guid
 *  @param[out] guidRes    Parameter-result to contain the guid
 *  @param[in] kind        The kind of the guid (whether is an event, an edt, etc...)
 *
 *  @return 0 on success and a non zero failure on failure
 */
static inline u8 guidify(struct _ocrPolicyDomain_t * pd, u64 val, ocrFatGuid_t * guidRes,
                         ocrGuidKind kind) __attribute__((unused));

/**
 * @brief Resolves the pointer to the metadata out of the GUID
 *
 * Note that the value in metaDataPtr should only be used as a
 * READ-ONLY value. This call may return a COPY of the metadata
 * area.
 * @param[in] pd          Policy domain
 * @param[in/out] res     Parameter-result to contain the fully resolved GUID
 * @param[out] kindRes    Parameter-result to contain the kind
 */
static inline u8 deguidify(struct _ocrPolicyDomain_t * pd, ocrFatGuid_t *res,
                           ocrGuidKind* kindRes) __attribute__((unused));

/**
 * @brief Check if a GUID represents a data-block
 *
 * @param[in] pd          Policy domain in which we are
 * @param[in] guid        The GUID to check the kind
 */
static inline bool isDatablockGuid(struct _ocrPolicyDomain_t *pd,
                                   ocrFatGuid_t guid) __attribute__((unused));

/**
 * @brief Check if a GUID represents an event
 *
 * @param[in] pd          Policy domain in which we are
 * @param[in] guid        The GUID to check the kind
 */
static inline bool isEventGuid(struct _ocrPolicyDomain_t *pd,
                               ocrFatGuid_t guid) __attribute__((unused));

/**
 * @brief Check if a GUID represents an EDT
 *
 * @param[in] pd          Policy domain in which we are
 * @param[in] guid        The GUID to check the kind
 */
static inline bool isEdtGuid(struct _ocrPolicyDomain_t *pd,
                             ocrFatGuid_t guid) __attribute__((unused));

/*! \brief Check if a guid represents a latch-event
 *  \param[in] guid        The guid to check the kind
 */
static inline ocrEventTypes_t eventType(struct _ocrPolicyDomain_t *pd,
                                        ocrFatGuid_t guid) __attribute__((unused));

#endif /* __OCR_GUID__H_ */
