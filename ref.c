/*
 * Mental - Ref (UUID-scoped shared memory references)
 *
 * Each mental process gets a UUID on first use, scoping all named
 * shared memory regions under /mental-{uuid}/{name}.
 *
 * The creating process owns the shm.  When the process exits, all
 * its regions are automatically unlinked via atexit.
 *
 * Observers (sparked children or peers) can open a ref by providing
 * the owner's UUID and the ref name.  If the owner has already exited
 * and the shm is gone, the open returns NULL gracefully.
 */

#define _POSIX_C_SOURCE 200809L

#include "mental.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#endif

/* ── UUID ──────────────────────────────────────────────────────── */

/*
 * 128-bit UUID v4 (random), rendered as 32 lowercase hex chars (no dashes)
 * to keep shm path lengths short.
 */

static char g_uuid[33]; /* 32 hex + NUL */
static int  g_uuid_init = 0;

#ifdef _WIN32
static CRITICAL_SECTION g_uuid_cs;
static int g_uuid_cs_init = 0;

static void uuid_lock(void) {
    if (!g_uuid_cs_init) {
        InitializeCriticalSection(&g_uuid_cs);
        g_uuid_cs_init = 1;
    }
    EnterCriticalSection(&g_uuid_cs);
}
static void uuid_unlock(void) { LeaveCriticalSection(&g_uuid_cs); }
#else
static pthread_mutex_t g_uuid_lock = PTHREAD_MUTEX_INITIALIZER;
static void uuid_lock(void)   { pthread_mutex_lock(&g_uuid_lock); }
static void uuid_unlock(void) { pthread_mutex_unlock(&g_uuid_lock); }
#endif

static void generate_uuid(void) {
    unsigned char buf[16];

#ifdef _WIN32
    /* Use CryptGenRandom or RtlGenRandom */
    typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
    HMODULE advapi = LoadLibraryA("advapi32.dll");
    if (advapi) {
        RtlGenRandomFn fn = (RtlGenRandomFn)GetProcAddress(advapi, "SystemFunction036");
        if (fn) fn(buf, sizeof(buf));
        FreeLibrary(advapi);
    }
#else
    /* /dev/urandom — always available on Linux/macOS */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, sizeof(buf));
        (void)r;
        close(fd);
    }
#endif

    /* Set version (4) and variant (10xx) bits */
    buf[6] = (buf[6] & 0x0F) | 0x40;
    buf[8] = (buf[8] & 0x3F) | 0x80;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        g_uuid[i * 2]     = hex[buf[i] >> 4];
        g_uuid[i * 2 + 1] = hex[buf[i] & 0x0F];
    }
    g_uuid[32] = '\0';
}

static void ensure_uuid(void) {
    if (g_uuid_init) return;
    uuid_lock();
    if (!g_uuid_init) {
        generate_uuid();
        g_uuid_init = 1;
    }
    uuid_unlock();
}

const char* mental_uuid(void) {
    ensure_uuid();
    return g_uuid;
}

/* ── Ref tracking (for atexit cleanup) ─────────────────────────── */

#define MAX_REFS 256

static char* g_ref_paths[MAX_REFS]; /* shm paths we own (heap-allocated) */
static int   g_ref_count = 0;

#ifdef _WIN32
static CRITICAL_SECTION g_ref_cs;
static int g_ref_cs_init = 0;
static void ref_lock(void) {
    if (!g_ref_cs_init) {
        InitializeCriticalSection(&g_ref_cs);
        g_ref_cs_init = 1;
    }
    EnterCriticalSection(&g_ref_cs);
}
static void ref_unlock(void) { LeaveCriticalSection(&g_ref_cs); }
#else
static pthread_mutex_t g_ref_lock = PTHREAD_MUTEX_INITIALIZER;
static void ref_lock(void)   { pthread_mutex_lock(&g_ref_lock); }
static void ref_unlock(void) { pthread_mutex_unlock(&g_ref_lock); }
#endif

static int g_ref_atexit_registered = 0;

static void ref_cleanup(void) {
    ref_lock();
    for (int i = 0; i < g_ref_count; i++) {
        if (g_ref_paths[i]) {
#ifdef _WIN32
            /* Windows named shared memory is reference-counted by the
             * kernel; closing all handles unmaps it. No explicit unlink. */
#else
            shm_unlink(g_ref_paths[i]);
#endif
            free(g_ref_paths[i]);
            g_ref_paths[i] = NULL;
        }
    }
    g_ref_count = 0;
    ref_unlock();
}

static void track_ref(const char* path) {
    ref_lock();
    if (!g_ref_atexit_registered) {
        mental_atexit(ref_cleanup);
        g_ref_atexit_registered = 1;
    }
    if (g_ref_count < MAX_REFS) {
        g_ref_paths[g_ref_count++] = strdup(path);
    }
    ref_unlock();
}

