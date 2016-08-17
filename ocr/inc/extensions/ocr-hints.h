/**
 * @brief OCR APIs for providing application hints to the runtime
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_HINT_H__
#define __OCR_HINT_H__

#include "ocr-types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  @defgroup user_hints User hints management for OCR
 *  @brief Describes the user hints management APIs for OCR.
 *
 *  These APIs will enable OCR application developers to provide runtime hints
 *  (which may be application or machine specific). OCR hints are optional and
 *  viewed as extra information that may help with better execution of the
 *  program. An OCR program's correctness is never dependent on hints. The
 *  runtime may or may not use all OCR user hints in a program, e.g., the
 *  scheduler may choose to ignore some user hints.
 *
 * @{
**/

/**
 * @brief Initializes a user hint variable
 *
 * User hint objects in OCR are of type ocrHint_t. A user hint object is a
 * stack allocated variable that can only exist within the lifetime of an EDT.
 * The user has to first call ocrHintInit to initialize a local hint variable
 * before using it. Subsequent calls to ocrHintInit will reset the variable to
 * an empty state, that is where none of the properties are set.
 * E.g:
 * ...
 * ocrHint_t hintVar;
 * u8 err = ocrHintInit( &hintVar, OCR_HINT_EDT_T );
 * ...
 *
 * @param[in,out] hint      Hint object to initialize
 * @param[in] hintType      The type of hint object
 *                          (denoting the target guid)
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: error in initializing hint
 **/
u8 ocrHintInit(ocrHint_t *hint, ocrHintType_t hintType);

/**
 * @brief Sets the value of a specific hint property
 *
 * A value for a specific hint property can be set if that property is
 * supported for that specific hint type. If a property value is already set,
 * then the value is updated.
 *
 * @param[in] hint          The hint object
 * @param[in] hintProp      The hint property to set
 * @param[in] value         The value of the hint property
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If property is invalid for the hint type.
 **/
u8 ocrHintSetValue(ocrHint_t *hint, ocrHintProp_t hintProp, ocrHintVal_t value);

/**
 * @brief Unsets the value of a specific hint property
 *
 * @param[in] hint          The hint object
 * @param[in] hintProp      The hint property to set
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If property is invalid for the hint type.
 **/
u8 ocrHintUnsetValue(ocrHint_t *hint, ocrHintProp_t hintProp);

/**
 * @brief Gets the value of a specific hint property
 *
 * A value for the hint property can be gotten from a hint object if the
 * property is supported and it has already been set.
 *
 * @param[in] hint          The hint object
 * @param[in] hintProp      The hint property to set
 * @param[out] value        The value of the hint property
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If property is invalid for this hint type
 *     - ENOENT: If property is not set on this hint object
 **/
u8 ocrHintGetValue(ocrHint_t *hint, ocrHintProp_t hintProp, ocrHintVal_t *value);

/**
 * Deprecated.
 * @see ocrHintSetValue
 **/
u8 ocrSetHintValue(ocrHint_t *hint, ocrHintProp_t hintProp, u64 value)
__attribute__((deprecated("Function ocrSetHintValue was removed in OCR v1.2.0. Please use ocrHintSetValue instead.")));

/**
 * Deprecated.
 * @see ocrHintUnsetValue
 **/
u8 ocrUnsetHintValue(ocrHint_t *hint, ocrHintProp_t hintProp)
__attribute__((deprecated("Function ocrUnsetHintValue was removed in OCR v1.2.0. Please use ocrHintUnsetValue instead.")));

/**
 * Deprecated.
 * @see ocrHintGetValue
 **/
u8 ocrGetHintValue(ocrHint_t *hint, ocrHintProp_t hintProp, u64 *value)
__attribute__((deprecated("Function ocrGetHintValue was removed in OCR v1.2.0. Please use ocrHintGetValue instead.")));

/**
 * @brief Sets all hints in the hint object to the target guid
 *
 * If the target guid has some of the hints already set then their values are
 * updated. If hints are set on the same guid concurrently, then the final
 * values are undefined.
 *
 * @param[in] guid          The target guid of the hints
 * @param[in] hint          The hint object
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If hint type and target guid kind are incompatible
 *     - EFAULT: Some hints were not set due to insufficient or invalid data
 **/
u8 ocrSetHint(ocrGuid_t guid, ocrHint_t *hint);

/**
 * @brief Gets the existing hints from a specific guid
 *
 * The hint variable that is passed to this call will be updated with hint
 * property values from the guid. The type of the hint variable should be
 * compatible with the guid kind. If the hint properties that exist on the
 * guid are already set on the hint variable then those properties will be
 * updated. The hint properties that are present on the guid but not on the
 * hint variable will be added to the hint variable. If the hint variable has
 * other properties set which do not exist on the guid, those properties will
 * be retained in the hint variable.
 *
 * @param[in] guid          The target guid of the hints
 * @param[out] hint         The hint variable that will be
 *                          populated with the target guid's
 *                          existing hints.
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If hint type and guid kind are incompatible
 **/
