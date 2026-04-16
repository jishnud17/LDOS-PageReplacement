/*
 * mmap_shim.c - LD_PRELOAD Shim for mmap Interception
 * 
 * Intercepts mmap() calls for large allocations (>1GB) and registers them
 * with userfaultfd for demand paging with tier placement control.
 * 
 * Usage: LD_PRELOAD=./libmmap_shim.so ./your_application
 * 
 * LDOS Research Project, UT Austin
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

static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = NULL;
static int (*real_munmap)(void*, size_t) = NULL;
static int shim_initialized = 0;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/*============================================================================
 * INITIALIZATION
 *===========================================================================*/

static void shim_init_impl(void) {
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    real_munmap = dlsym(RTLD_NEXT, "munmap");
    
    if (!real_mmap || !real_munmap) {
        fprintf(stderr, "[SHIM ERROR] Failed to find real mmap/munmap: %s\n", dlerror());
        abort();
    }
    
    if (tiered_manager_init() < 0) {
        fprintf(stderr, "[SHIM WARNING] Tiered manager init failed, passing through\n");
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

static int should_manage(size_t length, int flags, int fd) {
    (void)fd;
    
    if (length < LARGE_ALLOC_THRESHOLD) return 0;
    if (!(flags & MAP_ANONYMOUS)) return 0;
    if (!(flags & MAP_PRIVATE)) return 0;
    if (!g_manager.initialized) return 0;
    
    return 1;
}

void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    shim_init();
    
    if (!should_manage(length, flags, fd)) {
        return real_mmap(addr, length, prot, flags, fd, offset);
    }
    
    TM_INFO("Intercepted large mmap: %zu bytes", length);
    
    /* Allocate without populating pages */
    void *result = real_mmap(addr, length, prot, flags | MAP_NORESERVE, fd, offset);
    if (result == MAP_FAILED) {
        TM_ERROR("mmap failed");
        return result;
    }
    
    /* Register with userfaultfd for demand paging */
    if (register_managed_region(result, length) < 0) {
        TM_ERROR("Failed to register region with userfaultfd");
    } else {
        TM_INFO("Registered managed region: %p + %zu", result, length);
    }
    
    return result;
}

int munmap(void *addr, size_t length) {
    shim_init();
    
    if (g_manager.initialized && length >= LARGE_ALLOC_THRESHOLD) {
        unregister_managed_region(addr);
    }
    
    return real_munmap(addr, length);
}

/*============================================================================
 * LIBRARY LIFECYCLE
 *===========================================================================*/

__attribute__((constructor))
static void shim_load(void) {
    TM_INFO("mmap shim library loaded");
}

__attribute__((destructor))
static void shim_unload(void) {
    if (g_manager.initialized) {
        tiered_manager_shutdown();
    }
    TM_INFO("mmap shim library unloaded");
}
