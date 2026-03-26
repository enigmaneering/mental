/*
 * Mental - Reference (Unified Shareable Data)
 *
 * A reference is a named, UUID-scoped shared memory region that can
 * optionally be pinned to a GPU device for compute operations.
 *
 * Every reference has shared-memory backing:
 *   /mental-{uuid}/{name}
 *
 * The creating process owns the shm.  When it exits, all its regions
 * are automatically unlinked via atexit.
 *
 * Observers (sparked children or peers) open a reference by providing
 * the owner's UUID and the name.  If the owner has exited, the open
 * returns NULL gracefully.
 *
 * GPU pinning (optional):
 *   mental_reference_pin(ref, device) attaches a backend buffer.
 *   mental_reference_write/read transfer between host and GPU.
 *   Pinned references participate in dispatch and viewport operations.
 */

#define _POSIX_C_SOURCE 200809L

#include "mental.h"
#include "mental_internal.h"
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
#include "mental_pthread.h"
#include <time.h>
#endif

/* ── UUID ──────────────────────────────────────────────────────── */

static char g_uuid[33];
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
    typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
    HMODULE advapi = LoadLibraryA("advapi32.dll");
    if (advapi) {
        RtlGenRandomFn fn = (RtlGenRandomFn)GetProcAddress(advapi, "SystemFunction036");
        if (fn) fn(buf, sizeof(buf));
        FreeLibrary(advapi);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, sizeof(buf));
        (void)r;
        close(fd);
    }
#endif

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

/* ── Reference tracking (for atexit cleanup) ───────────────────── */

#define MAX_REFS 256

static char* g_ref_paths[MAX_REFS];
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
 * Build a shm name from the uuid+name pair.
 *
 * Always hashes to a compact fixed-width form: "/m-{24 hex}" (27 chars).
 * This is portable across all platforms (macOS PSHMNAMLEN = 31, Linux
 * PATH_MAX = 4096) and avoids ever hitting name-length limits regardless
 * of how long the user's reference names are.
 *
 * Uniqueness comes from two salted FNV-1a 64-bit hashes (96 bits total).
 * The uuid scopes per-process; the name scopes per-reference within a
 * process — so collisions require both to match, which can't happen
 * across different (uuid, name) pairs with overwhelming probability.
 */

