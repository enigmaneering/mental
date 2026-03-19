Won# Viewport Window Tests

## Overview

The Mental library includes real window tests that validate the viewport API by creating actual OS windows and presenting GPU buffers to them.

## Tests

### `test_viewport_window_metal.m` (macOS)
- Creates an NSWindow with CAMetalLayer
- Fills a buffer with orange color (RGB: 255, 165, 0)
- Attaches the buffer to the window via `mental_viewport_attach()`
- Presents the frame with `mental_viewport_present()`
- Displays the window for 1 second
- Validates no errors occurred

### `test_viewport_window_d3d12.c` (Windows)
- Creates a Win32 HWND window
- Fills a buffer with orange color
- Attaches via D3D12 swap chain
- Presents and displays for 1 second

### `test_viewport_window_vulkan.c` (Linux)
- Creates an X11 window
- Fills a buffer with orange color
- Creates VkSurfaceKHR from X11 window
- Presents via Vulkan swap chain
- Displays for 1 second

## Running Locally

### macOS
```bash
cd build
./test/test_viewport_window_metal
```
You should see an orange window appear for 1 second.

### Windows
```bash
cd build
./test/test_viewport_window_d3d12.exe
```

### Linux
```bash
# With X11 display
cd build
./test/test_viewport_window_vulkan

# Headless with Xvfb
xvfb-run ./test/test_viewport_window_vulkan
```

## CI/CD Testing

### GitHub Actions

The `.github/workflows/test.yml` workflow runs all tests on all platforms:

- **macOS**: Native window support, no special setup needed
- **Windows**: Native window support via Win32 API
- **Linux**: Uses `xvfb-action` to provide a virtual X11 display

### Key Features

1. **Xvfb on Linux**: The `GabrielBB/xvfb-action` provides a headless X server for window creation
2. **Platform Detection**: Tests automatically skip on non-matching platforms
3. **Visual Validation**: When run locally, you can see the orange window appear
4. **Error Validation**: Tests verify no errors occur during attach/present/detach

## Architecture

Each platform test follows the same pattern:

1. **Check backend** - Skip if not on the correct GPU API
2. **Create window** - Use platform-specific windowing API
3. **Fill buffer** - Create orange BGRA8 framebuffer
4. **Attach viewport** - Link GPU buffer to window surface
5. **Present** - Display the buffer (shows orange)
6. **Wait** - Display for 1 second (visible locally)
7. **Detach** - Clean up viewport
8. **Cleanup** - Destroy window and buffer

## Buffer Format

All tests use BGRA8 (8-bit per channel):
- **Blue**: 0
- **Green**: 165
- **Red**: 255
- **Alpha**: 255

This produces an orange color that's easy to visually verify.

## Why Orange?

Orange was chosen because:
1. It's distinct from common UI colors (black, white, gray)
2. It's not a primary color, so it validates all RGB channels
3. It's easy to see if something went wrong (blue/red channel swaps)
4. It's pleasant to look at during local testing!
