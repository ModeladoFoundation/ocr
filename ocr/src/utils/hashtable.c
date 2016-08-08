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
#include "utils/hashtable.h"

#define DEBUG_TYPE UTIL

/* Hashtable entries */
typedef struct _ocr_hashtable_entry_struct {
    void * key;
    void * value;
    struct _ocr_hashtable_entry_struct * nxt; /* the next bucket in the hashtable */
} ocr_hashtable_entry;

/******************************************************/
/* CONCURRENT BUCKET LOCKED HASHTABLE                 */
/******************************************************/

#ifdef STATS_HASHTABLE
typedef struct _hashtableBucketLockedStats_t {
    u64 watermark;
    u64 current;
#ifdef STATS_HASHTABLE_COLLIDE
    u64 get;
    u64 put;
    u64 del;
#endif
} hashtableBucketLockedStats_t;

static void hashtableBucketLockedStatsInit(hashtableBucketLockedStats_t * stats) {
    stats->watermark = 0;
    stats->current = 0;
#ifdef STATS_HASHTABLE_COLLIDE
    stats->get = 0;
    stats->put = 0;
    stats->del = 0;
#endif

}
#endif

#ifdef HASHTABLE_LOCK_SPREAD
#define GET_LOCK_IDX(id) (id * (CACHE_LINE_SZB/sizeof(lock_t)))
#else
#define GET_LOCK_IDX(id) (id)
#endif

typedef struct _hashtableBucketLocked_t {
    hashtable_t base;
    lock_t * bucketLock;
#ifdef STATS_HASHTABLE
    hashtableBucketLockedStats_t * stats;
#endif
} hashtableBucketLocked_t;

/******************************************************/
/* HASHTABLE COMMON FUNCTIONS                         */
/******************************************************/

static ocr_hashtable_entry* hashtableFindEntryAndPrev(hashtable_t * hashtable, void * key, ocr_hashtable_entry** prev) {
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    ocr_hashtable_entry * current = hashtable->table[bucket];
    *prev = current;
    while(current != NULL) {
        if (current->key == key) {
            return current;
        }
        *prev = current;
        current = current->nxt;
    }
    *prev = NULL;
    return NULL;
}

/**
 * @brief find hashtable entry for 'key'.
 */
static ocr_hashtable_entry* hashtableFindEntry(hashtable_t * hashtable, void * key) {
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    ocr_hashtable_entry * current = hashtable->table[bucket];
    while(current != NULL) {
        if (current->key == key) {
            return current;
        }
        current = current->nxt;
    }
    return NULL;
}

/**
 * @brief Initialize the hashtable.
 */
static void hashtableInit(ocrPolicyDomain_t * pd, hashtable_t * hashtable, u32 nbBuckets, hashFct hashing) {
    hashtable->pd = pd;
    ocr_hashtable_entry ** table = pd->fcts.pdMalloc(pd, nbBuckets*sizeof(ocr_hashtable_entry*));
    u32 i;
    for (i=0; i < nbBuckets; i++) {
        table[i] = NULL;
    }
    hashtable->table = table;
    hashtable->nbBuckets = nbBuckets;
    hashtable->hashing = hashing;
}

/**
 * @brief Create a new hashtable instance that uses the specified hashing function.
 */
hashtable_t * newHashtable(ocrPolicyDomain_t * pd, u32 nbBuckets, hashFct hashing) {
    hashtable_t * hashtable = pd->fcts.pdMalloc(pd, sizeof(hashtable_t));
    hashtableInit(pd, hashtable, nbBuckets, hashing);
    return hashtable;
}

/**
 * @brief Destruct the hashtable and all its entries (do not deallocate keys and values pointers).
 */
void destructHashtable(hashtable_t * hashtable, deallocFct entryDeallocator, void * deallocatorParam) {
    ocrPolicyDomain_t * pd = hashtable->pd;
    // go over each bucket and deallocate entries
    u32 i = 0;
    while(i < hashtable->nbBuckets) {
        ocr_hashtable_entry * bucketHead = hashtable->table[i];
        while (bucketHead != NULL) {
            ocr_hashtable_entry * next = bucketHead->nxt;
            if (entryDeallocator != NULL) {
                entryDeallocator(bucketHead->key, bucketHead->value, deallocatorParam);
            }
            pd->fcts.pdFree(pd, bucketHead);
            bucketHead = next;
        }
        i++;
    }
    pd->fcts.pdFree(pd, hashtable->table);
    pd->fcts.pdFree(pd, hashtable);
}

