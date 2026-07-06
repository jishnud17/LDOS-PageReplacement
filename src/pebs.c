/*
 * pebs.c - PEBS (Processor Event-Based Sampling) Implementation
 *
 * Uses Intel PEBS hardware to sample memory accesses at high frequency.
 * Collected samples are merged with page_stats for ML training.
 *
 * LDOS Research Project, UT Austin
 */

#ifdef __linux__

#define _GNU_SOURCE
#include <asm/unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pebs.h"
#include "tiered_memory.h"

/*============================================================================
 * CONFIGURATION
 *===========================================================================*/

#define PEBS_HASH_SIZE 65537 /* Prime for hash table */

/*============================================================================
 * INTERNAL DATA STRUCTURES
 *===========================================================================*/

struct perf_sample {
  struct perf_event_header header;
  __u64 ip;       /* Instruction pointer */
  __u32 pid, tid; /* Process/thread ID */
  __u64 addr;     /* Virtual address accessed */
  __u64 weight;   /* Access latency (cycles) */
};

/* Upper bound on tracked CPUs (c220g* nodes have 40 hw threads). */
#define PEBS_MAX_CPUS 256

/* PEBS state.
 * Events are opened per-CPU (pid=0, cpu=N) with attr.inherit=1 so that
 * worker threads created after init are sampled too.  The kernel refuses
 * to mmap a ring buffer on an inherited event with cpu=-1 (EINVAL), so a
 * single task-wide event cannot work -- this per-CPU layout is the same
 * approach perf record uses. */
static struct {
  bool initialized;
  bool running;
  int ncpus;

  /* Perf event file descriptors, per type per CPU */
  int perf_fd[PEBS_SAMPLE_TYPE_COUNT][PEBS_MAX_CPUS];

  /* Memory-mapped ring buffers, per type per CPU */
  struct perf_event_mmap_page *perf_page[PEBS_SAMPLE_TYPE_COUNT][PEBS_MAX_CPUS];
  size_t mmap_size;

  /* Collector thread */
  pthread_t collector_thread;
  volatile bool collector_running;

  /* Page access records (hash table) */
  pebs_page_record_t *records[PEBS_HASH_SIZE];
  pthread_rwlock_t records_lock;

  /* Statistics */
  _Atomic uint64_t total_samples;
  _Atomic uint64_t read_samples;
  _Atomic uint64_t write_samples;
  _Atomic uint64_t throttle_events;
  _Atomic uint64_t errors;
} pebs_state = {0};

/*============================================================================
 * INTERNAL FUNCTIONS
 *===========================================================================*/

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static inline size_t hash_addr(uint64_t addr) {
  /* Use page frame number for hashing */
  uint64_t pfn = addr >> 12;
  const uint64_t golden = 0x9E3779B97F4A7C15ULL;
  return (size_t)((pfn * golden) % PEBS_HASH_SIZE);
}

static inline uint64_t page_align_addr(uint64_t addr) {
  return addr & ~(PAGE_SIZE - 1);
}

/*
 * Page sampling divisor (LDOS_PAGE_SAMPLE_DIVISOR, default 1 = every page).
 * When N > 1, only pages whose page-frame number satisfies pfn % N == 0 are
 * tracked.  This bounds the page-stats table (and therefore signal-compute
 * and CSV-export cost) on huge workloads: a 12GB graph is ~3M pages, but the
 * policy thread can only sustain phase-1 cadence at ~10-50K tracked pages.
 * Sampling pages -- rather than truncating at the table cap -- keeps the
 * tracked set spread uniformly across the whole region.
 */
static uint64_t g_page_sample_divisor = 1;

static inline int page_is_sampled(uint64_t aligned_addr) {
  if (g_page_sample_divisor <= 1) return 1;
  return ((aligned_addr >> 12) % g_page_sample_divisor) == 0;
}

/* Minimum lifetime samples before a page earns a stats entry
 * (LDOS_MIN_SAMPLES_TO_TRACK, default 1 = admit on first sample).
 * See the admission check in pebs_merge_with_page_stats(). */
static uint64_t g_min_samples_to_track = 1;

