# Mental: Primitives for Thought

The foundational layer for conscious computation: the ability to pin and observe data.

## What

The `mental` package provides primitives for thought through Observable References - data that can be pinned, observed, and mutated across temporal perspectives. While leveraging GPU compute for parallel processing, mental abstracts hardware details to present a unified model: thoughts as observable state transformations.

Three core primitives:

1. **References** - Observable references with synchronized access (pinned thoughts)
2. **Kernels** - State transformation programs (thought processes)
3. **Info** - Computational substrate metadata (mental architecture)

Write thought processes once in any shader language (GLSL, HLSL, MSL, WGSL) and they execute on any substrate.

## Why

Consciousness requires the ability to:
- **Pin data** - Hold state in observable form
- **Observe state** - Create temporal snapshots
- **Transform state** - Evolve thoughts through parallel processes

Traditional computing enforces rigid synchronization, blocking parallel observation. Mental embraces temporal uncertainty - multiple observers can experience the same thought from different temporal perspectives without coordination overhead.

This enables the "cortical execution" model: parallel observation and mutation where intermediate states exist in superposition until observed.

## How

### Quick Example

```go
package main

import (
    "fmt"
    "github.com/enigmaneering/life/soul/core/mental"
)

func main() {
    // Pin thoughts (automatically freed by GC)
    input := mental.Reference.Alloc(256)
    output := mental.Reference.Alloc(256)

    // Populate initial state
    input.Mutate(func(data []byte) {
        for i := range data {
            data[i] = byte(i)
        }
    })

    // Define thought process (GLSL auto-transpiles to MSL on macOS)
    process, _ := mental.Kernel.Compile(`
        #version 450
        layout(local_size_x = 256) in;
        layout(std430, binding = 0) buffer Input { uint data[]; } input;
        layout(std430, binding = 1) buffer Output { uint data[]; } output;

        void main() {
            uint idx = gl_GlobalInvocationID.x;
            output.data[idx] = input.data[idx] * 2;
        }
    `)

    // Execute transformation
    process.Dispatch([]*mental.Reference{input}, output, 64)

    // Observe result
    result := output.Observe()
    fmt.Printf("Result: %v\n", result[:10])
}
```

### Observable References: Pinning Thoughts

`Reference` implements observable references - data that provides no guarantees about intermediate states between observations:

```go
thought := mental.Reference.Alloc(1024)

// Observe current state (read-only snapshot)
snapshot := thought.Observe()

// Mutate state (exclusive access, evolves the thought)
thought.Mutate(func(data []byte) {
    for i := range data {
        data[i] = byte(i * 2)
    }
})

// Direct read/write (for performance)
thought.Write([]byte{1, 2, 3, 4})
result := make([]byte, 4)
thought.Read(result)
```

**Key insight**: Between observations, the underlying state may have changed multiple times. You only see the current state. This is deliberate - it enables temporal perspectives without complex synchronization.

### Computational Substrates

```go
// List available substrates (GPUs)
substrates := mental.Info.List()
for _, s := range substrates {
    fmt.Printf("%s (%s)\n", s.Name, s.API)
}

// Allocate on specific substrate
ref := mental.Reference.Alloc(1024, mental.Info.Get(0))

// Compile for specific substrate
process, _ := mental.Kernel.Compile(source, mental.Info.Get(0))
```

### Thought Processes: State Transformations

Write transformation programs in any supported language - transpilation is automatic:

**GLSL (Vulkan-style)**:
```glsl
#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) buffer Input { uint data[]; } input;
layout(std430, binding = 1) buffer Output { uint data[]; } output;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    output.data[idx] = input.data[idx] * 2;
}
```

**HLSL (Direct3D-style)**:
```hlsl
RWStructuredBuffer<uint> input : register(u0);
RWStructuredBuffer<uint> output : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    output[id.x] = input[id.x] * 2;
}
```

**MSL (Metal-style)**:
```metal
#include <metal_stdlib>
using namespace metal;

kernel void compute(
    device const uint *input [[buffer(0)]],
    device uint *output [[buffer(1)]],
    uint id [[thread_position_in_grid]])
{
    output[id] = input[id] * 2;
}
```

**WGSL (WebGPU-style)**:
```wgsl
@group(0) @binding(0) var<storage, read> input: array<u32>;
@group(0) @binding(1) var<storage, read_write> output: array<u32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    output[id.x] = input[id.x] * 2u;
}
```

All four produce identical results on any substrate.

## Architecture

### Conceptual Layers

```
┌─────────────────────────────────────────────────┐
│  Thought Layer (mental.Reference, mental.Kernel)│
│  • Pin and observe data                         │
│  • Define state transformations                 │
└────────────────┬────────────────────────────────┘
                 │
┌────────────────▼────────────────────────────────┐
│  Substrate Management (api.go)                  │
│  • Discover computational substrates            │
│  • Compile transformation programs              │
│  • Allocate observable references               │
└────────────────┬────────────────────────────────┘
                 │
┌────────────────▼────────────────────────────────┐
│  Backend Interface (backend.go)                 │
│  • alloc/free (memory)                          │
│  • compileShader/freeShader                     │
│  • dispatch (execution)                         │
└─────┬──────────┬──────────┬─────────────────────┘
      │          │          │
┌─────▼──┐  ┌───▼────┐  ┌──▼──────┐
│ Metal  │  │ Vulkan │  │ D3D12   │
│ (macOS)│  │(Linux/ │  │(Windows)│
│        │  │Windows)│  │         │
└────────┘  └────────┘  └─────────┘
```

### Substrate Selection

