/* stdlink test: round-trip send/recv via socketpair */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <pthread.h>
#endif

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while(0)

/*
 * Round-trip test: a helper thread reads from the peer fd (simulating
 * a sparked child) and echoes the record back.
 */

struct echo_ctx {
    int peer_fd;
};

#ifndef _WIN32

static void *echo_thread(void *arg) {
    struct echo_ctx *ctx = (struct echo_ctx *)arg;
    int fd = ctx->peer_fd;

    /* Read length-prefixed record from peer fd */
    uint32_t net_len;
    if (read(fd, &net_len, 4) != 4) return NULL;
    uint32_t len = ntohl(net_len);

    char *buf = malloc(len);
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += (size_t)r;
    }

    /* Echo it back (write length-prefixed to peer fd) */
    write(fd, &net_len, 4);
    write(fd, buf, len);
    free(buf);
    return NULL;
}

#endif /* _WIN32 */

int main(void) {
    printf("Testing stdlink...\n");

    /* Test fd creation */
    int fd = mental_stdlink();
    printf("  stdlink fd = %d\n", fd);
    ASSERT(fd >= 0, "mental_stdlink() returned -1");

    int peer = mental_stdlink_peer();
    printf("  stdlink peer fd = %d\n", peer);
    ASSERT(peer >= 0, "mental_stdlink_peer() returned -1");
    ASSERT(fd != peer, "near and far fds must differ");

    /* Test idempotency */
    ASSERT(mental_stdlink() == fd, "mental_stdlink() not idempotent");
    ASSERT(mental_stdlink_peer() == peer, "mental_stdlink_peer() not idempotent");

#ifndef _WIN32
    /* Round-trip: send from near, echo thread reads from far and writes back,
     * then recv on near picks up the echo. */
    struct echo_ctx ctx = { .peer_fd = peer };
    pthread_t tid;
    ASSERT(pthread_create(&tid, NULL, echo_thread, &ctx) == 0,
           "Failed to create echo thread");

    const char *msg = "hello stdlink";
    int rc = mental_stdlink_send(msg, strlen(msg));
    ASSERT(rc == 0, "mental_stdlink_send failed");

    char buf[256];
    size_t out_len = 0;
    rc = mental_stdlink_recv(buf, sizeof(buf), &out_len);
    ASSERT(rc == 0, "mental_stdlink_recv failed");
    ASSERT(out_len == strlen(msg), "recv length mismatch");
    ASSERT(memcmp(buf, msg, out_len) == 0, "recv data mismatch");

    pthread_join(tid, NULL);

    printf("  round-trip: sent \"%s\", received \"%.*s\" (%zu bytes)\n",
           msg, (int)out_len, buf, out_len);

    /* Test zero-length record */
    struct echo_ctx ctx2 = { .peer_fd = peer };
    ASSERT(pthread_create(&tid, NULL, echo_thread, &ctx2) == 0,
           "Failed to create echo thread for empty record");

    rc = mental_stdlink_send(NULL, 0);
    ASSERT(rc == 0, "mental_stdlink_send(NULL, 0) failed");

    out_len = 999;
    rc = mental_stdlink_recv(buf, sizeof(buf), &out_len);
    ASSERT(rc == 0, "mental_stdlink_recv for empty record failed");
    ASSERT(out_len == 0, "empty record should have length 0");

    pthread_join(tid, NULL);
    printf("  zero-length record: OK\n");
#else
    printf("  (round-trip test skipped on Windows — needs thread helper)\n");
#endif

    printf("PASS: stdlink\n");
    return 0;
}