static pebs_page_record_t *get_or_create_record(uint64_t vaddr) {
  uint64_t aligned = page_align_addr(vaddr);
  size_t bucket = hash_addr(aligned);

  /* Read-only lookup */
  pthread_rwlock_rdlock(&pebs_state.records_lock);
  pebs_page_record_t *rec = pebs_state.records[bucket];
  while (rec != NULL) {
    if (rec->vaddr == aligned) {
      pthread_rwlock_unlock(&pebs_state.records_lock);
      return rec;
    }
    rec = rec->next;
  }
  pthread_rwlock_unlock(&pebs_state.records_lock);

  /* Create new record */
  pthread_rwlock_wrlock(&pebs_state.records_lock);

  /* Double-check */
  rec = pebs_state.records[bucket];
  while (rec != NULL) {
    if (rec->vaddr == aligned) {
      pthread_rwlock_unlock(&pebs_state.records_lock);
      return rec;
    }
    rec = rec->next;
  }

  rec = (pebs_page_record_t *)calloc(1, sizeof(pebs_page_record_t));
  if (rec == NULL) {
    pthread_rwlock_unlock(&pebs_state.records_lock);
    return NULL;
  }

  rec->vaddr = aligned;
  rec->next = pebs_state.records[bucket];
  pebs_state.records[bucket] = rec;

  pthread_rwlock_unlock(&pebs_state.records_lock);
  return rec;
}

static int setup_perf_event(__u64 config, __u64 config1, int precise, int cpu,
                            int *fd_out,
                            struct perf_event_mmap_page **page_out) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof(attr));

  attr.type = PERF_TYPE_RAW;
  attr.size = sizeof(struct perf_event_attr);
  attr.config = config;
  attr.config1 = config1;
  attr.sample_period = PEBS_SAMPLE_PERIOD;
  attr.sample_type =
      PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_ADDR;
  attr.disabled = 1; /* Start disabled */
  attr.inherit = 1;  /* Sample threads created after open (pthread/OpenMP
                      * workers), not just the opening thread.  Inherited
                      * events require per-CPU opens (cpu >= 0): the kernel
                      * refuses ring-buffer mmap on inherited cpu=-1 events. */
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.exclude_callchain_kernel = 1;
  attr.exclude_callchain_user = 1;
  attr.precise_ip = precise; /* Load latency facility needs 2; stores use 1 */

  int fd = perf_event_open(&attr, 0, cpu, -1, 0);
  while (fd == -1 && attr.precise_ip > 1) {
    /* Some kernels/CPUs reject precise_ip=2; degrade and retry. */
    attr.precise_ip--;
    fd = perf_event_open(&attr, 0, cpu, -1, 0);
  }
  if (fd == -1) {
    TM_ERROR("perf_event_open failed: %s (config=0x%llx cpu=%d)",
             strerror(errno), (unsigned long long)config, cpu);
    return -1;
  }

  size_t mmap_size = sysconf(_SC_PAGESIZE) * PEBS_BUFFER_PAGES;
  struct perf_event_mmap_page *page =
      mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (page == MAP_FAILED) {
    TM_ERROR("mmap for perf buffer failed: %s (cpu=%d)", strerror(errno), cpu);
    close(fd);
    return -1;
  }

  *fd_out = fd;
  *page_out = page;
  pebs_state.mmap_size = mmap_size;

  return 0;
}

/* Release every open event fd + ring buffer (init failure and shutdown). */
static void pebs_close_all_events(void) {
  for (int t = 0; t < PEBS_SAMPLE_TYPE_COUNT; t++) {
    for (int c = 0; c < pebs_state.ncpus; c++) {
      if (pebs_state.perf_page[t][c] != NULL) {
        munmap(pebs_state.perf_page[t][c], pebs_state.mmap_size);
        pebs_state.perf_page[t][c] = NULL;
      }
      if (pebs_state.perf_fd[t][c] > 0) {
        close(pebs_state.perf_fd[t][c]);
        pebs_state.perf_fd[t][c] = 0;
      }
    }
  }
}

static void process_sample(struct perf_sample *ps, pebs_sample_type_t type) {
  if (ps->addr == 0)
    return;

  /* Page-sampling: drop samples for non-selected pages up front so the
   * record table (and everything downstream) stays bounded. */
  if (!page_is_sampled(page_align_addr(ps->addr)))
    return;

  pebs_page_record_t *rec = get_or_create_record(ps->addr);
  if (rec == NULL) {
    atomic_fetch_add(&pebs_state.errors, 1);
    return;
  }

  /* Update record atomically where possible */
  if (type == PEBS_SAMPLE_READ) {
    __sync_fetch_and_add(&rec->read_samples, 1);
    atomic_fetch_add(&pebs_state.read_samples, 1);
  } else {
    __sync_fetch_and_add(&rec->write_samples, 1);
    atomic_fetch_add(&pebs_state.write_samples, 1);
  }

  __sync_fetch_and_add(&rec->total_latency, ps->weight);
  rec->last_sample_ns = get_time_ns();

  atomic_fetch_add(&pebs_state.total_samples, 1);
}

