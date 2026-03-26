/* Reference test: UUID, create/open/close, disclosure matrix, credential
 * providers, cross-process access via fork().
 *
 * Covers the entire mental_reference_* surface area except GPU pinning
 * (see test_clone.c and test_buffer.c for pinning tests).
 */
#include "../mental.h"
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

/* ── UUID ──────────────────────────────────────────────────────── */

static int test_uuid(void) {
    printf("  uuid...\n");

    const char *uuid = mental_uuid();
    ASSERT(uuid != NULL, "mental_uuid() returned NULL");
    ASSERT(strlen(uuid) == 32, "UUID should be 32 hex chars");

    /* Idempotent — same pointer every time */
    ASSERT(mental_uuid() == uuid, "mental_uuid() not idempotent");

    /* All lowercase hex */
    for (int i = 0; i < 32; i++) {
        char c = uuid[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        ASSERT(ok, "UUID contains non-hex character");
    }

    printf("    OK\n");
    return 0;
}

/* ── Create / close lifecycle ──────────────────────────────────── */

static int test_lifecycle(void) {
    printf("  lifecycle...\n");

    size_t sz = 4096;
    mental_reference ref = mental_reference_create("Lifecycle", sz);
    ASSERT(ref != NULL, "create returned NULL");

    void *data = mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "data returned NULL on owner");
    ASSERT(mental_reference_size(ref) == sz, "size mismatch");
    ASSERT(mental_reference_is_owner(ref) == 1, "creator should be owner");

    /* Write pattern */
    memset(data, 0xAB, sz);

    /* Close and verify re-open fails */
    mental_reference_close(ref);

    const char *uuid = mental_uuid();
    mental_reference gone = mental_reference_open(uuid, "Lifecycle");
    ASSERT(gone == NULL, "open after owner close should return NULL");

    printf("    OK\n");
    return 0;
}

/* ── Self-observer (same process) ──────────────────────────────── */

static int test_self_observer(void) {
    printf("  self-observer...\n");

    size_t sz = 4096;
    mental_reference ref = mental_reference_create("SelfObs", sz);
    ASSERT(ref != NULL, "create returned NULL");

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "SelfObs");
    ASSERT(obs != NULL, "open own ref returned NULL");
    ASSERT(mental_reference_is_owner(obs) == 0, "observer should not be owner");
    ASSERT(mental_reference_size(obs) == sz, "observer size mismatch");

    /* Bidirectional data visibility through shared memory */
    void *owner_data = mental_reference_data(ref, NULL, 0);
    void *obs_data = mental_reference_data(obs, NULL, 0);
    ASSERT(owner_data != NULL && obs_data != NULL, "data pointers NULL");

    memset(owner_data, 0xAB, sz);
    ASSERT(((uint8_t *)obs_data)[0] == 0xAB, "observer can't see owner write");
    ASSERT(((uint8_t *)obs_data)[sz - 1] == 0xAB, "observer data mismatch at end");

    ((uint8_t *)obs_data)[0] = 0xCD;
    ASSERT(((uint8_t *)owner_data)[0] == 0xCD, "owner can't see observer write");

    mental_reference_close(obs);

    /* Owner data still valid after observer closes */
    ASSERT(((uint8_t *)owner_data)[1] == 0xAB, "owner data corrupted after observer close");

    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Cross-process via fork() ──────────────────────────────────── */

