# `libmental`

A Universal GPU compute library

Write compute shaders in any language. Run them on any GPU. Fall back gracefully when there isn't one.

## Architecture

`libmental` abstracts GPU compute across every major graphics API behind a single C interface. All backends are loaded dynamically at runtime via `dlopen`/`LoadLibrary` -- the library has **zero** GPU link-time dependencies. Backends that aren't available on the target machine are silently skipped.

```
                  mental_compile() + mental_dispatch()
╭──────────────────────────────────┴──────────────────────────────────────╮
│                      Automatic Backend Selection                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│ Darwin:  Metal -> Vulkan -> WebGPU -> OpenGL -> D3D11 -> OpenCL -> PoCL │
│ Windows: D3D12 -> Vulkan -> WebGPU -> OpenGL -> D3D11 -> OpenCL -> PoCL │
│ Linux:   Vulkan -> WebGPU -> OpenGL -> D3D11 -> OpenCL -> PoCL          │
│                                                                         │
╰──────────────────────────────────┬──────────────────────────────────────╯
                              GPU or CPU
```

The first backend that initializes successfully and reports devices wins. Everything else is skipped.

## Transpilation

Shaders are automatically transpiled to whatever the active backend needs - through other languages or intermediaries, as necessary:

```                             
         Input              ╎      Backend
              ╭───────────╮ ╎
╭────────╮    │ GLSL      │ ╎ ╭→ Vulkan, OpenGL
│        │    │ HLSL      │ ╎ ├→ D3D12, D3D11
│ SPIR-V │ ←→ │ MSL       │─┼─┼→ Metal
│        │    │ WGSL      │ ╎ ├→ WebGPU    
╰────────╯    │ OpenCL C  │ ╎ ╰→ OpenCL, PoCL    
              ╰───────────╯ ╎
```

The OpenCL C path is a custom transpiler (`transpile_opencl.c`) that converts spirv-cross GLSL output into valid OpenCL C with a GLSL compatibility shim. This enables GLSL compute shaders to run on CPU-only PoCL -- the absolute last resort.

## Quick Start

```c
#include "mental.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    // Get the first available GPU
    mental_device dev = mental_device_get(0);
    printf("Using: %s (%s)\n", mental_device_name(dev), mental_device_api_name(dev));

    // Compile a GLSL compute shader
    const char* shader =
        "#version 450\n"
        "layout(local_size_x = 256) in;\n"
        "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
        "layout(std430, binding = 1) readonly buffer B { float b[]; };\n"
        "layout(std430, binding = 2) writeonly buffer C { float c[]; };\n"
        "void main() {\n"
        "    uint i = gl_GlobalInvocationID.x;\n"
        "    c[i] = a[i] + b[i];\n"
        "}\n";
    mental_kernel k = mental_compile(dev, shader, strlen(shader));

    // Create and pin buffers
    mental_reference a = mental_reference_create(1024, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference b = mental_reference_create(1024, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference c = mental_reference_create(1024, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(a, dev);
    mental_reference_pin(b, dev);
    mental_reference_pin(c, dev);

    // Dispatch
    mental_reference inputs[] = {a, b};
    mental_reference outputs[] = {c};
    mental_dispatch(k, inputs, 2, outputs, 1, 256);

    // Read results back
    float result[256];
    mental_reference_read(c, result, sizeof(result));

    // Cleanup
    mental_kernel_finalize(k);
    mental_reference_close(a);
    mental_reference_close(b);
    mental_reference_close(c);
    return 0;
}
```

Link: `cc -o app app.c -Iinclude -Llib -lmental -lc++ -ldl`

## Pipes (Chained Dispatch)

Record multiple kernel dispatches into a single GPU submission. Data stays on the GPU between stages -- no CPU round-trips.

```c
mental_pipe pipe = mental_pipe_create(dev);
mental_pipe_add(pipe, k_multiply, inputs, 1, intermediates, 1, N);
mental_pipe_add(pipe, k_add,      intermediates, 1, outputs, 1, N);
mental_pipe_execute(pipe);  // one GPU submission for both
mental_pipe_finalize(pipe);
```

## References and Disclosure

References are process-local data buffers with GPU pinning and access control.

```c
// Open: anyone can read/write
mental_reference ref = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

// Exclusive: requires credential
mental_disclosure dh;
mental_reference secret = mental_reference_create(size, MENTAL_RELATIONALLY_EXCLUSIVE,
                                                   "password", 8, &dh);

// Pin to GPU for compute
mental_reference_pin(ref, dev);
```

## Sanity Check

Every build ships with a self-test binary:

```bash
./sanity-check
```

```
mental sanity check
===================

  [PASS] state
  [PASS] device enumeration
  [PASS] reference lifecycle
  [PASS] disclosure

  Device: Apple M4 Max (Metal)

  [PASS] GPU buffer
  [PASS] shader compile
  [PASS] dispatch
  [PASS] pipe

---
8 passed, 0 failed, 0 skipped

OK
```

Or from code: `int result = mental_sanity_check();`

## Runtime State

```c
mental_state* state = mental_state_get();
// state->active_backend_name  -> "Metal"
// state->device_count         -> 1
// state->libraries            -> [{name: "Metal", version: "selected"}, ...]
mental_state_free(state);
```

## [License](license)