/* FNV-1a 64-bit — fast, no crypto needed, just uniqueness */
static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint64_t)(unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int build_shm_path(char *dst, size_t dst_len,
                           const char *uuid, const char *name) {
    char full[640];
    snprintf(full, sizeof(full), "%s-%s", uuid, name);
    uint64_t h1 = fnv1a(full);

    char salted[640];
    snprintf(salted, sizeof(salted), "%s-%s-salt", uuid, name);
    uint64_t h2 = fnv1a(salted);

    static const char hex[] = "0123456789abcdef";
    char tag[25]; /* 24 hex chars + nul */
    for (int i = 0; i < 8; i++) {
        tag[i * 2]     = hex[(h1 >> (i * 8 + 4)) & 0xF];
        tag[i * 2 + 1] = hex[(h1 >> (i * 8))     & 0xF];
    }
    for (int i = 0; i < 4; i++) {
        tag[16 + i * 2]     = hex[(h2 >> (i * 8 + 4)) & 0xF];
        tag[16 + i * 2 + 1] = hex[(h2 >> (i * 8))     & 0xF];
    }
    tag[24] = '\0';

    int n = snprintf(dst, dst_len, "/m-%s", tag);
    return (n > 0 && (size_t)n < dst_len) ? 0 : -1;
}

/* ── Disclosure header (lives at offset 0 in the shm region) ───── */

#define DISCLOSURE_CREDENTIAL_MAX 128
#define DISCLOSURE_HEADER_SIZE    256  /* padded for alignment */

#ifdef _MSC_VER
#include <intrin.h>
#define ATOMIC_LOAD32(p)       (*(volatile uint32_t*)(p))
#define ATOMIC_STORE32(p,v)    (*(volatile uint32_t*)(p) = (v))
#define ATOMIC_EXCHANGE32(p,v) _InterlockedExchange((volatile long*)(p), (long)(v))
#else
#include <stdatomic.h>
#define ATOMIC_LOAD32(p)       atomic_load_explicit((_Atomic uint32_t*)(p), memory_order_acquire)
#define ATOMIC_STORE32(p,v)    atomic_store_explicit((_Atomic uint32_t*)(p), (v), memory_order_release)
#define ATOMIC_EXCHANGE32(p,v) atomic_exchange_explicit((_Atomic uint32_t*)(p), (v), memory_order_acquire)
#endif

struct disclosure_header {
    uint32_t mode;
    uint32_t credential_len;
    uint8_t  credential[DISCLOSURE_CREDENTIAL_MAX];
    uint32_t lock;
};

/* ── Disclosure spinlock ───────────────────────────────────────── */

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

/* ── Internal helpers ──────────────────────────────────────────── */

static struct disclosure_header* ref_header(mental_reference ref) {
    return (struct disclosure_header *)ref->addr;
}

static void* ref_user_data(mental_reference ref) {
    return (char *)ref->addr + DISCLOSURE_HEADER_SIZE;
}

static int credential_matches(struct disclosure_header *hdr,
                               const void *credential, size_t credential_len) {
    if (!credential || credential_len == 0) return 0;
    if (credential_len != hdr->credential_len) return 0;
    return memcmp(hdr->credential, credential, credential_len) == 0;
}

static void refresh_credential(mental_reference ref,
                                struct disclosure_header *hdr) {
    if (!ref->credential_fn) return;

    size_t out_len = 0;
    uint8_t buf[DISCLOSURE_CREDENTIAL_MAX];
    ref->credential_fn(ref->credential_ctx, buf, sizeof(buf), &out_len);

    if (out_len > DISCLOSURE_CREDENTIAL_MAX)
        out_len = DISCLOSURE_CREDENTIAL_MAX;
    memcpy(hdr->credential, buf, out_len);
    hdr->credential_len = (uint32_t)out_len;
}

/* ── Create (owner) ────────────────────────────────────────────── */

mental_reference mental_reference_create(const char *name, size_t size) {
    if (!name || !name[0] || size == 0) return NULL;

    ensure_uuid();

    char path[320];
    if (build_shm_path(path, sizeof(path), g_uuid, name) < 0)
        return NULL;

    size_t total = DISCLOSURE_HEADER_SIZE + size;

    mental_reference ref = calloc(1, sizeof(struct mental_reference_t));
    if (!ref) return NULL;

    strncpy(ref->path, path, sizeof(ref->path) - 1);
    ref->total_size = total;
    ref->user_size  = size;
    ref->owner = 1;
    ref->valid = 1;
    ref->device = NULL;
    ref->backend_buffer = NULL;
    pthread_mutex_init(&ref->lock, NULL);

#ifdef _WIN32
    ref->hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)(total >> 32), (DWORD)total, path + 1);
    if (!ref->hMap) {
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    ref->addr = MapViewOfFile(ref->hMap, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!ref->addr) {
        CloseHandle(ref->hMap);
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }
#else
    int fd = shm_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    if (ftruncate(fd, (off_t)total) < 0) {
        close(fd);
        shm_unlink(path);
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    void *addr = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        shm_unlink(path);
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    ref->fd   = fd;
    ref->addr = addr;
#endif

    struct disclosure_header *hdr = (struct disclosure_header *)ref->addr;
    memset(hdr, 0, DISCLOSURE_HEADER_SIZE);
    ATOMIC_STORE32(&hdr->mode, MENTAL_RELATIONALLY_OPEN);
    ATOMIC_STORE32(&hdr->lock, 0);

    track_ref(path);
    return ref;
}

/* ── Open (observer) ───────────────────────────────────────────── */

mental_reference mental_reference_open(const char *peer_uuid, const char *name) {
    if (!peer_uuid || !peer_uuid[0] || !name || !name[0])
        return NULL;

    char path[320];
    if (build_shm_path(path, sizeof(path), peer_uuid, name) < 0)
        return NULL;

    mental_reference ref = calloc(1, sizeof(struct mental_reference_t));
    if (!ref) return NULL;

    strncpy(ref->path, path, sizeof(ref->path) - 1);
    ref->owner = 0;
    ref->valid = 1;
    ref->device = NULL;
    ref->backend_buffer = NULL;
    pthread_mutex_init(&ref->lock, NULL);

#ifdef _WIN32
    ref->hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, path + 1);
    if (!ref->hMap) {
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    MEMORY_BASIC_INFORMATION info;
    ref->addr = MapViewOfFile(ref->hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!ref->addr) {
        CloseHandle(ref->hMap);
        pthread_mutex_destroy(&ref->lock);
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
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        return NULL;
    }

    void *addr = mmap(NULL, (size_t)st.st_size,
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        pthread_mutex_destroy(&ref->lock);
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

/* ── Disclosure API ────────────────────────────────────────────── */

mental_disclosure mental_reference_get_disclosure(mental_reference ref) {
    if (!ref || !ref->addr) return MENTAL_RELATIONALLY_OPEN;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);
    disclosure_unlock(hdr);
    return mode;
}

void mental_reference_set_disclosure(mental_reference ref, mental_disclosure mode) {
    if (!ref || !ref->addr || !ref->owner) return;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    ATOMIC_STORE32(&hdr->mode, (uint32_t)mode);
    disclosure_unlock(hdr);
}

void mental_reference_set_credential(mental_reference ref,
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
    ref->credential_fn  = NULL;
    ref->credential_ctx = NULL;
    disclosure_unlock(hdr);
}

void mental_reference_set_credential_provider(mental_reference ref,
                                               mental_credential_fn fn,
                                               void *ctx) {
    if (!ref || !ref->addr || !ref->owner) return;
    struct disclosure_header *hdr = ref_header(ref);
    disclosure_lock(hdr);
    ref->credential_fn  = fn;
    ref->credential_ctx = ctx;
    if (fn) refresh_credential(ref, hdr);
    disclosure_unlock(hdr);
}

/* ── Accessors (disclosure-aware) ──────────────────────────────── */

void* mental_reference_data(mental_reference ref,
                             const void *credential, size_t credential_len) {
    if (!ref || !ref->addr || !ref->valid) return NULL;

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
    refresh_credential(ref, hdr);

    mental_disclosure mode = (mental_disclosure)ATOMIC_LOAD32(&hdr->mode);
    int granted = 0;

    switch (mode) {
    case MENTAL_RELATIONALLY_OPEN:
        granted = 1;
        break;
    case MENTAL_RELATIONALLY_INCLUSIVE:
        granted = 1;
        break;
    case MENTAL_RELATIONALLY_EXCLUSIVE:
        granted = credential_matches(hdr, credential, credential_len);
        break;
    }

    disclosure_unlock(hdr);
    return granted ? ref_user_data(ref) : NULL;
}

size_t mental_reference_size(mental_reference ref) {
    return (ref && ref->valid) ? ref->user_size : 0;
}

int mental_reference_writable(mental_reference ref,
                               const void *credential,
                               size_t credential_len) {
    if (!ref || !ref->addr || !ref->valid) return 0;

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

int mental_reference_is_owner(mental_reference ref) {
    if (!ref) return 0;
    return ref->owner;
}

/* ── GPU pinning ───────────────────────────────────────────────── */

int mental_reference_pin(mental_reference ref, mental_device device) {
    if (!ref || !ref->valid || !device) return -1;

    pthread_mutex_lock(&ref->lock);

    /* Already pinned to this device — no-op */
    if (ref->device == device && ref->backend_buffer) {
        pthread_mutex_unlock(&ref->lock);
        return 0;
    }

    /* If pinned to a different device, free old buffer first */
    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_destroy(ref->backend_buffer);
        ref->backend_buffer = NULL;
    }

    ref->device = device;
    ref->backend_buffer = device->backend->buffer_alloc(
        device->backend_device, ref->user_size);

    if (!ref->backend_buffer) {
        ref->device = NULL;
        pthread_mutex_unlock(&ref->lock);
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED,
                         "Backend buffer allocation failed during pin");
        return -1;
    }

    /* Upload current shm contents to GPU */
    void *src = ref_user_data(ref);
    device->backend->buffer_write(ref->backend_buffer, src, ref->user_size);

    pthread_mutex_unlock(&ref->lock);
    return 0;
}

int mental_reference_is_pinned(mental_reference ref) {
    if (!ref || !ref->valid) return 0;
    return ref->backend_buffer != NULL;
}

mental_device mental_reference_device(mental_reference ref) {
    if (!ref || !ref->valid) return NULL;
    return ref->device;
}

/* ── Write / Read (GPU + shm) ──────────────────────────────────── */

void mental_reference_write(mental_reference ref,
                             const void *data, size_t bytes) {
    if (!ref || !ref->valid || !data || bytes == 0) return;

    pthread_mutex_lock(&ref->lock);

    size_t write_size = bytes < ref->user_size ? bytes : ref->user_size;

    /* Always write to shm (for cross-process visibility) */
    memcpy(ref_user_data(ref), data, write_size);

    /* If pinned, also write to GPU */
    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_write(
            ref->backend_buffer, data, write_size);
    }

    pthread_mutex_unlock(&ref->lock);
}

