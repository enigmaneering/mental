/* Buffer operations test: create, pin, write, read, clone */
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

#define ASSERT_NO_ERROR() do { \
    if (mental_get_error() != MENTAL_SUCCESS) { \
        fprintf(stderr, "FAIL: %s\n", mental_get_error_message()); \
        return 1; \
    } \
} while(0)

int main(void) {
    printf("Testing buffer operations...\n");

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to create device");
    ASSERT_NO_ERROR();

    /* Test reference creation and pinning */
    mental_reference ref = mental_reference_create("buf-data", 2048);
    ASSERT(ref != NULL, "Failed to create reference");
    ASSERT_NO_ERROR();

    mental_reference_pin(ref, dev);
    ASSERT_NO_ERROR();
    ASSERT(mental_reference_is_pinned(ref), "reference should be pinned after pin");
    ASSERT(mental_reference_device(ref) == dev, "device should match");

    /* Test buffer write (256 floats = 1024 bytes into 2048-byte buffer) */
    float test_data[256];
    for (int i = 0; i < 256; i++) {
        test_data[i] = (float)i * 1.5f;
    }
    mental_reference_write(ref, test_data, sizeof(test_data));
    ASSERT_NO_ERROR();

    /* Test buffer read */
    float read_data[256];
    memset(read_data, 0, sizeof(read_data));
    mental_reference_read(ref, read_data, sizeof(read_data));
    ASSERT_NO_ERROR();

    /* Verify data */
    int match = 1;
    for (int i = 0; i < 256; i++) {
        if (read_data[i] != test_data[i]) {
            match = 0;
            break;
        }
    }
    ASSERT(match, "Read data does not match written data");

    /* Test writing full buffer (512 floats = 2048 bytes) */
    float large_data[512];
    for (int i = 0; i < 512; i++) {
        large_data[i] = (float)i * 2.5f;
    }
    mental_reference_write(ref, large_data, sizeof(large_data));
    ASSERT_NO_ERROR();

    /* Verify full buffer data is correct */
    float verify_data[512];
    mental_reference_read(ref, verify_data, sizeof(verify_data));
    ASSERT_NO_ERROR();

    match = 1;
    for (int i = 0; i < 512; i++) {
        if (verify_data[i] != large_data[i]) {
            match = 0;
            break;
        }
    }
    ASSERT(match, "Full buffer data incorrect");

    /* Test buffer clone (buffer now contains large_data) */
    mental_reference clone = mental_reference_clone(ref, "buf-clone", dev, NULL, 0);
    ASSERT(clone != NULL, "Failed to clone buffer");
    ASSERT_NO_ERROR();

    /* Verify clone has same data as current buffer contents */
    float clone_data[512];
    memset(clone_data, 0, sizeof(clone_data));
    mental_reference_read(clone, clone_data, sizeof(clone_data));
    ASSERT_NO_ERROR();

    match = 1;
    for (int i = 0; i < 512; i++) {
        if (clone_data[i] != large_data[i]) {
            match = 0;
            break;
        }
    }
    ASSERT(match, "Clone data does not match original");

    /* Cleanup */
    mental_reference_close(clone);
    mental_reference_close(ref);

    ASSERT_NO_ERROR();

    printf("PASS: All buffer tests passed\n");
    return 0;
}
