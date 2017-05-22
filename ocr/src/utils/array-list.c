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
#include "utils/array-list.h"

#define DEBUG_TYPE UTIL

#define ARRAY_CHUNK 64

// Implementation of single and double priority linked list with fixed sized elements.
// Nodes are allocated within array chunks and the node pool is managed by the implementation.
// NOTE: This is *NOT* a concurrent list

typedef struct _arrayChunkNode_t {
    struct _arrayChunkNode_t * next;
} arrayChunkNode_t;

static void newArrayChunkSingle(arrayList_t *list) {
    u32 i;
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    arrayChunkNode_t *chunkNode = (arrayChunkNode_t*) pd->fcts.pdMalloc(pd, sizeof(arrayChunkNode_t) + ((sizeof(slistNode_t) + list->elSize) * list->arrayChunkSize));
    ADebug(AllocDebugAllPD, "array-list/newArrayChunkSingle() "
           "pdMalloc(%ld), addr=%p\n",
           (sizeof(arrayChunkNode_t) + ((sizeof(slistNode_t) + list->elSize) * list->arrayChunkSize)), chunkNode);

    chunkNode->next = list->poolHead;
    list->poolHead = chunkNode;

    u64 ptr = (u64)chunkNode + sizeof(arrayChunkNode_t);
    slistNode_t *headNode = (slistNode_t*)ptr;
    slistNode_t *curNode = headNode;
    for (i = 0; i < list->arrayChunkSize; i++) {
        curNode = (slistNode_t*)ptr;
        curNode->data = list->elSize ? (void*)(ptr + sizeof(slistNode_t)) : NULL;
        ptr += sizeof(slistNode_t) + list->elSize;
        curNode->next = (slistNode_t*)ptr;
    }
    curNode->next = list->freeHead;
    list->freeHead = headNode;
}

static void newArrayChunkDouble(arrayList_t *list) {
    u32 i;
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    arrayChunkNode_t *chunkNode = (arrayChunkNode_t*) pd->fcts.pdMalloc(pd, sizeof(arrayChunkNode_t) + ((sizeof(dlistNode_t) + list->elSize) * list->arrayChunkSize));
    ADebug(AllocDebugAllPD, "array-list/newArrayChunkDouble() "
           "pdMalloc(%ld), addr=%p\n",
           (sizeof(arrayChunkNode_t) + ((sizeof(dlistNode_t) + list->elSize) * list->arrayChunkSize)), chunkNode);
    chunkNode->next = list->poolHead;
    list->poolHead = chunkNode;

    u64 ptr = (u64)chunkNode + sizeof(arrayChunkNode_t);
    slistNode_t *headNode = (slistNode_t*)ptr;
    slistNode_t *curNode = headNode;
    for (i = 0; i < list->arrayChunkSize; i++) {
        curNode = (slistNode_t*)ptr;
        curNode->data = list->elSize ? (void*)(ptr + sizeof(dlistNode_t)) : NULL;
        ((dlistNode_t*)curNode)->prev = NULL;
        ptr += sizeof(dlistNode_t) + list->elSize;
        curNode->next = (slistNode_t*)ptr;
    }
    curNode->next = list->freeHead;
    list->freeHead = headNode;
}

static void newArrayChunk(arrayList_t *list) {
    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        return newArrayChunkSingle(list);
    case OCR_LIST_TYPE_DOUBLE:
        return newArrayChunkDouble(list);
    default:
        ASSERT(0);
        break;
    }
}

/*****************************************************************************/
/* INSERT (BEFORE)                                                           */
/*****************************************************************************/
static void insertArrayListNodeBeforeSingle(arrayList_t *list, slistNode_t *node, slistNode_t *newNode, u64 key) {
    ASSERT(newNode);
    newNode->next = node;
    if (node) ASSERT(node->key >= key);
    if (list->head == node) {
        list->head = newNode;
        if (node == NULL) {
            ASSERT(list->tail == NULL);
            list->tail = newNode;
        }
    } else {
        slistNode_t *last = list->head;
        while (last && last->next != node) last = last->next;
        ASSERT(last && (last->key <= key));
        last->next = newNode;
    }
    newNode->key = key;
}

