cmake_minimum_required(VERSION 3.15)
project(mental VERSION 0.0.42 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# External redistributables directory
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

# Platform detection
if(APPLE)
    set(PLATFORM_DARWIN ON)
    enable_language(OBJC)
    enable_language(OBJCXX)
elseif(WIN32)
    set(PLATFORM_WINDOWS ON)
elseif(UNIX)
    set(PLATFORM_LINUX ON)
endif()

# Source files
set(MENTAL_SOURCES
    mental.c
    reference.c
    transpile.c
    transpile_other.c
    transpile_opencl.c
    sanity.c
    backend_opencl.c
    backend_opengl.c
    backend_pocl.c
    backend_webgpu.c
)

set(MENTAL_CXX_SOURCES
    transpile_glslang.cpp
    transpile_spirv_cross.cpp
    backend_vulkan.cpp
    backend_d3d12.cpp
    backend_d3d11.cpp
)

# Backend disable switches (for CI fallback-chain testing)
option(MENTAL_DISABLE_METAL  "Force-disable Metal backend"  OFF)
option(MENTAL_DISABLE_D3D12  "Force-disable D3D12 backend"  OFF)
option(MENTAL_DISABLE_D3D11  "Force-disable D3D11 backend"  OFF)
option(MENTAL_DISABLE_VULKAN "Force-disable Vulkan backend" OFF)
option(MENTAL_DISABLE_OPENCL "Force-disable OpenCL backend" OFF)
option(MENTAL_DISABLE_OPENGL "Force-disable OpenGL backend" OFF)
option(MENTAL_DISABLE_POCL   "Force-disable PoCL backend"   OFF)
option(MENTAL_DISABLE_WEBGPU "Force-disable WebGPU backend" OFF)

# WebGPU backend (all platforms — runtime-loaded via dlopen)
# Only needs headers at build time; wgpu-native library is loaded dynamically.
if(MENTAL_DISABLE_WEBGPU)
    message(STATUS "WebGPU backend force-disabled")
else()
    find_path(WGPU_HEADER "webgpu/webgpu.h" HINTS ${EXTERNAL_DIR}/wgpu/include)
    if(WGPU_HEADER)
        set(MENTAL_HAS_WEBGPU ON)
        message(STATUS "WebGPU headers found - enabling WebGPU backend (wgpu-native, runtime-loaded)")
    else()
        message(STATUS "WebGPU headers not found - WebGPU backend disabled")
    endif()
endif()

# Platform-specific backends (all optional — library degrades gracefully)
if(PLATFORM_DARWIN)
    # Metal is always available on macOS (part of the base OS)
    if(NOT MENTAL_DISABLE_METAL)
        list(APPEND MENTAL_SOURCES backend_metal.m)
        set_source_files_properties(backend_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        message(STATUS "Metal backend enabled")
    else()
        message(STATUS "Metal backend force-disabled")
    endif()
elseif(PLATFORM_WINDOWS)
    # DXGI is shared by D3D12 and D3D11 — find it once
    find_library(DXGI_LIBRARY dxgi)

    if(NOT MENTAL_DISABLE_D3D12)
        find_library(D3D12_LIBRARY d3d12)
        if(D3D12_LIBRARY AND DXGI_LIBRARY)
            list(APPEND MENTAL_CXX_SOURCES backend_d3d12.cpp)
            set(MENTAL_HAS_D3D12 ON)
            message(STATUS "D3D12 found - enabling D3D12 backend")
        else()
            message(STATUS "D3D12 not found - D3D12 backend disabled")
        endif()
    else()
        message(STATUS "D3D12 backend force-disabled")
    endif()
    if(NOT MENTAL_DISABLE_VULKAN)
        find_package(Vulkan)
        list(APPEND MENTAL_CXX_SOURCES backend_vulkan.cpp)
        if(Vulkan_FOUND)
            set(MENTAL_HAS_VULKAN_HEADERS ON)
            message(STATUS "Vulkan headers found - Vulkan backend enabled (runtime-loaded via dlopen)")
        else()
            message(STATUS "Vulkan headers not found - Vulkan backend will still compile but needs headers at build time")
        endif()
    else()
        message(STATUS "Vulkan backend force-disabled")
    endif()
    if(NOT MENTAL_DISABLE_D3D11)
        find_library(D3D11_LIBRARY d3d11)
        find_library(D3DCOMPILER_LIBRARY d3dcompiler)
        if(D3D11_LIBRARY AND D3DCOMPILER_LIBRARY AND DXGI_LIBRARY)
            list(APPEND MENTAL_CXX_SOURCES backend_d3d11.cpp)
            set(MENTAL_HAS_D3D11 ON)
            message(STATUS "D3D11 found - enabling D3D11 compute backend (Shader Model 5.0)")
        else()
            message(STATUS "D3D11 not found - D3D11 backend disabled")
        endif()
    else()
        message(STATUS "D3D11 backend force-disabled")
    endif()
elseif(PLATFORM_LINUX)
    if(NOT MENTAL_DISABLE_VULKAN)
        find_package(Vulkan)
        list(APPEND MENTAL_CXX_SOURCES backend_vulkan.cpp)
        if(Vulkan_FOUND)
            set(MENTAL_HAS_VULKAN_HEADERS ON)
            message(STATUS "Vulkan headers found - Vulkan backend enabled (runtime-loaded via dlopen)")
        else()
            message(STATUS "Vulkan headers not found - Vulkan backend will still compile but needs headers at build time")
        endif()
    else()
        message(STATUS "Vulkan backend force-disabled")
    endif()
endif()

# OpenGL compute backend — always compiled, dlopen'd at runtime.
# On macOS the file exports opengl_backend = NULL (Apple capped GL at 4.1).
# OpenGL backend is always compiled (dlopen'd at runtime, exports NULL on macOS)

# PoCL — CPU-only OpenCL last resort (all platforms, dlopen'd at runtime).
# Only needs OpenCL headers at build time; the library is loaded dynamically.
# Check for headers via OpenCL package or a manual header search.
if(MENTAL_DISABLE_POCL)
    message(STATUS "PoCL backend force-disabled")
elseif(OpenCL_FOUND OR OPENCL_FOUND)
    set(MENTAL_POCL_HEADERS ON)
elseif(PLATFORM_DARWIN AND OPENCL_LIBRARY)
    set(MENTAL_POCL_HEADERS ON)
else()
    find_path(POCL_CL_HEADER "CL/cl.h" HINTS ${EXTERNAL_DIR}/pocl/include)
    if(POCL_CL_HEADER)
        set(MENTAL_POCL_HEADERS ON)
    endif()
endif()

if(MENTAL_POCL_HEADERS)
    set(MENTAL_HAS_POCL ON)
    message(STATUS "OpenCL headers available - enabling PoCL fallback backend (dlopen'd at runtime)")
    if(EXISTS "${EXTERNAL_DIR}/pocl")
        set(MENTAL_POCL_LIB_PATH "${EXTERNAL_DIR}/pocl/${POCL_LIB_NAME}" CACHE STRING "Bundled PoCL library path")
    endif()
else()
    message(STATUS "OpenCL headers not found - PoCL backend disabled")
endif()

# Static library (all backends dlopen'd at runtime — no shared library needed)
add_library(mental STATIC
    ${MENTAL_SOURCES}
    ${MENTAL_CXX_SOURCES}
)

target_include_directories(mental PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXTERNAL_DIR}/glslang
    ${EXTERNAL_DIR}/glslang/SPIRV
    ${EXTERNAL_DIR}/spirv-cross
    ${EXTERNAL_DIR}/dxc/include
)

# Platform-specific configurations
if(PLATFORM_DARWIN)
    # Metal framework (already discovered above)
    if(MENTAL_HAS_METAL)
        find_library(FOUNDATION_LIBRARY Foundation)
        find_library(QUARTZCORE_LIBRARY QuartzCore)
        find_library(APPKIT_LIBRARY AppKit)
        # Metal is always compiled on macOS (link-time framework deps are base OS)
    endif()

    # OpenCL is now dlopen'd at runtime — no link-time dependency needed

    # PoCL (already discovered above)
    if(MENTAL_HAS_POCL)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_POCL=1)
        if(MENTAL_POCL_LIB_PATH)
            target_compile_definitions(mental PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
        endif()
    endif()

    if(NOT MENTAL_HAS_METAL AND NOT OPENCL_LIBRARY AND NOT MENTAL_HAS_POCL)
        message(WARNING "No GPU backend available on macOS - library will report MENTAL_ERROR_NO_DEVICES at runtime")
    endif()

    # Common dependencies — macOS frameworks are always present on macOS.
    # Metal uses Objective-C class references that require link-time resolution,
    # so we link the frameworks directly (they're part of the base OS).
    set(DARWIN_LIBS c++ dl
        "-framework Metal" "-framework Foundation"
        "-framework QuartzCore" "-framework AppKit")

    # glslang and SPIRV-Cross static libraries
    # Link order matters: dependencies must come after libraries that use them
    set(DARWIN_STATIC_LIBS
        ${EXTERNAL_DIR}/glslang/libglslang.a
        ${EXTERNAL_DIR}/glslang/libglslang-default-resource-limits.a
        ${EXTERNAL_DIR}/glslang/libSPIRV.a
        ${EXTERNAL_DIR}/glslang/libGenericCodeGen.a
        ${EXTERNAL_DIR}/glslang/libMachineIndependent.a
        ${EXTERNAL_DIR}/glslang/libOSDependent.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-core.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-glsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-hlsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-msl.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-opt.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-link.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-diff.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-lint.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-reduce.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools.a
    )

    target_link_libraries(mental PUBLIC ${DARWIN_LIBS} ${DARWIN_STATIC_LIBS})

elseif(PLATFORM_WINDOWS)
    # D3D12/D3D11 are dlopen'd at runtime — no compile definitions needed

    # Vulkan headers (runtime-loaded via dlopen — no link dependency)
    if(MENTAL_HAS_VULKAN_HEADERS)
        target_include_directories(mental PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()

    # (D3D11 compile definitions removed — dlopen'd at runtime)

    # OpenCL and OpenGL are dlopen'd at runtime — no link-time dependency

    # PoCL (already discovered above)
    if(MENTAL_HAS_POCL)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_POCL=1)
        if(MENTAL_POCL_LIB_PATH)
            target_compile_definitions(mental PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
        endif()
    endif()

    # Common dependencies — all GPU backends are dlopen'd at runtime
    set(WINDOWS_LIBS ws2_32)  # Winsock — needed for htonl/ntohl in stdlink.c

    # glslang and SPIRV-Cross static libraries
    # Link order matters: dependencies must come after libraries that use them
    set(WINDOWS_STATIC_LIBS
        ${EXTERNAL_DIR}/glslang/libglslang.a
        ${EXTERNAL_DIR}/glslang/libglslang-default-resource-limits.a
        ${EXTERNAL_DIR}/glslang/libSPIRV.a
        ${EXTERNAL_DIR}/glslang/libGenericCodeGen.a
        ${EXTERNAL_DIR}/glslang/libMachineIndependent.a
        ${EXTERNAL_DIR}/glslang/libOSDependent.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-core.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-glsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-hlsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-msl.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-opt.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-link.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-diff.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-lint.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-reduce.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools.a
    )

    target_link_libraries(mental PUBLIC ${WINDOWS_LIBS} ${WINDOWS_STATIC_LIBS})

elseif(PLATFORM_LINUX)
    # Vulkan headers (runtime-loaded via dlopen — no link dependency)
    if(MENTAL_HAS_VULKAN_HEADERS)
        target_include_directories(mental PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()

    # OpenCL and OpenGL are dlopen'd at runtime — no link-time dependency

    # PoCL (already discovered above)
    if(MENTAL_HAS_POCL)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_POCL=1)
        if(MENTAL_POCL_LIB_PATH)
            target_compile_definitions(mental PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
        endif()
    endif()

    # Common dependencies
    set(LINUX_LIBS stdc++ pthread rt dl)

    # glslang and SPIRV-Cross static libraries
    # Link order matters: dependencies must come after libraries that use them
    set(LINUX_STATIC_LIBS
        ${EXTERNAL_DIR}/glslang/libglslang.a
        ${EXTERNAL_DIR}/glslang/libglslang-default-resource-limits.a
        ${EXTERNAL_DIR}/glslang/libSPIRV.a
        ${EXTERNAL_DIR}/glslang/libGenericCodeGen.a
        ${EXTERNAL_DIR}/glslang/libMachineIndependent.a
        ${EXTERNAL_DIR}/glslang/libOSDependent.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-core.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-glsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-hlsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-msl.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-opt.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-link.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-diff.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-lint.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-reduce.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools.a
    )

    target_link_libraries(mental PUBLIC ${LINUX_LIBS} ${LINUX_STATIC_LIBS})
endif()

# WebGPU (cross-platform, runtime-loaded — same on all platforms)
if(MENTAL_HAS_WEBGPU)
    target_compile_definitions(mental PRIVATE MENTAL_HAS_WEBGPU=1)
    target_include_directories(mental PRIVATE ${EXTERNAL_DIR}/wgpu/include)
endif()

# Install targets
install(TARGETS mental
    ARCHIVE DESTINATION lib
)

install(FILES mental.h DESTINATION include)

# Install licenses — 1:1 snapshot of each tool's upstream license structure
install(FILES license DESTINATION .)

# Required dependencies (always present)
install(FILES external/glslang/LICENSE.txt DESTINATION LICENSES/glslang)
install(DIRECTORY external/glslang/LICENSES DESTINATION LICENSES/glslang)
install(FILES external/spirv-cross/LICENSE DESTINATION LICENSES/spirv-cross)
install(DIRECTORY external/spirv-cross/LICENSES DESTINATION LICENSES/spirv-cross)

# Optional dependencies (may not be fetched for all platforms)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/dxc/LICENSE.TXT")
    install(FILES external/dxc/LICENSE.TXT DESTINATION LICENSES/dxc)
    install(DIRECTORY external/dxc/LICENSES DESTINATION LICENSES/dxc)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/naga/LICENSE.APACHE")
    install(FILES external/naga/LICENSE.APACHE external/naga/LICENSE.MIT DESTINATION LICENSES/naga)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/wgpu/LICENSE.APACHE")
    install(FILES external/wgpu/LICENSE.APACHE external/wgpu/LICENSE.MIT DESTINATION LICENSES/wgpu)
endif()

# Examples (optional)
option(BUILD_EXAMPLES "Build examples" ON)
if(BUILD_EXAMPLES)
    add_executable(00-example-device-info examples/00-device-info.c)
    target_link_libraries(00-example-device-info mental)

    add_executable(01-example-reference examples/01-reference.c)
    target_link_libraries(01-example-reference mental)

    add_executable(02-example-gpu-compute examples/02-gpu-compute.c)
    target_link_libraries(02-example-gpu-compute mental)

    add_executable(03-example-clone-and-pin examples/03-clone-and-pin.c)
    target_link_libraries(03-example-clone-and-pin mental)

    add_executable(04-example-state examples/04-state.c)
    target_link_libraries(04-example-state mental)

    add_executable(05-example-pipe examples/05-pipe.c)
    target_link_libraries(05-example-pipe mental)
endif()

# Sanity check binary (ships with the library)
add_executable(sanity-check sanity-check.c)
target_link_libraries(sanity-check mental)
if(NOT EMSCRIPTEN)
    target_link_libraries(sanity-check m)  # libm for fabsf
endif()
# On Windows/MinGW, statically link the C/C++ runtime so the binary
# runs without needing libc++.dll or libstdc++.dll on the system PATH.
# Note: -static-libgcc and -static-libstdc++ only, NOT -static
# (full -static can cause crashes with system DLL initialization).
if(WIN32 AND MINGW)
    target_link_options(sanity-check PRIVATE -static-libgcc -static-libstdc++)
endif()
install(TARGETS sanity-check RUNTIME DESTINATION bin)

# Tests (optional - enable with BUILD_TESTING)
option(BUILD_TESTING "Build test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
