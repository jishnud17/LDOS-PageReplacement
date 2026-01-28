/*
 * policy_thread.c - Migration Policy Thread
 *
 * Background thread that wakes every 10ms to:
 *   1. Update page features (heat scores, access rates)
 *   2. Run migration policy (heuristic or ML-based)
 *   3. Execute tier migrations for hot/cold pages
 *
 * ML Integration Point: predict_migration() and set_migration_policy()
 *
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include "pebs.h"
#include "tiered_memory.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

migration_policy_fn g_migration_policy = NULL;

static void *policy_thread_loop(void *arg);

/*============================================================================
 * POLICY CONFIGURATION
 *===========================================================================*/

typedef struct policy_config {
  double hot_threshold;  /* Heat > this -> promote */
  double cold_threshold; /* Heat < this -> demote */
  double confidence_min;
  uint64_t min_residence_ns; /* Anti-thrashing: min time before migration */
  uint32_t max_migrations_per_cycle;
} policy_config_t;

static policy_config_t g_policy_config = {.hot_threshold = 0.7,
                                          .cold_threshold = 0.3,
                                          .confidence_min = 0.5,
                                          .min_residence_ns =
                                              100000000, /* 100ms */
                                          .max_migrations_per_cycle = 10};

/*============================================================================
 * DEFAULT HEURISTIC POLICY
 *===========================================================================*/

bool default_heuristic_policy(const page_stats_t *stats,
                              migration_decision_t *decision) {
  if (stats == NULL || decision == NULL)
    return false;

  uint64_t now = get_time_ns();

  /* Anti-thrashing: don't migrate recently migrated pages */
  if (stats->last_migration_ns > 0) {
    if (now - stats->last_migration_ns < g_policy_config.min_residence_ns) {
      return false;
    }
  }

  decision->page_addr = stats->page_addr;
  decision->from_tier = stats->current_tier;

  /* Hot page in NVM -> promote to DRAM */
  if (stats->current_tier == TIER_NVM &&
      stats->heat_score > g_policy_config.hot_threshold) {
    decision->to_tier = TIER_DRAM;
    decision->confidence = stats->heat_score;
    decision->reason = "Hot page promotion";
    return true;
  }

  /* Cold page in DRAM -> demote to NVM */
  if (stats->current_tier == TIER_DRAM &&
      stats->heat_score < g_policy_config.cold_threshold) {
    decision->to_tier = TIER_NVM;
    decision->confidence = 1.0 - stats->heat_score;
    decision->reason = "Cold page demotion";
    return true;
  }

  return false;
}

/*============================================================================
 * ML INTEGRATION POINT
 *===========================================================================*/

void set_migration_policy(migration_policy_fn policy) {
  g_migration_policy = policy ? policy : default_heuristic_policy;
  TM_INFO("Migration policy %s", policy ? "updated" : "reset to default");
}

/*
 * Main prediction function - replace internals with your ML model.
 *
 * Available features in page_stats_t:
 *   - access_count, read_count, write_count
 *   - heat_score (0.0-1.0), access_rate
 *   - current_tier, migration_count
 *   - first_access_ns, last_access_ns
 */
bool predict_migration(const page_stats_t *stats,
                       migration_decision_t *decision) {
  if (g_migration_policy != NULL) {
    return g_migration_policy(stats, decision);
  }
  return default_heuristic_policy(stats, decision);
}

/*============================================================================
 * MIGRATION EXECUTION
 *===========================================================================*/

static int execute_migration(migration_decision_t *decision) {
  if (decision == NULL)
    return -1;

  page_stats_t *stats = get_page_stats(decision->page_addr);
  if (stats == NULL) {
    TM_ERROR("No stats for page %p", decision->page_addr);
    return -1;
  }

  tier_config_t *dest = &g_manager.tiers[decision->to_tier];
  tier_config_t *src = &g_manager.tiers[decision->from_tier];

  if (dest->used + PAGE_SIZE > dest->capacity) {
    TM_DEBUG("Destination tier %s full", dest->name);
    return -1;
  }

  /* Update tier usage (in real system, would copy data here) */
  src->used -= PAGE_SIZE;
  dest->used += PAGE_SIZE;

  stats->current_tier = decision->to_tier;
  stats->last_migration_ns = get_time_ns();
  stats->migration_count++;

  atomic_fetch_add(&g_manager.total_migrations, 1);
  TM_DEBUG("Migrated %p: %s -> %s (%s)", decision->page_addr, src->name,
           dest->name, decision->reason);
  return 0;
}

/*============================================================================
 * POLICY THREAD
 *===========================================================================*/

static void *policy_thread_loop(void *arg) {
  (void)arg;
  TM_INFO("Policy thread running (interval=%dms)", POLICY_INTERVAL_MS);

  struct timespec sleep_time = {.tv_sec = 0,
                                .tv_nsec = POLICY_INTERVAL_MS * 1000000L};

  while (g_manager.threads_running) {
    nanosleep(&sleep_time, NULL);
    if (!g_manager.threads_running)
      break;

    atomic_fetch_add(&g_manager.policy_cycles, 1);

    /* Merge PEBS hardware samples with page stats */
    pebs_merge_with_page_stats();

    update_all_page_features();

    uint32_t migrations = 0;
    pthread_rwlock_rdlock(&g_manager.stats_lock);

    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE &&
                       migrations < g_policy_config.max_migrations_per_cycle;
         i++) {

      page_stats_t *entry = g_manager.page_stats_table[i];
      while (entry != NULL &&
             migrations < g_policy_config.max_migrations_per_cycle) {
        migration_decision_t decision = {0};

        if (predict_migration(entry, &decision) &&
            decision.confidence >= g_policy_config.confidence_min) {
          pthread_rwlock_unlock(&g_manager.stats_lock);
          if (execute_migration(&decision) == 0)
            migrations++;
          pthread_rwlock_rdlock(&g_manager.stats_lock);
        }
        entry = entry->next;
      }
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);

    /* Periodic logging (~1 second) */
    uint64_t cycles = atomic_load(&g_manager.policy_cycles);
    if (cycles % 100 == 0) {
      TM_INFO("Cycle %" PRIu64 ": pages=%" PRIu64 " faults=%" PRIu64
              " migrations=%" PRIu64,
              cycles, (uint64_t)atomic_load(&g_manager.total_pages_tracked),
              (uint64_t)atomic_load(&g_manager.total_faults),
              (uint64_t)atomic_load(&g_manager.total_migrations));
    }
  }

  TM_INFO("Policy thread exiting");
  return NULL;
}

int start_policy_thread(void) {
  if (g_migration_policy == NULL) {
    g_migration_policy = default_heuristic_policy;
  }

  if (pthread_create(&g_manager.policy_thread, NULL, policy_thread_loop,
                     NULL) != 0) {
    TM_ERROR("Failed to create policy thread: %s", strerror(errno));
    return -1;
  }
  TM_INFO("Policy thread started");
  return 0;
}

void stop_policy_thread(void) {
  pthread_join(g_manager.policy_thread, NULL);
  TM_INFO("Policy thread stopped");
}
