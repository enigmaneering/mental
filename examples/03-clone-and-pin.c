/*
 * Example 03: Clone and Pin
 *
 * Demonstrates cloning a reference onto a GPU device:
 *   1. Create a CPU-only reference with data
 *   2. Clone it onto the GPU in one step
 *   3. Read back from the GPU clone
 *   4. Verify the clone is independent (modifying original doesn't affect clone)
 *
 * Build:
 *   cc -o clone-and-pin 03-clone-and-pin.c -I.. -L../build -lmental -lc++ \
 *      -framework Metal -framework Foundation -framework QuartzCore \
 *      -framework AppKit -framework OpenCL
 *
 * Run:
 *   ./clone-and-pin
 */

#include "mental.h"
#include <stdio.h>
#include <string.h>

#define N 8

int main(void) {
    int count = mental_device_count();
    if (count == 0) {
        printf("No GPU devices — running CPU-only clone test.\n\n");
    }

    mental_device dev = count > 0 ? mental_device_get(0) : NULL;
    if (dev) {
        printf("Device: %s\n\n", mental_device_name(dev));
    }

    /* Create a CPU-only reference */
    size_t size = N * sizeof(float);
    mental_reference original = mental_reference_create(
        size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

    float data[N] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    mental_reference_write(original, data, size);

    printf("Original (CPU): [");
    for (int i = 0; i < N; i++) printf("%.0f%s", data[i], i < N-1 ? ", " : "");
    printf("]\n");
    printf("  Pinned: %s\n\n", mental_reference_is_pinned(original) ? "yes" : "no");

    /* Clone onto GPU (or CPU if no device) */
    mental_reference clone = mental_reference_clone(original, dev, NULL, 0);
    if (!clone) {
        printf("Clone failed!\n");
        mental_reference_close(original);
        return 1;
    }

    printf("Clone created.\n");
    printf("  Pinned: %s\n", mental_reference_is_pinned(clone) ? "yes" : "no");

    /* Read back from clone */
    float readback[N];
    mental_reference_read(clone, readback, size);
    printf("  Data:   [");
    for (int i = 0; i < N; i++) printf("%.0f%s", readback[i], i < N-1 ? ", " : "");
    printf("]\n\n");

    /* Modify the original — clone should NOT change */
    data[0] = 999.0f;
    mental_reference_write(original, data, size);

    mental_reference_read(clone, readback, size);
    printf("After modifying original to 999:\n");
    printf("  Clone[0] = %.0f %s\n", readback[0],
           readback[0] == 1.0f ? "(independent ✓)" : "(corrupted ✗)");

    mental_reference_close(clone);
    mental_reference_close(original);

    printf("\nDone.\n");
    return 0;
}
