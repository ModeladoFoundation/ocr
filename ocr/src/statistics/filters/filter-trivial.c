/**
 * @brief A trivial filter that logs everything that comes in to it
 */

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifdef OCR_ENABLE_STATISTICS

#include "debug.h"
#include "filters-macros.h"
#include "ocr-policy-domain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



#define FILTER_NAME TRIVIAL

// A simple filter (simply stores the events and will re-dump them out)
typedef struct {
    u64 tick;
    ocrGuid_t src, dest;
    ocrGuidKind srcK, destK;
    ocrStatsEvt_t type;
} intSimpleMessageNode_t;

START_FILTER
u64 count, maxCount;
intSimpleMessageNode_t *messages;
END_FILTER

FILTER_DESTRUCT {
    FILTER_CAST(rself, self);
    DPRINTF(DEBUG_LVL_VERB, "Destroying trivial filter @ 0x%"PRIx64"\n", (u64)self);
    if(self->parent) {
        DPRINTF(DEBUG_LVL_VERB, "Filter @ 0x%"PRIx64" merging with parent @ 0x%"PRIx64"\n",
        (u64)self, (u64)(self->parent));
        return self->parent->fcts.merge(self->parent, self, 1);
    } else {
        free(rself->messages);
        free(self);
    }
}

FILTER_DUMP {
    FILTER_CAST(rself, self);

    if(rself->count == 0)
        return 0;

    ocrAssert(chunk < rself->count);

    *out = (char*)malloc(sizeof(char)*82); // The output message should always fit in 80 chars given the format

    intSimpleMessageNode_t *tmess = &(rself->messages[chunk]);


    snprintf(*out, sizeof(char)*82, "%"PRId64" : T 0x%"PRIx32" "GUIDF" (%"PRId32") -> "GUIDF" (%"PRId32") ", tmess->tick,
    tmess->type, GUIDA(tmess->src), tmess->srcK, GUIDA(tmess->dest), tmess->destK);

    if(chunk < rself->count - 1)
        return chunk+1;
    return 0;
}

FILTER_NOTIFY {
    FILTER_CAST(rself, self);

    if(rself->count + 1 == rself->maxCount) {
        // Allocate more space
        rself->maxCount *= 2;
        intSimpleMessageNode_t *t = (intSimpleMessageNode_t*)malloc(
            sizeof(intSimpleMessageNode_t)*rself->maxCount);
        memcpy(t, rself->messages, rself->count*sizeof(intSimpleMessageNode_t));
        rself->messages = t;
    }

    DPRINTF(DEBUG_LVL_VVERB, "Filter @ 0x%"PRIx64" received message 0x%"PRIx64" src:"GUIDF" (%"PRId32") dest:"GUIDF" (%"PRId32") type:0x%"PRIx32", now have %"PRId64" messages.\n",
    (u64)self, (u64)mess, GUIDA(mess->src), mess->srcK, GUIDA(mess->dest), mess->destK, (u32)mess->type, rself->count);

    intSimpleMessageNode_t* tmess = &(rself->messages[rself->count++]);
    tmess->tick = mess->tick;
    tmess->src = mess->src;
    tmess->srcK = mess->srcK;
    tmess->dest = mess->dest;
    tmess->destK = mess->destK;
    tmess->type = mess->type;
}

FILTER_MERGE {
    ocrAssert(0);
}

FILTER_CREATE {
    FILTER_MALLOC(rself);
    FILTER_SETUP(rself, parent);

    rself->count = 0;
    rself->maxCount = 8; // Make a configurable number
    DPRINTF(DEBUG_LVL_VERB, "Created a trivial filter @ 0x%"PRIx64" with parent 0x%"PRIx64"\n",
    (u64)rself, (u64)parent);
    rself->messages = (intSimpleMessageNode_t*)malloc(sizeof(intSimpleMessageNode_t)*rself->maxCount);

    return (ocrStatsFilter_t*)rself;
}

#undef FILTER_NAME

#endif /* OCR_ENABLE_STATISTICS */
