/* Core operations test: device enumeration and API info */
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
    printf("Testing core operations...\n");

    /* Test device enumeration */
    int device_count = mental_device_count();
    printf("  Found %d device(s)\n", device_count);
    if (mental_device_count() == 0) {
        printf("SKIP: No GPU devices available\n");
        return 0;
    }
    ASSERT_NO_ERROR();

    /* Test device access and info */
    for (int i = 0; i < device_count && i < 3; i++) {
        mental_device dev = mental_device_get(i);
        ASSERT(dev != NULL, "Failed to get device");
        ASSERT_NO_ERROR();

        const char* name = mental_device_name(dev);
        printf("  Device %d: %s\n", i, name);
        ASSERT(name != NULL && strlen(name) > 0, "Device name is empty");

        const char* api_name = mental_device_api_name(dev);
        printf("    API: %s\n", api_name);
        ASSERT(api_name != NULL, "API name is NULL");
    }

    printf("PASS: All core tests passed\n");
    return 0;
}
