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

/* PEBS state */
static struct {
  bool initialized;
  bool running;

  /* Perf event file descriptors */
  int perf_fd[PEBS_SAMPLE_TYPE_COUNT];

  /* Memory-mapped ring buffers */
  struct perf_event_mmap_page *perf_page[PEBS_SAMPLE_TYPE_COUNT];
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

static int setup_perf_event(__u64 config, __u64 config1, int *fd_out,
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
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.exclude_callchain_kernel = 1;
  attr.exclude_callchain_user = 1;
  attr.precise_ip = 1; /* Request PEBS */

  int fd = perf_event_open(&attr, 0, -1, -1, 0);
  if (fd == -1) {
    TM_ERROR("perf_event_open failed: %s (config=0x%llx)", strerror(errno),
             (unsigned long long)config);
    return -1;
  }

  size_t mmap_size = sysconf(_SC_PAGESIZE) * PEBS_BUFFER_PAGES;
  struct perf_event_mmap_page *page =
      mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (page == MAP_FAILED) {
    TM_ERROR("mmap for perf buffer failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  *fd_out = fd;
  *page_out = page;
  pebs_state.mmap_size = mmap_size;

  return 0;
}

static void process_sample(struct perf_sample *ps, pebs_sample_type_t type) {
  if (ps->addr == 0)
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

static void drain_buffer(pebs_sample_type_t type) {
  struct perf_event_mmap_page *p = pebs_state.perf_page[type];
  if (p == NULL)
    return;

  char *pbuf = (char *)p + p->data_offset;

  __sync_synchronize();

  while (p->data_head != p->data_tail) {
    struct perf_event_header *hdr =
        (void *)(pbuf + (p->data_tail % p->data_size));

    switch (hdr->type) {
    case PERF_RECORD_SAMPLE:
      process_sample((struct perf_sample *)hdr, type);
      break;

    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      atomic_fetch_add(&pebs_state.throttle_events, 1);
      break;

    default:
      /* Ignore unknown record types */
      break;
    }

    p->data_tail += hdr->size;
  }
}

static void *collector_thread_fn(void *arg) {
  (void)arg;

  TM_INFO("PEBS collector thread started");

  while (pebs_state.collector_running) {
    for (int i = 0; i < PEBS_SAMPLE_TYPE_COUNT; i++) {
      drain_buffer(i);
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

  /* Initialize lock */
  if (pthread_rwlock_init(&pebs_state.records_lock, NULL) != 0) {
    TM_ERROR("Failed to init records lock");
    return -1;
  }

  /* Setup read sampling (memory loads) */
  if (setup_perf_event(PEBS_EVENT_MEM_LOADS, 0,
                       &pebs_state.perf_fd[PEBS_SAMPLE_READ],
                       &pebs_state.perf_page[PEBS_SAMPLE_READ]) < 0) {
    TM_ERROR("Failed to setup PEBS for reads - PEBS may be unavailable");
    TM_INFO("Check: Intel CPU with PEBS, perf_event_paranoid <= 2");
    pthread_rwlock_destroy(&pebs_state.records_lock);
    return -1;
  }

  /* Setup write sampling (memory stores) */
  if (setup_perf_event(PEBS_EVENT_MEM_STORES, 0,
                       &pebs_state.perf_fd[PEBS_SAMPLE_WRITE],
                       &pebs_state.perf_page[PEBS_SAMPLE_WRITE]) < 0) {
    TM_ERROR("Failed to setup PEBS for writes");
    /* Cleanup read fd */
    munmap(pebs_state.perf_page[PEBS_SAMPLE_READ], pebs_state.mmap_size);
    close(pebs_state.perf_fd[PEBS_SAMPLE_READ]);
    pthread_rwlock_destroy(&pebs_state.records_lock);
    return -1;
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

  /* Cleanup perf resources */
  for (int i = 0; i < PEBS_SAMPLE_TYPE_COUNT; i++) {
    if (pebs_state.perf_page[i] != NULL) {
      munmap(pebs_state.perf_page[i], pebs_state.mmap_size);
      pebs_state.perf_page[i] = NULL;
    }
    if (pebs_state.perf_fd[i] > 0) {
      close(pebs_state.perf_fd[i]);
      pebs_state.perf_fd[i] = 0;
    }
  }

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
    if (ioctl(pebs_state.perf_fd[i], PERF_EVENT_IOC_ENABLE, 0) < 0) {
      TM_ERROR("Failed to enable perf event %d: %s", i, strerror(errno));
      return -1;
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
    ioctl(pebs_state.perf_fd[i], PERF_EVENT_IOC_DISABLE, 0);
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

void pebs_merge_with_page_stats(void) {
  if (!pebs_state.initialized)
    return;

  pthread_rwlock_rdlock(&pebs_state.records_lock);

  for (size_t i = 0; i < PEBS_HASH_SIZE; i++) {
    pebs_page_record_t *rec = pebs_state.records[i];
    while (rec != NULL) {
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
