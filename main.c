/*
 * main.c - Tiered Memory Manager Demo Program
 * 
 * This program demonstrates the tiered memory manager's capabilities:
 *   1. Large allocation that triggers management
 *   2. Page faults being intercepted and handled
 *   3. Access patterns being tracked
 *   4. Policy thread making migration decisions
 * 
 * Usage:
 *   1. Compile: make
 *   2. Run directly: ./tiered_manager
 *   3. Or with shim: LD_PRELOAD=./libmmap_shim.so ./tiered_manager
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

/* Global flag for clean shutdown */
static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/*
 * Simulate a workload with varying access patterns.
 * 
 * This creates:
 *   - Hot pages: Frequently accessed
 *   - Cold pages: Rarely accessed
 *   - Mixed access patterns for the ML model to learn
 */
static void simulate_workload(void *region, size_t size) {
    size_t num_pages = size / PAGE_SIZE;
    char *data = (char*)region;
    
    printf("\n[DEMO] Starting workload simulation with %zu pages\n", num_pages);
    
    /* Phase 1: Initial sequential access (all pages become warm) */
    printf("[DEMO] Phase 1: Sequential initialization...\n");
    for (size_t i = 0; i < num_pages && running; i++) {
        size_t offset = i * PAGE_SIZE;
        data[offset] = 'A';  /* Trigger page fault */
        
        if (i % 100 == 0 && i > 0) {
            printf("  Initialized %zu pages\n", i);
        }
    }
    
    if (!running) return;
    
    /* Phase 2: Create "hot" pages by repeated access */
    printf("[DEMO] Phase 2: Creating hot pages (first 10%%)...\n");
    size_t hot_pages = num_pages / 10;  /* First 10% are hot */
    
    for (int round = 0; round < 50 && running; round++) {
        for (size_t i = 0; i < hot_pages && running; i++) {
            size_t offset = i * PAGE_SIZE;
            /* Mix of reads and writes */
            if (round % 3 == 0) {
                data[offset]++;  /* Write */
            } else {
                volatile char c = data[offset];  /* Read */
                (void)c;
            }
        }
        usleep(10000);  /* 10ms between rounds */
    }
    
    if (!running) return;
    
    /* Phase 3: Random access pattern */
    printf("[DEMO] Phase 3: Random access pattern...\n");
    srand(time(NULL));
    
    for (int i = 0; i < 1000 && running; i++) {
        /* Biased random: 70% chance of accessing hot pages */
        size_t page_idx;
        if (rand() % 100 < 70) {
            page_idx = rand() % hot_pages;
        } else {
            page_idx = hot_pages + (rand() % (num_pages - hot_pages));
        }
        
        size_t offset = page_idx * PAGE_SIZE;
        data[offset] = (char)(i & 0xFF);
        
        usleep(1000);  /* 1ms between accesses */
    }
    
    printf("[DEMO] Workload simulation complete\n");
}

/*
 * Demo: Manual initialization (not using LD_PRELOAD shim)
 */
