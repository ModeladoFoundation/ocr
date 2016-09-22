/**
 * @brief Macros for hardware-level primitives that the
 * rest of OCR uses. These macros are mostly related
 * to memory primitives
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */



#ifndef __OCR_HAL_FSIM_XE_H__
#define __OCR_HAL_FSIM_XE_H__

#include "ocr-types.h"
#include "xe-abi.h"


/****************************************************/
/* OCR LOW-LEVEL MACROS                             */
/****************************************************/

#ifdef OCR_TICKETLOCK
typedef struct {
    u32 ticket;
    volatile u32 now;
} lock_t;
#define INIT_LOCK ((lock_t){0, 0})

#ifndef OCR_TICKETLOCK_BACKOFF
#define OCR_TICKETLOCK_BACKOFF 0
#endif
#else
/* Default is a test test-and-set type of locking */
typedef volatile u32 lock_t;
#define INIT_LOCK 0
#endif

/**
 * @brief Perform a memory fence
 *
 * @todo Do we want to differentiate different types
 * of fences?
 */

#define hal_fence()                                     \
    do { __asm__ __volatile__("fence 0xF, B\n\t"); } while(0)


/**
 * @brief Memory copy from source to destination
 *
 * @param destination[in]    A u64 pointing to where the data is to be copied
 *                           to
 * @param source[in]         A u64 pointing to where the data is to be copied
 *                           from
 * @param size[in]           A u64 indicating the number of bytes to be copied
 * @param isBackground[in]   A u8: A zero value indicates that the call will
 *                           return only once the copy is fully complete and a
 *                           non-zero value indicates the copy may proceed
 *                           in the background. A fence will then be
 *                           required to ensure completion of the copy
 * @todo Define what behavior we want for overlapping
 * source and destination
 */
#define hal_memCopy(destination, source, size, isBackground)            \
    do {                                                                \
        volatile u64 __dst = (volatile u64)(destination);               \
        volatile u64 __src = (volatile u64)(source);                    \
        __asm__ __volatile__("lea %0, %0\n\t"                           \
                             "lea %1, %1\n\t"                           \
                             "dma.copy %0, %1, %2, 0, 8\n\t"            \
                             :                                          \
                             : "r" ((void *)(__dst)),                   \
                               "r" ((void *)(__src)),                   \
                               "r" (size)                               \
                             : "memory");                               \
        if (!isBackground) hal_fence();                                 \
    } while(0)


/**
 * @brief Memory move from source to destination. As if overlapping portions
 * were copied to a temporary array
 *
 * @param destination[in]    A u64 pointing to where the data is to be copied
 *                           to
 * @param source[in]         A u64 pointing to where the data is to be copied
 *                           from
 * @param size[in]           A u64 indicating the number of bytes to be copied
 * @param isBackground[in]   A u8: A zero value indicates that the call will
 *                           return only once the copy is fully complete and a
 *                           non-zero value indicates the copy may proceed
 *                           in the background. A fence will then be
 *                           required to ensure completion of the copy
 * source and destination
 * @todo This implementation is heavily *NOT* optimized
 */
#define hal_memMove(destination, source, size, isBackground) do {\
    u64 _source = (u64)source;                                          \
    u64 _destination = (u64)destination;                                \
    u64 count = 0;                                                      \
    if(_source != _destination) {                                       \
        if(_source < _destination) {                                    \
            for(count = size -1 ; count > 0; --count)                   \
                ((char*)_destination)[count] = ((char*)_source)[count]; \
            ((char*)_destination)[0] = ((char*)_source)[0];             \
        } else {                                                        \
            for(count = 0; count < size; ++count)                       \
                ((char*)_destination)[count] = ((char*)_source)[count]; \
        }                                                               \
    }                                                                   \
    } while(0)

/**
 * @brief Atomic swap (64 bit)
 *
 * Atomically swap:
 *
 * @param atomic        u64*: Pointer to the atomic value (location)
 * @param newValue      u64: New value to set
 *
 * @return Old value of the atomic
 */
