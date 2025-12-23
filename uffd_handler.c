/*
 * uffd_handler.c - Userfaultfd Page Fault Handler
 * 
 * This module implements the "Controller" - a background thread that
 * handles page faults via Linux's userfaultfd mechanism.
 * 
 * Key Concepts:
 *   - When a managed region is accessed, the kernel generates a page fault
 *   - This thread receives the fault via the userfaultfd file descriptor
 *   - We resolve the fault by copying data into either DRAM or NVM tier
 *   - Page access is recorded for the ML model
 * 
 * The initial placement decision can be:
 *   1. Always DRAM first (simple, then demote cold pages)
 *   2. Based on allocation context hints
 *   3. ML-predicted based on similar pages' behavior
 * 
 * Currently implements strategy #1 for simplicity.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/userfaultfd.h>
#include "tiered_memory.h"

/* Forward declaration */
static void* uffd_handler_thread(void *arg);

/*============================================================================
 * USERFAULTFD INITIALIZATION
 *===========================================================================*/

/*
 * Initialize the userfaultfd subsystem.
 * Creates the uffd file descriptor and configures it.
 */
int init_userfaultfd(void) {
    /* Create userfaultfd */
    g_manager.uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (g_manager.uffd < 0) {
        TM_ERROR("userfaultfd syscall failed: %s", strerror(errno));
        TM_ERROR("Make sure you're running on Linux >= 4.3 and have CAP_SYS_PTRACE");
        return -1;
    }
    
    /* Configure the userfaultfd with API version */
    struct uffdio_api uffdio_api = {
        .api = UFFD_API,
        .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP |
                    UFFD_FEATURE_MISSING_SHMEM |
                    UFFD_FEATURE_MISSING_HUGETLBFS
    };
    
    if (ioctl(g_manager.uffd, UFFDIO_API, &uffdio_api) < 0) {
        TM_ERROR("UFFDIO_API ioctl failed: %s", strerror(errno));
        close(g_manager.uffd);
        g_manager.uffd = -1;
        return -1;
    }
    
    TM_INFO("Userfaultfd initialized (fd=%d)", g_manager.uffd);
    return 0;
}

/*
 * Start the userfaultfd handler thread.
 */
int start_uffd_handler(void) {
    if (pthread_create(&g_manager.uffd_thread, NULL, 
                       uffd_handler_thread, NULL) != 0) {
        TM_ERROR("Failed to create UFFD handler thread: %s", strerror(errno));
        return -1;
    }
    
    TM_INFO("UFFD handler thread started");
    return 0;
}

/*============================================================================
 * REGION REGISTRATION
 *===========================================================================*/

/*
 * Register a memory region with userfaultfd.
 * After registration, page faults in this region will be handled by our thread.
 */
int register_managed_region(void *addr, size_t length) {
    if (g_manager.uffd < 0) {
        TM_ERROR("Userfaultfd not initialized");
        return -1;
    }
    
    pthread_mutex_lock(&g_manager.regions_lock);
    
    /* Find an empty slot */
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
    
    /* Register with userfaultfd */
    struct uffdio_register uffdio_register = {
        .range = {
            .start = (unsigned long)addr,
            .len = length
        },
        .mode = UFFDIO_REGISTER_MODE_MISSING  /* Handle missing page faults */
    };
    
    if (ioctl(g_manager.uffd, UFFDIO_REGISTER, &uffdio_register) < 0) {
        TM_ERROR("UFFDIO_REGISTER failed for %p+%zu: %s", 
                 addr, length, strerror(errno));
        pthread_mutex_unlock(&g_manager.regions_lock);
        return -1;
    }
    
    /* Store region info */
    g_manager.regions[slot].base_addr = addr;
    g_manager.regions[slot].length = length;
    g_manager.regions[slot].uffd = g_manager.uffd;
    g_manager.regions[slot].active = true;
    atomic_store(&g_manager.regions[slot].total_faults, 0);
    atomic_store(&g_manager.regions[slot].pages_in_dram, 0);
    atomic_store(&g_manager.regions[slot].pages_in_nvm, 0);
    g_manager.region_count++;
    
    pthread_mutex_unlock(&g_manager.regions_lock);
    
    TM_INFO("Registered managed region: %p + %zu bytes (slot %d)", 
            addr, length, slot);
    
    return 0;
}