void iterateHashtable(hashtable_t * hashtable, hashtableIterateFct iterate, void * args) {
    u32 i = 0;
    while(i < hashtable->nbBuckets) {
        ocr_hashtable_entry * entry = hashtable->table[i];
        while (entry != NULL) {
            ocr_hashtable_entry * next = entry->nxt;
            iterate(entry->key, entry->value, args);
            entry = next;
        }
        i++;
    }
}

/**
 * @brief Create a new hashtable bucket locked instance that uses the specified hashing function.
 */
hashtable_t * newHashtableBucketLocked(ocrPolicyDomain_t * pd, u32 nbBuckets, hashFct hashing) {
    hashtable_t * hashtable = pd->fcts.pdMalloc(pd, sizeof(hashtableBucketLocked_t));
    hashtableInit(pd, hashtable, nbBuckets, hashing);
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
    u32 i;
#ifdef HASHTABLE_LOCK_SPREAD
    ASSERT(sizeof(lock_t)<(CACHE_LINE_SZB));
    u64 bucketLockSz = nbBuckets*CACHE_LINE_SZB;
#else
    u64 bucketLockSz = nbBuckets*sizeof(lock_t);
#endif
    lock_t * bucketLock = pd->fcts.pdMalloc(pd, bucketLockSz);
#ifdef STATS_HASHTABLE
    hashtableBucketLockedStats_t * stats = pd->fcts.pdMalloc(pd, nbBuckets*sizeof(hashtableBucketLockedStats_t));
#endif
    for (i=0; i < nbBuckets; i++) {
        bucketLock[GET_LOCK_IDX(i)] = INIT_LOCK;
#ifdef STATS_HASHTABLE
        hashtableBucketLockedStatsInit(&stats[i]);
#endif
    }
    rhashtable->bucketLock = bucketLock;
#ifdef STATS_HASHTABLE
    rhashtable->stats = stats;
#endif
    return hashtable;
}

#ifdef STATS_HASHTABLE
#define GET_STAT_OFFSET(name) ((u64) &(((hashtableBucketLockedStats_t*)0)->watermark))
#define READ_STAT_VALUE(statEntry, offset) (*((u64*)((char *) statEntry)+offset))

static void dumpStats(hashtable_t * self, char * name, u64 offset) {
    hashtableBucketLocked_t * rself = (hashtableBucketLocked_t *) self;
    u32 nbBuckets = self->nbBuckets;
    if (nbBuckets > 0) {
        PRINTF("[PD:0x%"PRIu64"] Hashtable@%p statistics\n", (u64) self->pd->myLocation, self);
#ifdef STATS_HASHTABLE_VERB
        u64 curWm = READ_STAT_VALUE(&rself->stats[0], offset);
        u64 lb = 0;
#endif
        u64 hWm = READ_STAT_VALUE(&rself->stats[0], offset);
        u32 i;
        for (i=1; i < nbBuckets; i++) {
            u64 wm = READ_STAT_VALUE(&rself->stats[i], offset);
#ifdef STATS_HASHTABLE_VERB
            if (wm == curWm) {
                continue;
            } else { // Print previous if any
                if ((i-lb) == 1) {
                    PRINTF("[PD:0x%"PRIu64"] Bucket[%"PRId32"]: %s=%"PRIu64"\n", (u64) self->pd->myLocation, lb, name, curWm);
                } else {
                    PRINTF("[PD:0x%"PRIu64"] Bucket[%"PRId32"-%"PRId32"]: %s=%"PRIu64"\n", (u64) self->pd->myLocation, lb, i-1, name, curWm);
                }
                lb = i;
                curWm = wm;
            }
#endif
            if (wm > hWm) {
                hWm = wm;
            }
        }
#ifdef STATS_HASHTABLE_VERB
        if ((i-lb) == 1) {
            PRINTF("[PD:0x%"PRIu64"] Bucket[%"PRId32"]: %s=%"PRIu64"\n", (u64) self->pd->myLocation, lb, name, curWm);
        } else {
            PRINTF("[PD:0x%"PRIu64"] Bucket[%"PRId32"-%"PRId32"]: %s=%"PRIu64"\n", (u64) self->pd->myLocation, lb, i-1, name, curWm);
        }
#endif
        PRINTF("[PD:0x%"PRIu64"] High %s=%"PRIu64"\n", (u64) self->pd->myLocation, name, hWm);
    }
}
#endif

