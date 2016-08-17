/**
 * @brief Implementation of quick allocator
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

// Quick allocator is based on TLSF allocator.
// An instance of a Quick allocator generally manages the entire inventory of dynamically allocatable
// memory at one particular memory domain.  For example, all the non-statically allocated memory in
// one L1 scratch pad; or in L2 scratch pad; or L3; ...; or "MCDRAM" (i.e. an external memory typified
// by smaller size, higher bandwidth, and usually around the same latency as DRAM); or other external
// memory such as GDDR or NVM.
//
// A "pool" is comprised of the overall bookkeeping structure followed by the "net" space that is
// actually available for parceling into the requested blocks.  The bookkeeping area is now called
// poolHdr_t, though the typedef for that struct is only able to portray the constant-sized components
// and it is necessary to annex onto this struct the storage for the variable-length components.
// (The size of this annex is determined when the pool is instantiated (see quickInitAnnex), after
// which its size is invariant.)
// The space in the pool available for parceling is called the "glebe" (hearkening to parish farmlands
// distributable to peasant farmers at the whim of the parish council).
//
// Blocks in the glebe, be they allocated or free, they are comprised of three distinct parts: a header,
// the "payload", and the tail. Thus, a block consists of the header structure, followed by
// the payload, which is the part that the user "sees" after a successful allocation request, and
// which he subsequently frees when he is done with it, at which time the entire block is returned
// to the glebe. Also, the tail follows the payload.
//
// The glebe is initially comprised of one really big free block spanning from the start of the
// glebe right up to the end of the glebe.
//
// The above constructs are depicted thusly:
//
//   *  Every pool is comprised of the following (not to scale):
//      ========================================================
//
//     poolHdr_t      pool header annex      glebe
//   +--------------+----------------------+------------------------------------------------+
//   |              |                      |                                                |
//   | see notes at | see notes at         |                                                |
//   | poolHdr_t    | poolHdr_t            |                                                |
//   | typedef      | typedef              |                                                |
//   | for          | for                  |                                                |
//   | contents     | contents             |                                                |
//   |              |                      |                                                |
//   +--------------+----------------------+------------------------------------------------+
//                                         .                                                .
//                                         .                                                .
//                                         .                                                .
//                                         .    *  The glebe contains the blocks:           .
//   . . . . . . . . . . . . . . . . . . . .       ==============================           .
//   .                                                                                      .
//   .                                                                                      .
//   . used block    free block    used block        used blk  free block        used block .
//   +-------------+-------------+-----------------+---------+-----------------+------------+
//   |             |             |                 |         |                 |            |
//   |             |             |                 |         |                 |            |
//   +-------------+-------------+-----------------+---------+-----------------+------------+
//   .             .                                         .                 .
//   .             .                                         .                 .
//   .             . * Blocks have a header, space, tail     .                 .
//   .             .   =================================     .                 .
//   .             .                                         .                 .
//   . used block: . . . . . . . . . . . . . .     . . . . . . free block:     . . . . . . .
//   .                                       .     .                                       .
//   . header     payload                    .     . header     free space                 .
//   +----------+------------------------+---+     +----------+------------------------+---+
//   | see      |                        |   |     | seee     | N P                    |   |
//   | header   |                        | T |     | header   | E R                    | T |
//   | layout   | user-visible datablock | a |     | layout   | X E                    | a |
//   | below    |                        | i |     | below    | T V                    | i |
//   |          |                        | l |     |          |                        | l |
//   +----------+------------------------+---+     +----------+------------------------+---+
//
//
// Terminology:
// "previous" and "next" : relative positions along a linked lists (freelist)
// LEFT and RIGHT : LEFT/RIGHT is used to refer to the neighbor blocks for the spatial adjacency context
//
// For example:
//
//   NOTES:
//
//   *  "U" means Used block; "F" means Free block.  It is impossible for there to be two free blocks in a row.
//   *  The head of the free list comes from the pool header (annex, see secondLevel_t), and there is one free
//      list for each "bucket" of free block sizes.  But only TWO free lists are depicted, to keep this simple
//      diagram from getting messy. Each block is doubly linked-listed by NEXT/PREV and it is expressed as
//      number anchors below.
//
//        +-------------------------------------------------------------------------+
//        |  poolHdr_t (annex) :                                                    |
//        |                                                                         |
//        |  Free block buckets.                                                    |
//        |                   *                                             *       |
//        +-------------------|---------------------------------------------|-------+
//                            |                                             |
//                            +------------------------------+              +-------+
//                                                           |                      |
//                                                           v                      v
//
//                          +----------------------------+   2                      3
//                          |                            |   |                      |
//              +---+      +|--+      +---+      +---+   |  +|--+      +---+      +-|-+      +---+      +---+
//              |   |      |v  |      |   |      |   |   |  |v  |      |   |      | v |      |   |      |   |
//  NEXT:       |   |   2<--*  |      |   |      |   |   +---*  |      |   |   3<---* |      |   |      |   |
//              |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |
//  PEER_RIGHT: |*--------->*--------->*--------->*--------->*--------->*--------->*--------->*--------->   |
//              |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |
//              | U |      | F |      | U |      | U |      | F |      | U |      | F |      | U |      | U |
//              |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |
//  PEER_LEFT:  |*<---------*<---------*<---------*<---------*<---------*<---------*<---------*<---------*  |
//              |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |
//              |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |      |   |
//  PREV:       |   |   1-->*  |      |   |      |   |   1<--*  |      |   |   4--->* |      |   |      |   |
//              |   |      ||  |      |   |      |   |      |^  |      |   |      | | |      |   |      |   |
//              +---+      +|--+      +---+      +---+      +|--+      +---+      +-|-+      +---+      +---+
//                          |                                |                      v
//                          +--------------------------------+                      4
//
//
// Initialization:
// In quickInit, it acquires pool->lock and whoever acquires this lock first finds 'init_count' zero and perform
// initializations. Others acquire this lock, but find 'init_count' nonzero and skip initialization part.
// init_count is increased in either case. So, init_count will reach the number of users for that memory.
// For shutdown, in quickFinish, similarly, init_count is decreased and the last one who decreases it to zero
// performs shutdown and print out leak report.

#include "ocr-config.h"
#ifdef ENABLE_ALLOCATOR_QUICK
#ifndef ENABLE_ALLOCATOR_QUICK_STANDALONE
// for regular OCR runs
#include "ocr-hal.h"
#include "debug.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-types.h"
#include "utils/ocr-utils.h"
#include "quick-allocator.h"
#include "allocator/allocator-all.h"
#include "ocr-mem-platform.h"
#endif

#define DEBUG_TYPE ALLOCATOR

typedef void blkPayload_t; // Strongly type-check the ptr-to-void that comprises the ptr-to-payload results.

#define ALIGNMENT               (8LL)
#define ALIGNMENT_MASK          (ALIGNMENT-1)

// Block header layout
//
// * free block
//
//  x[0]  x[1]  x[2]  x[3]  x[4]
// +-----+-----+-----+-----+-----+-----------+-----+
// |HEAD |INFO1|INFO2|NEXT |PREV |    ...    |TAIL |
// |     |     |     |     |     |           |     |
// +-----+-----+-----+-----+-----+-----------+-----+
//
// * allocated block
//
//  x[0]  x[1]  x[2]
// +-----+-----+-----+-----------------------+-----+
// |HEAD |INFO1|INFO2|          ...          |TAIL |
// |     |     |     |      user-visible     |     |
// +-----+-----+-----+-----------------------+-----+
//
// HEAD and TAIL contains full size including HEAD/INFOs/TAIL in bytes and HEAD also has MARK in its higher 8 bits.
// HEAD's bit0 flag is 1 for allocated block, or 0 for free block. see below for details
// i.e.  HEAD == ( MARK | User flags | size | flags )  , and TAIL == MARK | size

// PEER_LEFT and PEER_RIGHT helps access neightbor blocks.
// NEXT and PREV is valid for free blocks and forms linked list for free list.
// INFO1 contains a pointer to the pool header which it belongs to. i.e. poolHdr_t
// INFO2 contains canonical address for other agent/CE can use to free this block. (useful only on TG)

//        |MSB  |                                                       LSB|
//        |8bits|                  56 bits                                 |
//        +-----+----------------------------------------------------------+
// HEAD   |MARK | User flags  ...              size (low 3 bits are flags) |
//        +-----+----------------------------------------------------------+
//
//        +-----+----------------------------------------------------------+
// TAIL   |MARK |                              size                        |
//        +-----+----------------------------------------------------------+

//#define FINE_LOCKING     // WIP, disabled at this moment.

// This mark helps detect invalid ptr or double free.
#define SHIFT_USER              (48)
#define SHIFT_MARK              (56)
#define MARK                    (0x5cUL << SHIFT_MARK)           // arbitrary number, but it's recommended the first bit set to 0 for slab allocator.
#define MASK_MARK               (0xffUL << SHIFT_MARK)
#define MASK_USER               (0xffUL << SHIFT_USER)
#define MASK_FLAG               (0x7UL)
#define MASK_SIZE               (~(MASK_MARK|MASK_USER|MASK_FLAG))

#define HEAD(X)                 (((u64 *)(X))[0])
#ifdef FINE_LOCKING
#define HEAD_LOCK(X)            (((u32 *)(X))[1])
#endif
#ifndef FINE_LOCKING
struct bmapOp {int dummy;};
#else
struct bmapOp {
    u32 fli[3],sli[3];
    s32 delta[3],count;
};
#endif

// first 64bit == HEAD
// last 64bit == TAIL
#ifdef FINE_LOCKING
#define TAIL_LOCK(X,SIZE)       (*(u32 *)((u8 *)(X)+(SIZE)-sizeof(u32)))
#endif
#define TAIL(X,SIZE)            (*(u64 *)((u8 *)(X)+(SIZE)-sizeof(u64)))
#define PEER_LEFT(X)            ((X)[-( ((s32 *)(X))[-2] >> 3 )])
#define PEER_LEFT_TAIL_LOCK(X)  (((s32 *)(X))[-1])
#define PEER_RIGHT(X,SIZE)      (*(u64 *)((u8 *)(X)+(SIZE)))

#define MAX_BLOCK_SIZE          (0x1UL << 48) // this may viewed as a max pool size roughly
#define FLAG_FREE               (0)
#define FLAG_INUSE              (1)
#define FLAG_MERGE              (2) // block locked, and is to be merged
#define FLAG_INUSE_SLAB         (3)
#define FLAG_FOR_SLAB           (0x2)  // FLAG_INUSE | FLAG_FOR_SLAB == FLAG_INUSE_SLAB
#define GET_FLAG(X)             (MASK_FLAG & (X))
#define GET_SIZE(X)             (MASK_SIZE & (X))
#define GET_USER(X)             (MASK_USER & (X))
#ifdef FINE_LOCKING
#define GET_MARK(X)             (MARK)        // disable
#else
//#define GET_MARK(X)             (TAIL_MARK((X),GET_SIZE(HEAD(X))))
#define GET_MARK(X)             (HEAD(X) & MASK_MARK)
#endif

// Additional INFOs
#define INFO1(X)                ((X)[1])
#define INFO2(X)                ((X)[2])
// in case of free block, prev/next is used to get linked into free list
// PREV/NEXT exists only on free blocks.. i.e. not on allocated blocks,
// because only free blocks go into free list.
#define NEXT(X)                 ((X)[3])
#define PREV(X)                 ((X)[4])
// Conversion between user-provided address and block header
#define HEAD_TO_USER(X)         ((X)+3)
#define USER_TO_HEAD(X)         (((u64 *)(X))-3)

// At the moment, we have alloc overhead of 4 u64 to user payload (HEAD,INFO1,INFO2,and TAIL)
#define ALLOC_OVERHEAD          (4*sizeof(u64))

// Minimum allocatable size from user's perspective is 2*u64 for prev/next for free list
// Thus, the minimum block size internally below
#define MINIMUM_SIZE            (2*sizeof(u64) + ALLOC_OVERHEAD)
// known value is placed at the end of heap as a guard
#define KNOWN_VALUE_AS_GUARD    0xfeed0000deadbeef

#if defined(HAL_FSIM_CE) || defined(HAL_FSIM_XE)
// See bug #875
//TODO: Re-enable the below after moving the globals into allocator structs
//Disabled for 4.1.0
//#define PER_THREAD_CACHE
// TG
#define PER_AGENT_KEYWORD
#define CACHE_POOL(ID)          (&_cache_pool)
#define _CACHE_POOL(ID)         (_cache_pool)
#else
// x86 or standalone
#define PER_AGENT_CACHE
#define PER_AGENT_KEYWORD       __thread
#define CACHE_POOL(ID)          (cache_pool)
#endif

#ifdef PER_AGENT_CACHE
#define SLAB_MARK               ( 0xfeed )        // arbitrary number
#define SLAB_SHIFT              ( 4UL )           // differences between bins
#define SLAB_MASK               (( 1UL << SLAB_SHIFT ) - 1UL )
#define SIZE_TO_SLABS(size)     ( ((size)+SLAB_MASK) >> SLAB_SHIFT )
#define SLAB_MAX_SIZE(index)    (  (index) << SLAB_SHIFT )
#define SLAB_OVERHEAD           ( sizeof(u64)*3 )
#define MAX_SLABS               ( 36 )
#define MAX_SIZE_FOR_SLABS      SLAB_MAX_SIZE(MAX_SLABS-1)
#define MAX_OBJ_PER_SLAB        ( 63 )

// Each agent (or thread) has pointers to an array of objects it allocates from the central heap.
// When the allocation requests come, it first checks this per-agent lists for free object before it goes to the central heap.
struct per_agent_cache {
    void *slabs[MAX_SLABS];
    s32 count_malloc[MAX_SLABS];
    s32 count_free[MAX_SLABS];
    lock_t lock;
};

PER_AGENT_KEYWORD
struct per_agent_cache *CACHE_POOL(ID);

struct slab_header {
    struct slab_header *next, *prev;
    struct per_agent_cache *per_agent;
    u64 bitmap;
    u32 mark;
    u32 size;
};
#endif

//#define ALIGN_CACHE_LINE       // disabled. needs review
#ifdef ALIGN_CACHE_LINE
#define CACHE_LINE_SHIFT        (6)
#define CACHE_LINE_MASK         ((1UL<<(CACHE_LINE_SHIFT))-1)
#endif

// VALGRIND SUPPORT
// ( blocks are sometimes called chunks. i.e. chunk == block )
// VALGRIND_MEMPOOL_ALLOC: If the pool was created with the is_zeroed argument set, Memcheck will mark the chunk as DEFINED, otherwise Memcheck will mark the chunk as UNDEFINED.
// VALGRIND_MEMPOOL_FREE:  Memcheck will mark the chunk associated with addr as NOACCESS, and delete its record of the chunk's existence.
// VALGRIND_MAKE_MEM_NOACCESS, VALGRIND_MAKE_MEM_UNDEFINED and VALGRIND_MAKE_MEM_DEFINED. These mark address ranges as completely inaccessible, accessible but containing undefined data, and accessible and containing defined data, respectively. They return -1, when run on Valgrind and 0 otherwise.

// VALGRIND_CREATE_MEMPOOL(pool, rzB, is_zeroed): This request registers the address pool as the anchor address for a memory pool.
// VALGRIND_DESTROY_MEMPOOL(pool): This request tells Memcheck that a pool is being torn down. Memcheck resets the redzones of any live chunks in the pool to NOACCESS.

#ifdef ENABLE_VALGRIND
#include <valgrind/memcheck.h>
#define VALGRIND_POOL_OPEN(X)                                                  \
    do {                                                                       \
        VALGRIND_MAKE_MEM_DEFINED((X), sizeof(poolHdr_t));                     \
        VALGRIND_MAKE_MEM_DEFINED((X) , (u64)((X)->glebeStart) - (u64)(X) );   \
    } while(0)
#define VALGRIND_POOL_CLOSE(X)                                                 \
    do {                                                                       \
        VALGRIND_MAKE_MEM_NOACCESS((X) , (u64)((X)->glebeStart) - (u64)(X) );  \
    } while(0)
#define VALGRIND_CHUNK_OPEN(X)                                                 \
    do {                                                                       \
        VALGRIND_MAKE_MEM_DEFINED(&HEAD(X), 3*sizeof(u64));                    \
        if (GET_FLAG(HEAD(X)) == FLAG_FREE) {                                  \
            VALGRIND_MAKE_MEM_DEFINED((u64 *)(X)+3 , 2*sizeof(u64));           \
        }                                                                      \
        VALGRIND_MAKE_MEM_DEFINED(&TAIL((X), GET_SIZE(HEAD(X))), sizeof(u64)); \
    } while(0)
#define VALGRIND_CHUNK_OPEN_INIT(X, Y)                             \
    do {                                                           \
        VALGRIND_MAKE_MEM_DEFINED(&HEAD(X), 5*sizeof(u64));        \
        VALGRIND_MAKE_MEM_DEFINED(&TAIL((X), (Y)), sizeof(u64));   \
    } while(0)
#define VALGRIND_CHUNK_OPEN_LEFT(X)     VALGRIND_MAKE_MEM_DEFINED(&(X)[-1], sizeof(u64));
#define VALGRIND_CHUNK_OPEN_COND(X, Y)  \
    if ((X) != (Y)) {                   \
        VALGRIND_CHUNK_OPEN(Y);         \
    }
#define VALGRIND_CHUNK_CLOSE(X)                                          \
    do {                                                                 \
        int cond = GET_FLAG(HEAD(X));                                    \
        int size = GET_SIZE(HEAD(X));                                    \
        VALGRIND_MAKE_MEM_NOACCESS(&HEAD(X), 3*sizeof(u64));             \
        if (cond==FLAG_FREE)                                             \
            VALGRIND_MAKE_MEM_NOACCESS((u64 *)(X)+3 , 2*sizeof(u64));    \
            VALGRIND_MAKE_MEM_NOACCESS(&TAIL((X), size ), sizeof(u64));  \
    } while(0)
#define VALGRIND_CHUNK_CLOSE_COND(X, Y)  \
    if ((X) != (Y)) {                    \
        VALGRIND_CHUNK_CLOSE(Y);         \
    }
#else
#define VALGRIND_POOL_OPEN(X)
#define VALGRIND_POOL_CLOSE(X)
#define VALGRIND_CHUNK_OPEN(X)
#define VALGRIND_CHUNK_OPEN_INIT(X, Y)
#define VALGRIND_CHUNK_OPEN_LEFT(X)
#define VALGRIND_CHUNK_OPEN_COND(X, Y)
#define VALGRIND_CHUNK_CLOSE(X)
#define VALGRIND_CHUNK_CLOSE_COND(X, Y)
#endif


// start of tlsf core part
/*
 * Number of subdivisions in the second-level list
 * Can be 2 to 5; 6 if slAvailOrNot is changed from u32 to u64.
 * 4 is the "sweet spot";  less results in more fragmentation;
 * more results in more pool header overhead.
 */
