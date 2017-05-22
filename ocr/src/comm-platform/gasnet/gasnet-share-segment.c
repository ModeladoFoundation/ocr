
/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_GASNET

#include "debug.h"

#include "amhandler.h"
#include "gasnet-share-segment.h"
#include "splay-tree.h"

#include <gasnet.h>

#define DEBUG_TYPE      COMM_PLATFORM


// ---------------------------------
// global variables
// ---------------------------------

// blocks of shared segment
static gasnetCommBlockSender_t segmentBlock[GASNET_MAX_SEGMENT_BLOCK];


// ---------------------------------
// initialization
// ---------------------------------
void gasnetBlockInit() {
    gasnet_node_t nodes = gasnet_nodes();

    // get the address of all the nodes including myself
    gasnet_seginfo_t segments[nodes], seginfo;
    GASNET_Safe( gasnet_getSegmentInfo(segments, nodes) );

    // store the information about my segment
    gasnet_node_t rank = gasnet_mynode();
    seginfo.addr = segments[rank].addr;
    seginfo.size = segments[rank].size;

    uintptr_t size =  seginfo.size / GASNET_MAX_SEGMENT_BLOCK;
    uintptr_t totSize  = 0;
    int i;
    // initialize list of segment blocks
    for(i=0; i < GASNET_MAX_SEGMENT_BLOCK; i++) {
        segmentBlock[i].block.addr = seginfo.addr + totSize;
        segmentBlock[i].block.size = size;
        segmentBlock[i].reserved = 0;
        totSize += size;
        if (i+1 == GASNET_MAX_SEGMENT_BLOCK) {
            size = seginfo.size - totSize;
        }
    }
}

/*
 * @brief get and reserve a segment block
 */
gasnetCommBlockSender_t* gasnetReserveSegmentBlock() {
    int i;
    for(i=0; i< GASNET_MAX_SEGMENT_BLOCK; i++) {
        if (segmentBlock[i].reserved == 0) {
            segmentBlock[i].reserved = 1;   // flip the flag
            DPRINTF(DEBUG_LVL_VVERB,"[GASNET] gasnetReserveSegmentBlock %p  size: %"PRId32" bytes\n",
                                    segmentBlock[i].block.addr, segmentBlock[i].block.size);
            return &segmentBlock[i];  // return the reserved block
        }
    }
    // no block available
    return NULL;
}


/*
 * @brief release a segment block
 */
void gasnetReleaseSegmentBlock(void *addr) {
    int i;
    for (i=0; i<GASNET_MAX_SEGMENT_BLOCK; i++) {
        if (segmentBlock[i].block.addr == addr) {
            segmentBlock[i].reserved = 0;
            DPRINTF(DEBUG_LVL_VVERB,"[GASNET] gasnetReleaseSegmentBlock %p  size: %"PRId32" bytes\n",
                                    addr, segmentBlock[i].block.size);
            return;
        }
    }
    DPRINTF(DEBUG_LVL_VVERB, "[GASNET] gasnetReleaseSegmentBlock addr not found: %p\n", addr);
}

// ------------------------------------------------------------------
// Splay tree declaration for storing segment block
// ------------------------------------------------------------------

typedef struct SegmentBlock_s
{
  SPLAY_ENTRY(SegmentBlock_s) link;
  gasnetCommBlock_t *block;   // the block of the segment
  u32  provider;    // the original node of the shared segment
} SegmentBlock_t;


static int block_compare(SegmentBlock_t *b1, SegmentBlock_t *b2) {
  return b1->provider - b2->provider;
}

/*
 * @brief rootBlock for the splay tree
 */
SPLAY_HEAD(SegmentBlockHead_s, SegmentBlock_s) rootBlock = SPLAY_INITIALIZER(rootBlock);

/*
 * @brief prototype of the tree
 */
SPLAY_PROTOTYPE(SegmentBlockHead_s, SegmentBlock_s, link, block_compare);

/*
 * @brief definition of the tree
 */
SPLAY_GENERATE(SegmentBlockHead_s, SegmentBlock_s, link, block_compare);


/*
 * @brief push a segment block into the database
 */
void gasnetSegmentBlockPush(ocrPolicyDomain_t * pd, u32 node, gasnetCommBlock_t *block) {
    SegmentBlock_t *item = (SegmentBlock_t *) pd->fcts.pdMalloc(pd, sizeof(SegmentBlock_t));
    ADebug(AllocDebugAllPD, "gasnet-share-segment.c/gasnetSegmentBlockPush() "
           "pdMalloc(%ld), addr=%p\n", sizeof(SegmentBlock_t), item);
    ASSERT(item != NULL);
    item->provider = node;
    item->block = block;
    SPLAY_INSERT(SegmentBlockHead_s, &rootBlock, item);
}

/*
 * @brief release a segment block
 *
 * The caller is responsible to free the returned variable
 */
gasnetCommBlock_t* gasnetSegmentBlockGet(ocrPolicyDomain_t * pd, u32 node) {
    SegmentBlock_t tmp = {.block = NULL, .provider = node};
    SegmentBlock_t *item = SPLAY_FIND(SegmentBlockHead_s, &rootBlock, &tmp);
    gasnetCommBlock_t *block = NULL;
    if (item != NULL) {
        block = item->block;
        SPLAY_REMOVE(SegmentBlockHead_s, &rootBlock, item);
        pd->fcts.pdFree(pd, item);
    }
    return block;
}

#endif /* ENABLE_COMM_PLATFORM_GASNET */
