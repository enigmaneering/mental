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

/* ── Disclosure header (lives at offset 0 in the shm region) ───── */

/*
 * The disclosure header is the first thing in every shared memory region.
 * It controls access: the owning process evaluates it before granting
 * data access to observers.
 *
 * Layout (64 bytes, fixed):
 *   [0..3]   uint32_t mode    (atomic — owner can flip on the fly)
 *   [4..67]  char passphrase[64]  (NUL-terminated, set by owner)
 *
 * User data starts at offset DISCLOSURE_HEADER_SIZE.
 */

#define DISCLOSURE_PASSPHRASE_MAX 64
#define DISCLOSURE_HEADER_SIZE    128  /* padded for alignment */

#ifdef _MSC_VER
#include <intrin.h>
#define ATOMIC_LOAD32(p)    (*(volatile uint32_t*)(p))
#define ATOMIC_STORE32(p,v) (*(volatile uint32_t*)(p) = (v))
#else
#include <stdatomic.h>
#define ATOMIC_LOAD32(p)    atomic_load_explicit((_Atomic uint32_t*)(p), memory_order_acquire)
#define ATOMIC_STORE32(p,v) atomic_store_explicit((_Atomic uint32_t*)(p), (v), memory_order_release)
#endif

struct disclosure_header {
    uint32_t mode;                              /* mental_disclosure enum */
    char     passphrase[DISCLOSURE_PASSPHRASE_MAX]; /* NUL-terminated */
};

/* ── Ref structure ─────────────────────────────────────────────── */

struct mental_ref_t {
    void  *addr;       /* mmap'd / MapViewOfFile address (includes header) */
    size_t total_size; /* total mapped size (header + user data) */
    size_t user_size;  /* user-visible data size */
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

    size_t total = DISCLOSURE_HEADER_SIZE + size;

    mental_ref ref = calloc(1, sizeof(struct mental_ref_t));
    if (!ref) return NULL;

    strncpy(ref->path, path, sizeof(ref->path) - 1);
    ref->total_size = total;
    ref->user_size  = size;
    ref->owner = 1;

#ifdef _WIN32
    ref->hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)(total >> 32), (DWORD)total, path + 1); /* skip '/' */
    if (!ref->hMap) { free(ref); return NULL; }

    ref->addr = MapViewOfFile(ref->hMap, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!ref->addr) {
        CloseHandle(ref->hMap);
        free(ref);
        return NULL;
    }
#else
    /* O_CREAT | O_EXCL: fail if it already exists (name collision) */
    int fd = shm_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { free(ref); return NULL; }

    if (ftruncate(fd, (off_t)total) < 0) {
        close(fd);
        shm_unlink(path);
        free(ref);
        return NULL;
    }

    void *addr = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        shm_unlink(path);
        free(ref);
        return NULL;
    }

    ref->fd   = fd;
    ref->addr = addr;
#endif

    /* Initialize disclosure: open by default, no passphrase */
    struct disclosure_header *hdr = (struct disclosure_header *)ref->addr;
    memset(hdr, 0, DISCLOSURE_HEADER_SIZE);
    ATOMIC_STORE32(&hdr->mode, MENTAL_RELATIONALLY_OPEN);

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

    MEMORY_BASIC_INFORMATION info;
    ref->addr = MapViewOfFile(ref->hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!ref->addr) {
        CloseHandle(ref->hMap);
        free(ref);
        return NULL;
    }
    VirtualQuery(ref->addr, &info, sizeof(info));
    ref->total_size = info.RegionSize;
    ref->user_size  = (ref->total_size > DISCLOSURE_HEADER_SIZE)
                    ? ref->total_size - DISCLOSURE_HEADER_SIZE : 0;
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

    ref->fd         = fd;
    ref->addr       = addr;
    ref->total_size = (size_t)st.st_size;
    ref->user_size  = (ref->total_size > DISCLOSURE_HEADER_SIZE)
                    ? ref->total_size - DISCLOSURE_HEADER_SIZE : 0;
#endif

    return ref;
}

/* ── Disclosure helpers ─────────────────────────────────────────── */