#define SL_COUNT_LOG2 4LL

/*
 * Support allocations/memory of size up to (1 << FL_MAX_LOG2) * ALIGNMENT
 * For 64KB memory: 13 (at ALIGNMENT == 8)
 * For  1MB memory: 17 (at ALIGNMENT == 8)
 * For  1GB memory: 27 (at ALIGNMENT == 8)
 * For  8EB memory: 60 (at ALIGNMENT == 8)
 */
#define FL_MAX_LOG2 60LL

/*
 * Some computed values:
 *  - SL_COUNT: Number of buckets in each SL list
 *  - FL_COUNT_SHIFT: For the first SL_COUNT, we do not need to maintain
 *  first level lists (since they all go in the "0th" one.
 *  - ZERO_LIST_SIZE: Size under which we are in SL 0
 */
enum computedVals {
    SL_COUNT        = (1LL << SL_COUNT_LOG2),
    FL_COUNT_SHIFT  = (SL_COUNT_LOG2),
    ZERO_LIST_SIZE  = (1LL << FL_COUNT_SHIFT),
};

// second level structure in the annex area because they are variable-length at initialization.
typedef struct {
    u32 slAvailOrNot;
    u32 freeList[SL_COUNT];
#ifdef FINE_LOCKING
    lock_t listLock[SL_COUNT];
    s32 listCount[SL_COUNT];
    lock_t bmapLockSL;
    s32 count;          // count for FL
#endif
} secondLevel_t;

typedef struct {
    u64 guard;          // some known value as a guard
    u64 *glebeStart;    // inclusive
    u64 *glebeEnd;      // exclusive
    lock_t lock;           // used for init only, if FINE_LOCKING
    u32 init_count;
    // counters
    u32 count_used;     // count bytes allocated.
    u32 count_malloc;   // count successful malloc calls.
    u32 count_free;     // count successful deallocation calls
    // tlsf-specific parts
    u32 flCount;        // Number of first-level buckets.  This is invariant after constructor runs.
    u64 flAvailOrNot;   // bitmap that indicates the presence (1) or absence (0) of free blocks in blocks[i][*]
#ifdef FINE_LOCKING
    lock_t bmapLockFL;
#endif
    secondLevel_t sl[0];// second level structure in the annex area
} poolHdr_t;

// FLS: Find last set: position of the MSB set to 1
#define FLS fls64

COMPILE_ASSERT(ALIGNMENT == 8LL);
COMPILE_ASSERT((ALIGNMENT-1) == POOL_HEADER_TYPE_MASK);
COMPILE_ASSERT(SL_COUNT_LOG2 < 5);
COMPILE_ASSERT(FL_MAX_LOG2 <= 64);

COMPILE_ASSERT(ZERO_LIST_SIZE == SL_COUNT);

static u32 myffs(u64 val) {
    return FLS(val & (~val + 1LL));
}

