/*
 * policy_thread.c - Predictor/Policy Thread (The Brain)
 * 
 * This module implements the background thread that periodically wakes up
 * to make migration decisions. This is where the ML model will be integrated.
 * 
 * Current implementation uses a simple heuristic (LRU-like).
 * The modular design allows easy replacement with ML inference.
 * 
 * The policy thread:
 *   1. Wakes every POLICY_INTERVAL_MS (10ms)
 *   2. Updates page features (heat scores, access rates)
 *   3. Scans pages and calls the migration policy function
 *   4. Triggers migrations for pages that should move
 * 
 * To integrate your ML model:
 *   1. Implement a function matching migration_policy_fn signature
 *   2. Call set_migration_policy() with your function
 *   3. Your function receives page_stats_t with features, returns decision
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <math.h>
#include "tiered_memory.h"

/* Current migration policy function pointer */
migration_policy_fn g_migration_policy = NULL;

/* Forward declaration */
static void* policy_thread_loop(void *arg);

/*============================================================================
 * POLICY CONFIGURATION
 *===========================================================================*/

/* Thresholds for the heuristic policy (tunable) */
typedef struct policy_config {
    double hot_threshold;       /* Heat score above this -> promote to DRAM */
    double cold_threshold;      /* Heat score below this -> demote to NVM */
    double confidence_min;      /* Minimum confidence to act on decision */
    uint64_t min_residence_ns;  /* Minimum time before considering migration */
    uint32_t max_migrations_per_cycle;  /* Rate limit migrations */
} policy_config_t;

static policy_config_t g_policy_config = {
    .hot_threshold = 0.7,
    .cold_threshold = 0.3,
    .confidence_min = 0.5,
    .min_residence_ns = 100000000,  /* 100ms */
    .max_migrations_per_cycle = 10
};

/*============================================================================
 * DEFAULT HEURISTIC POLICY
 *===========================================================================*/

/*
 * Default heuristic policy based on page heat score.
 * 
 * This is the placeholder that your ML model will replace.
 * 
 * Current logic:
 *   - Hot pages (heat > 0.7) in NVM -> promote to DRAM
 *   - Cold pages (heat < 0.3) in DRAM -> demote to NVM
 *   - Recently migrated pages are left alone (thrashing prevention)
 * 
 * Parameters:
 *   stats - Per-page statistics with ML features
 *   decision - Output: the migration decision if any
 * 
 * Returns:
 *   true if migration is recommended, false otherwise
 */
