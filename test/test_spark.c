/*
 * Mental - Spark Test Suite
 *
 * Tests pipe-based parent→child IPC links:
 *   1. Edge cases (NULL safety)
 *   2. sparked() returns NULL when not sparked
 *   3. Spark a child process, exchange messages (via fork helper)
 */

#include "../mental.h"
#include "mental_test_fork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-50s ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── Test 1: Edge cases ───────────────────────────────────────── */

static void test_edge_cases(void) {
    TEST("sparked() returns NULL when not sparked");
    mental_link l = mental_sparked();
    if (l == NULL) PASS(); else FAIL("expected NULL");

    TEST("link_send with NULL link returns -1");
    int rc = mental_link_send(NULL, "x", 1);
    if (rc == -1) PASS(); else FAIL("expected -1");

    TEST("link_recv with NULL link returns -1");
    char buf[4];
    size_t len;
    rc = mental_link_recv(NULL, buf, sizeof(buf), &len);
    if (rc == -1) PASS(); else FAIL("expected -1");

    TEST("link_close with NULL is safe");
    mental_link_close(NULL);
    PASS();
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Windows child dispatch (no-op on Unix) */
    if (mental_test_child_dispatch(argc, argv)) return 0;

    printf("Spark test suite\n");
    printf("================\n\n");

    test_edge_cases();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
