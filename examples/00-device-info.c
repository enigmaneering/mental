/*
 * Example 00: Device Information
 *
 * Enumerates all available GPU devices and prints their names
 * and backend APIs.  This is the simplest possible libmental
 * program — it just asks "what hardware do I have?"
 *
 * Build:
 *   cc -o device-info 00-device-info.c -I.. -L../build -lmental-static -lc++ \
 *      -framework Metal -framework Foundation -framework QuartzCore \
 *      -framework AppKit -framework OpenCL
 *
 * Run:
 *   ./device-info
 */

#include "mental.h"
#include <stdio.h>

int main(void) {
    int count = mental_device_count();
    printf("Found %d device(s):\n\n", count);

    for (int i = 0; i < count; i++) {
        mental_device dev = mental_device_get(i);
        printf("  [%d] %s\n", i, mental_device_name(dev));
        printf("      API: %s\n\n", mental_device_api_name(dev));
    }

    if (count == 0) {
        printf("  No GPU devices found.\n");
        printf("  Error: %s\n", mental_get_error_message());
    }

    return 0;
}
