/**
 * @brief OCR interface to the low level memory interface
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
 **/



#ifndef __OCR_MEM_PLATFORM_H__
#define __OCR_MEM_PLATFORM_H__

#include "ocr-types.h"
#include "ocr-runtime-def.h"


/****************************************************/
/* OCR MEM PLATFORM FACTORY                         */
/****************************************************/

// Forward declaration
struct ocrMemPlatform_t;

typedef struct ocrMemPlatformFactory_t {
    struct ocrMemPlatform_t * (*instantiate) ( struct ocrMemPlatformFactory_t * factory, void * per_type_configuration, void * per_instance_configuration);
    void (*destruct)(struct ocrMemPlatformFactory_t * factory);
} ocrMemPlatformFactory_t;


/****************************************************/
/* OCR MEM PLATFORM API                             */
/****************************************************/

/**
 * @brief Low-level memory provider.
 *
 * This allows low-level memory allocation (such as malloc)
 *
 * @warning Currently only uses malloc with no consideration for the
 * underlying machine but this will change as support for distributed and/or NUMA
 * architecture comes online. The API may therefore evolve
 */
typedef struct ocrMemPlatform_t {
    ocr_module_t module; /**< Base "class" for ocrMemPlatform */

    /**
     * @brief Destructor equivalent
     *
     * @param self          Pointer to this low-memory provider
     */
    void (*destruct)(struct ocrMemPlatform_t* self);

    /**
     * @brief Allocates a chunk of memory for the higher-level
     * allocators to manage
     *
     * @param self          Pointer to this low-memory provider
     * @param size          Size of the chunk to allocate
     * @return Pointer to the chunk of memory allocated
     */
    void* (*allocate)(struct ocrMemPlatform_t* self, u64 size);

    /**
     * @brief Frees a chunk of memory previously allocated
     * by self using allocate
     *
     * @param self          Pointer to this low-memory provider
     * @param addr          Address to free
     */
    void (*free)(struct ocrMemPlatform_t* self, void* addr);
} ocrMemPlatform_t;

typedef enum _ocrMemPlatformKind {
    OCR_MEMPLATFORM_DEFAULT = 0,
    OCR_MEMPLATFORM_MALLOC = 1
} ocrMemPlatformKind;

extern ocrMemPlatformKind ocrMemPlatformDefaultKind;

/**
 * @brief Allocate a new low-memory allocator of the type specified
 *
 * The user will need to call "create" on the returned pointer to properly
 * initialize it.
 *
 * @param type              Type of the low-memory allocator to return
 * @return A pointer to the meta-data for the low-memory allocator
 */
ocrMemPlatform_t* newMemPlatform(ocrMemPlatformKind type, void * per_type_configuration, void * per_instance_configuration);

#endif /* __OCR_MEM_PLATFORM_H__ */
