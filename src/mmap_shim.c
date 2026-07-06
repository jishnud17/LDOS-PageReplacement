/*
 * mmap_shim.c - LD_PRELOAD Shim for Large-Allocation Interception
 *
 * Intercepts large allocations and places them under the tiered-memory
 * manager (userfaultfd demand paging + PEBS tracking).
 *
 * Two interception paths:
 *   1. mmap()  -- catches programs that allocate via a direct mmap() call
 *                 (e.g. the demo binary's synthetic regions).
 *   2. malloc()/calloc()/realloc() -- catches C and C++ workloads.  glibc
 *                 routes large malloc()s (and libstdc++'s operator new) through
 *                 an *internal* mmap that LD_PRELOAD cannot see, so interposing
 *                 mmap alone misses graph-analytics workloads like GAPBS.
 *                 Interposing the allocator catches them regardless.
 *
 * Only allocations >= LARGE_ALLOC_THRESHOLD are managed; everything else is
 * passed straight through to the real allocator.
 *
 * Usage: LD_PRELOAD=./lib/libmmap_shim.so ./your_application
 *        LDOS_CSV_LABEL=<name> names the per-run CSV.
 *
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <pthread.h>
#include "tiered_memory.h"

/*============================================================================
 * REAL FUNCTION POINTERS
 *===========================================================================*/

static void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int   (*real_munmap)(void *, size_t) = NULL;
static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void  (*real_free)(void *) = NULL;

/* The synthetic workloads and the policy/uffd threads reference the global
 * `running` flag (declared extern in workloads.h, defined in main.c for the
 * demo binary).  main.o is NOT part of libmmap_shim.so, so under LD_PRELOAD
 * the symbol would be undefined and the loader aborts on first use.  Define it
 * here so the shared library is self-contained; kept 1 for the process life. */
volatile int running = 1;

/*============================================================================
 * DLSYM BOOTSTRAP
 *
 * dlsym(RTLD_NEXT, ...) can itself call calloc().  Until the real allocator
 * pointers are resolved, serve those early requests from a static buffer, and
 * make free() a no-op for pointers that fall inside it.
 *===========================================================================*/

static char   bootstrap_buf[1 << 20];   /* 1 MB is ample for dlsym's needs */
static size_t bootstrap_off = 0;
static int    resolving = 0;

static void *bootstrap_alloc(size_t size, int zero) {
    size_t aligned = (size + 15) & ~((size_t)15);
    if (bootstrap_off + aligned > sizeof(bootstrap_buf))
        return NULL;                     /* should never happen in practice */
    void *p = bootstrap_buf + bootstrap_off;
    bootstrap_off += aligned;
    if (zero) memset(p, 0, size);
    return p;
}

static inline int is_bootstrap(void *p) {
    return (char *)p >= bootstrap_buf &&
           (char *)p <  bootstrap_buf + sizeof(bootstrap_buf);
}

static void ensure_real(void) {
    if (real_malloc) return;
    if (resolving) return;               /* re-entered during dlsym: use bootstrap */
    resolving = 1;
    real_mmap    = dlsym(RTLD_NEXT, "mmap");
    real_munmap  = dlsym(RTLD_NEXT, "munmap");
    real_malloc  = dlsym(RTLD_NEXT, "malloc");
    real_calloc  = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_free    = dlsym(RTLD_NEXT, "free");
    resolving = 0;
}

/*============================================================================
 * MANAGED-REGION TABLE
 *
 * Records the base + length of every allocation we placed via mmap so that
 * free()/realloc() can recognise and release it.  Managed allocations are few
 * (one per multi-GB data structure), so a small locked table is plenty.
 *===========================================================================*/

#define MAX_BIG 1024
static struct { void *base; size_t len; } g_big[MAX_BIG];
static int g_big_count = 0;
static pthread_mutex_t g_big_lock = PTHREAD_MUTEX_INITIALIZER;

static void big_record(void *base, size_t len) {
    pthread_mutex_lock(&g_big_lock);
    for (int i = 0; i < MAX_BIG; i++) {
        if (g_big[i].base == NULL) {
            g_big[i].base = base;
            g_big[i].len  = len;
            g_big_count++;
            break;
        }
    }
    pthread_mutex_unlock(&g_big_lock);
}

/* If `ptr` is a managed region, remove it from the table and return its length
 * in *len_out; otherwise return 0. */