void mental_reference_read(mental_reference ref,
                            void *data, size_t bytes) {
    if (!ref || !ref->valid || !data || bytes == 0) return;

    pthread_mutex_lock(&ref->lock);

    size_t read_size = bytes < ref->user_size ? bytes : ref->user_size;

    if (ref->backend_buffer && ref->device) {
        /* Pinned: read from GPU */
        ref->device->backend->buffer_read(
            ref->backend_buffer, data, read_size);
    } else {
        /* Not pinned: read from shm */
        memcpy(data, ref_user_data(ref), read_size);
    }

    pthread_mutex_unlock(&ref->lock);
}

/* ── Clone (snapshot into local reference) ─────────────────────── */

mental_reference mental_reference_clone(mental_reference ref,
                                         const char *new_name,
                                         mental_device device,
                                         const void *credential,
                                         size_t credential_len) {
    if (!ref || !ref->addr || !ref->valid || !new_name || !new_name[0])
        return NULL;

    /* Read the source data (disclosure-checked) */
    void *src = mental_reference_data(ref, credential, credential_len);
    if (!src) return NULL;

    size_t sz = ref->user_size;

    /* Create a new owned reference with the same data size */
    mental_reference clone = mental_reference_create(new_name, sz);
    if (!clone) return NULL;

    /* Copy the current observed value */
    memcpy(ref_user_data(clone), src, sz);

    /* Pin to GPU if device provided (cross-boundary clone-and-pin) */
    if (device) {
        if (mental_reference_pin(clone, device) != 0) {
            mental_reference_close(clone);
            return NULL;
        }
    }

    return clone;
}

/* ── Close ─────────────────────────────────────────────────────── */

void mental_reference_close(mental_reference ref) {
    if (!ref) return;

    pthread_mutex_lock(&ref->lock);

    /* Free GPU buffer if pinned */
    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_destroy(ref->backend_buffer);
        ref->backend_buffer = NULL;
        ref->device = NULL;
    }

    ref->valid = 0;

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

    pthread_mutex_unlock(&ref->lock);
    pthread_mutex_destroy(&ref->lock);
    free(ref);
}
