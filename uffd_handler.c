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
      .features = 0 /* Request minimal features for compatibility */
  };

  if (ioctl(g_manager.uffd, UFFDIO_API, &uffdio_api) < 0) {
    TM_ERROR("UFFDIO_API ioctl failed: %s", strerror(errno));
    TM_ERROR("Kernel may not support userfaultfd properly");
    close(g_manager.uffd);
    g_manager.uffd = -1;
    return -1;
  }

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

  struct uffdio_register uffdio_register = {
      .range = {.start = (unsigned long)addr, .len = length},
      .mode = UFFDIO_REGISTER_MODE_MISSING};

  if (ioctl(g_manager.uffd, UFFDIO_REGISTER, &uffdio_register) < 0) {
    TM_ERROR("UFFDIO_REGISTER failed for %p+%zu: %s", addr, length,
             strerror(errno));
    pthread_mutex_unlock(&g_manager.regions_lock);
    return -1;
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
      struct uffdio_range range = {.start = (unsigned long)addr,
                                   .len = g_manager.regions[i].length};
      ioctl(g_manager.uffd, UFFDIO_UNREGISTER, &range);
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
        memory_tier_t tier = decide_initial_placement(fault_addr);
        resolve_page_fault(fault_addr, tier);
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

void cleanup_userfaultfd(void) {
  pthread_mutex_lock(&g_manager.regions_lock);
  for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
    if (g_manager.regions[i].active) {
      struct uffdio_range range = {
          .start = (unsigned long)g_manager.regions[i].base_addr,
          .len = g_manager.regions[i].length};
      ioctl(g_manager.uffd, UFFDIO_UNREGISTER, &range);
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
