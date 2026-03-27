/* Clone test: unpinned clone, clone independence, cross-process clone,
 * disclosure-checked clone, and clone-with-device (GPU pin-on-clone).
 *
 * GPU pinning tests are included but gracefully skip if no device is
 * available (headless CI without GPU).
 */
#include "../mental.h"
#include "mental_test_fork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#endif

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
    mental_reference ref = mental_reference_create("CloneUnpin", sz);
    ASSERT(ref != NULL, "create failed");

    /* Write test pattern */
    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "data NULL");
    for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)(i & 0xFF);

    /* Clone without device (CPU-only) */
    mental_reference clone = mental_reference_clone(ref, "CloneUnpinCopy",
                                                     NULL, NULL, 0);
    ASSERT(clone != NULL, "clone returned NULL");
    ASSERT(mental_reference_is_owner(clone) == 1, "clone should be owned");
    ASSERT(mental_reference_size(clone) == sz, "clone size mismatch");
    ASSERT(mental_reference_is_pinned(clone) == 0, "unpinned clone should not be pinned");

    /* Verify data copied correctly */
    uint8_t *clone_data = (uint8_t *)mental_reference_data(clone, NULL, 0);
    ASSERT(clone_data != NULL, "clone data NULL");
    for (size_t i = 0; i < sz; i++) {
        ASSERT(clone_data[i] == (uint8_t)(i & 0xFF), "clone data mismatch");
    }

    mental_reference_close(clone);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Clone independence ────────────────────────────────────────── */

static int test_clone_independence(void) {
    printf("  clone independence...\n");

    size_t sz = 256;
    mental_reference ref = mental_reference_create("CloneIndep", sz);
    ASSERT(ref != NULL, "create failed");

    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    memset(data, 0xAA, sz);

    mental_reference clone = mental_reference_clone(ref, "CloneIndepCopy",
                                                     NULL, NULL, 0);
    ASSERT(clone != NULL, "clone returned NULL");

    /* Modify clone — original should be unchanged */
    uint8_t *clone_data = (uint8_t *)mental_reference_data(clone, NULL, 0);
    memset(clone_data, 0xBB, sz);

    ASSERT(data[0] == 0xAA, "original should be unchanged after clone write");
    ASSERT(clone_data[0] == 0xBB, "clone should have new data");

    /* Modify original — clone should be unchanged */
    memset(data, 0xCC, sz);
    ASSERT(clone_data[0] == 0xBB, "clone should be unchanged after original write");

    mental_reference_close(clone);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Clone from observer (breaks linkage) ──────────────────────── */

static int test_clone_from_observer(void) {
    printf("  clone from observer...\n");

    size_t sz = 128;
    mental_reference ref = mental_reference_create("CloneFromObs", sz);
    ASSERT(ref != NULL, "create failed");

    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    memset(data, 0x55, sz);

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "CloneFromObs");
    ASSERT(obs != NULL, "open failed");
    ASSERT(mental_reference_is_owner(obs) == 0, "observer should not be owner");

    /* Clone from observer — creates a new owned reference */
    mental_reference clone = mental_reference_clone(obs, "CloneFromObsCopy",
                                                     NULL, NULL, 0);
    ASSERT(clone != NULL, "clone from observer returned NULL");
    ASSERT(mental_reference_is_owner(clone) == 1, "clone should be owned");

    /* Verify data was copied */
    uint8_t *clone_data = (uint8_t *)mental_reference_data(clone, NULL, 0);
    ASSERT(clone_data != NULL, "clone data NULL");
    ASSERT(clone_data[0] == 0x55, "clone should have observer's data");

    /* Modify original — clone should be independent */
    data[0] = 0x99;
    ASSERT(clone_data[0] == 0x55, "clone should be independent of original");

    mental_reference_close(clone);
    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Clone with disclosure (credential required) ───────────────── */

static int test_clone_disclosure(void) {
    printf("  clone with disclosure...\n");

    const uint8_t cred[] = { 0xDE, 0xAD };

    mental_reference ref = mental_reference_create("CloneDiscl", 128);
    ASSERT(ref != NULL, "create failed");

    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    memset(data, 0x33, 128);

    /* Set exclusive disclosure */
    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);
    mental_reference_set_credential(ref, cred, sizeof(cred));

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "CloneDiscl");
    ASSERT(obs != NULL, "open failed");

    /* Clone without credential should fail (can't read source data) */
    mental_reference bad_clone = mental_reference_clone(obs, "CloneDisclBad",
                                                         NULL, NULL, 0);
    ASSERT(bad_clone == NULL, "clone without credential should fail on exclusive ref");

    /* Clone with correct credential should succeed */
    mental_reference good_clone = mental_reference_clone(obs, "CloneDisclGood",
                                                          NULL, cred, sizeof(cred));
    ASSERT(good_clone != NULL, "clone with credential should succeed");

    uint8_t *clone_data = (uint8_t *)mental_reference_data(good_clone, NULL, 0);
    ASSERT(clone_data != NULL, "clone data NULL");
    ASSERT(clone_data[0] == 0x33, "clone data should match source");

    /* Clone's disclosure defaults to OPEN (fresh reference) */
    ASSERT(mental_reference_get_disclosure(good_clone) == MENTAL_RELATIONALLY_OPEN,
           "clone disclosure should default to OPEN");

    mental_reference_close(good_clone);
    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Cross-process clone ──────────────────────────────────────── */

