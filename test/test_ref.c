/* ref test: UUID generation, create/open/close shared memory refs */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while(0)

int main(void) {
    printf("Testing ref...\n");

    /* ── UUID ─────────────────────────────────────────────────── */

    const char *uuid = mental_uuid();
    printf("  uuid = %s\n", uuid);
    ASSERT(uuid != NULL, "mental_uuid() returned NULL");
    ASSERT(strlen(uuid) == 32, "UUID should be 32 hex chars");

    /* Idempotent */
    ASSERT(mental_uuid() == uuid, "mental_uuid() not idempotent (same pointer)");

    /* All hex chars */
    for (int i = 0; i < 32; i++) {
        char c = uuid[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        ASSERT(ok, "UUID contains non-hex character");
    }

    printf("  uuid: OK\n");

    /* ── Create ───────────────────────────────────────────────── */

    size_t sz = 4096;
    mental_ref ref = mental_ref_create("TestVelocity", sz);
    ASSERT(ref != NULL, "mental_ref_create returned NULL");

    void *data = mental_ref_data(ref);
    ASSERT(data != NULL, "mental_ref_data returned NULL");
    ASSERT(mental_ref_size(ref) == sz, "mental_ref_size mismatch");

    /* Write some data */
    memset(data, 0xAB, sz);

    printf("  create: OK\n");

    /* ── Open (self-observer) ─────────────────────────────────── */

    mental_ref obs = mental_ref_open(uuid, "TestVelocity");
    ASSERT(obs != NULL, "mental_ref_open returned NULL for own ref");

    void *obs_data = mental_ref_data(obs);
    ASSERT(obs_data != NULL, "observer mental_ref_data returned NULL");
    ASSERT(mental_ref_size(obs) == sz, "observer size mismatch");

    /* Verify data is visible */
    unsigned char *p = (unsigned char *)obs_data;
    ASSERT(p[0] == 0xAB, "observer cannot see owner's written data");
    ASSERT(p[sz - 1] == 0xAB, "observer data mismatch at end");

    /* Write from observer side, verify owner sees it */
    p[0] = 0xCD;
    unsigned char *owner_p = (unsigned char *)data;
    ASSERT(owner_p[0] == 0xCD, "owner cannot see observer's write");

    printf("  open (self-observer): OK\n");

    /* ── Close observer ───────────────────────────────────────── */

    mental_ref_close(obs);

    /* Owner data should still be valid */
    ASSERT(owner_p[1] == 0xAB, "owner data corrupted after observer close");

    printf("  close observer: OK\n");

    /* ── Graceful failure: open nonexistent ────────────────────── */

    mental_ref bad = mental_ref_open("00000000000000000000000000000000", "NoSuchRef");
    ASSERT(bad == NULL, "mental_ref_open should return NULL for nonexistent ref");

    printf("  graceful failure (nonexistent): OK\n");

    /* ── Graceful failure: NULL/empty args ─────────────────────── */

    ASSERT(mental_ref_create(NULL, 100) == NULL, "create(NULL) should fail");
    ASSERT(mental_ref_create("", 100) == NULL, "create(\"\") should fail");
    ASSERT(mental_ref_create("X", 0) == NULL, "create(size=0) should fail");
    ASSERT(mental_ref_open(NULL, "X") == NULL, "open(NULL uuid) should fail");
    ASSERT(mental_ref_open("abc", NULL) == NULL, "open(NULL name) should fail");

    /* mental_ref_data/size on NULL should not crash */
    ASSERT(mental_ref_data(NULL) == NULL, "ref_data(NULL) should return NULL");
    ASSERT(mental_ref_size(NULL) == 0, "ref_size(NULL) should return 0");

    /* mental_ref_close(NULL) should not crash */
    mental_ref_close(NULL);

    printf("  graceful failure (bad args): OK\n");

    /* ── Duplicate name should fail ────────────────────────────── */

    mental_ref dup = mental_ref_create("TestVelocity", 1024);
    ASSERT(dup == NULL, "creating duplicate name should fail (O_EXCL)");

    printf("  duplicate name rejected: OK\n");

    /* ── Close owner ──────────────────────────────────────────── */

    mental_ref_close(ref);

    /* After owner closes, open should fail gracefully */
    mental_ref gone = mental_ref_open(uuid, "TestVelocity");
    ASSERT(gone == NULL, "open after owner close should return NULL");

    printf("  close owner + re-open fails: OK\n");

    printf("PASS: ref\n");
    return 0;
}
