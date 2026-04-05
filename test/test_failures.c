/*
 * Failure & edge case test suite.
 *
 * Tests what happens when things go wrong with references
 * and disclosure handles.
 */

#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_tests = 0;
static int g_passed = 0;

#define TEST(name) do { g_tests++; printf("  %-60s ", name); fflush(stdout); } while(0)
#define PASS() do { g_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── Reference failures ───────────────────────────────────────── */

static void test_reference_failures(void) {
    TEST("ref: create with zero size returns NULL");
    if (mental_reference_create(0, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL) == NULL)
        PASS(); else FAIL("expected NULL");

    TEST("ref: pin to NULL device returns -1");
    mental_reference ref = mental_reference_create(16, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    if (mental_reference_pin(ref, NULL) != 0) PASS(); else FAIL("expected failure");
    mental_reference_close(ref);

    TEST("ref: write beyond capacity clamps silently");
    ref = mental_reference_create(4, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    uint8_t big_data[64];
    memset(big_data, 0xAB, 64);
    mental_reference_write(ref, big_data, 64);
    uint8_t readback[4];
    mental_reference_read(ref, readback, 4);
    if (readback[0] == 0xAB && readback[3] == 0xAB) PASS(); else FAIL("data not clamped correctly");
    mental_reference_close(ref);

    TEST("ref: double close is safe");
    ref = mental_reference_create(8, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_close(ref);
    mental_reference_close(NULL);
    PASS();

    TEST("ref: clone with wrong credential returns NULL");
    ref = mental_reference_create(16, MENTAL_RELATIONALLY_EXCLUSIVE, "secret", 6, NULL);
    mental_reference ref2 = mental_reference_clone(ref, NULL, "wrong", 5);
    if (ref2 == NULL) PASS(); else { FAIL("expected NULL"); mental_reference_close(ref2); }
    mental_reference_close(ref);

    TEST("ref: clone with correct credential succeeds");
    ref = mental_reference_create(16, MENTAL_RELATIONALLY_EXCLUSIVE, "secret", 6, NULL);
    uint8_t *data = (uint8_t *)mental_reference_data(ref, "secret", 6);
    if (data) data[0] = 42;
    ref2 = mental_reference_clone(ref, NULL, "secret", 6);
    if (ref2 != NULL) {
        uint8_t *cloned = (uint8_t *)mental_reference_data(ref2, NULL, 0);
        if (cloned && cloned[0] == 42) PASS(); else FAIL("data mismatch");
        mental_reference_close(ref2);
    } else { FAIL("clone returned NULL"); }
    mental_reference_close(ref);

    TEST("ref: data on NULL returns NULL");
    if (mental_reference_data(NULL, NULL, 0) == NULL) PASS(); else FAIL("expected NULL");

    TEST("ref: size on NULL returns 0");
    if (mental_reference_size(NULL) == 0) PASS(); else FAIL("expected 0");

    TEST("ref: writable on NULL returns 0");
    if (mental_reference_writable(NULL, NULL, 0) == 0) PASS(); else FAIL("expected 0");

    TEST("ref: is_pinned on NULL returns 0");
    if (mental_reference_is_pinned(NULL) == 0) PASS(); else FAIL("expected 0");

    TEST("ref: device on NULL returns NULL");
    if (mental_reference_device(NULL) == NULL) PASS(); else FAIL("expected NULL");
}

/* ── Disclosure handle failures ───────────────────────────────── */

static void test_disclosure_failures(void) {
    TEST("disclosure: set_mode on NULL handle is safe");
    mental_disclosure_set_mode(NULL, MENTAL_RELATIONALLY_EXCLUSIVE);
    PASS();

    TEST("disclosure: set_credential on NULL handle is safe");
    mental_disclosure_set_credential(NULL, "x", 1);
    PASS();

    TEST("disclosure: close NULL handle is safe");
    mental_disclosure_close(NULL);
    PASS();

    TEST("disclosure: double close is safe");
    mental_disclosure dh = NULL;
    mental_reference ref = mental_reference_create(8, MENTAL_RELATIONALLY_OPEN, NULL, 0, &dh);
    mental_disclosure_close(dh);
    mental_disclosure_close(NULL);
    PASS();
    mental_reference_close(ref);

    TEST("disclosure: exclusive with empty credential denies everything");
    ref = mental_reference_create(8, MENTAL_RELATIONALLY_EXCLUSIVE, NULL, 0, NULL);
    void *data = mental_reference_data(ref, NULL, 0);
    if (data == NULL) PASS(); else FAIL("should deny with no credential set");
    mental_reference_close(ref);
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("Failure & edge case test suite\n");
    printf("==============================\n\n");

    test_reference_failures();
    test_disclosure_failures();

    printf("\n%d/%d tests passed\n", g_passed, g_tests);
    return g_passed == g_tests ? 0 : 1;
}
