/*
 * uffd_handler.c - Userfaultfd Page Fault Handler
 *
 * Background thread that handles page faults via Linux userfaultfd.
 * On fault, decides tier placement (DRAM or NVM) and resolves with UFFDIO_COPY.
 *
 * Requires: /proc/sys/vm/unprivileged_userfaultfd = 1
 *
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include "tiered_memory.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static void *uffd_handler_thread(void *arg);

/*============================================================================
 * USERFAULTFD INITIALIZATION
 *===========================================================================*/

int init_userfaultfd(void) {
  g_manager.uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (g_manager.uffd < 0) {
    TM_ERROR("userfaultfd syscall failed: %s", strerror(errno));
    TM_ERROR(
        "Make sure you're running on Linux >= 4.3 and have CAP_SYS_PTRACE");
    return -1;
  }

  struct uffdio_api uffdio_api = {
      .api = UFFD_API,
      .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP
  };

  if (ioctl(g_manager.uffd, UFFDIO_API, &uffdio_api) < 0) {
    TM_ERROR("UFFDIO_API ioctl failed: %s", strerror(errno));
    TM_ERROR("Kernel may not support userfaultfd properly");
    close(g_manager.uffd);
    g_manager.uffd = -1;
    return -1;
  }

  g_manager.uffd_wp_supported =
      !!(uffdio_api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP);
  TM_INFO("UFFD write-protect tracking: %s",
          g_manager.uffd_wp_supported ? "enabled" : "unavailable");

  TM_DEBUG("UFFD API version: %llu, features: 0x%llx",
           (unsigned long long)uffdio_api.api,
           (unsigned long long)uffdio_api.features);

  TM_INFO("Userfaultfd initialized (fd=%d)", g_manager.uffd);
  return 0;
}

int start_uffd_handler(void) {
  if (pthread_create(&g_manager.uffd_thread, NULL, uffd_handler_thread, NULL) !=
      0) {
    TM_ERROR("Failed to create UFFD handler thread: %s", strerror(errno));
    return -1;
  }
  TM_INFO("UFFD handler thread started");
  return 0;
}

/*============================================================================
 * REGION REGISTRATION
 *===========================================================================*/

int register_managed_region(void *addr, size_t length) {
  if (g_manager.uffd < 0) {
    TM_ERROR("Userfaultfd not initialized");
    return -1;
  }

  pthread_mutex_lock(&g_manager.regions_lock);

  int slot = -1;
  for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
    if (!g_manager.regions[i].active) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    TM_ERROR("No free region slots (max=%d)", MAX_MANAGED_REGIONS);
    pthread_mutex_unlock(&g_manager.regions_lock);
    return -1;
  }

  /* Telemetry-only mode: record the region (so PEBS merge can filter to it)
   * but do NOT register with userfaultfd -- pages fault in natively at full
   * speed and access data comes exclusively from PEBS sampling. */
  if (!g_manager.telemetry_only) {
    struct uffdio_register uffdio_register = {
        .range = {.start = (unsigned long)addr, .len = length},
        .mode = UFFDIO_REGISTER_MODE_MISSING |
                (g_manager.uffd_wp_supported ? UFFDIO_REGISTER_MODE_WP : 0)};

    if (ioctl(g_manager.uffd, UFFDIO_REGISTER, &uffdio_register) < 0) {
      TM_ERROR("UFFDIO_REGISTER failed for %p+%zu: %s", addr, length,
               strerror(errno));
      pthread_mutex_unlock(&g_manager.regions_lock);
      return -1;
    }
  }

  g_manager.regions[slot] = (managed_region_t){.base_addr = addr,
                                               .length = length,
                                               .uffd = g_manager.uffd,
                                               .active = true};
  g_manager.region_count++;

  pthread_mutex_unlock(&g_manager.regions_lock);
  TM_INFO("Registered region: %p + %zu bytes (slot %d)", addr, length, slot);
  return 0;
}

