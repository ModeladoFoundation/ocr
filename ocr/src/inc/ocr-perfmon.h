/**
 * @cond IGNORED_FILES
 *
 * This file is ignored by doxygen
 */
/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifdef ENABLE_EXTENSION_PERF

#ifndef __OCR_PERFSTAT_H__
#define __OCR_PERFSTAT_H__

typedef struct _variance_t {
    double var_m;
    double var_s;
} variance_t;

typedef struct _ocrPerfStat {
    u64 average;
    variance_t var;
} ocrPerfStat_t;

typedef enum _perfEvents {
    PERF_HW_CYCLES,           // Total no. of HW cycles taken to execute the EDT
    PERF_L1_HITS,             // Total hits to the cache (varies by architecture)
    PERF_L1_MISSES,           // Total hits to the memory (varies by architecture)
    PERF_FLOAT_OPS,           // Total arithmetic floating point ops
    PERF_HW_MAX,
    /* Software events below */
    PERF_EDT_CREATES = PERF_HW_MAX,  // No. of EDTs created by this EDT
    PERF_DB_TOTAL,                   // Total size of all DBs acquired by this EDT
    PERF_DB_CREATES,                 // Total size of all DBs created by this EDT
    PERF_DB_DESTROYS,                // Total size of all DBs destroyed by this EDT
    PERF_EVT_SATISFIES,              // Total no. of events satisfied by this EDT
    PERF_MAX
} perfEvents;

#ifndef STEADY_STATE_COUNT
#define STEADY_STATE_COUNT 1000000    // How many events are needed to start checking for
#endif                                // steady state conditions; currently very high to
                                      // prevent it from settling down
#ifndef STEADY_STATE_SHIFT
#define STEADY_STATE_SHIFT  4         // Absolute difference is < 6.25% (1/1<<4)
#endif

typedef struct _ocrPerfCounters {
    void *edt;                        // EDT identified by its function pointer
    ocrPerfStat_t stats[PERF_MAX];    // Performance statistics for each counter
    u32 count;                        // No of samples
    u32 steadyStateMask;              // Mask indicating which counters haven't reached 'steady state'
} ocrPerfCounters_t;


#endif /* __OCR_PERFSTAT_H__ */

#endif /* ENABLE_EXTENSION_PERF */
