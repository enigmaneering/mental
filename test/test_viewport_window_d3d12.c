/* Real window viewport test for D3D12 on Windows */
#ifdef _WIN32

#include "../mental.h"
#include <windows.h>
#include <stdio.h>
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

/* Window procedure */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, NULL);
            return 0;
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

int main(void) {
    printf("Testing real window viewport (D3D12)...\n");

    /* Check if we're on D3D12 backend */
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "D3D12") == NULL && strstr(api_name, "Direct3D") == NULL) {
        printf("SKIP: Not on D3D12 backend\n");
        return 0;
    }

    /* Register window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MentalViewportTest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "FAIL: Failed to register window class\n");
        return 1;
    }

    /* Create window */
    HWND hwnd = CreateWindowExA(
        0,
        "MentalViewportTest",
        "Mental Viewport Test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600,
        NULL, NULL,
        GetModuleHandle(NULL),
        NULL
    );

    if (!hwnd) {
        fprintf(stderr, "FAIL: Failed to create window\n");
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    printf("  Window created: 800x600\n");

    /* Create buffer for viewport (BGRA8, 800x600) */
    size_t width = 800;
    size_t height = 600;
    size_t size = width * height * 4; /* BGRA8 */

    mental_reference ref = mental_reference_create("d3d12-window-buf", size);
    mental_reference_pin(ref, dev);
    ASSERT(ref != NULL, "Failed to allocate buffer");
    ASSERT_NO_ERROR();

    /* Fill buffer with orange (BGRA format: B=0, G=165, R=255, A=255) */
    unsigned char* orange_buffer = malloc(size);
    for (size_t i = 0; i < width * height; i++) {
        orange_buffer[i * 4 + 0] = 0;    /* B */
        orange_buffer[i * 4 + 1] = 165;  /* G */
        orange_buffer[i * 4 + 2] = 255;  /* R */
        orange_buffer[i * 4 + 3] = 255;  /* A */
    }
    mental_reference_write(ref, orange_buffer, size);
    free(orange_buffer);
    ASSERT_NO_ERROR();

    printf("  Buffer filled with orange color\n");

    /* Attach viewport */
    mental_viewport viewport = mental_viewport_attach(ref, hwnd);
    ASSERT(viewport != NULL, "Failed to attach viewport");
    ASSERT_NO_ERROR();

    printf("  Viewport attached to window\n");

    /* Present to screen */
    mental_viewport_present(viewport);
    ASSERT_NO_ERROR();

    printf("  Frame presented - window should show orange\n");
    printf("  Displaying for 11 seconds...\n");

    /* Process messages and sleep for 11 seconds */
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 11000) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }

    /* Detach viewport */
    mental_viewport_detach(viewport);
    printf("  Viewport detached\n");

    /* Cleanup */
    mental_reference_close(ref);
    DestroyWindow(hwnd);
    UnregisterClassA("MentalViewportTest", GetModuleHandle(NULL));

    printf("PASS: Window viewport test successful\n");
    return 0;
}

#else

#include <stdio.h>
int main(void) {
    printf("SKIP: D3D12 window test only runs on Windows\n");
    return 0;
}

#endif /* _WIN32 */
