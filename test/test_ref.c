/* ref test: UUID, create/open/close, and disclosure access control */
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

    void *data = mental_ref_data(ref, NULL, 0);
    ASSERT(data != NULL, "mental_ref_data returned NULL");
    ASSERT(mental_ref_size(ref) == sz, "mental_ref_size mismatch");

    /* Write some data */
    memset(data, 0xAB, sz);

    printf("  create: OK\n");

    /* ── Default disclosure is OPEN ───────────────────────────── */

    ASSERT(mental_ref_get_disclosure(ref) == MENTAL_RELATIONALLY_OPEN,
           "default disclosure should be OPEN");

    printf("  default disclosure (open): OK\n");

    /* ── Open (self-observer) ─────────────────────────────────── */

    mental_ref obs = mental_ref_open(uuid, "TestVelocity");
    ASSERT(obs != NULL, "mental_ref_open returned NULL for own ref");

    void *obs_data = mental_ref_data(obs, NULL, 0);
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

    /* Reset for later tests */
    owner_p[0] = 0xAB;

    printf("  open (self-observer, open mode): OK\n");

    /* ── Disclosure: INCLUSIVE with byte credential ────────────── */

    /* Use a raw byte credential (not a string — could be a hash) */
    const uint8_t cred[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x00, 0xFF };
    const uint8_t wrong[] = { 0xBA, 0xAD, 0xF0, 0x0D };

    mental_ref_set_disclosure(ref, MENTAL_RELATIONALLY_INCLUSIVE);
    mental_ref_set_credential(ref, cred, sizeof(cred));

    ASSERT(mental_ref_get_disclosure(ref) == MENTAL_RELATIONALLY_INCLUSIVE,
           "disclosure should be INCLUSIVE");

    /* Observer also sees the mode change (same shm) */
    ASSERT(mental_ref_get_disclosure(obs) == MENTAL_RELATIONALLY_INCLUSIVE,
           "observer should see INCLUSIVE via shared header");

    /* Observer can still read without credential */
    void *inc_data = mental_ref_data(obs, NULL, 0);
    ASSERT(inc_data != NULL, "inclusive: observer should get read access without credential");

    /* Observer is NOT writable without credential */
    ASSERT(mental_ref_writable(obs, NULL, 0) == 0,
           "inclusive: observer should NOT be writable without credential");

    /* Observer IS writable with correct credential */
    ASSERT(mental_ref_writable(obs, cred, sizeof(cred)) == 1,
           "inclusive: observer should be writable with correct credential");

    /* Wrong credential: not writable */
    ASSERT(mental_ref_writable(obs, wrong, sizeof(wrong)) == 0,
           "inclusive: wrong credential should not grant write access");

    /* Partial credential (right prefix, wrong length): not writable */
    ASSERT(mental_ref_writable(obs, cred, sizeof(cred) - 1) == 0,
           "inclusive: partial credential should not match");

    /* Owner always writable */
    ASSERT(mental_ref_writable(ref, NULL, 0) == 1,
           "owner should always be writable");

    printf("  disclosure (inclusive, byte credential): OK\n");

    /* ── Disclosure: EXCLUSIVE ────────────────────────────────── */

    mental_ref_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);

    /* Observer gets NULL without credential */
    void *exc_data = mental_ref_data(obs, NULL, 0);
    ASSERT(exc_data == NULL, "exclusive: observer should get NULL without credential");

    /* Observer gets data with correct credential */
    exc_data = mental_ref_data(obs, cred, sizeof(cred));
    ASSERT(exc_data != NULL, "exclusive: observer should get data with correct credential");

    /* Wrong credential: denied */
    ASSERT(mental_ref_data(obs, wrong, sizeof(wrong)) == NULL,
           "exclusive: wrong credential should return NULL");

    /* Owner always has access */
    ASSERT(mental_ref_data(ref, NULL, 0) != NULL,
           "exclusive: owner should always have access");

    printf("  disclosure (exclusive): OK\n");

    /* ── Owner can change credential on-the-fly ───────────────── */

    const uint8_t new_cred[] = { 0x01, 0x02, 0x03 };
    mental_ref_set_credential(ref, new_cred, sizeof(new_cred));

    /* Old credential no longer works */
    ASSERT(mental_ref_data(obs, cred, sizeof(cred)) == NULL,
           "old credential should be rejected after change");

    /* New credential works */
    ASSERT(mental_ref_data(obs, new_cred, sizeof(new_cred)) != NULL,
           "new credential should grant access");

    printf("  credential change on-the-fly: OK\n");

    /* ── Owner can clear credential ───────────────────────────── */

    mental_ref_set_credential(ref, NULL, 0);

    /* With no credential set, nothing matches — stays locked */
    ASSERT(mental_ref_data(obs, new_cred, sizeof(new_cred)) == NULL,
           "cleared credential: old cred should not work");
    ASSERT(mental_ref_data(obs, "", 0) == NULL,
           "cleared credential: empty cred should not work");

    printf("  credential clear: OK\n");

    /* ── Owner can revert to OPEN ─────────────────────────────── */

    mental_ref_set_disclosure(ref, MENTAL_RELATIONALLY_OPEN);
    ASSERT(mental_ref_data(obs, NULL, 0) != NULL,
           "reverting to OPEN should restore access without credential");

    printf("  revert to open: OK\n");

    /* ── Observer cannot change disclosure ─────────────────────── */

    mental_ref_set_disclosure(obs, MENTAL_RELATIONALLY_EXCLUSIVE);
    ASSERT(mental_ref_get_disclosure(obs) == MENTAL_RELATIONALLY_OPEN,
           "observer should not be able to change disclosure");

    const uint8_t hacked[] = { 0x66 };
    mental_ref_set_credential(obs, hacked, sizeof(hacked));
    mental_ref_set_disclosure(ref, MENTAL_RELATIONALLY_EXCLUSIVE);

    /* Credential should still be cleared (from owner's clear above),
     * not the observer's attempted "hacked" credential */
    mental_ref_set_credential(ref, cred, sizeof(cred));
    ASSERT(mental_ref_data(obs, cred, sizeof(cred)) != NULL,
           "observer should not be able to change credential");
    ASSERT(mental_ref_data(obs, hacked, sizeof(hacked)) == NULL,
           "observer's credential attempt should be ignored");

    /* Revert for cleanup */
    mental_ref_set_disclosure(ref, MENTAL_RELATIONALLY_OPEN);

    printf("  observer cannot modify disclosure: OK\n");

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
    ASSERT(mental_ref_data(NULL, NULL, 0) == NULL, "ref_data(NULL) should return NULL");
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
