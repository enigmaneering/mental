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
 * The credential is a generic byte array — mental doesn't interpret it.
 * It could be a plaintext string, a SHA-256 hash, an ed25519 signature,
 * or any other scheme.  "We're just the messengers, not the cryptographers."
 *
 * Layout:
 *   [0..3]     uint32_t mode            (atomic — owner can flip on the fly)
 *   [4..7]     uint32_t credential_len  (how many bytes of credential are set)
 *   [8..135]   uint8_t  credential[128] (raw bytes, set by owner)
 *
 * User data starts at offset DISCLOSURE_HEADER_SIZE.
 */

#define DISCLOSURE_CREDENTIAL_MAX 128
#define DISCLOSURE_HEADER_SIZE    256  /* padded for alignment */

#ifdef _MSC_VER
#include <intrin.h>
#define ATOMIC_LOAD32(p)      (*(volatile uint32_t*)(p))
#define ATOMIC_STORE32(p,v)   (*(volatile uint32_t*)(p) = (v))
#define ATOMIC_EXCHANGE32(p,v) _InterlockedExchange((volatile long*)(p), (long)(v))
#else
#include <stdatomic.h>
#define ATOMIC_LOAD32(p)      atomic_load_explicit((_Atomic uint32_t*)(p), memory_order_acquire)
#define ATOMIC_STORE32(p,v)   atomic_store_explicit((_Atomic uint32_t*)(p), (v), memory_order_release)
#define ATOMIC_EXCHANGE32(p,v) atomic_exchange_explicit((_Atomic uint32_t*)(p), (v), memory_order_acquire)
#endif

struct disclosure_header {
    uint32_t mode;                                    /* mental_disclosure enum */
    uint32_t credential_len;                          /* bytes of credential set */
    uint8_t  credential[DISCLOSURE_CREDENTIAL_MAX];   /* raw bytes */
    uint32_t lock;                                    /* spinlock: 0=unlocked, 1=locked */
};

/* ── Disclosure spinlock ───────────────────────────────────────── */

/*
 * Process-shared spinlock in the shm header.  Protects credential
 * reads, writes, and comparisons so they are always atomic — no
 * window where a half-written credential can be observed.
 */
static void disclosure_lock(struct disclosure_header *hdr) {
    while (ATOMIC_EXCHANGE32(&hdr->lock, 1) != 0) {
#if defined(_WIN32)
        SwitchToThread();
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#else
        sched_yield();
#endif
    }
}

static void disclosure_unlock(struct disclosure_header *hdr) {
    ATOMIC_STORE32(&hdr->lock, 0);
}

/* ── Ref structure ─────────────────────────────────────────────── */

/*
 * Credential provider callback.
 *
 * Instead of caching credential bytes, the owner can register a
 * function that produces them on demand.  The function is called
 * under the disclosure spinlock each time an access check occurs,
 * guaranteeing the comparison always uses a fresh credential.
 *
 *   fn(ctx, buf, buf_size, out_len)
 *     ctx      — opaque context pointer (passed through)
 *     buf      — write credential bytes here
 *     buf_size — capacity (always DISCLOSURE_CREDENTIAL_MAX)
 *     out_len  — set to number of bytes written
 */
typedef void (*mental_credential_fn)(void *ctx,
                                      void *buf, size_t buf_size,
                                      size_t *out_len);

struct mental_ref_t {
    void  *addr;       /* mmap'd / MapViewOfFile address (includes header) */
    size_t total_size; /* total mapped size (header + user data) */
    size_t user_size;  /* user-visible data size */
    int    owner;      /* 1 = we created it, 0 = observer */
    char   path[320];  /* shm path for cleanup */

    /* Credential provider (owner only) — evaluated under spinlock */
    mental_credential_fn credential_fn;
    void                *credential_ctx;

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

    /* Initialize disclosure: open by default, no credential, unlocked */
    struct disclosure_header *hdr = (struct disclosure_header *)ref->addr;
    memset(hdr, 0, DISCLOSURE_HEADER_SIZE);
    ATOMIC_STORE32(&hdr->mode, MENTAL_RELATIONALLY_OPEN);
    ATOMIC_STORE32(&hdr->lock, 0);

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

static int credential_matches(struct disclosure_header *hdr,
                               const void *credential, size_t credential_len) {
    if (!credential || credential_len == 0) return 0;
    if (credential_len != hdr->credential_len) return 0;
    return memcmp(hdr->credential, credential, credential_len) == 0;
}

/*
 * Refresh the credential from the provider function (if set).
 * MUST be called under the disclosure spinlock.
 */
static void refresh_credential(mental_ref ref, struct disclosure_header *hdr) {
    if (!ref->credential_fn) return;

    size_t out_len = 0;
    uint8_t buf[DISCLOSURE_CREDENTIAL_MAX];
    ref->credential_fn(ref->credential_ctx, buf, sizeof(buf), &out_len);

    if (out_len > DISCLOSURE_CREDENTIAL_MAX)
        out_len = DISCLOSURE_CREDENTIAL_MAX;
    memcpy(hdr->credential, buf, out_len);
    hdr->credential_len = (uint32_t)out_len;
}

/* ── Disclosure API ────────────────────────────────────────────── */

mental_disclosure mental_ref_get_disclosure(mental_ref ref) {
    if (!ref || !ref->addr) return MENTAL_RELATIONALLY_OPEN;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);
    disclosure_unlock(hdr);
    return mode;
}

