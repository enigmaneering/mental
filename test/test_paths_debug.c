/* Debug path resolution for external compilers */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("Current working directory: %s\n", cwd);
    }

    printf("\nTesting DXC paths:\n");
    const char* dxc_paths[] = {
        "../external/dxc/bin/dxc",
        "external/dxc/bin/dxc",
        "../../external/dxc/bin/dxc",
        NULL
    };

    for (int i = 0; dxc_paths[i]; i++) {
        int result = access(dxc_paths[i], X_OK);
        printf("  %s: %s\n", dxc_paths[i], result == 0 ? "FOUND" : "NOT FOUND");
    }

    printf("\nTesting Naga paths:\n");
    const char* naga_paths[] = {
        "../external/naga/bin/naga",
        "external/naga/bin/naga",
        "../../external/naga/bin/naga",
        NULL
    };

    for (int i = 0; naga_paths[i]; i++) {
        int result = access(naga_paths[i], X_OK);
        printf("  %s: %s\n", naga_paths[i], result == 0 ? "FOUND" : "NOT FOUND");
    }

    return 0;
}
