/*
 * Mental - Spark (Pipe-Based Parent→Child IPC)
 *
 * Provides bidirectional communication between a parent process and
 * its sparked children via inherited pipes.
 *
 * The link IS the pipe.  No shared memory, no ring buffers, no
 * semaphores, no named kernel objects.
 *
 * Unix: socketpair(AF_UNIX, SOCK_STREAM), child end at fd 42.
 * Windows: two anonymous pipe pairs, child ends via env vars.
 *
 * Wire protocol: length-prefixed records matching stdlink:
 *   [4 bytes: uint32 payload length, network byte order][payload]
 *
 * When either side dies, the other detects it via EOF on the pipe.
 */

#define _POSIX_C_SOURCE 200809L

#include "mental.h"
#include "mental_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

/* ── Network byte order ───────────────────────────────────────── */

#ifdef _WIN32
static uint32_t spark_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static uint32_t spark_ntohl(uint32_t v) { return spark_htonl(v); }
#else
#include <arpa/inet.h>
#define spark_htonl htonl
#define spark_ntohl ntohl
#endif

/* ── Well-known fd for sparked children ───────────────────────── */

#define MENTAL_SPARK_FD 3

/* ── Pipe I/O helpers ─────────────────────────────────────────── */

#ifdef _WIN32

static int pipe_write_all(HANDLE h, const void *buf, size_t n) {
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

static int pipe_read_all(HANDLE h, void *buf, size_t n) {
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

static int pipe_write_all(int fd, const void *buf, size_t n) {
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

static int pipe_read_all(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;  /* EOF or error — peer died */
        }
        p += r;
        left -= (size_t)r;
    }
    return 0;
}

#endif /* Unix */

/* ── Link I/O ─────────────────────────────────────────────────── */

int mental_link_send(mental_link link, const void *data, size_t len) {
    if (!link) return -1;
    if (!data && len > 0) return -1;

    uint32_t net_len = spark_htonl((uint32_t)len);
#ifdef _WIN32
    if (pipe_write_all(link->pipe_write, &net_len, 4) < 0) return -1;
    if (len > 0 && pipe_write_all(link->pipe_write, data, len) < 0) return -1;
#else
    if (pipe_write_all(link->pipe_fd, &net_len, 4) < 0) return -1;
    if (len > 0 && pipe_write_all(link->pipe_fd, data, len) < 0) return -1;
#endif
    return 0;
}

int mental_link_recv(mental_link link, void *buf, size_t buf_len, size_t *out_len) {
    if (!link) return -1;

    uint32_t net_len;
#ifdef _WIN32
    if (pipe_read_all(link->pipe_read, &net_len, 4) < 0) return -1;
#else
    if (pipe_read_all(link->pipe_fd, &net_len, 4) < 0) return -1;
#endif
    uint32_t payload_len = spark_ntohl(net_len);
    if (out_len) *out_len = payload_len;

    /* Read payload — copy up to buf_len, discard excess */
    size_t to_read = payload_len < buf_len ? payload_len : buf_len;
    if (to_read > 0 && buf) {
#ifdef _WIN32
        if (pipe_read_all(link->pipe_read, buf, to_read) < 0) return -1;
#else
        if (pipe_read_all(link->pipe_fd, buf, to_read) < 0) return -1;
#endif
    }
    /* Discard excess if message larger than buffer */
    if (payload_len > buf_len) {
        size_t excess = payload_len - buf_len;
        char discard[256];
        while (excess > 0) {
            size_t chunk = excess < sizeof(discard) ? excess : sizeof(discard);
#ifdef _WIN32
            if (pipe_read_all(link->pipe_read, discard, chunk) < 0) return -1;
#else
            if (pipe_read_all(link->pipe_fd, discard, chunk) < 0) return -1;
#endif
            excess -= chunk;
        }
    }
    return 0;
}

void mental_link_close(mental_link link) {
    if (!link) return;
#ifdef _WIN32
    if (link->pipe_read != INVALID_HANDLE_VALUE)
        CloseHandle(link->pipe_read);
    if (link->pipe_write != INVALID_HANDLE_VALUE && link->pipe_write != link->pipe_read)
        CloseHandle(link->pipe_write);
#else
    if (link->pipe_fd >= 0)
        close(link->pipe_fd);
#endif
    free(link);
}

/* ── Link creation helper ─────────────────────────────────────── */

static mental_link pipe_link_wrap(int fd) {
    mental_link link = calloc(1, sizeof(struct mental_link_t));
    if (!link) return NULL;
#ifdef _WIN32
    link->pipe_read = INVALID_HANDLE_VALUE;
    link->pipe_write = INVALID_HANDLE_VALUE;
#else
    link->pipe_fd = fd;
#endif
    return link;
}

/* ── Child spark (mental_spark / mental_sparked) ──────────────── */

static mental_link g_sparked_link = NULL;
static int g_sparked_checked = 0;

mental_link mental_sparked(void) {
    if (g_sparked_checked) return g_sparked_link;
    g_sparked_checked = 1;

    /* Check for the MENTAL_SPARK env var — set by the parent's Spark().
     * The value is the fd number (Unix) or read/write handle pair (Windows). */
    const char *env = getenv("MENTAL_SPARK");
    if (!env || !env[0]) return NULL;

#ifdef _WIN32
    /* Windows: env contains "read_handle,write_handle" in hex */
    const char *comma = strchr(env, ',');
    if (!comma) return NULL;
    mental_link link = calloc(1, sizeof(struct mental_link_t));
    if (!link) return NULL;
    link->pipe_read  = (HANDLE)(uintptr_t)strtoull(env, NULL, 16);
    link->pipe_write = (HANDLE)(uintptr_t)strtoull(comma + 1, NULL, 16);
    _putenv_s("MENTAL_SPARK", "");
#else
    int fd = atoi(env);
    if (fd < 0 || fcntl(fd, F_GETFD) < 0) return NULL;
    mental_link link = pipe_link_wrap(fd);
    if (!link) return NULL;
    unsetenv("MENTAL_SPARK");
#endif

    g_sparked_link = link;
    return g_sparked_link;
}

/* Wrap an existing file descriptor as a link.
 * The fd must be a connected bidirectional channel (e.g., socketpair).
 * The link takes ownership — mental_link_close will close the fd.
 * Returns NULL on failure. */
mental_link mental_link_wrap_fd(int fd) {
    if (fd < 0) return NULL;
    return pipe_link_wrap(fd);
}