struct clone_xproc_shared {
    char parent_uuid[33];
    int results[4];
};

static int clone_xproc_child(void *shared_ptr, size_t shared_size) {
    (void)shared_size;
    struct clone_xproc_shared *s = (struct clone_xproc_shared *)shared_ptr;
    size_t sz = 256;

    mental_reference obs = mental_reference_open(s->parent_uuid, "CloneXProc");
    if (!obs) return 1;

    mental_reference clone = mental_reference_clone(obs, "CloneXProcChild", NULL, NULL, 0);
    if (!clone) { mental_reference_close(obs); return 2; }

    uint8_t *cdata = (uint8_t *)mental_reference_data(clone, NULL, 0);
    if (!cdata) return 3;

    int match = 1;
    for (size_t i = 0; i < sz; i++) {
        if (cdata[i] != (uint8_t)((i * 7) & 0xFF)) { match = 0; break; }
    }
    s->results[0] = match;

    memset(cdata, 0xFF, sz);
    s->results[1] = 1;
    s->results[2] = mental_reference_is_owner(clone);
    s->results[3] = mental_reference_size(clone) == sz;

    mental_reference_close(clone);
    mental_reference_close(obs);
    return 0;
}

static int test_clone_cross_process(void) {
    printf("  clone cross-process...\n");

    size_t sz = 256;
    mental_reference ref = mental_reference_create("CloneXProc", sz);
    ASSERT(ref != NULL, "create failed");

    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)((i * 7) & 0xFF);

    struct clone_xproc_shared *shared = NULL;
    mental_test_shm shm = mental_test_shm_create("clone_xproc", sizeof(*shared), (void**)&shared);
    ASSERT(shared != NULL, "shared memory creation failed");
    strncpy(shared->parent_uuid, mental_uuid(), 32);
    shared->parent_uuid[32] = '\0';

    int exit_code;
#ifdef _WIN32
    exit_code = mental_test_run_child(&shm, "clone_xproc_child", shared, sizeof(*shared));
#else
    MENTAL_TEST_FORK(clone_xproc_child, shared, sizeof(*shared), exit_code);
#endif

    ASSERT(exit_code == 0, "child failed");
    ASSERT(shared->results[0] == 1, "child: clone data should match parent");
    ASSERT(shared->results[1] == 1, "child: clone was modified");
    ASSERT(shared->results[2] == 1, "child: clone should be owned");
    ASSERT(shared->results[3] == 1, "child: clone size should match");

    ASSERT(data[0] == (uint8_t)(0 & 0xFF), "parent data should be unchanged");
    ASSERT(data[100] == (uint8_t)((100 * 7) & 0xFF), "parent data integrity check");

    mental_test_shm_destroy(&shm);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Clone with GPU pinning (gracefully skips if no GPU) ───────── */

