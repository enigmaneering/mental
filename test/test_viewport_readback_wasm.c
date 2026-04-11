/* Real viewport test for WASM (readback-based).
 * Matches the structure of ViewportWindowMetal, ViewportWindowD3D12,
 * and ViewportWindowVulkan — same buffer size, same orange fill,
 * same 11-second display duration, same lifecycle. */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Testing real viewport (WASM readback)...\n");

    /* Check if we're on WebGPU backend */
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "WebGPU") == NULL) {
        printf("SKIP: Not on WebGPU backend\n");
        return 0;
    }

    /* Create buffer for viewport (BGRA8, 800x600) */
    size_t width = 800;
    size_t height = 600;
    size_t size = width * height * 4; /* BGRA8 */

    mental_reference ref = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(ref, dev);
    ASSERT(ref != NULL, "Failed to allocate buffer");
    ASSERT_NO_ERROR();

    /* Fill buffer with orange (BGRA format: B=0, G=165, R=255, A=255) */
    unsigned char* orange_buffer = malloc(size);
    for (size_t i = 0; i < width * height; i++) {
        orange_buffer[i * 4 + 0] = 0;    /* B */
        orange_buffer[i * 4 + 1] = 165;  /* G */
        orange_buffer[i * 4 + 2] = 255;  /* R */
        orange_buffer[i * 4 + 3] = 255;  /* A */
    }
    mental_reference_write(ref, orange_buffer, size);
    ASSERT_NO_ERROR();

    printf("  Buffer filled with orange color\n");

    /* Attach viewport (NULL surface — WASM readback mode) */
    mental_viewport viewport = mental_viewport_attach(ref, NULL);
    ASSERT(viewport != NULL, "Failed to attach viewport");
    ASSERT_NO_ERROR();

    printf("  Viewport attached (readback mode)\n");

    /* Present to internal framebuffer */
    mental_viewport_present(viewport);
    ASSERT_NO_ERROR();

    printf("  Frame presented - readback framebuffer should contain orange\n");
    printf("  Displaying for 11 seconds...\n");

    /* Match native test behavior: present repeatedly for 11 seconds.
     * On native platforms this keeps a window visible and responsive.
     * On WASM this exercises the present path repeatedly, and a host
     * environment (browser) could blit each frame to a canvas. */
    for (int t = 0; t < 11; t++) {
        mental_viewport_present(viewport);
        usleep(1000000); /* 1 second */
    }

    /* Verify readback data matches what we wrote */
    const void *pixels = NULL;
    size_t pixel_size = 0;
    int rc = mental_viewport_read(viewport, &pixels, &pixel_size);
    ASSERT(rc == 0, "mental_viewport_read failed");
    ASSERT(pixels != NULL, "Readback pixels should not be NULL");
    ASSERT(pixel_size == size, "Readback size should match buffer size");

    const unsigned char *px = (const unsigned char *)pixels;
    int mismatches = 0;
    for (size_t i = 0; i < width * height; i++) {
        if (px[i * 4 + 0] != 0 || px[i * 4 + 1] != 165 ||
            px[i * 4 + 2] != 255 || px[i * 4 + 3] != 255) {
            if (mismatches < 5) {
                fprintf(stderr, "  Pixel %zu mismatch: got BGRA(%u,%u,%u,%u) expected (0,165,255,255)\n",
                        i, (unsigned)px[i*4], (unsigned)px[i*4+1],
                        (unsigned)px[i*4+2], (unsigned)px[i*4+3]);
            }
            mismatches++;
        }
    }
    ASSERT(mismatches == 0, "Readback pixel data does not match orange fill");
    printf("  Readback verified: all %zu pixels match orange\n", width * height);

    /* Detach viewport */
    mental_viewport_detach(viewport);
    printf("  Viewport detached\n");

    /* Cleanup */
    free(orange_buffer);
    mental_reference_close(ref);

    printf("PASS: Window viewport test successful\n");
    return 0;
}