/*
 * Unregister a memory region from userfaultfd.
 */
void unregister_managed_region(void *addr) {
    pthread_mutex_lock(&g_manager.regions_lock);
    
    for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
        if (g_manager.regions[i].active && 
            g_manager.regions[i].base_addr == addr) {
            
            /* Unregister from userfaultfd */
            struct uffdio_range range = {
                .start = (unsigned long)addr,
                .len = g_manager.regions[i].length
            };
            
            ioctl(g_manager.uffd, UFFDIO_UNREGISTER, &range);
            
            g_manager.regions[i].active = false;
            g_manager.region_count--;
            
            TM_INFO("Unregistered managed region: %p", addr);
            break;
        }
    }
    
    pthread_mutex_unlock(&g_manager.regions_lock);
}

/*============================================================================
 * FAULT HANDLING
 *===========================================================================*/

/*
 * Determine which tier to place a new page in.
 * 
 * Current policy: Place in DRAM first (hot start).
 * 
 * Future: Could use ML prediction here based on:
 *   - Allocation context (call stack, allocation size)
 *   - Similar pages' behavior patterns
 *   - Current tier capacity
 */
static memory_tier_t decide_initial_placement(void *fault_addr) {
    tier_config_t *dram = &g_manager.tiers[TIER_DRAM];
    tier_config_t *nvm = &g_manager.tiers[TIER_NVM];
    
    /* Check if DRAM has capacity */
    if (dram->used + PAGE_SIZE <= dram->capacity) {
        return TIER_DRAM;
    }
    
    /* Fall back to NVM if DRAM is full */
    if (nvm->used + PAGE_SIZE <= nvm->capacity) {
        return TIER_NVM;
    }
    
    /* Both tiers full - should trigger eviction in real system */
    TM_ERROR("Both tiers full! Need eviction policy.");
    return TIER_DRAM;  /* Try DRAM anyway */
}

/*
 * Resolve a page fault by copying a zero page into the faulting address.
 * 
 * In a real system, this might:
 *   - Copy from backing store
 *   - Copy from another tier
 *   - Zero-fill for anonymous pages
 */
