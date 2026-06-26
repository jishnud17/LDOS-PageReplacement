/*
 * tiered_memory.h - LDOS Tiered Memory Manager
 * 
 * Core header defining shared data structures and interfaces for
 * ML-based tiered memory management (DRAM + NVM/CXL).
 * 
 * Architecture:
 *   - LD_PRELOAD shim intercepts mmap() for large allocations
 *   - Userfaultfd handler resolves page faults with tier placement
 *   - Policy thread runs ML predictions every 10ms
 *   - Page statistics track access patterns as ML features
 * 
 * LDOS Research Project, UT Austin (NSF-funded)
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
 * CONFIGURATION
 *===========================================================================*/

/* Build with -DSIMULATION_MODE=1 to test without userfaultfd privileges */
#ifndef SIMULATION_MODE
#define SIMULATION_MODE 0
#endif

#define LARGE_ALLOC_THRESHOLD (1UL << 30)  /* 1 GB - threshold for managed allocations */
#define PAGE_SIZE 4096
#define POLICY_INTERVAL_MS 10              /* ML inference interval */
#define MAX_MANAGED_REGIONS 64
#define MAX_TRACKED_PAGES (1 << 20)        /* ~1M pages = 4GB */
#define PAGE_STATS_HASH_SIZE 1048583       /* Prime for better distribution */

/* Predictive-signal sampling (phase-1 ML feature discovery).
 * One "bar" of per-page interval access-rate is sampled every SIGNAL_SAMPLE_MS
 * (= 5 policy cycles at POLICY_INTERVAL_MS=10).  SIGNAL_WINDOW bars of history
 * are retained per page and the 19 technical-analysis signals are computed
 * over that window.  See page_stats.c (SIGNALS section). */
#define SIGNAL_WINDOW 64
#define SIGNAL_SAMPLE_MS 50
#define STC_CYCLE 10                       /* Schaff Trend Cycle stochastic length */

/*============================================================================
 * MEMORY TIERS
 *===========================================================================*/

typedef enum {
    TIER_UNKNOWN = 0,
    TIER_DRAM,      /* Fast tier */
    TIER_NVM,       /* Slow tier (NVM/CXL) */
    TIER_COUNT
} memory_tier_t;

typedef struct tier_config {
    const char *name;
    size_t capacity;
    size_t used;
    uint64_t read_latency_ns;
    uint64_t write_latency_ns;
    void *backing_memory;
} tier_config_t;

/*============================================================================
 * PAGE STATISTICS (ML Features)
 *===========================================================================*/

/*
 * Per-page predictive signals (phase-1 ML feature discovery).
 *
 * All 19 signals are computed over a rolling window of "interval access-rate"
 * bars (accesses/sec measured over each SIGNAL_SAMPLE_MS window).  Stateful
 * indicators (TRIX, STC, PSAR, Supertrend, ASI) carry recursive state between
 * bars; the remaining indicators are recomputed from the window each bar.
 *
 * Several classic indicators assume OHLC bars; here each bar carries a single
 * value (the interval access rate), so High=Low=Close=value.  True Range then
 * reduces to |Δrate|, and +DM/-DM to the positive/negative bar-to-bar moves.
 * These standard single-series adaptations are noted per-signal in page_stats.c.
 *
 * Fields ending without a unit suffix are dimensionless indicator outputs.
 * Rows produced before enough history accumulates report 0.0 (never NaN).
 */
typedef struct page_signals {
    /* --- rolling window of interval access-rate bars (circular buffer) --- */
    double   rate_hist[SIGNAL_WINDOW];  /* accesses/sec per SIGNAL_SAMPLE_MS bar */
    uint32_t hist_count;                /* valid samples (saturates at WINDOW) */
    uint32_t hist_head;                 /* next write index */
    uint64_t last_sample_ns;            /* time of previous bar (0 = uninit) */
    uint64_t last_sample_access;        /* access_count at previous bar */
    double   interval_access_rate;      /* most recent bar value (exported) */

    /* --- persistent recursive state (not all exported directly) --- */
    double trix_ema1, trix_ema2, trix_ema3, trix_prev_ema3;
    bool   trix_init;

    double stc_ema_fast, stc_ema_slow;          /* MACD line EMAs */
    double stc_macd_hist[STC_CYCLE];            /* recent MACD for 1st stochastic */
    double stc_pf_hist[STC_CYCLE];              /* recent smoothed %K for 2nd stoch */
    uint32_t stc_macd_n, stc_pf_n;
    double stc_pf, stc_pff;                     /* smoothed %K, smoothed %D (STC) */
    bool   stc_init;

    double psar, psar_ep, psar_af;              /* PSAR value, extreme point, accel */
    bool   psar_trend_up, psar_init;

    double st_value, st_upper, st_lower;        /* Supertrend bands */
    int    st_dir;
    bool   st_init;

    double asi_accum;                           /* cumulative swing index */

    /* --- exported signal outputs --- */
    double si;                                  /* Swing Index (last bar) */
    double asi;                                 /* Accumulative Swing Index */
    double aroon_up, aroon_down, aroon_osc;
    double adx, plus_di, minus_di;
    double gapo;                                /* Gopalakrishnan Range Index */
    double ich_tenkan, ich_kijun, ich_senkou_a, ich_senkou_b, ich_chikou;
    double linreg_slope, linreg_intercept;      /* rate change per bar, offset */
    double psar_out; int psar_dir;              /* PSAR value, +1 up / -1 down */
    double rwi_high, rwi_low;                   /* Random Walk Index */
    double ravi;                                /* Range Action Verification Index */
    double stc;                                 /* Schaff Trend Cycle [0,100] */
    double stc_signal;                          /* smoothed STC */
    double supertrend; int supertrend_dir;      /* value, +1 up / -1 down */
    double sqn;                                 /* System Quality Number */
    double trix;                                /* Triple EMA rate-of-change (%) */
    double vhf;                                 /* Vertical Horizontal Filter [0,1] */
    double inter_access_interval_ms;            /* avg time between accesses */
    double inter_access_variance_ms2;           /* burstiness of access timing */
    double recency_weighted_freq;               /* recency-decayed access rate */
} page_signals_t;

