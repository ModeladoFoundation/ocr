/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

/**
 * @brief Implements a variable-sized array-based deque
 * that is not thread safe
 *
 * This deque is meant to grow dynamically in size but also
 * to have constant time insertion and deletion at head and
 * tail; in other words, there is never any copy needed when
 * inserting elements.
 *
 * The deque also tries to optimize the number of frees/creates
 * it does by basically only deleting chunks if there is at least
 * one spare chunk on either end of the deque. The idea here is
 * to avoid cases where, if the chunk size is 4, a deque
 * that oscilates between 4 and 5 elements for example, would
 * have to destroy the new chunk and recreate it. This
 * gives some "buffer" and only deletes if it
 */
#ifndef __ARRAYDEQUE_H__
#define __ARRAYDEQUE_H__

#include "ocr-config.h"
#include "ocr-types.h"
#include "ocr-policy-domain.h"

typedef struct _arrayDequeChunk_t {
    void** data; /**< Data is a ** as it is an array of void* */
    struct _arrayDequeChunk_t *next, *prev;
} arrayDequeChunk_t;

typedef struct _arrayDeque_t {
    u32 chunkSize; /**< Size of each chunk. Should be a power of two */
    u32 chunkSizeOffset; /**< Number of bits in chunk size - 1; for eg: 8 -> 3 */
    u32 chunkCount; /**< Number of chunks */
    arrayDequeChunk_t *head, *tail; /**< Head and tail of the array deque */
    u32 headIdx, tailIdx; /**< Head and tail index of the deque. Note that these point
                           * to valid elements except when both 0 and isEmpty is true.
                           * In other words, headIdx = tailIdx = 0 and isEmpty is false
                           * means that there is one valid element at index 0. If we need to
                           * push at the tail, we would push in position 1. */
    bool isEmpty; /**< True if the deque is empty */
} arrayDeque_t;

/**
 * @brief Initialize the array deque
 *
 * The deque structure needs to already be allocated but
 * will contain 0 elements.
 * @param[in] deque     Array deque to initialize
 * @param[in] chunkSize Size that each chunk should be (in number
 *                      of elements). Must be power of 2
 * @return 0 on success or an error code:
 *   - OCR_EINVAL: Invalid value for chunkSize
 */
u8 arrayDequeInit(arrayDeque_t* deque, u32 chunkSize);

/**
 * @brief Destroys the array deque
 *
 * This frees all memory assocated with the deque
 *
 * @param[in] deque deque to destroy
 * @return 0 on success
 */
u8 arrayDequeDestruct(arrayDeque_t* deque);

/**
 * @brief Returns the number of elements
 * in the deque
 *
 * @param[in] deque Array deque of which the size is
 *                  requested
 *
 * @return the number of elements currently in the deque
 */
u32 arrayDequeSize(arrayDeque_t* deque);

/**
 * @brief Returns the number of elements
 * that can be inserted into the deque without
 * needing a resize
 *
 * @note that this does not specify whether the elements
 * can fit at the beginning or end.
 *
 * @param[in] deque Array deque of which the capacity is
 *                  requested
 *
 * @return the total capacity of the deque without it needing to
 * resize. This includes elements that are already used up and
 * those that are free. To get the number of free elements,
 * you can substract the result from arrayDequeSize() from this
 */
u32 arrayDequeCapacity(arrayDeque_t* deque);

/**
 * @brief Push an element at the tail of the deque
 *
 * @param[in] deque Array deque to push to
 * @param[in] entry Entry to push to the deque
 *
 * @return 0 on success or an error code:
 *   - OCR_ENOMEM if there is not enough memory to add the entry
 */
u8 arrayDequePushAtTail(arrayDeque_t* deque, void* entry);

/**
 * @brief Pop an element from the tail of the deque
 *
 * @param[in]  deque Array deque to pop from
 * @param[out] entry Entry popped
 *
 * @return 0 on success or an error code:
 *   - OCR_EAGAIN: No element to pop -- try later
 */
u8 arrayDequePopFromTail(arrayDeque_t* deque, void** entry);

/**
 * @brief Peeks at an element from the tail of the deque
 *
 * This returns the element at the tail of the deque
 * but does not modify the deque
 *
 * @param[in]  deque Array deque to peek from
 * @param[out] entry Entry peeked
 *
 * @return 0 on success or an error code:
 *   - OCR_EAGAIN: No element to peek -- try later
 */
u8 arrayDequePeekFromTail(arrayDeque_t* deque, void** entry);

/**
 * @brief Push an element at the head of the deque
 *
 * @param[in] deque Array deque to push to
 * @param[in] entry Entry to push to the deque
 *
 * @return 0 on success or an error code:
 *   - OCR_ENOMEM if there is not enough memory to add the entry
 */
u8 arrayDequePushAtHead(arrayDeque_t* deque, void* entry);

/**
 * @brief Pop an element from the head of the deque
 *
 * @param[in]  deque Array deque to pop from
 * @param[out] entry Entry popped
 *
 * @return 0 on success or an error code:
 *   - OCR_EAGAIN: No element to pop -- try later
 */
u8 arrayDequePopFromHead(arrayDeque_t* deque, void** entry);

/**
 * @brief Peeks at an element from the head of the deque
 *
 * This returns the element at the head of the deque
 * but does not modify the deque
 *
 * @param[in]  deque Array deque to peek from
 * @param[out] entry Entry peeked
 *
 * @return 0 on success or an error code:
 *   - OCR_EAGAIN: No element to peek -- try later
 */
u8 arrayDequePeekFromHead(arrayDeque_t* deque, void** entry);

/**
 * @brief Returns the element at index idx
 *
 * A valid 'entry' pointer to a pointer should be passed
 * and upon exit, the element returned can be used to modify
 * the element in the array
 *
 * @param[in] deque   Array deque to get an element from
 * @param[in] idx     Index of the element to get (0 based)
 * @param[out] entry  Element at idx (pointer to a void* pointer)
 * @return 0 on success or an error code:
 *     - OCR_EINVAL if index is invalid
 */
u8 arrayDequeAtIndex(arrayDeque_t* deque, u64 idx, void*** entry);

/**
 * @brief Hook into the deque API
 *
 * This allows a arrayDeque to be used using the function
 * pointers defined in the deque.h file
 *
 * @param[in] pd        Policy domain
 * @param[in] initValue Initial value for elements in the
 *                      deque. This is currently ignored as
 *                      no elements are created when the arrayDeque
 *                      is created
 * @return a deque
 */
struct _ocrDeque_t;
struct _ocrDeque_t* newArrayQueue(struct _ocrPolicyDomain_t *pd, void * initValue);
#endif /* __ARRAYDEQUE_H__ */

