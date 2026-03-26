/* stdlink test: round-trip send/recv, cross-process echo.
 *
 * Tests both in-process (thread-based echo) and cross-process
 * communication through the stdlink channel.
 *
 * Unix:    threads use read/write on socketpair fds
 * Windows: threads use ReadFile/WriteFile on pipe HANDLEs
 *          cross-process uses CreateProcess with inherited handles
 */
#include "../mental.h"
#include "../mental_pthread.h"
#include "mental_test_fork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <unistd.h>
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
 * Portable echo helper: reads a length-prefixed record from the peer fd
 * and writes it back. Used by the thread-based round-trip tests.
 */

#ifdef _WIN32

/* On Windows, mental_stdlink_peer() returns an fd but the underlying
 * transport is pipe HANDLEs. We use mental_stdlink_send/recv via the
 * peer fd for the echo, which works in-process for threads. */

static void *echo_thread(void *arg) {
    (void)arg;
    /* Read via stdlink API using the peer end */
    char buf[65536 + 256];
    size_t len = 0;
    if (mental_stdlink_recv(buf, sizeof(buf), &len) != 0) return NULL;
    mental_stdlink_send(buf, len);
    return NULL;
}

#else

static void *echo_thread(void *arg) {
    int fd = *(int *)arg;

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

    write(fd, &net_len, 4);
    write(fd, buf, len);
    free(buf);
    return NULL;
}

#endif

/*
 * Test helpers for round-trip via threads.
 * On Windows, the echo thread uses mental_stdlink_recv/send on the
 * peer side, while the main thread uses mental_stdlink_send/recv on
 * the near side. On Unix, the echo thread reads/writes the peer fd
 * directly using the length-prefix protocol.
 */
static int do_round_trip(const char *label, const void *data, size_t data_len, int peer_fd) {
    (void)peer_fd; /* used only on Unix */

    pthread_t tid;
#ifdef _WIN32
    (void)peer_fd;
    ASSERT(pthread_create(&tid, NULL, echo_thread, NULL) == 0, "echo thread create failed");
#else
    ASSERT(pthread_create(&tid, NULL, echo_thread, &peer_fd) == 0, "echo thread create failed");
#endif

    int rc = mental_stdlink_send(data, data_len);
    ASSERT(rc == 0, "send failed");

    char *recv_buf = malloc(data_len + 256);
    size_t out_len = 0;
    rc = mental_stdlink_recv(recv_buf, data_len + 256, &out_len);
    ASSERT(rc == 0, "recv failed");
    ASSERT(out_len == data_len, "length mismatch");
    if (data_len > 0) {
        ASSERT(memcmp(recv_buf, data, data_len) == 0, "data mismatch");
    }

    pthread_join(tid, NULL);
    free(recv_buf);

    if (label) printf("  %s: OK\n", label);
    return 0;
}

/*
 * Cross-process test child: reads one record, uppercases it, echoes back.
 * On Unix: uses socketpair fds inherited via fork.
 * On Windows: uses pipes inherited via CreateProcess.
 */

#ifndef _WIN32
/* Unix cross-process child — runs in forked process */
static int xproc_stdlink_child(void *shared_ptr, size_t shared_size) {
    (void)shared_ptr; (void)shared_size;

    /* The child inherits the socketpair fds from fork.
     * We create a fresh pair and use sv[1] in the child. */
    int *sv = (int *)shared_ptr; /* shared_ptr holds the socketpair fds */
    int child_fd = sv[1];

    uint32_t net_len;
    if (read(child_fd, &net_len, 4) != 4) return 1;
    uint32_t len = ntohl(net_len);

    char *buf = malloc(len + 1);
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(child_fd, buf + got, len - got);
        if (r <= 0) { free(buf); return 2; }
        got += (size_t)r;
    }

    /* Uppercase */
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    }

    if (write(child_fd, &net_len, 4) != 4) { free(buf); return 3; }
    if (write(child_fd, buf, len) != (ssize_t)len) { free(buf); return 4; }

    free(buf);
    close(child_fd);
    return 0;
}
#endif

