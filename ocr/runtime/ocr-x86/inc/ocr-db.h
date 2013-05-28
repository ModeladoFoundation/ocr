/**
 * @brief Data-block management API for OCR
 * @authors Romain Cledat, Intel Corporation
 * @date 2012-09-21
 *
 * Copyright (c) 2012, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the
 *       distribution.
 *    3. Neither the name of Intel Corporation nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **/


#ifndef __OCR_DB_H__
#define __OCR_DB_H__

#include "ocr-types.h"

/**
 * @defgroup OCRDataBlock Data-block management for OCR
 * @brief Describes the data-block API for OCR
 *
 * Data-blocks are the only form of non-ephemeral storage
 * and are therefore the only way to "share" data between
 * EDTs. Conceptually, data-blocks are contiguous chunks
 * of memory that have a start address and a size. They also
 * have the following characteristics:
 *   - all memory within the data-block is accessible from the
 *     start-address using an offset (ie: addresses
 *     [start-address; start-address+size[ uniquely and totally
 *     address the entire data-block)
 *   - non-overlaping
 *   - the pointer to the start of a data-block is only
 *     valid between the acquire of the data-block (implicit when
 *     the EDT starts) and the corresponding ocrDbRelease call (or the
 *     end of the EDT, whichever comes first)
 *
 * @{
 **/

/**
 * @brief Request the creation of a data-block
 *
 * On successful creation, the returned memory location will
 * be 8 byte aligned. ocrDbCreate also implicitly acquires the data-block
 * for the calling EDT
 *
 * @param db        On successful creation, contains the GUID for the newly created data-block.
 *                  Will be NULL if the call fails
 * @param addr      On successful creation, contains the 64 bit address of the start of the data-block
 * @param len       Size in bytes of the block to allocate
 * @param flags     Reserved for future use
 * @param location  Used as input to supply a hint to the runtime concerning the
 *                  location to allocate the memory;
 *                  on successful allocation will contain the location of
 *                  the actual allocation RAM, SP, or something NUMA. If NULL,
 *                  allocate in a default location and does not give any information
 *                  on the actual location
 * @param allocator Allocator to use to allocate memory within the data-block. If not NO_ALLOC,
 *                  on creation, the data-block will be properly initialized to be used as
 *                  a heap for the allocator specified
 *
 * @return a status code on failure and 0 on success. On failure will return one of:
 *      - ENXIO:  location does not exist
 *      - ENOMEM: allocation failed because of insufficient memory or too constraining constraints
 *      - EINVAL: invalid arguments (flags or something else)
 *      - EBUSY : the agent that is needed to process this request is busy. Retry is possible.
 *      - EPERM : trying to allocate in an area of memory that is not allowed
 *
 * @note The flags are currently unused but are reserved for future use
 * @note The default allocator (NO_ALLOC) will disallow calls to ocrDbMalloc and ocrDbFree.
 * If an allocator is used, part of the data-block's space will be taken up by the
 * allocator's management overhead
 *
 **/
u8 ocrDbCreate(ocrGuid_t *db, void** addr, u64 len, u16 flags,
               ocrLocation_t *location, ocrInDbAllocator_t allocator);

/**
 * @brief Request for the destruction of a data-block
 *
 * The EDT does not need to have acquired the data-block to destroy it.
 * ocrDbDestroy will request destruction of the DB but the DB will only be destroyed
 * once all other EDTs that have acquired it, release it.
 *
 * Note that if the EDT has acquired this DB, this call implicitly
 * releases the DB.
 *
 * Once a DB has been marked as 'to-be-destroyed' by this call, the following
 * operations will result in an error:
 *      - acquiring the DB (will return EPERM)
 *      - re-destroying the DB (will return EPERM)
 * The following operations will produce undefined behavior:
 *      - accessing the actual location of the DB (through a pointer)
 *
 * @param db  Used as input to determine the DB to be destroyed
 * @return A status code
 *      - 0: successful
 *      - EPERM: DB cannot be destroyed because it was already destroyed
 *      - EINVAL: db does not refer to a valid data-block
 */
u8 ocrDbDestroy(ocrGuid_t db);

/**
 * @brief To access a DB, an EDT needs to "acquire" it to
 * obtain a physical address (pointer) through which it should access
 * the DB.
 *
 * ocrDbAcquire performs this function and returns a pointer to the
 * base of the DB that is valid until either:
 *      - the EDT calls ocrDbRelease()
 *      - the EDT calls ocrDbDestroy()
 *
 * The functionality of ocrDbAcquire is implicitly contained in:
 *      - ocrDbCreate()
 *      - EDT entry for the input data-blocks
 *
 * Multiple calls to this function are equivalent to a single call (provided
 * there are no intervening releases).
 *
 * @param db        DB to acquire an address to
 * @param addr      On successful completion, contains the address to use for the data-block
 * @param mode      Flags specifying how the DB will be accessed (unused for now)
 *
 * @return The status of the operation:
 *      - 0: successful
 *      - EPERM: DB cannot be registered as it has been destroyed
 *      - EINVAL: The 'db' parameter does not refer to a valid data-block
 *
 * @warning No exclusivity of access is guaranteed by the call
 * @deprecated This call is being phased out and all DB acquisitions are to occur
 * on DB start; you will no longer be able to acquire DBs that are not
 * explicitly passed as input to an EDT.
 */