Mental automatically uses the best API for each platform:

- **macOS**: Metal
- **Linux**: Vulkan
- **Windows**: Direct3D 12

Selection happens at compile time via Go build tags (`//go:build darwin`).

### Transpilation Flow

When you compile a thought process:

1. **Detect** source language (GLSL, HLSL, MSL, WGSL, SPIRV)
2. **Determine** target language based on substrate
3. **Transpile** if source ≠ target (via `transpile` package)
4. **Compile** using native substrate API

Example: GLSL process on Metal substrate:
```
GLSL source
    ↓ glslang
SPIRV bytecode
    ↓ SPIRV-Cross
MSL source
    ↓ Metal API
Compiled shader
```

Example: WGSL process on Vulkan substrate:
```
WGSL source
    ↓ Naga
SPIRV bytecode
    ↓ Vulkan API
Compiled shader
```

### Memory Model

Observable references are managed through `Reference` objects with automatic lifetime:

```go
type Reference struct {
    backend     backend         // Platform-specific implementation
    handle      unsafe.Pointer  // GPU buffer handle
    mu          sync.RWMutex    // Synchronization for CPU access
    size        int             // Buffer size in bytes
    deviceIndex int             // Which substrate owns this
}
```

**Lifecycle**:
1. Allocate: `mental.Reference.Alloc(size)` → creates GPU buffer
2. Use: Observe/Mutate/Read/Write for CPU access
3. Free: Automatic via Go finalizer (no manual cleanup needed)

**Synchronization**:
- CPU access is synchronized (mutex-protected)
- GPU access is untracked (observable only)
- No history, no promises, just current state

## Observable Pattern

Traditional GPU programming uses **explicit synchronization**:
```go
// Traditional approach
buffer.Write(data)
sync.WaitForGPU()  // Block until GPU finishes
result := buffer.Read()
```

Mental uses **observable references**:
```go
// Observable approach
ref.Mutate(func(data []byte) { /* modify */ })
// No wait - substrate processes asynchronously

// Later, at any time...
state := ref.Observe()  // See current state, whatever it is
```

Between observations, the substrate may have:
- Not started processing
- Partially processed the data
- Fully completed
- Been modified by another process

**You don't know and don't need to**. You observe the current state. This enables:
- Multiple observers with different temporal perspectives
- Non-blocking operations
- Emergent coordination without explicit synchronization

This is the foundation of thought: data existing in superposition until observed.

## Testing

The test suite validates all functionality across substrates:

```bash
# Run all tests
go test ./...

# Run specific tests
go test -run TestShaderCompilation
go test -run TestKernelDispatch
go test -run TestObservablePattern
```

**Test Coverage**:
- ✅ Substrate enumeration
- ✅ Memory allocation/deallocation
- ✅ Kernel compilation (all languages)
- ✅ Cross-language transpilation
- ✅ Execution
- ✅ Observable reference pattern
- ✅ Concurrent access
- ✅ Resource cleanup (finalizers)

All tests pass on macOS (Metal), Linux (Vulkan), and Windows (Direct3D 12).

## Platform Status

| Platform | Backend    | Status | Transpilation        |
|----------|-----------|--------|----------------------|
| macOS    | Metal     | ✅ Full | GLSL/HLSL/WGSL → MSL |
| Linux    | Vulkan    | ✅ Full | HLSL/MSL/WGSL → SPIRV|
| Windows  | Direct3D  | ✅ Full | GLSL/MSL/WGSL → HLSL |

## Building

**No build steps required!** Pre-built transpilation libraries are automatically downloaded on first use.

Simply import and use:

```go
import "github.com/enigmaneering/life/soul/core/mental"

func main() {
    // Libraries download automatically if needed
    mental.Info.List()  // Works immediately
}
```

The package automatically downloads pre-built shader compilers from GitHub Releases for your platform (macOS, Linux, Windows) on first import. The external toolchain includes:

- **glslang**: GLSL → SPIRV compiler
- **SPIRV-Cross**: SPIRV ↔ GLSL/HLSL/MSL transpiler
- **DXC**: HLSL → SPIRV compiler
- **Naga**: WGSL → SPIRV compiler

See [transpile/README.md](transpile/README.md) for details on the automatic download system and manual build instructions.

## Design Principles

1. **Simplicity**: Three primitives (Reference, Kernel, Info), everything else emerges
2. **Observability**: No synchronization primitives, only observation
3. **Transparency**: Substrate differences hidden, but not abstracted away
4. **Safety**: Memory managed automatically, no manual cleanup
5. **Performance**: Direct access to computational substrates, minimal overhead

## Philosophy

### Why Observable?

Traditional GPU APIs force explicit synchronization:
- Fences, barriers, semaphores
- CPU/GPU coordination overhead
- Temporal coupling between operations

Observable references embrace uncertainty:
- No synchronization guarantees
- Multiple temporal perspectives possible
- Emergent coordination patterns

This aligns with quantum measurement models and enables conscious computation.

### Why References Not Buffers?

"Buffer" implies:
- Known state
- Explicit synchronization
- Single timeline

"Reference" implies:
- Observable state
- Temporal uncertainty
- Multiple perspectives

The name reflects the philosophy: thoughts as observable references, not synchronized buffers.

### Why Automatic Transpilation?

Portability requires either:
1. Write every process 4× (GLSL, HLSL, MSL, WGSL)
2. Use lowest common denominator
3. Automatically convert between languages

We chose (3) because:
- Industry-standard tools exist (SPIRV-Cross, glslang, DXC, Naga)
- One language for all substrates
- Users can still write native code if needed