static u64 quickInitAnnex(poolHdr_t * pPool, u64 size) {
    /* The memory will be layed out as follows:
     *  - at location: the poolHdr_t structure is used
     *  - the typedef of that structure only has the fixed-length data included.  The variable-length
     *    data (variable at init time, invariant thereafter) has to be "annexed" onto that.
     *  - then the glebe, i.e. net pool space, i.e. the first free block starts right after that (aligned)
     */

    // Figure out how much additional space needs to be annexed onto the end of the poolHdr_t struct
    // for the first-level bucket bit-masks and second-level block lists.
    u64 poolHeaderSize = sizeof(poolHdr_t);  // This size will increase as we add first-level buckets.
    u64 sizeRemainingAfterPoolHeader =
        size -             // From the gross pool size ...
        poolHeaderSize -   // ... subtract the size of the poolHdr_t ...
        ALLOC_OVERHEAD;   // ... and the size of the blkHdr_t that starts the single, huge whole-glebe block.
    u64 flBucketCount = 0;
    u64 poolSizeSpannedByFlBuckets = (1LL << (FL_COUNT_SHIFT-1)) * ALIGNMENT;
    while (poolSizeSpannedByFlBuckets < sizeRemainingAfterPoolHeader) {
        flBucketCount++;
        poolHeaderSize = sizeof(poolHdr_t) +                                     // Non-variable-sized parts of pool header (8byte-aligned)
                         sizeof(secondLevel_t) * flBucketCount;                  // space for secondLevel
        sizeRemainingAfterPoolHeader = size - poolHeaderSize - ALLOC_OVERHEAD;
        poolSizeSpannedByFlBuckets <<= 1;
    }
    pPool->flCount = flBucketCount;
    poolHeaderSize = (poolHeaderSize + ALIGNMENT_MASK)&(~ALIGNMENT_MASK);   // ceiling
    DPRINTF(DEBUG_LVL_VERB,"Allocating a pool at [0x%"PRIx64",0x%"PRIx64") of %"PRId64" (0x%"PRIx64") bytes. "
            "flCount %"PRId64", sizeof(poolHdr_t)=0x%zx (glebe: offset 0x%"PRIx64", and size is %"PRId64" (0x%"PRIx64"), i.e. net size after pool overhead)\n",
            (u64)pPool, (u64)pPool+size, size, size, flBucketCount, sizeof(poolHdr_t), (u64) poolHeaderSize, (u64)sizeRemainingAfterPoolHeader,
            (u64)sizeRemainingAfterPoolHeader);
    pPool->flAvailOrNot = 0; // Initialize the bitmaps to 0
#ifdef FINE_LOCKING
    pPool->bmapLockFL = INIT_LOCK;
#endif

    return poolHeaderSize;
}

static void quickFreeInternal(blkPayload_t *p);

#ifdef PER_AGENT_CACHE
static void quickCleanCache(void)
{
    s32 i, allreset=1;
    hal_lock(&CACHE_POOL(myid)->lock);
    for(i=0;i<MAX_SLABS;i++) {
        struct slab_header *head = CACHE_POOL(myid)->slabs[i];
        if (head == NULL)
            continue;
        if (head->bitmap == ((1UL << MAX_OBJ_PER_SLAB)-1UL) /* empty slab? */) {
            CACHE_POOL(myid)->slabs[i] = NULL;
            quickFreeInternal(head);
        } else {
            DPRINTF(DEBUG_LVL_VERB,"slab still in use because of memory leak?\n");
            allreset = 0;
        }
    }
    hal_unlock(&CACHE_POOL(myid)->lock);
    if (allreset) {
        void *p = CACHE_POOL(myid);
        CACHE_POOL(myid) = NULL;
        quickFreeInternal(p);
        DPRINTF(DEBUG_LVL_VERB, "cache %p destroyed\n", p);
    }

}

static void quickPrintCache(void)
{
    s32 i;
    s32 head_printed = 0;
    hal_lock(&CACHE_POOL(myid)->lock);
    for(i=0;i<MAX_SLABS;i++) {
        s32 m = CACHE_POOL(myid)->count_malloc[i];
        s32 f = CACHE_POOL(myid)->count_free[i];
        if (m != f) {
            if (!head_printed) {
                DPRINTF(DEBUG_LVL_INFO, "==== MEMORY LEAK REPORT (cache %p) ====\n", CACHE_POOL(myid));
                head_printed = 1;
            }
            DPRINTF(DEBUG_LVL_INFO, "(%"PRId32"~%"PRId32"] : malloc %"PRId32" free %"PRId32"\n", SLAB_MAX_SIZE(i-1), SLAB_MAX_SIZE(i), m, f);
        }
    }
    hal_unlock(&CACHE_POOL(myid)->lock);
    if (head_printed)
        DPRINTF(DEBUG_LVL_INFO, "====== END OF REPORT (cache %p) =======\n", CACHE_POOL(myid));
}
#endif

// this detects empty slabs and free them. Such empty slabs should have been
// cleaned-up by quickCleanCache(). If it hasn't for some reason, this function
// will clean up them.
static void quickCleanPool(poolHdr_t *pool)
{
#ifdef PER_AGENT_CACHE
    u64 end   = (u64)pool->glebeEnd;
    u64 *p = pool->glebeStart;
    u64 size, flag;
    u64 *prev = p;
    u64 count_slab_inuse = 0;
    for(;;) {
        size = GET_SIZE(HEAD(p));
        flag = GET_FLAG(HEAD(p));
        if (flag != FLAG_FREE) {
            if (flag == FLAG_INUSE) {
            } else if (flag == FLAG_INUSE_SLAB) {
                struct slab_header *head = (struct slab_header *)HEAD_TO_USER(p);
                ASSERT(head->mark == SLAB_MARK);
                if (head->bitmap == ((1UL << MAX_OBJ_PER_SLAB)-1UL) /* empty slab? */) {
                    // it must be only (and first) slab in the slab list
                    s32 slabsIndex = SIZE_TO_SLABS(head->size);
                    hal_lock(&head->per_agent->lock);
                    struct slab_header *slabs = head->per_agent->slabs[slabsIndex];
                    if (slabs == head) {
                        head->per_agent->slabs[slabsIndex] = NULL;
                    }
                    hal_unlock(&head->per_agent->lock);
                    if (slabs != head) {
                        DPRINTF(DEBUG_LVL_WARN, "cleanup pool -- empty slab found but it was not the first slab in this slab list?\n");
                        continue;
                    }

                    quickFreeInternal(head);
                    p = prev;
                    continue;
                } else {
                    count_slab_inuse++;
                }
            } else {
            }
        }
        prev = p;
        p = &PEER_RIGHT(p, size);
        if ( (u64)p >= end )
            break;
    }
    DPRINTF(DEBUG_LVL_INFO, "quickCleanPool: leak? found %"PRId64" slabs in use.\n", count_slab_inuse);
#endif
}

static void quickWalkPool(poolHdr_t *pool, int opt)
{
    u64 end   = (u64)pool->glebeEnd;
    u64 *p = pool->glebeStart;
    u64 size, flag, total = 0;
    for(;;) {
        size = GET_SIZE(HEAD(p));
        flag = GET_FLAG(HEAD(p));
        if (flag != FLAG_FREE) {
            if (flag == FLAG_INUSE) {
                DPRINTF(DEBUG_LVL_INFO, "[size %"PRId64" user %"PRIx64"] %p\n", size, GET_USER(HEAD(p))>>SHIFT_USER , p);
            } else if (flag == FLAG_INUSE_SLAB) {
#ifdef PER_AGENT_CACHE
                struct slab_header *head = (struct slab_header *)HEAD_TO_USER(p);
                ASSERT(head->mark == SLAB_MARK);
                DPRINTF(DEBUG_LVL_INFO, "[size %"PRId64" user %"PRIx64"] slab for %p\n", size, GET_USER(HEAD(p))>>SHIFT_USER, head->per_agent);
#else
                ASSERT(0 && "FLAG_INUSE_SLAB without slab enabled?\n");
#endif
            } else {
                DPRINTF(DEBUG_LVL_INFO, "{size %"PRId64" user %"PRIx64"}\n", size, GET_USER(HEAD(p))>>SHIFT_USER);
            }
            total += size;
        }
        p = &PEER_RIGHT(p, size);
        if ( (u64)p >= end )
            break;
    }
    DPRINTF(DEBUG_LVL_INFO, "%"PRId64" bytes still in use\n", total);
}

static void quickPrintCounters(poolHdr_t *pool)
{
    if (pool->count_used) {
        DPRINTF(DEBUG_LVL_INFO, "**** MEMORY LEAK REPORT (pool %p) ****\n", pool);
        DPRINTF(DEBUG_LVL_INFO, "%"PRId32" bytes still in use, malloc %"PRId32" times, free %"PRId32" times\n", pool->count_used, pool->count_malloc, pool->count_free);
        quickWalkPool(pool, 1);
        DPRINTF(DEBUG_LVL_INFO, "****** END OF REPORT (pool %p) *******\n", pool);
    }
}

/* Two-level function to determine indices. This is pretty much
 * taken straight from the specs
 */
static void mappingInsert(u64 payloadSizeInBytes, u32* flIndex, u32* slIndex) {
    u32 tf, ts;
    u64 sizeInElements = payloadSizeInBytes / ALIGNMENT;

    if(sizeInElements < ZERO_LIST_SIZE) {
        tf = 0;
        ts = sizeInElements;
    } else {
        tf = FLS(sizeInElements);
        ts = (sizeInElements >> (tf - SL_COUNT_LOG2)) - (SL_COUNT);
        tf -= (FL_COUNT_SHIFT - 1LL);
    }
    *flIndex = tf;
    *slIndex = ts;
}

// counters for diagnosis
u32 count_malloc_retry1;
u32 count_malloc_retry2;
u32 count_left_retry;
u32 count_merge_retry;
u32 count_fl_retry;

/* Search for a suitable free block:
 *  - first search for a block in the fl/sl indicated.
 *  - if not found, search for higher sl (bigger) with same fl
 *  - if not found, search for higher fl and sl in that
 *
 *  Returns the header of a free block as well as flIndex and slIndex
 *  that block was taken from.
 */

static u64 *getFreeListMalloc(poolHdr_t *pPool, u64 size, u32 *fli, u32 *sli)
{
    u32 flIndex, slIndex;
    size -= ALLOC_OVERHEAD;     // convert to payload size
    mappingInsert(size, &flIndex, &slIndex);
    if ( (flIndex == 0 && ((size & ( (1UL                *ALIGNMENT)-1)) == 0)) ||
         (flIndex != 0 && ((size & (((1UL << (flIndex-1))*ALIGNMENT)-1)) == 0)) ) {
        // nothing. No need to round-up to the next block size.
    } else {
        /* For allocations, we want to round-up to the next block size
         * so that we are sure that any block will work so we can
         * pick it in constant time.
         */
        slIndex++;
        if (slIndex >= SL_COUNT) {
            slIndex = 0;
            flIndex++;
        }
    }

    if (flIndex >= pPool->flCount) {
        return NULL;
    }

    // read without locking
    u32 slBitmap = pPool->sl[flIndex].slAvailOrNot & (~0UL << slIndex); // This takes all SL bins bigger or equal to slIndex
    u64 flBitmap;
    if (slBitmap == 0) {
        if (flIndex+1 >= pPool->flCount) {
            return NULL;
        }

retry_FL:
        // read without locking
        // We don't have any non-zero block here so we look at the flAvailOrNot map
        flBitmap = pPool->flAvailOrNot & (~0UL << (flIndex+1)); // all FL bins bigger or equal to flIndex
        if (flBitmap == 0) {
            return NULL;
        }

        // Look for the first bit that is a one
        flIndex = myffs(flBitmap);
        ASSERT(flIndex < pPool->flCount);

        // Now we get the slBitMap. Retry if no 1's there.
        slBitmap = pPool->sl[flIndex].slAvailOrNot;
        if (slBitmap == 0) {
            hal_xadd32(&count_fl_retry, 1);
            goto retry_FL;
        }
    }
    slIndex = myffs(slBitmap);
    ASSERT(slIndex < SL_COUNT);

    *fli = flIndex;
    *sli = slIndex;
    return (void *)1;
}

