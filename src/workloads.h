/*
 * workloads.h - Synthetic Workload Definitions
 *
 * Deterministic memory access patterns for benchmarking
 * the tiered memory manager's migration policy.
 *
 * LDOS Research Project, UT Austin
 */

#ifndef WORKLOADS_H
#define WORKLOADS_H

#include <stddef.h>

/* Shared stop flag (set by signal handler in main.c) */
extern volatile int running;

/*
 * Hot/Cold Split (80/20 Rule)
 *   20% of pages receive 80% of accesses.
 *   Validates: promotion of hot pages to DRAM.
 */
void testcase_hot_cold_split(void *region, size_t size);

/*
 * Sequential Scan (Thrashing Stress Test)
 *   Sweeps through entire region repeatedly.
 *   Validates: anti-thrashing / min_residence_ns guard.
 */
void testcase_sequential_scan(void *region, size_t size);

/*
 * Temporal Shift (Phase Change)
 *   Phase 1: Region A is hot.  Phase 2: Region B is hot.
 *   Validates: responsiveness of heat decay and re-promotion.
 */
void testcase_temporal_shift(void *region, size_t size);

#endif /* WORKLOADS_H */
