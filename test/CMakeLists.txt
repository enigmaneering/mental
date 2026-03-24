# Test suite for mental library
enable_testing()

# Test executables (link against static library for simpler test builds)
add_executable(test_core test_core.c)
target_link_libraries(test_core mental-static)

add_executable(test_buffer test_buffer.c)
target_link_libraries(test_buffer mental-static)

add_executable(test_kernel test_kernel.c)
target_link_libraries(test_kernel mental-static)

add_executable(test_viewport test_viewport.c)
target_link_libraries(test_viewport mental-static)

add_executable(test_integration test_integration.c)
target_link_libraries(test_integration mental-static)

add_executable(test_shaders test_shaders.c)
target_link_libraries(test_shaders mental-static)

# Register tests with CTest
add_test(NAME CoreOperations COMMAND test_core)
add_test(NAME BufferOperations COMMAND test_buffer)
add_test(NAME KernelOperations COMMAND test_kernel)
add_test(NAME ViewportOperations COMMAND test_viewport)
add_test(NAME MultiLanguageShaders COMMAND test_shaders)
add_test(NAME Integration COMMAND test_integration)

# Platform-specific native backend tests (always built, skip at runtime if backend unavailable)
add_executable(test_metal_direct test_metal_direct.c)
target_link_libraries(test_metal_direct mental-static)
add_test(NAME MetalDirect COMMAND test_metal_direct)

add_executable(test_d3d12_direct test_d3d12_direct.c)
target_link_libraries(test_d3d12_direct mental-static)
add_test(NAME D3D12Direct COMMAND test_d3d12_direct)

add_executable(test_vulkan_direct test_vulkan_direct.c)
target_link_libraries(test_vulkan_direct mental-static)
add_test(NAME VulkanDirect COMMAND test_vulkan_direct)

add_executable(test_opencl_direct test_opencl_direct.c)
target_link_libraries(test_opencl_direct mental-static)
add_test(NAME OpenCLDirect COMMAND test_opencl_direct)

add_executable(test_transpile_debug test_transpile_debug.c)
target_link_libraries(test_transpile_debug mental-static)

add_executable(test_msl_debug test_msl_debug.c)
target_link_libraries(test_msl_debug mental-static)

add_executable(test_paths_debug test_paths_debug.c)
target_link_libraries(test_paths_debug mental-static)
add_test(NAME PathDebug COMMAND test_paths_debug)

add_executable(test_stdlink test_stdlink.c)
target_link_libraries(test_stdlink mental-static)
add_test(NAME Stdlink COMMAND test_stdlink)

add_executable(test_monotonic test_monotonic.c)
target_link_libraries(test_monotonic mental-static)
add_test(NAME Monotonic COMMAND test_monotonic)

add_executable(test_hlsl_debug test_hlsl_debug.c)
target_link_libraries(test_hlsl_debug mental-static)

# Real window viewport tests (platform-specific)
if(APPLE)
    add_executable(test_viewport_window_metal test_viewport_window_metal.m)
    target_link_libraries(test_viewport_window_metal mental-static)
    target_link_libraries(test_viewport_window_metal "-framework Cocoa" "-framework QuartzCore" "-framework Metal")
    add_test(NAME ViewportWindowMetal COMMAND test_viewport_window_metal)
endif()

if(WIN32)
    add_executable(test_viewport_window_d3d12 test_viewport_window_d3d12.c)
    target_link_libraries(test_viewport_window_d3d12 mental-static)
    add_test(NAME ViewportWindowD3D12 COMMAND test_viewport_window_d3d12)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND)
        add_executable(test_viewport_window_vulkan test_viewport_window_vulkan.c)
        target_link_libraries(test_viewport_window_vulkan mental-static ${X11_LIBRARIES})
        target_include_directories(test_viewport_window_vulkan PRIVATE ${X11_INCLUDE_DIR})
        add_test(NAME ViewportWindowVulkan COMMAND test_viewport_window_vulkan)
    else()
        message(STATUS "X11 not found - skipping Vulkan window test")
    endif()
endif()