static int big_take(void *ptr, size_t *len_out) {
    int found = 0;
    pthread_mutex_lock(&g_big_lock);
    if (g_big_count > 0) {
        for (int i = 0; i < MAX_BIG; i++) {
            if (g_big[i].base == ptr) {
                *len_out = g_big[i].len;
                g_big[i].base = NULL;
                g_big[i].len  = 0;
                g_big_count--;
                found = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_big_lock);
    return found;
}

/* Return the length of a managed region without removing it; 0 if not ours. */
static int big_find(void *ptr, size_t *len_out) {
    int found = 0;
    pthread_mutex_lock(&g_big_lock);
    if (g_big_count > 0) {
        for (int i = 0; i < MAX_BIG; i++) {
            if (g_big[i].base == ptr) {
                *len_out = g_big[i].len;
                found = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_big_lock);
    return found;
}

/*============================================================================
 * MANAGER INITIALISATION (malloc-reentrancy safe)
 *
 * tiered_manager_init() allocates internally, which re-enters our malloc.  A
 * pthread_once here would self-deadlock, so guard with a mutex plus a
 * thread-local flag: the initialising thread's re-entrant allocations fall
 * straight through to the real allocator, other threads wait for readiness.
 *===========================================================================*/

static int mgr_ready = 0;
static pthread_mutex_t mgr_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int in_mgr_init = 0;

static void ensure_manager(void) {
    if (mgr_ready || in_mgr_init) return;
    pthread_mutex_lock(&mgr_lock);
    if (!mgr_ready && !in_mgr_init) {
        in_mgr_init = 1;
        const char *label = getenv("LDOS_CSV_LABEL");
        if (label) set_csv_label(label);
        if (tiered_manager_init() == 0)
            mgr_ready = 1;
        else
            TM_ERROR("tiered_manager_init failed; allocations pass through");
        in_mgr_init = 0;
    }
    pthread_mutex_unlock(&mgr_lock);
}

/* Allocate `size` bytes as an unpopulated private mapping and register it with
 * the manager.  Returns NULL if we should fall back to the real allocator. */
static void *managed_alloc(size_t size) {
    if (!real_mmap) return NULL;
    void *p = real_mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (register_managed_region(p, size) < 0) {
        TM_ERROR("register_managed_region failed for %zu bytes", size);
        real_munmap(p, size);
        return NULL;
    }
    big_record(p, size);
    TM_INFO("Managed large allocation: %p + %zu bytes", p, size);
    return p;
}

/* Decide + perform: manage if large enough and the manager is available. */
static void *maybe_manage(size_t size) {
    if (size < LARGE_ALLOC_THRESHOLD || in_mgr_init)
        return NULL;
    ensure_manager();
    if (!mgr_ready)
        return NULL;
    return managed_alloc(size);
}

/*============================================================================
 * ALLOCATOR INTERPOSITION
 *===========================================================================*/

void *malloc(size_t size) {
    ensure_real();
    if (!real_malloc) return bootstrap_alloc(size, 0);
    void *p = maybe_manage(size);
    return p ? p : real_malloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    ensure_real();
    if (!real_calloc) return bootstrap_alloc(nmemb * size, 1);
    size_t total = nmemb * size;
    if (size != 0 && total / size != nmemb)     /* overflow -> let libc handle */
        return real_calloc(nmemb, size);
    void *p = maybe_manage(total);
    return p ? p : real_calloc(nmemb, size);    /* mmap memory is already zero */
}

void *realloc(void *ptr, size_t size) {
    ensure_real();
    size_t old_len;
    if (ptr && big_find(ptr, &old_len)) {
        /* Growing/shrinking a managed region.  Re-allocate through our own path
         * (so a still-large result stays managed), copy, then release the old
         * map.  On allocation failure the original region is left intact, per
         * realloc() semantics. */
        if (size == 0) {                       /* realloc(ptr,0): free the region */
            big_take(ptr, &old_len);
            unregister_managed_region(ptr);
            real_munmap(ptr, old_len);
            return NULL;
        }
        void *np = malloc(size);
        if (!np) return NULL;                  /* keep old region intact */
        memcpy(np, ptr, old_len < size ? old_len : size);
        big_take(ptr, &old_len);
        unregister_managed_region(ptr);
        real_munmap(ptr, old_len);
        return np;
    }
    if (!real_realloc) return bootstrap_alloc(size, 0);
    return real_realloc(ptr, size);
}

void free(void *ptr) {
    if (!ptr) return;
    if (is_bootstrap(ptr)) return;              /* served during dlsym bootstrap */
    ensure_real();
    size_t len;
    if (big_take(ptr, &len)) {
        unregister_managed_region(ptr);
        real_munmap(ptr, len);
        return;
    }
    if (real_free) real_free(ptr);      /* NULL only in the dlsym window */
}

/*============================================================================
 * MMAP INTERPOSITION (direct-mmap workloads, e.g. the demo binary)
 *===========================================================================*/

static int should_manage_mmap(size_t length, int flags, int fd) {
    (void)fd;
    if (length < LARGE_ALLOC_THRESHOLD) return 0;
    if (!(flags & MAP_ANONYMOUS)) return 0;
    if (flags & MAP_PRIVATE) return 1;
    /* Shared anonymous mappings (e.g. GUPS's MAP_SHARED table) are safe to
     * manage in telemetry-only mode, which never registers with uffd --
     * uffd write-protect on shmem requires kernel >= 5.19. */
    if (flags & MAP_SHARED) {
        const char *t = getenv("LDOS_PEBS_TELEMETRY_ONLY");
        return t != NULL && t[0] == '1';
    }
    return 0;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    ensure_real();
    if (in_mgr_init || !should_manage_mmap(length, flags, fd))
        return real_mmap(addr, length, prot, flags, fd, offset);

    ensure_manager();
    if (!mgr_ready)
        return real_mmap(addr, length, prot, flags, fd, offset);

    void *result = real_mmap(addr, length, prot, flags | MAP_NORESERVE, fd, offset);
    if (result == MAP_FAILED) return result;
    if (register_managed_region(result, length) < 0) {
        TM_ERROR("Failed to register mmap region with userfaultfd");
    } else {
        big_record(result, length);
        TM_INFO("Managed mmap region: %p + %zu", result, length);
    }
    return result;
}

int munmap(void *addr, size_t length) {
    ensure_real();
    size_t len;
    if (big_take(addr, &len)) {
        unregister_managed_region(addr);
        return real_munmap(addr, len);
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
    if (mgr_ready)
        tiered_manager_shutdown();
    TM_INFO("mmap shim library unloaded");
}