void unregister_managed_region(void *addr) {
  pthread_mutex_lock(&g_manager.regions_lock);
  for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
    if (g_manager.regions[i].active && g_manager.regions[i].base_addr == addr) {
      if (!g_manager.telemetry_only) {
        struct uffdio_range range = {.start = (unsigned long)addr,
                                     .len = g_manager.regions[i].length};
        ioctl(g_manager.uffd, UFFDIO_UNREGISTER, &range);
      }
      g_manager.regions[i].active = false;
      g_manager.region_count--;
      TM_INFO("Unregistered region: %p", addr);
      break;
    }
  }
  pthread_mutex_unlock(&g_manager.regions_lock);
}

/*============================================================================
 * FAULT HANDLING
 *===========================================================================*/

/* Initial placement policy: DRAM first, fall back to NVM if full */
static memory_tier_t decide_initial_placement(void *fault_addr) {
  (void)fault_addr; /* Reserved for ML-based placement */

  tier_config_t *dram = &g_manager.tiers[TIER_DRAM];
  tier_config_t *nvm = &g_manager.tiers[TIER_NVM];

  if (dram->used + PAGE_SIZE <= dram->capacity)
    return TIER_DRAM;
  if (nvm->used + PAGE_SIZE <= nvm->capacity)
    return TIER_NVM;

  TM_ERROR("Both tiers full!");
  return TIER_DRAM;
}

static int resolve_page_fault(void *fault_addr, memory_tier_t tier) {
  void *page_addr = page_align(fault_addr);
  tier_config_t *tier_config = &g_manager.tiers[tier];

  static __thread char zero_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
  memset(zero_page, 0, PAGE_SIZE);

  struct uffdio_copy uffdio_copy = {.dst = (unsigned long)page_addr,
                                    .src = (unsigned long)zero_page,
                                    .len = PAGE_SIZE,
                                    .mode = 0};

  if (ioctl(g_manager.uffd, UFFDIO_COPY, &uffdio_copy) < 0) {
    if (errno == EEXIST)
      return 0; /* Race condition, harmless */
    TM_ERROR("UFFDIO_COPY failed for %p: %s", page_addr, strerror(errno));
    return -1;
  }

  tier_config->used += PAGE_SIZE;

  page_stats_t *stats = get_or_create_page_stats(page_addr);
  if (stats) {
    stats->current_tier = tier;
    record_page_access(page_addr, false);
  }

  /* Write-protect the page so every subsequent write faults back here,
   * letting us increment access_count for true access frequency tracking. */
  if (g_manager.uffd_wp_supported) {
    struct uffdio_writeprotect wp = {
        .range = {.start = (unsigned long)page_addr, .len = PAGE_SIZE},
        .mode = UFFDIO_WRITEPROTECT_MODE_WP};
    if (ioctl(g_manager.uffd, UFFDIO_WRITEPROTECT, &wp) < 0)
      TM_ERROR("UFFDIO_WRITEPROTECT failed for %p: %s", page_addr, strerror(errno));
  }

  /* Update region stats */
  pthread_mutex_lock(&g_manager.regions_lock);
  for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
    managed_region_t *r = &g_manager.regions[i];
    if (r->active && page_addr >= r->base_addr &&
        page_addr < r->base_addr + r->length) {
      atomic_fetch_add(&r->total_faults, 1);
      if (tier == TIER_DRAM)
        atomic_fetch_add(&r->pages_in_dram, 1);
      else
        atomic_fetch_add(&r->pages_in_nvm, 1);
      break;
    }
  }
  pthread_mutex_unlock(&g_manager.regions_lock);

  atomic_fetch_add(&g_manager.total_faults, 1);
  TM_DEBUG("Resolved fault at %p -> %s", page_addr,
           tier == TIER_DRAM ? "DRAM" : "NVM");
  return 0;
}

/*============================================================================
 * HANDLER THREAD
 *===========================================================================*/

