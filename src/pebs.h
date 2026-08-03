/*
 * pebs.h - PEBS (Processor Event-Based Sampling) Interface
 *
 * Hardware-level memory access sampling using Intel PEBS.
 * Provides high-fidelity access counts for ML training.
 *
 * LDOS Research Project, UT Austin
 */

#ifndef PEBS_H
#define PEBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __linux__

/*============================================================================
 * CONFIGURATION
 *===========================================================================*/

#define PEBS_SAMPLE_PERIOD 5003          /* Sample every ~5K memory ops (prime).
                                          * Chosen so the 1MB ring buffer does not
                                          * throttle on the graded workloads (2003
                                          * caused ~2.5K throttle events, which
                                          * showed up as ~10x dips in hot-page rate);
                                          * still yields >250K samples per run. */
#define PEBS_BUFFER_PAGES (1 + (1 << 8)) /* 1MB ring buffer (must be 1+2^n) */

/* Intel PEBS event codes. */
#define PEBS_EVENT_MEM_LOADS 0x81d0  /* MEM_INST_RETIRED.ALL_LOADS (Skylake+) */
#define PEBS_EVENT_MEM_STORES 0x82d0 /* MEM_INST_RETIRED.ALL_STORES */
/* NOTE on the load event history:
 * - 0x81d0 addresses are GARBAGE on Haswell/Broadwell (constant 0xfeb8xxxx;
 *   verified with stock perf mem).  Those nodes are store-only regardless.
 * - The load-latency facility (0x01cd + ldlat in config1) was used as a
 *   Haswell workaround, but it samples TAGGED loads only -- roughly 600x
 *   fewer samples at the same period (303 vs 196K on comparable runs),
 *   which starved fast Ice Lake runs into empty datasets.
 * - On Skylake/Ice Lake, 0x81d0 gives full-rate sampling with correct
 *   DataLA -- verified on the r650 smoke test (20,936 distinct pages,
 *   reads spread across real heap addresses). */

/*============================================================================
 * DATA STRUCTURES
 *===========================================================================*/

typedef enum {
  PEBS_SAMPLE_READ = 0,
  PEBS_SAMPLE_WRITE = 1,
  PEBS_SAMPLE_TYPE_COUNT
} pebs_sample_type_t;

typedef struct pebs_page_record {
  uint64_t vaddr;          /* Page-aligned virtual address */
  uint64_t read_samples;   /* Number of read samples */
  uint64_t write_samples;  /* Number of write samples */
  uint64_t total_latency;  /* Sum of access latencies (from PEBS weight) */
  uint64_t last_sample_ns; /* Drain-time timestamp of most recent sample */

  /* Inter-access gap tracking from HARDWARE sample timestamps
   * (PERF_SAMPLE_TIME).  gap_ewma_ns is an exponential moving average of
   * the time between consecutive samples of this page.  Because samples
   * occur every ~PEBS_SAMPLE_PERIOD accesses, this is the true inter-access
   * interval scaled by a constant factor -- which z-scored reactivity
   * analysis is insensitive to.  Restores the leading inter-access signal
   * in telemetry mode (the previous lifetime span/count computation was
   * destroyed by 10ms merge quantization). */
  uint64_t last_sample_time_ns; /* hardware timestamp of previous sample */
  double gap_ewma_ns;           /* EWMA of inter-sample gaps */

  struct pebs_page_record *next; /* Hash chain */
} pebs_page_record_t;

typedef struct pebs_stats {
  uint64_t total_samples;
  uint64_t read_samples;
  uint64_t write_samples;
  uint64_t throttle_events;
  uint64_t errors;
  uint64_t lost_samples; /* PERF_RECORD_LOST: samples the KERNEL dropped */
  bool active;
} pebs_stats_t;

/*============================================================================
 * PUBLIC API
 *===========================================================================*/

/**
 * Initialize PEBS subsystem.
 * Sets up perf_event file descriptors and ring buffers.
 * Returns 0 on success, -1 on failure (PEBS unavailable).
 */
int pebs_init(void);

/**
 * Shutdown PEBS subsystem.
 * Stops collector thread and releases resources.
 */
void pebs_shutdown(void);

/**
 * Start PEBS sampling.
 * Begins collector thread if not already running.
 */
int pebs_start(void);

/**
 * Stop PEBS sampling.
 * Pauses collection without releasing resources.
 */
void pebs_stop(void);

/**
 * Check if PEBS is available and active.
 */
bool pebs_is_active(void);

/**
 * Get PEBS statistics for a page.
 * Returns NULL if page has no recorded samples.
 */
pebs_page_record_t *pebs_get_page_record(void *page_addr);

/**
 * Get global PEBS statistics.
 */
pebs_stats_t pebs_get_stats(void);

/**
 * Merge PEBS data into tiered memory page_stats.
 * Called by policy thread to combine hardware samples with
 * userfaultfd-based access tracking.
 */
void pebs_merge_with_page_stats(void);

/**
 * Clear all PEBS records (for testing or reset).
 */
void pebs_clear_records(void);

/**
 * Print PEBS status summary.
 */
void pebs_print_status(void);

/**
 * Debug: print the top-N sampled pages (vaddr + read/write sample counts).
 * Enabled at shutdown via LDOS_PEBS_DEBUG=1.
 */
void pebs_debug_dump_top(int topn);

#else /* !__linux__ */

/* Stub interface for non-Linux platforms */
static inline int pebs_init(void) { return -1; }
static inline void pebs_shutdown(void) {}
static inline int pebs_start(void) { return -1; }
static inline void pebs_stop(void) {}
static inline bool pebs_is_active(void) { return false; }
static inline void pebs_merge_with_page_stats(void) {}
static inline void pebs_clear_records(void) {}
static inline void pebs_print_status(void) {}
static inline void pebs_debug_dump_top(int topn) { (void)topn; }

#endif /* __linux__ */

#endif /* PEBS_H */
