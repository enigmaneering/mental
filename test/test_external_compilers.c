/* Test external compiler availability via tool path API */
#include <stdio.h>
#include "../transpile.h"

int main(void) {
    printf("External compiler status:\n");

    const char* dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    printf("  DXC:  %s\n", dxc  ? dxc  : "NOT CONFIGURED");

    const char* naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    printf("  Naga: %s\n", naga ? naga : "NOT CONFIGURED");

    return 0;
}
