/*
 * tiered_memory.h - LDOS Tiered Memory Manager Core Header
 * 
 * This header defines the shared data structures and interfaces for the
 * tiered memory management system. The design is modular to allow easy
 * swapping between heuristic-based and ML-based migration policies.
 * 
 * Architecture:
 *   - LD_PRELOAD shim intercepts mmap() for large allocations
 *   - Userfaultfd handler resolves page faults asynchronously
 *   - Policy thread runs predictions every 10ms
 *   - Page statistics track access patterns for ML features
 * 
 * Author: LDOS Research Project, UT Austin
 * License: Research/Educational Use
 */

#ifndef TIERED_MEMORY_H
#define TIERED_MEMORY_H

#define _GNU_SOURCE

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

/*============================================================================
 * CONFIGURATION CONSTANTS
 *===========================================================================*/

/* Threshold for "large" allocations that we manage (1GB default) */
#define LARGE_ALLOC_THRESHOLD (1UL << 30)  /* 1 GB */

/* System page size (4KB on most systems) */
#define PAGE_SIZE 4096

/* Policy thread wake interval in milliseconds */
#define POLICY_INTERVAL_MS 10

/* Maximum number of managed regions we track */
#define MAX_MANAGED_REGIONS 64

/* Maximum pages we track statistics for (for hash table sizing) */
#define MAX_TRACKED_PAGES (1 << 20)  /* ~1M pages = 4GB worth */

/* Hash table size for page stats (should be prime for better distribution) */
#define PAGE_STATS_HASH_SIZE 1048583

/*============================================================================
 * MEMORY TIER DEFINITIONS
 *===========================================================================*/

/* Memory tier enumeration */
typedef enum {
    TIER_UNKNOWN = 0,
    TIER_DRAM,      /* Fast tier: simulated DRAM */
    TIER_NVM,       /* Slow tier: simulated NVM/CXL memory */
    TIER_COUNT
} memory_tier_t;

/* Simulated tier characteristics (for future cost modeling) */
typedef struct tier_config {
    const char *name;           /* Human-readable tier name */
    size_t capacity;            /* Total capacity in bytes */
    size_t used;                /* Currently used bytes */
    uint64_t read_latency_ns;   /* Simulated read latency */
    uint64_t write_latency_ns;  /* Simulated write latency */
    void *backing_memory;       /* Actual memory backing this tier */
} tier_config_t;

/*============================================================================
 * PER-PAGE STATISTICS (Features for ML Model)
 *===========================================================================*/

/*
 * Per-page access statistics structure.
 * These are the "features" that will feed into the ML model for
 * predicting page "heat" and migration decisions.
 * 
 * In production, some of these would come from PEBS hardware counters.
 * For now, we simulate them via software tracking.
 */
typedef struct page_stats {
    void *page_addr;                /* Page-aligned virtual address (key) */
    
    /* Access pattern features */
    _Atomic uint64_t access_count;  /* Total accesses (read + write) */
    _Atomic uint64_t read_count;    /* Read accesses */
    _Atomic uint64_t write_count;   /* Write accesses */
    
    /* Temporal features */
    uint64_t first_access_ns;       /* Timestamp of first access */
    _Atomic uint64_t last_access_ns;/* Timestamp of most recent access */
    uint64_t allocation_ns;         /* When the page was allocated */
    
    /* Derived features (computed by policy thread) */
    double heat_score;              /* Current "hotness" estimate [0.0, 1.0] */
    double access_rate;             /* Accesses per second */
    
    /* Current placement */
    memory_tier_t current_tier;     /* Which tier the page is on */
    uint64_t last_migration_ns;     /* When page was last migrated */
    uint32_t migration_count;       /* How many times migrated */
    
    /* Hash table chaining */
    struct page_stats *next;        /* For hash collision chaining */
} page_stats_t;

/*============================================================================
 * MANAGED REGION TRACKING
 *===========================================================================*/

/*
 * Represents a memory region that we're managing via userfaultfd.
 * Created when mmap() intercepts a large allocation.
 */
typedef struct managed_region {
    void *base_addr;            /* Start address of the region */
    size_t length;              /* Length in bytes */
    int uffd;                   /* Userfaultfd file descriptor for this region */
    bool active;                /* Is this region currently active? */
    
    /* Statistics for the region */
    _Atomic uint64_t total_faults;   /* Total page faults in this region */
    _Atomic uint64_t pages_in_dram;  /* Pages currently in DRAM */
    _Atomic uint64_t pages_in_nvm;   /* Pages currently in NVM */
} managed_region_t;