/*
 * Copy `len` bytes starting at ring offset `off`, handling wrap-around.
 * The perf ring buffer is circular: a record whose start is within
 * sizeof(record) bytes of the top continues at the bottom.  Reading fields
 * in place past the end of the mapping is a segfault (observed on the
 * twitter run: fault at mapping_end+0x8 dereferencing sample->addr).
 */
static void ring_copy(void *dst, const char *pbuf, uint64_t size,
                      uint64_t off, size_t len) {
  if (off + len <= size) {
    memcpy(dst, pbuf + off, len);
  } else {
    size_t first = (size_t)(size - off);
    memcpy(dst, pbuf + off, first);
    memcpy((char *)dst + first, pbuf, len - first);
  }
}

static void drain_buffer(pebs_sample_type_t type, int cpu) {
  struct perf_event_mmap_page *p = pebs_state.perf_page[type][cpu];
  if (p == NULL)
    return;

  char *pbuf = (char *)p + p->data_offset;
  uint64_t size = p->data_size;

  uint64_t head = p->data_head;
  __sync_synchronize(); /* rmb: read data_head before reading record bytes */
  uint64_t tail = p->data_tail;

  while (tail != head) {
    uint64_t off = tail % size;

    struct perf_event_header hdr;
    ring_copy(&hdr, pbuf, size, off, sizeof(hdr));

    if (hdr.size == 0) /* corrupt record; stop rather than spin forever */
      break;

    switch (hdr.type) {
    case PERF_RECORD_SAMPLE: {
      struct perf_sample sample;
      size_t want = hdr.size < sizeof(sample) ? hdr.size : sizeof(sample);
      ring_copy(&sample, pbuf, size, off, want);
      if (want >= sizeof(sample))
        process_sample(&sample, type);
      break;
    }

    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      atomic_fetch_add(&pebs_state.throttle_events, 1);
      break;

    default:
      /* Ignore unknown record types */
      break;
    }

    tail += hdr.size;
  }

  __sync_synchronize(); /* finish reading records before releasing space */
  p->data_tail = tail;
}

static void *collector_thread_fn(void *arg) {
  (void)arg;

  TM_INFO("PEBS collector thread started");

  while (pebs_state.collector_running) {
    for (int i = 0; i < PEBS_SAMPLE_TYPE_COUNT; i++) {
      for (int c = 0; c < pebs_state.ncpus; c++) {
        drain_buffer(i, c);
      }
    }
    usleep(1000); /* 1ms polling interval */
  }

  TM_INFO("PEBS collector thread stopped");
  return NULL;
}

/*============================================================================
 * PUBLIC API
 *===========================================================================*/

