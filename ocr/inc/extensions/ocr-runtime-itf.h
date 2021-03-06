/**
 * @brief Contains additional APIs to enable other runtimes to be developed
 * on top of OCR.
 */
/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef OCR_RUNTIME_ITF_H_
#define OCR_RUNTIME_ITF_H_

#ifdef ENABLE_EXTENSION_RTITF

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @ingroup OCRExt
 * @{
 */
/**
 * @defgroup OCRExtRuntimeItf Interface for runtimes built on top of OCR
 * @brief Defines additional API for runtime implementers
 *
 * @warning These APIs are not fully supported at this time
 * and should be used with caution

 * @{
 **/

#include "ocr-types.h"

/**
 * @brief Get the value stored at 'offset' in the
 * current EDT's local storage
 *
 * Each EDT can store some information in its
 * metadata in a way akin to TLS. This call
 * retrieves this information from the ELS. The
 * programmer can set this information using
 * ocrElsUserSet()
 *
 * @param[in] offset Offset (in bytes) in the ELS to fetch
 * @return the value at the requested offset or
 * NULL_GUID if there is no ELS support
 * @warning Must be called from within an EDT code.
 **/
ocrGuid_t ocrElsUserGet(u8 offset);

/**
 * @brief Set the value stored at 'offset in the current
 * EDT's local storage
 *
 * @see ocrElsUserGet()
 *
 * @param[in] offset Offset (in bytes) in the ELS to set
 * @param[in] data   Value to write at that offset
 *
 * @note This is a no-op if there is no ELS support
 **/
void ocrElsUserSet(u8 offset, ocrGuid_t data);

/**
 * @brief Get the local data-store associated with the
 * current EDT and its size.
 *
 * @param[out] ptr Pointer to the local data-store
 * @param[out] elsSize Size (in bytes) of the local data-store
 * */
u8 ocrEdtLocalStorageGet(void **ptr, u64 *elsSize);

/**
 * @brief Get the GUID of the calling EDT
 *
 * @return 0, upon success and OCR_EINVAL if the pointer is invalid
 **/
u8 ocrCurrentEdtGet(ocrGuid_t * curEdt);

/**
 * @brief Get the GUID of the output event of executing EDT
 *
 * @return 0, upon success and OCR_EINVAL if the pointer is invalid
 **/
u8 ocrCurrentEdtOutputGet(ocrGuid_t * outputEvent);

/**
 * @brief Get the number of workers the runtime currently uses
 *
 * @note Exposed as a convenience to runtime implementors,
 * may be deprecated anytime.
 *
 * @return the number of workers
 **/
u64 ocrNbWorkers();

/**
 * @brief Get the GUID of the calling worker
 *
 * @note Exposed as a convenience to runtime implementors,
 * may be deprecated anytime.
 *
 * @return the GUID of the calling worker
 **/
void ocrCurrentWorkerGuid(ocrGuid_t * guid);

/**
 * @brief Inform the OCR runtime that the currently
 * executing thread is logically blocked
 *
 * @note Very experimental
 **/
u8 ocrInformLegacyCodeBlocking();

/**
 * @}
 * @}
 */
#ifdef __cplusplus
}
#endif

#endif /* ENABLE_EXTENSION_RTITF */
#endif /* OCR_RUNTIME_ITF_H_ */
