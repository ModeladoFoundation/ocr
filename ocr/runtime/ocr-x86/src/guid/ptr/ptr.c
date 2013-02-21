/**
 * @brief Trivial implementation of GUIDs
 * @authors Romain Cledat, Intel Corporation
 * @date 2012-11-13
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

#include "ptr.h"
#include "stdlib.h"


void ptrCreate(ocrGuidProvider_t* self, void* config) {
    return;
}

void ptrDestruct(ocrGuidProvider_t* self) {
    free(self);
    return;
}

u8 ptrGetGuid(ocrGuidProvider_t* self, ocrGuid_t* guid, u64 val, ocrGuidKind type) {
    *guid = (ocrGuid_t)val;
    return 0;
}

u8 ptrGetVal(ocrGuidProvider_t* self, ocrGuid_t guid, u64* val, ocrGuidKind* kind) {
    *val = (u64)guid;
    if(kind)
        *kind = OCR_GUID_NONE;
    return 0;
}

u8 ptrGetKind(ocrGuidProvider_t* self, ocrGuid_t guid, ocrGuidKind* kind) {
    *kind = OCR_GUID_NONE;
    return 0;
}

u8 ptrReleaseGuid(ocrGuidProvider_t *self, ocrGuid_t guid) {
    return 0;
}

ocrGuidProvider_t* newGuidProviderPtr() {
    ocrGuidProviderPtr_t *result = (ocrGuidProviderPtr_t*)malloc(sizeof(ocrGuidProviderPtr_t));
    result->base.create = &ptrCreate;
    result->base.destruct = &ptrDestruct;
    result->base.getGuid = &ptrGetGuid;
    result->base.getVal = &ptrGetVal;
    result->base.getKind = &ptrGetKind;
    result->base.releaseGuid = &ptrReleaseGuid;

    return (ocrGuidProvider_t*)result;
}