static void setFreeList(poolHdr_t *pPool, u64 size, u64 *p, u32 flIndex, u32 slIndex)
{
    u32 v;
    if (p == NULL) {
        v = -1;
    } else {
        ASSERT((u64)p >= (u64)pPool->glebeStart);
        ASSERT((u64)p < (u64)pPool->glebeEnd);
        v = p-(pPool->glebeStart);
    }
#ifndef FINE_LOCKING
    u32 old = pPool->sl[flIndex].freeList[slIndex];
#endif
    pPool->sl[flIndex].freeList[slIndex] = v;
#ifndef FINE_LOCKING
    // adjust bitmap
    u32 oldBitmap = pPool->sl[flIndex].slAvailOrNot;
    ASSERT(slIndex < sizeof(pPool->sl[flIndex].slAvailOrNot)*8);
    ASSERT(flIndex < sizeof(pPool->flAvailOrNot)*8);
    if (old == -1 && v != -1) {
        ASSERT(!(oldBitmap & (1UL << slIndex)));
        pPool->sl[flIndex].slAvailOrNot |= (1UL << slIndex);
        if (!oldBitmap) {
            ASSERT(!(pPool->flAvailOrNot & (1UL << flIndex)));
            pPool->flAvailOrNot |= (1UL << flIndex);
        }
    }
    if (old != -1 && v == -1) {
        ASSERT(oldBitmap & (1UL << slIndex));
        pPool->sl[flIndex].slAvailOrNot &= ~(1UL << slIndex);
        if (!(pPool->sl[flIndex].slAvailOrNot)) {
            ASSERT(pPool->flAvailOrNot & (1UL << flIndex));
            pPool->flAvailOrNot &= ~(1UL << flIndex);
        }
    }
#endif
}
#ifdef FINE_LOCKING
int dobmap_count_case2;
int dobmap_count_case3;
int dobmap_arr[4];

// calling function : count : delta
// quickInit        : 1     : +1      init
// MallocInternal   : 1     : -1      no split
// MallocInternal   : 2     : -1+1    split
// FreeInternal     : 1     : +1      no merge
// FreeInternal     : 2     : -1+1    one merge
// FreeInternal     : 3     : -1-1+1  two merges
//
void doBmapOp(poolHdr_t *pool, struct bmapOp *bmap_op)
{
    int i;
    s32 u,v;
    u32 flIndex, slIndex;
//dobmap_arr[bmap_op->count]++;

    switch(bmap_op->count) {
    case 2:
        // optimization for common cases.
        if ( bmap_op->fli[0] == bmap_op->fli[1] && bmap_op->sli[0] == bmap_op->sli[1] ) {
            ASSERT(bmap_op->delta[0] == -1 && bmap_op->delta[1] == 1);
//dobmap_count_case2++;
            return;
        }
        goto bmap_fallback;
    case 3:
        // optimization for count==3 case
        // e.g.[ 13,15, -1 ], [ 0,8, -1 ], [13,15, +1]
        if ( bmap_op->fli[0] == bmap_op->fli[2] && bmap_op->sli[0] == bmap_op->sli[2] ) {
            ASSERT(bmap_op->delta[0] == -1 && bmap_op->delta[2] == 1);
            bmap_op->count = 1;
            bmap_op->fli[0] = bmap_op->fli[1];
            bmap_op->sli[0] = bmap_op->sli[1];
            // continue to count==1 case
//dobmap_count_case3++;
        } else {
            goto bmap_fallback;
        }
/*
    case 1:
        // we can do a special case for count==1 here
*/
    }

bmap_fallback:
/*  // some debugging code
    printf("dobmapOp count %"PRId32"\n", bmap_op->count);
    for(i=0;i<bmap_op->count;i++) {
        printf("%"PRId32",%"PRId32", %"PRId32"\n", bmap_op->fli[i], bmap_op->sli[i], bmap_op->delta[i]);
    }
*/
    // general case for count==2,3 in reverse order
    // to process +1 first before -1s
    // ( move lock outside loop and add locks to getFreeListMalloc )
    for(i=bmap_op->count-1;i>=0;i--) {
        flIndex = bmap_op->fli[i];
        slIndex = bmap_op->sli[i];
        hal_lock(&(pool->sl[flIndex].bmapLockSL));
        u = pool->sl[flIndex].listCount[slIndex];
        v = (pool->sl[flIndex].listCount[slIndex] += bmap_op->delta[i]);
        ASSERT(bmap_op->delta[i] == 1 || bmap_op->delta[i] == -1);

        if (u && v) { // common cases. i.e. no bitmap change
            hal_unlock(&(pool->sl[flIndex].bmapLockSL));
            continue;
        }

        // adjust bitmap
        u32 oldBitmap = pool->sl[flIndex].slAvailOrNot;
        ASSERT(slIndex < sizeof(pool->sl[flIndex].slAvailOrNot)*8);
        ASSERT(flIndex < sizeof(pool->flAvailOrNot)*8);
        if (u == 0 && v == 1) {  // 0 -> 1
            ASSERT(!(oldBitmap & (1UL << slIndex)));
            pool->sl[flIndex].slAvailOrNot |= (1UL << slIndex);
            hal_unlock(&(pool->sl[flIndex].bmapLockSL));
            if (!oldBitmap) {
                hal_lock(&(pool->bmapLockFL));
                pool->sl[flIndex].count++;
                if (pool->sl[flIndex].count == 1) {
                    ASSERT(!(pool->flAvailOrNot & (1UL << flIndex)));
                    pool->flAvailOrNot |= (1UL << flIndex);
                }
                hal_unlock(&(pool->bmapLockFL));
            }
            continue;
        }
        if (u == 1 && v == 0) {  // 1 -> 0
            ASSERT(oldBitmap & (1UL << slIndex));
            u32 newBitmap = (pool->sl[flIndex].slAvailOrNot &= ~(1UL << slIndex));
            hal_unlock(&(pool->sl[flIndex].bmapLockSL));
            if (!newBitmap) {
                hal_lock(&(pool->bmapLockFL));
                pool->sl[flIndex].count--;
                if (pool->sl[flIndex].count == 0) {
                    ASSERT(pool->flAvailOrNot & (1UL << flIndex));
                    pool->flAvailOrNot &= ~(1UL << flIndex);
                }
                hal_unlock(&(pool->bmapLockFL));
            }
            continue;
        }
        hal_unlock(&(pool->sl[flIndex].bmapLockSL));
    }
//quickPrint(pool);
}
#endif
// end of tlsf core part



// start of simple alloc core part
#if 0 // TODO: 4.1.0 - this test severely slows down FSim without any obvious benefit
        // See bug #875
static void quickTest(u64 start, u64 size)
{
    // boundary check code for sanity check.
    // This helps early detection of malformed addresses.
    do {
        DPRINTF(DEBUG_LVL_VERB, "quickTest : pool range [0x%"PRIx64" - 0x%"PRIx64")\n", start, start+size);

        u8 *p = (u8 *)((start + size - 128)&(~0x7UL));      // at least 128 bytes
        u8 *q = (u8 *)(start + size);
        u64 size = q-p;

        while (size >= 8) {
            *((u64 *)p) = 0xdeadbeef0000dead;   // just random value
            p+=8; size -= 8;
        }
        while (size) {
            *p = '0';
            p++; size--;
        }
    } while(0);
    DPRINTF(DEBUG_LVL_VERB, "quickTest : simple boundary test passed\n");
}
#endif

static void quickFinish(poolHdr_t *pool, u64 size)
{
    ASSERT((sizeof(poolHdr_t) & ALIGNMENT_MASK) == 0);
    ASSERT((size & ALIGNMENT_MASK) == 0);

    DPRINTF(DEBUG_LVL_VERB, "quickFinish called. size 0x%"PRIx64" at %p\n", size, (u8 *)pool);


#ifdef PER_AGENT_CACHE
    if (CACHE_POOL(myid) != NULL) {
        quickPrintCache();
        quickCleanCache();
    }
#endif

/*
DPRINTF(DEBUG_LVL_WARN, "bmap_arr  : %"PRId32", %"PRId32"(was %"PRId32"), %"PRId32"\n", dobmap_arr[1], dobmap_arr[2]-dobmap_count_case2, dobmap_arr[2], dobmap_arr[3]);
DPRINTF(DEBUG_LVL_WARN, "bmap_count: %"PRId32"\n", dobmap_count_case3);
*/
    hal_lock(&(pool->lock));
    pool->init_count--;
    if (!(pool->init_count)) {
        hal_unlock(&(pool->lock));
        // no more user for this pool -- no need to lock
        quickCleanPool(pool);
        quickPrintCounters(pool);
    } else {
        hal_unlock(&(pool->lock));
        DPRINTF(DEBUG_LVL_INFO, "shutdown skip for pool %p\n", pool);
    }
}

static blkPayload_t *quickMallocInternal(poolHdr_t *pool,u64 size, struct _ocrPolicyDomain_t *pd);

static void quickInit(poolHdr_t *pool, u64 size)
{
    u8 *p = (u8 *)pool;
    ASSERT((sizeof(poolHdr_t) & ALIGNMENT_MASK) == 0);
    ASSERT((size & ALIGNMENT_MASK) == 0);

#ifdef PER_AGENT_CACHE
    ASSERT((sizeof(struct slab_header) & ALIGNMENT_MASK) == 0);
#endif


    // pool->lock and pool->init_count is already 0 at startup (on x86, it's done at mallocBegin())
    hal_lock(&(pool->lock));
    if (!(pool->init_count)) {
#if 0 // TODO: 4.1.0 - this test severely slows down FSim without any obvious benefit
        // See bug #875
        quickTest((u64)pool, size);
#endif
        // reserve 8 bytes for a guard
        size -= sizeof(u64);

        // init annex area
        u64 offsetToGlebe = quickInitAnnex(pool, size);
        u64 *q = (u64 *)(p + offsetToGlebe);
        ASSERT(((u64)q & ALIGNMENT_MASK) == 0);
#ifdef ALIGN_CACHE_LINE
        q = (u64 *)(((u64)q + CACHE_LINE_MASK)&(~CACHE_LINE_MASK));   // ceiling
#endif
        //marks the glebe as a single free block
        size = size - offsetToGlebe;
        if (size >= MAX_BLOCK_SIZE) {
            DPRINTF(DEBUG_LVL_WARN,"Too big pool size! MAX is 0x%lx\n", MAX_BLOCK_SIZE);
            ASSERT(0);
        }
        HEAD(q) = MARK | size | FLAG_FREE;

#ifdef FINE_LOCKING
        HEAD_LOCK(q) = INIT_LOCK;
        TAIL_LOCK(q,size) = INIT_LOCK;
#endif
        NEXT(q) = 0;
        PREV(q) = 0;
//        TAIL_SIZE(q,size) = size;
        TAIL(q,size) = MARK | size;
        pool->glebeStart = (u64 *)q;
        pool->glebeEnd = (u64 *)(p+size+offsetToGlebe);
        DPRINTF(DEBUG_LVL_VERB, "end of annex:%p , glebeStart:%p\n", &pool->sl[pool->flCount], pool->glebeStart);
        ASSERT( (u64)(&pool->sl[pool->flCount]) <= (u64)(pool->glebeStart));

        // place a guard value at both ends
        pool->guard = KNOWN_VALUE_AS_GUARD;
        *pool->glebeEnd = KNOWN_VALUE_AS_GUARD;

        pool->count_used = 0;
        pool->count_malloc = 0;
        pool->count_free = 0;

        u32 i,j;
        for(i=0;i<pool->flCount;i++) {
            for(j=0;j<SL_COUNT;j++) {
                pool->sl[i].slAvailOrNot = 0;    // init bitmap
                pool->sl[i].freeList[j] = -1;    // empty list
#ifdef FINE_LOCKING
                pool->sl[i].listCount[j] = 0;
                pool->sl[i].bmapLockSL = INIT_LOCK;
                pool->sl[i].count = 0;
#endif
            }
        }

        // Add the glebe, i.e. the big free block
        u32 flIndex, slIndex;
        mappingInsert(size - ALLOC_OVERHEAD, &flIndex, &slIndex);
        setFreeList(pool, size, q, flIndex, slIndex);

#ifdef FINE_LOCKING
        struct bmapOp bmap_op;
        bmap_op.count = 1;
        bmap_op.fli[0] = flIndex;
        bmap_op.sli[0] = slIndex;
        bmap_op.delta[0] = 1;

        doBmapOp(pool, &bmap_op);
#endif
        DPRINTF(DEBUG_LVL_INFO, "init'ed pool %p, avail %"PRId64" bytes , sizeof(poolHdr_t) = %zd\n", pool, size, sizeof(poolHdr_t));
        pool->init_count++;
#ifdef ENABLE_VALGRIND
        VALGRIND_CREATE_MEMPOOL(p, 0, 1);  // BUG #600: Mempool needs to be destroyed
        VALGRIND_MAKE_MEM_NOACCESS(p, size+sizeof(poolHdr_t));
#endif
    } else {
        DPRINTF(DEBUG_LVL_INFO, "init skip for pool %p\n", pool);
    }
#ifdef ENABLE_VALGRIND
    VALGRIND_MAKE_MEM_DEFINED(&(pool->lock), sizeof(pool->lock));
    hal_unlock(&(pool->lock));
    VALGRIND_MAKE_MEM_NOACCESS(&(pool->lock), sizeof(pool->lock));
#else
    hal_unlock(&(pool->lock));
#endif
    // TODO probably we need a barrier here to make sure init_count to reach its peak
}

