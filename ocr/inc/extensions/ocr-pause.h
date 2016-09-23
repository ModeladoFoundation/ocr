/*
 * @brief OCR APIs for pause, query, and resume of runtime
 */

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_PAUSE_H__
#define __OCR_PAUSE_H__
#ifdef ENABLE_EXTENSION_PAUSE

#ifdef __cplusplus
extern "C" {
#endif

#include "ocr-types.h"

/**
 * @brief Halts forward progress of all workers to allow querying of current
 * runtime contents.
 *
 * @This call will cause workers to suspend all future EDT execution
 * until ocrResume() is called, where execution will then continue normally
 *
 * @param[in] isBlocking   Flag to indicate whether to perform a blocking pause
 *
 * @return a status code
 *      - 0: not paused by caller
 *      - 1: paused by caller
 *
 **/
u32 ocrPause(bool isBlocking);

/**
 * @brief queries the contents of runtime while runtime is paused
 *
 * The query results are put in a datablock of a suitable size, which is the
 * caller's responsibility to destroy.
 *
 * @note If ocrQuery() gets called while runtime is not paused it will be
 * ignored.
 *
 * @param[in] query     The type of query performed. See inc/ocr-types.h
 * @param[in] guid      Guid of previous <query type>, NULL_GUID for first
 * @param[out] result   Pointer to the results of the query
 * @param[out] size     Size of query result (datablock) in bytes
 * @param[in] flags     flags vary based on query type
 *                      TODO: work toward defining these as query evolves
 *
 * @return a guid of datablock created to hold the query results
 **/
ocrGuid_t ocrQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags);

/**
 * @brief Continues execution of a paused OCR program.
 *
 * @This call will restart suspended workers
 *
 * @note If ocrResume() is called while a paused runtime state is not detected
 * the call will effectively be ignored.
 *
 * @param[in] flag      Currently unused, placeholder for future control
 *
 **/
void ocrResume(u32 flag);

/**
 * User driven fault injection
 **/
void ocrInjectFault();

#ifdef __cplusplus
}
#endif

#endif /* ENABLE_EXTENSION_PAUSE */

#endif /* __OCR_PAUSE_H__ */
