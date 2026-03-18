package mental

import (
	"fmt"
	"runtime"
	"sync"

	"git.enigmaneering.org/mental/language"
	"git.enigmaneering.org/mental/transpile"
)

// Package-level state (auto-initialized)
var (
	backends   map[int]backend // Map device index to backend
	devices    []Info          // All available devices
	initOnce   sync.Once
	backendsMu sync.RWMutex
)

// init automatically discovers GPU devices and creates contexts.
func init() {
	initOnce.Do(func() {
		var err error
		devices, err = enumerateDevices()
		if err != nil {
			panic(fmt.Sprintf("device: failed to enumerate devices: %v", err))
		}

		if len(devices) == 0 {
			panic("device: no GPU devices found")
		}

		// Create backends for all devices
		backends = make(map[int]backend)
		for _, dev := range devices {
			b, err := createBackend(dev)
			if err != nil {
				panic(fmt.Sprintf("device: failed to create backend for device %d (%s): %v", dev.Index, dev.Name, err))
			}
			backends[dev.Index] = b
		}
	})
}

// getBackend returns the backend for the specified device index.
func getBackend(deviceIndex int) backend {
	backendsMu.RLock()
	defer backendsMu.RUnlock()

	b, ok := backends[deviceIndex]
	if !ok {
		panic(fmt.Sprintf("device: invalid device index %d", deviceIndex))
	}
	return b
}

// List returns all available GPU devices.
func List() []Info {
	backendsMu.RLock()
	defer backendsMu.RUnlock()

	result := make([]Info, len(devices))
	copy(result, devices)
	return result
}

// Get returns the Info for a specific device index.
func Get(index int) Info {
	backendsMu.RLock()
	defer backendsMu.RUnlock()

	if index < 0 || index >= len(devices) {
		panic(fmt.Sprintf("device: index %d out of range (0-%d)", index, len(devices)-1))
	}
	return devices[index]
}

// AllocRef allocates GPU memory on the specified device.
func AllocRef(deviceIndex, size int) *Reference {
	ref := getBackend(deviceIndex).alloc(size)
	ref.deviceIndex = deviceIndex
	ref.created = true
	runtime.SetFinalizer(ref, (*Reference).free)
	return ref
}

// CompileKernel compiles a shader for the specified device.
// Automatically detects the source language (GLSL, HLSL, MSL, SPIRV) and transpiles
// to the target backend's native language if needed (e.g., GLSL → MSL on Metal).
//
// Supported source languages:
//   - GLSL (Vulkan/OpenGL compute shaders)
//   - HLSL (Direct3D compute shaders)
//   - MSL (Metal compute shaders)
//   - SPIRV (binary format)
//
// Transpilation is automatic and uses SPIRV as an intermediate representation.
func CompileKernel(deviceIndex int, source string) (*Kernel, error) {
	// Detect source language
	srcLang := language.Detect(source)
	if srcLang == language.Auto {
		return nil, fmt.Errorf("could not detect shader language")
	}

	// Get device info to determine target language
	dev := Get(deviceIndex)
	targetLang := dev.API.NativeLanguage()

	// Transpile if necessary
	if srcLang != targetLang {
		transpiled, err := transpile.To(source, srcLang, targetLang)
		if err != nil {
			return nil, fmt.Errorf("transpilation failed (%v → %v): %w", srcLang, targetLang, err)
		}
		source = transpiled
	}

	// Compile shader using backend
	kernel, err := getBackend(deviceIndex).compileShader(source)
	if err != nil {
		return nil, err
	}
	kernel.created = true
	runtime.SetFinalizer(kernel, (*Kernel).free)
	return kernel, nil
}