#ifndef _WIN32
static int test_cross_process(void) {
    printf("  cross-process (fork)...\n");

    size_t sz = 1024;
    mental_reference ref = mental_reference_create("CrossProc", sz);
    ASSERT(ref != NULL, "create returned NULL");

    /* Write a pattern the child will verify */
    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    ASSERT(data != NULL, "owner data NULL");
    for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)(i & 0xFF);

    const char *parent_uuid = mental_uuid();

    /* Shared flag: child writes 0xBEEF on success, parent reads it */
    int *shared_flag = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *shared_flag = 0;

    pid_t pid = fork();
    if (pid == 0) {
        /* ── Child process ────────────────────────────────────── */
        mental_reference child_obs = mental_reference_open(parent_uuid, "CrossProc");
        if (!child_obs) _exit(1);

        uint8_t *child_data = (uint8_t *)mental_reference_data(child_obs, NULL, 0);
        if (!child_data) { mental_reference_close(child_obs); _exit(2); }

        /* Verify parent's data is visible */
        for (size_t i = 0; i < sz; i++) {
            if (child_data[i] != (uint8_t)(i & 0xFF)) {
                mental_reference_close(child_obs);
                _exit(3);
            }
        }

        /* Write back a marker the parent will check */
        child_data[0] = 0x42;
        *shared_flag = 0xBEEF;

        mental_reference_close(child_obs);
        _exit(0);
    }

    /* ── Parent ───────────────────────────────────────────────── */
    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "child process failed (see exit code)");
    ASSERT(*shared_flag == 0xBEEF, "child did not signal success");
    ASSERT(data[0] == 0x42, "parent can't see child's write");

    munmap(shared_flag, sizeof(int));
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}
#endif

/* ── Disclosure: full matrix ───────────────────────────────────── */

static int test_disclosure_open(void) {
    printf("  disclosure (open)...\n");

    mental_reference ref = mental_reference_create("DisclOpen", 256);
    ASSERT(ref != NULL, "create failed");

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "DisclOpen");
    ASSERT(obs != NULL, "open failed");

    /* Default is OPEN */
    ASSERT(mental_reference_get_disclosure(ref) == MENTAL_RELATIONALLY_OPEN,
           "default should be OPEN");

    /* Observer reads without credential */
    ASSERT(mental_reference_data(obs, NULL, 0) != NULL,
           "open: observer should read without credential");

    /* Observer is writable without credential */
    ASSERT(mental_reference_writable(obs, NULL, 0) == 1,
           "open: observer should be writable without credential");

    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

static int test_disclosure_inclusive(void) {
    printf("  disclosure (inclusive)...\n");

    const uint8_t cred[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const uint8_t wrong[] = { 0xBA, 0xAD };

    mental_reference ref = mental_reference_create("DisclInc", 256);
    ASSERT(ref != NULL, "create failed");

    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_INCLUSIVE);
    mental_reference_set_credential(ref, cred, sizeof(cred));

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "DisclInc");
    ASSERT(obs != NULL, "open failed");

    /* Observer sees disclosure mode change via shared header */
    ASSERT(mental_reference_get_disclosure(obs) == MENTAL_RELATIONALLY_INCLUSIVE,
           "observer should see INCLUSIVE");

    /* Read access: always granted (no credential needed) */
    ASSERT(mental_reference_data(obs, NULL, 0) != NULL,
           "inclusive: read should work without credential");

    /* Write access: denied without credential */
    ASSERT(mental_reference_writable(obs, NULL, 0) == 0,
           "inclusive: write should be denied without credential");

    /* Write access: granted with correct credential */
    ASSERT(mental_reference_writable(obs, cred, sizeof(cred)) == 1,
           "inclusive: write should be granted with correct credential");

    /* Write access: denied with wrong credential */
    ASSERT(mental_reference_writable(obs, wrong, sizeof(wrong)) == 0,
           "inclusive: wrong credential should be denied");

    /* Partial credential (prefix of correct, wrong length) */
    ASSERT(mental_reference_writable(obs, cred, sizeof(cred) - 1) == 0,
           "inclusive: partial credential should be denied");

    /* Owner always writable regardless */
    ASSERT(mental_reference_writable(ref, NULL, 0) == 1,
           "owner should always be writable");

    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

