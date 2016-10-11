/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_SCHEDULER_OBJECT_LIST

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-task.h"
#include "scheduler-object/list/list-scheduler-object.h"
#include "scheduler-object/scheduler-object-all.h"

/******************************************************/
/* OCR-LIST SCHEDULER_OBJECT FUNCTIONS                */
/******************************************************/

void* ocrSchedulerObjectListHead(ocrSchedulerObject_t *self) {
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)self;
    ASSERT(listObj && listObj->list);
    if (listObj->list->head)
        return listObj->list->head->data;
    return NULL;
}

void* ocrSchedulerObjectListHeadNext(ocrSchedulerObject_t *self) {
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)self;
    ASSERT(listObj && listObj->list);
    slistNode_t *node = listObj->list->head;
    if (node) node = node->next;
    if (node) return node->data;
    return NULL;
}

void* ocrSchedulerObjectListHeadPlus(ocrSchedulerObject_t *self, u32 index) {
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)self;
    ASSERT(listObj && listObj->list);
    slistNode_t *node = listObj->list->head;
    while (node && index--) node = node->next;
    if (node) return node->data;
    return NULL;
}

static void listSchedulerObjectStart(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrListType listType, u32 elSize, u32 arrayChunkSize) {
    self->loc = PD->myLocation;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_PINNED;
    ocrSchedulerObjectList_t* listSchedObj = (ocrSchedulerObjectList_t*)self;
    listSchedObj->list = newArrayList(elSize, arrayChunkSize, listType);
}

static void listSchedulerObjectInitialize(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    self->guid.guid = NULL_GUID;
    self->guid.metaDataPtr = self;
    self->kind = OCR_SCHEDULER_OBJECT_LIST;
    self->fctId = fact->factoryId;
    self->loc = INVALID_LOCATION;
    self->mapping = OCR_SCHEDULER_OBJECT_MAPPING_UNDEFINED;
    ocrSchedulerObjectList_t* listSchedObj = (ocrSchedulerObjectList_t*)self;
    listSchedObj->list = NULL;
}

ocrSchedulerObject_t* newSchedulerObjectList(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif

    ocrSchedulerObject_t* schedObj = (ocrSchedulerObject_t*)runtimeChunkAlloc(sizeof(ocrSchedulerObjectList_t), PERSISTENT_CHUNK);
    listSchedulerObjectInitialize(factory, schedObj);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_CONFIG;
    return schedObj;
}

ocrSchedulerObject_t* listSchedulerObjectCreate(ocrSchedulerObjectFactory_t *factory, ocrParamList_t *perInstance) {
#ifdef OCR_ASSERT
    paramListSchedulerObject_t *paramSchedObj = (paramListSchedulerObject_t*)perInstance;
    ASSERT(!paramSchedObj->config);
    ASSERT(!paramSchedObj->guidRequired);
#endif
    ocrPolicyDomain_t *pd = factory->pd;
    ocrSchedulerObject_t *schedObj = (ocrSchedulerObject_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectList_t));
    listSchedulerObjectInitialize(factory, schedObj);
    paramListSchedulerObjectList_t *paramsList = (paramListSchedulerObjectList_t*)perInstance;
    listSchedulerObjectStart(schedObj, pd, paramsList->type, paramsList->elSize, paramsList->arrayChunkSize);
    schedObj->kind |= OCR_SCHEDULER_OBJECT_ALLOC_PD;
    return schedObj;
}

u8 listSchedulerObjectDestroy(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self) {
    if (IS_SCHEDULER_OBJECT_CONFIG_ALLOCATED(self->kind)) {
        runtimeChunkFree((u64)self, PERSISTENT_CHUNK);
    } else {
        ASSERT(IS_SCHEDULER_OBJECT_PD_ALLOCATED(self->kind));
        ocrPolicyDomain_t *pd = fact->pd;
        ocrSchedulerObjectList_t* listSchedObj = (ocrSchedulerObjectList_t*)self;
        destructArrayList(listSchedObj->list);
        pd->fcts.pdFree(pd, self);
    }
    return 0;
}

