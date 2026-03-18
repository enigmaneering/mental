package mental

// backend abstracts GPU APIs (Metal, Vulkan, Direct3D12).
// This is an internal interface not exposed to users.
//
// Each platform implements this interface with its native GPU API:
//   - macOS: Metal (backend_metal.go)
//   - Linux/Windows: Vulkan (backend_vulkan.go)
//   - Windows: Direct3D 12 (backend_d3d12.go, not yet implemented)
//
// Backends receive shaders in their native language - transpilation happens
// at a higher level (see CompileKernel in api.go).
type backend interface {
	// Memory operations
	alloc(size int) *Reference        // Allocate GPU memory
	free(ref *Reference)              // Release GPU memory
	read(ref *Reference, dest []byte) // Copy GPU → CPU
	write(ref *Reference, src []byte) // Copy CPU → GPU
	clone(ref *Reference) *Reference  // Duplicate GPU memory

	// Shader operations
	compileShader(source string) (*Kernel, error) // Compile native shader
	freeShader(kernel *Kernel)                    // Release shader resources

	// Compute operations
	dispatch(kernel *Kernel, inputs []*Reference, output *Reference, workSize int) error // Execute shader

	// Device info
	deviceInfo() Info // Get device metadata
}
