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

#define PEBS_SAMPLE_PERIOD 100007        /* Samples every ~100K memory ops */
#define PEBS_BUFFER_PAGES (1 + (1 << 8)) /* 1MB ring buffer (must be 1+2^n) */

/* Intel PEBS event codes */
#define PEBS_EVENT_MEM_LOADS 0x80d1  /* MEM_LOAD_RETIRED.ALL_LOADS */
#define PEBS_EVENT_MEM_STORES 0x82d0 /* MEM_INST_RETIRED.ALL_STORES */

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
  uint64_t last_sample_ns; /* Timestamp of most recent sample */
  struct pebs_page_record *next; /* Hash chain */
} pebs_page_record_t;

typedef struct pebs_stats {
  uint64_t total_samples;
  uint64_t read_samples;
  uint64_t write_samples;
  uint64_t throttle_events;
  uint64_t errors;
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

#endif /* __linux__ */

#endif /* PEBS_H */
