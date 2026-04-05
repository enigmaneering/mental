/*
 * Example 01: References and Disclosure
 *
 * Demonstrates the reference system:
 *   - Create a reference with data
 *   - Read and write through the data pointer
 *   - Set up disclosure (open, inclusive, exclusive)
 *   - Use a disclosure handle to modify access rules
 *   - Lock disclosure to freeze rules permanently
 *
 * Build:
 *   cc -o reference 01-reference.c -I.. -L../build -lmental-static -lc++ \
 *      -framework Metal -framework Foundation -framework QuartzCore \
 *      -framework AppKit -framework OpenCL
 *
 * Run:
 *   ./reference
 */

#include "mental.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    float x, y, z;
} Vec3;

int main(void) {
    printf("=== Reference Basics ===\n\n");

    /* Create an open reference */
    mental_reference ref = mental_reference_create(
        sizeof(Vec3), MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

    Vec3 *data = (Vec3 *)mental_reference_data(ref, NULL, 0);
    data->x = 1.0f;
    data->y = 2.0f;
    data->z = 3.0f;
    printf("Written: {%.1f, %.1f, %.1f}\n", data->x, data->y, data->z);

    /* Read back via mental_reference_read */
    Vec3 readback;
    mental_reference_read(ref, &readback, sizeof(Vec3));
    printf("Read:    {%.1f, %.1f, %.1f}\n", readback.x, readback.y, readback.z);

    mental_reference_close(ref);

    printf("\n=== Disclosure ===\n\n");

    /* Create an exclusive reference with a credential */
    const char *secret = "password123";
    mental_disclosure dh = NULL;
    ref = mental_reference_create(
        sizeof(Vec3), MENTAL_RELATIONALLY_EXCLUSIVE,
        secret, strlen(secret), &dh);

    /* Access without credential — denied */
    data = (Vec3 *)mental_reference_data(ref, NULL, 0);
    printf("Without credential: %s\n", data ? "GRANTED (wrong!)" : "DENIED (correct)");

    /* Access with wrong credential — denied */
    data = (Vec3 *)mental_reference_data(ref, "wrong", 5);
    printf("Wrong credential:   %s\n", data ? "GRANTED (wrong!)" : "DENIED (correct)");

    /* Access with correct credential — granted */
    data = (Vec3 *)mental_reference_data(ref, secret, strlen(secret));
    printf("Correct credential: %s\n", data ? "GRANTED (correct)" : "DENIED (wrong!)");

    /* Change to open via disclosure handle */
    mental_disclosure_set_mode(dh, MENTAL_RELATIONALLY_OPEN);
    data = (Vec3 *)mental_reference_data(ref, NULL, 0);
    printf("After set to open:  %s\n", data ? "GRANTED (correct)" : "DENIED (wrong!)");

    /* Lock the disclosure — rules are frozen */
    mental_disclosure_close(dh);
    printf("Disclosure locked.\n");

    mental_reference_close(ref);

    printf("\nDone.\n");
    return 0;
}
