# Shader Transpilation

Cross-platform shader language transpilation for GPU compute kernels.

## What

The `transpile` package automatically converts shader code between languages (GLSL, HLSL, MSL, WGSL) using SPIRV as an intermediate format. This enables you to write GPU kernels once and run them on any platform:

- Write in **GLSL** (Vulkan's language) → runs on Metal
- Write in **HLSL** (Direct3D's language) → runs on Vulkan
- Write in **MSL** (Metal's language) → runs on Vulkan
- Write in **WGSL** (WebGPU's language) → runs on any platform
- etc...

The transpilation happens automatically when you compile a kernel - you never need to call this package directly unless you want manual control.

## Why

Modern GPUs use different shader languages:
- **macOS**: Metal Shading Language (MSL)
- **Linux**: GLSL or SPIRV for Vulkan
- **Windows**: HLSL for Direct3D or GLSL for Vulkan

Writing the same shader three times is tedious and error-prone. SPIRV is Khronos's standard intermediate representation - all modern GPU APIs can either consume it directly (Vulkan) or have tools to convert from it (Metal, D3D12).

We chose to:
1. Use industry-standard tools (glslang, DXC, SPIRV-Cross)
2. Vendor them for reproducible builds
3. Hide the complexity behind a simple API

## How

### Automatic (Recommended)

Just write your shader and compile it - transpilation is automatic:

```go
// Write in GLSL
kernel, err := mental.Kernel.Compile(`
    #version 450
    layout(local_size_x = 256) in;
    layout(std430, binding = 0) buffer Input { uint data[]; } input;
    layout(std430, binding = 1) buffer Output { uint data[]; } output;

    void main() {
        uint idx = gl_GlobalInvocationID.x;
        output.data[idx] = input.data[idx] * 2;
    }
`)
// On Metal: GLSL → SPIRV → MSL → compiled
// On Vulkan: GLSL → SPIRV → compiled
```

### Manual Control

If you need fine-grained control:

```go
import "github.com/enigmaneering/life/soul/core/mental/transpile"
import "github.com/enigmaneering/life/soul/core/mental/language"

// Convert between languages
msl, err := transpile.To(glslSource, language.GLSL, language.MSL)
hlsl, err := transpile.To(glslSource, language.GLSL, language.HLSL)

// Compile to SPIRV bytecode
spirv, err := transpile.ToSPIRV(glslSource, language.GLSL)

// Auto-detect source language
output, err := transpile.To(source, language.Auto, language.MSL)
```

### Supported Transpilation

| From → To | GLSL | HLSL | MSL | WGSL | SPIRV |
|-----------|------|------|-----|------|-------|
| **GLSL**  | ✅   | ✅   | ✅  | ✅   | ✅    |
| **HLSL**  | ✅   | ✅   | ✅  | ✅   | ✅    |
| **MSL**   | ❌   | ❌   | ✅  | ❌   | ❌    |
| **WGSL**  | ✅   | ✅   | ✅  | ✅   | ✅    |
| **SPIRV** | ✅   | ✅   | ✅  | ✅   | ✅    |

Note: MSL → SPIRV is not supported - MSL is designed as a compilation target only.

## Building

The transpilation system uses vendored C++ libraries. **In most cases, you don't need to do anything** - pre-built libraries are automatically downloaded when you first import the package.

### Automatic (Recommended)

Simply import the package:

```go
import "github.com/enigmaneering/life/soul/core/mental"
```

On first use, the package will:
1. Detect missing libraries
2. Download pre-built binaries from GitHub Releases (10 seconds)
3. Extract and configure automatically

Supported platforms:
- **macOS**: Intel (amd64), Apple Silicon (arm64)
- **Linux**: x86_64 (amd64), ARM64 (arm64)
- **Windows**: x86_64 (amd64), x86 (386)

### Manual Build (If Download Fails)

If the automatic download fails or you need a custom build:

```bash
# Install cmake
brew install cmake  # macOS
sudo apt install cmake  # Linux
choco install cmake  # Windows

# Build libraries (~2-5 minutes, one-time)
go generate github.com/enigmaneering/life/soul/core/mental/transpile
```

The build script:
- Clones dependencies (glslang, DXC, SPIRV-Cross, SPIRV-Tools, Naga)
- Compiles with optimal settings
- Supports incremental builds (only rebuilds changed files)

## Architecture

```
Your GLSL/HLSL/WGSL shader
        ↓
    glslang/DXC/Naga (compilation)
        ↓
   SPIRV bytecode
        ↓
   SPIRV-Cross (transpilation)
        ↓
Target language (MSL/GLSL/HLSL/WGSL)
        ↓
   Platform compiler (Metal/Vulkan/D3D12)
```

### Components

- **glslang**: Khronos reference compiler for GLSL → SPIRV
- **DXC**: Microsoft's DirectX Shader Compiler for HLSL → SPIRV
- **Naga**: WebGPU shader compiler for WGSL → SPIRV
- **SPIRV-Cross**: Khronos tool for SPIRV → GLSL/HLSL/MSL/WGSL
- **SPIRV-Tools**: Optimizer and validator

All components are vendored in `external/` and statically linked.

## Implementation Details

### Buffer Binding Preservation

Metal (MSL) has a quirk: SPIRV-Cross can reorder buffer parameters during transpilation. We solved this by explicitly mapping SPIRV binding indices to Metal buffer indices using `MSLResourceBinding`:

```cpp
// In spirv_cross_wrapper.cpp
spirv_cross::MSLResourceBinding msl_binding;
msl_binding.binding = original_binding;  // From SPIRV
msl_binding.msl_buffer = original_binding;  // Force same index in Metal
compiler.add_msl_resource_binding(msl_binding);
```

This ensures `binding = 0` in GLSL stays `[[buffer(0)]]` in MSL.

### Language Detection

Source language is auto-detected via simple heuristics in `language/detect.go`:
- `#version` → GLSL
- `#include <metal_stdlib>` → MSL
- `[numthreads` → HLSL
- `@compute` or `@group` → WGSL
- Binary SPIRV magic number → SPIRV

### Error Handling

The package validates dependencies at startup via `build_check.go`. If libraries aren't built, you get a clear error message with build instructions rather than cryptic linker errors.

### Platform Support

- **macOS**: Full support (Metal backend with GLSL/HLSL transpilation)
- **Linux**: Full support (Vulkan backend with HLSL transpilation)
- **Windows**: Planned (D3D12 backend, Vulkan fallback available)

## Notes

**Linker Warning on macOS**: You may see `ld: warning: ignoring duplicate libraries: '-lc++'`. This is harmless - it occurs because we link multiple C++ source files. To suppress:
```bash
export CGO_LDFLAGS_ALLOW="-Wl,-no_warn_duplicate_libraries"
export CGO_LDFLAGS="-Wl,-no_warn_duplicate_libraries"
```

**Build Time**: First build takes 2-5 minutes. Subsequent builds are incremental (only rebuild changed files).

**Binary Size**: Transpilation libraries add ~50MB to your binary. This is acceptable for developer tools and enables truly portable GPU code.