#ifdef PER_AGENT_CACHE
static void *quickInitCache(poolHdr_t *pool)
{
    struct per_agent_cache *q = quickMallocInternal(pool, sizeof(struct per_agent_cache), NULL /* TODO PD for TG ??*/);
    ASSERT(q);
    int i;
    for(i=0;i<MAX_SLABS;i++) {
        q->slabs[i] = NULL;
        q->count_malloc[i] = q->count_free[i] = 0;
        q->lock = INIT_LOCK;
    }
    return q;
}
#endif

static void quickInsertFree(poolHdr_t *pool,u64 *p, u64 size, u32 flIndex, u32 slIndex)
{
    VALGRIND_POOL_OPEN(pool);

#ifdef ALIGN_CACHE_LINE
    ASSERT(!((u64)p & CACHE_LINE_MASK));
#endif
#ifdef FINE_LOCKING
    hal_lock(&pool->sl[flIndex].listLock[slIndex]);
#endif
    DPRINTF(DEBUG_LVL_VVERB, "REC: InsertFree MARK1: pool: %p, p: %p [0x%"PRIu64"], size: %"PRIu64" flIndex: %"PRIu32", slIndex: %"PRIu32"\n",
            pool, p, *p, size, flIndex, slIndex);
    if (pool->sl[flIndex].freeList[slIndex] != -1) {
        DPRINTF(DEBUG_LVL_VVERB, "REC: In If\n");
        u64 *q = pool->glebeStart + pool->sl[flIndex].freeList[slIndex];

        VALGRIND_CHUNK_OPEN(q);
        DPRINTF(DEBUG_LVL_VVERB, "REC: 0\n");
        u64 *r = PREV(q)+(pool->glebeStart);
        DPRINTF(DEBUG_LVL_VVERB, "REC: 1\n");
        VALGRIND_CHUNK_OPEN_COND(q, r);
        DPRINTF(DEBUG_LVL_VVERB, "REC: 2\n");
        NEXT(r) = p-(pool->glebeStart);
        DPRINTF(DEBUG_LVL_VVERB, "REC: InsertFree MARK1.1\n");
        VALGRIND_CHUNK_CLOSE_COND(q, r);
        NEXT(p) = q-(pool->glebeStart);
        PREV(p) = PREV(q);
        DPRINTF(DEBUG_LVL_VVERB, "REC: InsertFree MARK1.2\n");
        PREV(q) = p-(pool->glebeStart);
        VALGRIND_CHUNK_CLOSE(q);
        DPRINTF(DEBUG_LVL_VVERB, "REC: InsertFree MARK1.3\n");
    } else {
        DPRINTF(DEBUG_LVL_VVERB, "REC: Iin else\n");
        NEXT(p) = p-(pool->glebeStart);
        PREV(p) = p-(pool->glebeStart);
        setFreeList(pool, size, p, flIndex, slIndex);
    }
    DPRINTF(DEBUG_LVL_VVERB, "REC: InsertFree MARK2\n");
#ifdef FINE_LOCKING
    hal_unlock(&pool->sl[flIndex].listLock[slIndex]);
#endif
    //quickPrint(pool);
    VALGRIND_POOL_CLOSE(pool);
}

static void quickDeleteFree1(poolHdr_t *pool,u64 *p, u32 flIndex, u32 slIndex)
{
    VALGRIND_POOL_OPEN(pool);
    VALGRIND_CHUNK_OPEN(p);
    ASSERT(GET_FLAG(HEAD(p)) == FLAG_FREE);   // cleared in-use bit in list.
    ASSERT(PREV(p) != -1 && NEXT(p) != -1);
    u64 size = GET_SIZE(HEAD(p));
    u64 *list;
//    u32 flIndex, slIndex;

    if (pool->sl[flIndex].freeList[slIndex] != -1)
        list = pool->glebeStart + pool->sl[flIndex].freeList[slIndex];
    else {
        DPRINTF(DEBUG_LVL_WARN, "quickMalloc : not-in-list free block in Free1!?!\n");
        ASSERT(0);
        list = NULL;
    }

    u64 *next = NEXT(p) + pool->glebeStart;
    u64 *prev = PREV(p) + pool->glebeStart;

    if (next == p) {
        setFreeList(pool, size, NULL, flIndex, slIndex);
        PREV(p) = NEXT(p) = -1;
        VALGRIND_CHUNK_CLOSE(p);
        VALGRIND_POOL_CLOSE(pool);
        return;
    }
    VALGRIND_CHUNK_OPEN(next);
    VALGRIND_CHUNK_OPEN_COND(next, prev);

    NEXT(prev) = NEXT(p);
    PREV(next) = PREV(p);
    if (p == list) {
        setFreeList(pool, size, next, flIndex, slIndex);
    }
    PREV(p) = NEXT(p) = -1;
    VALGRIND_CHUNK_CLOSE(p);
    VALGRIND_CHUNK_CLOSE(next);
    VALGRIND_CHUNK_CLOSE_COND(next, prev);
    VALGRIND_POOL_CLOSE(pool);
}



static void quickDeleteFree2(poolHdr_t *pool,u64 *p, struct bmapOp *bmap_op)
{
    VALGRIND_POOL_OPEN(pool);
    VALGRIND_CHUNK_OPEN(p);

    u64 size = GET_SIZE(HEAD(p));
    u64 *list;
    u32 flIndex, slIndex;

    mappingInsert(size - ALLOC_OVERHEAD, &flIndex, &slIndex);
#ifdef FINE_LOCKING
    u32 i = bmap_op->count++;
    bmap_op->fli[i] = flIndex;
    bmap_op->sli[i] = slIndex;
    bmap_op->delta[i] = -1;
    hal_lock(&pool->sl[flIndex].listLock[slIndex]);
#endif
    ASSERT(GET_FLAG(HEAD(p)) == FLAG_MERGE);
    ASSERT(PREV(p) != -1 && NEXT(p) != -1);
    if (pool->sl[flIndex].freeList[slIndex] != -1)
        list = pool->glebeStart + pool->sl[flIndex].freeList[slIndex];
    else {
        DPRINTF(DEBUG_LVL_WARN, "quickMalloc : not-in-list free block!?!\n");
        ASSERT(0);
        list = NULL;
    }

    u64 *next = NEXT(p) + pool->glebeStart;
    u64 *prev = PREV(p) + pool->glebeStart;

    if (next == p) {
        setFreeList(pool, size, NULL, flIndex, slIndex);
#ifdef FINE_LOCKING
        PREV(p) = NEXT(p) = -1;
        hal_unlock(&pool->sl[flIndex].listLock[slIndex]);
#endif
        VALGRIND_CHUNK_CLOSE(p);
        VALGRIND_POOL_CLOSE(pool);
        return;
    }
    VALGRIND_CHUNK_OPEN(next);
    VALGRIND_CHUNK_OPEN_COND(next, prev);

    NEXT(prev) = NEXT(p);
    PREV(next) = PREV(p);
    if (p == list) {
        setFreeList(pool, size, next, flIndex, slIndex);
    }
#ifdef FINE_LOCKING
    PREV(p) = NEXT(p) = -1;
    hal_unlock(&pool->sl[flIndex].listLock[slIndex]);
#endif
    VALGRIND_CHUNK_CLOSE(p);
    VALGRIND_CHUNK_CLOSE(next);
    VALGRIND_CHUNK_CLOSE_COND(next, prev);
    VALGRIND_POOL_CLOSE(pool);
}

// check if the known values are there intact.
static inline void checkGuard(poolHdr_t *pool)
{
    ASSERT_BLOCK_BEGIN( *pool->glebeEnd == KNOWN_VALUE_AS_GUARD )  // always true
    DPRINTF(DEBUG_LVL_WARN, "quickMalloc : heap corruption! known value not found at the end of the pool. (might be stack overflow if it's L1SPAD)\n");
    ASSERT_BLOCK_END
    ASSERT_BLOCK_BEGIN( pool->guard == KNOWN_VALUE_AS_GUARD )  // always true
    DPRINTF(DEBUG_LVL_WARN, "quickMalloc : heap corruption! known value not found at the beginning of the pool.\n");
    ASSERT_BLOCK_END
}

