/*
 * page_stats.c - Per-Page Statistics Collection
 * 
 * This module implements a hash table for tracking per-page access statistics.
 * These statistics serve as "features" for the ML model to learn from.
 * 
 * In a production LDOS system, some features would come from hardware
 * performance counters (PEBS/IBS). This module simulates those with
 * software-based tracking.
 * 
 * Thread Safety:
 *   - Uses RW locks for concurrent access
 *   - Atomic operations for frequently updated counters
 * 
 * Performance Considerations:
 *   - Hash table provides O(1) average lookup
 *   - Lock striping could be added for higher concurrency
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>
#include "tiered_memory.h"

/*============================================================================
 * UTILITY FUNCTIONS
 *===========================================================================*/

/*
 * Get current time in nanoseconds (monotonic clock).
 * Used for all timestamp tracking.
 */
uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * Align an address down to page boundary.
 */
void* page_align(void *addr) {
    return (void*)((uintptr_t)addr & ~(PAGE_SIZE - 1));
}

/*
 * Hash function for page addresses.
 * Uses a simple but effective multiplicative hash.
 * Page addresses are already aligned, so we shift right first.
 */
static inline size_t hash_page_addr(void *addr) {
    /* Shift right by 12 (page size bits) to get page number */
    uintptr_t page_num = (uintptr_t)addr >> 12;
    
    /* Multiplicative hash with golden ratio prime */
    const uint64_t golden = 0x9E3779B97F4A7C15ULL;
    return (size_t)((page_num * golden) % PAGE_STATS_HASH_SIZE);
}

/*============================================================================
 * PAGE STATISTICS MANAGEMENT
 *===========================================================================*/

/*
 * Look up page statistics without creating an entry.
 * Returns NULL if the page is not being tracked.
 * 
 * Thread-safe: acquires read lock.
 */
page_stats_t* get_page_stats(void *page_addr) {
    void *aligned = page_align(page_addr);
    size_t bucket = hash_page_addr(aligned);
    
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    
    page_stats_t *entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    return NULL;
}

/*
 * Get or create page statistics entry.
 * Creates a new entry if one doesn't exist.
 * 
 * Thread-safe: upgrades to write lock when creating.
 */
page_stats_t* get_or_create_page_stats(void *page_addr) {
    void *aligned = page_align(page_addr);
    size_t bucket = hash_page_addr(aligned);
    
    /* First, try read-only lookup */
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    
    page_stats_t *entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    /* Not found, need to create - acquire write lock */
    pthread_rwlock_wrlock(&g_manager.stats_lock);
    
    /* Double-check after acquiring write lock (another thread may have created it) */
    entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    
    /* Create new entry */
    entry = (page_stats_t*)calloc(1, sizeof(page_stats_t));
    if (entry == NULL) {
        TM_ERROR("Failed to allocate page_stats_t");
        pthread_rwlock_unlock(&g_manager.stats_lock);
        return NULL;
    }
    
    uint64_t now = get_time_ns();
    
    entry->page_addr = aligned;
    atomic_store(&entry->access_count, 0);
    atomic_store(&entry->read_count, 0);
    atomic_store(&entry->write_count, 0);
    entry->first_access_ns = now;
    atomic_store(&entry->last_access_ns, now);
    entry->allocation_ns = now;
    entry->heat_score = 0.0;
    entry->access_rate = 0.0;
    entry->current_tier = TIER_UNKNOWN;
    entry->last_migration_ns = 0;
    entry->migration_count = 0;
    
    /* Insert at head of bucket chain */
    entry->next = g_manager.page_stats_table[bucket];
    g_manager.page_stats_table[bucket] = entry;
    
    atomic_fetch_add(&g_manager.total_pages_tracked, 1);
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    TM_DEBUG("Created stats for page %p (bucket %zu, total: %" PRIu64 ")",
             aligned, bucket, (uint64_t)atomic_load(&g_manager.total_pages_tracked));
    
    return entry;
}

