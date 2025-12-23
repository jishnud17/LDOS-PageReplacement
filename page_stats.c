/*
 * page_stats.c - Per-Page Statistics Collection
 * 
 * Hash table for tracking per-page access statistics.
 * These statistics serve as ML features for migration decisions.
 * 
 * LDOS Research Project, UT Austin
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
 * UTILITIES
 *===========================================================================*/

uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void* page_align(void *addr) {
    return (void*)((uintptr_t)addr & ~(PAGE_SIZE - 1));
}

static inline size_t hash_page_addr(void *addr) {
    uintptr_t page_num = (uintptr_t)addr >> 12;
    const uint64_t golden = 0x9E3779B97F4A7C15ULL;
    return (size_t)((page_num * golden) % PAGE_STATS_HASH_SIZE);
}

/*============================================================================
 * PAGE STATISTICS MANAGEMENT
 *===========================================================================*/

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

page_stats_t* get_or_create_page_stats(void *page_addr) {
    void *aligned = page_align(page_addr);
    size_t bucket = hash_page_addr(aligned);
    
    /* Try read-only lookup first */
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
    
    /* Create new entry */
    pthread_rwlock_wrlock(&g_manager.stats_lock);
    
    /* Double-check after acquiring write lock */
    entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    
    entry = (page_stats_t*)calloc(1, sizeof(page_stats_t));
    if (entry == NULL) {
        TM_ERROR("Failed to allocate page_stats_t");
        pthread_rwlock_unlock(&g_manager.stats_lock);
        return NULL;
    }
    
    uint64_t now = get_time_ns();
    entry->page_addr = aligned;
    entry->first_access_ns = now;
    atomic_store(&entry->last_access_ns, now);
    entry->allocation_ns = now;
    entry->current_tier = TIER_UNKNOWN;
    
    entry->next = g_manager.page_stats_table[bucket];
    g_manager.page_stats_table[bucket] = entry;
    atomic_fetch_add(&g_manager.total_pages_tracked, 1);
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    return entry;
}

void record_page_access(void *page_addr, bool is_write) {
    page_stats_t *stats = get_or_create_page_stats(page_addr);
    if (stats == NULL) return;
    
    atomic_fetch_add(&stats->access_count, 1);
    if (is_write) {
        atomic_fetch_add(&stats->write_count, 1);
    } else {
        atomic_fetch_add(&stats->read_count, 1);
    }
    atomic_store(&stats->last_access_ns, get_time_ns());
}

/*============================================================================
 * FEATURE COMPUTATION
 *===========================================================================*/

void compute_page_features(page_stats_t *stats) {
    uint64_t now = get_time_ns();
    uint64_t access_count = atomic_load(&stats->access_count);
    uint64_t last_access = atomic_load(&stats->last_access_ns);
    
    /* Access rate (accesses per second) */
    uint64_t lifetime_ns = now - stats->allocation_ns;
    if (lifetime_ns > 0) {
        stats->access_rate = (double)access_count * 1e9 / (double)lifetime_ns;
    }
    
    /* Heat score using exponential decay (~10 second half-life) */
    double decay_seconds = (double)(now - last_access) / 1e9;
    double recency_factor = exp(-0.07 * decay_seconds);
    double frequency_factor = fmin(stats->access_rate / 1000.0, 1.0);
    
    stats->heat_score = 0.6 * recency_factor + 0.4 * frequency_factor;
    stats->heat_score = fmax(0.0, fmin(1.0, stats->heat_score));
}

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

void print_page_stats_summary(void) {
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    
    uint64_t total = atomic_load(&g_manager.total_pages_tracked);
    uint64_t hot = 0, cold = 0;
    double total_heat = 0.0;
    
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            total_heat += entry->heat_score;
            if (entry->heat_score > 0.5) hot++;
            else cold++;
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    TM_INFO("Pages: %" PRIu64 " total, %" PRIu64 " hot, %" PRIu64 " cold, avg heat: %.3f",
            total, hot, cold, total > 0 ? total_heat / total : 0.0);
}

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
