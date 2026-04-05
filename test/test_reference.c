/* Reference test: UUID, create/close, disclosure, credential,
 * write/read, clone.
 *
 * References are now process-local (no shared memory).
 * Cross-process sharing goes through the Manifest system.
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

/* ── Create / Close ────────────────────────────────────────────── */

static int test_create_close(void) {
    printf("  Create and close...\n");

    mental_reference ref = mental_reference_create(256, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "Create should succeed");

    ASSERT(mental_reference_size(ref) == 256, "Size should be 256");

    void *data = mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "Data pointer should not be NULL");

    /* Data should be zero-initialized */
    uint8_t *bytes = (uint8_t *)data;
    int all_zero = 1;
    for (int i = 0; i < 256; i++) {
        if (bytes[i] != 0) { all_zero = 0; break; }
    }
    ASSERT(all_zero, "Data should be zero-initialized");

    mental_reference_close(ref);

    /* Closing NULL should be safe */
    mental_reference_close(NULL);

    return 0;
}

/* ── Write / Read ──────────────────────────────────────────────── */

static int test_write_read(void) {
    printf("  Write and read...\n");

    mental_reference ref = mental_reference_create(64, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "Create should succeed");

    /* Write data */
    uint8_t write_data[64];
    for (int i = 0; i < 64; i++) write_data[i] = (uint8_t)(i * 3);
    mental_reference_write(ref, write_data, 64);

    /* Read data back */
    uint8_t read_data[64];
    memset(read_data, 0, 64);
    mental_reference_read(ref, read_data, 64);

    ASSERT(memcmp(write_data, read_data, 64) == 0, "Read should match write");

    /* Also verify via data pointer */
    void *ptr = mental_reference_data(ref, NULL, 0);
    ASSERT(memcmp(ptr, write_data, 64) == 0, "Data pointer should match write");

    mental_reference_close(ref);
    return 0;
}

/* ── Disclosure ────────────────────────────────────────────────── */

static int test_disclosure(void) {
    printf("  Disclosure modes...\n");

    /* Create with disclosure handle */
    mental_disclosure dh = NULL;
    mental_reference ref = mental_reference_create(32, MENTAL_RELATIONALLY_OPEN, NULL, 0, &dh);
    ASSERT(ref != NULL, "Create should succeed");
    ASSERT(dh != NULL, "Disclosure handle should not be NULL");

    /* Default is open */
    ASSERT(mental_reference_get_disclosure(ref) == MENTAL_RELATIONALLY_OPEN,
           "Default should be open");

    /* Open: anyone can access */
    void *data = mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "Open: should have access");
    ASSERT(mental_reference_writable(ref, NULL, 0) == 1,
           "Open: should be writable");

    /* Set credential via disclosure handle */
    const char *cred = "secret123";
    mental_disclosure_set_credential(dh, cred, strlen(cred));

    /* Inclusive: read without cred, write requires cred */
    mental_disclosure_set_mode(dh, MENTAL_RELATIONALLY_INCLUSIVE);
    ASSERT(mental_reference_get_disclosure(ref) == MENTAL_RELATIONALLY_INCLUSIVE,
           "Should be inclusive");
    ASSERT(mental_reference_data(ref, NULL, 0) != NULL,
           "Inclusive: read without cred should work");
    ASSERT(mental_reference_writable(ref, NULL, 0) == 0,
           "Inclusive: write without cred should fail");
    ASSERT(mental_reference_writable(ref, cred, strlen(cred)) == 1,
           "Inclusive: write with cred should work");

    /* Exclusive: all access requires cred */
    mental_disclosure_set_mode(dh, MENTAL_RELATIONALLY_EXCLUSIVE);
    ASSERT(mental_reference_data(ref, NULL, 0) == NULL,
           "Exclusive: no cred should deny");
    ASSERT(mental_reference_data(ref, cred, strlen(cred)) != NULL,
           "Exclusive: correct cred should grant");
    ASSERT(mental_reference_data(ref, "wrong", 5) == NULL,
           "Exclusive: wrong cred should deny");

    mental_disclosure_close(dh);
    mental_reference_close(ref);
    return 0;
}

/* ── Clone ─────────────────────────────────────────────────────── */

static int test_clone(void) {
    printf("  Clone...\n");

    mental_reference ref = mental_reference_create(16, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "Create should succeed");

    /* Write some data */
    uint32_t vals[4] = {10, 20, 30, 40};
    mental_reference_write(ref, vals, sizeof(vals));

    /* Clone it */
    mental_reference clone = mental_reference_clone(ref, NULL, NULL, 0);
    ASSERT(clone != NULL, "Clone should succeed");
    ASSERT(mental_reference_size(clone) == 16, "Clone size should match");

    /* Verify clone has the same data */
    uint32_t clone_vals[4];
    mental_reference_read(clone, clone_vals, sizeof(clone_vals));
    ASSERT(memcmp(vals, clone_vals, sizeof(vals)) == 0,
           "Clone data should match original");

    /* Modify original — clone should NOT change (independent copy) */
    vals[0] = 999;
    mental_reference_write(ref, vals, sizeof(vals));
    mental_reference_read(clone, clone_vals, sizeof(clone_vals));
    ASSERT(clone_vals[0] == 10, "Clone should be independent of original");

    mental_reference_close(clone);
    mental_reference_close(ref);
    return 0;
}

/* ── Edge cases ────────────────────────────────────────────────── */

static int test_edge_cases(void) {
    printf("  Edge cases...\n");

    /* Zero size */
    ASSERT(mental_reference_create(0, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL) == NULL, "Zero size should fail");

    /* Operations on NULL ref */
    ASSERT(mental_reference_data(NULL, NULL, 0) == NULL, "NULL ref data should be NULL");
    ASSERT(mental_reference_size(NULL) == 0, "NULL ref size should be 0");
    ASSERT(mental_reference_writable(NULL, NULL, 0) == 0, "NULL ref writable should be 0");

    return 0;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
    printf("Reference test suite (process-local)\n");
    printf("====================================\n\n");

    test_create_close();
    test_write_read();
    test_disclosure();
    test_clone();
    test_edge_cases();

    printf("\n");
    if (g_failures == 0) {
        printf("All tests passed.\n");
    } else {
        printf("%d test(s) FAILED.\n", g_failures);
    }
    return g_failures > 0 ? 1 : 0;
}
