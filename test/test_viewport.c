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

    const char* device_name = mental_device_name(dev);
    printf("  Testing on device: %s\n", device_name);

#ifdef __EMSCRIPTEN__
    /* WASM readback viewport: attach with NULL surface, present, read back pixels */
    printf("  Testing WASM readback viewport...\n");

    mental_viewport viewport = mental_viewport_attach(ref, NULL);
    ASSERT(viewport != NULL, "WASM viewport attach failed (NULL surface should be accepted)");
    ASSERT_NO_ERROR();

    /* Present: copies GPU buffer into internal framebuffer */
    mental_viewport_present(viewport);
    ASSERT_NO_ERROR();

    /* Read back the framebuffer */
    const void *pixels = NULL;
    size_t pixel_size = 0;
    int rc = mental_viewport_read(viewport, &pixels, &pixel_size);
    ASSERT(rc == 0, "mental_viewport_read failed");
    ASSERT(pixels != NULL, "Readback pixels should not be NULL");
    ASSERT(pixel_size == size, "Readback size should match reference size");

    /* Verify pixel data matches the test pattern we wrote */
    const unsigned char *px = (const unsigned char*)pixels;
    int mismatch = 0;
    for (size_t i = 0; i < size && i < pixel_size; i++) {
        if (px[i] != (unsigned char)(i % 256)) {
            fprintf(stderr, "  Pixel mismatch at byte %zu: expected %u, got %u\n",
                    i, (unsigned)(i % 256), (unsigned)px[i]);
            mismatch = 1;
            break;
        }
    }
    ASSERT(!mismatch, "Readback pixel data does not match expected pattern");
    printf("  Readback verified: %zu bytes match\n", pixel_size);

    mental_viewport_detach(viewport);
    ASSERT_NO_ERROR();
    printf("  WASM readback viewport: PASS\n");

#else
    /* Native: test error handling for NULL surface and unsupported backends */
    mental_viewport viewport = mental_viewport_attach(ref, NULL);
    if (viewport == NULL) {
        printf("  NULL surface correctly rejected\n");
        (void)mental_get_error(); /* Clear error */
    }

    if (strstr(device_name, "OpenCL") != NULL) {
        viewport = mental_viewport_attach(ref, (void*)0x1234);
        ASSERT(viewport == NULL, "OpenCL should not support viewports");
        ASSERT(mental_get_error() != MENTAL_SUCCESS, "Expected error for unsupported backend");
        printf("  OpenCL correctly reports no viewport support\n");
    } else {
        printf("  Native backend detected (viewport support available)\n");
        printf("  Note: Full viewport testing requires actual window/surface\n");
    }
#endif

    /* Cleanup */
    mental_reference_close(ref);


    printf("PASS: All viewport tests passed\n");
    return 0;
}
