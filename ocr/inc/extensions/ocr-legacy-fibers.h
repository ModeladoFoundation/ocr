/**
 * @brief Top level OCR legacy fiber (blocking) support. Include this file if you
 * want to safely support blocking events via fibers.
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_LEGACY_FIBERS_H__
#define __OCR_LEGACY_FIBERS_H__

#ifdef ENABLE_EXTENSION_LEGACY_FIBERS

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup OCRExt
 * @{
 */
/**
 * @defgroup OCRExtLegacyFibers OCR legacy blocking support via fibers
 * @brief API to use OCR from legacy programming models that require fibers
 *
 *
 * @{
 **/

/**
 * @brief Starts a disposable worker fiber on the current worker thread,
 * which is a prerequisite for running suspendable tasks with fibers.
 *
 * @param[in] worker           The current OCR worker thread
 */
void ocrLegacyFiberStart(ocrWorker_t *worker);

/**
 * @brief Suspends the current worker fiber until a given event is satisfied,
 * then acquires and returns the event's playload data block.
 *
 * @param[in] event          The target event's GUID to await
 * @param[in] mode           The desired mode for acquiring the event's playload data block
 * @return                   The GUID and pointer for the satisfied event's playload data block
 */
ocrEdtDep_t ocrLegacyFiberSuspendOnEvent(ocrGuid_t event, ocrDbAccessMode_t mode);

/**
 * @}
 * @}
 */
#ifdef __cplusplus
}
#endif

#endif /* ENABLE_EXTENSION_LEGACY_FIBERS */
#endif /* __OCR_LEGACY_FIBERS_H__ */