#define hal_swap64(atomic, newValue)                                    \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("xchg %0, %1, 64\n\t"                      \
                             : "+r" (__tmp)                             \
                             : "r" (atomic));                           \
        __tmp;                                                          \
    })

/**
 * @brief Compare and swap (64 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - if location is cmpValue, atomically replace with
 *       newValue and return cmpValue
 *     - if location is *not* cmpValue, return value at location
 *
 * @param atomic        u64*: Pointer to the atomic value (location)
 * @param cmpValue      u64: Expected value of the atomic
 * @param newValue      u64: Value to set if the atomic has the expected value
 *
 * @return Old value of the atomic
 */
#define hal_cmpswap64(atomic, cmpValue, newValue)                       \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("cmpxchg %0, %1, %2, 64\n\t"               \
                             : "+r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (cmpValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Atomic add (64 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - return old value (before addition)
 *
 * @param atomic    u64*: Pointer to the atomic value (location)
 * @param addValue  u64: Value to add to location
 * @return Old value of the location
 */
#define hal_xadd64(atomic, addValue)                                    \
    ({                                                                  \
        u64 __tmp;                                                      \
        __asm__ __volatile__("xaddI %0, %1, %2, 64, R\n\t"              \
                             : "=r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (addValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Remote atomic add (64 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - no value is returned (the increment will happen "at some
 *       point")
 *
 * @param atomic    u64*: Pointer to the atomic value (location)
 * @param addValue  u64: Value to add to location
 */
#define hal_radd64(atomic, addValue)                                    \
    __asm__ __volatile__("xaddI %1, %0, %1, 64, N\n\t"                  \
                         : "r" (atomic),                                \
                           "r" (addValue));

/**
 * @brief Atomic swap (32 bit)
 *
 * Atomically swap:
 *
 * @param atomic        u32*: Pointer to the atomic value (location)
 * @param newValue      u32: New value to set
 *
 * @return Old value of the atomic
 */
#define hal_swap32(atomic, newValue)                                    \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("xchg %0, %1, 32\n\t"                      \
                             : "+r" (__tmp)                             \
                             : "r" (atomic));                           \
        __tmp;                                                          \
    })

/**
 * @brief Compare and swap (32 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - if location is cmpValue, atomically replace with
 *       newValue and return cmpValue
 *     - if location is *not* cmpValue, return value at location
 *
 * @param atomic        u32*: Pointer to the atomic value (location)
 * @param cmpValue      u32: Expected value of the atomic
 * @param newValue      u32: Value to set if the atomic has the expected value
 *
 * @return Old value of the atomic
 */
#define hal_cmpswap32(atomic, cmpValue, newValue)                       \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("cmpxchg %0, %1, %2, 32\n\t"               \
                             : "+r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (cmpValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Atomic add (32 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - return old value (before addition)
 *
 * @param atomic    u32*: Pointer to the atomic value (location)
 * @param addValue  u32: Value to add to location
 * @return Old value of the location
 */
#define hal_xadd32(atomic, addValue)                                    \
    ({                                                                  \
        u64 __tmp;                                                      \
        __asm__ __volatile__("xaddI %0, %1, %2, 32, R\n\t"              \
                             : "=r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (addValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Remote atomic add (32 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - no value is returned (the increment will happen "at some
 *       point")
 *
 * @param atomic    u32*: Pointer to the atomic value (location)
 * @param addValue  u32: Value to add to location
 */
#define hal_radd32(atomic, addValue)                                    \
    __asm__ __volatile__("xaddI %1, %0, %1, 32, N\n\t"                  \
                         : "r" (atomic),                                \
                           "r" (addValue));

/**
 * @brief Atomic swap (16 bit)
 *
 * Atomically swap:
 *
 * @param atomic        u16*: Pointer to the atomic value (location)
 * @param newValue      u16: New value to set
 *
 * @return Old value of the atomic
 */
#define hal_swap16(atomic, newValue)                                    \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("xchg %0, %1, 16\n\t"                      \
                             : "+r" (__tmp)                             \
                             : "r" (atomic));                           \
        __tmp;                                                          \
    })