static int test_disclosure_exclusive(void) {
    printf("  disclosure (exclusive)...\n");

    const uint8_t cred[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    const uint8_t wrong[] = { 0xFF, 0xFE };

    mental_reference ref = mental_reference_create("DisclExc", 256);
    ASSERT(ref != NULL, "create failed");

    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);
    mental_reference_set_credential(ref, cred, sizeof(cred));

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "DisclExc");
    ASSERT(obs != NULL, "open failed");

    /* Denied without credential */
    ASSERT(mental_reference_data(obs, NULL, 0) == NULL,
           "exclusive: should be denied without credential");
    ASSERT(mental_reference_writable(obs, NULL, 0) == 0,
           "exclusive: write should be denied without credential");

    /* Granted with correct credential */
    ASSERT(mental_reference_data(obs, cred, sizeof(cred)) != NULL,
           "exclusive: should be granted with correct credential");
    ASSERT(mental_reference_writable(obs, cred, sizeof(cred)) == 1,
           "exclusive: write should be granted with correct credential");

    /* Denied with wrong credential */
    ASSERT(mental_reference_data(obs, wrong, sizeof(wrong)) == NULL,
           "exclusive: wrong credential should be denied");

    /* Owner always has access */
    ASSERT(mental_reference_data(ref, NULL, 0) != NULL,
           "exclusive: owner should always have access");

    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Credential operations ─────────────────────────────────────── */

static int test_credential_lifecycle(void) {
    printf("  credential lifecycle...\n");

    const uint8_t cred1[] = { 0xAA, 0xBB };
    const uint8_t cred2[] = { 0xCC, 0xDD, 0xEE };

    mental_reference ref = mental_reference_create("CredLife", 256);
    ASSERT(ref != NULL, "create failed");
    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);
    mental_reference_set_credential(ref, cred1, sizeof(cred1));

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "CredLife");
    ASSERT(obs != NULL, "open failed");

    /* Initial credential works */
    ASSERT(mental_reference_data(obs, cred1, sizeof(cred1)) != NULL,
           "initial credential should work");

    /* Change credential on-the-fly */
    mental_reference_set_credential(ref, cred2, sizeof(cred2));

    /* Old credential rejected */
    ASSERT(mental_reference_data(obs, cred1, sizeof(cred1)) == NULL,
           "old credential should be rejected after change");

    /* New credential works */
    ASSERT(mental_reference_data(obs, cred2, sizeof(cred2)) != NULL,
           "new credential should work");

    /* Clear credential — nothing matches */
    mental_reference_set_credential(ref, NULL, 0);
    ASSERT(mental_reference_data(obs, cred2, sizeof(cred2)) == NULL,
           "cleared: old cred should not work");
    ASSERT(mental_reference_data(obs, NULL, 0) == NULL,
           "cleared: NULL should not work");

    /* Revert to OPEN restores access */
    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_OPEN);
    ASSERT(mental_reference_data(obs, NULL, 0) != NULL,
           "revert to OPEN should restore access");

    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Credential provider ───────────────────────────────────────── */

static uint8_t g_provider_cred[4] = { 0x10, 0x20, 0x30, 0x40 };

static void test_credential_provider_fn(void *ctx,
                                         void *buf, size_t buf_size,
                                         size_t *out_len) {
    (void)buf_size;
    int *call_count = (int *)ctx;
    (*call_count)++;
    memcpy(buf, g_provider_cred, sizeof(g_provider_cred));
    *out_len = sizeof(g_provider_cred);
}

static int test_credential_provider(void) {
    printf("  credential provider...\n");

    mental_reference ref = mental_reference_create("CredProv", 256);
    ASSERT(ref != NULL, "create failed");

    int call_count = 0;
    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);
    mental_reference_set_credential_provider(ref, test_credential_provider_fn, &call_count);

    ASSERT(call_count > 0, "provider should be called on registration");

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "CredProv");
    ASSERT(obs != NULL, "open failed");

    /* Observer with provider's credential gets access */
    ASSERT(mental_reference_data(obs, g_provider_cred, sizeof(g_provider_cred)) != NULL,
           "provider credential should grant access");

    /* Wrong credential still denied */
    const uint8_t wrong[] = { 0xFF };
    ASSERT(mental_reference_data(obs, wrong, sizeof(wrong)) == NULL,
           "wrong credential should be denied even with provider");

    /* Change provider credential dynamically */
    g_provider_cred[0] = 0x99;
    int count_before = call_count;

    /* Owner must access to trigger provider refresh into shared memory */
    void *owner_access = mental_reference_data(ref, NULL, 0);
    ASSERT(owner_access != NULL, "owner access should trigger provider refresh");
    ASSERT(call_count > count_before, "provider should be called on owner access");

    /* Now observer check with old value should fail (credential was refreshed) */
    const uint8_t old_cred[] = { 0x10, 0x20, 0x30, 0x40 };
    void *result = mental_reference_data(obs, old_cred, sizeof(old_cred));
    ASSERT(result == NULL, "old provider value should be rejected after change");

    /* Access with new value succeeds */
    ASSERT(mental_reference_data(obs, g_provider_cred, sizeof(g_provider_cred)) != NULL,
           "new provider credential should grant access");

    /* Restore for other tests */
    g_provider_cred[0] = 0x10;

    /* Clear provider */
    mental_reference_set_credential_provider(ref, NULL, NULL);

    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Observer cannot modify disclosure ─────────────────────────── */

