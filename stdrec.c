/*
 * Mental - Standard Record Channel (stdrec)
 *
 * Provides a bidirectional fd for length-prefixed record exchange.
 * Unix: socketpair(AF_UNIX, SOCK_STREAM)
 * Windows: two anonymous pipe pairs (near->far, far->near)
 */

#include "mental.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>
#endif

/*
 * Platform state
 */

#ifdef _WIN32

/* Windows: two pipe pairs simulate bidirectional channel.
 * near_write -> far_read  (local sends, peer receives)
 * far_write  -> near_read (peer sends, local receives) */
static HANDLE g_near_read  = INVALID_HANDLE_VALUE;
static HANDLE g_near_write = INVALID_HANDLE_VALUE;
static HANDLE g_far_read   = INVALID_HANDLE_VALUE;
static HANDLE g_far_write  = INVALID_HANDLE_VALUE;
static int g_stdrec_init   = 0;
static CRITICAL_SECTION g_stdrec_cs;
static int g_cs_init = 0;

static void stdrec_init_lock(void) {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_stdrec_cs);
        g_cs_init = 1;
    }
    EnterCriticalSection(&g_stdrec_cs);
}

static void stdrec_init_unlock(void) {
    LeaveCriticalSection(&g_stdrec_cs);
}

static int stdrec_create(void) {
    HANDLE nr, nw, fr, fw;
    if (!CreatePipe(&fr, &nw, NULL, 0)) return -1;
    if (!CreatePipe(&nr, &fw, NULL, 0)) {
        CloseHandle(fr);
        CloseHandle(nw);
        return -1;
    }
    g_near_read  = nr;
    g_near_write = nw;
    g_far_read   = fr;
    g_far_write  = fw;
    return 0;
}

/* Write exactly n bytes to a HANDLE. */
static int write_all(HANDLE h, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        DWORD written;
        if (!WriteFile(h, p, (DWORD)left, &written, NULL))
            return -1;
        p += written;
        left -= written;
    }
    return 0;
}

/* Read exactly n bytes from a HANDLE. */
static int read_all(HANDLE h, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t left = n;
    while (left > 0) {
        DWORD got;
        if (!ReadFile(h, p, (DWORD)left, &got, NULL) || got == 0)
            return -1;
        p += got;
        left -= got;
    }
    return 0;
}

#else /* Unix */

static int g_stdrec_near = -1;
static int g_stdrec_far  = -1;
static int g_stdrec_init = 0;
static pthread_mutex_t g_stdrec_lock = PTHREAD_MUTEX_INITIALIZER;

static void stdrec_init_lock(void) {
    pthread_mutex_lock(&g_stdrec_lock);
}

static void stdrec_init_unlock(void) {
    pthread_mutex_unlock(&g_stdrec_lock);
}

static int stdrec_create(void) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        return -1;
    g_stdrec_near = fds[0];
    g_stdrec_far  = fds[1];
    return 0;
}

/* Write exactly n bytes. */
static int write_all_fd(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return 0;
}

/* Read exactly n bytes. */
static int read_all_fd(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        left -= (size_t)r;
    }
    return 0;
}

#endif /* _WIN32 */

/*
 * Lazy initialization
 */

static int ensure_stdrec(void) {
    if (g_stdrec_init) return 0;

    stdrec_init_lock();
    if (!g_stdrec_init) {
        if (stdrec_create() == 0)
            g_stdrec_init = 1;
    }
    stdrec_init_unlock();
    return g_stdrec_init ? 0 : -1;
}

/*
 * Public API
 */

int mental_stdrec(void) {
    if (ensure_stdrec() < 0) return -1;
#ifdef _WIN32
    /* On Windows return a pseudo-fd (the HANDLE cast to int).
     * Callers needing the raw HANDLE can cast back. */
    return (int)(intptr_t)g_near_read;
#else
    return g_stdrec_near;
#endif
}

int mental_stdrec_peer(void) {
    if (ensure_stdrec() < 0) return -1;
#ifdef _WIN32
    return (int)(intptr_t)g_far_read;
#else
    return g_stdrec_far;
#endif
}

int mental_stdrec_send(const void *data, size_t len) {
    if (ensure_stdrec() < 0) return -1;
    if (!data && len > 0) return -1;

    /* Frame: [4 bytes length (network order)][payload] */
    uint32_t net_len = htonl((uint32_t)len);

#ifdef _WIN32
    if (write_all(g_near_write, &net_len, 4) < 0) return -1;
    if (len > 0 && write_all(g_near_write, data, len) < 0) return -1;
#else
    if (write_all_fd(g_stdrec_near, &net_len, 4) < 0) return -1;
    if (len > 0 && write_all_fd(g_stdrec_near, data, len) < 0) return -1;
#endif
    return 0;
}

int mental_stdrec_recv(void *buf, size_t buf_len, size_t *out_len) {
    if (ensure_stdrec() < 0) return -1;

    /* Read 4-byte length header */
    uint32_t net_len;
#ifdef _WIN32
    if (read_all(g_near_read, &net_len, 4) < 0) return -1;
#else
    if (read_all_fd(g_stdrec_near, &net_len, 4) < 0) return -1;
#endif

    uint32_t payload_len = ntohl(net_len);
    if (out_len) *out_len = payload_len;

    /* Read payload — copy up to buf_len, discard the rest */
    size_t to_copy = payload_len < buf_len ? payload_len : buf_len;
    size_t to_discard = payload_len - to_copy;

#ifdef _WIN32
    if (to_copy > 0 && read_all(g_near_read, buf, to_copy) < 0) return -1;
    /* Drain excess */
    while (to_discard > 0) {
        char tmp[512];
        size_t chunk = to_discard < sizeof(tmp) ? to_discard : sizeof(tmp);
        if (read_all(g_near_read, tmp, chunk) < 0) return -1;
        to_discard -= chunk;
    }
#else
    if (to_copy > 0 && read_all_fd(g_stdrec_near, buf, to_copy) < 0) return -1;
    while (to_discard > 0) {
        char tmp[512];
        size_t chunk = to_discard < sizeof(tmp) ? to_discard : sizeof(tmp);
        if (read_all_fd(g_stdrec_near, tmp, chunk) < 0) return -1;
        to_discard -= chunk;
    }
#endif

    return 0;
}