/*
 * Record an access to a page.
 * Updates counters atomically for minimal contention.
 * 
 * This is called on every tracked page access, so it must be fast.
 * In a real PEBS-based system, this would be replaced by counter sampling.
 */
void record_page_access(void *page_addr, bool is_write) {
    page_stats_t *stats = get_or_create_page_stats(page_addr);
    if (stats == NULL) {
        return;
    }
    
    uint64_t now = get_time_ns();
    
    /* Update counters atomically */
    atomic_fetch_add(&stats->access_count, 1);
    
    if (is_write) {
        atomic_fetch_add(&stats->write_count, 1);
    } else {
        atomic_fetch_add(&stats->read_count, 1);
    }
    
    /* Update timestamp (relaxed ordering is fine for this) */
    atomic_store(&stats->last_access_ns, now);
}

/*
 * Compute derived features for a page.
 * Called periodically by the policy thread.
 * 
 * These derived features are what the ML model will use for predictions:
 *   - heat_score: Normalized hotness [0.0, 1.0]
 *   - access_rate: Accesses per second
 */
void compute_page_features(page_stats_t *stats) {
    uint64_t now = get_time_ns();
    
    uint64_t access_count = atomic_load(&stats->access_count);
    uint64_t last_access = atomic_load(&stats->last_access_ns);
    
    /* Compute access rate (accesses per second) */
    uint64_t lifetime_ns = now - stats->allocation_ns;
    if (lifetime_ns > 0) {
        stats->access_rate = (double)access_count * 1e9 / (double)lifetime_ns;
    }
    
    /* Compute heat score using exponential decay */
    /* Heat decays over time since last access */
    uint64_t time_since_access_ns = now - last_access;
    double decay_seconds = (double)time_since_access_ns / 1e9;
    
    /* Decay constant: ~10 second half-life */
    const double decay_rate = 0.07;
    double recency_factor = exp(-decay_rate * decay_seconds);
    
    /* Combine recency with access frequency */
    /* Normalize access rate (assuming 1000 accesses/sec is "hot") */
    double frequency_factor = stats->access_rate / 1000.0;
    if (frequency_factor > 1.0) frequency_factor = 1.0;
    
    /* Heat score is weighted combination */
    stats->heat_score = 0.6 * recency_factor + 0.4 * frequency_factor;
    
    /* Clamp to [0, 1] */
    if (stats->heat_score > 1.0) stats->heat_score = 1.0;
    if (stats->heat_score < 0.0) stats->heat_score = 0.0;
}

/*
 * Iterate over all tracked pages and compute features.
 * Called by the policy thread each cycle.
 */
void update_all_page_features(void) {
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            compute_page_features(entry);
            entry = entry->next;
        }
    }
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
}

/*
 * Get statistics summary for debugging/logging.
 */
void print_page_stats_summary(void) {
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    
    uint64_t total_pages = atomic_load(&g_manager.total_pages_tracked);
    uint64_t hot_pages = 0;
    uint64_t cold_pages = 0;
    double total_heat = 0.0;
    
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            total_heat += entry->heat_score;
            if (entry->heat_score > 0.5) {
                hot_pages++;
            } else {
                cold_pages++;
            }
            entry = entry->next;
        }
    }
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    double avg_heat = total_pages > 0 ? total_heat / total_pages : 0.0;
    
    TM_INFO("Page Stats: %" PRIu64 " total, %" PRIu64 " hot, %" PRIu64 " cold, avg heat: %.3f",
            total_pages, hot_pages, cold_pages, avg_heat);
}

/*
 * Clean up all page statistics (called on shutdown).
 */
void cleanup_page_stats(void) {
    pthread_rwlock_wrlock(&g_manager.stats_lock);
    
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            page_stats_t *next = entry->next;
            free(entry);
            entry = next;
        }
        g_manager.page_stats_table[i] = NULL;
    }
    
    atomic_store(&g_manager.total_pages_tracked, 0);
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    TM_INFO("Page statistics cleaned up");
}
