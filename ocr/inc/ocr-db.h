/**
 * @brief Data block management API for OCR
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_DB_H__
#define __OCR_DB_H__
#ifdef __cplusplus
extern "C" {
#endif

#include "ocr-types.h"
#include "ocr-version.h"

/**
 * @defgroup OCRDataBlock Data-block management for OCR
 * @brief Describes the data block API for OCR
 *
 * Data blocks are the only form of non-ephemeral storage
 * and are therefore the only way to "share" data between
 * EDTs. Conceptually, data blocks are contiguous chunks
 * of memory that have a start address and a size. They also
 * have the following characteristics:
 *   - all memory within the data block is accessible from the
 *     start-address using an offset (ie: addresses
 *     [start-address; start-address+size[ uniquely and totally
 *     address the entire data-block)
 *   - non-overlaping with other distinct data blocks
 *   - the pointer to the start of a data block is only
 *     valid between the acquire of the data-block (implicit when
 *     the EDT starts) and the corresponding ocrDbRelease call (or the
 *     end of the EDT, whichever comes first)
 *
 * @{
 **/


/**
 * @brief Request the creation of a data block
 *
 * On successful creation, the returned memory location will
 * be 8 byte aligned. ocrDbCreate also implicitly acquires the data-block
 * for the calling EDT unless DB_PROP_NO_ACQUIRE is specified
 *
 * @param[out] db        On successful creation, contains the GUID for the newly created
 *                       data block. Will be NULL if the call fails
 * @param[out] addr      On successful creation, contains the 64 bit address
 *                       if DB_PROP_NO_ACQUIRE is not specified. NULL otherwise
 * @param[in] len        Size in bytes of the block to allocate.
 * @param[in] flags      Flags to create the data block. Currently, the following
 *                       properties are supported:
 *                       - #DB_PROP_NONE, indicates no specific properties
 *                       - #DB_PROP_NO_ACQUIRE, the DB will be created but not
 *                         acquired (addr will be NULL).
 * @param[in,out] hint   The hint value passed to the datablock, can be NULL_HINT
 * @param[in] allocator  Allocator to use to allocate within the data block. Supported
 *                       values are given by #ocrInDbAllocator_t
 *
 * @return a status code:
 *      - -     : successful
 *      - ENXIO : hint is invalid
 *      - ENOMEM: allocation failed because of insufficient memory
 *      - EINVAL: invalid arguments (flags or something else)
 *      - EBUSY : the agent that is needed to process this request is busy. Retry is possible.
 *      - EPERM : trying to allocate in an area of memory that is not allowed
 *
 * @note The default allocator (NO_ALLOC) will disallow calls to ocrDbMalloc and ocrDbFree.
 * If an allocator is used, part of the data block's space will be taken up by the
 * allocator's management overhead
 *
 **/
u8 ocrDbCreate(ocrGuid_t *db, void** addr, u64 len, u16 flags, ocrHint_t *hint,
               ocrInDbAllocator_t allocator);
/**
 * @brief Request for the destruction of a data block
 *
 * The EDT does not need to have acquired the data block to destroy it.
 * ocrDbDestroy() will request destruction of the DB but the DB will only be destroyed
 * once all other EDTs that have acquired it release it (either implicitly
 * at the end of the EDT or with an explicit ocrDbRelease() call).
 *
 * @note If the EDT has acquired this DB, this call implicitly
 * releases the DB.
 *
 * Once a data block has been marked as 'to-be-destroyed' by this call, the following
 * operations on the same data block will result in an error:
 * - calling ocrDbDestroy() again (will return EPERM)
 * The following operations will produce undefined behavior:
 *  - accessing the actual location of the data block (through a pointer)
 *  - acquiring the data block (implicit on EDT start)
 *
 * @param[in] db  Data block to destroy
 * @return a status code:
 *      - 0: successful
 *      - EPERM: db was already destroyed
 *      - EINVAL: db does not refer to a valid data-block
 */
u8 ocrDbDestroy(ocrGuid_t db);

/**
 * @brief Release the DB (indicates that the EDT no longer needs to access it)
 *
 * This call can be used to indicate an early release
 * of the data block (ie: it is not needed in the rest of the EDT).
 * Once the data block is released, pointers that were previously
 * associated with it are invalid and should not be used to access the data.
 *
 * The functionality of ocrDbRelease() is implicitly contained in:
 * - ocrDbDestroy()
 * - EDT exit
 *
 * @note ocrDbRelease should only be called *once* at most.
 *
 * @param[in] db Data block to release
 *
 * @return a status code:
 *      - 0: successful
 *      - EINVAL: db does not refer to a valid data block
 *      - EACCES: EDT has not acquired the data block and therefore cannot
 *        release it
 */