/**
 * @brief Destruct the hashtable and all its entries (do not deallocate keys and values pointers).
 */
void destructHashtableBucketLocked(hashtable_t * hashtable, deallocFct entryDeallocator, void * deallocatorParam) {
    ocrPolicyDomain_t * pd = hashtable->pd;
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
    pd->fcts.pdFree(pd, (void*)(rhashtable->bucketLock));
#ifdef STATS_HASHTABLE
    dumpStats(hashtable,"watermark", GET_STAT_OFFSET(watermark));
#ifdef STATS_HASHTABLE_COLLIDE
    dumpStats(hashtable,"get", GET_STAT_OFFSET(get));
    dumpStats(hashtable,"put", GET_STAT_OFFSET(put));
    dumpStats(hashtable,"del", GET_STAT_OFFSET(del));
#endif
#endif
    destructHashtable(hashtable, entryDeallocator, deallocatorParam);
}

void iterateHashtableBucketLocked(hashtable_t * hashtable, hashtableIterateFct iterate, void * args) {
    u32 i;
    u32 nbBuckets = hashtable->nbBuckets;
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
    for (i = 0; i < nbBuckets; i++) {
        hal_lock(&(rhashtable->bucketLock[i]));
        ocr_hashtable_entry * entry = hashtable->table[i];
        while (entry != NULL) {
            ocr_hashtable_entry * next = entry->nxt;
            iterate(entry->key, entry->value, args);
            entry = next;
        }
        hal_unlock(&(rhashtable->bucketLock[i]));
    }
}

/******************************************************/
/* NON-CONCURRENT HASHTABLE                           */
/******************************************************/

/**
 * @brief get the value associated with a key
 */
void * hashtableNonConcGet(hashtable_t * hashtable, void * key) {
    ocr_hashtable_entry * entry = hashtableFindEntry(hashtable, key);
    return (entry == NULL) ? 0x0: entry->value;
}

/**
 * @brief Attempt to insert the key if absent.
 * Return the current value associated with the key
 * if present in the table, otherwise returns the value
 * passed as parameter.
 */
void * hashtableNonConcTryPut(hashtable_t * hashtable, void * key, void * value) {
    ocr_hashtable_entry * entry = hashtableFindEntry(hashtable, key);
    if (entry == NULL) {
        hashtableNonConcPut(hashtable, key, value);
        return value;
    } else {
        return entry->value;
    }
}

/**
 * @brief Put a key associated with a given value in the map.
 */
bool hashtableNonConcPut(hashtable_t * hashtable, void * key, void * value) {
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    ocrPolicyDomain_t * pd = hashtable->pd;
    ocr_hashtable_entry * newHead = pd->fcts.pdMalloc(pd, sizeof(ocr_hashtable_entry));
    newHead->key = key;
    newHead->value = value;
    ocr_hashtable_entry * oldHead = hashtable->table[bucket];
    newHead->nxt = oldHead;
    hashtable->table[bucket] = newHead;
    return true;
}

/**
 * @brief Removes a key from the table.
 * If 'value' is not NULL, fill-in the pointer with the entry's value associated with 'key'.
 * If the hashtable implementation allows for NULL values entries, check the returned boolean.
 * Returns true if entry has been found and removed.
 */
bool hashtableNonConcRemove(hashtable_t * hashtable, void * key, void ** value) {
    ocr_hashtable_entry * prev;
    ocr_hashtable_entry * entry = hashtableFindEntryAndPrev(hashtable, key, &prev);
    if (entry != NULL) {
        ASSERT(prev != NULL);
        ASSERT(key == entry->key);
        if (entry == prev) {
            // entry is bucket's head
            u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
            hashtable->table[bucket] = entry->nxt;
        } else {
            prev->nxt = entry->nxt;
        }
        if (value != NULL) {
            *value = entry->value;
        }
        hashtable->pd->fcts.pdFree(hashtable->pd, entry);
        return true;
    }
    return false;
}