u8 listSchedulerObjectInsert(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObject_t *element, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    slistNode_t *node = NULL;
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)self;
    switch(properties & SCHEDULER_OBJECT_INSERT_POSITION) {
    case SCHEDULER_OBJECT_INSERT_POSITION_HEAD:
        {
            switch(properties & SCHEDULER_OBJECT_INSERT_KIND) {
            case SCHEDULER_OBJECT_INSERT_BEFORE:
                {
                    node = newArrayListNodeBefore(listObj->list, listObj->list->head);
                }
                break;
            case SCHEDULER_OBJECT_INSERT_AFTER:
                {
                    node = newArrayListNodeAfter(listObj->list, listObj->list->head);
                }
                break;
            case SCHEDULER_OBJECT_INSERT_INPLACE:
                {
                    node = listObj->list->head;
                }
                break;
            default:
                ASSERT(0);
                break;
            }
        }
        break;
    case SCHEDULER_OBJECT_INSERT_POSITION_TAIL:
        {
ASSERT(0);
            switch(properties & SCHEDULER_OBJECT_INSERT_KIND) {
            case SCHEDULER_OBJECT_INSERT_BEFORE:
                {
                    node = newArrayListNodeBefore(listObj->list, listObj->list->tail);
                }
                break;
            case SCHEDULER_OBJECT_INSERT_AFTER:
                {
                    node = newArrayListNodeAfter(listObj->list, listObj->list->tail);
                }
                break;
            case SCHEDULER_OBJECT_INSERT_INPLACE:
                {
                    node = listObj->list->tail;
                }
                break;
            default:
                ASSERT(0);
                break;
            }
        }
        break;
    case SCHEDULER_OBJECT_INSERT_POSITION_ITERATOR:
        {
            ocrSchedulerObjectListIterator_t *lit = (ocrSchedulerObjectListIterator_t*)iterator;
            switch(properties & SCHEDULER_OBJECT_INSERT_KIND) {
            case SCHEDULER_OBJECT_INSERT_BEFORE:
                {
                    node = newArrayListNodeBefore(listObj->list, lit->current);
                }
                break;
            case SCHEDULER_OBJECT_INSERT_AFTER:
                {
                    node = newArrayListNodeAfter(listObj->list, lit->current);
                }
                break;
            case SCHEDULER_OBJECT_INSERT_INPLACE:
                {
                    node = lit->current;
                }
                break;
            default:
                ASSERT(0);
                break;
            }
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    if (node) {
        if (listObj->list->elSize) {
            hal_memCopy(node->data, element, listObj->list->elSize, 0);
        } else {
            node->data = element;
        }
    }
    return 0;
}

u8 listSchedulerObjectRemove(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, u32 count, ocrSchedulerObject_t *dst, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    slistNode_t *node = NULL;
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)self;
    switch(properties) {
    case SCHEDULER_OBJECT_REMOVE_HEAD:
        {
            node = listObj->list->head;
        }
        break;
    case SCHEDULER_OBJECT_REMOVE_TAIL:
        {
            node = listObj->list->tail;
        }
        break;
    case SCHEDULER_OBJECT_REMOVE_ITERATOR:
        {
            ocrSchedulerObjectListIterator_t *lit = (ocrSchedulerObjectListIterator_t*)iterator;
            node = lit->current;
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    if (node) {
        if (listObj->list->elSize) {
            if (IS_SCHEDULER_OBJECT_TYPE_SINGLETON(dst->kind)) {
                ASSERT(listObj->list->elSize == sizeof(ocrGuid_t));
                // See BUG #928 on GUID issues
#if GUID_BIT_COUNT == 64
                dst->guid.guid.guid = *((u64*)(node->data));
#elif GUID_BIT_COUNT == 128
                dst->guid.guid.lower = *((u64*)(node->data));
                dst->guid.guid.upper = 0x0;
#endif
            } else {
                ASSERT(dst->guid.metaDataPtr);
                hal_memCopy(dst->guid.metaDataPtr, node->data, listObj->list->elSize, 0);
            }
        } else {
            if (dst) dst->guid.metaDataPtr = node->data;
        }
        freeArrayListNode(listObj->list, node);
    }
    return 0;
}

u64 listSchedulerObjectCount(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrSchedulerObjectList_t *schedObj = (ocrSchedulerObjectList_t*)self;
    return schedObj->list->count;
}

ocrSchedulerObjectIterator_t* listSchedulerObjectCreateIterator(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 properties) {
    ocrPolicyDomain_t *pd = fact->pd;
    ocrSchedulerObjectIterator_t* iterator = (ocrSchedulerObjectIterator_t*)pd->fcts.pdMalloc(pd, sizeof(ocrSchedulerObjectListIterator_t));
    iterator->schedObj = self;
    iterator->data = NULL;
    iterator->fctId = fact->factoryId;
    ocrSchedulerObjectListIterator_t *lit = (ocrSchedulerObjectListIterator_t*)iterator;
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)self;
    lit->internal = self;
    lit->current = listObj ? listObj->list->head : NULL;
    return iterator;
}

u8 listSchedulerObjectDestroyIterator(ocrSchedulerObjectFactory_t * fact, ocrSchedulerObjectIterator_t *iterator) {
    ocrPolicyDomain_t *pd = fact->pd;
    pd->fcts.pdFree(pd, iterator);
    return 0;
}

u8 listSchedulerObjectIterate(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectIterator_t *iterator, u32 properties) {
    ocrSchedulerObjectList_t *listObj = (ocrSchedulerObjectList_t*)iterator->schedObj;
    ASSERT(listObj);
    ocrSchedulerObjectListIterator_t *lit = (ocrSchedulerObjectListIterator_t*)iterator;
    if (lit->internal != iterator->schedObj) {
        lit->internal = iterator->schedObj;
        lit->current = listObj->list->head;
    }
    iterator->data = NULL;
    switch(properties) {
    case SCHEDULER_OBJECT_ITERATE_CURRENT:
        {
            if (lit->current) iterator->data = lit->current->data;
        }
        break;
    case SCHEDULER_OBJECT_ITERATE_HEAD:
        {
            lit->current = listObj->list->head;
            if (lit->current) iterator->data = lit->current->data;
        }
        break;
    case SCHEDULER_OBJECT_ITERATE_TAIL:
        {
            lit->current = listObj->list->tail;
            if (lit->current) iterator->data = lit->current->data;
        }
        break;
    case SCHEDULER_OBJECT_ITERATE_NEXT:
        {
            if (lit->current) lit->current = lit->current->next;
            if (lit->current) iterator->data = lit->current->data;
        }
        break;
    case SCHEDULER_OBJECT_ITERATE_PREV:
        {
            if (lit->current) {
                if (listObj->list->type == OCR_LIST_TYPE_DOUBLE) {
                    lit->current = ((dlistNode_t*)(lit->current))->prev;
                } else {
                    slistNode_t *prev = listObj->list->head;
                    while (prev && prev->next != lit->current) prev = prev->next;
                    lit->current = prev;
                }
            }
            if (lit->current) iterator->data = lit->current->data;
        }
        break;
    case SCHEDULER_OBJECT_ITERATE_SEARCH_KEY:
        {
            ASSERT(0); //TODO
        }
        break;
    default:
        ASSERT(0);
        break;
    }
    return 0;
}

ocrSchedulerObject_t* listGetSchedulerObjectForLocation(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrSchedulerObjectKind kind, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping, u32 properties) {
    ASSERT(0);
    return NULL;
}

u8 listSetLocationForSchedulerObject(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, ocrLocation_t loc, ocrSchedulerObjectMappingKind mapping) {
    self->loc = loc;
    self->mapping = mapping;
    return 0;
}

ocrSchedulerObjectActionSet_t* listSchedulerObjectNewActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObject_t *self, u32 count) {
    ASSERT(0);
    return NULL;
}