static struct disclosure_header* ref_header(mental_ref ref) {
    return (struct disclosure_header *)ref->addr;
}

static void* ref_user_data(mental_ref ref) {
    return (char *)ref->addr + DISCLOSURE_HEADER_SIZE;
}

static int passphrase_matches(struct disclosure_header *hdr,
                               const char *passphrase) {
    if (!passphrase) return 0;
    return strncmp(hdr->passphrase, passphrase,
                   DISCLOSURE_PASSPHRASE_MAX) == 0;
}

/* ── Disclosure API ────────────────────────────────────────────── */

mental_disclosure mental_ref_get_disclosure(mental_ref ref) {
    if (!ref || !ref->addr) return MENTAL_RELATIONALLY_OPEN;
    return (mental_disclosure)ATOMIC_LOAD32(&ref_header(ref)->mode);
}

void mental_ref_set_disclosure(mental_ref ref, mental_disclosure mode) {
    if (!ref || !ref->addr || !ref->owner) return;
    ATOMIC_STORE32(&ref_header(ref)->mode, (uint32_t)mode);
}

void mental_ref_set_passphrase(mental_ref ref, const char *passphrase) {
    if (!ref || !ref->addr || !ref->owner) return;
    struct disclosure_header *hdr = ref_header(ref);
    if (passphrase) {
        strncpy(hdr->passphrase, passphrase, DISCLOSURE_PASSPHRASE_MAX - 1);
        hdr->passphrase[DISCLOSURE_PASSPHRASE_MAX - 1] = '\0';
    } else {
        memset(hdr->passphrase, 0, DISCLOSURE_PASSPHRASE_MAX);
    }
}

/* ── Accessors (disclosure-aware) ──────────────────────────────── */

void* mental_ref_data(mental_ref ref, const char *passphrase) {
    if (!ref || !ref->addr) return NULL;

    /* Owner always has full access */
    if (ref->owner) return ref_user_data(ref);

    struct disclosure_header *hdr = ref_header(ref);
    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);

    switch (mode) {
    case MENTAL_RELATIONALLY_OPEN:
        return ref_user_data(ref);

    case MENTAL_RELATIONALLY_INCLUSIVE:
        /* Read-only access without passphrase — return the pointer.
         * The "read-only" semantic is advisory at this level; the
         * owner trusts the spark chain.  With passphrase: full access. */
        return ref_user_data(ref);

    case MENTAL_RELATIONALLY_EXCLUSIVE:
        /* All access requires passphrase */
        if (passphrase_matches(hdr, passphrase))
            return ref_user_data(ref);
        return NULL; /* graceful denial */
    }

    return NULL;
}

size_t mental_ref_size(mental_ref ref) {
    return ref ? ref->user_size : 0;
}

int mental_ref_writable(mental_ref ref, const char *passphrase) {
    if (!ref || !ref->addr) return 0;

    /* Owner always writable */
    if (ref->owner) return 1;

    struct disclosure_header *hdr = ref_header(ref);
    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);

    switch (mode) {
    case MENTAL_RELATIONALLY_OPEN:
        return 1;

    case MENTAL_RELATIONALLY_INCLUSIVE:
        /* Writable only with passphrase */
        return passphrase_matches(hdr, passphrase) ? 1 : 0;

    case MENTAL_RELATIONALLY_EXCLUSIVE:
        /* All access requires passphrase */
        return passphrase_matches(hdr, passphrase) ? 1 : 0;
    }

    return 0;
}

/* ── Close ─────────────────────────────────────────────────────── */

void mental_ref_close(mental_ref ref) {
    if (!ref) return;

#ifdef _WIN32
    if (ref->addr) UnmapViewOfFile(ref->addr);
    if (ref->hMap) CloseHandle(ref->hMap);
    if (ref->owner) {
        untrack_ref(ref->path);
    }
#else
    if (ref->addr && ref->addr != MAP_FAILED)
        munmap(ref->addr, ref->total_size);
    if (ref->fd >= 0)
        close(ref->fd);
    if (ref->owner) {
        shm_unlink(ref->path);
        untrack_ref(ref->path);
    }
#endif

    free(ref);
}
