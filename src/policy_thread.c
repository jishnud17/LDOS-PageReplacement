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
static FILE *g_csv_file = NULL;
static const char *g_csv_label = "default";

/*
 * How often to re-arm UFFD write-protect on all tracked pages.
 * Each arm opens one sampling window: the first write per page per window
 * fires a WP fault and increments access_count.  Smaller values give finer
 * time resolution; larger values reduce ioctl overhead.
 * At POLICY_INTERVAL_MS=10 ms, 5 cycles = one re-arm every 50 ms.
 */
#define WP_RESAMPLE_CYCLES 5

void set_csv_label(const char *label) {
    if (label) g_csv_label = label;
}

static void *policy_thread_loop(void *arg);

static void export_page_stats_to_csv(uint64_t cycle) {
    if (!g_csv_file) return;
    
    uint64_t now = get_time_ns();
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            if (entry->access_count > 0) {
                const page_signals_t *s = &entry->sig;
                fprintf(g_csv_file,
                        /* existing 10 columns */
                        "%" PRIu64 ",%" PRIu64 ",%p,%d,%f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%f"
                        /* 32 predictive-signal columns (psar_dir & supertrend_dir are %d) */
                        ",%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g"
                        ",%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%d,%.6g"
                        ",%.6g,%.6g,%.6g,%.6g,%d,%.6g,%.6g,%.6g,%.6g,%.6g"
                        ",%.6g,%.6g"
                        /* 2 latency columns (0 unless LDOS_PEBS_LOAD_EVENT=0x1cd) */
                        ",%.6g,%.6g\n",
                        cycle, now, entry->page_addr, entry->current_tier,
                        entry->heat_score, entry->access_count,
                        entry->read_count, entry->write_count, entry->migration_count,
                        entry->access_rate,
                        s->interval_access_rate, s->si, s->asi,
                        s->aroon_up, s->aroon_down, s->aroon_osc,
                        s->adx, s->plus_di, s->minus_di, s->gapo,
                        s->ich_tenkan, s->ich_kijun, s->ich_senkou_a, s->ich_senkou_b, s->ich_chikou,
                        s->linreg_slope, s->linreg_intercept, s->psar_out, s->psar_dir, s->rwi_high,
                        s->rwi_low, s->ravi, s->stc, s->stc_signal, s->supertrend_dir,
                        s->supertrend, s->sqn, s->trix, s->vhf, s->inter_access_interval_ms,
                        s->inter_access_variance_ms2, s->recency_weighted_freq,
                        entry->pebs_interval_latency_cycles,
                        entry->pebs_mean_latency_cycles);
            }
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
}

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

/* Export cadence in policy cycles.  SIGNAL_SAMPLE_MS/POLICY_INTERVAL_MS by
 * default (5 cycles = 50 ms), overridable at runtime with
 * LDOS_SIGNAL_SAMPLE_MS so a run can be collected at a coarser bar without
 * recompiling.  Coarser bars are not merely a downgrade: at 50 ms nominal the
 * thread cannot keep up once many pages are tracked, so the MEASURED cadence
 * drifts per workload (143-357 ms across our real datasets) and becomes an
 * uncontrolled variable across datasets.  Asking for a cadence the thread can
 * actually hold makes it a controlled one. */
static uint64_t g_export_every_cycles = SIGNAL_SAMPLE_MS / POLICY_INTERVAL_MS;

static void policy_init_export_cadence(void) {
  const char *e = getenv("LDOS_SIGNAL_SAMPLE_MS");
  if (e != NULL) {
    long ms = strtol(e, NULL, 10);
    if (ms >= POLICY_INTERVAL_MS) {
      g_export_every_cycles = (uint64_t)(ms / POLICY_INTERVAL_MS);
    } else {
      TM_ERROR("LDOS_SIGNAL_SAMPLE_MS=%s below POLICY_INTERVAL_MS=%d; ignoring",
               e, POLICY_INTERVAL_MS);
    }
  }
  TM_INFO("Signal export cadence: %llu cycles (%llu ms nominal)",
          (unsigned long long)g_export_every_cycles,
          (unsigned long long)(g_export_every_cycles * POLICY_INTERVAL_MS));
}