u8 listSchedulerObjectDestroyActionSet(ocrSchedulerObjectFactory_t *fact, ocrSchedulerObjectActionSet_t *actionSet) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 listSchedulerObjectSwitchRunlevel(ocrSchedulerObject_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                                    phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 listSchedulerObjectOcrPolicyMsgGetMsgSize(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u64 *marshalledSize, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 listSchedulerObjectOcrPolicyMsgMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *buffer, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

u8 listSchedulerObjectOcrPolicyMsgUnMarshallMsg(ocrSchedulerObjectFactory_t *fact, ocrPolicyMsg_t *msg, u8 *localMainPtr, u8 *localAddlPtr, u32 properties) {
    ASSERT(0);
    return OCR_ENOTSUP;
}

/******************************************************/
/* OCR-LIST SCHEDULER_OBJECT FACTORY FUNCTIONS        */
/******************************************************/

void destructSchedulerObjectFactoryList(ocrSchedulerObjectFactory_t * factory) {
    runtimeChunkFree((u64)factory, PERSISTENT_CHUNK);
}

ocrSchedulerObjectFactory_t * newOcrSchedulerObjectFactoryList(ocrParamList_t *perType, u32 factoryId) {
    ocrSchedulerObjectFactory_t* schedObjFact = (ocrSchedulerObjectFactory_t*) runtimeChunkAlloc(
                                      sizeof(ocrSchedulerObjectFactoryList_t), PERSISTENT_CHUNK);

    schedObjFact->factoryId = schedulerObjectList_id;
    schedObjFact->kind = OCR_SCHEDULER_OBJECT_LIST;
    schedObjFact->pd = NULL;

    schedObjFact->destruct = &destructSchedulerObjectFactoryList;
    schedObjFact->instantiate = &newSchedulerObjectList;

    schedObjFact->fcts.create = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrParamList_t*), listSchedulerObjectCreate);
    schedObjFact->fcts.destroy = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*), listSchedulerObjectDestroy);
    schedObjFact->fcts.insert = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), listSchedulerObjectInsert);
    schedObjFact->fcts.remove = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, u32, ocrSchedulerObject_t*, ocrSchedulerObjectIterator_t*, u32), listSchedulerObjectRemove);
    schedObjFact->fcts.count = FUNC_ADDR(u64 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), listSchedulerObjectCount);
    schedObjFact->fcts.createIterator = FUNC_ADDR(ocrSchedulerObjectIterator_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), listSchedulerObjectCreateIterator);
    schedObjFact->fcts.destroyIterator = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*), listSchedulerObjectDestroyIterator);
    schedObjFact->fcts.iterate = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectIterator_t*, u32), listSchedulerObjectIterate);
    schedObjFact->fcts.setLocationForSchedulerObject = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrLocation_t, ocrSchedulerObjectMappingKind), listSetLocationForSchedulerObject);
    schedObjFact->fcts.getSchedulerObjectForLocation = FUNC_ADDR(ocrSchedulerObject_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, ocrSchedulerObjectKind, ocrLocation_t, ocrSchedulerObjectMappingKind, u32), listGetSchedulerObjectForLocation);
    schedObjFact->fcts.createActionSet = FUNC_ADDR(ocrSchedulerObjectActionSet_t* (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObject_t*, u32), listSchedulerObjectNewActionSet);
    schedObjFact->fcts.destroyActionSet = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrSchedulerObjectActionSet_t*), listSchedulerObjectDestroyActionSet);
    schedObjFact->fcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrSchedulerObject_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                        phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), listSchedulerObjectSwitchRunlevel);
    schedObjFact->fcts.ocrPolicyMsgGetMsgSize = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u64*, u32), listSchedulerObjectOcrPolicyMsgGetMsgSize);
    schedObjFact->fcts.ocrPolicyMsgMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u32), listSchedulerObjectOcrPolicyMsgMarshallMsg);
    schedObjFact->fcts.ocrPolicyMsgUnMarshallMsg = FUNC_ADDR(u8 (*)(ocrSchedulerObjectFactory_t*, ocrPolicyMsg_t*, u8*, u8*, u32), listSchedulerObjectOcrPolicyMsgUnMarshallMsg);
    return schedObjFact;
}

#endif /* ENABLE_SCHEDULER_OBJECT_LIST */
