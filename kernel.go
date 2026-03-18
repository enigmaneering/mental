package mental

import (
	"fmt"
	"sync"
	"unsafe"

	"git.enigmaneering.org/mental/language"
)

// Kernel represents a GPU compute kernel that has been compiled
// for the current backend. Kernels can be executed multiple times with
// different input buffers.
//
// GPU kernel resources are automatically released when the Kernel
// is garbage collected. Users do not need to manually manage kernel resources.
//
// IMPORTANT: Kernel instances must be created via proper factory methods.
// Do not instantiate Kernel directly.
type Kernel struct {
	backend  backend
	handle   unsafe.Pointer
	language language.Language
	freed    bool       // Track if manually freed to avoid double-free
	created  bool       // Sanity check to ensure proper construction
	mu       sync.Mutex // Protect freed flag
}

// sanityCheck verifies this Kernel was created properly via factory methods.
func (k *Kernel) sanityCheck() {
	if !k.created {
		panic("device.Kernel must be created via proper factory methods (e.g., glitter.Shader.Compile())")
	}
}

// Dispatch executes the compute kernel with the given input and output buffers.
//
// Parameters:
//   - inputs: Array of input buffer references
//   - output: Output buffer reference
//   - workSize: Number of work items to dispatch (e.g., 1024 means 1024 parallel invocations)
//
// The kernel will be executed on the GPU with the specified work size.
// The exact thread group configuration is determined by the backend.
func (k *Kernel) Dispatch(inputs []*Reference, output *Reference, workSize int) error {
	k.sanityCheck()
	k.mu.Lock()
	defer k.mu.Unlock()
	if k.freed {
		return fmt.Errorf("device: dispatch on freed kernel")
	}
	return k.backend.dispatch(k, inputs, output, workSize)
}

// Language returns the source language this kernel was compiled from.
func (k *Kernel) Language() language.Language {
	k.sanityCheck()
	return k.language
}

// free is the internal cleanup function called by the finalizer or manual Free().
func (k *Kernel) free() {
	k.mu.Lock()
	defer k.mu.Unlock()

	if !k.freed {
		k.backend.freeShader(k)
		k.freed = true
	}
}