static void *uffd_handler_thread(void *arg) {
  (void)arg;
  TM_INFO("UFFD handler thread running");

  struct pollfd pollfd = {.fd = g_manager.uffd, .events = POLLIN};

  while (g_manager.threads_running) {
    int ret = poll(&pollfd, 1, 100);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      TM_ERROR("poll() failed: %s", strerror(errno));
      break;
    }
    if (ret == 0)
      continue;

    if (pollfd.revents & POLLERR) {
      TM_ERROR("POLLERR on userfaultfd");
      break;
    }

    if (pollfd.revents & POLLIN) {
      struct uffd_msg msg;
      ssize_t nread = read(g_manager.uffd, &msg, sizeof(msg));

      if (nread < 0) {
        if (errno == EAGAIN)
          continue;
        TM_ERROR("read() failed: %s", strerror(errno));
        break;
      }
      if (nread != sizeof(msg))
        continue;

      if (msg.event == UFFD_EVENT_PAGEFAULT) {
        void *fault_addr = (void *)msg.arg.pagefault.address;

        if (g_manager.uffd_wp_supported &&
            (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP)) {
          /* Repeat write to a tracked page: count it, then re-protect. */
          record_page_access(fault_addr, true);

          void *page = page_align(fault_addr);
          struct uffdio_writeprotect wp = {
              .range = {.start = (unsigned long)page, .len = PAGE_SIZE},
              .mode = 0};
          ioctl(g_manager.uffd, UFFDIO_WRITEPROTECT, &wp); /* clear — lets write proceed */
          /* Do NOT re-protect here; the policy thread re-arms all pages
           * on a timer so we sample at a controlled rate, not on every write. */
        } else {
          /* First access (missing page): place and resolve. */
          memory_tier_t tier = decide_initial_placement(fault_addr);
          resolve_page_fault(fault_addr, tier);
          /* Patch up read/write split now that we know the fault cause. */
          if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) {
            page_stats_t *s = get_page_stats(page_align(fault_addr));
            if (s) {
              atomic_fetch_add(&s->write_count, 1);
              atomic_fetch_sub(&s->read_count, 1);
            }
          }
        }
      }
    }
  }

  TM_INFO("UFFD handler thread exiting");
  return NULL;
}

void stop_uffd_handler(void) {
  pthread_join(g_manager.uffd_thread, NULL);
  TM_INFO("UFFD handler thread stopped");
}

/*
 * reprotect_all_tracked_pages - Re-arm write-protection on every tracked page.
 *
 * Called by the policy thread every WP_RESAMPLE_CYCLES to open a new
 * sampling window.  Pages that were written since the last call will have
 * had their WP cleared; re-protecting them here means the next write will
 * fault again and increment access_count.  Pages already WP (never written
 * since last arm) silently return ENOTSUP/EBUSY from the ioctl — that is
 * harmless and expected.
 */
void reprotect_all_tracked_pages(void) {
  if (!g_manager.uffd_wp_supported) return;
  if (g_manager.telemetry_only) return;  /* no WP registration to re-arm */

  pthread_rwlock_rdlock(&g_manager.stats_lock);
  for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
    page_stats_t *entry = g_manager.page_stats_table[i];
    while (entry != NULL) {
      struct uffdio_writeprotect wp = {
          .range = {.start = (unsigned long)entry->page_addr, .len = PAGE_SIZE},
          .mode = UFFDIO_WRITEPROTECT_MODE_WP};
      /* Ignore errors: page may already be WP or not yet mapped. */
      ioctl(g_manager.uffd, UFFDIO_WRITEPROTECT, &wp);
      entry = entry->next;
    }
  }
  pthread_rwlock_unlock(&g_manager.stats_lock);
}

void cleanup_userfaultfd(void) {
  pthread_mutex_lock(&g_manager.regions_lock);
  for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
    if (g_manager.regions[i].active) {
      if (!g_manager.telemetry_only) {
        struct uffdio_range range = {
            .start = (unsigned long)g_manager.regions[i].base_addr,
            .len = g_manager.regions[i].length};
        ioctl(g_manager.uffd, UFFDIO_UNREGISTER, &range);
      }
      g_manager.regions[i].active = false;
    }
  }
  g_manager.region_count = 0;
  pthread_mutex_unlock(&g_manager.regions_lock);

  if (g_manager.uffd >= 0) {
    close(g_manager.uffd);
    g_manager.uffd = -1;
  }
  TM_INFO("Userfaultfd cleaned up");
}
