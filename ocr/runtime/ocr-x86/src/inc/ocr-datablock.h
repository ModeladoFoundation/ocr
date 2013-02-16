/**
 * @brief OCR interface to the datablocks
 * @authors Romain Cledat, Intel Corporation
 * @date 2012-09-21
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
*/


#ifndef __OCR_DATABLOCK_H__
#define __OCR_DATABLOCK_H__

#include "ocr-types.h"
#include "ocr-guid.h"
#include "ocr-allocator.h"

/**
 * @brief Internal description of a data-block.
 *
 * This describes the internal representation of
 * a data-block and the meta-data that is associated
 * with it for book-keeping
 *
 **/
typedef struct _ocrDataBlock_t {
    ocrGuid_t guid;
    /**
     * @brief Creates a data-block to represent the memory of size 'size'
     * at address 'address'
     *
     * The constructor does not allocate any memory nor will
     * any memory be freed on destruction. This just creates the
     * meta-data associated with the data-block and gets a GUID for it
     *
     * @param self          Pointer for this data-block
     * @param allocator     Allocator from where this data-block was created (its GUID)
     * @param size          Size in bytes of the data-block
     * @param flags         Data-block flags (unused)
     */
    void (*create)(struct _ocrDataBlock_t *self,
                   ocrGuid_t allocator,
                   u64 size, u16 flags, void* configuration);

    /**
     * @brief Destroys a data-block
     *
     * Again, this does not perform any de-allocation
     * but makes the meta-data invalid and informs
     * the GUID system that the GUID for this data-block
     * is no longer used
     *
     * @param self          Pointer for this data-block
     */
    void (*destruct)(struct _ocrDataBlock_t *self);

    /**
     * @brief Acquires the data-block for an EDT
     *
     * This call registers a user (the EDT) for the data-block
     *
     * @param self          Pointer for this data-block
     * @param edt           EDT seeking registration
     * @param isInternal    True if this is an acquire implicitly
     *                      done by the runtime at EDT launch
     * @return Address of the data-block
     *
     * @note Multiple acquires for the same EDT have no effect
     */
    void* (*acquire)(struct _ocrDataBlock_t *self, ocrGuid_t edt, bool isInternal);

    /**
     * @brief Releases a data-block previously acquired
     *
     * @param self          Pointer for this data-block
     * @param edt           EDT seeking to de-register from the data-block.
     * @param isInternal    True if matching an internal acquire
     * @return 0 on success and an error code on failure (see ocr-db.h)
     *
     * @note No need to match one-to-one with acquires. One release
     * releases any and all previous acquires
     */
    u8 (*release)(struct _ocrDataBlock_t *self, ocrGuid_t edt, bool isInternal);

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
     * @return 0 on success and an error code on failure (see ocr-db.h)
     */
    u8 (*free)(struct _ocrDataBlock_t *self, ocrGuid_t edt);
} ocrDataBlock_t;

/**
 * @brief Enum defining the type of data-blocks supported
 * by the runtime
 *
 * If extending the runtime, add your type here and
 * modify the newDataBlock function as well
 */
typedef enum _ocrDataBlockKind {
    OCR_DATABLOCK_DEFAULT = 0,
    OCR_DATABLOCK_REGULAR = 1
} ocrDataBlockKind;

extern ocrDataBlockKind ocrDataBlockDefaultKind;

/**
 * @brief Allocats a new data-block of the type specified
 *
 * The user will need to call "create" on the data-block to
 * properly initialize it
 *
 * @param type              Type of the data-block to return
 * @return A pointer to the meta-data for the data-block
 */
ocrDataBlock_t* newDataBlock(ocrDataBlockKind type);

#endif /* __OCR_DATABLOCK_H__ */