int pebs_init(void) {
  if (pebs_state.initialized) {
    TM_INFO("PEBS already initialized");
    return 0;
  }

  TM_INFO("Initializing PEBS subsystem...");

  /* Optional page sampling to bound tracked-page count on huge workloads. */
  const char *divisor_env = getenv("LDOS_PAGE_SAMPLE_DIVISOR");
  if (divisor_env != NULL) {
    long v = atol(divisor_env);
    if (v > 1) {
      g_page_sample_divisor = (uint64_t)v;
      TM_INFO("Page sampling enabled: tracking 1 of every %ld pages", v);
    }
  }

  /* Optional admission threshold to keep sparse cold pages out of the
   * stats table (holds the 50ms signal cadence on huge workloads). */
  const char *min_env = getenv("LDOS_MIN_SAMPLES_TO_TRACK");
  if (min_env != NULL) {
    long v = atol(min_env);
    if (v > 1) {
      g_min_samples_to_track = (uint64_t)v;
      TM_INFO("Track admission threshold: %ld samples before a page "
              "enters the stats table", v);
    }
  }

  /* Initialize lock */
  if (pthread_rwlock_init(&pebs_state.records_lock, NULL) != 0) {
    TM_ERROR("Failed to init records lock");
    return -1;
  }

  /* One event per CPU per type (inherit=1 requires per-CPU ring buffers). */
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  pebs_state.ncpus = (online > PEBS_MAX_CPUS) ? PEBS_MAX_CPUS : (int)online;
  TM_INFO("PEBS: opening events on %d CPUs (loads + stores)",
          pebs_state.ncpus);

  for (int c = 0; c < pebs_state.ncpus; c++) {
    /* Read sampling: load latency facility (guaranteed DataLA capture) */
    if (setup_perf_event(PEBS_EVENT_MEM_LOADS, PEBS_LOAD_LATENCY_THRESHOLD, 2,
                         c, &pebs_state.perf_fd[PEBS_SAMPLE_READ][c],
                         &pebs_state.perf_page[PEBS_SAMPLE_READ][c]) < 0) {
      TM_ERROR("Failed to setup PEBS loads on cpu %d - PEBS may be unavailable", c);
      TM_INFO("Check: Intel CPU with PEBS, perf_event_paranoid <= 2");
      pebs_close_all_events();
      pthread_rwlock_destroy(&pebs_state.records_lock);
      return -1;
    }

    /* Write sampling (memory stores) */
    if (setup_perf_event(PEBS_EVENT_MEM_STORES, 0, 1,
                         c, &pebs_state.perf_fd[PEBS_SAMPLE_WRITE][c],
                         &pebs_state.perf_page[PEBS_SAMPLE_WRITE][c]) < 0) {
      TM_ERROR("Failed to setup PEBS stores on cpu %d", c);
      pebs_close_all_events();
      pthread_rwlock_destroy(&pebs_state.records_lock);
      return -1;
    }
  }

  pebs_state.initialized = true;
  TM_INFO("PEBS initialized successfully");

  return 0;
}

void pebs_shutdown(void) {
  if (!pebs_state.initialized)
    return;

  TM_INFO("Shutting down PEBS...");

  pebs_stop();

  /* Cleanup perf resources (all per-CPU events) */
  pebs_close_all_events();

  pebs_clear_records();
  pthread_rwlock_destroy(&pebs_state.records_lock);

  pebs_state.initialized = false;
  TM_INFO("PEBS shutdown complete");
}

int pebs_start(void) {
  if (!pebs_state.initialized) {
    TM_ERROR("PEBS not initialized");
    return -1;
  }

  if (pebs_state.running) {
    return 0; /* Already running */
  }

  TM_INFO("Starting PEBS sampling...");

  /* Enable perf events */
  for (int i = 0; i < PEBS_SAMPLE_TYPE_COUNT; i++) {
    for (int c = 0; c < pebs_state.ncpus; c++) {
      if (ioctl(pebs_state.perf_fd[i][c], PERF_EVENT_IOC_ENABLE, 0) < 0) {
        TM_ERROR("Failed to enable perf event %d cpu %d: %s", i, c,
                 strerror(errno));
        return -1;
      }
    }
  }

  /* Start collector thread */
  pebs_state.collector_running = true;
  if (pthread_create(&pebs_state.collector_thread, NULL, collector_thread_fn,
                     NULL) != 0) {
    TM_ERROR("Failed to create collector thread: %s", strerror(errno));
    pebs_state.collector_running = false;
    return -1;
  }

  pebs_state.running = true;
  TM_INFO("PEBS sampling started");

  return 0;
}

void pebs_stop(void) {
  if (!pebs_state.running)
    return;

  TM_INFO("Stopping PEBS sampling...");

  /* Stop collector thread */
  pebs_state.collector_running = false;
  pthread_join(pebs_state.collector_thread, NULL);

  /* Disable perf events */
  for (int i = 0; i < PEBS_SAMPLE_TYPE_COUNT; i++) {
    for (int c = 0; c < pebs_state.ncpus; c++) {
      ioctl(pebs_state.perf_fd[i][c], PERF_EVENT_IOC_DISABLE, 0);
    }
  }

  pebs_state.running = false;
  TM_INFO("PEBS sampling stopped");
}

bool pebs_is_active(void) {
  return pebs_state.initialized && pebs_state.running;
}