/**
 * @brief Compare and swap (16 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - if location is cmpValue, atomically replace with
 *       newValue and return cmpValue
 *     - if location is *not* cmpValue, return value at location
 *
 * @param atomic        u16*: Pointer to the atomic value (location)
 * @param cmpValue      u16: Expected value of the atomic
 * @param newValue      u16: Value to set if the atomic has the expected value
 *
 * @return Old value of the atomic
 */
#define hal_cmpswap16(atomic, cmpValue, newValue)                       \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("cmpxchg %0, %1, %2, 16\n\t"               \
                             : "+r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (cmpValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Atomic add (16 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - return old value (before addition)
 *
 * @param atomic    u16*: Pointer to the atomic value (location)
 * @param addValue  u16: Value to add to location
 * @return Old value of the location
 */
#define hal_xadd16(atomic, addValue)                                    \
    ({                                                                  \
        u64 __tmp;                                                      \
        __asm__ __volatile__("xaddI %0, %1, %2, 16, R\n\t"              \
                             : "=r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (addValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Remote atomic add (16 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - no value is returned (the increment will happen "at some
 *       point")
 *
 * @param atomic    u16*: Pointer to the atomic value (location)
 * @param addValue  u16: Value to add to location
 */
#define hal_radd16(atomic, addValue)                                    \
    __asm__ __volatile__("xaddI %1, %0, %1, 16, N\n\t"                  \
                         : "r" (atomic),                                \
                           "r" (addValue));

/**
 * @brief Atomic swap (8 bit)
 *
 * Atomically swap:
 *
 * @param atomic        u8*: Pointer to the atomic value (location)
 * @param newValue      u8: New value to set
 *
 * @return Old value of the atomic
 */
#define hal_swap8(atomic, newValue)                                     \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("xchg %0, %1, 8\n\t"                       \
                             : "+r" (__tmp)                             \
                             : "r" (atomic));                           \
        __tmp;                                                          \
    })

/**
 * @brief Compare and swap (8 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - if location is cmpValue, atomically replace with
 *       newValue and return cmpValue
 *     - if location is *not* cmpValue, return value at location
 *
 * @param atomic        u8*: Pointer to the atomic value (location)
 * @param cmpValue      u8: Expected value of the atomic
 * @param newValue      u8: Value to set if the atomic has the expected value
 *
 * @return Old value of the atomic
 */