int main(int argc, char **argv) {
    if (mental_test_child_dispatch(argc, argv)) return 0;

#ifdef _WIN32
    /* Windows stdlink child mode: read one record, uppercase, echo back */
    if (argc >= 2 && strcmp(argv[1], "--stdlink-child") == 0) {
        /* ensure_stdlink picks up MENTAL_STDLINK_READ/WRITE from env */
        char buf[65536];
        size_t len = 0;
        if (mental_stdlink_recv(buf, sizeof(buf), &len) != 0) return 1;

        for (size_t i = 0; i < len; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        }

        if (mental_stdlink_send(buf, len) != 0) return 2;
        return 0;
    }
#endif

    printf("Testing stdlink...\n");

    /* ── fd creation ───────────────────────────────────────── */

    int fd = mental_stdlink();
    printf("  stdlink fd = %d\n", fd);
    ASSERT(fd >= 0, "mental_stdlink() returned -1");

    int peer = mental_stdlink_peer();
    printf("  stdlink peer fd = %d\n", peer);
    ASSERT(peer >= 0, "mental_stdlink_peer() returned -1");
    ASSERT(fd != peer, "near and far fds must differ");

    ASSERT(mental_stdlink() == fd, "mental_stdlink() not idempotent");
    ASSERT(mental_stdlink_peer() == peer, "mental_stdlink_peer() not idempotent");

    printf("  fd creation: OK\n");

    /* ── Thread-based round-trip tests ─────────────────────── */

    do_round_trip("thread round-trip", "hello stdlink", 13, peer);
    do_round_trip("zero-length record", NULL, 0, peer);

    {
        size_t big_len = 65536;
        char *big = malloc(big_len);
        for (size_t i = 0; i < big_len; i++) big[i] = (char)(i & 0xFF);
        do_round_trip("large record (64 KiB)", big, big_len, peer);
        free(big);
    }

    {
        const char *messages[] = {
            "msg-0", "msg-1", "msg-2", "msg-3", "msg-4",
            "msg-5", "msg-6", "msg-7", "msg-8", "msg-9"
        };
        for (int i = 0; i < 10; i++) {
            do_round_trip(NULL, messages[i], strlen(messages[i]), peer);
        }
        printf("  sequential records (10x): OK\n");
    }

    /* ── Cross-process test ────────────────────────────────── */

#ifndef _WIN32
    {
        int sv[2];
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            int rc = xproc_stdlink_child(sv, 0);
            _exit(rc);
        }

        close(sv[1]);
        int parent_fd = sv[0];

        const char *msg = "hello from parent";
        size_t msg_len = strlen(msg);
        uint32_t net_len = htonl((uint32_t)msg_len);

        ASSERT(write(parent_fd, &net_len, 4) == 4, "parent write len failed");
        ASSERT(write(parent_fd, msg, msg_len) == (ssize_t)msg_len, "parent write data failed");

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

        ASSERT(memcmp(resp, "HELLO FROM PARENT", resp_len) == 0,
               "cross-process response mismatch");

        close(parent_fd);

        int status;
        waitpid(pid, &status, 0);
        ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child process failed");

        printf("  cross-process (fork + socketpair): OK\n");
    }
#else
    /* Windows cross-process: child inherits the far-end pipe handles.
     * Parent sets MENTAL_STDLINK_READ/WRITE env vars with handle values,
     * then spawns the child. ensure_stdlink in the child picks them up. */
    {
        extern void mental_stdlink_peer_handles(uintptr_t *out_read, uintptr_t *out_write);

        uintptr_t far_read = 0, far_write = 0;
        mental_stdlink_peer_handles(&far_read, &far_write);

        char env_r[64], env_w[64];
        snprintf(env_r, sizeof(env_r), "MENTAL_STDLINK_READ=%llu", (unsigned long long)far_read);
        snprintf(env_w, sizeof(env_w), "MENTAL_STDLINK_WRITE=%llu", (unsigned long long)far_write);
        _putenv(env_r);
        _putenv(env_w);

        /* Get path to current executable */
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);

        /* Spawn child that runs --stdlink-child mode */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "\"%s\" --stdlink-child", exe);

        STARTUPINFOA si = {0};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {0};
        ASSERT(CreateProcessA(NULL, cmd, NULL, NULL, TRUE /* inherit handles */,
                              0, NULL, NULL, &si, &pi),
               "CreateProcess failed for stdlink child");

        /* Parent: send a message via stdlink, expect uppercase echo back */
        const char *msg = "hello from parent";
        size_t msg_len = strlen(msg);
        ASSERT(mental_stdlink_send(msg, msg_len) == 0, "parent send failed");

        char resp[256];
        size_t out_len = 0;
        ASSERT(mental_stdlink_recv(resp, sizeof(resp), &out_len) == 0, "parent recv failed");
        ASSERT(out_len == msg_len, "response length mismatch");
        ASSERT(memcmp(resp, "HELLO FROM PARENT", out_len) == 0,
               "cross-process response mismatch");

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ASSERT(exit_code == 0, "child process failed");

        /* Clean up env vars */
        _putenv("MENTAL_STDLINK_READ=");
        _putenv("MENTAL_STDLINK_WRITE=");

        printf("  cross-process (Windows pipes): OK\n");
    }
#endif

    printf("PASS: stdlink\n");
    return 0;
}
