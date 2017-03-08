/**
 * @brief Extensions for querying additional data block properties.
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_DB_INFO_H__
#define __OCR_DB_INFO_H__

#ifdef ENABLE_EXTENSION_DB_INFO

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup OCRExt
 * @{
 */
/**
 * @defgroup OCRExtDbInfo Additional APIs for info on data blocks.
 * @brief Additional APIs for info on data blocks.
 *
 * @{
 **/

/**
 * @brief Get size of an acquired DB
 *
 * @param[in] db Data block to query
 *
 * @param[out] size of the data block (in bytes)
 *
 * @return a status code:
 *      - 0: successful
 *      - EINVAL: db does not refer to a valid data block
 *      - EACCES: current EDT has not acquired the data block
 *
 * @warning ocrDbGetSize should only be called on a currently-acquired data block
 */
u8 ocrDbGetSize(ocrGuid_t db, u64 *size);

/**
 * @}
 * @}
 */
#ifdef __cplusplus
}
#endif

#endif /* ENABLE_EXTENSION_DB_INFO */
#endif /* __OCR_DB_INFO_H__ */