u8 ocrGetHint(ocrGuid_t guid, ocrHint_t *hint);

/**
   @}
**/

#if 0 //Will be moved to ocr-tuning.h
/**
 * @defgroup API for Application Level Groups in OCR
 * @brief OCR groups are logical containers for sets of guids
 *
 * Application groups help the user provide hints to the runtime regarding
 * information related to groups of guids. For example, if the user wants
 * to say that a certain set of guids have some specific properties, then
 * application groups can be used to convey that message. This information
 * may be used by the runtime to affect the scheduling of EDTs and DBs.
 * However, creating groups is optional in a OCR program, and in the absence
 * of groups, the program will still execute correctly.
 *
 * The group type decides the semantics of a group. An EDT or DB or group guid
 * can be part of groups of different types. However, a guid can belong to only
 * one group of each type.
 *
 * A group contains EDT and/or DB and/or other groups guids as group elements.
 * Groups are hierarchical, i.e, a group can have other groups as group
 * elements. A group's parent has to be of the same group type.
 *
 * The guid of a group can be passed to the ocrEdtCreate and ocrDbCreate APIs
 * as the "affinity" argument. The user can also set hints to a group.
 *
 * Example:
 * A possible objective of groups can be to maximize data reuse by the EDTs in
 * the group. E.g: If a group of independent EDTs share a common set of
 * datablocks, a group of such EDTs and DBs may indicate strong affinity among
 * the group elements. Such groups may guide the scheduler to execute the EDTs
 * and allocate the DBs of the group close together in space and time.
 * (Note: The runtime can derive this information from the dependence structure
 * of the program, but it involves the scheduler spending extra cycles to get
 * this information, which may be too expensive on certain machines).
 *
 * @{
 **/

typedef enum {
    OCR_GROUP_AFFINITY,
} ocrGroupType_t;

/**
 * @brief Creates an application group object
 *
 * Creates and initializes a group object. A group is created with a specific
 * group type. The group type defines the usage semantics of the group.
 *
 * @param[out] groupGuid        The guid of the group object
 * @param[in]  groupType        The type of the group
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If groupType is invalid
 */
u8 ocrCreateGroup(ocrGuid_t *groupGuid, ocrGroupType_t groupType);

/**
 * @brief Associates a EDT/DB/Group guid with application group
 *
 * An EDT/DB/Group guid is added to an application group. This makes the guid
 * become an element of the application group. If the guid is already an
 * element of another group of the same type, then it is removed from that
 * original group and then added to this new group.
 *
 * @param[in] groupGuid        The guid of the application group
 * @param[in] elGuid           The guid which is given a new group
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If element could not be added to group
 */
u8 ocrAddToGroup(ocrGuid_t groupGuid, ocrGuid_t elGuid);

/**
 * @brief Dissociate a EDT/DB/Group guid with a group type
 *
 * If an EDT/DB/Group guid is an element of an application group of type
 * groupType, then it is removed from that group. If no application group
 * was already set on the guid for that groupType, then this call is a no-op.
 *
 * @param[in] groupType        The type of the group that element is removed from
 * @param[in] elGuid           The guid which dissociates itself from the group
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If groupGuid could not be unset for elGuid
 */
u8 ocrRemoveFromGroup(ocrGroupType_t groupType, ocrGuid_t elGuid);

/**
 * @brief Gets the guid of the application group associated with a guid
 *
 * This retrieves the application group of type groupType that is associated
 * with elGuid.
 *
 * @param[out] groupGuid       The guid of the application group
 * @param[in]  groupType       The type of the group to get
 * @param[in]  elGuid          The element guid from which group guid is retrieved
 *
 * @return a status code
 *     - 0: successful
 *     - EINVAL: If elGuid has no group of type groupType set
 */
u8 ocrGetGroup(ocrGuid_t *groupGuid, ocrGroupType_t groupType, ocrGuid_t elGuid);

/**
 * @brief Destroys the an application group object
 *
 * This destroys an application group only if the group is empty.
 *
 * @param[in] groupGuid        The guid of the application group to destroy
 *
 * @return a status code
 *     - 0: successful
 *     - ENOTEMPTY: If group is not empty when destroy is called
 */
u8 ocrDestroyGroup(ocrGuid_t groupGuid);

/**
   @}
**/
#endif

#ifdef __cplusplus
}
#endif

#endif /* __OCR_HINT_H__ */