/*============================================================================
 * GLOBAL MANAGER STATE
 *===========================================================================*/

/*
 * Central state structure for the tiered memory manager.
 * This is the "shared state" between all threads.
 */
typedef struct tiered_manager {
    /* Initialization flag */
    bool initialized;
    
    /* Userfaultfd master descriptor */
    int uffd;
    
    /* Thread handles */
    pthread_t uffd_thread;          /* Fault handler thread */
    pthread_t policy_thread;        /* Prediction/migration thread */
    bool threads_running;           /* Control flag for shutdown */
    
    /* Managed regions */
    managed_region_t regions[MAX_MANAGED_REGIONS];
    int region_count;
    pthread_mutex_t regions_lock;
    
    /* Page statistics hash table */
    page_stats_t *page_stats_table[PAGE_STATS_HASH_SIZE];
    pthread_rwlock_t stats_lock;    /* RW lock for stats access */
    _Atomic uint64_t total_pages_tracked;
    
    /* Tier configurations */
    tier_config_t tiers[TIER_COUNT];
    
    /* Global statistics */
    _Atomic uint64_t total_faults;
    _Atomic uint64_t total_migrations;
    _Atomic uint64_t policy_cycles;
    
    /* Inter-thread communication */
    pthread_mutex_t migration_lock;
    pthread_cond_t migration_cond;
    
} tiered_manager_t;

/* Global manager instance (defined in tiered_memory.c) */
extern tiered_manager_t g_manager;

/*============================================================================
 * MIGRATION POLICY INTERFACE (The Swappable "Brain")
 *===========================================================================*/

/*
 * Migration decision structure.
 * Returned by the policy function to indicate what action to take.
 */
typedef struct migration_decision {
    void *page_addr;            /* Which page to migrate */
    memory_tier_t from_tier;    /* Current tier */
    memory_tier_t to_tier;      /* Destination tier */
    double confidence;          /* Policy confidence [0.0, 1.0] */
    const char *reason;         /* Human-readable reason (for debugging) */
} migration_decision_t;

/*
 * Migration policy function type.
 * 
 * This is the interface for swappable policies. Implement this function
 * signature to plug in your ML model:
 * 
 *   - stats: Page statistics for the page being evaluated
 *   - decision: Output parameter for the migration decision
 *   - Returns: true if a migration is recommended, false otherwise
 * 
 * The default heuristic implementation uses simple LRU-like logic.
 * Replace with ML inference for learned policies.
 */
typedef bool (*migration_policy_fn)(
    const page_stats_t *stats,
    migration_decision_t *decision
);

/* Currently active policy function pointer */
extern migration_policy_fn g_migration_policy;

/*============================================================================
 * PUBLIC API FUNCTIONS
 *===========================================================================*/

/* Initialization and shutdown */
int tiered_manager_init(void);
void tiered_manager_shutdown(void);

/* Region management (called from mmap shim) */
int register_managed_region(void *addr, size_t length);
void unregister_managed_region(void *addr);

/* Page statistics */
page_stats_t* get_page_stats(void *page_addr);
page_stats_t* get_or_create_page_stats(void *page_addr);
void record_page_access(void *page_addr, bool is_write);

/* Policy interface */
void set_migration_policy(migration_policy_fn policy);
bool default_heuristic_policy(const page_stats_t *stats, migration_decision_t *decision);

/* Utility functions */
uint64_t get_time_ns(void);
void* page_align(void *addr);

/*============================================================================
 * DEBUG AND LOGGING
 *===========================================================================*/

#ifndef NDEBUG
#define TM_DEBUG(fmt, ...) \
    fprintf(stderr, "[TM DEBUG] %s:%d: " fmt "\n", \
            __func__, __LINE__, ##__VA_ARGS__)
#else
#define TM_DEBUG(fmt, ...) ((void)0)
#endif

#define TM_INFO(fmt, ...) \
    fprintf(stderr, "[TM INFO] " fmt "\n", ##__VA_ARGS__)

#define TM_ERROR(fmt, ...) \
    fprintf(stderr, "[TM ERROR] %s:%d: " fmt "\n", \
            __func__, __LINE__, ##__VA_ARGS__)

#endif /* TIERED_MEMORY_H */
