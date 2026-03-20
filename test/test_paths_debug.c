/* Debug path resolution for external compilers */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("Current working directory: %s\n", cwd);
    }

#ifdef _WIN32
    #define DXC_EXE "dxc.exe"
    #define NAGA_EXE "naga.exe"
#else
    #define DXC_EXE "dxc"
    #define NAGA_EXE "naga"
#endif

    printf("\nTesting DXC paths (looking for %s):\n", DXC_EXE);
    const char* dxc_paths[] = {
        "../external/dxc/" DXC_EXE,
        "external/dxc/" DXC_EXE,
        "../../external/dxc/" DXC_EXE,
        "../external/dxc/bin/" DXC_EXE,
        "external/dxc/bin/" DXC_EXE,
        "../../external/dxc/bin/" DXC_EXE,
        NULL
    };

    for (int i = 0; dxc_paths[i]; i++) {
        int result = access(dxc_paths[i], 0);  /* F_OK = file exists */
        printf("  %s: %s\n", dxc_paths[i], result == 0 ? "FOUND" : "NOT FOUND");
    }

    printf("\nTesting Naga paths (looking for %s):\n", NAGA_EXE);
    const char* naga_paths[] = {
        "../external/naga/bin/" NAGA_EXE,
        "external/naga/bin/" NAGA_EXE,
        "../../external/naga/bin/" NAGA_EXE,
        NULL
    };

    for (int i = 0; naga_paths[i]; i++) {
        int result = access(naga_paths[i], 0);  /* F_OK = file exists */
        printf("  %s: %s\n", naga_paths[i], result == 0 ? "FOUND" : "NOT FOUND");
    }

    return 0;
}
