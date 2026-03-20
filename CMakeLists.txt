cmake_minimum_required(VERSION 3.15)
project(mental VERSION 0.1.0 LANGUAGES C CXX)

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
    transpile.c
    transpile_other.c
)

set(MENTAL_CXX_SOURCES
    transpile_glslang.cpp
    transpile_spirv_cross.cpp
)

# Platform-specific backends (OpenCL is added conditionally later)
if(PLATFORM_DARWIN)
    list(APPEND MENTAL_SOURCES backend_metal.m)
    set_source_files_properties(backend_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(PLATFORM_WINDOWS)
    list(APPEND MENTAL_CXX_SOURCES backend_d3d12.cpp)
elseif(PLATFORM_LINUX)
    list(APPEND MENTAL_CXX_SOURCES backend_vulkan.cpp)
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
    # Metal framework
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(APPKIT_LIBRARY AppKit)

    # OpenCL framework (optional - may be deprecated in future macOS versions)
    find_library(OPENCL_LIBRARY OpenCL)
    if(OPENCL_LIBRARY)
        target_sources(mental-static PRIVATE backend_opencl.c)
        target_sources(mental PRIVATE backend_opencl.c)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENCL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENCL=1)
        message(STATUS "OpenCL found - enabling OpenCL backend")
    else()
        message(STATUS "OpenCL not found - only Metal backend available")
    endif()

    # Common dependencies for both shared and static
    set(DARWIN_LIBS
        ${METAL_LIBRARY} ${FOUNDATION_LIBRARY} ${QUARTZCORE_LIBRARY} ${APPKIT_LIBRARY}
        c++
    )

    if(OPENCL_LIBRARY)
        list(APPEND DARWIN_LIBS ${OPENCL_LIBRARY})
    endif()

    # glslang and SPIRV-Cross static libraries (only for mental-static)
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

    # Link shared library (needs glslang/SPIRV-Cross too for symbols)
    target_link_libraries(mental PUBLIC ${DARWIN_LIBS} ${DARWIN_STATIC_LIBS})

    # Link static library (with all static dependencies embedded)
    target_link_libraries(mental-static PUBLIC ${DARWIN_LIBS} ${DARWIN_STATIC_LIBS})

elseif(PLATFORM_WINDOWS)
    # OpenCL (optional on Windows - D3D12 is the primary backend)
    find_package(OpenCL)

    # Add OpenCL backend source if OpenCL is available
    if(OpenCL_FOUND)
        target_sources(mental-static PRIVATE backend_opencl.c)
        target_sources(mental PRIVATE backend_opencl.c)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENCL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENCL=1)
        message(STATUS "OpenCL found - enabling OpenCL backend")
    else()
        message(STATUS "OpenCL not found - only D3D12 backend available")
    endif()

    # Common dependencies for both shared and static
    set(WINDOWS_LIBS
        d3d12.lib dxgi.lib dxcompiler.lib
    )

    if(OpenCL_FOUND)
        list(APPEND WINDOWS_LIBS OpenCL::OpenCL)
    endif()

    # glslang and SPIRV-Cross static libraries (only for mental-static)
    # Link order matters: dependencies must come after libraries that use them
    # MinGW uses .a extension for static libraries
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

    # Link shared library (needs glslang/SPIRV-Cross too for symbols)
    target_link_libraries(mental PUBLIC ${WINDOWS_LIBS} ${WINDOWS_STATIC_LIBS})

    # Link static library (with all static dependencies embedded)
    target_link_libraries(mental-static PUBLIC ${WINDOWS_LIBS} ${WINDOWS_STATIC_LIBS})

elseif(PLATFORM_LINUX)
    # Vulkan
    find_package(Vulkan REQUIRED)

    # OpenCL (optional - not all Linux systems have it)
    find_package(OpenCL)
    if(OpenCL_FOUND)
        target_sources(mental-static PRIVATE backend_opencl.c)
        target_sources(mental PRIVATE backend_opencl.c)
        target_compile_definitions(mental-static PRIVATE MENTAL_HAS_OPENCL=1)
        target_compile_definitions(mental PRIVATE MENTAL_HAS_OPENCL=1)
        message(STATUS "OpenCL found - enabling OpenCL backend")
    else()
        message(STATUS "OpenCL not found - only Vulkan backend available")
    endif()

    # Common dependencies for both shared and static
    set(LINUX_LIBS
        Vulkan::Vulkan
        stdc++ pthread
    )

    if(OpenCL_FOUND)
        list(APPEND LINUX_LIBS OpenCL::OpenCL)
    endif()

    # glslang and SPIRV-Cross static libraries (only for mental-static)
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

    # Link shared library (needs glslang/SPIRV-Cross too for symbols)
    target_link_libraries(mental PUBLIC ${LINUX_LIBS} ${LINUX_STATIC_LIBS})

    # Link static library (with all static dependencies embedded)
    target_link_libraries(mental-static PUBLIC ${LINUX_LIBS} ${LINUX_STATIC_LIBS})
endif()

# Install targets (both shared and static)
install(TARGETS mental mental-static
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES mental.h DESTINATION include)

# Tests (optional - enable with BUILD_TESTING)
option(BUILD_TESTING "Build test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
