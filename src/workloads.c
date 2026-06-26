/*
 * workloads.c - Synthetic Workload Implementations
 *
 * Each testcase creates a memory access pattern to exercise the tiered
 * memory manager and, importantly, to generate a *graded, time-varying*
 * access-rate signal that the per-page predictive signals can characterize.
 *
 * Design notes (phase-1 signal discovery):
 *   - Accesses are READ-dominant with periodic writes, so both the PEBS load
 *     and store events (and read_count/write_count) carry information.
 *   - Pages are split into intensity tiers (hot/warm/cold) that differ by
 *     ~order of magnitude in access frequency, so signals have a real range
 *     to discriminate -- not just binary hot/cold.
 *   - Loops are driven by wall-clock DURATION, not a fixed iteration count, so
 *     activity spans many 50 ms sampling windows regardless of CPU speed.
 *     Each page therefore accumulates dozens of bars of history (the indicators
 *     need ~25-52 bars before their lookbacks are meaningful).
 *   - The post-workload soak in main.c leaves all pages idle, which captures a
 *     clean hot->cold transition for the trend/cycle signals (PSAR, STC, ...).
 *
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
 * TUNABLES
 *===========================================================================*/

#define WL_ACTIVE_SECONDS   6       /* sustained active-access duration */
#define WL_PHASE_SECONDS    3       /* per-phase duration for temporal shift */

#define HOT_HITS_PER_SWEEP   64     /* accesses per hot page per sweep */
#define WARM_HITS_PER_SWEEP   8     /* accesses per warm page per sweep */
#define SEQ_HITS_PER_SWEEP    4     /* accesses per page in a sequential sweep */
#define COLD_STRIDE          37     /* touch every Nth cold page per sweep */
#define HOT_WRITE_EVERY       8     /* 1 write per N reads on hot pages */
#define SEQ_WRITE_EVERY       4     /* 1 write per N reads on sequential sweep */

/* Volatile sink prevents the compiler from optimizing the reads away. */
static volatile char g_workload_sink;

/*
 * Access one page `n_access` times.  Reads dominate (retire load uops, visible
 * to the PEBS ALL_LOADS event); a write is issued every `write_every` reads
 * (write_every == 0 disables writes -> read-only page).  Repeated touches to a
 * cache-resident line still retire memory uops, so PEBS sees the full volume
 * even though the data stays in L1.
 */
static inline void hammer_page(char *data, size_t page_idx,
                               int n_access, int write_every) {
    volatile char *p = (volatile char *)&data[page_idx * PAGE_SIZE];
    for (int k = 0; k < n_access; k++) {
        g_workload_sink = *p;                       /* read  (load uop)  */
        if (write_every && (k % write_every == 0))
            *p = (char)(g_workload_sink + 1);       /* write (store uop) */
    }
}

/* Fault every page in once so it is tracked before the access phase begins. */
static void prefault_region(char *data, size_t num_pages) {
    for (size_t i = 0; i < num_pages && running; i++) {
        data[i * PAGE_SIZE] = 'A';
    }
}

/*============================================================================
 * Hot/Cold Split (graded intensity)
 *
 * 10% hot pages, 20% warm pages, 70% cold pages, accessed at ~order-of-
 * magnitude-different frequencies for the full active duration.
 *
 * Expected behavior:
 *   - Hot pages: high, sustained access rate -> promoted to DRAM.
 *   - Warm pages: moderate rate.
 *   - Cold pages: near-idle -> remain in NVM.
 *===========================================================================*/

void testcase_hot_cold_split(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char *)region;

    size_t hot  = num_pages / 10;   /* 10% hot  */
    size_t warm = num_pages / 5;    /* 20% warm */

    printf("\n[TEST] Hot/Cold Split (graded: %zu hot, %zu warm, %zu cold pages)\n",
           hot, warm, num_pages - hot - warm);
    printf("[TEST] Prefaulting %zu pages...\n", num_pages);
    prefault_region(data, num_pages);

    printf("[TEST] Driving graded accesses for %ds...\n", WL_ACTIVE_SECONDS);
    uint64_t end = get_time_ns() + (uint64_t)WL_ACTIVE_SECONDS * 1000000000ULL;
    while (running && get_time_ns() < end) {
        for (size_t i = 0; i < hot; i++)
            hammer_page(data, i, HOT_HITS_PER_SWEEP, HOT_WRITE_EVERY);
        for (size_t i = hot; i < hot + warm; i++)
            hammer_page(data, i, WARM_HITS_PER_SWEEP, 0);   /* read-only warm */
        for (size_t i = hot + warm; i < num_pages; i += COLD_STRIDE)
            hammer_page(data, i, 1, 0);                     /* occasional cold */
    }
    printf("[TEST] Hot/Cold Split complete\n");
}

/*============================================================================
 * Sequential Scan (uniform, no persistent hot set)
 *
 * Repeatedly sweeps the whole region at uniform intensity for the active
 * duration.  No page stays hotter than any other.
 *
 * Expected behavior:
 *   - Trend/cycle signals should NOT flag a persistent trend on any page.
 *   - Anti-thrashing guard should limit tier ping-ponging.
 *===========================================================================*/

void testcase_sequential_scan(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char *)region;

    printf("\n[TEST] Sequential Scan (%zu pages, uniform, %ds)\n",
           num_pages, WL_ACTIVE_SECONDS);
    prefault_region(data, num_pages);

    uint64_t end = get_time_ns() + (uint64_t)WL_ACTIVE_SECONDS * 1000000000ULL;
    int iter = 0;
    while (running && get_time_ns() < end) {
        for (size_t i = 0; i < num_pages && running; i++)
            hammer_page(data, i, SEQ_HITS_PER_SWEEP, SEQ_WRITE_EVERY);
        iter++;
    }
    printf("[TEST] Sequential Scan complete (%d sweeps)\n", iter);
}

/*============================================================================
 * Temporal Shift (phase change)
 *
 * Phase 1: Region A (first 10% of pages) is hammered; Region B is idle.
 * Phase 2: Region B (next 10% of pages) is hammered; Region A goes cold.
 *
 * Expected behavior:
 *   - A's signals should show a clear hot->cold transition at the phase change
 *     (PSAR/Supertrend/STC flip), while B's show cold->hot.
 *===========================================================================*/

static void hammer_range(char *data, size_t lo, size_t hi, int seconds) {
    uint64_t end = get_time_ns() + (uint64_t)seconds * 1000000000ULL;
    while (running && get_time_ns() < end) {
        for (size_t i = lo; i < hi; i++)
            hammer_page(data, i, HOT_HITS_PER_SWEEP, HOT_WRITE_EVERY);
    }
}

void testcase_temporal_shift(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char *)region;
    size_t hot_pages = num_pages / 10; /* 10% per region */

    printf("\n[TEST] Temporal Shift (%zu pages, 2 phases x %ds)\n",
           num_pages, WL_PHASE_SECONDS);
    prefault_region(data, num_pages);

    printf("[TEST] Phase 1 - Region A (pages 0-%zu)\n", hot_pages - 1);
    hammer_range(data, 0, hot_pages, WL_PHASE_SECONDS);

    if (!running) return;

    printf("[TEST] Phase 2 - Region B (pages %zu-%zu), A goes cold\n",
           hot_pages, 2 * hot_pages - 1);
    hammer_range(data, hot_pages, 2 * hot_pages, WL_PHASE_SECONDS);

    printf("[TEST] Temporal Shift complete\n");
}
