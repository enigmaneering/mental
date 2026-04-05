/*
 * Example 04: Runtime State
 *
 * Queries and prints the complete runtime state:
 *   - Active backend
 *   - Available devices
 *   - Loaded libraries and their versions
 *
 * Build:
 *   cc -o state 04-state.c -I.. -L../build -lmental-static -lc++ \
 *      -framework Metal -framework Foundation -framework QuartzCore \
 *      -framework AppKit -framework OpenCL
 *
 * Run:
 *   ./state
 */

#include "mental.h"
#include <stdio.h>

int main(void) {
    mental_state *s = mental_state_get();
    if (!s) {
        printf("Failed to get state.\n");
        return 1;
    }

    printf("Mental State\n");
    printf("============\n\n");

    printf("  Backend: %s\n\n", s->active_backend_name ? s->active_backend_name : "none");

    printf("  Devices (%d):\n", s->device_count);
    for (int i = 0; i < s->device_count; i++) {
        mental_device dev = s->devices[i];
        printf("    [%d] %s (%s)\n", i,
               mental_device_name(dev),
               mental_device_api_name(dev));
    }
    if (s->device_count == 0) {
        printf("    (none)\n");
    }

    printf("\n  Libraries (%d):\n", s->library_count);
    for (int i = 0; i < s->library_count; i++) {
        const char *status = s->libraries[i].available ? "+" : "x";
        const char *ver = s->libraries[i].version;
        if (ver) {
            printf("    [%s] %s %s\n", status, s->libraries[i].name, ver);
        } else {
            printf("    [%s] %s\n", status, s->libraries[i].name);
        }
    }

    mental_state_free(s);
    printf("\nDone.\n");
    return 0;
}