bool default_heuristic_policy(const page_stats_t *stats, 
                               migration_decision_t *decision) {
    if (stats == NULL || decision == NULL) {
        return false;
    }
    
    uint64_t now = get_time_ns();
    
    /* Don't migrate pages that were recently migrated (prevent thrashing) */
    if (stats->last_migration_ns > 0) {
        uint64_t time_since_migration = now - stats->last_migration_ns;
        if (time_since_migration < g_policy_config.min_residence_ns) {
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
        decision->confidence = 1.0 - stats->heat_score;  /* Higher confidence for colder pages */
        decision->reason = "Cold page demotion";
        return true;
    }
    
    return false;
}

/*============================================================================
 * ML MODEL INTEGRATION POINT
 *===========================================================================*/

/*
 * Set the migration policy function.
 * 
 * Call this to replace the default heuristic with your ML model.
 * 
 * Example integration:
 * 
 *   bool my_ml_policy(const page_stats_t *stats, migration_decision_t *decision) {
 *       // Extract features from stats
 *       float features[NUM_FEATURES] = {
 *           stats->heat_score,
 *           stats->access_rate,
 *           (float)atomic_load(&stats->access_count),
 *           ... 
 *       };
 *       
 *       // Run inference
 *       float prediction = ml_model_predict(features);
 *       
 *       // Make decision
 *       if (prediction > PROMOTE_THRESHOLD) {
 *           decision->to_tier = TIER_DRAM;
 *           return true;
 *       }
 *       ...
 *   }
 *   
 *   // In main:
 *   set_migration_policy(my_ml_policy);
 */
void set_migration_policy(migration_policy_fn policy) {
    if (policy == NULL) {
        g_migration_policy = default_heuristic_policy;
        TM_INFO("Migration policy reset to default heuristic");
    } else {
        g_migration_policy = policy;
        TM_INFO("Migration policy updated to custom function");
    }
}

/*============================================================================
 * PLACEHOLDER: predict_migration()
 *===========================================================================*/

/*
 * PLACEHOLDER FUNCTION FOR ML MODEL
 * 
 * This is the function where you will integrate your learned policy.
 * Currently it wraps the heuristic, but you can replace the internals
 * with your ML inference code.
 * 
 * Features available in page_stats_t:
 *   - access_count: Total number of accesses
 *   - read_count / write_count: Read vs write ratio
 *   - first_access_ns / last_access_ns: Temporal pattern
 *   - heat_score: Pre-computed hotness (can recompute with your own formula)
 *   - access_rate: Accesses per second
 *   - current_tier: Where the page currently resides
 *   - migration_count: How many times already migrated
 * 
 * You might also want to add:
 *   - PEBS/IBS hardware counter data
 *   - Memory access pattern (stride, random)
 *   - Application-level hints
 *   - Prefetch predictions
 * 
 * Returns:
 *   true if a migration should happen, decision filled with details
 */
bool predict_migration(const page_stats_t *stats, 
                       migration_decision_t *decision) {
    /*
     * ========================================================
     * TODO: REPLACE THIS WITH YOUR ML MODEL INFERENCE
     * ========================================================
     * 
     * Example structure for ML integration:
     * 
     * // 1. Extract features into tensor
     * float features[] = {
     *     (float)stats->heat_score,
     *     (float)stats->access_rate,
     *     (float)atomic_load(&stats->access_count),
     *     (float)atomic_load(&stats->write_count) / 
     *         (float)(atomic_load(&stats->access_count) + 1),
     *     (float)(get_time_ns() - stats->last_access_ns) / 1e9,
     *     (float)stats->migration_count,
     *     // ... more features
     * };
     * 
     * // 2. Run model inference
     * float* output = ml_model_forward(model, features, NUM_FEATURES);
     * 
     * // 3. Interpret output
     * //    output[0] = P(should be in DRAM)
     * //    output[1] = P(should be in NVM)
     * 
     * if (output[0] > 0.5 && stats->current_tier == TIER_NVM) {
     *     decision->to_tier = TIER_DRAM;
     *     decision->confidence = output[0];
     *     return true;
     * }
     * 
     * ========================================================
     */
    
    /* For now, delegate to the configured policy function */
    if (g_migration_policy != NULL) {
        return g_migration_policy(stats, decision);
    }
    
    return default_heuristic_policy(stats, decision);
}

/*============================================================================
 * PAGE MIGRATION
 *===========================================================================*/

/*
 * Execute a page migration between tiers.
 * 
 * In a real system, this would:
 *   1. Copy page data from source to destination tier
 *   2. Update page table mappings
 *   3. Free source tier memory
 * 
 * For our simulation, we just update the metadata.
 * The actual data doesn't move since both "tiers" are regular memory.
 */
static int execute_migration(migration_decision_t *decision) {
    if (decision == NULL) {
        return -1;
    }
    
    void *page_addr = decision->page_addr;
    page_stats_t *stats = get_page_stats(page_addr);
    
    if (stats == NULL) {
        TM_ERROR("No stats for page %p during migration", page_addr);
        return -1;
    }
    
    /* Check tier capacity */
    tier_config_t *dest_tier = &g_manager.tiers[decision->to_tier];
    tier_config_t *src_tier = &g_manager.tiers[decision->from_tier];
    
    if (dest_tier->used + PAGE_SIZE > dest_tier->capacity) {
        TM_DEBUG("Destination tier %s full, skipping migration", 
                 dest_tier->name);
        return -1;
    }
    
    /*
     * In a real system, we would:
     *   1. Allocate page in destination tier
     *   2. Copy data
     *   3. Update page table (might use userfault REMAP or mprotect tricks)
     *   4. Free source page
     * 
     * For simulation, just update accounting.
     */
    
    /* Update tier usage */
    src_tier->used -= PAGE_SIZE;
    dest_tier->used += PAGE_SIZE;
    
    /* Update page stats */
    stats->current_tier = decision->to_tier;
    stats->last_migration_ns = get_time_ns();
    stats->migration_count++;
    
    atomic_fetch_add(&g_manager.total_migrations, 1);
    
    TM_DEBUG("Migrated %p: %s -> %s (reason: %s, confidence: %.2f)",
             page_addr,
             src_tier->name, dest_tier->name,
             decision->reason, decision->confidence);
    
    return 0;
}

/*============================================================================
 * POLICY THREAD
 *===========================================================================*/

/*
 * Main loop for the policy thread.
 * 
 * Wakes every POLICY_INTERVAL_MS and:
 *   1. Updates all page features (heat scores, etc.)
 *   2. Scans pages and runs the prediction function
 *   3. Executes migrations for recommended pages
 */
static void* policy_thread_loop(void *arg) {
    (void)arg;
    
    TM_INFO("Policy thread running (interval=%dms)", POLICY_INTERVAL_MS);
    
    struct timespec sleep_time = {
        .tv_sec = 0,
        .tv_nsec = POLICY_INTERVAL_MS * 1000000L
    };
    
    while (g_manager.threads_running) {
        /* Sleep for the policy interval */
        nanosleep(&sleep_time, NULL);
        
        if (!g_manager.threads_running) break;
        
        atomic_fetch_add(&g_manager.policy_cycles, 1);
        
        /* Update features for all tracked pages */
        update_all_page_features();
        
        /* Scan pages and make migration decisions */
        uint32_t migrations_this_cycle = 0;
        
        pthread_rwlock_rdlock(&g_manager.stats_lock);
        
        for (size_t i = 0; i < PAGE_STATS_HASH_SIZE && 
                          migrations_this_cycle < g_policy_config.max_migrations_per_cycle; 
             i++) {
            
            page_stats_t *entry = g_manager.page_stats_table[i];
            while (entry != NULL && 
                   migrations_this_cycle < g_policy_config.max_migrations_per_cycle) {
                
                migration_decision_t decision = {0};
                
                /* Call the prediction function (ML model goes here) */
                if (predict_migration(entry, &decision)) {
                    /* Check confidence threshold */
                    if (decision.confidence >= g_policy_config.confidence_min) {
                        /* Need to release read lock before migration */
                        pthread_rwlock_unlock(&g_manager.stats_lock);
                        
                        if (execute_migration(&decision) == 0) {
                            migrations_this_cycle++;
                        }
                        
                        pthread_rwlock_rdlock(&g_manager.stats_lock);
                    }
                }
                
                entry = entry->next;
            }
        }
        
        pthread_rwlock_unlock(&g_manager.stats_lock);
        
        /* Periodic status logging (every ~1 second) */
        uint64_t cycles = atomic_load(&g_manager.policy_cycles);
        if (cycles % 100 == 0) {
            TM_INFO("Policy cycle %" PRIu64 ": %" PRIu64 " pages, %" PRIu64 " faults, %" PRIu64 " migrations",
                    cycles,
                    (uint64_t)atomic_load(&g_manager.total_pages_tracked),
                    (uint64_t)atomic_load(&g_manager.total_faults),
                    (uint64_t)atomic_load(&g_manager.total_migrations));
            print_page_stats_summary();
        }
    }
    
    TM_INFO("Policy thread exiting");
    return NULL;
}

/*
 * Start the policy thread.
 */
int start_policy_thread(void) {
    /* Initialize policy function if not set */
    if (g_migration_policy == NULL) {
        g_migration_policy = default_heuristic_policy;
    }
    
    if (pthread_create(&g_manager.policy_thread, NULL, 
                       policy_thread_loop, NULL) != 0) {
        TM_ERROR("Failed to create policy thread: %s", strerror(errno));
        return -1;
    }
    
    TM_INFO("Policy thread started");
    return 0;
}

/*
 * Stop the policy thread.
 */
void stop_policy_thread(void) {
    pthread_join(g_manager.policy_thread, NULL);
    TM_INFO("Policy thread stopped");
}