/******************************************************/
/* CONCURRENT HASHTABLE FUNCTIONS                     */
/******************************************************/

/**
 * @brief get the value associated with a key
 */
void * hashtableConcGet(hashtable_t * hashtable, void * key) {
    ocr_hashtable_entry * entry = hashtableFindEntry(hashtable, key);
    return (entry == NULL) ? 0x0 : entry->value;
}

/**
 * @brief Attempt to insert the key if absent.
 * Return the current value associated with the key
 * if present in the table, otherwise returns the value
 * passed as parameter.
 */
void * hashtableConcTryPut(hashtable_t * hashtable, void * key, void * value) {
    // Compete with other potential put
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    ocr_hashtable_entry * newHead = NULL;
    bool tryPut = true;
    ocrPolicyDomain_t * pd = hashtable->pd;
    while(tryPut) {
        // Lookup for the key
        ocr_hashtable_entry * oldHead = hashtable->table[bucket];
        // Ensure oldHead is read before iterating
        //BUG #589 not sure which hashtable members should be volatile
        hal_fence();
        ocr_hashtable_entry * entry = hashtableFindEntry(hashtable, key);
        if (entry == NULL) {
            // key is not there, try to CAS the head to insert it
            if (newHead == NULL) {
                newHead = pd->fcts.pdMalloc(pd, sizeof(ocr_hashtable_entry));
                newHead->key = key;
                newHead->value = value;
            }
            newHead->nxt = oldHead;
            // if old value read is different from the old value read by the cmpswap we failed
            tryPut = (hal_cmpswap64((u64*)&hashtable->table[bucket], (u64)oldHead, (u64)newHead) != (u64)oldHead);
            // If failed, go for another round of check, there's been an insert but we don't
            // know if it's for that key or another one which hash matches the bucket
        } else {
            if (newHead != NULL) {
                ASSERT(pd != NULL);
                // insertion failed because the key had been inserted by a concurrent thread
                pd->fcts.pdFree(pd, newHead);
            }
            // key already present, return the value
            return entry->value;
        }
    }
    // could successfully insert the (key,value) pair
    return value;
}

/**
 * @brief Put a key associated with a given value in the map.
 * The put always occurs.
 */
bool hashtableConcPut(hashtable_t * hashtable, void * key, void * value) {
    // Compete with other potential put
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    ocrPolicyDomain_t * pd = hashtable->pd;
    ocr_hashtable_entry * newHead = pd->fcts.pdMalloc(pd, sizeof(ocr_hashtable_entry));
    newHead->key = key;
    newHead->value = value;
    bool success;
    do {
        ocr_hashtable_entry * oldHead = hashtable->table[bucket];
        newHead->nxt = oldHead;
        success = (hal_cmpswap64((u64*)&hashtable->table[bucket], (u64)oldHead, (u64)newHead) == (u64)oldHead);
    } while (!success);
    return success;
}

/**
 * @brief Removes a key from the table.
 * If 'value' is not NULL, fill-in the pointer with the entry's value associated with 'key'.
 * If the hashtable implementation allows for NULL values entries, check the returned boolean.
 * Returns true if entry has been found and removed.
 */
bool hashtableConcRemove(hashtable_t * hashtable, void * key, void ** value) {
    //BUG #589 implement concurrent removal
    ASSERT(false);
    return false;
}


/******************************************************/
/* CONCURRENT BUCKET LOCKED HASHTABLE                 */
/******************************************************/

