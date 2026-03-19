/* Test external compiler paths */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("Testing external compiler paths...\n");

    const char* dxc_paths[] = {
        "../external/dxc/bin/dxc",
        "external/dxc/bin/dxc",
        NULL
    };

    const char* naga_paths[] = {
        "../external/naga/bin/naga",
        "external/naga/bin/naga",
        NULL
    };

    printf("\nDXC paths:\n");
    for (int i = 0; dxc_paths[i]; i++) {
        int found = access(dxc_paths[i], X_OK) == 0;
        printf("  %s: %s\n", dxc_paths[i], found ? "FOUND" : "NOT FOUND");
    }

    printf("\nNaga paths:\n");
    for (int i = 0; naga_paths[i]; i++) {
        int found = access(naga_paths[i], X_OK) == 0;
        printf("  %s: %s\n", naga_paths[i], found ? "FOUND" : "NOT FOUND");
    }

    return 0;
}