static void insertArrayListNodeBeforeDouble(arrayList_t *list, slistNode_t *node, slistNode_t *newNode, u64 key) {
    ASSERT(newNode);
    dlistNode_t *dNode = (dlistNode_t*)node;
    dlistNode_t *dNewNode = (dlistNode_t*)newNode;
    if (node) {
        if (dNode->prev) ASSERT(dNode->prev->key <= key);
        if (node->next) ASSERT(node->next->key >= key);
        newNode->next = node;
        dNewNode->prev = dNode->prev;
        dNode->prev = newNode;
        if (dNewNode->prev) dNewNode->prev->next = newNode;
    } else {
        ASSERT(list->head == NULL);
        ASSERT(list->tail == NULL);
        newNode->next = NULL;
        dNewNode->prev = NULL;
        list->tail = newNode;
    }
    if (list->head == node)
        list->head = newNode;
    newNode->key = key;
}

/*****************************************************************************/
/* INSERT (AFTER)                                                            */
/*****************************************************************************/
static void insertArrayListNodeAfterSingle(arrayList_t *list, slistNode_t *node, slistNode_t *newNode, u64 key) {
    ASSERT(newNode);
    if (node) {
        ASSERT(node->key <= key);
        if (node->next) ASSERT(node->next->key >= key);
        newNode->next = node->next;
        node->next = newNode;
    } else {
        ASSERT(list->head == NULL);
        ASSERT(list->tail == NULL);
        newNode->next = NULL;
        list->head = newNode;
    }
    if (list->tail == node)
        list->tail = newNode;
    newNode->key = key;
}

static void insertArrayListNodeAfterDouble(arrayList_t *list, slistNode_t *node, slistNode_t *newNode, u64 key) {
    ASSERT(newNode);
    dlistNode_t *dNewNode = (dlistNode_t*)newNode;
    if (node) {
        ASSERT(node->key <= key);
        if (node->next) ASSERT(node->next->key >= key);
        newNode->next = node->next;
        dNewNode->prev = node;
        node->next = newNode;
        if (newNode->next) ((dlistNode_t*)(newNode->next))->prev = newNode;
    } else {
        ASSERT(list->head == NULL);
        ASSERT(list->tail == NULL);
        newNode->next = NULL;
        dNewNode->prev = NULL;
        list->head = newNode;
    }
    if (list->tail == node)
        list->tail = newNode;
    newNode->key = key;
}

/*****************************************************************************/
/* REMOVE                                                                    */
/*****************************************************************************/
static void removeArrayListNodeSingle(arrayList_t *list, slistNode_t *node) {
    ASSERT(node);
    if (list->head == node) {
        list->head = node->next;
        if (list->tail == node) {
            ASSERT(list->head == NULL);
            list->tail = NULL;
        }
    } else {
        slistNode_t *last = list->head;
        while (last && last->next != node) last = last->next;
        ASSERT(last);
        last->next = node->next;
        if (list->tail == node) list->tail = last;
    }
    node->key = 0;
    node->next = NULL;
}

static void removeArrayListNodeDouble(arrayList_t *list, slistNode_t *node) {
    ASSERT(node);
    dlistNode_t *dNode = (dlistNode_t*)node;
    if (dNode->prev) dNode->prev->next = node->next;
    if (node->next) ((dlistNode_t*)(node->next))->prev = dNode->prev;
    if (list->head == node) list->head = node->next;
    if (list->tail == node) list->tail = dNode->prev;
    node->key = 0;
    node->next = NULL;
    dNode->prev = NULL;
}

/*****************************************************************************/
/* MOVE (BEFORE)                                                             */
/*****************************************************************************/
static void moveArrayListNodeBeforeSingle(arrayList_t *list, slistNode_t *src, slistNode_t *dst) {
    ASSERT(src && dst);
    removeArrayListNodeSingle(list, src);
    insertArrayListNodeBeforeSingle(list, dst, src, 0);
}

static void moveArrayListNodeBeforeDouble(arrayList_t *list, slistNode_t *src, slistNode_t *dst) {
    ASSERT(src && dst);
    removeArrayListNodeDouble(list, src);
    insertArrayListNodeBeforeDouble(list, dst, src, 0);
}

void moveArrayListNodeBefore(arrayList_t *list, slistNode_t *src, slistNode_t *dst) {
    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        moveArrayListNodeBeforeSingle(list, src, dst);
        break;
    case OCR_LIST_TYPE_DOUBLE:
        moveArrayListNodeBeforeDouble(list, src, dst);
        break;
    default:
        ASSERT(0);
        break;
    }
}

