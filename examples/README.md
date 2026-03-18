# Mental Examples

This directory contains example programs demonstrating Mental's GPU compute capabilities in both Go and C.

## Directory Structure

```
examples/
├── 00_simple_compute/
│   ├── go/
│   │   └── main.go
│   └── c/
│       ├── 00_simple_compute.c
│       └── Makefile
├── 01_observable_refs/
│   ├── go/
│   │   └── main.go
│   └── c/
├── 02_cross_language_shaders/
│   ├── go/
│   │   └── main.go
│   └── c/
├── 03_mutable_references/
│   ├── go/
│   │   └── main.go
│   └── c/
└── test/        # Go test suite
    └── *.go
```

Each example has parallel Go and C implementations in their respective subdirectories.

## Running Examples

### Go Examples

```bash
# From repository root
cd examples/00_simple_compute/go
go run main.go

# Or run all tests
cd examples/test
go test -v
```

### C Examples

```bash
# First, build the Mental library from repository root
make lib

# Then run any C example
cd examples/00_simple_compute/c
make
```

## Example Descriptions

### 00_simple_compute
**Go**: `00_simple_compute/go/main.go`
**C**: `00_simple_compute/c/00_simple_compute.c`

Basic GPU compute example that:
- Enumerates available GPU devices
- Allocates GPU memory buffers
- Compiles a simple compute kernel
- Executes the kernel
- Reads back results

Demonstrates the fundamental Mental workflow.

### 01_observable_refs (Go only)
**Go**: `01_observable_refs/go/main.go`

Demonstrates the "observable reference" pattern:
- Multiple goroutines observing GPU memory
- Concurrent reads without blocking
- Exclusive writes with automatic synchronization

### 02_cross_language_shaders (Go only)
**Go**: `02_cross_language_shaders/go/main.go`

Shows automatic shader transpilation:
- Write shaders in GLSL, HLSL, MSL, or WGSL
- Mental automatically transpiles to native format
- Same shader runs on any GPU backend

### 03_mutable_references (Go only)
**Go**: `03_mutable_references/go/main.go`

Demonstrates dynamic GPU buffer resizing:
- Allocate initial GPU memory
- Grow/shrink buffers via mutations
- Automatic reallocation when size changes

## Key Differences: Go vs C

### Memory Management

**Go (Automatic)**:
```go
ref := mental.Thought.Alloc(1024)
kernel, _ := mental.Program.Compile(source)
// No cleanup needed - GC handles it
```

**C (Manual)**:
```c
MentalReference ref = mental_create_reference(1024, 0);
MentalKernel kernel = mental_compile_kernel(source, 0);

// MUST cleanup manually
mental_release_kernel(kernel);
mental_release_reference(ref);
```

### Error Handling

**Go**:
```go
kernel, err := mental.Program.Compile(source)
if err != nil {
    panic(err)
}
```

**C**:
```c
MentalKernel kernel = mental_compile_kernel(source, 0);
if (!kernel) {
    fprintf(stderr, "Compilation failed\n");
    return 1;
}
```

## Building More C Examples

To add new C examples:

1. Create `examples/c/XX_example_name.c`
2. Add to `EXAMPLES` in `examples/c/Makefile`:
   ```makefile
   EXAMPLES = 00_simple_compute 01_new_example
   ```
3. Build: `make`
4. Run: `DYLD_LIBRARY_PATH=../.. ./01_new_example`

## Platform-Specific Notes

### macOS
- Uses Metal by default
- OpenCL available as fallback (deprecated)
- Use `DYLD_LIBRARY_PATH` for runtime library loading

### Linux
- Uses Vulkan or OpenCL
- Use `LD_LIBRARY_PATH` for runtime library loading
- May need to install Vulkan drivers

### Windows
- Uses D3D12 or Vulkan
- DLL must be in same directory or on PATH
- Build with MSVC or MinGW

## Testing

### Go Tests
```bash
cd examples/go/test
go test -v                    # Run all tests
go test -v -run TestDevice    # Run specific test
```

### C Examples
```bash
cd examples/c
make run-all                  # Build and run all examples
```

## Common Issues

### "Library not loaded" (macOS/Linux)
Set library path:
```bash
export DYLD_LIBRARY_PATH=/path/to/mental:$DYLD_LIBRARY_PATH  # macOS
export LD_LIBRARY_PATH=/path/to/mental:$LD_LIBRARY_PATH      # Linux
```

### "No GPU devices found"
- Ensure GPU drivers are installed
- Check that GPU is not being used by another application
- Try running with sudo (may need elevated permissions)

### Compilation errors
- Ensure Mental library is built: `cd ../.. && make lib`
- Check that `mental.h` and `libmental.{dylib,so}` exist in repository root

## Contributing Examples

When adding new examples:
1. Create both Go and C versions when practical
2. Add clear comments explaining what the example demonstrates
3. Include error handling
4. Update this README with description
5. Add to test suite when applicable