pebs_page_record_t *pebs_get_page_record(void *page_addr) {
  if (!pebs_state.initialized)
    return NULL;

  uint64_t aligned = page_align_addr((uint64_t)page_addr);
  size_t bucket = hash_addr(aligned);

  pthread_rwlock_rdlock(&pebs_state.records_lock);
  pebs_page_record_t *rec = pebs_state.records[bucket];
  while (rec != NULL) {
    if (rec->vaddr == aligned) {
      pthread_rwlock_unlock(&pebs_state.records_lock);
      return rec;
    }
    rec = rec->next;
  }
  pthread_rwlock_unlock(&pebs_state.records_lock);

  return NULL;
}

pebs_stats_t pebs_get_stats(void) {
  pebs_stats_t stats = {.total_samples = atomic_load(&pebs_state.total_samples),
                        .read_samples = atomic_load(&pebs_state.read_samples),
                        .write_samples = atomic_load(&pebs_state.write_samples),
                        .throttle_events =
                            atomic_load(&pebs_state.throttle_events),
                        .errors = atomic_load(&pebs_state.errors),
                        .active = pebs_state.running};
  return stats;
}

/*
 * Debug aid (LDOS_PEBS_DEBUG=1): print the top-N sampled pages so we can see
 * WHERE samples actually land -- compare against the "Registered region"
 * base+length lines in the run log to tell in-region from out-of-region.
 * Motivated by the twitter run: 196K read samples collected, zero pages
 * created, i.e. seemingly no samples inside the managed regions.
 */
void pebs_debug_dump_top(int topn) {
  if (!pebs_state.initialized || topn <= 0)
    return;
  if (topn > 64) topn = 64;

  pebs_page_record_t *top[64] = {0};
  uint64_t total_records = 0;

  pthread_rwlock_rdlock(&pebs_state.records_lock);
  for (size_t i = 0; i < PEBS_HASH_SIZE; i++) {
    for (pebs_page_record_t *rec = pebs_state.records[i]; rec != NULL;
         rec = rec->next) {
      total_records++;
      /* replace the current minimum if this record has more samples */
      int min_idx = 0;
      uint64_t min_val = UINT64_MAX;
      for (int k = 0; k < topn; k++) {
        uint64_t v = top[k] ? top[k]->read_samples + top[k]->write_samples : 0;
        if (v < min_val) { min_val = v; min_idx = k; }
      }
      if (rec->read_samples + rec->write_samples > min_val)
        top[min_idx] = rec;
    }
  }

  TM_INFO("PEBS DEBUG: %" PRIu64 " distinct sampled pages; top %d by samples:",
          total_records, topn);
  for (int k = 0; k < topn; k++) {
    if (top[k])
      TM_INFO("  vaddr=0x%" PRIx64 "  reads=%" PRIu64 "  writes=%" PRIu64,
              top[k]->vaddr, top[k]->read_samples, top[k]->write_samples);
  }
  pthread_rwlock_unlock(&pebs_state.records_lock);
}