/*****************************************************************************/
/* MOVE (AFTER)                                                              */
/*****************************************************************************/
static void moveArrayListNodeAfterSingle(arrayList_t *list, slistNode_t *src, slistNode_t *dst) {
    ASSERT(src && dst);
    removeArrayListNodeSingle(list, src);
    insertArrayListNodeAfterSingle(list, dst, src, 0);
}

static void moveArrayListNodeAfterDouble(arrayList_t *list, slistNode_t *src, slistNode_t *dst) {
    ASSERT(src && dst);
    removeArrayListNodeDouble(list, src);
    insertArrayListNodeAfterDouble(list, dst, src, 0);
}

void moveArrayListNodeAfter(arrayList_t *list, slistNode_t *src, slistNode_t *dst) {
    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        moveArrayListNodeAfterSingle(list, src, dst);
        break;
    case OCR_LIST_TYPE_DOUBLE:
        moveArrayListNodeAfterDouble(list, src, dst);
        break;
    default:
        ASSERT(0);
        break;
    }
}

/*****************************************************************************/
/* UPDATE KEY                                                                */
/*****************************************************************************/
static void updatePriorityArrayListNodeSingle(arrayList_t *list, slistNode_t *node, u64 newKey) {
    slistNode_t *ptr = NULL;
    if (newKey <= node->key) {
        ptr = list->head;
        while((ptr != NULL) && (ptr->key < newKey)) ptr = ptr->next;
        ASSERT(ptr != NULL);
    } else {
        ptr = node;
        while((ptr != NULL) && (ptr->key < newKey)) ptr = ptr->next;
    }
    while((ptr != NULL) && (ptr->key == newKey) && ((u64)(ptr->data) < (u64)(node->data))) ptr = ptr->next;
    if (ptr != node) {
        removeArrayListNodeSingle(list, node);
        if (ptr == NULL) {
            insertArrayListNodeAfterSingle(list, list->tail, node, newKey);
        } else {
            insertArrayListNodeBeforeSingle(list, ptr, node, newKey);
        }
    } else {
        node->key = newKey;
    }
}

static void updatePriorityArrayListNodeDouble(arrayList_t *list, slistNode_t *node, u64 newKey) {
    slistNode_t *ptr = node;
    if (newKey <= node->key) {
        while((ptr != list->head) && (ptr->key >= newKey)) ptr = ((dlistNode_t*)ptr)->prev;
        ASSERT(ptr != NULL);
    } else {
        while((ptr != NULL) && (ptr->key < newKey)) ptr = ptr->next;
    }
    while((ptr != NULL) && (ptr->key == newKey) && ((u64)(ptr->data) < (u64)(node->data))) ptr = ptr->next;
    if (ptr != node) {
        removeArrayListNodeDouble(list, node);
        if (ptr == NULL) {
            insertArrayListNodeAfterDouble(list, list->tail, node, newKey);
        } else {
            insertArrayListNodeBeforeDouble(list, ptr, node, newKey);
        }
    } else {
        node->key = newKey;
    }
}

void updatePriorityArrayListNode(arrayList_t *list, slistNode_t *node, u64 newKey) {
    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        updatePriorityArrayListNodeSingle(list, node, newKey);
        break;
    case OCR_LIST_TYPE_DOUBLE:
        updatePriorityArrayListNodeDouble(list, node, newKey);
        break;
    default:
        ASSERT(0);
        break;
    }
}

/*****************************************************************************/
/* NEW                                                                       */
/*****************************************************************************/
slistNode_t* newPriorityArrayListNodeBefore(arrayList_t *list, slistNode_t *node, u64 key) {
    ASSERT(list->freeHead);
    slistNode_t *newNode = list->freeHead;
    list->freeHead = list->freeHead->next;
    list->count++;

    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        insertArrayListNodeBeforeSingle(list, node, newNode, key);
        break;
    case OCR_LIST_TYPE_DOUBLE:
        insertArrayListNodeBeforeDouble(list, node, newNode, key);
        break;
    default:
        ASSERT(0);
        break;
    }

    if (list->freeHead == NULL)
        newArrayChunk(list);
    return newNode;
}

