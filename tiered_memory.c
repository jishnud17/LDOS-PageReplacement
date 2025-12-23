/*
 * tiered_memory.c - Core Manager Implementation
 * 
 * This file implements the initialization, shutdown, and coordination
 * logic for the tiered memory manager.
 * 
 * It ties together:
 *   - Simulated DRAM and NVM tier configuration
 *   - Userfaultfd initialization and handler thread
 *   - Policy/predictor thread
 *   - Page statistics management
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include "tiered_memory.h"

/* Global manager instance */
tiered_manager_t g_manager = {0};

/* Forward declarations from other modules */
extern int init_userfaultfd(void);
extern int start_uffd_handler(void);
extern void stop_uffd_handler(void);
extern void cleanup_userfaultfd(void);
extern int start_policy_thread(void);
extern void stop_policy_thread(void);
extern void cleanup_page_stats(void);
extern void update_all_page_features(void);
extern void print_page_stats_summary(void);

/*============================================================================
 * TIER INITIALIZATION
 *===========================================================================*/

/*
 * Initialize simulated memory tiers.
 * 
 * In this simulation:
 *   - "DRAM" tier: 4GB of fast memory
 *   - "NVM" tier: 16GB of slow memory
 * 
 * For production with real hardware:
 *   - DRAM tier would use regular system memory
 *   - NVM tier would use CXL/NVM devices via DAX or mmap
 */
static int init_memory_tiers(void) {
    /* DRAM tier configuration */
    tier_config_t *dram = &g_manager.tiers[TIER_DRAM];
    dram->name = "DRAM";
    dram->capacity = 4UL * 1024 * 1024 * 1024;  /* 4 GB */
    dram->used = 0;
    dram->read_latency_ns = 80;   /* ~80ns for DRAM */
    dram->write_latency_ns = 100;
    dram->backing_memory = NULL;  /* We don't actually allocate backing memory in simulation */
    
    /* NVM tier configuration */
    tier_config_t *nvm = &g_manager.tiers[TIER_NVM];
    nvm->name = "NVM";
    nvm->capacity = 16UL * 1024 * 1024 * 1024;  /* 16 GB */
    nvm->used = 0;
    nvm->read_latency_ns = 300;   /* ~300ns for NVM/CXL */
    nvm->write_latency_ns = 500;
    nvm->backing_memory = NULL;
    
    TM_INFO("Initialized tiers: DRAM=%luGB, NVM=%luGB",
            dram->capacity / (1024*1024*1024),
            nvm->capacity / (1024*1024*1024));
    
    return 0;
}

/*============================================================================
 * MANAGER LIFECYCLE
 *===========================================================================*/

/*
 * Initialize the tiered memory manager.
 * 
 * This sets up:
 *   - Memory tier configurations
 *   - Userfaultfd subsystem
 *   - Background threads (fault handler, policy)
 *   - Page statistics tracking
 */
int tiered_manager_init(void) {
    if (g_manager.initialized) {
        TM_DEBUG("Manager already initialized");
        return 0;
    }
    
    TM_INFO("Initializing tiered memory manager...");
    
    /* Initialize synchronization primitives */
    if (pthread_mutex_init(&g_manager.regions_lock, NULL) != 0) {
        TM_ERROR("Failed to init regions_lock");
        return -1;
    }
    
    if (pthread_rwlock_init(&g_manager.stats_lock, NULL) != 0) {
        TM_ERROR("Failed to init stats_lock");
        pthread_mutex_destroy(&g_manager.regions_lock);
        return -1;
    }
    
    if (pthread_mutex_init(&g_manager.migration_lock, NULL) != 0) {
        TM_ERROR("Failed to init migration_lock");
        pthread_rwlock_destroy(&g_manager.stats_lock);
        pthread_mutex_destroy(&g_manager.regions_lock);
        return -1;
    }
    
    if (pthread_cond_init(&g_manager.migration_cond, NULL) != 0) {
        TM_ERROR("Failed to init migration_cond");
        pthread_mutex_destroy(&g_manager.migration_lock);
        pthread_rwlock_destroy(&g_manager.stats_lock);
        pthread_mutex_destroy(&g_manager.regions_lock);
        return -1;
    }
    
    /* Initialize page stats hash table */
    memset(g_manager.page_stats_table, 0, sizeof(g_manager.page_stats_table));
    atomic_store(&g_manager.total_pages_tracked, 0);
    
    /* Initialize managed regions array */
    memset(g_manager.regions, 0, sizeof(g_manager.regions));
    g_manager.region_count = 0;
    
    /* Initialize global statistics */
    atomic_store(&g_manager.total_faults, 0);
    atomic_store(&g_manager.total_migrations, 0);
    atomic_store(&g_manager.policy_cycles, 0);
    
    /* Initialize memory tiers */
    if (init_memory_tiers() < 0) {
        TM_ERROR("Failed to initialize memory tiers");
        goto cleanup_locks;
    }
    
    /* Initialize userfaultfd */
    if (init_userfaultfd() < 0) {
        TM_ERROR("Failed to initialize userfaultfd");
        goto cleanup_locks;
    }
    
    /* Start background threads */
    g_manager.threads_running = true;
    
    if (start_uffd_handler() < 0) {
        TM_ERROR("Failed to start UFFD handler thread");
        goto cleanup_uffd;
    }
    
    if (start_policy_thread() < 0) {
        TM_ERROR("Failed to start policy thread");
        goto cleanup_uffd_thread;
    }
    
    g_manager.initialized = true;
    
    TM_INFO("Tiered memory manager initialized successfully");
    return 0;
    
cleanup_uffd_thread:
    g_manager.threads_running = false;
    stop_uffd_handler();
    
cleanup_uffd:
    cleanup_userfaultfd();
    
cleanup_locks:
    pthread_cond_destroy(&g_manager.migration_cond);
    pthread_mutex_destroy(&g_manager.migration_lock);
    pthread_rwlock_destroy(&g_manager.stats_lock);
    pthread_mutex_destroy(&g_manager.regions_lock);
    
    return -1;
}

