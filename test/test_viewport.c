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

    if (mental_device_count() == 0) {
        printf("SKIP: No GPU devices available\n");
        return 0;
    }
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to create device");
    ASSERT_NO_ERROR();

    /* Create buffer for viewport */
    size_t size = 1920 * 1080 * 4; /* RGBA8 1080p */
    mental_reference ref = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    ASSERT(ref != NULL, "Failed to create reference");
    ASSERT_NO_ERROR();

    mental_reference_pin(ref, dev);
    ASSERT_NO_ERROR();

    /* Fill with test pattern */
    unsigned char* pattern = malloc(size);
    for (size_t i = 0; i < size; i++) {
        pattern[i] = (unsigned char)(i % 256);
    }
    mental_reference_write(ref, pattern, size);
    free(pattern);
    ASSERT_NO_ERROR();

    /* Test viewport detach on NULL (defensive programming) */
    mental_viewport_detach(NULL);
    printf("  NULL viewport detach handled\n");

    /*
     * Note: We can't actually create a real window/surface in a headless test,
     * so we test error handling for NULL surface and unsupported backends.
     * Full viewport testing is done by the platform-specific test #16
     * (ViewportWindowMetal, ViewportWindowD3D12, ViewportWindowVulkan,
     * ViewportReadbackWasm).
     */

    const char* device_name = mental_device_name(dev);
    printf("  Testing on device: %s\n", device_name);

    /* Test NULL surface (should fail gracefully on native, succeed on WASM readback) */
    mental_viewport viewport = mental_viewport_attach(ref, NULL);
    if (viewport == NULL) {
        /* Expected on native backends that require a real surface */
        printf("  NULL surface correctly rejected\n");
        (void)mental_get_error(); /* Clear error */
    } else {
        /* Expected on WASM readback viewport (NULL surface is valid) */
        printf("  NULL surface accepted (readback viewport)\n");
        mental_viewport_detach(viewport);
    }

    if (strstr(device_name, "OpenCL") != NULL) {
        /* OpenCL backend should not support viewports */
        viewport = mental_viewport_attach(ref, (void*)0x1234);
        ASSERT(viewport == NULL, "OpenCL should not support viewports");
        ASSERT(mental_get_error() != MENTAL_SUCCESS, "Expected error for unsupported backend");
        printf("  OpenCL correctly reports no viewport support\n");
    }

    /* Cleanup */
    mental_reference_close(ref);


    printf("PASS: All viewport tests passed\n");
    return 0;
}
