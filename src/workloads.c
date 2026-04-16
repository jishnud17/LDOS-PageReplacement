/*
 * workloads.c - Synthetic Workload Implementations
 *
 * Each testcase creates a deterministic memory access pattern
 * to benchmark the tiered memory migration policy.
 * All functions respect the global `running` flag for clean shutdown.
 *
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include "workloads.h"
#include "tiered_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*============================================================================
 * Hot/Cold Split (80/20 Rule)
 *
 * 20% of the memory region receives 80% of all read/write accesses.
 * The remaining 80% of pages are touched infrequently.
 *
 * Expected behavior:
 *   - The 20% hot pages should be promoted to DRAM.
 *   - The 80% cold pages should remain in (or be demoted to) NVM.
 *===========================================================================*/

void testcase_hot_cold_split(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char*)region;

    printf("\n[TEST] Hot/Cold Split (20%% hot, 80%% cold, %zu pages)\n", num_pages);
    size_t hot_pages = num_pages / 5; /* 20% */

    /* Initialize all pages (triggers faults) */
    printf("[TEST] Initializing %zu pages...\n", num_pages);
    for (size_t i = 0; i < num_pages && running; i++) {
        data[i * PAGE_SIZE] = 'A';
    }

    /* Access loop: 80% of accesses go to 20% of pages */
    printf("[TEST] Running 2000 accesses (80/20 skew)...\n");
    for (int i = 0; i < 2000 && running; i++) {
        size_t idx;
        if (rand() % 100 < 80) {
            idx = rand() % hot_pages;
        } else {
            idx = hot_pages + (rand() % (num_pages - hot_pages));
        }
        data[idx * PAGE_SIZE] = (char)(i % 256);
        usleep(1000); /* 1ms between accesses */
    }
    printf("[TEST] Hot/Cold Split complete\n");
}

/*============================================================================
 * Sequential Scan (Thrashing Stress Test)
 *
 * Sweeps through the entire memory region from start to end,
 * repeated 5 times. Every page gets equal attention.
 *
 * Expected behavior:
 *   - No single page should stay "hot" for long.
 *   - Anti-thrashing guard (min_residence_ns) should prevent
 *     excessive ping-ponging between tiers.
 *===========================================================================*/

void testcase_sequential_scan(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char*)region;

    printf("\n[TEST] Sequential Scan (%zu pages, 5 iterations)\n", num_pages);

    for (int iter = 0; iter < 5 && running; iter++) {
        printf("[TEST] Scan iteration %d/5...\n", iter + 1);
        for (size_t i = 0; i < num_pages && running; i++) {
            data[i * PAGE_SIZE] = (char)(iter % 256);
            usleep(100); /* 0.1ms between accesses */
        }
    }
    printf("[TEST] Sequential Scan complete\n");
}

/*============================================================================
 * Temporal Shift (Phase Change)
 *
 * Phase 1 (~2s): Region A (first 10% of pages) is heavily accessed.
 * Phase 2 (~2s): Region B (next 10% of pages) is heavily accessed.
 *                Region A goes completely cold.
 *
 * Expected behavior:
 *   - During Phase 1, Region A pages should be promoted to DRAM.
 *   - After the shift, Region A's heat should decay.
 *   - Region B pages should be promoted during Phase 2.
 *===========================================================================*/

void testcase_temporal_shift(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char*)region;

    printf("\n[TEST] Temporal Shift (%zu pages, 2 phases)\n", num_pages);
    size_t hot_pages = num_pages / 10; /* 10% per region */

    /* Initialize all pages */
    printf("[TEST] Initializing %zu pages...\n", num_pages);
    for (size_t i = 0; i < num_pages && running; i++) {
        data[i * PAGE_SIZE] = 'A';
    }

    /* Phase 1: Hammer Region A */
    printf("[TEST] Phase 1 - Region A (pages 0-%zu)\n", hot_pages - 1);
    for (int i = 0; i < 1000 && running; i++) {
        size_t idx = rand() % hot_pages;
        data[idx * PAGE_SIZE] = (char)(i % 256);
        usleep(2000); /* 2ms between accesses */
    }

    if (!running) return;

    /* Phase 2: Shift to Region B, Region A goes cold */
    printf("[TEST] Phase 2 - Region B (pages %zu-%zu)\n", hot_pages, 2 * hot_pages - 1);
    for (int i = 0; i < 1000 && running; i++) {
        size_t idx = hot_pages + (rand() % hot_pages);
        data[idx * PAGE_SIZE] = (char)(i % 256);
        usleep(2000); /* 2ms between accesses */
    }
    printf("[TEST] Temporal Shift complete\n");
}