static int resolve_page_fault(void *fault_addr, memory_tier_t tier) {
    /* Align to page boundary */
    void *page_addr = page_align(fault_addr);
    
    /* Get the appropriate tier backing memory */
    tier_config_t *tier_config = &g_manager.tiers[tier];
    
    /*
     * For this simulation, we create a temporary zero page and copy it.
     * In a real tiered system, this would copy from actual tier memory.
     */
    static __thread char zero_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
    memset(zero_page, 0, PAGE_SIZE);
    
    /* Use UFFDIO_COPY to resolve the fault */
    struct uffdio_copy uffdio_copy = {
        .dst = (unsigned long)page_addr,
        .src = (unsigned long)zero_page,
        .len = PAGE_SIZE,
        .mode = 0  /* Don't wake (we do it ourselves) */
    };
    
    if (ioctl(g_manager.uffd, UFFDIO_COPY, &uffdio_copy) < 0) {
        if (errno == EEXIST) {
            /* Page was already mapped - race condition, but harmless */
            TM_DEBUG("Page %p already exists", page_addr);
            return 0;
        }
        TM_ERROR("UFFDIO_COPY failed for %p: %s", page_addr, strerror(errno));
        return -1;
    }
    
    /* Update tier usage */
    tier_config->used += PAGE_SIZE;
    
    /* Create/update page statistics */
    page_stats_t *stats = get_or_create_page_stats(page_addr);
    if (stats) {
        stats->current_tier = tier;
        record_page_access(page_addr, false);  /* Initial access is a read */
    }
    
    /* Update region stats */
    pthread_mutex_lock(&g_manager.regions_lock);
    for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
        managed_region_t *region = &g_manager.regions[i];
        if (region->active &&
            page_addr >= region->base_addr &&
            page_addr < region->base_addr + region->length) {
            
            atomic_fetch_add(&region->total_faults, 1);
            if (tier == TIER_DRAM) {
                atomic_fetch_add(&region->pages_in_dram, 1);
            } else {
                atomic_fetch_add(&region->pages_in_nvm, 1);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_manager.regions_lock);
    
    atomic_fetch_add(&g_manager.total_faults, 1);
    
    TM_DEBUG("Resolved fault at %p -> %s tier", 
             page_addr, tier == TIER_DRAM ? "DRAM" : "NVM");
    
    return 0;
}

/*============================================================================
 * FAULT HANDLER THREAD
 *===========================================================================*/

/*
 * Main loop for the userfaultfd handler thread.
 * 
 * This thread:
 *   1. Polls the userfaultfd for events
 *   2. Handles UFFD_EVENT_PAGEFAULT by deciding tier and resolving
 *   3. Records access statistics for the ML model
 */
static void* uffd_handler_thread(void *arg) {
    (void)arg;
    
    TM_INFO("UFFD handler thread running");
    
    struct pollfd pollfd = {
        .fd = g_manager.uffd,
        .events = POLLIN
    };
    
    while (g_manager.threads_running) {
        /* Poll with timeout so we can check threads_running flag */
        int ret = poll(&pollfd, 1, 100);  /* 100ms timeout */
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            TM_ERROR("poll() failed: %s", strerror(errno));
            break;
        }
        
        if (ret == 0) {
            /* Timeout - no events, just continue */
            continue;
        }
        
        if (pollfd.revents & POLLERR) {
            TM_ERROR("POLLERR on userfaultfd");
            break;
        }
        
        if (pollfd.revents & POLLIN) {
            /* Read the fault event */
            struct uffd_msg msg;
            ssize_t nread = read(g_manager.uffd, &msg, sizeof(msg));
            
            if (nread < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                TM_ERROR("read() from userfaultfd failed: %s", strerror(errno));
                break;
            }
            
            if (nread != sizeof(msg)) {
                TM_ERROR("Partial read from userfaultfd: %zd", nread);
                continue;
            }
            
            /* Handle the event */
            if (msg.event == UFFD_EVENT_PAGEFAULT) {
                void *fault_addr = (void*)msg.arg.pagefault.address;
                
                TM_DEBUG("Page fault at %p (flags=0x%llx)", 
                         fault_addr, msg.arg.pagefault.flags);
                
                /* Decide initial placement and resolve */
                memory_tier_t tier = decide_initial_placement(fault_addr);
                resolve_page_fault(fault_addr, tier);
                
            } else {
                TM_DEBUG("Unhandled UFFD event: %d", msg.event);
            }
        }
    }
    
    TM_INFO("UFFD handler thread exiting");
    return NULL;
}

/*
 * Stop the userfaultfd handler thread.
 */
void stop_uffd_handler(void) {
    /* Thread will exit on next poll timeout */
    pthread_join(g_manager.uffd_thread, NULL);
    TM_INFO("UFFD handler thread stopped");
}

/*
 * Clean up userfaultfd resources.
 */
void cleanup_userfaultfd(void) {
    /* Unregister all regions */
    pthread_mutex_lock(&g_manager.regions_lock);
    for (int i = 0; i < MAX_MANAGED_REGIONS; i++) {
        if (g_manager.regions[i].active) {
            struct uffdio_range range = {
                .start = (unsigned long)g_manager.regions[i].base_addr,
                .len = g_manager.regions[i].length
            };
            ioctl(g_manager.uffd, UFFDIO_UNREGISTER, &range);
            g_manager.regions[i].active = false;
        }
    }
    g_manager.region_count = 0;
    pthread_mutex_unlock(&g_manager.regions_lock);
    
    /* Close userfaultfd */
    if (g_manager.uffd >= 0) {
        close(g_manager.uffd);
        g_manager.uffd = -1;
    }
    
    TM_INFO("Userfaultfd cleaned up");
}