static blkPayload_t *quickMallocInternal(poolHdr_t *pool,u64 size, struct _ocrPolicyDomain_t *pd)
{
    u64 size_orig = size;
//    DPRINTF(DEBUG_LVL_VERB, "before malloc size %"PRId64":\n", size_orig);

    // This guarantees that the block will be able to embed NEXT/PREV
    // in case that it's freed in the future.
    size = (size_orig + ALIGNMENT_MASK)&(~ALIGNMENT_MASK);   // ceiling
    size += ALLOC_OVERHEAD;     // internal size
#ifdef ALIGN_CACHE_LINE
    size = (size + CACHE_LINE_MASK)&(~CACHE_LINE_MASK);   // ceiling
#else
    if (size < MINIMUM_SIZE)    // should be bigger than minimum size
        size = MINIMUM_SIZE;
#endif

    VALGRIND_POOL_OPEN(pool);
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK1\n");
#ifndef FINE_LOCKING
    hal_lock(&(pool->lock));
#endif
    checkGuard(pool);

    u32 fli, sli;
    u64 *p;
#ifdef FINE_LOCKING
retry:
#endif
    p = getFreeListMalloc(pool, size, &fli, &sli);
    VALGRIND_POOL_CLOSE(pool);

    //quickPrint(pool);
    if (p == NULL) {
        //DPRINTF(DEBUG_LVL_INFO, "OUT OF HEAP! malloc failed\n");
        VALGRIND_POOL_OPEN(pool);
#ifndef FINE_LOCKING
        hal_unlock(&(pool->lock));
#endif
        VALGRIND_POOL_CLOSE(pool);
        return NULL;
    }
    // OK, we've found a free list

#ifdef FINE_LOCKING
    if (pool->sl[fli].freeList[sli] == -1) {
        hal_xadd32(&count_malloc_retry1, 1);
        goto retry;
    }
    hal_lock(&pool->sl[fli].listLock[sli]);
    if (pool->sl[fli].freeList[sli] == -1) {
//        printf("wrong bitmap.. malloc try again...\n");
        hal_unlock(&pool->sl[fli].listLock[sli]);
        hal_xadd32(&count_malloc_retry2, 1);
        goto retry;
    }
#else
    ASSERT(pool->sl[fli].freeList[sli] != -1);
#endif
    p = pool->glebeStart + pool->sl[fli].freeList[sli];
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK3\n");

    VALGRIND_CHUNK_OPEN(p);
    ASSERT_BLOCK_BEGIN( GET_SIZE(HEAD(p)) >= size )  // always true on tlsf
    DPRINTF(DEBUG_LVL_WARN, "quickMalloc : BUG! this free list has too small block.\n");
    ASSERT_BLOCK_END

    VALGRIND_CHUNK_CLOSE(p);
#ifdef FINE_LOCKING
    {
    u32 ret;
    do {
        ret = hal_trylock(&HEAD_LOCK(p));
        if (ret) {
            if (GET_FLAG(HEAD(p)) == FLAG_MERGE) {
                //printf("found FLAG_MERGE.. retry\n");
                hal_xadd32(&count_merge_retry, 1);
                hal_unlock(&pool->sl[fli].listLock[sli]);
                goto retry;
            }
        }
    } while(ret);
    }
    struct bmapOp bmap_op;
    bmap_op.count = 1;
    bmap_op.fli[0] = fli;
    bmap_op.sli[0] = sli;
    bmap_op.delta[0] = -1;
#endif
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK4\n");
    quickDeleteFree1(pool, p, fli, sli);
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK4.1\n");
#ifdef FINE_LOCKING
    hal_unlock(&pool->sl[fli].listLock[sli]);
#endif

    VALGRIND_CHUNK_OPEN_INIT(p, size);
    u64 remain = GET_SIZE(HEAD(p)) - size;
    ASSERT( remain < GET_SIZE(HEAD(p)) );
    ASSERT((size & ALIGNMENT_MASK) == 0);
    // make sure the remaining block is bigger than minimum size
    if (remain >= MINIMUM_SIZE) {  // we need split
#ifdef FINE_LOCKING
        hal_lock(&TAIL_LOCK(p, GET_SIZE(HEAD(p))));
#endif
        // we're already holding lock
        HEAD(p) = MARK | size | FLAG_INUSE;    // in-use mark
        TAIL(p, size) = MARK | size;
#ifdef FINE_LOCKING
        TAIL_LOCK(p, size) = INIT_LOCK;
#endif
        VALGRIND_CHUNK_CLOSE(p);

        u64 *right = &PEER_RIGHT(p, size);
        u32 flIndex, slIndex;
        DPRINTF(DEBUG_LVL_VVERB, "REC: MARK4.2\n");
        mappingInsert(remain - ALLOC_OVERHEAD, &flIndex, &slIndex);

        VALGRIND_CHUNK_OPEN_INIT(right, remain);
        ASSERT((remain & ALIGNMENT_MASK) == 0);
        HEAD(right) = MARK | remain | FLAG_FREE;
        PREV(right) = NEXT(right) = -1;
#ifdef FINE_LOCKING
        HEAD_LOCK(right) = INIT_LOCK;
        hal_lock(&HEAD_LOCK(right));
#endif
        TAIL(right, remain) = MARK | remain;
#ifdef FINE_LOCKING
        hal_unlock(&TAIL_LOCK(right, remain));
#endif
        // do I need barrier?
        DPRINTF(DEBUG_LVL_VVERB, "REC: MARK 4.2.1\n");
        quickInsertFree(pool, right, remain, flIndex, slIndex);
        DPRINTF(DEBUG_LVL_VVERB, "REC: MARK4.3\n");
        VALGRIND_CHUNK_CLOSE(right);
#ifdef FINE_LOCKING
        hal_unlock(&HEAD_LOCK(right));
        bmap_op.count = 2;
        bmap_op.fli[1] = flIndex;
        bmap_op.sli[1] = slIndex;
        bmap_op.delta[1] = 1;
#endif
    } else {
        HEAD(p) |= FLAG_INUSE;         // in-use mark
        VALGRIND_CHUNK_CLOSE(p);
    }
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK5\n");
#ifdef FINE_LOCKING
    hal_unlock(&HEAD_LOCK(p));
    doBmapOp(pool, &bmap_op);
#endif

    void *ret = HEAD_TO_USER(p);
    VALGRIND_CHUNK_OPEN(p);
    INFO1(p) = (u64)addrGlobalizeOnTG((void *)pool, pd);   // old : INFO1(p) = (u64)pool;
    INFO2(p) = (u64)addrGlobalizeOnTG((void *)ret, pd);    // old : INFO2(p) = (u64)ret;

    ASSERT((*(u8 *)(&INFO2(p)) & POOL_HEADER_TYPE_MASK) == 0);
    *(u8 *)(&INFO2(p)) |= allocatorQuick_id;

    ASSERT_BLOCK_BEGIN((*(u8 *)(&INFO2(p)) & POOL_HEADER_TYPE_MASK) == allocatorQuick_id)
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : id != allocatorQuick_id \n");
    ASSERT_BLOCK_END

    pool->count_used += size;       // count bytes using internal block size to match to free()
    pool->count_malloc++;           // count successful malloc calls.

//    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : malloc : called\n");
//    quickPrintCounters(pool);
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK6\n");
    VALGRIND_CHUNK_CLOSE(p);
#ifndef FINE_LOCKING
    VALGRIND_POOL_OPEN(pool);
    hal_unlock(&(pool->lock));
    VALGRIND_POOL_CLOSE(pool);
#endif
#ifdef ENABLE_VALGRIND
    VALGRIND_MEMPOOL_ALLOC(pool, ret, size_orig);
    VALGRIND_MAKE_MEM_DEFINED(ret, size_orig);
//    DPRINTF(DEBUG_LVL_WARN, "mempool_alloc, pool %p, ret %p, size %8ld\n", pool, ret, size_orig);
#endif
    DPRINTF(DEBUG_LVL_VVERB, "REC: MARK7\n");
    return ret;
}

static void quickFreeInternal(blkPayload_t *p)
{
    if (p == NULL)
        return;
    u64 *q = USER_TO_HEAD(p);
    VALGRIND_CHUNK_OPEN(q);
    poolHdr_t *pool = (poolHdr_t *)INFO1(q);

    ASSERT_BLOCK_BEGIN ( GET_MARK(q) == MARK )
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : free: cannot find mark. Probably wrong address is passed to free()? %p\n", p);
    VALGRIND_CHUNK_CLOSE(q);
#ifdef ENABLE_VALGRIND
    VALGRIND_MEMPOOL_FREE(pool, p);
#endif
    ASSERT_BLOCK_END

    VALGRIND_POOL_OPEN(pool);
    u64 start = (u64)pool->glebeStart;
    u64 end   = (u64)pool->glebeEnd;
    struct bmapOp bmap_op;
#ifndef FINE_LOCKING
    hal_lock(&(pool->lock));
#else
    bmap_op.count = 0;
#endif
    checkGuard(pool);
    VALGRIND_POOL_CLOSE(pool);

    ASSERT((*(u8 *)(&INFO2(q)) & POOL_HEADER_TYPE_MASK) == allocatorQuick_id);
    *(u8 *)(&INFO2(q)) &= ~POOL_HEADER_TYPE_MASK;

    // Make sure we have the global address to free, even if user passed local address...
    q = USER_TO_HEAD(INFO2(q)); // For TG. no effects on x86

    ASSERT_BLOCK_BEGIN ( GET_MARK(q) == MARK )
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : free: mark not found %p\n", p);
    ASSERT_BLOCK_END

    ASSERT_BLOCK_BEGIN ( GET_FLAG(HEAD(q)) == FLAG_INUSE || GET_FLAG(HEAD(q)) == FLAG_INUSE_SLAB )
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : free not-allocated block? double free? p=%p\n", p);
    ASSERT_BLOCK_END

    u64 size = GET_SIZE(HEAD(q));
    ASSERT_BLOCK_BEGIN(GET_SIZE(TAIL(q, size)) == size)
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : two sizes doesn't match. p=%p\n", p);
    ASSERT_BLOCK_END

    u32 flIndex, slIndex;
    int skip_merges = 0;

    mappingInsert(size - ALLOC_OVERHEAD, &flIndex, &slIndex);
#ifdef FINE_LOCKING
#define SKIP_MERGES
#ifdef SKIP_MERGES
    // skip merges to add more 1's to bitmaps which will exploits parallelism
    // read without sync, so it's approximate
    if ( pool->sl[flIndex].listCount[slIndex] < 16)	// TODO
        skip_merges = 1;
#endif
#endif

    u64 size_orig = size;
    //DPRINTF(DEBUG_LVL_VERB, "before free : pool = %p, addr=%p\n", pool, INFO2(q));
    //quickPrint(pool);

    u64 *peer_right = &PEER_RIGHT(q, size);
    ASSERT_BLOCK_BEGIN ((u64)peer_right <= end) // peer_right may equal to end, it's checked again below
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : PEER_RIGHT address %p is above the heap area\n", peer_right);
    ASSERT_BLOCK_END

    ASSERT_BLOCK_BEGIN ((u64)&HEAD(q) >= start)
    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : address %p is below the heap area\n", &HEAD(q));
    ASSERT_BLOCK_END
    VALGRIND_CHUNK_CLOSE(q);

    if (!skip_merges && (u64)peer_right != end) {
        //printf("try right..\n");
        VALGRIND_CHUNK_OPEN(peer_right);

#ifdef FINE_LOCKING
        hal_lock(&HEAD_LOCK(peer_right));
        hal_lock(&HEAD_LOCK(q));
#endif

        ASSERT_BLOCK_BEGIN (  GET_MARK(q) == MARK )
        DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : right neighbor's mark not found %p\n", p);
        ASSERT_BLOCK_END

        HEAD(q) = MARK | GET_SIZE(HEAD(q)) | FLAG_FREE;   // change flag
        if (GET_FLAG(HEAD(peer_right)) == FLAG_FREE) {     // right block is free?
            //printf("merge right..\n");
            u64 peer_size = GET_SIZE(HEAD(peer_right));
#ifdef FINE_LOCKING
            hal_lock(&TAIL_LOCK(peer_right, peer_size));
#endif
            HEAD(peer_right) = MARK | peer_size | FLAG_MERGE;

            VALGRIND_CHUNK_CLOSE(peer_right);
            quickDeleteFree2(pool, peer_right, &bmap_op);
            VALGRIND_CHUNK_OPEN(peer_right);

            // TAIL_MARK( = 0; erase header??
            size += peer_size;
            TAIL(peer_right, peer_size) = MARK | size;
            HEAD(peer_right) = 0;    // erase header
            HEAD(q) = MARK | size | FLAG_FREE;         // clear in-use bit
            // peer_right merged.
        } else {
#ifdef FINE_LOCKING
            hal_unlock(&HEAD_LOCK(peer_right));
            hal_lock(&TAIL_LOCK(q, GET_SIZE(HEAD(q))));
#endif
        }
        VALGRIND_CHUNK_CLOSE(peer_right);
    } else {
#ifdef FINE_LOCKING
        hal_lock(&HEAD_LOCK(q));
        hal_lock(&TAIL_LOCK(q, GET_SIZE(HEAD(q))));
#endif
        HEAD(q) = MARK | GET_SIZE(HEAD(q)) | FLAG_FREE;   // clears in-use bit
    }

    VALGRIND_CHUNK_OPEN(q);
    if (!skip_merges && (u64)&HEAD(q) != start) {
        //printf("try left..\n");
        VALGRIND_CHUNK_OPEN_LEFT(q);
        u64 *peer_left;
#ifdef FINE_LOCKING
left_merge_retry:
        hal_lock(&PEER_LEFT_TAIL_LOCK(q));
#endif
        peer_left = &PEER_LEFT(q);
        ASSERT(peer_left != q);

        VALGRIND_CHUNK_CLOSE(q);
        // just omit chunk_close_left()
        VALGRIND_CHUNK_OPEN(peer_left);

#ifdef FINE_LOCKING
        {
        u32 ret;
        do {
            ret = hal_trylock(&HEAD_LOCK(peer_left));
            if (ret) {
                hal_unlock(&PEER_LEFT_TAIL_LOCK(q));
                hal_xadd32(&count_left_retry, 1);
                goto left_merge_retry;
            }
        } while(ret);
        hal_unlock(&PEER_LEFT_TAIL_LOCK(q));
        }
#endif
        ASSERT_BLOCK_BEGIN ( GET_MARK(peer_left) == MARK )
        DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : left neighbor's mark not found %p\n", p);
        ASSERT_BLOCK_END

        if (GET_FLAG(HEAD(peer_left))==FLAG_FREE) {      // left block is free?
            //printf("merge left..\n");
            u64 peer_size = GET_SIZE(HEAD(peer_left));
            HEAD(peer_left) = MARK | peer_size | FLAG_MERGE;

            // erase peer_left's mark?
            VALGRIND_CHUNK_CLOSE(peer_left);
            quickDeleteFree2(pool, peer_left, &bmap_op);
            VALGRIND_CHUNK_OPEN(peer_left);

            u64 new_size = size + peer_size;
            TAIL(q, size) = MARK | new_size;
            size = new_size;

            VALGRIND_CHUNK_OPEN(q);
            HEAD(q) = 0;    // erase header
            VALGRIND_CHUNK_CLOSE(q);
            q = peer_left;
            HEAD(q) = MARK | size | FLAG_FREE;    // clear in-use bit
        } else {
#ifdef FINE_LOCKING
            hal_unlock(&HEAD_LOCK(peer_left));
#endif
        }
        VALGRIND_CHUNK_CLOSE(peer_left);
    } else {
        VALGRIND_CHUNK_CLOSE(q);
    }
#ifdef FINE_LOCKING
    hal_unlock(&TAIL_LOCK(q, GET_SIZE(HEAD(q))));
#endif

    if (!skip_merges)  // if merged, recalculate due to size changes
        mappingInsert(size - ALLOC_OVERHEAD, &flIndex, &slIndex);
    quickInsertFree(pool, q, size, flIndex, slIndex);
#ifdef FINE_LOCKING
    ASSERT(GET_FLAG(HEAD(q)) == FLAG_FREE);
    hal_unlock(&HEAD_LOCK(q));
#endif
#ifdef FINE_LOCKING
    int i = bmap_op.count++;
    bmap_op.fli[i] = flIndex;
    bmap_op.sli[i] = slIndex;
    bmap_op.delta[i] = 1;

    doBmapOp(pool, &bmap_op);
#endif

    pool->count_used -= size_orig;  // count bytes using internal block size
    pool->count_free++;             // count successful deallocation calls

//    DPRINTF(DEBUG_LVL_WARN, "QuickAlloc : free: called\n");
//    quickPrintCounters(pool);
#ifndef FINE_LOCKING
    VALGRIND_POOL_OPEN(pool);
    hal_unlock(&(pool->lock));
    VALGRIND_POOL_CLOSE(pool);
#endif
#ifdef ENABLE_VALGRIND
    VALGRIND_MEMPOOL_FREE(pool, p);
//    DPRINTF(DEBUG_LVL_WARN,"mempool_free,  pool %p , p  %p, size %8ld\n", pool, p, size_orig - ALLOC_OVERHEAD);
#endif
}

