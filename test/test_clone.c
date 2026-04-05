/* Clone test: unpinned clone, clone independence, and clone-with-device
 * (GPU pin-on-clone).
 *
 * GPU pinning tests are included but gracefully skip if no device is
 * available (headless CI without GPU).
 *
 * References are now process-local (no shared memory, no cross-process).
 */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failures++; \
        return 1; \
    } \
} while(0)

/* ── Clone unpinned (CPU-only) ─────────────────────────────────── */

static int test_clone_unpinned(void) {
    printf("  clone unpinned...\n");

    size_t sz = 512;
    mental_reference ref = mental_reference_create(sz, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "create failed");

    /* Write test pattern */
    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "data NULL");
    for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)(i & 0xFF);

    /* Clone without device (CPU-only) */
    mental_reference clone = mental_reference_clone(ref,
                                                     NULL, NULL, 0);
    ASSERT(clone != NULL, "clone returned NULL");
    ASSERT(mental_reference_size(clone) == sz, "clone size mismatch");
    ASSERT(mental_reference_is_pinned(clone) == 0, "unpinned clone should not be pinned");

    /* Verify data matches */
    uint8_t *clone_data = (uint8_t *)mental_reference_data(clone, NULL, 0);
    ASSERT(clone_data != NULL, "clone data NULL");
    ASSERT(memcmp(data, clone_data, sz) == 0, "clone data mismatch");

    mental_reference_close(clone);
    mental_reference_close(ref);
    return 0;
}

/* ── Clone independence (modifying original doesn't affect clone) ── */

static int test_clone_independence(void) {
    printf("  clone independence...\n");

    mental_reference ref = mental_reference_create(16, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "create failed");

    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    memset(data, 0xAA, 16);

    mental_reference clone = mental_reference_clone(ref,
                                                     NULL, NULL, 0);
    ASSERT(clone != NULL, "clone failed");

    /* Modify original */
    memset(data, 0xBB, 16);

    /* Clone should be unchanged */
    uint8_t *clone_data = (uint8_t *)mental_reference_data(clone, NULL, 0);
    for (int i = 0; i < 16; i++) {
        ASSERT(clone_data[i] == 0xAA, "clone was modified by original");
    }

    mental_reference_close(clone);
    mental_reference_close(ref);
    return 0;
}

/* ── Clone with device (GPU pin-on-clone) ──────────────────────── */

static int test_clone_with_device(void) {
    printf("  clone with device...\n");

    int count = mental_device_count();
    if (count == 0) {
        printf("    (skipped: no GPU devices)\n");
        return 0;
    }

    mental_device dev = mental_device_get(0);
    size_t sz = 256;

    mental_reference ref = mental_reference_create(sz, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "create failed");

    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)(i);

    /* Clone and pin to device in one shot */
    mental_reference clone = mental_reference_clone(ref,
                                                     dev, NULL, 0);
    ASSERT(clone != NULL, "clone with device failed");
    ASSERT(mental_reference_is_pinned(clone) == 1, "clone should be pinned");

    /* Read back from GPU and verify */
    uint8_t readback[256];
    mental_reference_read(clone, readback, sz);
    ASSERT(memcmp(data, readback, sz) == 0, "GPU clone data mismatch");

    mental_reference_close(clone);
    mental_reference_close(ref);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
    printf("Clone test suite (process-local)\n");
    printf("================================\n\n");

    test_clone_unpinned();
    test_clone_independence();
    test_clone_with_device();

    printf("\n");
    if (g_failures == 0) {
        printf("All tests passed.\n");
    } else {
        printf("%d test(s) FAILED.\n", g_failures);
    }
    return g_failures > 0 ? 1 : 0;
}
