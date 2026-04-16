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
#include "workloads.h"

volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/*============================================================================
 * DEMO
 *===========================================================================*/

static int demo_manual_init(const char *testcase_name) {
    printf("\n=== Tiered Memory Manager Demo ===\n\n");
    
    /* Check if shim already initialized the manager (LD_PRELOAD case) */
    int shim_mode = g_manager.initialized;
    
    if (shim_mode) {
        printf("[DEMO] Manager already initialized by shim\n");
    } else {
        printf("[DEMO] Initializing manager...\n");
        if (tiered_manager_init() < 0) {
            fprintf(stderr, "[DEMO] Init failed\n");
            fprintf(stderr, "[DEMO] Check: /proc/sys/vm/unprivileged_userfaultfd = 1\n");
            return -1;
        }
    }
    
    /* 16MB test region (smaller than 1GB threshold for manual testing) */
    size_t test_size = 16 * 1024 * 1024;
    printf("[DEMO] Allocating %zu bytes...\n", test_size);
    
    void *region = mmap(NULL, test_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        if (!shim_mode) tiered_manager_shutdown();
        return -1;
    }
    
    printf("[DEMO] Registering with userfaultfd...\n");
    if (register_managed_region(region, test_size) < 0) {
        fprintf(stderr, "[DEMO] Registration failed\n");
        munmap(region, test_size);
        if (!shim_mode) tiered_manager_shutdown();
        return -1;
    }
    
    sleep(1);
    tiered_manager_print_status();
    
    if (strcmp(testcase_name, "hot_cold") == 0) {
        testcase_hot_cold_split(region, test_size);
    } else if (strcmp(testcase_name, "sequential") == 0) {
        testcase_sequential_scan(region, test_size);
    } else if (strcmp(testcase_name, "temporal") == 0) {
        testcase_temporal_shift(region, test_size);
    } else {
        printf("[DEMO] Unknown testcase '%s', falling back to hot_cold\n", testcase_name);
        testcase_hot_cold_split(region, test_size);
    }
    
    printf("[DEMO] Running policy thread for 5s (soak)...\n");
    sleep(5);
    
    tiered_manager_print_status();
    
    printf("[DEMO] Cleanup...\n");
    unregister_managed_region(region);
    munmap(region, test_size);
    
    /* Only shutdown if we initialized (shim handles its own shutdown) */
    if (!shim_mode) {
        tiered_manager_shutdown();
    }
    
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
    
    const char *testcase_name = "hot_cold"; /* Default testcase */
    unsigned int seed = 42;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--help | --shim | --test=<name>]\n\n", argv[0]);
            printf("Run demo: ./tiered_manager [--test=hot_cold|sequential|temporal]\n");
            printf("Use shim: LD_PRELOAD=./libmmap_shim.so ./your_app\n");
            return 0;
        } else if (strcmp(argv[i], "--shim") == 0) {
            printf("Shim intercepts mmap > 1GB.\n");
            printf("Usage: LD_PRELOAD=./libmmap_shim.so ./your_app\n");
            return 0;
        } else if (strncmp(argv[i], "--test=", 7) == 0) {
            testcase_name = argv[i] + 7;
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            seed = (unsigned int)atoi(argv[i] + 7);
        }
    }
    
    srand(seed);
    printf("[DEMO] Seed: %u, Testcase: %s\n", seed, testcase_name);

    print_ml_integration_info();
    set_csv_label(testcase_name);
    
    int result = demo_manual_init(testcase_name);
    if (result == 0) {
        printf("\n[DEMO] Success!\n");
    }
    
    return result;
}