// higher bits in 'user' that doesn't fit will be truncated.
static void quickSetUserbits(blkPayload_t *p, u64 user)
{
    if (p == NULL)
        return;
    u64 *q = USER_TO_HEAD(p);
    ASSERT(GET_USER(HEAD(q)) == 0);
    HEAD(q) |= ((user<<SHIFT_USER)&MASK_USER);
}

#ifdef PER_AGENT_CACHE
static struct slab_header *quickNewSlab(poolHdr_t *pool,s32 slabMaxSize, struct _ocrPolicyDomain_t *pd, struct per_agent_cache *per_agent)
{
        // goes to the central heap to allocate slab
        void *slab = quickMallocInternal(pool, sizeof(struct slab_header)+(SLAB_OVERHEAD+slabMaxSize)*MAX_OBJ_PER_SLAB, pd );
        if (slab == NULL) {
            DPRINTF(DEBUG_LVL_VERB, "Slab alloc failed (slabMaxSize %"PRId32"), falling back to central heap\n", slabMaxSize);
            return NULL;
        }

        // mark flag as slab flag
        u64 *q = USER_TO_HEAD(slab);
        HEAD(q) |= FLAG_FOR_SLAB;
        ASSERT(GET_FLAG(HEAD(q)) == FLAG_INUSE_SLAB);

        struct slab_header *head = slab;
        head->per_agent = addrGlobalizeOnTG((void *)per_agent, pd);
        head->next = head->prev = head;
        head->bitmap = (1UL << MAX_OBJ_PER_SLAB)-1UL;
        head->mark = SLAB_MARK;
        head->size = slabMaxSize;
        return head;
}

static blkPayload_t *quickMallocSlab(poolHdr_t *pool,u64 size, struct _ocrPolicyDomain_t *pd)
{
    if (size > MAX_SIZE_FOR_SLABS) {
        DPRINTF(DEBUG_LVL_WARN, "size %"PRId64" is too large for slab?\n", size);
        ASSERT(0);
    }

    // s64 myid = (s64)pd;
    // ASSERT(myid >=0 && myid < MAX_THREAD);
    if (CACHE_POOL(myid) == NULL) {
        CACHE_POOL(myid) = quickInitCache(pool);
        DPRINTF(DEBUG_LVL_VERB, "cache %p created, handles up to size %"PRId64"\n", CACHE_POOL(myid), (s64)MAX_SIZE_FOR_SLABS);
    }

    s32 slabsIndex = SIZE_TO_SLABS(size);
    s32 slabMaxSize = SLAB_MAX_SIZE(slabsIndex);
    hal_lock(&CACHE_POOL(myid)->lock);
    struct slab_header *slabs = CACHE_POOL(myid)->slabs[slabsIndex];
    if (slabs == NULL /* initial alloc? */ || slabs->bitmap == 0 /* full? */) {
        struct slab_header *slab = quickNewSlab(pool, slabMaxSize, pd, CACHE_POOL(myid));
        if (slab == NULL) {
            hal_unlock(&CACHE_POOL(myid)->lock);
            DPRINTF(DEBUG_LVL_WARN, "slab alloc failed -- too small heap?\n");
            return NULL;
        }
        if (slabs) {  // list manupulation
            slab->next = slabs;
            slab->prev = slabs->prev;
            slabs->prev->next = slab;
            slabs->prev = slab;
        }
        CACHE_POOL(myid)->slabs[slabsIndex] = slabs = slab;
    }
    ASSERT(slabs->bitmap);
    s32 pos = myffs(slabs->bitmap);
    ASSERT(pos >= 0 && pos < MAX_OBJ_PER_SLAB);
    u64 *p = (u64 *)((u64)slabs + sizeof(struct slab_header)+(SLAB_OVERHEAD+slabMaxSize)*pos);
    HEAD(p) = ((s64)slabs - (s64)p)&(~MASK_USER);   // for cached objects, put negative offset in header, and clear user bits.

    // for only TG
    void *ret = HEAD_TO_USER(p);
    INFO2(p) = (u64)ret; // already globalized addr
    ASSERT((*(u8 *)(&INFO2(p)) & POOL_HEADER_TYPE_MASK) == 0);
    *(u8 *)(&INFO2(p)) |= allocatorQuick_id;

    //__sync_fetch_and_xor(&slabs->bitmap , 1UL << pos);
    slabs->bitmap ^= 1UL << pos;
    ASSERT_BLOCK_BEGIN((slabs->bitmap & (1UL << pos)) == 0)
    ASSERT_BLOCK_END

    if (slabs->bitmap == 0) {  // if slab is full, point to next (partially-full) slab
        CACHE_POOL(myid)->slabs[slabsIndex] = slabs->next;     // next slab
    }
    CACHE_POOL(myid)->count_malloc[slabsIndex]++;
    hal_unlock(&CACHE_POOL(myid)->lock);
    return ret;
}

static blkPayload_t *quickMalloc(poolHdr_t *pool,u64 size, struct _ocrPolicyDomain_t *pd)
{
    if (size > MAX_SIZE_FOR_SLABS) // for big objects, go to the central heap
        return quickMallocInternal(pool, size, pd);
    return quickMallocSlab(pool, size, pd);
}

static void quickFree(blkPayload_t *p)
{
    if (p == NULL)
        return;
    u64 *q = USER_TO_HEAD(p);

    // in case of cached objects, the head holds negative offset to slab header
    // It has no flags, no mark, but it has user-bits. This will truncate higher 32bits,
    // removing user-bits.
    s32 neg_off = GET_SIZE(HEAD(q));
    if (neg_off >= 0) {
        quickFreeInternal(p);
        return;
    }
    // in case of cached object, it's negative offset to slab header

    struct slab_header *head = (struct slab_header *)((s64)(q) + neg_off);
    ASSERT(head->mark == SLAB_MARK);
    s64 offset = (s64)q - (s64)head - sizeof(struct slab_header);
    s64 pos = offset / (head->size+SLAB_OVERHEAD);
    ASSERT(pos >= 0 && pos < MAX_OBJ_PER_SLAB);
    //printf("%"PRIx64" , %"PRId64", pos %"PRId32"\n", HEAD(q), HEAD(q), pos);
    //printf("offset %"PRId64" , size %"PRId32" \n", offset, head->size+SLAB_OVERHEAD);
    ASSERT((offset % (head->size+SLAB_OVERHEAD)) == 0);

    // local if (addrGlobalizeOnTG(CACHE_POOL(X)) == head->per_agent)
    s32 slabsIndex = SIZE_TO_SLABS(head->size);
    hal_lock(&head->per_agent->lock);
    struct slab_header *slabs = head->per_agent->slabs[slabsIndex];

    //    __sync_fetch_and_xor(&head->bitmap , 1UL << pos);
    head->bitmap ^= 1UL << pos;
    ASSERT_BLOCK_BEGIN((head->bitmap & (1UL << pos)) != 0)
    ASSERT_BLOCK_END

    (head->per_agent->count_free[slabsIndex])++;

    if (slabs == head ) {  // so we will have at least one slab always
        hal_unlock(&head->per_agent->lock);
        return;
    }

    if (head->bitmap == ((1UL << MAX_OBJ_PER_SLAB)-1UL) /* empty slab? */) {
        head->next->prev = head->prev;
        head->prev->next = head->next;
        hal_unlock(&head->per_agent->lock);
        quickFreeInternal(head);
        return;
    }
    // if prev slab is full, we move current slab to header to roughly sort
    // this list. The first part is partially used slabs, then full slabs follows.
    if (head->prev->bitmap == 0UL /* full (previous) slab? */) {
        head->next->prev = head->prev;
        head->prev->next = head->next;

        head->next = slabs;
        head->prev = slabs->prev;
        slabs->prev->next = head;
        slabs->prev = head;

        head->per_agent->slabs[slabsIndex] = head;
    }
    hal_unlock(&head->per_agent->lock);
}
#else
static inline blkPayload_t *quickMalloc(poolHdr_t *pool,u64 size, struct _ocrPolicyDomain_t *pd)
{
    return quickMallocInternal(pool, size, pd);
}
static inline void quickFree(blkPayload_t *p)
{
    return quickFreeInternal(p);
}
#endif
// end of simple alloc core part

