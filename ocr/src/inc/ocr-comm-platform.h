/**
 * @brief OCR interface to communication platforms
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#ifndef __OCR_COMM_PLATFORM_H__
#define __OCR_COMM_PLATFORM_H__

#include "ocr-runtime-types.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"

struct _ocrCommApi_t;
struct _ocrPolicyDomain_t;
struct _ocrPolicyMsg_t;

/****************************************************/
/* PARAMETER LISTS                                  */
/****************************************************/

/**
 * @brief Parameter list to create a comm-platform factory
 */
typedef struct _paramListCommPlatformFact_t {
    ocrParamList_t base;    /**< Base class */
} paramListCommPlatformFact_t;

/**
 * @brief Parameter list to create a comm-platform instance
 */
typedef struct _paramListCommPlatformInst_t {
    ocrParamList_t base;    /**< Base class */
    ocrLocation_t location;
} paramListCommPlatformInst_t;


/****************************************************/
/* OCR COMMUNICATION PLATFORM                       */
/****************************************************/

struct _ocrCommPlatform_t;

/**
 * @brief Comm-platform function pointers
 *
 * The function pointers are separate from the comm-platform instance to allow for
 * the sharing of function pointers for comm-platform from the same factory
 */
typedef struct _ocrCommPlatformFcts_t {
    /*! \brief Destroys a comm-platform
     */
    void (*destruct)(struct _ocrCommPlatform_t *self);

    /**
     * @brief Switch runlevel
     *
     * @param[in] self         Pointer to this object
     * @param[in] PD           Policy domain this object belongs to
     * @param[in] runlevel     Runlevel to switch to
     * @param[in] phase        Phase for this runlevel
     * @param[in] properties   Properties (see ocr-runtime-types.h)
     * @param[in] callback     Callback to call when the runlevel switch
     *                         is complete. NULL if no callback is required
     * @param[in] val          Value to pass to the callback
     *
     * @return 0 if the switch command was successful and a non-zero error
     * code otherwise. Note that the return value does not indicate that the
     * runlevel switch occured (the callback will be called when it does) but only
     * that the call to switch runlevel was well formed and will be processed
     * at some point
     */
    u8 (*switchRunlevel)(struct _ocrCommPlatform_t* self, struct _ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                         phase_t phase, u32 properties, void (*callback)(struct _ocrPolicyDomain_t*, u64), u64 val);

    /**
     * @brief Tells the communication platfrom that the biggest
     * messages expected are of size 'size'
     *
     * Certain implementations may need/want to pre-allocated
     * structures to receive incoming messages. This indicates how
     * big the biggest message that is expected is for a particular
     * mask (mask of 0 means all messages).
     *
     * This call can be called multiple times if new information about
     * message sizes comes in (for example a query will require a larger
     * response than usual).
     *
     * @param[in] self          Pointer to this
     * @param[in] size          Maximum expected size for the mask given
     * @param[in] mask          Mask (0 means no mask so all messages)
     * @return 0 on success and a non-zero error code
     */
    u8 (*setMaxExpectedMessageSize)(struct _ocrCommPlatform_t *self, u64 size, u32 mask);

    /**
     * @brief Send a message to another target
     *
     * This call, which executes on the compute platform making the call,
     * will send an asynchronous message to target. The call will, if the user
     * wants it, return an ID to check on the status of the message
     *
     * The actual implementation of this function may be a fully synchronous
     * call but users of this call should assume asynchrony.
     *
     * The 'properties' field allows the caller to specify the way the call
     * is being made with regards to the lifetime of 'message':
     *    - if (properties & TWOWAY_MSG_PROP) is non zero:
     *        - An "answer" to this message is expected. This flag is used in
     *          conjunction with PERSIST_MSG_PROP
     *    - if (properties & PERSIST_MSG_PROP) is non zero:
     *        - If TWOWAY_MSG_PROP: the comm-platform can use the message space
     *          to send the message and does not need to free it
     *        - If not TWOWAY_MSG_PROP: the comm-platform must free message using
     *          pdFree when it no longer needs it.
     * To recap the values of properties:
     *     - 0: The comm-platform needs to make a copy of the message or "fully"
     *          send it before returning
     *     - TWOWAY_MSG_PROP: Invalid value (ignored)
     *     - TWOWAY_MSG_PROP | PERSIST_MSG_PROP: The comm-platform can use
     *       msg to send the message (no copy needed). It should NOT free msg
     *     - PERSIST_MSG_PROP: The comm-platform needs to free msg at some point
     *       using pdFree when it has finished using the buffer.
     * The properties can also encode a priority which, if supported, will cause
     * the receiving end's poll/wait to prioritize messages with higher priority
     * if multiple messages are available.
     *
     *
     * The mask parameter allows the underlying implementation to differentiate
     * messages if it wants to (see setMaxExpectedMessageSize())
     *
     * @param[in] self        Pointer to this
     * @param[in] target      Target to communicate with
     * @param[in] message     Message to send
     * @param[out] id         If non-NULL, an ID is returned which can be used
     *                        in getMessageStatus. Note that getMessageStatus
     *                        may always return MSG_NORMAL.
     * @param[in] properties  Properties for this send. See above.
     * @param[in] mask        Mask of the outgoing message (may be ignored)
     * @return 0 on success and a non-zero error code
     */
    u8 (*sendMessage)(struct _ocrCommPlatform_t* self, ocrLocation_t target,
                      struct _ocrPolicyMsg_t *message, u64 *id,
                      u32 properties, u32 mask);

    /**
     * @brief Non-blocking check for incoming messages
     *
     * This function checks for ANY incoming message
     * @param[in] self        Pointer to this comm-platform
     * @param[in/out] msg     Returns a pointer to an ocrPolicyMsg_t. On input,
     *                        if *msg is non-NULL, this is a "hint" to the
     *                        comm-platform that it can use the provided buffer
     *                        for the response message. There is, however
     *                        no obligation. If *msg is non NULL on input,
     *                        (*msg)->bufferSize contains the size of the buffer
     *                        for this message (max size to use)
     *                        The comm-platform will *NEVER* free the input
     *                        buffer passed in. It is up to the caller to
     *                        determine if the comm-platform used the buffer
     *                        (if *msg on output is the same as *msg on input).
     *                        If on output *msg is not the same as the one
     *                        passed in, the output *msg will have been created
     *                        by the comm-platform and the caller needs to call
     *                        destructMessage once the buffer is done being used.
     *                        Otherwise (ie: *msg is the same on input and
     *                        output), destructMessage should not be called
     *                        If *msg is NULL on return, check return code
     *                        to see if there was an error or if no messages
     *                        were available
     * @param[in] properties  Unused for now
     * @param[out] mask       Mask of the returned message (may always be 0)
     * @return
     *     - #POLL_NO_MESSAGE: 0 messages found
     *     - 0: One message found and returned
     *     - #POLL_MORE_MESSAGE: 1 message found and returned but more available
     *     - <val> & #POLL_ERR_MASK: error code or 0 if all went well
     */
    u8 (*pollMessage)(struct _ocrCommPlatform_t *self, struct _ocrPolicyMsg_t **msg,
                      u32 properties, u32 *mask);

    /**
     * @brief Blocking check for incoming messages
     *
     * This function checks for ANY incoming message
     * See pollMessage() for a detailed explanation of the parameters
     * @param[in] self        Pointer to this comm-platform
     * @param[in/out] msg     Pointer to the message returned
     * @param[in] properties  Unused for now
     * @param[out] mask       Mask of the returned message (may always be 0)
     * @return 0 on success and a non-zero error code
     */
    u8 (*waitMessage)(struct _ocrCommPlatform_t *self, struct _ocrPolicyMsg_t **msg,
                      u32 properties, u32 *mask);

    /**
     * @brief Sends a message to a destination outside this PD
     *
     * This call sends a message to 'target' and returns up to two events depending
     * on what the programmer wants to wait on
     * @param[in] self           This communication platform/target
     * @param[in] target         Target to send this message to
     * @param[in/out] inOutEvent Event containing the message to send to the
     *                           target (must be a PDEVT_TYPE_MSG).
     *                           Upon successful completion of the call, will
     *                           contain the event (and eventually policy message)
     *                           to wait on to get the response to the message
     *                           unless COMM_ONE_WAY is set in properties (in which case, on
     *                           return, *inOutEvent will be NULL). Note that this
     *                           is a pointer-to-a-pointer argument to give the freedom
     *                           to the communication framework to return a different
     *                           event if needed. In that case, the initial event
     *                           will be properly freed.
     * @param[out] statusEvent   If non-NULL when the call is made, will return an
     *                           event that can be used to query the state of the
     *                           communication. This event must be a PDEVT_TYPE_COMMSTATUS
     *                           and you should specify what you want to wait for in the
     *                           properties field of that event:
     *                             - COMM_STATUS_SENT if the statusEvent should represent whether
     *                               the message has been sent
     *                             - COMM_STATUS_RECEIVED if the statusEvent should represent that
     *                               the message was received
     *                             - COMM_STATUS_PROCESSED if the statusEvent should represent
     *                               that the message was received and fully processed by
     *                               the destination. This is similar to waiting for a
     *                               response except that the response is a single "done" value
     * @param[in] idx            Unused for now (support for continuations later)
     * @return 0 or an error code. Codes TBD. The code would only indicate local errors
     *
     * @todo: Do we need a special error code to indicate inOutEvent reuse
     * @todo: Do we need to distinguish received by final destination and received by first
     * intermediate destination
     */
    u8 (*sendMessageMT)(struct _ocrCommPlatform_t *self, struct _pdEvent_t** inOutEvent,
                        struct _pdEvent_t* statusEvent, u32 idx);

    /**
     * @brief Non-blocking check for incoming messages
     *
     * This function checks for incomming message that are COMM_ONE_WAY or initial requests
     * All responses to messages sent via sendMessageMT can be waited on using the returned
     * inOutEvent. This call polls the communication interface for any new incomming messages;
     * either new requests (that will require a response from us) or COMM_ONE_WAY messages.
     *
     * @param[in] self        Pointer to this comm-platform
     * @param[out] outEvent   Returns a pointer to an event (subclass of PDEVT_TYPE_BASE_MSG)
     *                        that contains or will contain the next message to
     *                        be received by this communication platform. Note that
     *                        the event return may not yet contain the full message (and
     *                        therefore not be fully ready). This is useful, for example,
     *                        to allow the message to be buffered asynchronasly.
     *                        If no message is available, this will be set to NULL.
     * @param[in] idx         Unused for now
     * @return
     *     - #POLL_NO_MESSAGE: 0 messages found
     *     - 0: One message found and returned
     *     - #POLL_MORE_MESSAGE: 1 message found and returned but more available
     *     - <val> & #POLL_ERR_MASK: error code or 0 if all went well
     */
    u8 (*pollMessageMT)(struct _ocrCommPlatform_t *self, struct _pdEvent_t **outEvent,
                        u32 idx);

    /**
     * @brief Blocking check for incoming messages
     *
     * This function checks for incomming message that are COMM_ONE_WAY or initial requests
     * All responses to messages sent via sendMessageMT can be waited on using the returned
     * inOutEvent. This call polls the communication interface for any new incomming messages;
     * either new requests (that will require a response from us) or COMM_ONE_WAY messages.
     *
     * In other words, this will block until a *new* unknown request comes up.
     *
     * @param[in] self        Pointer to this comm-platform
     * @param[out] outEvent   Returns a pointer to an event (subclass of PDEVT_TYPE_BASE_MSG)
     *                        that contains or will contain the next message to
     *                        be received by this communication platform. Note that
     *                        the event return may not yet contain the full message (and
     *                        therefore not be fully ready). This is useful, for example,
     *                        to allow the message to be buffered asynchronasly.
     *                        If no message is available, this will be set to NULL.
     * @param[in] idx         Unused for now
     * @return 0 on success and a non-zero code on error
     */
    u8 (*waitMessageMT)(struct _ocrCommPlatform_t *self, struct _pdEvent_t **outEvent,
                        u32 idx);

    /**
     * @brief Releases/frees a message returned by pollMessage/waitMessage
     *
     * Implementations may vary in how this is implemented. This call must
     * be called at some point after pollMessage/waitMessage on the returned message
     * buffer
     *
     * @param[in] self          Pointer to this comm-platform
     * @param[out] msg          Pointer to the message to destroy. The caller guarantees
     *                          that it is done using that message buffer
     * @return 0 on success and a non-zero error code
     */
    u8 (*destructMessage)(struct _ocrCommPlatform_t *self, struct _ocrPolicyMsg_t *msg);

    /**
     * @brief Returns the status of a message sent via sentMessage
     *
     * Note that some implementations may not return any useful status and always
     * return UNKNOWN_MSG_STATUS.
     *
     * @param[in] self          Pointer to this comm-platform
     * @param[in] id            ID returned by sendMessage
     * @return The status of the message
     */
    ocrMsgStatus_t (*getMessageStatus)(struct _ocrCommPlatform_t *self, u64 id);

    /**
     * @brief Returns the sequential index of this policy domain
     *        maintained by the neighbor policy domain.
     *
     * @param[in] self          Pointer to this comm-platform
     * @param[in] neighborLoc   Location of neighbor policy domain
     * @param[in] neighborId    Sequential ID of neighbor location in this policy domain
     */
    u64 (*getSeqIdAtNeighbor)(struct _ocrCommPlatform_t *self, ocrLocation_t neighborLoc, u64 neighborId);
} ocrCommPlatformFcts_t;

