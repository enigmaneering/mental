/* Buffer operations test: alloc, write, read, resize, clone */
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

    /* Test buffer allocation */
    size_t size = 1024;
    mental_reference ref = mental_alloc(dev, size);
    ASSERT(ref != NULL, "Failed to allocate buffer");
    ASSERT_NO_ERROR();

    /* Test buffer write */
    float test_data[256];
    for (int i = 0; i < 256; i++) {
        test_data[i] = (float)i * 1.5f;
    }
    mental_write(ref, test_data, sizeof(test_data));
    ASSERT_NO_ERROR();

    /* Test buffer read */
    float read_data[256];
    memset(read_data, 0, sizeof(read_data));
    mental_read(ref, read_data, sizeof(read_data));
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

    /* Test automatic reallocation on write */
    size_t original_size = mental_size(ref);
    ASSERT(original_size == size, "Size mismatch");

    /* Write larger data - should auto-resize */
    float large_data[512];
    for (int i = 0; i < 512; i++) {
        large_data[i] = (float)i * 2.5f;
    }
    mental_write(ref, large_data, sizeof(large_data));
    ASSERT_NO_ERROR();

    /* Verify size increased */
    size_t new_size = mental_size(ref);
    ASSERT(new_size >= sizeof(large_data), "Buffer did not auto-resize");

    /* Verify new data is correct */
    float verify_data[512];
    mental_read(ref, verify_data, sizeof(verify_data));
    ASSERT_NO_ERROR();

    match = 1;
    for (int i = 0; i < 512; i++) {
        if (verify_data[i] != large_data[i]) {
            match = 0;
            break;
        }
    }
    ASSERT(match, "Auto-resized buffer data incorrect");

    /* Test buffer clone (buffer now contains large_data from resize test) */
    mental_reference clone = mental_clone(ref);
    ASSERT(clone != NULL, "Failed to clone buffer");
    ASSERT_NO_ERROR();

    /* Verify clone has same data as current buffer contents */
    float clone_data[512];
    memset(clone_data, 0, sizeof(clone_data));
    mental_read(clone, clone_data, sizeof(clone_data));
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
    mental_finalize(clone);
    mental_finalize(ref);
    
    ASSERT_NO_ERROR();

    printf("PASS: All buffer tests passed\n");
    return 0;
}