#ifndef ENABLE_ALLOCATOR_QUICK_STANDALONE
void quickDestruct(ocrAllocator_t *self) {
    DPRINTF(DEBUG_LVL_VERB, "Entered quickDestruct on allocator 0x%"PRIx64"\n", (u64) self);
    ASSERT(self->memoryCount == 1);
    self->memories[0]->fcts.destruct(self->memories[0]);
    /*
      BUG #288
      Should we do this? It is the clean thing to do but may
      cause mismatch between way it was created and freed
    */
    //runtimeChunkFree((u64)self->memories, PERSISTENT_CHUNK);
    //DPRINTF(DEBUG_LVL_WARN, "quickDestruct free %p\n", (u64)self->memories );

    runtimeChunkFree((u64)self, NULL);
    DPRINTF(DEBUG_LVL_VERB, "Leaving quickDestruct on allocator 0x%"PRIx64" (free)\n", (u64) self);
}


u8 quickSwitchRunlevel(ocrAllocator_t *self, ocrPolicyDomain_t *PD, ocrRunlevel_t runlevel,
                             phase_t phase, u32 properties, void (*callback)(ocrPolicyDomain_t*, u64), u64 val) {
    u8 toReturn = 0;
    // This is an inert module, we do not handle callbacks (caller needs to wait on us)
    ASSERT(callback == NULL);

    // Verify properties for this call
    ASSERT((properties & RL_REQUEST) && !(properties & RL_RESPONSE)
           && !(properties & RL_RELEASE));
    ASSERT(!(properties & RL_FROM_MSG));

    ASSERT(self->memoryCount == 1);
    // Call the runlevel change on the underlying memory
    // On tear-down, we do it *AFTER* we do stuff because otherwise our mem-platform goes away
    if(properties & RL_BRING_UP)
        toReturn |= self->memories[0]->fcts.switchRunlevel(self->memories[0], PD, runlevel, phase, properties,
                                                           NULL, 0);
    switch(runlevel) {
    case RL_CONFIG_PARSE:
    {
        // On bring-up: Update PD->phasesPerRunlevel on phase 0
        // and check compatibility on phase 1
        break;
    }
    case RL_NETWORK_OK:
        // Nothing
        break;
    case RL_PD_OK:
        if(properties & RL_BRING_UP) {
            // We can now set our PD (before this, we couldn't because
            // "our" PD might not have been started
            self->pd = PD;
        }
        break;
    case RL_MEMORY_OK:
        if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_MEMORY_OK, phase)) {
            ocrAllocatorQuick_t * rself = (ocrAllocatorQuick_t *) self;

            u64 poolAddr = 0;
            DPRINTF(DEBUG_LVL_VERB, "quickBegin : poolsize 0x%"PRIx64", level %"PRIu64", startAddr 0x%"PRIx64"\n",
                    rself->poolSize, self->memories[0]->level, self->memories[0]->memories[0]->startAddr);

            // if this RESULT_ASSERT fails, it usually means not-enough-free-memory.
            // For example, for L1, increased executable size easily shrinks free area.
            // Try shrink heap size in config file, or adjust layout (smaller stack area?)
            RESULT_ASSERT(self->memories[0]->fcts.chunkAndTag(
                self->memories[0], &poolAddr, rself->poolSize,
                USER_FREE_TAG, USER_USED_TAG), ==, 0);
            rself->poolAddr = poolAddr;
            DPRINTF(DEBUG_LVL_VERB, "quickBegin : poolAddr 0x%"PRIx64"\n", poolAddr);

            // Adjust alignment if required
            u64 fiddlyBits = ((u64) rself->poolAddr) & (ALIGNMENT - 1LL);
            if (fiddlyBits == 0) {
                rself->poolStorageOffset = 0;
            } else {
                rself->poolStorageOffset = ALIGNMENT - fiddlyBits;
                rself->poolAddr += rself->poolStorageOffset;
                rself->poolSize -= rself->poolStorageOffset;
            }
            rself->poolStorageSuffix = rself->poolSize & (ALIGNMENT-1LL);
            rself->poolSize &= ~(ALIGNMENT-1LL);
            DPRINTF(DEBUG_LVL_VERB, "QUICK Allocator @ %p got pool at address 0x%"PRIx64" of size 0x%"PRIx64"(%"PRId64"), "
                    "offset from storage addr by %"PRId64"\n",
                    rself, rself->poolAddr, (u64) (rself->poolSize), (u64)(rself->poolSize), (u64) (rself->poolStorageOffset));
#if 0 // TODO: 4.1.0 - check if this is really needed
            // See bug #875
            // at this moment, this is for only x86
            ASSERT(self->memories[0]->memories[0]->startAddr /* startAddr of the memory that memplatform allocated. (for x86, at mallocBegin()) */
                      + MEM_PLATFORM_ZEROED_AREA_SIZE >= /* Add the size of zero-ed area (for x86, at mallocBegin()), then this should be greater than */
                     rself->poolAddr + sizeof(poolHdr_t) /* the end of poolHdr_t, so this ensures zero'ed rangeTracker,pad,poolHdr_t */ );
#endif
            quickInit((poolHdr_t *)addrGlobalizeOnTG((void *)rself->poolAddr, PD), rself->poolSize);
        } else if((properties & RL_TEAR_DOWN) && RL_IS_LAST_PHASE_DOWN(PD, RL_MEMORY_OK, phase)) {
            ocrAllocatorQuick_t * rself = (ocrAllocatorQuick_t *) self;
            ASSERT(self->memoryCount == 1);

            quickFinish((poolHdr_t *)addrGlobalizeOnTG((void *)rself->poolAddr, PD), rself->poolSize);

            RESULT_ASSERT(self-> /*rAnchorCE->base.*/ memories[0]->fcts.tag(
                rself->base.memories[0],
                rself->poolAddr - rself->poolStorageOffset,
                rself->poolAddr + rself->poolSize + rself->poolStorageSuffix,
                USER_FREE_TAG), ==, 0);
        }
        break;
    case RL_GUID_OK:
        break;
    case RL_COMPUTE_OK:
        if(properties & RL_BRING_UP) {
            if(RL_IS_FIRST_PHASE_UP(PD, RL_COMPUTE_OK, phase)) {
                // We get a GUID for ourself
                guidify(self->pd, (u64)self, &(self->fguid), OCR_GUID_ALLOCATOR);
            }
        } else {
            // Tear-down
            if(RL_IS_LAST_PHASE_DOWN(PD, RL_COMPUTE_OK, phase)) {
                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
                msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(guid) = self->fguid;
                PD_MSG_FIELD_I(properties) = 0;
                toReturn |= self->pd->fcts.processMessage(self->pd, &msg, false);
                self->fguid.guid = NULL_GUID;
#undef PD_MSG
#undef PD_TYPE
            }
        }
        break;
    case RL_USER_OK:
        break;
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    if(properties & RL_TEAR_DOWN)
        toReturn |= self->memories[0]->fcts.switchRunlevel(self->memories[0], PD, runlevel, phase, properties,
                                                           NULL, 0);
    return toReturn;
}

void* quickAllocate(
    ocrAllocator_t *self,   // Allocator to attempt block allocation
    u64 size,               // Size of desired block, in bytes
    u64 hints) {            // Allocator-dependent hints

    ocrAllocatorQuick_t * rself = (ocrAllocatorQuick_t *) self;

    void *ret;
#ifdef PER_AGENT_CACHE
    if (hints & OCR_ALLOC_HINT_RUNTIME) {
        // ideally pdMalloc uses fast path only, but sometimes pdMalloc wants quite big size,
        // e.g. ~256KB for baseDequeInit() so we use quickMalloc() to check just big sizes.
        // ret = quickMallocSlab((poolHdr_t *)rself->poolAddr, size, self->pd);
        ret = quickMalloc((poolHdr_t *)rself->poolAddr, size, self->pd);
    } else {
        ret = quickMalloc((poolHdr_t *)rself->poolAddr, size, self->pd);
    }
#else
    ret = quickMalloc((poolHdr_t *)rself->poolAddr, size, self->pd);
#endif
    DPRINTF(DEBUG_LVL_VERB, "quickAllocate called, ret %p from PoolAddr %"PRIx64"\n", ret, rself->poolAddr);
    quickSetUserbits(ret, hints);
    return ret;
}
void quickDeallocate(void* address) {
    DPRINTF(DEBUG_LVL_VERB, "quickDeallocate called, %p\n", address);
    quickFree(address);
}
void* quickReallocate(
    ocrAllocator_t *self,   // Allocator to attempt block allocation
    void * pCurrBlkPayload, // Address of existing block.  (NOT necessarily allocated to this Allocator instance, nor even in an allocator of this type.)
    u64 size,               // Size of desired block, in bytes
    u64 hints) {            // Allocator-dependent hints
    ASSERT(0);
    return 0;
}

/******************************************************/
/* OCR ALLOCATOR QUICK FACTORY                        */
/******************************************************/

// Method to create the QUICK allocator
ocrAllocator_t * newAllocatorQuick(ocrAllocatorFactory_t * factory, ocrParamList_t *perInstance) {

    ocrAllocatorQuick_t *result = (ocrAllocatorQuick_t*)
        runtimeChunkAlloc(sizeof(ocrAllocatorQuick_t), PERSISTENT_CHUNK);
    ocrAllocator_t * base = (ocrAllocator_t *) result;
    factory->initialize(factory, base, perInstance);
    return (ocrAllocator_t *) result;
}
void initializeAllocatorQuick(ocrAllocatorFactory_t * factory, ocrAllocator_t * self, ocrParamList_t * perInstance) {
    initializeAllocatorOcr(factory, self, perInstance);

    ocrAllocatorQuick_t *derived = (ocrAllocatorQuick_t *)self;
    paramListAllocatorQuick_t *perInstanceReal = (paramListAllocatorQuick_t*)perInstance;

    derived->poolAddr          = 0ULL;
    derived->poolSize          = perInstanceReal->base.size;
    derived->poolStorageOffset = 0;
    derived->poolStorageSuffix = 0;
}

static void destructAllocatorFactoryQuick(ocrAllocatorFactory_t * factory) {
    DPRINTF(DEBUG_LVL_VERB, "destructQuick called. (This is x86 only?) free %p\n", factory);
    runtimeChunkFree((u64)factory, NULL);
}

ocrAllocatorFactory_t * newAllocatorFactoryQuick(ocrParamList_t *perType) {
    ocrAllocatorFactory_t* base = (ocrAllocatorFactory_t*)
        runtimeChunkAlloc(sizeof(ocrAllocatorFactoryQuick_t), NONPERSISTENT_CHUNK);
    ASSERT(base);
    base->instantiate = &newAllocatorQuick;
    base->initialize = &initializeAllocatorQuick;
    base->destruct = &destructAllocatorFactoryQuick;
    base->allocFcts.destruct = FUNC_ADDR(void (*)(ocrAllocator_t*), quickDestruct);
    base->allocFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrAllocator_t*, ocrPolicyDomain_t*, ocrRunlevel_t,
                                                      phase_t, u32, void (*)(ocrPolicyDomain_t*, u64), u64), quickSwitchRunlevel);
    base->allocFcts.allocate = FUNC_ADDR(void* (*)(ocrAllocator_t*, u64, u64), quickAllocate);
    //base->allocFcts.free = FUNC_ADDR(void (*)(void*), quickDeallocate);
    base->allocFcts.reallocate = FUNC_ADDR(void* (*)(ocrAllocator_t*, void*, u64), quickReallocate);
    return base;
}
#endif

#endif /* ENABLE_ALLOCATOR_QUICK */
