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
)

set(MENTAL_CXX_SOURCES
    transpile_glslang.cpp
    transpile_spirv_cross.cpp
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
        list(APPEND MENTAL_SOURCES backend_webgpu.c)
        set(MENTAL_HAS_WEBGPU ON)
        message(STATUS "WebGPU headers found - enabling WebGPU backend (wgpu-native, runtime-loaded)")
    else()
        message(STATUS "WebGPU headers not found - WebGPU backend disabled")
    endif()
endif()

# Platform-specific backends (all optional — library degrades gracefully)
if(PLATFORM_DARWIN)
    if(NOT MENTAL_DISABLE_METAL)
        find_library(METAL_LIBRARY Metal)
        if(METAL_LIBRARY)
            list(APPEND MENTAL_SOURCES backend_metal.m)
            set_source_files_properties(backend_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
            set(MENTAL_HAS_METAL ON)
            message(STATUS "Metal found - enabling Metal backend")
        else()
            message(STATUS "Metal not found - Metal backend disabled")
        endif()
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
        if(Vulkan_FOUND)
            list(APPEND MENTAL_CXX_SOURCES backend_vulkan.cpp)
            set(MENTAL_HAS_VULKAN ON)
            message(STATUS "Vulkan found - enabling Vulkan backend")
        else()
            message(STATUS "Vulkan not found - Vulkan backend disabled")
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
        if(Vulkan_FOUND)
            list(APPEND MENTAL_CXX_SOURCES backend_vulkan.cpp)
            set(MENTAL_HAS_VULKAN ON)
            message(STATUS "Vulkan found - enabling Vulkan backend")
        else()
            message(STATUS "Vulkan not found - Vulkan backend disabled")
        endif()
    else()
        message(STATUS "Vulkan backend force-disabled")
    endif()
endif()

# OpenGL compute backend (Linux + Windows only — macOS capped at 4.1, below the 4.3 floor)
if(NOT PLATFORM_DARWIN AND NOT MENTAL_DISABLE_OPENGL)
    find_package(OpenGL)
    if(OpenGL_FOUND OR OPENGL_FOUND)
        list(APPEND MENTAL_SOURCES backend_opengl.c)
        set(MENTAL_HAS_OPENGL ON)
        message(STATUS "OpenGL found - enabling OpenGL compute backend (requires 4.3+ at runtime)")
    else()
        message(STATUS "OpenGL not found - OpenGL backend disabled")
    endif()
endif()

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
    list(APPEND MENTAL_SOURCES backend_pocl.c)
    set(MENTAL_HAS_POCL ON)
    message(STATUS "OpenCL headers available - enabling PoCL fallback backend (dlopen'd at runtime)")
    if(EXISTS "${EXTERNAL_DIR}/pocl")
        set(MENTAL_POCL_LIB_PATH "${EXTERNAL_DIR}/pocl/${POCL_LIB_NAME}" CACHE STRING "Bundled PoCL library path")
    endif()
else()
    message(STATUS "OpenCL headers not found - PoCL backend disabled")
endif()

# Create both static and dynamic libraries
add_library(mental-static STATIC
    ${MENTAL_SOURCES}
    ${MENTAL_CXX_SOURCES}
)

add_library(mental SHARED
    ${MENTAL_SOURCES}
    ${MENTAL_CXX_SOURCES}
)

# Set output name for static library to avoid conflicts
set_target_properties(mental-static PROPERTIES OUTPUT_NAME mental-static)

# Shared library versioning (soname)
# SOVERSION is the major version — bump on breaking ABI changes.
# VERSION is the full version — bump on any release.
# Produces: libmental.so.1.0.0, libmental.so.1 → libmental.so.1.0.0, libmental.so → libmental.so.1
#           libmental.1.0.0.dylib, libmental.1.dylib → libmental.1.0.0.dylib (macOS)
set_target_properties(mental PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(mental-static PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXTERNAL_DIR}/glslang
    ${EXTERNAL_DIR}/spirv-cross
    ${EXTERNAL_DIR}/dxc/include
)

target_include_directories(mental PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXTERNAL_DIR}/glslang
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
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_METAL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_METAL=1)
    endif()

    # OpenCL framework (optional - may be deprecated in future macOS versions)
    if(NOT MENTAL_DISABLE_OPENCL)
        find_library(OPENCL_LIBRARY OpenCL)
        if(OPENCL_LIBRARY)
            target_sources(mental-static PRIVATE backend_opencl.c)
            target_sources(mental PRIVATE backend_opencl.c)
            target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENCL=1)
            target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENCL=1)
            message(STATUS "OpenCL found - enabling OpenCL backend")
        else()
            message(STATUS "OpenCL not found")
        endif()
    else()
        message(STATUS "OpenCL backend force-disabled")
    endif()

    # PoCL (already discovered above)
    if(MENTAL_HAS_POCL)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_POCL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_POCL=1)
        if(MENTAL_POCL_LIB_PATH)
            target_compile_definitions(mental-static PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
            target_compile_definitions(mental PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
        endif()
    endif()

    if(NOT MENTAL_HAS_METAL AND NOT OPENCL_LIBRARY AND NOT MENTAL_HAS_POCL)
        message(WARNING "No GPU backend available on macOS - library will report MENTAL_ERROR_NO_DEVICES at runtime")
    endif()

    # Common dependencies
    set(DARWIN_LIBS c++)
    if(MENTAL_HAS_METAL)
        list(APPEND DARWIN_LIBS ${METAL_LIBRARY} ${FOUNDATION_LIBRARY} ${QUARTZCORE_LIBRARY} ${APPKIT_LIBRARY})
    endif()
    if(OPENCL_LIBRARY)
        list(APPEND DARWIN_LIBS ${OPENCL_LIBRARY})
    endif()

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
    target_link_libraries(mental-static PUBLIC ${DARWIN_LIBS} ${DARWIN_STATIC_LIBS})

elseif(PLATFORM_WINDOWS)
    # D3D12 (already discovered above)
    if(MENTAL_HAS_D3D12)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_D3D12=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_D3D12=1)
    endif()

    # Vulkan (already discovered above)
    if(MENTAL_HAS_VULKAN)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_VULKAN=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_VULKAN=1)
    endif()

    # D3D11 (already discovered above)
    if(MENTAL_HAS_D3D11)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_D3D11=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_D3D11=1)
    endif()

    # OpenCL (optional)
    if(NOT MENTAL_DISABLE_OPENCL)
        find_package(OpenCL)
        if(OpenCL_FOUND)
            target_sources(mental-static PRIVATE backend_opencl.c)
            target_sources(mental PRIVATE backend_opencl.c)
            target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENCL=1)
            target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENCL=1)
            message(STATUS "OpenCL found - enabling OpenCL backend")
        else()
            message(STATUS "OpenCL not found")
        endif()
    else()
        message(STATUS "OpenCL backend force-disabled")
    endif()

    # OpenGL (already discovered above — shared across Linux/Windows)
    if(MENTAL_HAS_OPENGL)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENGL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENGL=1)
    endif()

    # PoCL (already discovered above)
    if(MENTAL_HAS_POCL)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_POCL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_POCL=1)
        if(MENTAL_POCL_LIB_PATH)
            target_compile_definitions(mental-static PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
            target_compile_definitions(mental PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
        endif()
    endif()

    if(NOT MENTAL_HAS_D3D12 AND NOT MENTAL_HAS_VULKAN AND NOT OpenCL_FOUND AND NOT MENTAL_HAS_OPENGL AND NOT MENTAL_HAS_POCL)
        message(WARNING "No GPU backend available on Windows - library will report MENTAL_ERROR_NO_DEVICES at runtime")
    endif()

    # Common dependencies
    set(WINDOWS_LIBS ws2_32)  # Winsock — needed for htonl/ntohl in stdlink.c
    if(MENTAL_HAS_D3D12)
        list(APPEND WINDOWS_LIBS d3d12.lib dxgi.lib)
    endif()
    if(MENTAL_HAS_VULKAN)
        list(APPEND WINDOWS_LIBS Vulkan::Vulkan)
    endif()
    if(OpenCL_FOUND)
        list(APPEND WINDOWS_LIBS OpenCL::OpenCL)
    endif()
    if(MENTAL_HAS_OPENGL)
        list(APPEND WINDOWS_LIBS OpenGL::GL)
    endif()
    if(MENTAL_HAS_D3D11)
        list(APPEND WINDOWS_LIBS d3d11.lib d3dcompiler.lib)
        if(NOT MENTAL_HAS_D3D12)
            list(APPEND WINDOWS_LIBS dxgi.lib)
        endif()
    endif()

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
    target_link_libraries(mental-static PUBLIC ${WINDOWS_LIBS} ${WINDOWS_STATIC_LIBS})

elseif(PLATFORM_LINUX)
    # Vulkan (already discovered above)
    if(MENTAL_HAS_VULKAN)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_VULKAN=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_VULKAN=1)
    endif()

    # OpenCL (optional)
    if(NOT MENTAL_DISABLE_OPENCL)
        find_package(OpenCL)
        if(OpenCL_FOUND)
            target_sources(mental-static PRIVATE backend_opencl.c)
            target_sources(mental PRIVATE backend_opencl.c)
            target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENCL=1)
            target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENCL=1)
            message(STATUS "OpenCL found - enabling OpenCL backend")
        else()
            message(STATUS "OpenCL not found")
        endif()
    else()
        message(STATUS "OpenCL backend force-disabled")
    endif()

    # OpenGL (already discovered above — shared across Linux/Windows)
    if(MENTAL_HAS_OPENGL)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENGL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENGL=1)
    endif()

    # PoCL (already discovered above)
    if(MENTAL_HAS_POCL)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_POCL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_POCL=1)
        if(MENTAL_POCL_LIB_PATH)
            target_compile_definitions(mental-static PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
            target_compile_definitions(mental PRIVATE MENTAL_POCL_LIB_PATH="${MENTAL_POCL_LIB_PATH}")
        endif()
    endif()

    if(NOT MENTAL_HAS_VULKAN AND NOT OpenCL_FOUND AND NOT MENTAL_HAS_OPENGL AND NOT MENTAL_HAS_POCL)
        message(WARNING "No GPU backend available on Linux - library will report MENTAL_ERROR_NO_DEVICES at runtime")
    endif()

    # Common dependencies
    set(LINUX_LIBS stdc++ pthread rt)
    if(MENTAL_HAS_VULKAN)
        list(APPEND LINUX_LIBS Vulkan::Vulkan)
    endif()
    if(OpenCL_FOUND)
        list(APPEND LINUX_LIBS OpenCL::OpenCL)
    endif()
    if(MENTAL_HAS_OPENGL)
        list(APPEND LINUX_LIBS OpenGL::GL X11)
    endif()
    if(MENTAL_HAS_POCL)
        list(APPEND LINUX_LIBS dl)
    endif()

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
    target_link_libraries(mental-static PUBLIC ${LINUX_LIBS} ${LINUX_STATIC_LIBS})
endif()

# WebGPU (cross-platform, runtime-loaded — same on all platforms)
if(MENTAL_HAS_WEBGPU)
    target_compile_definitions(mental-static PRIVATE MENTAL_HAS_WEBGPU=1)
    target_compile_definitions(mental PRIVATE MENTAL_HAS_WEBGPU=1)
    target_include_directories(mental-static PRIVATE ${EXTERNAL_DIR}/wgpu/include)
    target_include_directories(mental PRIVATE ${EXTERNAL_DIR}/wgpu/include)
endif()

# Install targets (both shared and static)
install(TARGETS mental mental-static
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES mental.h DESTINATION include)

# Examples (optional)
option(BUILD_EXAMPLES "Build examples" ON)
if(BUILD_EXAMPLES)
    add_executable(00-example-device-info examples/00-device-info.c)
    target_link_libraries(00-example-device-info mental-static)

    add_executable(01-example-reference examples/01-reference.c)
    target_link_libraries(01-example-reference mental-static)

    add_executable(02-example-gpu-compute examples/02-gpu-compute.c)
    target_link_libraries(02-example-gpu-compute mental-static)

    add_executable(03-example-clone-and-pin examples/03-clone-and-pin.c)
    target_link_libraries(03-example-clone-and-pin mental-static)

    add_executable(04-example-state examples/04-state.c)
    target_link_libraries(04-example-state mental-static)

    add_executable(05-example-pipe examples/05-pipe.c)
    target_link_libraries(05-example-pipe mental-static)
endif()

# Tests (optional - enable with BUILD_TESTING)
option(BUILD_TESTING "Build test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