void mental_ref_set_disclosure(mental_ref ref, mental_disclosure mode) {
    if (!ref || !ref->addr || !ref->owner) return;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    ATOMIC_STORE32(&hdr->mode, (uint32_t)mode);
    disclosure_unlock(hdr);
}

void mental_ref_set_credential(mental_ref ref,
                                const void *credential, size_t len) {
    if (!ref || !ref->addr || !ref->owner) return;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    if (credential && len > 0) {
        if (len > DISCLOSURE_CREDENTIAL_MAX)
            len = DISCLOSURE_CREDENTIAL_MAX;
        memcpy(hdr->credential, credential, len);
        hdr->credential_len = (uint32_t)len;
    } else {
        memset(hdr->credential, 0, DISCLOSURE_CREDENTIAL_MAX);
        hdr->credential_len = 0;
    }
    /* Setting raw credential clears any provider */
    ref->credential_fn  = NULL;
    ref->credential_ctx = NULL;
    disclosure_unlock(hdr);
}

void mental_ref_set_credential_provider(mental_ref ref,
                                         mental_credential_fn fn, void *ctx) {
    if (!ref || !ref->addr || !ref->owner) return;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    ref->credential_fn  = fn;
    ref->credential_ctx = ctx;
    /* Immediately evaluate so credential is fresh right now */
    if (fn) refresh_credential(ref, hdr);
    disclosure_unlock(hdr);
}

/* ── Accessors (disclosure-aware) ──────────────────────────────── */

void* mental_ref_data(mental_ref ref,
                       const void *credential, size_t credential_len) {
    if (!ref || !ref->addr) return NULL;

    /* Owner always has full access — but still refresh credential */
    if (ref->owner) {
        if (ref->credential_fn) {
            struct disclosure_header *hdr = ref_header(ref);
            disclosure_lock(hdr);
            refresh_credential(ref, hdr);
            disclosure_unlock(hdr);
        }
        return ref_user_data(ref);
    }

    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);

    /* Refresh owner credential from provider (in-process only) */
    refresh_credential(ref, hdr);

    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);
    int granted = 0;

    switch (mode) {
    case MENTAL_RELATIONALLY_OPEN:
        granted = 1;
        break;

    case MENTAL_RELATIONALLY_INCLUSIVE:
        /* Read access without credential; write access is advisory. */
        granted = 1;
        break;

    case MENTAL_RELATIONALLY_EXCLUSIVE:
        granted = credential_matches(hdr, credential, credential_len);
        break;
    }

    disclosure_unlock(hdr);
    return granted ? ref_user_data(ref) : NULL;
}

size_t mental_ref_size(mental_ref ref) {
    return ref ? ref->user_size : 0;
}

int mental_ref_writable(mental_ref ref,
                         const void *credential, size_t credential_len) {
    if (!ref || !ref->addr) return 0;

    /* Owner always writable — but still refresh credential */
    if (ref->owner) {
        if (ref->credential_fn) {
            struct disclosure_header *hdr = ref_header(ref);
            disclosure_lock(hdr);
            refresh_credential(ref, hdr);
            disclosure_unlock(hdr);
        }
        return 1;
    }

    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);

    /* Refresh owner credential from provider (in-process only) */
    refresh_credential(ref, hdr);

    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);
    int writable = 0;

    switch (mode) {
    case MENTAL_RELATIONALLY_OPEN:
        writable = 1;
        break;

    case MENTAL_RELATIONALLY_INCLUSIVE:
        writable = credential_matches(hdr, credential, credential_len) ? 1 : 0;
        break;

    case MENTAL_RELATIONALLY_EXCLUSIVE:
        writable = credential_matches(hdr, credential, credential_len) ? 1 : 0;
        break;
    }

    disclosure_unlock(hdr);
    return writable;
}

/* ── Ownership query ───────────────────────────────────────────── */

int mental_ref_is_owner(mental_ref ref) {
    if (!ref) return 0;
    return ref->owner;
}

/* ── Clone (snapshot into local ref) ───────────────────────────── */

/*
 * Clone creates a new locally-owned ref seeded with the current value
 * of the source.  If the source is a cross-process observer handle,
 * this breaks the linkage — the result is an independent local copy
 * under this process's UUID namespace.
 *
 * The caller provides a new_name for the clone.  The credential is
 * used to obtain read access on the source (required for exclusive
 * disclosure; ignored for open/inclusive).
 *
 * Returns NULL if the source is inaccessible or allocation fails.
 */
mental_ref mental_ref_clone(mental_ref ref, const char *new_name,
                             const void *credential, size_t credential_len) {
    if (!ref || !ref->addr || !new_name || !new_name[0])
        return NULL;

    /* Read the source data (disclosure-checked) */
    void *src = mental_ref_data(ref, credential, credential_len);
    if (!src) return NULL;

    size_t sz = ref->user_size;

    /* Create a new owned ref with the same data size */
    mental_ref clone = mental_ref_create(new_name, sz);
    if (!clone) return NULL;

    /* Copy the current observed value */
    void *dst = ref_user_data(clone);
    memcpy(dst, src, sz);

    return clone;
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
