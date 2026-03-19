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

# Platform-specific backends
if(PLATFORM_DARWIN)
    list(APPEND MENTAL_SOURCES backend_opencl.c)
    list(APPEND MENTAL_SOURCES backend_metal.m)
    set_source_files_properties(backend_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(PLATFORM_WINDOWS)
    list(APPEND MENTAL_SOURCES backend_opencl.c)
    list(APPEND MENTAL_CXX_SOURCES backend_d3d12.cpp)
elseif(PLATFORM_LINUX)
    list(APPEND MENTAL_SOURCES backend_opencl.c)
    list(APPEND MENTAL_CXX_SOURCES backend_vulkan.cpp)
endif()

# Create static library
add_library(mental STATIC
    ${MENTAL_SOURCES}
    ${MENTAL_CXX_SOURCES}
)

target_include_directories(mental PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXTERNAL_DIR}/glslang
    ${EXTERNAL_DIR}/spirv-cross
)

# Platform-specific configurations
if(PLATFORM_DARWIN)
    # Metal framework
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(APPKIT_LIBRARY AppKit)
    target_link_libraries(mental PUBLIC ${METAL_LIBRARY} ${FOUNDATION_LIBRARY} ${QUARTZCORE_LIBRARY} ${APPKIT_LIBRARY})

    # OpenCL framework
    find_library(OPENCL_LIBRARY OpenCL)
    target_link_libraries(mental PUBLIC ${OPENCL_LIBRARY})

    # glslang and SPIRV-Cross
    target_link_libraries(mental PUBLIC
        ${EXTERNAL_DIR}/glslang/libglslang.a
        ${EXTERNAL_DIR}/glslang/libglslang-default-resource-limits.a
        ${EXTERNAL_DIR}/glslang/libSPIRV.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools.a
        ${EXTERNAL_DIR}/glslang/libSPIRV-Tools-opt.a
        ${EXTERNAL_DIR}/glslang/libGenericCodeGen.a
        ${EXTERNAL_DIR}/glslang/libMachineIndependent.a
        ${EXTERNAL_DIR}/glslang/libOSDependent.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-core.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-glsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-hlsl.a
        ${EXTERNAL_DIR}/spirv-cross/libspirv-cross-msl.a
    )

    # C++ standard library
    target_link_libraries(mental PUBLIC c++)

elseif(PLATFORM_WINDOWS)
    # D3D12 and DXGI
    target_link_libraries(mental PUBLIC d3d12.lib dxgi.lib)

    # OpenCL
    find_package(OpenCL REQUIRED)
    target_link_libraries(mental PUBLIC OpenCL::OpenCL)

    # glslang and SPIRV-Cross
    target_link_libraries(mental PUBLIC
        ${EXTERNAL_DIR}/glslang/lib/windows/glslang.lib
        ${EXTERNAL_DIR}/glslang/lib/windows/SPIRV.lib
        ${EXTERNAL_DIR}/glslang/lib/windows/SPIRV-Tools.lib
        ${EXTERNAL_DIR}/glslang/lib/windows/SPIRV-Tools-opt.lib
        ${EXTERNAL_DIR}/glslang/lib/windows/GenericCodeGen.lib
        ${EXTERNAL_DIR}/glslang/lib/windows/MachineIndependent.lib
        ${EXTERNAL_DIR}/glslang/lib/windows/OSDependent.lib
        ${EXTERNAL_DIR}/spirv-cross/lib/windows/spirv-cross-core.lib
        ${EXTERNAL_DIR}/spirv-cross/lib/windows/spirv-cross-glsl.lib
        ${EXTERNAL_DIR}/spirv-cross/lib/windows/spirv-cross-hlsl.lib
        ${EXTERNAL_DIR}/spirv-cross/lib/windows/spirv-cross-msl.lib
    )

elseif(PLATFORM_LINUX)
    # Vulkan
    find_package(Vulkan REQUIRED)
    target_link_libraries(mental PUBLIC Vulkan::Vulkan)

    # OpenCL
    find_package(OpenCL REQUIRED)
    target_link_libraries(mental PUBLIC OpenCL::OpenCL)

    # glslang and SPIRV-Cross
    target_link_libraries(mental PUBLIC
        ${EXTERNAL_DIR}/glslang/lib/linux/libglslang.a
        ${EXTERNAL_DIR}/glslang/lib/linux/libSPIRV.a
        ${EXTERNAL_DIR}/glslang/lib/linux/libSPIRV-Tools.a
        ${EXTERNAL_DIR}/glslang/lib/linux/libSPIRV-Tools-opt.a
        ${EXTERNAL_DIR}/glslang/lib/linux/libGenericCodeGen.a
        ${EXTERNAL_DIR}/glslang/lib/linux/libMachineIndependent.a
        ${EXTERNAL_DIR}/glslang/lib/linux/libOSDependent.a
        ${EXTERNAL_DIR}/spirv-cross/lib/linux/libspirv-cross-core.a
        ${EXTERNAL_DIR}/spirv-cross/lib/linux/libspirv-cross-glsl.a
        ${EXTERNAL_DIR}/spirv-cross/lib/linux/libspirv-cross-hlsl.a
        ${EXTERNAL_DIR}/spirv-cross/lib/linux/libspirv-cross-msl.a
    )

    # C++ standard library and pthread
    target_link_libraries(mental PUBLIC stdc++ pthread)
endif()

# Install targets
install(TARGETS mental
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES mental.h DESTINATION include)

# Tests (optional - enable with BUILD_TESTING)
option(BUILD_TESTING "Build test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