/**
 * @brief Interface to a comp-platform representing a
 * resource able to perform computation.
 */
typedef struct _ocrCommPlatform_t {
    struct _ocrPolicyDomain_t *pd;  /**< Policy domain this comm-platform is used by */
    ocrLocation_t location;
    ocrCommPlatformFcts_t fcts;     /**< Functions for this instance */
} ocrCommPlatform_t;


/****************************************************/
/* OCR COMPUTE PLATFORM FACTORY                     */
/****************************************************/

/**
 * @brief comm-platform factory
 */
typedef struct _ocrCommPlatformFactory_t {
    /**
     * @brief Instantiate a new comm-platform and returns a pointer to it.
     *
     * @param factory       Pointer to this factory
     * @param instanceArg   Arguments specific for this instance
     */
    ocrCommPlatform_t* (*instantiate)(struct _ocrCommPlatformFactory_t *factory,
                                      ocrParamList_t *instanceArg);
    void (*initialize)(struct _ocrCommPlatformFactory_t *factory, ocrCommPlatform_t * self,
                       ocrParamList_t *instanceArg);

    /**
     * @brief comm-platform factory destructor
     * @param factory       Pointer to the factory to destroy.
     */
    void (*destruct)(struct _ocrCommPlatformFactory_t *factory);

    ocrCommPlatformFcts_t platformFcts; /**< Function pointers created instances should use */
} ocrCommPlatformFactory_t;

/**
 * @brief Base initializer for comm-platforms
 */
void initializeCommPlatformOcr(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * self, ocrParamList_t *perInstance);

#endif /* __OCR_COMM_PLATFORM_H__ */



