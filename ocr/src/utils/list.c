/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"

#include "ocr-hal.h"
#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-types.h"
#include "utils/list.h"

#define DEBUG_TYPE UTIL

//
// Linked List Implementation
//

typedef struct _listnode {
    void * elt;
    struct _listnode * next;
} listnode_t;

bool linkedListEmpty(linkedlist_t * list) {
    return list->head == NULL;
}

void linkedListPushFront(linkedlist_t * list, void * elt) {
    ocrPolicyDomain_t * pd = list->pd;
    listnode_t * node = pd->fcts.pdMalloc(pd, sizeof(listnode_t));
    ADebug(AllocDebugAllPD, "list.c/linkedListPushFront() "
           "pdMalloc(%ld), addr=%p\n", sizeof(listnode_t), node);
    node->elt = elt;
    node->next = list->head;
    list->head = node;
    if (linkedListEmpty(list)) {
        list->tail = node;
    }
}

void linkedListPushTail(linkedlist_t * list, void * elt) {
    ocrPolicyDomain_t * pd = list->pd;
    listnode_t * node = pd->fcts.pdMalloc(pd, sizeof(listnode_t));
    ADebug(AllocDebugAllPD, "list.c/linkedListPushTail() "
           "pdMalloc(%ld), addr=%p\n", sizeof(listnode_t), node);
    ASSERT((list->tail == NULL) || (list->tail->next == NULL));
    node->elt = elt;
    node->next = NULL;
    if (list->tail == NULL) {
        list->head = list->tail = node;
    } else {
        list->tail->next = node;
        list->tail = node;
    }
}

//Fwd decl
iterator_t * newLinkedListIterator(linkedlist_t *self);

void linkedListDestruct(linkedlist_t *self) {
    ocrPolicyDomain_t * pd = self->pd;
    listnode_t * head = self->head;
    while(head != NULL) {
        listnode_t * curr = head;
        head = curr->next;
        pd->fcts.pdFree(pd, curr);
    }
    pd->fcts.pdFree(pd, self);
}

linkedlist_t * newLinkedList(ocrPolicyDomain_t *pd) {
    linkedlist_t * list = (linkedlist_t *) pd->fcts.pdMalloc(pd, sizeof(linkedlist_t));
    ADebug(AllocDebugAllPD, "list.c/newLinkedList() "
           "pdMalloc(%ld), addr=%p\n", sizeof(linkedlist_t), list);
    list->pd = pd;
    list->head = NULL;
    list->tail = NULL;
    list->iterator = &newLinkedListIterator;
    list->pushFront = &linkedListPushFront;
    list->pushTail = &linkedListPushTail;
    list->isEmpty = &linkedListEmpty;
    list->destruct = &linkedListDestruct;
    return list;
}

//
// Linked List Iterator Implementation
//

typedef struct _linkedlist_t_iterator_t {
    iterator_t base;
    linkedlist_t *list;
    listnode_t * ante;
    listnode_t * prev;
    listnode_t * curr;
} linkedlist_iterator_t;

bool linkedListIteratorReset(iterator_t * iterator) {
    linkedlist_iterator_t * llit = (linkedlist_iterator_t *) iterator;
    llit->ante = NULL;
    llit->prev = NULL;
    llit->curr = llit->list->head;
    return false;
}

bool linkedListIteratorHasNext(iterator_t * iterator) {
    linkedlist_iterator_t * it = (linkedlist_iterator_t *) iterator;
    return (it->curr != NULL);
}

void * linkedListIteratorNext(iterator_t * iterator) {
    linkedlist_iterator_t * it = (linkedlist_iterator_t *) iterator;
    ASSERT(linkedListIteratorHasNext(iterator));
    it->ante = it->prev;
    it->prev = it->curr;
    it->curr = it->curr->next;
    return it->prev->elt;
}

void linkedListIteratorRemove(iterator_t * iterator) {
    // When we remove, we've already called next, so
    // the element to erase is the previous one
    linkedlist_iterator_t * it = (linkedlist_iterator_t *) iterator;
    // first next is NULL | head | head->next
    // first next is head | head+1 | head+2
    // after rm      NULL | head   | head+2
    // next
    //               head | head+2 | head+3
    listnode_t * toErase = it->prev;
    if (it->ante == NULL) {
        it->list->head = it->curr;
        it->prev = NULL;
    } else {
        it->ante->next = it->curr;
        it->prev = it->ante;
        it->ante = NULL;
    }
    if (it->curr == NULL) {
        it->list->tail = it->prev;
    }
    iterator->pd->fcts.pdFree(iterator->pd, toErase);
}

void linkedListIteratorDestruct(iterator_t * iterator) {
    iterator->pd->fcts.pdFree(iterator->pd, iterator);
}

iterator_t * newLinkedListIterator(linkedlist_t *self) {
    ocrPolicyDomain_t * pd = self->pd;
    linkedlist_iterator_t * iterator = (linkedlist_iterator_t *)
        pd->fcts.pdMalloc(pd, sizeof(linkedlist_iterator_t));
    ADebug(AllocDebugAllPD, "list.c/newLinkedListIterator() "
           "pdMalloc(%ld), addr=%p\n", sizeof(linkedlist_iterator_t), iterator);
    iterator->base.pd = pd;
    iterator->base.reset = linkedListIteratorReset;
    iterator->base.hasNext = linkedListIteratorHasNext;
    iterator->base.next = linkedListIteratorNext;
    iterator->base.removeCurrent = linkedListIteratorRemove;
    iterator->base.destruct = linkedListIteratorDestruct;
    iterator->list = self;
    iterator->ante = NULL;
    iterator->prev = NULL;
    iterator->curr = self->head;
    return (iterator_t *) iterator;
}

