/* Real window viewport test for Vulkan on Linux/X11 */
#if defined(__linux__) || defined(__unix__)

#include "../mental.h"
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

/* X11 surface structure to pass to Vulkan */
typedef struct {
    Display* display;
    Window window;
} X11Surface;

int main(void) {
    printf("Testing real window viewport (Vulkan/X11)...\n");

    /* Check if we're on Vulkan backend */
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "Vulkan") == NULL) {
        printf("SKIP: Not on Vulkan backend\n");
        return 0;
    }

    /* Open X11 display */
    Display* display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "FAIL: Cannot open X11 display (DISPLAY not set or no X server)\n");
        fprintf(stderr, "      If running headless, ensure Xvfb is running\n");
        return 1;
    }

    printf("  X11 display opened\n");

    /* Create window */
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    Window window = XCreateSimpleWindow(
        display, root,
        100, 100,  /* x, y */
        800, 600,  /* width, height */
        1,         /* border width */
        BlackPixel(display, screen),  /* border */
        BlackPixel(display, screen)   /* background */
    );

    /* Set window title */
    XStoreName(display, window, "Mental Viewport Test");

    /* Select events */
    XSelectInput(display, window, ExposureMask | KeyPressMask);

    /* Show window */
    XMapWindow(display, window);
    XFlush(display);

    printf("  Window created: 800x600\n");

    /* Wait for window to be mapped */
    XEvent event;
    do {
        XNextEvent(display, &event);
    } while (event.type != Expose);

    /* Create buffer for viewport (BGRA8, 800x600) */
    size_t width = 800;
    size_t height = 600;
    size_t size = width * height * 4; /* BGRA8 */

    mental_reference ref = mental_reference_create("vk-window-buf", size);
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

    /* Create surface structure for Vulkan */
    X11Surface surface;
    surface.display = display;
    surface.window = window;

    /* Attach viewport */
    mental_viewport viewport = mental_viewport_attach(ref, &surface);
    ASSERT(viewport != NULL, "Failed to attach viewport");
    ASSERT_NO_ERROR();

    printf("  Viewport attached to window\n");

    /* Present to screen */
    mental_viewport_present(viewport);
    ASSERT_NO_ERROR();

    printf("  Frame presented - window should show orange\n");
    printf("  Displaying for 11 seconds...\n");

    /* Display for 11 seconds and process events */
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);

    do {
        /* Process pending events */
        while (XPending(display) > 0) {
            XNextEvent(display, &event);
            if (event.type == Expose) {
                mental_viewport_present(viewport);
            }
        }

        usleep(10000); /* 10ms */
        clock_gettime(CLOCK_MONOTONIC, &current);
    } while ((current.tv_sec - start.tv_sec) +
             (current.tv_nsec - start.tv_nsec) / 1e9 < 11.0);

    /* Detach viewport */
    mental_viewport_detach(viewport);
    printf("  Viewport detached\n");

    /* Cleanup */
    mental_reference_close(ref);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    printf("PASS: Window viewport test successful\n");
    return 0;
}

#else

#include <stdio.h>
int main(void) {
    printf("SKIP: Vulkan/X11 window test only runs on Linux/Unix\n");
    return 0;
}

#endif /* __linux__ || __unix__ */