u8 ocrDbAcquire(ocrGuid_t db, void** addr, u16 flags);

/**
 * @brief Release the DB (indicates that the EDT no longer needs to access it)
 *
 * Call should be used to indicate an early release
 * of the DB (ie: it is not needed in the rest of the EDT).
 * Once the DB is released, pointers that were previously
 * associated with it are invalid and should not be used to access the data.
 *
 * The functionality of ocrDbRelease is implicitly contained in:
 *      - ocrDbDestroy()
 *      - EDT exit (for EDTs implicitly acquired on entry)
 *
 * @param db DB to release
 *
 * @return The status of the operation.
 *      - 0: successful
 *      - EINVAL: db does not refer to a valid data-block
 *      - EACCES: EDT is not registered with the data-block and therefore cannot
 *        release it
 */
u8 ocrDbRelease(ocrGuid_t db);

/**
 * @brief Allocates memory *inside* a data-block in a way similar to malloc
 *
 * This will allocate a chunk of size 'size' and return its address
 * in 'addr' using the memory available in the data-block
 *
 * @param guid              DB to malloc from
 * @param size              Size of the chunk to allocate
 * @param addr              Address to the chunk allocated or NULL on failure
 *
 * @return The status of the operation:
 *      - 0: successful
 *      - ENOMEM: Not enough space to allocate
 *      - EINVAL: DB does not support allocation
 *
 * @warning The address returned is valid *only* until the data-block is
 * released.
  */
u8 ocrDbMalloc(ocrGuid_t guid, u64 size, void** addr);

/**
 * @brief Allocates memory *inside* a data-block in a way similar to malloc
 *
 * This call is very similar to ocrDbMalloc except that it returns
 * the location of the memory allocated as an *offset* from the start
 * of the data-block. This is the preferred method.
 *
 * @param guid              DB to malloc from
 * @param size              Size of the chunk to allocate
 * @param offset            Offset of the chunk allocated in the data-block
 *
 * @return The status of the operation:
 *      - 0: successful
 *      - ENOMEM: Not enough space to allocate
 *      - EINVAL: DB does not support allocation
 *
 */
u8 ocrDbMallocOffset(ocrGuid_t guid, u64 size, u64* offset);

/**
 * @brief Frees memory allocated through ocrDbMalloc
 *
 * @param guid              DB to free from
 * @param addr              Address to free
 *
 * @warning The address passed here must have been
 * allocated before the release of the containing data-block. Use
 * ocrDbFreeOffset if allocating and freeing across EDTs for
 * example
 */
u8 ocrDbFree(ocrGuid_t guid, void* addr);

/**
 * @brief Frees memory allocated through ocrDbMallocOffset
 *
 * @param guid              DB to free from
 * @param offset            Offset to free
 */
u8 ocrDbFreeOffset(ocrGuid_t guid, u64 offset);

/**
 * @brief Copies data between two data-blocks in an asynchronous manner
 *
 * This call will trigger the creation of an EDT which will perform a copy from a source data-block
 * into a destination data-block. Once the copy is complete,
 * the event with GUID 'completionEvt'' will be satisfied. That event will carry the destination data-block.
 *
 * The type of GUID passed in as source also determines the starting point of the copy:
 *    - if it is an event GUID, the EDT will be available to run when that event is satisfied. The data-block carried by
 *       that event will be used as the source data-block
 *    - if it is a data-block GUID, the EDT is immediately available to run.
 *
 * @param destination           GUID of the data-block to copy to
 * @param destinationOffset     Starting offset within the destination to copy to
 * @param source                GUID of an event or data-block to copy from
 * @param sourceOffset          Starting offset within the source to copy from
 * @param size                  Number of bytes to copy
 * @param copyType              Reserved for future use
 * @param completionEvt         If non NULL, returns the GUID of the event that will be
 *                              satisfied when the copy is completed.
 *
 * @return 0 on success or the following error codes:
 *    - EINVAL: Invalid values for one of the arguments
 *    - EPERM: Overlapping data-blocks
 *    - ENOMEM: Destination too small to copy into or source too small to copy from
 */
u8 ocrDbCopy(ocrGuid_t destination, u64 destinationOffset, ocrGuid_t source, u64 sourceOffset, u64 size, u64 copyType, ocrGuid_t * completionEvt);

/**
 * @}
 */
#endif /* __OCR_DB_H__ */
