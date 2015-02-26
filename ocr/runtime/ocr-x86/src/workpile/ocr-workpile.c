/* Copyright (c) 2012, Rice University

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1.  Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   2.  Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided
   with the distribution.
   3.  Neither the name of Intel Corporation
   nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific
   prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include <stdlib.h>
#include <assert.h>

#include "ocr-types.h"
#include "ocr-workpile.h"

//TODO don't like that: I think we should have some kind of a plugin
// mechanism where an implementation can associate its string to a function pointer.
// i.e HC_WORKPOOL -> hc_workpile_constructor
extern ocr_workpile_t * hc_workpile_constructor();
extern ocr_workpile_t * ce_message_workpile_constructor();

extern ocr_workpile_t * dequeish_heap_steal_last_workpile_constructor();
extern ocr_workpile_t * dequeish_heap_steal_half_workpile_constructor();
extern ocr_workpile_t * dequeish_heap_counting_steal_half_workpile_constructor();

extern ocr_workpile_t * event_priority_workpile_steal_half_constructor();
extern ocr_workpile_t * event_priority_workpile_counting_steal_half_constructor();
extern ocr_workpile_t * event_priority_workpile_steal_last_constructor();
extern ocr_workpile_t * event_priority_workpile_steal_selfish_constructor();
extern ocr_workpile_t * event_priority_workpile_steal_altruistic_constructor();

extern ocr_workpile_t * data_priority_workpile_steal_half_constructor();
extern ocr_workpile_t * data_priority_workpile_counting_steal_half_constructor();
extern ocr_workpile_t * data_priority_workpile_steal_last_constructor();
extern ocr_workpile_t * data_priority_workpile_steal_selfish_constructor();
extern ocr_workpile_t * data_priority_workpile_steal_altruistic_constructor();

extern ocr_workpile_t * user_priority_workpile_steal_half_constructor();
extern ocr_workpile_t * user_priority_workpile_counting_steal_half_constructor();
extern ocr_workpile_t * user_priority_workpile_steal_last_constructor();
extern ocr_workpile_t * user_priority_workpile_steal_altruistic_constructor();

extern ocr_workpile_t * event_sorted_priority_workpile_steal_half_constructor();
extern ocr_workpile_t * event_sorted_priority_workpile_counting_steal_half_constructor();
extern ocr_workpile_t * event_sorted_priority_workpile_steal_last_constructor();
extern ocr_workpile_t * event_sorted_priority_workpile_steal_selfish_constructor();
extern ocr_workpile_t * event_sorted_priority_workpile_steal_altruistic_constructor();

extern ocr_workpile_t * data_sorted_priority_workpile_steal_half_constructor();
extern ocr_workpile_t * data_sorted_priority_workpile_counting_steal_half_constructor();
extern ocr_workpile_t * data_sorted_priority_workpile_steal_last_constructor();
extern ocr_workpile_t * data_sorted_priority_workpile_steal_selfish_constructor();
extern ocr_workpile_t * data_sorted_priority_workpile_steal_altruistic_constructor();

extern ocr_workpile_t * user_sorted_priority_workpile_steal_half_constructor();
extern ocr_workpile_t * user_sorted_priority_workpile_counting_steal_half_constructor();
extern ocr_workpile_t * user_sorted_priority_workpile_steal_last_constructor();
extern ocr_workpile_t * user_sorted_priority_workpile_steal_altruistic_constructor();

ocr_workpile_t * newWorkpile(ocr_workpile_kind workpileType) {
    switch(workpileType) {
    case OCR_DEQUE:
        return hc_workpile_constructor();
    case OCR_DEQUEISH_HEAP_STEAL_LAST_KIND:
        return dequeish_heap_steal_last_workpile_constructor();
    case OCR_DEQUEISH_HEAP_STEAL_HALF_KIND:
        return dequeish_heap_steal_half_workpile_constructor();
    case OCR_DEQUEISH_HEAP_COUNTING_STEAL_HALF_KIND:
        return dequeish_heap_counting_steal_half_workpile_constructor();

    case OCR_EVENT_PRIORITY_QUEUE_STEAL_HALF_KIND:
        return event_priority_workpile_steal_half_constructor();
    case OCR_EVENT_PRIORITY_QUEUE_COUNTING_STEAL_HALF_KIND:
        return event_priority_workpile_counting_steal_half_constructor();
    case OCR_EVENT_PRIORITY_QUEUE_STEAL_LAST_KIND:
        return event_priority_workpile_steal_last_constructor();
    case OCR_EVENT_PRIORITY_QUEUE_STEAL_SELFISH_KIND:
        return event_priority_workpile_steal_selfish_constructor();
    case OCR_EVENT_PRIORITY_QUEUE_STEAL_ALTRUISTIC_KIND:
        return event_priority_workpile_steal_altruistic_constructor();

    case OCR_DATA_PRIORITY_QUEUE_STEAL_HALF_KIND:
        return data_priority_workpile_steal_half_constructor();
    case OCR_DATA_PRIORITY_QUEUE_COUNTING_STEAL_HALF_KIND:
        return data_priority_workpile_counting_steal_half_constructor();
    case OCR_DATA_PRIORITY_QUEUE_STEAL_LAST_KIND:
        return data_priority_workpile_steal_last_constructor();
    case OCR_DATA_PRIORITY_QUEUE_STEAL_SELFISH_KIND:
        return data_priority_workpile_steal_selfish_constructor();
    case OCR_DATA_PRIORITY_QUEUE_STEAL_ALTRUISTIC_KIND:
        return data_priority_workpile_steal_altruistic_constructor();

    case OCR_USER_PRIORITY_QUEUE_STEAL_HALF_KIND:
        return user_priority_workpile_steal_half_constructor();
    case OCR_USER_PRIORITY_QUEUE_COUNTING_STEAL_HALF_KIND:
        return user_priority_workpile_counting_steal_half_constructor();
    case OCR_USER_PRIORITY_QUEUE_STEAL_LAST_KIND:
        return user_priority_workpile_steal_last_constructor();
    case OCR_USER_PRIORITY_QUEUE_STEAL_ALTRUISTIC_KIND:
        return user_priority_workpile_steal_altruistic_constructor();

    case OCR_EVENT_SORTED_PRIORITY_QUEUE_STEAL_HALF_KIND:
        return event_sorted_priority_workpile_steal_half_constructor();
    case OCR_EVENT_SORTED_PRIORITY_QUEUE_COUNTING_STEAL_HALF_KIND:
        return event_sorted_priority_workpile_counting_steal_half_constructor();
    case OCR_EVENT_SORTED_PRIORITY_QUEUE_STEAL_SELFISH_KIND:
        return event_sorted_priority_workpile_steal_selfish_constructor();
    case OCR_EVENT_SORTED_PRIORITY_QUEUE_STEAL_ALTRUISTIC_KIND:
        return event_sorted_priority_workpile_steal_altruistic_constructor();

    case OCR_DATA_SORTED_PRIORITY_QUEUE_STEAL_HALF_KIND:
        return data_sorted_priority_workpile_steal_half_constructor();
    case OCR_DATA_SORTED_PRIORITY_QUEUE_COUNTING_STEAL_HALF_KIND:
        return data_sorted_priority_workpile_counting_steal_half_constructor();
    case OCR_DATA_SORTED_PRIORITY_QUEUE_STEAL_SELFISH_KIND:
        return data_sorted_priority_workpile_steal_selfish_constructor();
    case OCR_DATA_SORTED_PRIORITY_QUEUE_STEAL_ALTRUISTIC_KIND:
        return data_sorted_priority_workpile_steal_altruistic_constructor();

    case OCR_USER_SORTED_PRIORITY_QUEUE_STEAL_HALF_KIND:
        return user_sorted_priority_workpile_steal_half_constructor();
    case OCR_USER_SORTED_PRIORITY_QUEUE_COUNTING_STEAL_HALF_KIND:
        return user_sorted_priority_workpile_counting_steal_half_constructor();
    case OCR_USER_SORTED_PRIORITY_QUEUE_STEAL_ALTRUISTIC_KIND:
        return user_sorted_priority_workpile_steal_altruistic_constructor();

    case OCR_MESSAGE_QUEUE:
        return ce_message_workpile_constructor();
    default:
        assert(false && "Unrecognized workpile kind");
        break;
    }

    return NULL;
}


/******************************************************/
/* OCR Workpile iterator                              */
/******************************************************/

bool workpile_iterator_hasNext (workpile_iterator_t * base) {
    return base->id != base->curr;
}

ocr_workpile_t * workpile_iterator_next (workpile_iterator_t * base) {
    int next = (base->curr+1) % base->mod;
    ocr_workpile_t * to_be_returned = base->array[base->curr];
    base->curr = next;
    return to_be_returned;
}

workpile_iterator_t* workpile_iterator_constructor ( int i, size_t n_pools, ocr_workpile_t ** pools ) {
    workpile_iterator_t* it = (workpile_iterator_t *) malloc(sizeof(workpile_iterator_t));
    it->array = pools;
    it->id = i;
    it->curr = (i+1)%n_pools;
    it->mod = n_pools;
    it->hasNext = workpile_iterator_hasNext;
    it->next = workpile_iterator_next;
    return it;
}

void workpile_iterator_destructor (workpile_iterator_t* base) {
    free(base);
}