u8 ocrDbRelease(ocrGuid_t db);

/**
 * @brief Allocates memory *inside* a data block in a way similar to malloc
 *
 * This will allocate a chunk of size 'size' and return its address
 * in 'addr' using the memory available in the data block. This call, and
 * others related to it, allow you to use a data block in a heap-like fashion
 *
 * @param[in] guid     Data block to malloc from
 * @param[in] size     Size of the chunk to allocate
 * @param[out] addr    Address to the chunk allocated or NULL on failure
 *
 * @return a status code
 *      - 0: successful
 *      - ENOMEM: Not enough space to allocate
 *      - EINVAL: Data block does not support allocation
 *
 * @warning The address returned is valid *only* for the current
 * acquire of the data block (ie: it is an absolute address).
 * Use ocrDbMallocOffset() to get a more stable 'pointer'
 *
 * @note This call is not supported at this time.
 */
u8 ocrDbMalloc(ocrGuid_t guid, u64 size, void** addr);

/**
 * @brief Allocates memory *inside* a data block in a way similar to malloc
 *
 * This call is very similar to ocrDbMalloc except that it returns
 * the location of the memory allocated as an *offset* from the start
 * of the data block. This is a more preferred method as this allows
 * the returned pointer to be used across EDTs (provided they all have
 * access to the data block).
 *
 * @param[in] guid          Data block to malloc from
 * @param[in] size          Size of the chunk to allocate
 * @param[out] offset       Offset of the chunk allocated in the data block
 *
 * @return a status code
 *      - 0: successful
 *      - ENOMEM: Not enough space to allocate
 *      - EINVAL: Data block does not support allocation
 *
 * @note This call is not supported at this time.
 */
u8 ocrDbMallocOffset(ocrGuid_t guid, u64 size, u64* offset);

/**
 * @brief Frees memory allocated through ocrDbMalloc()
 *
 * @param[in] guid          Data block to free from
 * @param[in] addr          Address to free (as returned by ocrDbMalloc())
 *
 * @return a status code
 *      - 0: successful
 *      - EINVAL: Data block does not support allocation or addr
 *                is invalid
 *
 * @warning The address 'addr' must have been
 * allocated before the release of the containing data block. Use
 * ocrDbFreeOffset if allocating and freeing across EDTs for
 * example
 *
 * @note This call is not supported at this time.
 */
u8 ocrDbFree(ocrGuid_t guid, void* addr);

/**
 * @brief Frees memory allocated through ocrDbMallocOffset()
 *
 * @param[in] guid          Data block to free from
 * @param[in] offset        Offset to free (as returned by
 *                          ocrDbMallocOffset())
 *
 * @return a status code
 *      - 0: successful
 *      - EINVAL: Data block does not support allocation or
 *                offset is invalid
 *
 * @note This call is not supported at this time.
 */
u8 ocrDbFreeOffset(ocrGuid_t guid, u64 offset);

/**
 * @brief Copies data between two data blocks in an asynchronous manner
 *
 * This call will trigger the creation of an EDT which will perform a copy from
 * a source data block into a destination data block. Once the copy is complete,
 * the event with GUID 'completionEvt'' will be satisfied. That event will carry
 * the destination data block.
 *
 * The type of GUID passed in as source also determines the starting point of
 * the copy:
 *    - if it is an event GUID, the EDT will be available to run when that
 *      event is satisfied. The data block carried by that event will be
 *      used as the source data block
 *    - if it is a data block GUID, the EDT is immediately available to run
 *      and will be used as the source data block
 *
 * @param[in] destination       Data block to copy to (must already be created
 *                              and large enough to contain copy)
 * @param[in] destinationOffset Offset from the start of the destination data block
 *                              to copy to (in bytes)
 * @param[in] source            Data block to copy from
 * @param[in] sourceOffset      Offset from the start of the destination data block
 *                              to copy from (in bytes)
 * @param[in] size              Number of bytes to copy
 * @param[in] copyType          Reserved
 * @param[out] completionEvt    GUID of the event that will be satisfied when the
 *                              copy is successful
 *
 * @return a status code
 *      - 0: successful (note that this does not mean that the copy was done)
 *      - EINVAL: Invalid values for one of the arguments
 *      - EPERM: Overlapping data blocks
 *      - ENOMEM: Destination too small to copy into or source too small to copy from
 *
 * @note This call is not supported at this time.
 */
u8 ocrDbCopy(ocrGuid_t destination, u64 destinationOffset, ocrGuid_t source,
             u64 sourceOffset, u64 size, u64 copyType, ocrGuid_t * completionEvt);

/**
 * @}
 */
#ifdef __cplusplus
}
#endif

#endif /* __OCR_DB_H__ */