static void untrack_ref(const char* path) {
    ref_lock();
    for (int i = 0; i < g_ref_count; i++) {
        if (g_ref_paths[i] && strcmp(g_ref_paths[i], path) == 0) {
            free(g_ref_paths[i]);
            g_ref_paths[i] = g_ref_paths[g_ref_count - 1];
            g_ref_paths[g_ref_count - 1] = NULL;
            g_ref_count--;
            break;
        }
    }
    ref_unlock();
}

/* ── Helpers ───────────────────────────────────────────────────── */

/*
 * Build shm path: "/mental-{uuid}/{name}"
 * The leading '/' is required by shm_open on POSIX.
 */
static int build_shm_path(char *dst, size_t dst_len,
                           const char *uuid, const char *name) {
    int n = snprintf(dst, dst_len, "/mental-%s-%s", uuid, name);
    return (n > 0 && (size_t)n < dst_len) ? 0 : -1;
}

/* ── Ref structure ─────────────────────────────────────────────── */

struct mental_ref_t {
    void  *addr;       /* mmap'd / MapViewOfFile address */
    size_t size;       /* mapped size */
    int    owner;      /* 1 = we created it, 0 = observer */
    char   path[320];  /* shm path for cleanup */
#ifdef _WIN32
    HANDLE hMap;
#else
    int    fd;
#endif
};

/* ── Create (owner) ────────────────────────────────────────────── */

mental_ref mental_ref_create(const char *name, size_t size) {
    if (!name || !name[0] || size == 0) return NULL;

    ensure_uuid();

    char path[320];
    if (build_shm_path(path, sizeof(path), g_uuid, name) < 0)
        return NULL;

    mental_ref ref = calloc(1, sizeof(struct mental_ref_t));
    if (!ref) return NULL;

    strncpy(ref->path, path, sizeof(ref->path) - 1);
    ref->size  = size;
    ref->owner = 1;

#ifdef _WIN32
    ref->hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)(size >> 32), (DWORD)size, path + 1); /* skip '/' */
    if (!ref->hMap) { free(ref); return NULL; }

    ref->addr = MapViewOfFile(ref->hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!ref->addr) {
        CloseHandle(ref->hMap);
        free(ref);
        return NULL;
    }
#else
    /* O_CREAT | O_EXCL: fail if it already exists (name collision) */
    int fd = shm_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { free(ref); return NULL; }

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        shm_unlink(path);
        free(ref);
        return NULL;
    }

    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        shm_unlink(path);
        free(ref);
        return NULL;
    }

    ref->fd   = fd;
    ref->addr = addr;
#endif

    track_ref(path);
    return ref;
}

/* ── Open (observer) ───────────────────────────────────────────── */

mental_ref mental_ref_open(const char *peer_uuid, const char *name) {
    if (!peer_uuid || !peer_uuid[0] || !name || !name[0])
        return NULL;

    char path[320];
    if (build_shm_path(path, sizeof(path), peer_uuid, name) < 0)
        return NULL;

    mental_ref ref = calloc(1, sizeof(struct mental_ref_t));
    if (!ref) return NULL;

    strncpy(ref->path, path, sizeof(ref->path) - 1);
    ref->owner = 0;

#ifdef _WIN32
    ref->hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, path + 1);
    if (!ref->hMap) {
        /* Owner gone or never existed — graceful failure */
        free(ref);
        return NULL;
    }

    /* Query the mapping size */
    MEMORY_BASIC_INFORMATION info;
    ref->addr = MapViewOfFile(ref->hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!ref->addr) {
        CloseHandle(ref->hMap);
        free(ref);
        return NULL;
    }
    VirtualQuery(ref->addr, &info, sizeof(info));
    ref->size = info.RegionSize;
#else
    int fd = shm_open(path, O_RDWR, 0);
    if (fd < 0) {
        /* Owner gone or never existed — graceful failure */
        free(ref);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        free(ref);
        return NULL;
    }

    void *addr = mmap(NULL, (size_t)st.st_size,
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        free(ref);
        return NULL;
    }

    ref->fd   = fd;
    ref->addr = addr;
    ref->size = (size_t)st.st_size;
#endif

    return ref;
}

/* ── Accessors ─────────────────────────────────────────────────── */

void* mental_ref_data(mental_ref ref) {
    return ref ? ref->addr : NULL;
}

size_t mental_ref_size(mental_ref ref) {
    return ref ? ref->size : 0;
}

/* ── Close ─────────────────────────────────────────────────────── */

void mental_ref_close(mental_ref ref) {
    if (!ref) return;

#ifdef _WIN32
    if (ref->addr) UnmapViewOfFile(ref->addr);
    if (ref->hMap) CloseHandle(ref->hMap);
    if (ref->owner) {
        /* Windows auto-cleans when all handles close, but untrack */
        untrack_ref(ref->path);
    }
#else
    if (ref->addr && ref->addr != MAP_FAILED)
        munmap(ref->addr, ref->size);
    if (ref->fd >= 0)
        close(ref->fd);
    if (ref->owner) {
        shm_unlink(ref->path);
        untrack_ref(ref->path);
    }
#endif

    free(ref);
}
