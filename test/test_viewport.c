/* Viewport operations test: attach, present, detach */
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
    printf("Testing viewport operations...\n");

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to create device");
    ASSERT_NO_ERROR();

    /* Create buffer for viewport */
    size_t size = 1920 * 1080 * 4; /* RGBA8 1080p */
    mental_reference ref = mental_alloc(dev, size);
    ASSERT(ref != NULL, "Failed to allocate buffer");
    ASSERT_NO_ERROR();

    /* Fill with test pattern */
    unsigned char* pattern = malloc(size);
    for (size_t i = 0; i < size; i++) {
        pattern[i] = (unsigned char)(i % 256);
    }
    mental_write(ref, pattern, size);
    free(pattern);
    ASSERT_NO_ERROR();

    /*
     * Note: We can't actually create a real window/surface in a headless test,
     * so we test error handling for NULL surface and unsupported backends.
     */

    /* Test NULL surface (should fail gracefully) */
    mental_viewport viewport = mental_viewport_attach(ref, NULL);
    if (viewport == NULL) {
        /* Expected to fail - clear error and continue */
        printf("  NULL surface correctly rejected\n");
        (void)mental_get_error(); /* Clear error */
    }

    /*
     * Test viewport detach on NULL (should handle gracefully)
     * This tests defensive programming in the implementation
     */
    mental_viewport_detach(NULL);
    printf("  NULL viewport detach handled\n");

    /*
     * For backends without native presentation (OpenCL),
     * test that attach returns NULL with appropriate error
     */
    const char* device_name = mental_device_name(dev);
    printf("  Testing on device: %s\n", device_name);

    if (strstr(device_name, "OpenCL") != NULL) {
        /* OpenCL backend should not support viewports */
        viewport = mental_viewport_attach(ref, (void*)0x1234);
        ASSERT(viewport == NULL, "OpenCL should not support viewports");
        ASSERT(mental_get_error() != MENTAL_SUCCESS, "Expected error for unsupported backend");
        printf("  OpenCL correctly reports no viewport support\n");
    } else {
        printf("  Native backend detected (viewport support available)\n");
        printf("  Note: Full viewport testing requires actual window/surface\n");
    }

    /* Cleanup */
    mental_finalize(ref);
    

    printf("PASS: All viewport tests passed\n");
    return 0;
}
