#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_GASNET

#include "ocr-policy-domain.h"
#include "splay-tree.h"

#include <gasnet.h>

/*
 * message data structure
 */
typedef struct MessageType_s {
  SPLAY_ENTRY(MessageType_s) link;

  volatile int count;   // number of stored messages
  int messageID;        // message ID generated by PD
  char buffer[];        // temporary storage of messages
} MessageType_t;

/*
 * comparison function between messages
 * this function is used by splay tree
 */
static int message_compare(MessageType_t *m1, MessageType_t *m2) {
  return (m1->messageID - m2->messageID);
}

// root for the splay tree
SPLAY_HEAD(MessageHead_s, MessageType_s) rootMessage = SPLAY_INITIALIZER(rootMessage);

// prototype of the tree
SPLAY_PROTOTYPE(MessageHead_s, MessageType_s, link, message_compare);

// definition of the tree
SPLAY_GENERATE(MessageHead_s, MessageType_s, link, message_compare);

/*
 * Check if there's a message already in the database
 *
 * @param msgID : a message ID in which the function will check if the same msg ID
 *           is already in the database
 *
 * @return pointer to the existing message if exist; NULL otherwise
 */
static struct MessageType_s* message_check(int msgID) {
    struct MessageType_s item = { .count=0, .messageID=msgID };
    return ((struct MessageType_s*) SPLAY_FIND(MessageHead_s, &rootMessage, &item) );
}


/*
 * @brief pushing a message part into the database
 * The caller has to check the return of this function.
 * If the returns is NULL, nothing to be done,
 * but if the returns is not NULL (a memory address), the caller needs
 * to use the address to push into the incoming queue.
 *
 * @param pd : policy domain of the owner of this message. We use policy domain just
 *             for the memory allocation only.
 * @param messageID: a unique ID of the big message
 * @param message: the medium message
 * @param size: the size of this medium message
 * @param position: the position of the message in the big message
 * @param tot_msg_size: the total size of the big message
 *
 * @return the big message if all messages have been completed, NULL otherwise
 */
void * gasnet_message_push(ocrPolicyDomain_t* pd, int messageID, void *message, int size,
                           int position, int tot_parts, int tot_msg_size) {
    struct MessageType_s *current = message_check(messageID);
    int found = current != NULL;
    if (!found) {
        // message doesn't exist, create a new one
        uint32_t alloc_size = sizeof(struct MessageType_s) + tot_msg_size;
        struct MessageType_s *entry = (struct MessageType_s*) pd->fcts.pdMalloc(pd, alloc_size);
        entry->count = tot_parts-1;
        entry->messageID = messageID;
        memcpy(&entry->buffer[position], message, size);
        SPLAY_INSERT(MessageHead_s, &rootMessage, entry);
    } else {
        // already exist, add into the buffer for a given position
        current->count--;
        memcpy(&current->buffer[position], message, size);
        if (current->count == 0) {
            // message has been completed, returns all the assemble parts
            return current->buffer;
        }
    }
return NULL;
}


/*
 * @brief remove a message from the database
 *
 * @param messageID : the ID of the message to be removed
 */
void gasnet_message_pop(ocrPolicyDomain_t* pd, int messageID) {
    struct MessageType_s *current = message_check(messageID);
    if (current != NULL) {
        SPLAY_REMOVE(MessageHead_s, &rootMessage, current);
        pd->fcts.pdFree(pd, current);
    }
}

#endif /* ENABLE_COMM_PLATFORM_GASNET */