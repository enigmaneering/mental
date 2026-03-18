package mental

// Static factory for Program operations
var Program = programFactory{}

type programFactory struct{}

// Compile compiles shader source code with automatic language detection.
// The shader language is detected from the source code syntax, then transpiled
// to the native format required by the current GPU backend if necessary.
//
// GPU kernel resources are automatically freed when the kernel is garbage collected.
// Manual cleanup is not required.
//
// Supported languages: GLSL, HLSL, MSL, SPIR-V
//
// The optional target parameter specifies which device to compile the kernel for.
// If omitted, defaults to device 0.
//
// Example:
//
//	kernel, err := device.Program.Compile(`
//	    #version 450
//	    layout(local_size_x = 256) in;
//	    void main() { ... }
//	`)
func (programFactory) Compile(source string, target ...Info) (*Kernel, error) {
	deviceIndex := 0
	if len(target) > 0 {
		deviceIndex = target[0].Index
	}

	return CompileKernel(deviceIndex, source)
}

// Static factory for Reference operations
var Thought = thoughtFactory{}

type thoughtFactory struct{}

// Alloc allocates GPU memory of the specified size in bytes.
// The returned Reference provides synchronized access to the GPU memory.
//
// GPU memory is automatically freed when the Reference is garbage collected.
// Manual cleanup is not required.
//
// The optional target parameter specifies which device to allocate memory on.
// If omitted, defaults to device 0.
func (thoughtFactory) Alloc(size int, target ...Info) *Reference {
	deviceIndex := 0
	if len(target) > 0 {
		deviceIndex = target[0].Index
	}

	return AllocRef(deviceIndex, size)
}

// From allocates GPU memory and populates it with the provided data.
// This is a convenience method that combines Alloc() and Write().
//
// GPU memory is automatically freed when the Reference is garbage collected.
// Manual cleanup is not required.
//
// The optional target parameter specifies which device to allocate memory on.
// If omitted, defaults to device 0.
func (thoughtFactory) From(data []byte, target ...Info) *Reference {
	deviceIndex := 0
	if len(target) > 0 {
		deviceIndex = target[0].Index
	}

	ref := AllocRef(deviceIndex, len(data))
	ref.Write(data)
	return ref
}
