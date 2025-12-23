# LDOS Tiered Memory Manager

**Learning Directed Operating System** - ML-based page migration for tiered memory (DRAM + NVM/CXL)


## Overview

This project replaces traditional memory management heuristics (LRU, Clock) with ML-based policies for tiered memory systems. The architecture enables swappable migration policies while handling page faults with minimal latency.

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Application                                                    │
│       │                                                         │
│       ▼  mmap() > 1GB                                          │
│  ┌─────────────────┐                                            │
│  │  mmap_shim.c    │  LD_PRELOAD intercepts large allocations  │
│  └────────┬────────┘                                            │
│           ▼                                                     │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │  Tiered Memory Manager                                      ││
│  │                                                             ││
│  │  ┌─────────────────┐    ┌─────────────────────────────┐    ││
│  │  │ uffd_handler.c  │    │ policy_thread.c             │    ││
│  │  │                 │    │                             │    ││
│  │  │ • Page faults   │    │ • 10ms wake cycle           │    ││
│  │  │ • UFFDIO_COPY   │    │ • Feature computation       │    ││
│  │  │ • Fast path     │    │ • ML inference (pluggable)  │    ││
│  │  └─────────────────┘    │ • Migration decisions       │    ││
│  │                         └─────────────────────────────┘    ││
│  │                                                             ││
│  │  ┌─────────────────┐    ┌─────────────────────────────┐    ││
│  │  │ page_stats.c    │    │ tiered_memory.c             │    ││
│  │  │                 │    │                             │    ││
│  │  │ • Hash table    │    │ • 4GB DRAM (simulated)      │    ││
│  │  │ • ML features   │    │ • 16GB NVM (simulated)      │    ││
│  │  │ • Heat scores   │    │ • Init/shutdown             │    ││
│  │  └─────────────────┘    └─────────────────────────────┘    ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

## How It Works

### Two-Thread Design

The system uses two threads with different responsibilities:

| Thread | Latency | Role |
|--------|---------|------|
| **Fault Handler** | ~µs | Intercepts page faults, must respond immediately or app blocks |
| **Policy Thread** | ~ms | Runs ML inference every 10ms, makes migration decisions |

This separation is critical: ML inference takes milliseconds, but page faults must be resolved in microseconds. The fault handler makes fast initial placement decisions (DRAM first), while the policy thread later migrates pages based on learned access patterns.

### Data Flow

```
1. APPLICATION TOUCHES UNMAPPED PAGE
   │
   ▼
2. KERNEL GENERATES PAGE FAULT
   │  (userfaultfd intercepts instead of normal handling)
   ▼
3. UFFD HANDLER THREAD RECEIVES FAULT
   │  - Decides initial tier (DRAM if capacity, else NVM)
   │  - Calls UFFDIO_COPY to map a zero page
   │  - Records access in page_stats hash table
   │  - Application unblocks and continues
   ▼
4. POLICY THREAD (every 10ms)
   │  - Updates heat scores using exponential decay
   │  - Scans pages and calls predict_migration()
   │  - Hot pages in NVM → promote to DRAM
   │  - Cold pages in DRAM → demote to NVM
   ▼
5. ML MODEL (pluggable)
   │  - Receives page_stats_t with features
   │  - Returns migration decision with confidence
   │  - Currently: heuristic based on heat_score
   │  - Future: learned policy from traces
```

### Heat Score Computation

Pages are scored from 0.0 (cold) to 1.0 (hot) using:

heat_score = 0.6 × recency_factor + 0.4 × frequency_factor

recency_factor = exp(-0.07 × seconds_since_last_access)  # ~10s half-life
frequency_factor = min(access_rate / 1000, 1.0)          # Normalized


## Files

| File | Description |
|------|-------------|
| `tiered_memory.h` | Core header: data structures, policy interface |
| `tiered_memory.c` | Manager init/shutdown, tier configuration |
| `page_stats.c` | Per-page statistics hash table, feature computation |
| `uffd_handler.c` | Userfaultfd thread, page fault handling |
| `policy_thread.c` | 10ms policy loop, migration execution |
| `mmap_shim.c` | LD_PRELOAD library for mmap interception |
| `main.c` | Demo program |

## Building

```bash
make            # Release build
make DEBUG=1    # Debug build with verbose output
make clean      # Remove artifacts
```

**Outputs:**
- `tiered_manager` - Demo executable
- `libmmap_shim.so` - LD_PRELOAD library


## Usage

### Demo Mode
```bash
./tiered_manager
```

### Shim Mode (for real applications)
```bash
LD_PRELOAD=./libmmap_shim.so ./your_application
```

## ML Integration

The migration policy is swappable via `set_migration_policy()`. Implement the `migration_policy_fn` signature:

```c
bool my_ml_policy(const page_stats_t *stats, migration_decision_t *decision) {
    // Features available:
    //   stats->access_count, read_count, write_count
    //   stats->heat_score (0.0-1.0)
    //   stats->access_rate (accesses/sec)
    //   stats->current_tier (TIER_DRAM or TIER_NVM)
    //   stats->migration_count
    //   stats->first_access_ns, last_access_ns
    
    float prediction = ml_model_infer(stats);
    
    if (prediction > THRESHOLD && stats->current_tier == TIER_NVM) {
        decision->to_tier = TIER_DRAM;
        decision->confidence = prediction;
        decision->reason = "ML promotion";
        return true;
    }
    return false;
}

// Register the policy
set_migration_policy(my_ml_policy);
```

The integration point is `predict_migration()` in `policy_thread.c`.

## Design Decisions

1. **Two-thread architecture**: Fault handler (µs) + Policy thread (ms) - decouples fast fault resolution from slow ML inference
2. **Userfaultfd**: Kernel-level page fault interception without modifying applications
3. **Exponential decay heat score**: Balances recency and frequency for page hotness
4. **Pluggable policies**: Easy to swap between heuristics and ML models
5. **Simulated tiers**: Currently simulates 4GB DRAM + 16GB NVM (latency modeling for future work)