typedef struct page_stats {
    void *page_addr;                /* Page-aligned virtual address (key) */
    
    /* Access counters */
    _Atomic uint64_t access_count;
    _Atomic uint64_t read_count;
    _Atomic uint64_t write_count;
    
    /* Temporal features */
    uint64_t first_access_ns;
    _Atomic uint64_t last_access_ns;
    uint64_t allocation_ns;
    
    /* Derived features (computed by policy thread) */
    double heat_score;              /* Hotness estimate [0.0, 1.0] */
    double access_rate;             /* Accesses per second */
    
    /* Placement state */
    memory_tier_t current_tier;
    uint64_t last_migration_ns;
    uint32_t migration_count;

    /* Predictive signals (phase-1 ML features), updated every SIGNAL_SAMPLE_MS */
    page_signals_t sig;

    struct page_stats *next;        /* Hash table chaining */
} page_stats_t;

/*============================================================================
 * MANAGED REGIONS
 *===========================================================================*/

typedef struct managed_region {
    void *base_addr;
    size_t length;
    int uffd;
    bool active;
    _Atomic uint64_t total_faults;
    _Atomic uint64_t pages_in_dram;
    _Atomic uint64_t pages_in_nvm;
} managed_region_t;

/*============================================================================
 * GLOBAL MANAGER STATE
 *===========================================================================*/

typedef struct tiered_manager {
    bool initialized;
    int uffd;
    bool uffd_wp_supported;
    
    /* Threads */
    pthread_t uffd_thread;
    pthread_t policy_thread;
    bool threads_running;
    
    /* Managed regions */
    managed_region_t regions[MAX_MANAGED_REGIONS];
    int region_count;
    pthread_mutex_t regions_lock;
    
    /* Page statistics */
    page_stats_t *page_stats_table[PAGE_STATS_HASH_SIZE];
    pthread_rwlock_t stats_lock;
    _Atomic uint64_t total_pages_tracked;
    
    /* Tier configurations */
    tier_config_t tiers[TIER_COUNT];
    
    /* Global statistics */
    _Atomic uint64_t total_faults;
    _Atomic uint64_t total_migrations;
    _Atomic uint64_t policy_cycles;
    
    /* Synchronization */
    pthread_mutex_t migration_lock;
    pthread_cond_t migration_cond;
} tiered_manager_t;

extern tiered_manager_t g_manager;

/*============================================================================
 * MIGRATION POLICY INTERFACE
 *===========================================================================*/

typedef struct migration_decision {
    void *page_addr;
    memory_tier_t from_tier;
    memory_tier_t to_tier;
    double confidence;          /* [0.0, 1.0] */
    const char *reason;
} migration_decision_t;

/*
 * Migration policy function signature.
 * Implement this to plug in your ML model.
 */
typedef bool (*migration_policy_fn)(
    const page_stats_t *stats,
    migration_decision_t *decision
);

extern migration_policy_fn g_migration_policy;

/*============================================================================
 * PUBLIC API
 *===========================================================================*/

/* Lifecycle */
int tiered_manager_init(void);
void tiered_manager_shutdown(void);
void tiered_manager_print_status(void);

/* Region management */
int register_managed_region(void *addr, size_t length);
void unregister_managed_region(void *addr);

/* Page statistics */
page_stats_t* get_page_stats(void *page_addr);
page_stats_t* get_or_create_page_stats(void *page_addr);
void record_page_access(void *page_addr, bool is_write);
void compute_page_features(page_stats_t *stats);
void update_all_page_features(void);
void update_page_signals(page_stats_t *stats);
void update_all_page_signals(void);
void print_page_stats_summary(void);
void cleanup_page_stats(void);

/* Policy */
void set_migration_policy(migration_policy_fn policy);
void set_csv_label(const char *label);
bool default_heuristic_policy(const page_stats_t *stats, migration_decision_t *decision);

/* Utilities */
uint64_t get_time_ns(void);
void* page_align(void *addr);
void reprotect_all_tracked_pages(void);

/*============================================================================
 * LOGGING
 *===========================================================================*/

#ifndef NDEBUG
#define TM_DEBUG(fmt, ...) \
    fprintf(stderr, "[TM DEBUG] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define TM_DEBUG(fmt, ...) ((void)0)
#endif

#define TM_INFO(fmt, ...) \
    fprintf(stderr, "[TM INFO] " fmt "\n", ##__VA_ARGS__)

#define TM_ERROR(fmt, ...) \
    fprintf(stderr, "[TM ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#endif /* TIERED_MEMORY_H */