void * hashtableConcBucketLockedGet(hashtable_t * hashtable, void * key) {
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
#ifdef STATS_HASHTABLE_COLLIDE
    bool isBusy = hal_islocked(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
#endif
    hal_lock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
#ifdef STATS_HASHTABLE
#ifdef STATS_HASHTABLE_COLLIDE
    hashtableBucketLockedStats_t * stats = &rhashtable->stats[bucket];
    if (isBusy) { stats->get++; }
#endif
#endif
    ocr_hashtable_entry * entry = hashtableFindEntry(hashtable, key);
    //DPRINTF(DEBUG_LVL_WARN, "ht=%p For get key=%p: bucket=%"PRIu32" found entry=%p value=%p\n", hashtable, key, bucket, entry, (entry) ? entry->value : NULL);
    hal_unlock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
    return (entry == NULL) ? 0x0 : entry->value;
}

bool hashtableConcBucketLockedPut(hashtable_t * hashtable, void * key, void * value) {
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
    ocr_hashtable_entry * newHead = hashtable->pd->fcts.pdMalloc(hashtable->pd, sizeof(ocr_hashtable_entry));
    newHead->key = key;
    newHead->value = value;
    //DPRINTF(DEBUG_LVL_WARN, "ht=%p For put key=%p: bucket=%"PRIu32" found entry=%p value=%p\n", hashtable, key, bucket, newHead, newHead->value);
#ifdef STATS_HASHTABLE_COLLIDE
    bool isBusy = hal_islocked(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
#endif
    hal_lock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
    ocr_hashtable_entry * oldHead = hashtable->table[bucket];
    newHead->nxt = oldHead;
    hashtable->table[bucket] = newHead;
#ifdef STATS_HASHTABLE
    hashtableBucketLockedStats_t * stats = &rhashtable->stats[bucket];
    stats->current++;
    if (stats->current > stats->watermark) {
        ASSERT((stats->current-1) ==stats->watermark);
        stats->watermark++;
    }
#ifdef STATS_HASHTABLE_COLLIDE
    if (isBusy) { stats->put++; }
#endif
#endif
    hal_unlock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
    return true;
}

void * hashtableConcBucketLockedTryPut(hashtable_t * hashtable, void * key, void * value) {
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
    hal_lock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
    ocr_hashtable_entry * entry = hashtableFindEntry(hashtable, key);
    if (entry == NULL) {
        hashtableNonConcPut(hashtable, key, value); // we already own the lock
#ifdef STATS_HASHTABLE
        hashtableBucketLockedStats_t * stats = &rhashtable->stats[bucket];
        stats->current++;
        if (stats->current > stats->watermark) {
            stats->watermark++;
        }
#endif
        hal_unlock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
        return value;
    } else {
        hal_unlock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
        return entry->value;
    }
}

bool hashtableConcBucketLockedRemove(hashtable_t * hashtable, void * key, void ** value) {
    hashtableBucketLocked_t * rhashtable = (hashtableBucketLocked_t *) hashtable;
    u32 bucket = hashtable->hashing(key, hashtable->nbBuckets);
#ifdef STATS_HASHTABLE_COLLIDE
    bool isBusy = hal_islocked(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
#endif
    hal_lock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
#ifdef STATS_HASHTABLE
    hashtableBucketLockedStats_t * stats = &rhashtable->stats[bucket];
    ASSERT(stats->current >= 1);
    stats->current--;
#ifdef STATS_HASHTABLE_COLLIDE
        if (isBusy) { stats->del++; }
#endif
#endif
    bool res = hashtableNonConcRemove(hashtable, key, value);
    //DPRINTF(DEBUG_LVL_WARN, "ht=%p For del key=%p: bucket=%"PRIu32" found value=%p *value=%p\n", hashtable, key, bucket, value, ((value) ? *value : NULL));
    hal_unlock(&(rhashtable->bucketLock[GET_LOCK_IDX(bucket)]));
    return res;
}


//
// Variants of the generic hashtable through hashing function specialization
//

/*
 * This implementation is meant to be used with monotonically generated guids.
 * Modulo more or less ensure buckets' load is balanced.
 */
static u32 hashModulo(void * ptr, u32 nbBuckets) {
    return (((unsigned long) ptr) % nbBuckets);
}

hashtable_t * newHashtableModulo(ocrPolicyDomain_t * pd, u32 nbBuckets) {
    return newHashtable(pd, nbBuckets, hashModulo);
}

hashtable_t * newHashtableBucketLockedModulo(ocrPolicyDomain_t * pd, u32 nbBuckets) {
    return newHashtableBucketLocked(pd, nbBuckets, hashModulo);
}
