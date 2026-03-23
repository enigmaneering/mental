/* Debug tool path resolution */
#include <stdio.h>
#include "../transpile.h"

int main(void) {
    const char* dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    const char* naga = mental_get_tool_path(MENTAL_TOOL_NAGA);

    printf("DXC:  %s\n", dxc  ? dxc  : "(not configured)");
    printf("Naga: %s\n", naga ? naga : "(not configured)");

    return 0;
}
