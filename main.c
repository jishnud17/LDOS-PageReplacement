/*
 * main.c - Tiered Memory Manager Demo
 * 
 * Demonstrates the tiered memory manager:
 *   1. Large allocation triggers userfaultfd registration
 *   2. Page faults intercepted and handled with tier placement
 *   3. Access patterns tracked as ML features
 *   4. Policy thread makes migration decisions
 * 
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include "tiered_memory.h"

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/*============================================================================
 * WORKLOAD SIMULATION
 *===========================================================================*/

static void simulate_workload(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char*)region;
    
    printf("\n[DEMO] Simulating workload with %zu pages\n", num_pages);
    
    /* Phase 1: Sequential initialization */
    printf("[DEMO] Phase 1: Sequential init...\n");
    for (size_t i = 0; i < num_pages && running; i++) {
        data[i * PAGE_SIZE] = 'A';
        if (i % 100 == 0 && i > 0) printf("  %zu pages\n", i);
    }
    if (!running) return;
    
    /* Phase 2: Create hot pages (first 10%) */
    printf("[DEMO] Phase 2: Creating hot pages...\n");
    size_t hot_pages = num_pages / 10;
    for (int round = 0; round < 50 && running; round++) {
        for (size_t i = 0; i < hot_pages && running; i++) {
            if (round % 3 == 0) data[i * PAGE_SIZE]++;
            else { volatile char c = data[i * PAGE_SIZE]; (void)c; }
        }
        usleep(10000);
    }
    if (!running) return;
    
    /* Phase 3: Random access (biased toward hot pages) */
    printf("[DEMO] Phase 3: Random access...\n");
    srand(time(NULL));
    for (int i = 0; i < 1000 && running; i++) {
        size_t idx = (rand() % 100 < 70) ? rand() % hot_pages 
                                          : hot_pages + rand() % (num_pages - hot_pages);
        data[idx * PAGE_SIZE] = (char)i;
        usleep(1000);
    }
    
    printf("[DEMO] Workload complete\n");
}

/*============================================================================
 * DEMO
 *===========================================================================*/

static int demo_manual_init(void) {
    printf("\n=== Tiered Memory Manager Demo ===\n\n");
    
    printf("[DEMO] Initializing manager...\n");
    if (tiered_manager_init() < 0) {
        fprintf(stderr, "[DEMO] Init failed\n");
        fprintf(stderr, "[DEMO] Check: /proc/sys/vm/unprivileged_userfaultfd = 1\n");
        return -1;
    }
    
    /* 16MB test region (smaller than 1GB threshold for manual testing) */
    size_t test_size = 16 * 1024 * 1024;
    printf("[DEMO] Allocating %zu bytes...\n", test_size);
    
    void *region = mmap(NULL, test_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        tiered_manager_shutdown();
        return -1;
    }
    
    printf("[DEMO] Registering with userfaultfd...\n");
    if (register_managed_region(region, test_size) < 0) {
        fprintf(stderr, "[DEMO] Registration failed\n");
        munmap(region, test_size);
        tiered_manager_shutdown();
        return -1;
    }
    
    sleep(1);
    tiered_manager_print_status();
    
    simulate_workload(region, test_size);
    
    printf("[DEMO] Running policy thread for 2s...\n");
    sleep(2);
    
    tiered_manager_print_status();
    
    printf("[DEMO] Cleanup...\n");
    unregister_managed_region(region);
    munmap(region, test_size);
    tiered_manager_shutdown();
    
    return 0;
}

static void print_ml_integration_info(void) {
    printf("\n=== ML Integration ===\n\n");
    printf("Implement migration_policy_fn and call set_migration_policy().\n");
    printf("See predict_migration() in policy_thread.c.\n\n");
    printf("Features available in page_stats_t:\n");
    printf("  - access_count, read_count, write_count\n");
    printf("  - heat_score (0.0-1.0), access_rate\n");
    printf("  - current_tier, migration_count\n");
    printf("  - first_access_ns, last_access_ns\n");
    printf("======================\n\n");
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("LDOS Tiered Memory Manager\n");
    printf("UT Austin - NSF Research\n");
    printf("==========================\n");
    
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [--help | --shim]\n\n", argv[0]);
        printf("Run demo: ./tiered_manager\n");
        printf("Use shim: LD_PRELOAD=./libmmap_shim.so ./your_app\n");
        return 0;
    }
    
    if (argc > 1 && strcmp(argv[1], "--shim") == 0) {
        printf("Shim intercepts mmap > 1GB.\n");
        printf("Usage: LD_PRELOAD=./libmmap_shim.so ./your_app\n");
        return 0;
    }
    
    print_ml_integration_info();
    
    int result = demo_manual_init();
    if (result == 0) {
        printf("\n[DEMO] Success!\n");
    }
    
    return result;
}