static int test_observer_immutability(void) {
    printf("  observer immutability...\n");

    mental_reference ref = mental_reference_create("ObsImmut", 256);
    ASSERT(ref != NULL, "create failed");

    const char *uuid = mental_uuid();
    mental_reference obs = mental_reference_open(uuid, "ObsImmut");
    ASSERT(obs != NULL, "open failed");

    /* Observer tries to set EXCLUSIVE — should be no-op */
    mental_reference_set_disclosure(obs, MENTAL_RELATIONALLY_EXCLUSIVE);
    ASSERT(mental_reference_get_disclosure(ref) == MENTAL_RELATIONALLY_OPEN,
           "observer should not change disclosure");

    /* Observer tries to set credential — should be no-op */
    const uint8_t hack[] = { 0x66 };
    mental_reference_set_credential(obs, hack, sizeof(hack));

    /* Set real credential from owner, verify observer's attempt was ignored */
    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);
    const uint8_t real[] = { 0xAA };
    mental_reference_set_credential(ref, real, sizeof(real));

    ASSERT(mental_reference_data(obs, real, sizeof(real)) != NULL,
           "real credential should work");
    ASSERT(mental_reference_data(obs, hack, sizeof(hack)) == NULL,
           "observer's credential attempt should be ignored");

    mental_reference_close(obs);
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Cross-process disclosure ──────────────────────────────────── */