void pebs_merge_with_page_stats(void) {
  if (!pebs_state.initialized)
    return;

  /* In telemetry-only mode page_stats entries are created exclusively here
   * (no uffd faults), and PEBS samples the WHOLE address space -- stack,
   * code, allocator metadata.  Snapshot the managed regions once so we only
   * admit pages that belong to a registered region.  In full-management
   * mode keep historical behavior (merge everything PEBS saw). */
  struct { void *base; size_t len; } regions[MAX_MANAGED_REGIONS];
  int nregions = 0;
  if (g_manager.telemetry_only) {
    pthread_mutex_lock(&g_manager.regions_lock);
    for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
      if (g_manager.regions[i].active) {
        regions[nregions].base = g_manager.regions[i].base_addr;
        regions[nregions].len  = g_manager.regions[i].length;
        nregions++;
      }
    }
    pthread_mutex_unlock(&g_manager.regions_lock);
  }

  pthread_rwlock_rdlock(&pebs_state.records_lock);

  for (size_t i = 0; i < PEBS_HASH_SIZE; i++) {
    pebs_page_record_t *rec = pebs_state.records[i];
    while (rec != NULL) {
      if (g_manager.telemetry_only) {
        int in_region = 0;
        for (int r = 0; r < nregions; r++) {
          if ((char *)rec->vaddr >= (char *)regions[r].base &&
              (char *)rec->vaddr < (char *)regions[r].base + regions[r].len) {
            in_region = 1;
            break;
          }
        }
        if (!in_region) {
          rec = rec->next;
          continue;
        }
      }

      /* Admission threshold (LDOS_MIN_SAMPLES_TO_TRACK): a page must
       * accumulate N lifetime samples before it earns a stats entry.
       * Purpose: hold the 50ms signal cadence on huge sparse workloads.
       * GUPS's 10% uniform traffic gives every page in a 2GB table an
       * occasional stray sample (~1 per page per 7min); admitting them all
       * grew the table to 159K pages and stretched the signal loop ~30x,
       * which invalidated fast-vs-slow rankings.  Hot pages clear a
       * threshold of 2-3 within a bar or two; one-sample cold pages never
       * enter.  Pages that already have an entry keep updating regardless,
       * so no history is lost.  Default 1 = previous behavior. */
      if (g_min_samples_to_track > 1 &&
          rec->read_samples + rec->write_samples < g_min_samples_to_track &&
          get_page_stats((void *)rec->vaddr) == NULL) {
        rec = rec->next;
        continue;
      }

      /* Get or create corresponding page_stats entry */
      page_stats_t *stats = get_or_create_page_stats((void *)rec->vaddr);
      if (stats != NULL) {
        /*
         * PEBS samples are statistical - multiply by sample period
         * to estimate true access count. However, for ML features
         * we use the raw sample counts as they're more stable.
         */
        uint64_t pebs_reads = rec->read_samples;
        uint64_t pebs_writes = rec->write_samples;

        /* Merge PEBS samples with userfaultfd counts */
        uint64_t current_reads = atomic_load(&stats->read_count);
        uint64_t current_writes = atomic_load(&stats->write_count);

        /* Use max of PEBS estimate and uffd count */
        uint64_t estimated_reads = pebs_reads * PEBS_SAMPLE_PERIOD;
        uint64_t estimated_writes = pebs_writes * PEBS_SAMPLE_PERIOD;

        if (estimated_reads > current_reads) {
          atomic_store(&stats->read_count, estimated_reads);
        }
        if (estimated_writes > current_writes) {
          atomic_store(&stats->write_count, estimated_writes);
        }

        /* Update access count */
        uint64_t total =
            atomic_load(&stats->read_count) + atomic_load(&stats->write_count);
        atomic_store(&stats->access_count, total);

        /* Update last access time if PEBS saw more recent activity */
        if (rec->last_sample_ns > atomic_load(&stats->last_access_ns)) {
          atomic_store(&stats->last_access_ns, rec->last_sample_ns);
        }
      }
      rec = rec->next;
    }
  }

  pthread_rwlock_unlock(&pebs_state.records_lock);
}

void pebs_clear_records(void) {
  pthread_rwlock_wrlock(&pebs_state.records_lock);

  for (size_t i = 0; i < PEBS_HASH_SIZE; i++) {
    pebs_page_record_t *rec = pebs_state.records[i];
    while (rec != NULL) {
      pebs_page_record_t *next = rec->next;
      free(rec);
      rec = next;
    }
    pebs_state.records[i] = NULL;
  }

  /* Reset stats */
  atomic_store(&pebs_state.total_samples, 0);
  atomic_store(&pebs_state.read_samples, 0);
  atomic_store(&pebs_state.write_samples, 0);
  atomic_store(&pebs_state.throttle_events, 0);
  atomic_store(&pebs_state.errors, 0);

  pthread_rwlock_unlock(&pebs_state.records_lock);
}

void pebs_print_status(void) {
  pebs_stats_t stats = pebs_get_stats();

  TM_INFO("=== PEBS Status ===");
  TM_INFO("  Active: %s", stats.active ? "yes" : "no");
  TM_INFO("  Total samples: %lu", stats.total_samples);
  TM_INFO("  Read samples: %lu", stats.read_samples);
  TM_INFO("  Write samples: %lu", stats.write_samples);
  TM_INFO("  Throttle events: %lu", stats.throttle_events);
  TM_INFO("  Errors: %lu", stats.errors);

  /* Count unique pages */
  size_t unique_pages = 0;
  pthread_rwlock_rdlock(&pebs_state.records_lock);
  for (size_t i = 0; i < PEBS_HASH_SIZE; i++) {
    pebs_page_record_t *rec = pebs_state.records[i];
    while (rec != NULL) {
      unique_pages++;
      rec = rec->next;
    }
  }
  pthread_rwlock_unlock(&pebs_state.records_lock);

  TM_INFO("  Unique pages sampled: %zu", unique_pages);
}

#endif /* __linux__ */
