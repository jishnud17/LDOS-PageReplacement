/*
 * tiered_memory.c - Core Manager Implementation
 *
 * Initialization, shutdown, and tier configuration for the
 * tiered memory manager (4GB DRAM + 16GB NVM simulated).
 *
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include "tiered_memory.h"
#include "pebs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

tiered_manager_t g_manager = {0};

/* External module functions */
extern int init_userfaultfd(void);
extern int start_uffd_handler(void);
extern void stop_uffd_handler(void);
extern void cleanup_userfaultfd(void);
extern int start_policy_thread(void);
extern void stop_policy_thread(void);

/*============================================================================
 * TIER INITIALIZATION
 *===========================================================================*/

static int init_memory_tiers(void) {
  /* DRAM tier: 4GB, ~80ns read latency */
  tier_config_t *dram = &g_manager.tiers[TIER_DRAM];
  dram->name = "DRAM";
  dram->capacity = 4UL * 1024 * 1024 * 1024;
  dram->used = 0;
  dram->read_latency_ns = 80;
  dram->write_latency_ns = 100;
  dram->backing_memory = NULL;

  /* NVM tier: 16GB, ~300ns read latency */
  tier_config_t *nvm = &g_manager.tiers[TIER_NVM];
  nvm->name = "NVM";
  nvm->capacity = 16UL * 1024 * 1024 * 1024;
  nvm->used = 0;
  nvm->read_latency_ns = 300;
  nvm->write_latency_ns = 500;
  nvm->backing_memory = NULL;

  TM_INFO("Initialized tiers: DRAM=%luGB, NVM=%luGB",
          dram->capacity / (1024 * 1024 * 1024),
          nvm->capacity / (1024 * 1024 * 1024));

  return 0;
}

/*============================================================================
 * MANAGER LIFECYCLE
 *===========================================================================*/

int tiered_manager_init(void) {
  if (g_manager.initialized) {
    TM_DEBUG("Manager already initialized");
    return 0;
  }

  TM_INFO("Initializing tiered memory manager...");

  /* Initialize synchronization primitives */
  if (pthread_mutex_init(&g_manager.regions_lock, NULL) != 0 ||
      pthread_rwlock_init(&g_manager.stats_lock, NULL) != 0 ||
      pthread_mutex_init(&g_manager.migration_lock, NULL) != 0 ||
      pthread_cond_init(&g_manager.migration_cond, NULL) != 0) {
    TM_ERROR("Failed to initialize synchronization primitives");
    return -1;
  }

  /* Initialize state */
  memset(g_manager.page_stats_table, 0, sizeof(g_manager.page_stats_table));
  memset(g_manager.regions, 0, sizeof(g_manager.regions));
  g_manager.region_count = 0;
  atomic_store(&g_manager.total_pages_tracked, 0);
  atomic_store(&g_manager.total_faults, 0);
  atomic_store(&g_manager.total_migrations, 0);
  atomic_store(&g_manager.policy_cycles, 0);

  if (init_memory_tiers() < 0) {
    TM_ERROR("Failed to initialize memory tiers");
    goto cleanup;
  }

  if (init_userfaultfd() < 0) {
    TM_ERROR("Failed to initialize userfaultfd");
    TM_ERROR("Ensure: /proc/sys/vm/unprivileged_userfaultfd = 1");
    goto cleanup;
  }

  /* Initialize PEBS (optional - continues without it) */
  if (pebs_init() == 0) {
    pebs_start();
    TM_INFO("PEBS hardware sampling enabled");
  } else {
    TM_INFO("PEBS unavailable - using userfaultfd only");
  }

  /* Start background threads */
  g_manager.threads_running = true;

  if (start_uffd_handler() < 0 || start_policy_thread() < 0) {
    TM_ERROR("Failed to start background threads");
    g_manager.threads_running = false;
    pebs_shutdown();
    cleanup_userfaultfd();
    goto cleanup;
  }

  g_manager.initialized = true;
  TM_INFO("Tiered memory manager initialized successfully");
  return 0;

cleanup:
  pthread_cond_destroy(&g_manager.migration_cond);
  pthread_mutex_destroy(&g_manager.migration_lock);
  pthread_rwlock_destroy(&g_manager.stats_lock);
  pthread_mutex_destroy(&g_manager.regions_lock);
  return -1;
}

void tiered_manager_shutdown(void) {
  if (!g_manager.initialized)
    return;

  TM_INFO("Shutting down tiered memory manager...");

  g_manager.threads_running = false;
  stop_policy_thread();
  stop_uffd_handler();
  pebs_shutdown();

  /* Final statistics */
  TM_INFO("Final stats: faults=%" PRIu64 ", migrations=%" PRIu64
          ", cycles=%" PRIu64,
          (uint64_t)atomic_load(&g_manager.total_faults),
          (uint64_t)atomic_load(&g_manager.total_migrations),
          (uint64_t)atomic_load(&g_manager.policy_cycles));

  cleanup_userfaultfd();
  cleanup_page_stats();

  pthread_cond_destroy(&g_manager.migration_cond);
  pthread_mutex_destroy(&g_manager.migration_lock);
  pthread_rwlock_destroy(&g_manager.stats_lock);
  pthread_mutex_destroy(&g_manager.regions_lock);

  g_manager.initialized = false;
  TM_INFO("Shutdown complete");
}

void tiered_manager_print_status(void) {
  if (!g_manager.initialized) {
    printf("Tiered memory manager not initialized\n");
    return;
  }

  printf("\n=== Tiered Memory Manager Status ===\n");
  printf("Faults: %" PRIu64 "  Migrations: %" PRIu64 "  Cycles: %" PRIu64
         "  Pages: %" PRIu64 "\n",
         (uint64_t)atomic_load(&g_manager.total_faults),
         (uint64_t)atomic_load(&g_manager.total_migrations),
         (uint64_t)atomic_load(&g_manager.policy_cycles),
         (uint64_t)atomic_load(&g_manager.total_pages_tracked));

  printf("\nTiers:\n");
  for (int t = 1; t < TIER_COUNT; t++) {
    tier_config_t *tier = &g_manager.tiers[t];
    printf("  %s: %lu/%lu bytes (%.1f%%)\n", tier->name, tier->used,
           tier->capacity,
           tier->capacity > 0 ? 100.0 * tier->used / tier->capacity : 0);
  }

  printf("\nManaged Regions: %d\n", g_manager.region_count);
  pthread_mutex_lock(&g_manager.regions_lock);
  for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
    if (g_manager.regions[i].active) {
      managed_region_t *r = &g_manager.regions[i];
      printf("  [%d] %p + %zu bytes\n", i, r->base_addr, r->length);
    }
  }
  pthread_mutex_unlock(&g_manager.regions_lock);

  /* PEBS status */
  if (pebs_is_active()) {
    printf("\n");
    pebs_print_status();
  }

  printf("====================================\n\n");
}