#define hal_cmpswap8(atomic, cmpValue, newValue)                        \
    ({                                                                  \
        u64 __tmp = newValue;                                           \
        __asm__ __volatile__("cmpxchg %0, %1, %2, 8\n\t"                \
                             : "+r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (cmpValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Atomic add (8 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - return old value (before addition)
 *
 * @param atomic    u8*: Pointer to the atomic value (location)
 * @param addValue  u8: Value to add to location
 * @return Old value of the location
 */
#define hal_xadd8(atomic, addValue)                                     \
    ({                                                                  \
        u64 __tmp;                                                      \
        __asm__ __volatile__("xaddI %0, %1, %2, 8, R\n\t"               \
                             : "=r" (__tmp)                             \
                             : "r" (atomic),                            \
                               "r" (addValue));                         \
        __tmp;                                                          \
    })

/**
 * @brief Remote atomic add (8 bit)
 *
 * The semantics are as follows (all operations performed atomically):
 *     - atomically increment location by addValue
 *     - no value is returned (the increment will happen "at some
 *       point")
 *
 * @param atomic    u8*: Pointer to the atomic value (location)
 * @param addValue  u8: Value to add to location
 */
#define hal_radd8(atomic, addValue)                                    \
    __asm__ __volatile__("xaddI %1, %0, %1, 8, N\n\t"                  \
                         : "r" (atomic),                               \
                           "r" (addValue));


#ifdef OCR_TICKETLOCK
#define hal_lock(__l)                                       \
    do {                                                    \
        volatile u32 _i;                                    \
        u32 _t = hal_xadd32(&((__l)->ticket), 1);           \
        while(true) {                                       \
            u32 _spinTime = OCR_TICKETLOCK_BACKOFF*(_t - (__l)->now);            \
            for(_i = 0; _i < _spinTime; ++_i)               \
                ;                                           \
            if(_t == (__l)->now) break;                     \
        }                                                   \
        __asm__ __volatile__ ("" : : : "memory");           \
    } while(0);

/* Ideally need a release barrier; using a full barrier
 * and a flush to make sure everything prior is visible prior to the
 * ticket increment */
#define hal_unlock(__l)                                     \
    do {                                                    \
        __asm__ __volatile__ ("flush B,N" : : : "memory");  \
        ++((__l)->now);                                     \
    } while(0);

#define hal_trylock(__l)                                    \
    ({                                                      \
        u8 _r = 0;                                          \
        u32 _val = (__l)->ticket;                           \
        if(_val == (__l)->now) {                            \
            u32 _old = hal_cmpswap32(&((__l)->ticket), _val, _val+1);\
            if(_old != _val) _r = 1;                        \
            else __asm__ __volatile__ ("" : : : "memory");  \
        } else {                                            \
            _r = 1;                                         \
        }                                                   \
        _r;                                                 \
    })

#define hal_islocked(__l) ((__l)->ticket > (__l)->now)
#else /* ! OCR_TICKETLOCK */
/**
 * @brief Convenience function that basically implements a simple
 * lock
 *
 * This will usually be a wrapper around cmpswap32. This function
 * will block until the lock can be acquired
 *
 * @param lock      Pointer to a 32 bit value
 */
#define hal_lock(__l)                                   \
    do {                                                \
        while(hal_cmpswap32(__l, 0, 1))                 \
            while(*(__l))                               \
                ;                                       \
        __asm__ __volatile__ ("" : : : "memory");       \
    } while(0);


/**
 * @brief Convenience function to implement a simple
 * unlock
 *
 * @param lock      Pointer to a 32 bit value
 */
#define hal_unlock(__l)                                         \
    do {                                                        \
        __asm__ __volatile__ ("flush B,N" : : : "memory");      \
        *(u32 *)(__l) = 0;                                      \
    } while(0);

/**
 * @brief Convenience function to implement a simple
 * trylock
 *
 * @param lock      Pointer to a 32 bit value
 * @return 0 if the lock has been acquired and a non-zero
 * value if it cannot be acquired
 */
#define hal_trylock(__l)                                      \
    ({                                                        \
        u32 _r = hal_cmpswap32(__l, 0, 1);                    \
        if(_r == 0) __asm__ __volatile__("" : : : "memory");  \
        _r;                                                   \
    })

#define hal_islocked(__l) (*(__l) == 1)
#endif

/**
 * @brief Abort the runtime
 *
 * Will crash the runtime
 */
#define hal_abort()                             \
    __asm__ __volatile__(".quad 0\n\t")

/**
 * @brief Exit the runtime
 *
 * This will exit the runtime more cleanly than abort
 */
#define hal_exit(arg)                           \
    __asm__ __volatile__("alarm %0\n\t" : : "L" (XE_TERMINATE_ALARM))

/**
 * @brief Pause execution
 *
 * This is used to support a primitive version of ocrWait
 * and may be deprecated in the future
 */
#define hal_pause() do {                        \
        u32 _i = 1000;                          \
        while(_i > 0) --_i;                     \
    } while(0)

/**
 * @brief Put the XE core to sleep
 *
 * Currently does nothing
 */
#define hal_sleep(id) do {                       \
    } while(0)

/**
 * @brief Wake the XE core from sleep
 *
 * Currently does nothing
 */
#define hal_wake(id) do {                         \
    } while(0)

/**
 * @brief On architectures (like TG) that
 * have different address "formats", canonicalize it
 * to the unique form
 */