slistNode_t* newPriorityArrayListNodeAfter(arrayList_t *list, slistNode_t *node, u64 key) {
    ASSERT(list->freeHead);
    slistNode_t *newNode = list->freeHead;
    list->freeHead = list->freeHead->next;
    list->count++;

    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        insertArrayListNodeAfterSingle(list, node, newNode, key);
        break;
    case OCR_LIST_TYPE_DOUBLE:
        insertArrayListNodeAfterDouble(list, node, newNode, key);
        break;
    default:
        ASSERT(0);
        break;
    }

    if (list->freeHead == NULL)
        newArrayChunk(list);
    return newNode;
}

slistNode_t* newArrayListNodeBefore(arrayList_t *list, slistNode_t *node) {
    return newPriorityArrayListNodeBefore(list, node, 0);
}

slistNode_t* newArrayListNodeAfter(arrayList_t *list, slistNode_t *node) {
    return newPriorityArrayListNodeAfter(list, node, 0);
}

/*****************************************************************************/
/* FREE                                                                      */
/*****************************************************************************/
static void freeArrayListNodeSingle(arrayList_t *list, slistNode_t *node) {
    ASSERT(node);
    removeArrayListNodeSingle(list, node);
    if (list->elSize == 0) node->data = NULL;
    node->next = list->freeHead;
    list->freeHead = node;
    list->count--;
    return;
}

static void freeArrayListNodeDouble(arrayList_t *list, slistNode_t *node) {
    ASSERT(node);
    removeArrayListNodeDouble(list, node);
    if (list->elSize == 0) node->data = NULL;
    node->next = list->freeHead;
    ((dlistNode_t*)node)->prev = NULL;
    list->freeHead = node;
    list->count--;
    return;
}

void freeArrayListNode(arrayList_t *list, slistNode_t *node) {
    switch(list->type) {
    case OCR_LIST_TYPE_SINGLE:
        return freeArrayListNodeSingle(list, node);
    case OCR_LIST_TYPE_DOUBLE:
        return freeArrayListNodeDouble(list, node);
    default:
        ASSERT(0);
        break;
    }
}

/*****************************************************************************/
/* DESTRUCT LIST                                                             */
/*****************************************************************************/
void destructArrayList(arrayList_t *list) {
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    arrayChunkNode_t *chunkNode = list->poolHead;
    while(chunkNode) {
        arrayChunkNode_t *chunkNodePtr = chunkNode;
        chunkNode = chunkNode->next;
        pd->fcts.pdFree(pd, chunkNodePtr);
    }
    pd->fcts.pdFree(pd, list);
}

/*****************************************************************************/
/* CREATE LIST                                                               */
/*****************************************************************************/
arrayList_t* newArrayList(u32 elSize, u32 arrayChunkSize, ocrListType type) {
    ocrPolicyDomain_t * pd = NULL;
    getCurrentEnv(&pd, NULL, NULL, NULL);
    if (arrayChunkSize == 0) arrayChunkSize = ARRAY_CHUNK;
    arrayList_t* list = (arrayList_t*) pd->fcts.pdMalloc(pd, sizeof(arrayList_t));
    ADebug(AllocDebugAllPD, "array-list/newArrayList() "
           "pdMalloc(%ld), addr=%p\n", sizeof(arrayList_t), list);
    list->type = type;
    list->elSize = elSize;
    list->arrayChunkSize = arrayChunkSize;
    list->poolHead = NULL;
    list->freeHead = NULL;
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    newArrayChunk(list);
    return list;
}

/*****************************************************************************/
/* WRAPPER FUNCTIONS                                                         */
/*****************************************************************************/
void* pushFrontArrayList(arrayList_t *list, void *data) {
    slistNode_t *newnode = newArrayListNodeBefore(list, list->head);
    if (data) {
        if (list->elSize) {
            hal_memCopy(newnode->data, data, list->elSize, 0);
        } else {
            newnode->data = data;
        }
    }
    return newnode->data;
}

void* pushBackArrayList(arrayList_t *list, void *data) {
    slistNode_t *newnode = newArrayListNodeAfter(list, list->tail);
    if (data) {
        if (list->elSize) {
            hal_memCopy(newnode->data, data, list->elSize, 0);
        } else {
            newnode->data = data;
        }
    }
    return newnode->data;
}

void popFrontArrayList(arrayList_t *list) {
    freeArrayListNode(list, list->head);
}

void popBackArrayList(arrayList_t *list) {
    freeArrayListNode(list, list->tail);
}

void* peekFrontArrayList(arrayList_t *list) {
    return list->head->data;
}

void* peekBackArrayList(arrayList_t *list) {
    return list->tail->data;
}

