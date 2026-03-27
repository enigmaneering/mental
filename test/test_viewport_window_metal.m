/* Real window viewport test for Metal on macOS */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "../mental.h"
#include <stdio.h>
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
    printf("Testing real window viewport (Metal)...\n");

    /* Check if we're on Metal backend */
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "Metal") == NULL) {
        printf("SKIP: Not on Metal backend\n");
        return 0;
    }

    @autoreleasepool {
        /* Initialize NSApplication (required for window creation) */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* Create window */
        NSRect frame = NSMakeRect(100, 100, 800, 600);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:(NSWindowStyleMaskTitled |
                                                       NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable)
                                             backing:NSBackingStoreBuffered
                                             defer:NO];
        [window setTitle:@"Mental Viewport Test"];
        [window setLevel:NSFloatingWindowLevel]; /* Keep window on top */
        [window setBackgroundColor:[NSColor orangeColor]]; /* Fallback orange background */

        /* Create Metal layer */
        CAMetalLayer* metalLayer = [CAMetalLayer layer];
        metalLayer.bounds = NSMakeRect(0, 0, 800, 600);
        metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        metalLayer.device = MTLCreateSystemDefaultDevice();
        metalLayer.opaque = YES;

        /* Set layer as window's content view layer */
        NSView* contentView = [window contentView];
        [contentView setWantsLayer:YES];
        [contentView setLayer:metalLayer];
        metalLayer.frame = [contentView bounds];

        /* Show window and bring to front */
        [window makeKeyAndOrderFront:nil];
        [window orderFrontRegardless];
        [NSApp activateIgnoringOtherApps:YES];

        /* Force window to appear */
        [window display];
        [window setIsVisible:YES];

        printf("  Window created: 800x600\n");

        /* Create buffer for viewport (BGRA8, 800x600) */
        size_t width = 800;
        size_t height = 600;
        size_t size = width * height * 4; /* BGRA8 */

        mental_reference ref = mental_reference_create("metal-window-buf", size);
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
        free(orange_buffer);
        ASSERT_NO_ERROR();

        printf("  Buffer filled with orange color\n");

        /* Attach viewport */
        mental_viewport viewport = mental_viewport_attach(ref, (__bridge void*)metalLayer);
        ASSERT(viewport != NULL, "Failed to attach viewport");
        ASSERT_NO_ERROR();

        printf("  Viewport attached to window\n");

        /* Present to screen */
        mental_viewport_present(viewport);
        ASSERT_NO_ERROR();

        printf("  Frame presented - window should show orange\n");
        printf("  Displaying for 11 seconds...\n");

        /* Run event loop for 11 seconds to keep window visible and responsive */
        NSDate* endTime = [NSDate dateWithTimeIntervalSinceNow:11.0];
        while ([endTime timeIntervalSinceNow] > 0) {
            /* Process all pending events */
            NSEvent* event;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                  untilDate:[NSDate distantPast]
                                  inMode:NSDefaultRunLoopMode
                                  dequeue:YES])) {
                [NSApp sendEvent:event];
                [NSApp updateWindows];
            }

            /* Small sleep to avoid spinning */
            usleep(10000); /* 10ms */
        }

        /* Detach viewport */
        mental_viewport_detach(viewport);
        printf("  Viewport detached\n");

        /* Cleanup */
        mental_reference_close(ref);
        [window close];

        printf("PASS: Window viewport test successful\n");
    }

    return 0;
}