#define hal_globalizeAddr(addr) ({                                      \
    u64 __dest;                                                         \
    __asm__ __volatile__("lea %0, %1\n\t" : "=r" (__dest) : "r" (addr) ); \
    __dest;                                                             \
        })

/**
 * @brief On architectures (like TG) that have
 * different address "formats", this returns the
 * smallest usable address from the global address 'addr'
 */
#define hal_localizeAddr(addr) addr

// Abstraction to do a load operation from any level of the memory hierarchy
#define GET8(temp, addr)   ((temp) = *((u8*)(addr)))
#define GET16(temp, addr)  ((temp) = *((u16*)(addr)))
#define GET32(temp, addr)  ((temp) = *((u32*)(addr)))
#define GET64(temp, addr)  ((temp) = *((u64*)(addr)))
// Abstraction to do a store operation to any level of the memory hierarchy
#define SET8(addr, value)  (*((u8*)(addr))  = (u8)(value))
#define SET16(addr, value) (*((u16*)(addr)) = (u16)(value))
#define SET32(addr, value) (*((u32*)(addr)) = (u32)(value))
#define SET64(addr, value) (*((u64*)(addr)) = (u64)(value))

/**
 * @brief Start a new chain
 *
 * @param decaddr[in]     u64*: Pointer memory location in tg to decrement
 *                        after sucessful completion of entire chain.
 *                        location is to be decremented upon completion of this
 *                        chain.
 * @return id of allotted chain.
 * @todo Add interrupt and decrement options.
 *       interrupt[in]   Not supported yet. u8 0 or 1 to indicate if an interrupt
 *                        is desired after completion of this chain.
 *       decrement[in]   Not supported yet. u8 0 or 1 to indicate if a memory
 */
#define hal_chain_start(decaddr) ({                          \
    u64 __cid;                                             \
    __asm__ __volatile__("chain.init %0, %1 , N , N, 64\n\t"           \
                              : "=r" (__cid)            \
                              : "r" (decaddr));            \
    __cid;                                                              \
       })

/**
 * @brief End chain that is currently open.
 */
#define hal_chain_end(decaddr) ({                          \
    __asm__ __volatile__("chain.end\n\t"           \
                              :             \
                              :           );  \
       })


/**
 * @brief Wait on a chain whose id matches with supplied hwid.
 * If no match found , call returns.
 * @param cid[in]      u64 chain id to wait upon.
 */
#define hal_chain_wait(cid) ({                          \
    __asm__ __volatile__("chain.wait %0, 64\n\t"           \
                              :             \
                              : "r" (cid));            \
       })


/**
 * @brief Poll chain whose id matches with supplied hwid.
 * If no match found , call returns.
 * @param cid[in]      u64 chain id to wait upon.
 *
 * @return u64 status of the chain.
 */
#define hal_chain_poll(cid)                          \
    ({                                                                  \
        u64 __status;                                 \
    __asm__ __volatile__("chain.poll %0, %1, 64\n\t"           \
                              : "=r" (__status)            \
                              : "r" (cid));            \
        __status;\
    })

/**
 * @brief Kill chain whose id matches with supplied hwid.
 * If no match found , call returns.
 * @param hwid[in]      u64 chain id of chain to kill.
 *
 */
#define hal_chain_kill(cid)                          \
    ({                                                                  \
    __asm__ __volatile__("chain.kill %0,64\n\t"           \
                              :             \
                              : "r" (cid));            \
    })

