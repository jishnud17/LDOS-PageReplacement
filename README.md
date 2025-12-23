# LDOS Tiered Memory Manager - Lite

**NSF-funded LDOS (Learning Directed Operating System) Research Project**  
**The University of Texas at Austin**

A modular C infrastructure for tiered memory management research, enabling ML-based page migration policies to replace traditional heuristics like LRU or Clock.

> **⚠️ Linux Required**: This code uses Linux-specific features (`userfaultfd`, `/proc/sys/vm/`) and must be compiled and run on a Linux system (kernel 4.3+).

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Complete Code Explanation](#complete-code-explanation)
4. [Building](#building)
5. [Usage](#usage)
6. [ML Integration Guide](#ml-integration-guide)
7. [Configuration](#configuration)

---

## Overview

This "Lite" manager provides the dataplane infrastructure for:

1. **Syscall Interception (The Hook)**: LD_PRELOAD shim intercepts `mmap()` for large allocations
2. **Userfaultfd Handler (The Controller)**: Background thread handles page faults via `UFFDIO_COPY`
3. **Predictor/Policy Thread (The Brain)**: 10ms wake cycle with `predict_migration()` placeholder
4. **Signal Collection**: Per-page access statistics for ML model features

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        User Application                         │
│                     (calls mmap for memory)                     │
└─────────────────────────┬───────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                    LD_PRELOAD Shim (mmap_shim.c)                │
│  • Intercepts mmap() calls > 1GB threshold                      │
│  • Registers regions with userfaultfd                           │
└─────────────────────────┬───────────────────────────────────────┘
                          │
          ┌───────────────┴───────────────┐
          ▼                               ▼
┌──────────────────────┐     ┌──────────────────────────────────┐
│  UFFD Handler Thread │     │      Policy Thread (Brain)       │
│   (uffd_handler.c)   │     │       (policy_thread.c)          │
│                      │     │                                  │
│ • Polls for faults   │     │ • Wakes every 10ms               │
│ • UFFDIO_COPY resolve│     │ • Updates page heat scores       │
│ • Records accesses   │     │ • Calls predict_migration()      │
│ • Initial placement  │◄────┤ • Triggers tier migrations       │
└──────────────────────┘     └──────────────────────────────────┘
          │                               │
          └───────────┬───────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────────┐
│               Page Statistics (page_stats.c)                    │
│  • Hash table for O(1) lookup                                   │
│  • Tracks: access_count, heat_score, access_rate, etc.          │
│  • Features ready for ML model input                            │
└─────────────────────────────────────────────────────────────────┘
                      │
          ┌───────────┴───────────┐
          ▼                       ▼
    ┌──────────┐           ┌──────────┐
    │   DRAM   │           │   NVM    │
    │  (Fast)  │           │  (Slow)  │
    │   4 GB   │           │  16 GB   │
    └──────────┘           └──────────┘
```

---

## Complete Code Explanation

### File 1: `tiered_memory.h` — The Blueprint

This header defines all shared data structures. Think of it as the "contract" that all files agree to.

#### Configuration Constants
```c
#define LARGE_ALLOC_THRESHOLD (1UL << 30)  // Only manage allocations > 1GB
#define PAGE_SIZE 4096                      // 4KB pages (smallest unit we track)
#define POLICY_INTERVAL_MS 10               // Brain wakes every 10ms
```

#### Memory Tier Enum
```c
typedef enum {
    TIER_UNKNOWN = 0,
    TIER_DRAM,      // Fast memory (~80ns latency)
    TIER_NVM,       // Slow memory (~300ns latency)
    TIER_COUNT      // Automatically = 3 (used for array sizing)
} memory_tier_t;
```
**Purpose:** Models your two-tier memory system. `TIER_COUNT` is a C idiom that lets arrays auto-size.

#### Page Statistics Structure (⭐ ML FEATURES)
```c
typedef struct page_stats {
    void *page_addr;              // Which page is this?
    
    // Counting features (atomics for thread safety)
    _Atomic uint64_t access_count; // Total touches
    _Atomic uint64_t read_count;   // Read accesses
    _Atomic uint64_t write_count;  // Write accesses
    
    // Temporal features
    uint64_t first_access_ns;      // When first touched
    uint64_t last_access_ns;       // When last touched
    uint64_t allocation_ns;        // When allocated
    
    // Derived features (computed by policy thread)
    double heat_score;             // Hotness [0.0 cold → 1.0 hot]
    double access_rate;            // Accesses per second
    
    // Current state
    memory_tier_t current_tier;    // Where is it now?
    uint32_t migration_count;      // How many times moved?
} page_stats_t;
```
**Purpose:** This is the "scorecard" for every page. **These are the features your ML model will learn from!**

#### Swappable Policy Interface (⭐ KEY FOR YOUR RESEARCH)
```c
typedef bool (*migration_policy_fn)(
    const page_stats_t *stats,      // INPUT: page's scorecard
    migration_decision_t *decision  // OUTPUT: what to do
);
```
**Purpose:** A function pointer that lets you swap the "brain":
- Start with `default_heuristic_policy()` (LRU-like)
- Replace with `my_ml_policy()` that runs your trained model

---

### File 2: `tiered_memory.c` — The Control Room

Initializes and shuts down the entire system.

#### Key Functions:

**`tiered_manager_init()`**
```c
int tiered_manager_init(void) {
    // 1. Initialize locks for thread safety
    // 2. Set up simulated tiers (4GB DRAM, 16GB NVM)
    // 3. Create userfaultfd
    // 4. Start fault handler thread
    // 5. Start policy thread
}
```
**Purpose:** Boots up the whole system. Call once at program start.

**`tiered_manager_shutdown()`**
```c
void tiered_manager_shutdown(void) {
    // 1. Signal threads to stop
    // 2. Wait for threads to exit
    // 3. Print final statistics
    // 4. Clean up resources
}
```
**Purpose:** Graceful shutdown with stats printout.

**Simulated Tier Configuration:**
```c
// DRAM: Fast tier
dram->capacity = 4GB;
dram->read_latency_ns = 80;

// NVM: Slow tier  
nvm->capacity = 16GB;
nvm->read_latency_ns = 300;
```
**Purpose:** In simulation, we model tier characteristics. With real hardware, these would be actual memory regions.

---

### File 3: `page_stats.c` — The Scorecard System

Manages per-page statistics using a hash table.

#### Key Functions:

**`get_or_create_page_stats(page_addr)`**
```c
page_stats_t* get_or_create_page_stats(void *page_addr) {
    // 1. Hash the page address
    // 2. Look up in hash table
    // 3. If not found, create new entry
    // 4. Return the scorecard
}
```
**Purpose:** O(1) lookup for any page's statistics.

**`record_page_access(page_addr, is_write)`**
```c
void record_page_access(void *page_addr, bool is_write) {
    // 1. Get/create scorecard
    // 2. Increment access_count (atomic)
    // 3. Increment read_count or write_count
    // 4. Update last_access timestamp
}
```
**Purpose:** Called on every page fault to track access patterns. In production, PEBS hardware counters would provide this.

**`compute_page_features(stats)`**
```c
void compute_page_features(page_stats_t *stats) {
    // Compute access_rate = accesses / lifetime
    
    // Compute heat_score using exponential decay:
    // - Recent access → high heat
    // - Old access → low heat
    heat_score = 0.6 * recency_factor + 0.4 * frequency_factor;
}
```
**Purpose:** Derives ML-ready features from raw counters. The heat score formula:
- `recency_factor`: e^(-0.07 × seconds_since_access) — decays over ~10 seconds
- `frequency_factor`: access_rate / 1000 — normalized frequency

---

### File 4: `uffd_handler.c` — The Fault Catcher

Background thread that intercepts and handles page faults.

#### How It Works:

**1. Register Region with Userfaultfd:**
```c
struct uffdio_register reg = {
    .range = { .start = addr, .len = length },
    .mode = UFFDIO_REGISTER_MODE_MISSING  // Catch "missing page" faults
};
ioctl(uffd, UFFDIO_REGISTER, &reg);
```
**Purpose:** Tells the kernel "send me page faults for this region instead of handling them."

**2. Fault Handler Loop:**
```c
while (running) {
    poll(uffd, ...);                    // Wait for fault event
    read(uffd, &msg, ...);              // Get fault details
    
    if (msg.event == UFFD_EVENT_PAGEFAULT) {
        tier = decide_initial_placement();  // DRAM if space, else NVM
        resolve_page_fault(addr, tier);     // Copy data in
    }
}
```
**Purpose:** When app touches an unallocated page, we decide where to put it.

**3. Resolve Fault:**
```c
struct uffdio_copy copy = {
    .dst = fault_addr,      // Where the app wants data
    .src = zero_page,       // What to copy (zeros for new alloc)
    .len = PAGE_SIZE
};
ioctl(uffd, UFFDIO_COPY, &copy);  // Kernel copies and resumes app
```
**Purpose:** Fills the page and lets the application continue.

---

### File 5: `policy_thread.c` — The Brain (⭐ ML GOES HERE)

Background thread that periodically makes migration decisions.

#### Main Loop:
```c
while (running) {
    nanosleep(10ms);                    // Wake every 10ms
    
    update_all_page_features();         // Recompute heat scores
    
    for each tracked page:
        if (predict_migration(stats, &decision)) {
            if (decision.confidence >= threshold) {
                execute_migration(&decision);  // Move the page
            }
        }
}
```

#### The ML Placeholder (Your Integration Point):
```c
bool predict_migration(const page_stats_t *stats, 
                       migration_decision_t *decision) {
    /*
     * ========================================================
     * TODO: REPLACE THIS WITH YOUR ML MODEL INFERENCE
     * ========================================================
     * 
     * Features available:
     *   - stats->heat_score
     *   - stats->access_rate
     *   - stats->access_count, read_count, write_count
     *   - stats->current_tier
     *   - timestamps for temporal patterns
     * 
     * Your code would look like:
     *   float features[] = { heat_score, access_rate, ... };
     *   float prediction = ml_model_forward(features);
     *   if (prediction > threshold) { ... }
     */
    
    // Currently uses simple heuristic
    return default_heuristic_policy(stats, decision);
}
```

#### Default Heuristic:
```c
bool default_heuristic_policy(stats, decision) {
    // Hot page in NVM → promote to DRAM
    if (stats->current_tier == TIER_NVM && stats->heat_score > 0.7) {
        decision->to_tier = TIER_DRAM;
        return true;
    }
    
    // Cold page in DRAM → demote to NVM
    if (stats->current_tier == TIER_DRAM && stats->heat_score < 0.3) {
        decision->to_tier = TIER_NVM;
        return true;
    }
    
    return false;  // No migration needed
}
```
**Purpose:** Simple LRU-like baseline. Your ML model should beat this!

---

### File 6: `mmap_shim.c` — The Hook

LD_PRELOAD library that intercepts `mmap()` without modifying applications.

#### How It Works:

**1. Intercept mmap:**
```c
// We replace the standard mmap function
void* mmap(void *addr, size_t length, ...) {
    if (length >= 1GB && is_anonymous && is_private) {
        // This is a large allocation we want to manage
        void *result = real_mmap(...);  // Call actual mmap
        register_managed_region(result, length);  // Register with uffd
        return result;
    }
    return real_mmap(...);  // Small allocs pass through
}
```

**2. Get the Real mmap:**
```c
real_mmap = dlsym(RTLD_NEXT, "mmap");  // Find next mmap in chain
```
**Purpose:** `RTLD_NEXT` finds the *real* mmap from libc.

**Usage:**
```bash
LD_PRELOAD=./libmmap_shim.so ./any_application
```
The application doesn't know we're intercepting its memory!

---

### File 7: `main.c` — The Demo

Shows how to use the system and simulates a workload.

#### Workflow:
```c
int main() {
    tiered_manager_init();              // Start system
    
    void *region = mmap(16MB, ...);     // Allocate test region
    register_managed_region(region);     // Register for management
    
    simulate_workload(region);          // Create hot/cold pages
    sleep(2);                           // Let policy thread work
    
    tiered_manager_print_status();      // Show results
    tiered_manager_shutdown();          // Clean up
}
```

#### Simulated Workload:
```c
void simulate_workload(region) {
    // Phase 1: Touch all pages (creates them)
    for each page: data[offset] = 'A';
    
    // Phase 2: Create "hot" pages (first 10%)
    for 50 rounds: touch first 10% repeatedly
    
    // Phase 3: Random access (70% bias to hot pages)
    for 1000 accesses: random page touch
}
```
**Purpose:** Creates a mix of hot and cold pages for the policy thread to detect.

---

## Building

### Requirements
- Linux kernel 4.3+ (for userfaultfd)
- GCC with C11 support
- pthreads, libdl, libm

### Enable userfaultfd for unprivileged users (if needed)
```bash
sudo sysctl -w vm.unprivileged_userfaultfd=1
```

### Compile
```bash
make          # Build everything
make debug    # Build with debug symbols
make clean    # Clean artifacts
```

### Outputs
- `tiered_manager` - Demo executable
- `libmmap_shim.so` - LD_PRELOAD library

---

## Usage

### Demo Program
```bash
./tiered_manager
```

### With Your Application
```bash
LD_PRELOAD=./libmmap_shim.so ./your_memory_intensive_app
```

---

## ML Integration Guide

The `predict_migration()` function in `policy_thread.c` is your integration point.

### Available Features (in `page_stats_t`)

| Feature | Type | Description |
|---------|------|-------------|
| `access_count` | uint64_t | Total page accesses |
| `read_count` | uint64_t | Number of reads |
| `write_count` | uint64_t | Number of writes |
| `first_access_ns` | uint64_t | First access timestamp |
| `last_access_ns` | uint64_t | Most recent access |
| `heat_score` | double | Computed hotness [0.0, 1.0] |
| `access_rate` | double | Accesses per second |
| `current_tier` | enum | TIER_DRAM or TIER_NVM |
| `migration_count` | uint32_t | Times this page migrated |

### Implementing Your ML Policy

```c
#include "tiered_memory.h"

bool my_ml_policy(const page_stats_t *stats, 
                  migration_decision_t *decision) {
    // 1. Extract features
    float features[] = {
        (float)stats->heat_score,
        (float)stats->access_rate,
        (float)atomic_load(&stats->access_count),
        (float)atomic_load(&stats->write_count) / 
            (float)(atomic_load(&stats->access_count) + 1),
        // ... more features
    };
    
    // 2. Run inference
    float prediction = your_ml_model_infer(features);
    
    // 3. Make decision
    if (prediction > PROMOTE_THRESHOLD && 
        stats->current_tier == TIER_NVM) {
        decision->page_addr = stats->page_addr;
        decision->from_tier = TIER_NVM;
        decision->to_tier = TIER_DRAM;
        decision->confidence = prediction;
        decision->reason = "ML: predicted hot";
        return true;
    }
    
    return false;
}

// Register your policy at startup
set_migration_policy(my_ml_policy);
```

---

## Configuration

### Key constants in `tiered_memory.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `LARGE_ALLOC_THRESHOLD` | 1 GB | Minimum size to manage |
| `PAGE_SIZE` | 4096 | Page size in bytes |
| `POLICY_INTERVAL_MS` | 10 | Policy thread wake interval |

### Policy thresholds in `policy_thread.c`:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `hot_threshold` | 0.7 | Promote if heat > this |
| `cold_threshold` | 0.3 | Demote if heat < this |
| `min_residence_ns` | 100ms | Min time before migrate |
| `max_migrations_per_cycle` | 10 | Rate limit |

---

## Future Work

1. **PEBS/IBS Integration**: Replace software counters with hardware performance counters
2. **Real NVM Support**: Use DAX/CXL devices instead of simulated tiers  
3. **More Features**: Add prefetch predictions, stride detection, allocation context
4. **ML Backends**: TensorFlow Lite, ONNX Runtime, or custom inference

---

## License

Research/Educational Use - UT Austin LDOS Project
