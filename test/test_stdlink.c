/* stdlink test: round-trip send/recv via socketpair, cross-process via fork.
 *
 * Tests both in-process (thread-based echo) and cross-process (fork-based)
 * communication through the stdlink channel.
 */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
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

    /* ── fd creation ───────────────────────────────────────── */

    int fd = mental_stdlink();
    printf("  stdlink fd = %d\n", fd);
    ASSERT(fd >= 0, "mental_stdlink() returned -1");

    int peer = mental_stdlink_peer();
    printf("  stdlink peer fd = %d\n", peer);
    ASSERT(peer >= 0, "mental_stdlink_peer() returned -1");
    ASSERT(fd != peer, "near and far fds must differ");

    /* Idempotency */
    ASSERT(mental_stdlink() == fd, "mental_stdlink() not idempotent");
    ASSERT(mental_stdlink_peer() == peer, "mental_stdlink_peer() not idempotent");

    printf("  fd creation: OK\n");

#ifndef _WIN32

    /* ── Thread-based round-trip (in-process echo) ─────────── */

    {
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

        printf("  thread round-trip: sent \"%s\", received \"%.*s\" (%zu bytes)\n",
               msg, (int)out_len, buf, out_len);
    }

    /* ── Zero-length record ────────────────────────────────── */

    {
        struct echo_ctx ctx = { .peer_fd = peer };
        pthread_t tid;
        ASSERT(pthread_create(&tid, NULL, echo_thread, &ctx) == 0,
               "Failed to create echo thread for empty record");

        int rc = mental_stdlink_send(NULL, 0);
        ASSERT(rc == 0, "mental_stdlink_send(NULL, 0) failed");

        char buf[256];
        size_t out_len = 999;
        rc = mental_stdlink_recv(buf, sizeof(buf), &out_len);
        ASSERT(rc == 0, "mental_stdlink_recv for empty record failed");
        ASSERT(out_len == 0, "empty record should have length 0");

        pthread_join(tid, NULL);
        printf("  zero-length record: OK\n");
    }

    /* ── Large record ──────────────────────────────────────── */

    {
        struct echo_ctx ctx = { .peer_fd = peer };
        pthread_t tid;
        ASSERT(pthread_create(&tid, NULL, echo_thread, &ctx) == 0,
               "Failed to create echo thread for large record");

        /* 64 KiB record */
        size_t big_len = 65536;
        char *big = malloc(big_len);
        for (size_t i = 0; i < big_len; i++) big[i] = (char)(i & 0xFF);

        int rc = mental_stdlink_send(big, big_len);
        ASSERT(rc == 0, "send large record failed");

        char *big_recv = malloc(big_len);
        size_t out_len = 0;
        rc = mental_stdlink_recv(big_recv, big_len, &out_len);
        ASSERT(rc == 0, "recv large record failed");
        ASSERT(out_len == big_len, "large record length mismatch");
        ASSERT(memcmp(big, big_recv, big_len) == 0, "large record data mismatch");

        pthread_join(tid, NULL);
        free(big);
        free(big_recv);

        printf("  large record (64 KiB): OK\n");
    }

    /* ── Multiple sequential records ───────────────────────── */

    {
        /* Send 10 records quickly, echo thread handles one at a time */
        const char *messages[] = {
            "msg-0", "msg-1", "msg-2", "msg-3", "msg-4",
            "msg-5", "msg-6", "msg-7", "msg-8", "msg-9"
        };
        int count = 10;

        for (int i = 0; i < count; i++) {
            struct echo_ctx ctx = { .peer_fd = peer };
            pthread_t tid;
            ASSERT(pthread_create(&tid, NULL, echo_thread, &ctx) == 0,
                   "Failed to create echo thread");

            int rc = mental_stdlink_send(messages[i], strlen(messages[i]));
            ASSERT(rc == 0, "send sequential record failed");

            char buf[256];
            size_t out_len = 0;
            rc = mental_stdlink_recv(buf, sizeof(buf), &out_len);
            ASSERT(rc == 0, "recv sequential record failed");
            ASSERT(out_len == strlen(messages[i]), "sequential length mismatch");
            ASSERT(memcmp(buf, messages[i], out_len) == 0, "sequential data mismatch");

            pthread_join(tid, NULL);
        }

        printf("  sequential records (10x): OK\n");
    }

    /* ── Cross-process via fork() ──────────────────────────── */

    {
        /* Create a FRESH socketpair for the cross-process test.
         * The existing stdlink fds have been used for in-process echo tests
         * and their state (buffered data, etc.) shouldn't leak across.
         *
         * We use raw socketpair + length-prefix protocol to simulate what
         * spark will do: parent keeps near end, child inherits far end. */
        int sv[2];
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0,
               "socketpair failed");

        pid_t pid = fork();
        if (pid == 0) {
            /* ── Child: read from sv[1], echo back ─────────── */
            close(sv[0]);
            int child_fd = sv[1];

            /* Read one length-prefixed record */
            uint32_t net_len;
            if (read(child_fd, &net_len, 4) != 4) _exit(1);
            uint32_t len = ntohl(net_len);

            char *buf = malloc(len + 1);
            size_t got = 0;
            while (got < len) {
                ssize_t r = read(child_fd, buf + got, len - got);
                if (r <= 0) _exit(2);
                got += (size_t)r;
            }

            /* Transform and echo back: uppercase the message */
            for (uint32_t i = 0; i < len; i++) {
                if (buf[i] >= 'a' && buf[i] <= 'z')
                    buf[i] -= 32;
            }

            /* Write back length-prefixed */
            if (write(child_fd, &net_len, 4) != 4) _exit(3);
            if (write(child_fd, buf, len) != (ssize_t)len) _exit(4);

            free(buf);
            close(child_fd);
            _exit(0);
        }

        /* ── Parent: send on sv[0], recv transformed response ── */
        close(sv[1]);
        int parent_fd = sv[0];

        const char *msg = "hello from parent";
        size_t msg_len = strlen(msg);
        uint32_t net_len = htonl((uint32_t)msg_len);

        ASSERT(write(parent_fd, &net_len, 4) == 4, "parent write len failed");
        ASSERT(write(parent_fd, msg, msg_len) == (ssize_t)msg_len,
               "parent write data failed");

        /* Read response */
        uint32_t resp_net_len;
        ASSERT(read(parent_fd, &resp_net_len, 4) == 4, "parent read resp len failed");
        uint32_t resp_len = ntohl(resp_net_len);
        ASSERT(resp_len == msg_len, "response length mismatch");

        char resp[256];
        size_t got = 0;
        while (got < resp_len) {
            ssize_t r = read(parent_fd, resp + got, resp_len - got);
            ASSERT(r > 0, "parent read resp data failed");
            got += (size_t)r;
        }

        /* Child should have uppercased the message */
        ASSERT(memcmp(resp, "HELLO FROM PARENT", resp_len) == 0,
               "cross-process response mismatch");

        close(parent_fd);

        int status;
        waitpid(pid, &status, 0);
        ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "child process failed");

        printf("  cross-process (fork + socketpair): OK\n");
    }

#else
    printf("  (round-trip and cross-process tests skipped on Windows)\n");
#endif

    printf("PASS: stdlink\n");
    return 0;
}