/**
 * @brief Add 1 item to head of specified queue in blocking fashion.
 *
 * @param item[in]     u64 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_head_64(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, H, 64\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Add 1 item to head of specified queue in blocking fashion.
 *
 * @param item[in]     u32 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_head_32(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, H, 32\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })
/**
 * @brief Add 1 item to head of specified queue in blocking fashion.
 *
 * @param item[in]     u16 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_head_16(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, H, 16\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })
/**
 * @brief Add 1 item to head of specified queue in blocking fashion.
 *
 * @param item[in]     u8 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_head_8(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, H, 8\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Add 1 item to tail of specified queue in blocking fashion.
 *
 * @param item[in]     u64 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_tail_64(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, T, 64\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Add 1 item to tail of specified queue in blocking fashion.
 *
 * @param item[in]     u32 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_tail_32(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, T, 32\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })
/**
 * @brief Add 1 item to tail of specified queue in blocking fashion.
 *
 * @param item[in]     u16 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_tail_16(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, T, 16\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })
/**
 * @brief Add 1 item to tail of specified queue in blocking fashion.
 *
 * @param item[in]     u8 item to added in queue
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_add1_b_tail_8(item, qbuff)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.add1.w %0, %1, T, 8\n\t"           \
                              :             \
                              : "r" (item),            \
                                "r" (qbuff));            \
    })


/**
 * @brief Remove 1 item from head of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u64 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_head_64(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, H, 64\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from head of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u32 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_head_32(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, H, 32\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from head of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u16 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_head_16(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, H, 16\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from head of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u8 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_head_8(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, H, 8\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from tail of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u64 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_tail_64(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, T, 64\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from tail of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u32 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_tail_32(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, T, 32\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from tail of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u16 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_tail_16(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, T, 16\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })
/**
 * @brief Remove 1 item from tail of specified queue in blocking fashion.
 *
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 * @return u8 element popped from queue. Cast it to size you desire.
 */
#define hal_q_rem1_b_tail_8(qbuff)                          \
    ({                                                                  \
        u64 __item ; \
    __asm__ __volatile__("qma.rem1.w %0, %1, T, 8\n\t"           \
                              : "=r" (__item)            \
                              : "r" (qbuff));            \
        __item;\
    })


/**
 * @brief Add n items to head of specified queue in blocking fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_head_64(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, H, 64\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items of size 32 to head of specified queue in blocking
 *        fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_head_32(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, H, 32\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items of size 16 to head of specified queue in blocking fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_head_16(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, H, 16\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items of size 8 to head of specified queue in blocking fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_head_8(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, H, 8\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items to tail of specified queue in blocking fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_tail_64(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, T, 64\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items of size 32 to tail of specified queue in blocking
 *        fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_tail_32(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, T, 32\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items of size 16 to tail of specified queue in blocking fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_tail_16(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, T, 16\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })

/**
 * @brief Add n items of size 8 to tail of specified queue in blocking fashion.
 *
 * @param daddr[in]     u64 tg-address where elements are located
 *                     (address of 1st element)
 * @param dnum[in]     u64 number of elements to be read from addr.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_addx_b_tail_8(qbuff, daddr, dnum)                      \
    ({                                                                  \
    __asm__ __volatile__("qma.addX.w %0, %1, %2, T, 8\n\t"           \
                              :             \
                              : "r" (qbuff),            \
                                "r" (daddr),            \
                                "r" (dnum));            \
    })


/**
 * @brief Remove n items of size 64 from head of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_head_64(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, H, 64\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 32 from head of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_head_32(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, H, 32\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 16 from head of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_head_16(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, H, 16\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 8 from head of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_head_8(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, H, 8\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 64 from tail of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_tail_64(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, T, 64\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 32 from tail of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_tail_32(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, T, 32\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 16 from tail of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_tail_16(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, T, 16\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })

/**
 * @brief Remove n items of size 8 from tail of specified queue in blocking fashion.
 *
 * @param daddr[in]    u64 tg-address where elements popped from queue
 *                     will be written.
 * @param dnum[in]     u64 number of elements to be removed from queue.
 * @param qbuff[in]    u64 Target queue's qbuff
 *
 */
#define hal_q_remx_b_tail_8(qbuff, daddr, dnum)                          \
    ({                                                                  \
    __asm__ __volatile__("qma.remX.w %0, %1, %2, T, 8\n\t"           \
                              :             \
                              : "r" (daddr),            \
                                "r" (dnum),            \
                                "r" (qbuff));            \
    })


#endif /* __OCR_HAL_FSIM_XE_H__ */