static void *policy_thread_loop(void *arg) {
  (void)arg;
  TM_INFO("Policy thread running (interval=%dms)", POLICY_INTERVAL_MS);
  policy_init_export_cadence();

  struct timespec sleep_time = {.tv_sec = 0,
                                .tv_nsec = POLICY_INTERVAL_MS * 1000000L};

  while (g_manager.threads_running) {
    nanosleep(&sleep_time, NULL);
    if (!g_manager.threads_running)
      break;

    uint64_t cycles = atomic_fetch_add(&g_manager.policy_cycles, 1) + 1;

    /* Merge PEBS hardware samples with page stats */
    pebs_merge_with_page_stats();

    update_all_page_features();

    /* Re-arm write-protect every WP_RESAMPLE_CYCLES ticks so the next
     * write to each page fires a WP fault and increments access_count.
     * This is the only place WP is re-applied; the fault handler clears
     * it but never re-sets it, preventing the infinite-fault loop. */
    if (cycles % WP_RESAMPLE_CYCLES == 0) {
        reprotect_all_tracked_pages();
    }

    /* Telemetry-only mode: no migrations -- pages are not under uffd
     * management, so tier moves would be meaningless bookkeeping. */
    if (!g_manager.telemetry_only) {
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
    }

    /* Update predictive signals then export, every g_export_every_cycles
     * cycles (50ms by default, see LDOS_SIGNAL_SAMPLE_MS).
     * One signal "bar" per export keeps the window cadence aligned to rows. */
    if (cycles % g_export_every_cycles == 0) {
        update_all_page_signals();
        export_page_stats_to_csv(cycles);
    }

    /* Periodic logging (~1 second) */
    if (cycles % 100 == 0) {
      pebs_stats_t ps = pebs_get_stats();
      TM_INFO("Cycle %" PRIu64 ": pages=%" PRIu64 " faults=%" PRIu64
              " migrations=%" PRIu64 " pebs[samples=%" PRIu64 " r=%" PRIu64
              " w=%" PRIu64 " throttle=%" PRIu64 " err=%" PRIu64
              " lost=%" PRIu64 "]",
              cycles, (uint64_t)atomic_load(&g_manager.total_pages_tracked),
              (uint64_t)atomic_load(&g_manager.total_faults),
              (uint64_t)atomic_load(&g_manager.total_migrations),
              ps.total_samples, ps.read_samples, ps.write_samples,
              ps.throttle_events, ps.errors, ps.lost_samples);
    }
  }

  TM_INFO("Policy thread exiting");
  return NULL;
}

int start_policy_thread(void) {
  if (g_migration_policy == NULL) {
    g_migration_policy = default_heuristic_policy;
  }

  char csv_filename[256];
  snprintf(csv_filename, sizeof(csv_filename), "ml_dataset_%s.csv", g_csv_label);
  g_csv_file = fopen(csv_filename, "w");
  if (g_csv_file) {
      /* Descriptive signal names (column order matches the fprintf in
       * export_page_stats_to_csv; reactivity_analysis.py maps the old
       * abbreviated names from phase-1 CSVs onto these). */
      fprintf(g_csv_file,
              "cycle,timestamp_ns,page_addr,current_tier,heat_score,access_count,read_count,write_count,migration_count,access_rate,"
              "interval_access_rate,swing_index,accum_swing_index,aroon_up,aroon_down,aroon_oscillator,avg_directional_index,plus_directional_indicator,minus_directional_indicator,gopalakrishnan_range_index,"
              "ichimoku_tenkan,ichimoku_kijun,ichimoku_senkou_a,ichimoku_senkou_b,ichimoku_chikou,linear_reg_slope,linear_reg_intercept,parabolic_sar,parabolic_sar_direction,random_walk_index_high,"
              "random_walk_index_low,range_action_verification_index,schaff_trend_cycle,schaff_trend_cycle_signal,supertrend_direction,supertrend,system_quality_number,triple_exp_rate_of_change,vertical_horizontal_filter,inter_access_interval_ms,"
              "inter_access_variance_ms2,recency_weighted_frequency,"
              "interval_latency_cycles,mean_latency_cycles\n");
      TM_INFO("CSV output: %s", csv_filename);
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
  if (g_csv_file) {
      fclose(g_csv_file);
      g_csv_file = NULL;
  }
  TM_INFO("Policy thread stopped");
}