#ifndef _WIN32
static int test_cross_process_disclosure(void) {
    printf("  cross-process disclosure (fork)...\n");

    const uint8_t cred[] = { 0xCA, 0xFE, 0xBA, 0xBE };

    mental_reference ref = mental_reference_create("XProcDiscl", 512);
    ASSERT(ref != NULL, "create failed");

    /* Write marker data */
    uint8_t *data = (uint8_t *)mental_reference_data(ref, NULL, 0);
    memset(data, 0x77, 512);

    /* Set exclusive disclosure */
    mental_reference_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);
    mental_reference_set_credential(ref, cred, sizeof(cred));

    const char *parent_uuid = mental_uuid();

    /* Shared results: child writes exit codes */
    int *results = mmap(NULL, 4 * sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(results, 0, 4 * sizeof(int));

    pid_t pid = fork();
    if (pid == 0) {
        /* ── Child ─────────────────────────────────────────── */
        mental_reference child_obs = mental_reference_open(parent_uuid, "XProcDiscl");
        if (!child_obs) _exit(1);

        /* Without credential: data() should return NULL */
        void *p = mental_reference_data(child_obs, NULL, 0);
        results[0] = (p == NULL) ? 1 : 0;

        /* With correct credential: should succeed */
        p = mental_reference_data(child_obs, cred, sizeof(cred));
        results[1] = (p != NULL) ? 1 : 0;

        /* Verify data content */
        if (p) {
            results[2] = (((uint8_t *)p)[0] == 0x77) ? 1 : 0;
        }

        /* Wrong credential: should fail */
        const uint8_t wrong[] = { 0x00 };
        p = mental_reference_data(child_obs, wrong, sizeof(wrong));
        results[3] = (p == NULL) ? 1 : 0;

        mental_reference_close(child_obs);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exited abnormally");

    ASSERT(results[0] == 1, "child: without cred should get NULL");
    ASSERT(results[1] == 1, "child: with cred should get data");
    ASSERT(results[2] == 1, "child: data content should match");
    ASSERT(results[3] == 1, "child: wrong cred should get NULL");

    munmap(results, 4 * sizeof(int));
    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}
#endif

/* ── Reference write/read (shm only, no GPU) ──────────────────── */

static int test_write_read_shm(void) {
    printf("  write/read (shm only)...\n");

    size_t sz = 1024;
    mental_reference ref = mental_reference_create("WriteRead", sz);
    ASSERT(ref != NULL, "create failed");
    ASSERT(mental_reference_is_pinned(ref) == 0, "should not be pinned");

    /* Write data via mental_reference_write */
    float values[64];
    for (int i = 0; i < 64; i++) values[i] = (float)i * 1.5f;

    mental_reference_write(ref, values, sizeof(values));

    /* Read back via mental_reference_read */
    float readback[64];
    memset(readback, 0, sizeof(readback));
    mental_reference_read(ref, readback, sizeof(readback));

    for (int i = 0; i < 64; i++) {
        ASSERT(readback[i] == values[i], "readback mismatch (shm path)");
    }

    /* Also verify via direct data pointer */
    float *direct = (float *)mental_reference_data(ref, NULL, 0);
    ASSERT(direct != NULL, "data pointer NULL");
    for (int i = 0; i < 64; i++) {
        ASSERT(direct[i] == values[i], "direct pointer mismatch");
    }

    mental_reference_close(ref);

    printf("    OK\n");
    return 0;
}

/* ── Edge cases ────────────────────────────────────────────────── */

static int test_edge_cases(void) {
    printf("  edge cases...\n");

    /* NULL / empty args */
    ASSERT(mental_reference_create(NULL, 100) == NULL, "create(NULL) should fail");
    ASSERT(mental_reference_create("", 100) == NULL, "create(\"\") should fail");
    ASSERT(mental_reference_create("X", 0) == NULL, "create(size=0) should fail");
    ASSERT(mental_reference_open(NULL, "X") == NULL, "open(NULL uuid) should fail");
    ASSERT(mental_reference_open("abc", NULL) == NULL, "open(NULL name) should fail");

    /* Operations on NULL should not crash */
    ASSERT(mental_reference_data(NULL, NULL, 0) == NULL, "data(NULL) should return NULL");
    ASSERT(mental_reference_size(NULL) == 0, "size(NULL) should return 0");
    ASSERT(mental_reference_is_owner(NULL) == 0, "is_owner(NULL) should return 0");
    ASSERT(mental_reference_is_pinned(NULL) == 0, "is_pinned(NULL) should return 0");
    ASSERT(mental_reference_device(NULL) == NULL, "device(NULL) should return NULL");
    mental_reference_close(NULL);  /* should not crash */

    /* Nonexistent reference */
    mental_reference bad = mental_reference_open(
        "00000000000000000000000000000000", "NoSuchRef");
    ASSERT(bad == NULL, "open nonexistent should return NULL");

    /* Duplicate name */
    mental_reference ref = mental_reference_create("DupTest", 64);
    ASSERT(ref != NULL, "create DupTest failed");
    mental_reference dup = mental_reference_create("DupTest", 64);
    ASSERT(dup == NULL, "duplicate name should fail (O_EXCL)");
    mental_reference_close(ref);

    /* Re-create after close should succeed (name unlinked on close) */
    mental_reference reuse = mental_reference_create("DupTest", 128);
    ASSERT(reuse != NULL, "re-create after close should succeed");
    mental_reference_close(reuse);

    printf("    OK\n");
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
    printf("Testing reference...\n");

    test_uuid();
    test_lifecycle();
    test_self_observer();
    test_disclosure_open();
    test_disclosure_inclusive();
    test_disclosure_exclusive();
    test_credential_lifecycle();
    test_credential_provider();
    test_observer_immutability();
    test_write_read_shm();
    test_edge_cases();

#ifndef _WIN32
    test_cross_process();
    test_cross_process_disclosure();
#else
    printf("  (cross-process tests skipped on Windows)\n");
#endif

    if (g_failures > 0) {
        fprintf(stderr, "FAIL: %d test(s) failed\n", g_failures);
        return 1;
    }

    printf("PASS: reference\n");
    return 0;
}