/*
 * Shutdown the tiered memory manager.
 * 
 * Stops all threads and cleans up resources.
 */
void tiered_manager_shutdown(void) {
    if (!g_manager.initialized) {
        return;
    }
    
    TM_INFO("Shutting down tiered memory manager...");
    
    /* Signal threads to stop */
    g_manager.threads_running = false;
    
    /* Wait for threads to exit */
    stop_policy_thread();
    stop_uffd_handler();
    
    /* Print final statistics */
    TM_INFO("Final statistics:");
    TM_INFO("  Total page faults: %" PRIu64, (uint64_t)atomic_load(&g_manager.total_faults));
    TM_INFO("  Total migrations: %" PRIu64, (uint64_t)atomic_load(&g_manager.total_migrations));
    TM_INFO("  Policy cycles: %" PRIu64, (uint64_t)atomic_load(&g_manager.policy_cycles));
    TM_INFO("  Pages tracked: %" PRIu64, (uint64_t)atomic_load(&g_manager.total_pages_tracked));
    print_page_stats_summary();
    
    /* Clean up userfaultfd */
    cleanup_userfaultfd();
    
    /* Clean up page stats */
    cleanup_page_stats();
    
    /* Destroy synchronization primitives */
    pthread_cond_destroy(&g_manager.migration_cond);
    pthread_mutex_destroy(&g_manager.migration_lock);
    pthread_rwlock_destroy(&g_manager.stats_lock);
    pthread_mutex_destroy(&g_manager.regions_lock);
    
    g_manager.initialized = false;
    
    TM_INFO("Tiered memory manager shutdown complete");
}

/*
 * Print current manager status.
 */
void tiered_manager_print_status(void) {
    if (!g_manager.initialized) {
        printf("Tiered memory manager not initialized\n");
        return;
    }
    
    printf("\n=== Tiered Memory Manager Status ===\n");
    printf("Page faults handled: %" PRIu64 "\n", (uint64_t)atomic_load(&g_manager.total_faults));
    printf("Migrations performed: %" PRIu64 "\n", (uint64_t)atomic_load(&g_manager.total_migrations));
    printf("Policy cycles completed: %" PRIu64 "\n", (uint64_t)atomic_load(&g_manager.policy_cycles));
    printf("Pages being tracked: %" PRIu64 "\n", (uint64_t)atomic_load(&g_manager.total_pages_tracked));
    
    printf("\nTier Status:\n");
    for (int t = 1; t < TIER_COUNT; t++) {
        tier_config_t *tier = &g_manager.tiers[t];
        double used_pct = tier->capacity > 0 ? 
            100.0 * tier->used / tier->capacity : 0;
        printf("  %s: %lu/%lu bytes (%.1f%%)\n",
               tier->name, tier->used, tier->capacity, used_pct);
    }
    
    printf("\nManaged Regions: %d\n", g_manager.region_count);
    pthread_mutex_lock(&g_manager.regions_lock);
    for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
        if (g_manager.regions[i].active) {
            managed_region_t *r = &g_manager.regions[i];
            printf("  Region %d: %p + %zu bytes, faults=%" PRIu64 ", DRAM=%" PRIu64 ", NVM=%" PRIu64 "\n",
                   i, r->base_addr, r->length,
                   (uint64_t)atomic_load(&r->total_faults),
                   (uint64_t)atomic_load(&r->pages_in_dram),
                   (uint64_t)atomic_load(&r->pages_in_nvm));
        }
    }
    pthread_mutex_unlock(&g_manager.regions_lock);
    
    printf("====================================\n\n");
}