static int demo_manual_init(void) {
    printf("\n=== Tiered Memory Manager Demo ===\n\n");
    
    /* Initialize the manager */
    printf("[DEMO] Initializing tiered memory manager...\n");
    if (tiered_manager_init() < 0) {
        fprintf(stderr, "[DEMO] Failed to initialize manager\n");
        fprintf(stderr, "[DEMO] Note: This requires Linux with userfaultfd support\n");
        fprintf(stderr, "[DEMO] Check: /proc/sys/vm/unprivileged_userfaultfd = 1\n");
        return -1;
    }
    
    /* Allocate a large region for testing */
    /* 
     * Note: For the demo, we use a smaller allocation (16MB) that won't
     * trigger the 1GB threshold in the shim. This lets us test the
     * infrastructure without needing huge amounts of memory.
     */
    size_t test_size = 16 * 1024 * 1024;  /* 16 MB for demo */
    printf("[DEMO] Allocating test region: %zu bytes\n", test_size);
    
    void *region = mmap(NULL, test_size, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                        -1, 0);
    
    if (region == MAP_FAILED) {
        perror("mmap failed");
        tiered_manager_shutdown();
        return -1;
    }
    
    /* Register with userfaultfd manually (since shim won't see this) */
    printf("[DEMO] Registering region with userfaultfd...\n");
    if (register_managed_region(region, test_size) < 0) {
        fprintf(stderr, "[DEMO] Failed to register region\n");
        munmap(region, test_size);
        tiered_manager_shutdown();
        return -1;
    }
    
    /* Wait for threads to start */
    sleep(1);
    
    /* Print initial status */
    tiered_manager_print_status();
    
    /* Run the workload */
    simulate_workload(region, test_size);
    
    /* Let the policy thread process for a bit */
    printf("[DEMO] Letting policy thread run for 2 seconds...\n");
    sleep(2);
    
    /* Print final status */
    tiered_manager_print_status();
    
    /* Cleanup */
    printf("[DEMO] Cleaning up...\n");
    unregister_managed_region(region);
    munmap(region, test_size);
    tiered_manager_shutdown();
    
    return 0;
}

/*
 * Demo: Show how to integrate a custom ML policy
 */
static void demo_ml_integration(void) {
    printf("\n=== ML Integration Example ===\n\n");
    printf("To integrate your ML model, implement a function like:\n\n");
    printf("  bool my_ml_policy(const page_stats_t *stats,\n");
    printf("                    migration_decision_t *decision) {\n");
    printf("      // Available features in stats:\n");
    printf("      //   - stats->access_count (total accesses)\n");
    printf("      //   - stats->read_count, stats->write_count\n");
    printf("      //   - stats->heat_score (pre-computed 0.0-1.0)\n");
    printf("      //   - stats->access_rate (accesses/sec)\n");
    printf("      //   - stats->current_tier (TIER_DRAM or TIER_NVM)\n");
    printf("      //   - stats->migration_count\n");
    printf("      //   - timestamps: first_access_ns, last_access_ns\n");
    printf("      \n");
    printf("      // Run your model inference here\n");
    printf("      float prediction = ml_model_infer(stats);\n");
    printf("      \n");
    printf("      // Return true to trigger migration\n");
    printf("      if (prediction > threshold) {\n");
    printf("          decision->to_tier = TIER_DRAM;  // or TIER_NVM\n");
    printf("          decision->confidence = prediction;\n");
    printf("          return true;\n");
    printf("      }\n");
    printf("      return false;\n");
    printf("  }\n\n");
    printf("Then call: set_migration_policy(my_ml_policy);\n\n");
    printf("The predict_migration() function in policy_thread.c is the\n");
    printf("main integration point where you can add your model.\n");
    printf("================================\n\n");
}

int main(int argc, char *argv[]) {
    /* Set up signal handler for clean shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("LDOS Tiered Memory Manager - Lite Version\n");
    printf("NSF Research Project - UT Austin\n");
    printf("==========================================\n");
    
    /* Show ML integration instructions */
    demo_ml_integration();
    
    /* Check for command line options */
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [options]\n", argv[0]);
        printf("Options:\n");
        printf("  --help     Show this help\n");
        printf("  --shim     Run with LD_PRELOAD shim info\n");
        printf("\n");
        printf("To use with the shim:\n");
        printf("  LD_PRELOAD=./libmmap_shim.so ./your_program\n");
        return 0;
    }
    
    if (argc > 1 && strcmp(argv[1], "--shim") == 0) {
        printf("The LD_PRELOAD shim intercepts mmap calls for allocations > 1GB.\n");
        printf("To test with a real workload:\n");
        printf("  LD_PRELOAD=./libmmap_shim.so ./memory_intensive_app\n");
        return 0;
    }
    
    /* Run the demo */
    int result = demo_manual_init();
    
    if (result == 0) {
        printf("\n[DEMO] Demo completed successfully!\n");
        printf("[DEMO] Check the code comments for ML integration guidance.\n");
    }
    
    return result;
}
