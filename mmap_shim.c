/*
 * mmap_shim.c - LD_PRELOAD Shim for mmap Interception
 * 
 * This library intercepts mmap() calls via LD_PRELOAD. When an application
 * requests a "large" allocation (>1GB by default), we:
 *   1. Let the real mmap allocate the memory
 *   2. Register the region with userfaultfd for demand paging
 *   3. The userfaultfd handler then controls page placement
 * 
 * Usage:
 *   LD_PRELOAD=./libmmap_shim.so ./your_application
 * 
 * This allows us to intercept and manage memory without modifying
 * the application source code.
 * 
 * Note: This requires the tiered memory manager to be initialized.
 * The library automatically initializes itself on first mmap call.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <pthread.h>
#include "tiered_memory.h"

/*============================================================================
 * ORIGINAL FUNCTION POINTERS
 *===========================================================================*/

/* Pointer to the real mmap function */
static void* (*real_mmap)(void *addr, size_t length, int prot, 
                          int flags, int fd, off_t offset) = NULL;

/* Pointer to the real munmap function */
static int (*real_munmap)(void *addr, size_t length) = NULL;

/* Initialization flag */
static int shim_initialized = 0;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/*============================================================================
 * SHIM INITIALIZATION
 *===========================================================================*/

/*
 * Initialize the shim library.
 * Called once on first mmap/munmap call.
 */
static void shim_init_impl(void) {
    /* Get the real mmap function */
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    if (real_mmap == NULL) {
        fprintf(stderr, "[SHIM ERROR] Failed to find real mmap: %s\n", dlerror());
        abort();
    }
    
    /* Get the real munmap function */
    real_munmap = dlsym(RTLD_NEXT, "munmap");
    if (real_munmap == NULL) {
        fprintf(stderr, "[SHIM ERROR] Failed to find real munmap: %s\n", dlerror());
        abort();
    }
    
    /* Initialize the tiered memory manager */
    if (tiered_manager_init() < 0) {
        fprintf(stderr, "[SHIM ERROR] Failed to initialize tiered memory manager\n");
        /* Continue anyway - we'll just pass through to real mmap */
    }
    
    shim_initialized = 1;
    
    TM_INFO("mmap shim initialized (threshold=%lu bytes)", LARGE_ALLOC_THRESHOLD);
}

static void shim_init(void) {
    pthread_once(&init_once, shim_init_impl);
}

/*============================================================================
 * MMAP INTERCEPTION
 *===========================================================================*/

/*
 * Determine if an allocation should be managed.
 * 
 * Criteria for management:
 *   - Large enough (>1GB by default)
 *   - Anonymous memory (not file-backed)
 *   - Private mapping
 */
static int should_manage(size_t length, int flags, int fd) {
    /* Only manage large allocations */
    if (length < LARGE_ALLOC_THRESHOLD) {
        return 0;
    }
    
    /* Only manage anonymous, private mappings */
    if (!(flags & MAP_ANONYMOUS)) {
        TM_DEBUG("Skipping file-backed mapping");
        return 0;
    }
    
    if (!(flags & MAP_PRIVATE)) {
        TM_DEBUG("Skipping shared mapping");
        return 0;
    }
    
    /* Skip if we couldn't initialize */
    if (!g_manager.initialized) {
        return 0;
    }
    
    return 1;
}

/*
 * Intercepted mmap function.
 * 
 * This is called instead of the real mmap when LD_PRELOAD is used.
 */
void* mmap(void *addr, size_t length, int prot, int flags, 
           int fd, off_t offset) {
    /* Ensure initialization */
    shim_init();
    
    /* For small or ineligible allocations, just pass through */
    if (!should_manage(length, flags, fd)) {
        return real_mmap(addr, length, prot, flags, fd, offset);
    }
    
    TM_INFO("Intercepted large mmap: %zu bytes", length);
    
    /*
     * Strategy for managed allocations:
     * 
     * 1. Allocate memory WITHOUT populating pages (use MAP_NORESERVE)
     *    This prevents immediate physical allocation.
     * 
     * 2. Register with userfaultfd to intercept page faults
     *    Now when the app touches pages, we get notified.
     * 
     * 3. In the fault handler, we decide where to place each page
     *    (DRAM or NVM tier) and resolve the fault.
     */
    
    /* Allocate with MAP_NORESERVE - pages won't be backed until accessed */
    int managed_flags = flags | MAP_NORESERVE;
    
    void *result = real_mmap(addr, length, prot, managed_flags, fd, offset);
    
    if (result == MAP_FAILED) {
        TM_ERROR("mmap failed for managed allocation");
        return result;
    }
    
    /* Register with userfaultfd for demand paging */
    if (register_managed_region(result, length) < 0) {
        TM_ERROR("Failed to register region with userfaultfd");
        /* Fall back to unmanaged - pages will be allocated normally */
        /* We could munmap and retry without management, but let's continue */
    } else {
        TM_INFO("Registered managed region: %p + %zu", result, length);
    }
    
    return result;
}

/*
 * Intercepted munmap function.
 * 
 * Unregisters the region from userfaultfd before unmapping.
 */
int munmap(void *addr, size_t length) {
    /* Ensure initialization */
    shim_init();
    
    /* Check if this is a managed region */
    if (g_manager.initialized && length >= LARGE_ALLOC_THRESHOLD) {
        /* Try to unregister (will be a no-op if not managed) */
        unregister_managed_region(addr);
    }
    
    return real_munmap(addr, length);
}

/*============================================================================
 * LIBRARY CONSTRUCTOR/DESTRUCTOR
 *===========================================================================*/

/*
 * Called when the library is loaded.
 */
__attribute__((constructor))
static void shim_load(void) {
    TM_INFO("mmap shim library loaded");
}

/*
 * Called when the library is unloaded.
 */
__attribute__((destructor))
static void shim_unload(void) {
    if (g_manager.initialized) {
        tiered_manager_shutdown();
    }
    TM_INFO("mmap shim library unloaded");
}