static int test_clone_with_device(void) {
    printf("  clone with device (pin-on-clone)...\n");

    mental_device dev = mental_device_get(0);
    if (!dev) {
        printf("    SKIP (no GPU device)\n");
        return 0;
    }

    size_t sz = 1024;
    mental_reference ref = mental_reference_create("CloneGPU", sz);
    ASSERT(ref != NULL, "create failed");

    /* Write test data to shm */
    float *data = (float *)mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "data NULL");
    int count = (int)(sz / sizeof(float));
    for (int i = 0; i < count; i++) data[i] = (float)i;

    /* Clone with device — should create pinned clone */
    mental_reference clone = mental_reference_clone(ref, "CloneGPUCopy",
                                                     dev, NULL, 0);
    ASSERT(clone != NULL, "clone with device returned NULL");
    ASSERT(mental_reference_is_pinned(clone) == 1, "clone should be GPU-pinned");
    ASSERT(mental_reference_device(clone) == dev, "clone device should match");

    /* Read back from GPU to verify data was uploaded */
    float readback[256]; /* 1024 / 4 */
    memset(readback, 0, sizeof(readback));
    mental_reference_read(clone, readback, sz);

    int match = 1;
    for (int i = 0; i < count; i++) {
        if (readback[i] != (float)i) { match = 0; break; }
    }
    ASSERT(match, "GPU clone data should match source");

    /* Source should NOT be pinned (we didn't pin it) */
    ASSERT(mental_reference_is_pinned(ref) == 0, "source should not be pinned");

    mental_reference_close(clone);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Pin then clone (both pinned) ──────────────────────────────── */

static int test_clone_pinned_source(void) {
    printf("  clone pinned source...\n");

    mental_device dev = mental_device_get(0);
    if (!dev) {
        printf("    SKIP (no GPU device)\n");
        return 0;
    }

    size_t sz = 512;
    mental_reference ref = mental_reference_create("ClonePinSrc", sz);
    ASSERT(ref != NULL, "create failed");

    /* Pin and write data (goes to GPU + shm) */
    ASSERT(mental_reference_pin(ref, dev) == 0, "pin failed");
    ASSERT(mental_reference_is_pinned(ref) == 1, "should be pinned");

    float values[128]; /* 512 / 4 */
    for (int i = 0; i < 128; i++) values[i] = (float)i * 3.14f;
    mental_reference_write(ref, values, sz);

    /* Clone with same device */
    mental_reference clone = mental_reference_clone(ref, "ClonePinSrcCopy",
                                                     dev, NULL, 0);
    ASSERT(clone != NULL, "clone returned NULL");
    ASSERT(mental_reference_is_pinned(clone) == 1, "clone should be pinned");

    /* Verify via GPU read */
    float readback[128];
    memset(readback, 0, sizeof(readback));
    mental_reference_read(clone, readback, sz);

    int match = 1;
    for (int i = 0; i < 128; i++) {
        if (readback[i] != values[i]) { match = 0; break; }
    }
    ASSERT(match, "pinned clone data should match source");

    mental_reference_close(clone);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    mental_test_register_child("clone_xproc_child", clone_xproc_child);
    if (mental_test_child_dispatch(argc, argv)) return 0;

    printf("Testing clone...\n");

    test_clone_unpinned();
    test_clone_independence();
    test_clone_from_observer();
    test_clone_disclosure();
    test_clone_with_device();
    test_clone_pinned_source();
    test_clone_cross_process();

    if (g_failures > 0) {
        fprintf(stderr, "FAIL: %d test(s) failed\n", g_failures);
        return 1;
    }

    printf("PASS: clone\n");
    return 0;
}
