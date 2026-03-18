package mental

import (
	"runtime"
	"testing"

	"git.enigmaneering.org/mental/language"
)

// Unified cross-platform test suite
// Tests automatically adapt based on platform and available backends

func TestDeviceEnumeration(t *testing.T) {
	suite := NewSharedTestSuite(t)
	suite.TestDeviceEnumeration()
}

func TestDeviceGet(t *testing.T) {
	suite := NewSharedTestSuite(t)
	suite.TestDeviceGet()
}

func TestBufferOperations(t *testing.T) {
	suite := NewSharedTestSuite(t)

	t.Run("Allocation", func(t *testing.T) {
		suite.TestBufferAllocation()
	})
	t.Run("ReadWrite", func(t *testing.T) {
		suite.TestBufferReadWrite()
	})
	t.Run("Clone", func(t *testing.T) {
		suite.TestBufferClone()
	})
	t.Run("Resize", func(t *testing.T) {
		suite.TestBufferResize()
	})
}

func TestObservablePattern(t *testing.T) {
	suite := NewSharedTestSuite(t)
	suite.TestObservablePattern()
}

func TestFinalizers(t *testing.T) {
	suite := NewSharedTestSuite(t)
	suite.TestFinalizers()
}

func TestErrorHandling(t *testing.T) {
	suite := NewSharedTestSuite(t)
	suite.TestErrorHandling()
}

// Shader compilation tests - test platform-appropriate languages
func TestShaderCompilation(t *testing.T) {
	suite := NewSharedTestSuite(t)

	// GLSL works everywhere (Vulkan native, transpiled to Metal/D3D12)
	t.Run("GLSL", func(t *testing.T) {
		suite.TestShaderCompilation(Shaders.SimpleKernelGLSL, language.GLSL)
	})

	// HLSL works everywhere (D3D12 native, transpiled to Metal/Vulkan)
	t.Run("HLSL", func(t *testing.T) {
		suite.TestShaderCompilation(Shaders.SimpleKernelHLSL, language.HLSL)
	})

	// WGSL works everywhere (WebGPU, transpiled to all platforms)
	t.Run("WGSL", func(t *testing.T) {
		suite.TestShaderCompilation(Shaders.SimpleKernelWGSL, language.WGSL)
	})

	// MSL only works on Metal (cannot transpile FROM MSL)
	if runtime.GOOS == "darwin" {
		t.Run("MSL", func(t *testing.T) {
			suite.TestShaderCompilation(Shaders.SimpleKernelMSL, language.MSL)
		})
	}
}

// Kernel dispatch tests
func TestKernelDispatch(t *testing.T) {
	suite := NewSharedTestSuite(t)

	// Test with GLSL (works on all platforms)
	t.Run("GLSL", func(t *testing.T) {
		suite.TestKernelDispatch(Shaders.SimpleKernelGLSL)
	})

	// Test with HLSL (works everywhere)
	t.Run("HLSL", func(t *testing.T) {
		suite.TestKernelDispatch(Shaders.SimpleKernelHLSL)
	})

	// Test with WGSL (works everywhere)
	t.Run("WGSL", func(t *testing.T) {
		suite.TestKernelDispatch(Shaders.SimpleKernelWGSL)
	})

	// MSL only on Metal
	if runtime.GOOS == "darwin" {
		t.Run("MSL", func(t *testing.T) {
			suite.TestKernelDispatch(Shaders.SimpleKernelMSL)
		})
	}
}

// Compute tests - verify actual data processing works correctly
func TestComputeCopy(t *testing.T) {
	suite := NewSharedTestSuite(t)

	// Test data copying with different shader languages
	t.Run("GLSL", func(t *testing.T) {
		suite.TestComputeCopy(Shaders.CopyKernelGLSL)
	})

	t.Run("HLSL", func(t *testing.T) {
		suite.TestComputeCopy(Shaders.CopyKernelHLSL)
	})

	t.Run("WGSL", func(t *testing.T) {
		suite.TestComputeCopy(Shaders.CopyKernelWGSL)
	})

	// MSL only on Metal
	if runtime.GOOS == "darwin" {
		t.Run("MSL", func(t *testing.T) {
			suite.TestComputeCopy(Shaders.CopyKernelMSL)
		})
	}
}

// TestCrossPlatformTranspilation verifies that shaders can be cross-compiled
func TestCrossPlatformTranspilation(t *testing.T) {
	suite := NewSharedTestSuite(t)

	tests := []struct {
		name   string
		shader string
		skip   bool
	}{
		{"GLSL_to_native", Shaders.CopyKernelGLSL, false},
		{"HLSL_to_native", Shaders.CopyKernelHLSL, false},
		{"WGSL_to_native", Shaders.CopyKernelWGSL, false},
		{"MSL_to_native", Shaders.CopyKernelMSL, runtime.GOOS != "darwin"}, // MSL only on macOS
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.skip {
				t.Skip("Platform not supported for this shader language")
			}
			suite.TestComputeCopy(tt.shader)
		})
	}
}

// TestMultipleKernels verifies that multiple kernels can coexist
func TestMultipleKernels(t *testing.T) {
	// Compile multiple kernels
	kernel1, err := Program.Compile(Shaders.CopyKernelGLSL)
	if err != nil {
		t.Fatalf("Failed to compile kernel 1: %v", err)
	}

	kernel2, err := Program.Compile(Shaders.CopyKernelHLSL)
	if err != nil {
		t.Fatalf("Failed to compile kernel 2: %v", err)
	}

	// Create test data
	input := Thought.Alloc(256)
	output1 := Thought.Alloc(256)
	output2 := Thought.Alloc(256)

	input.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = byte(i)
		}
		return data
})

	// Execute both kernels
	if err := kernel1.Dispatch([]*Reference{input}, output1, 64); err != nil {
		t.Fatalf("Kernel 1 dispatch failed: %v", err)
	}

	if err := kernel2.Dispatch([]*Reference{input}, output2, 64); err != nil {
		t.Fatalf("Kernel 2 dispatch failed: %v", err)
	}

	// Verify both produced correct results
	result1 := output1.Observe()
	result2 := output2.Observe()

	for i := 0; i < 64; i++ {
		if result1[i] != byte(i) {
			t.Errorf("Kernel 1: output[%d] = %d, want %d", i, result1[i], i)
		}
		if result2[i] != byte(i) {
			t.Errorf("Kernel 2: output[%d] = %d, want %d", i, result2[i], i)
		}
	}
}

// TestConcurrentBufferAccess tests multiple goroutines accessing buffers
func TestConcurrentBufferAccess(t *testing.T) {
	const numGoroutines = 10
	const bufferSize = 1024

	ref := Thought.Alloc(bufferSize)

	// Write from multiple goroutines
	done := make(chan bool, numGoroutines)
	for i := 0; i < numGoroutines; i++ {
		go func(val byte) {
			ref.Mutate(func(data []byte) []byte {
				data[0] = val
				return data
})
			done <- true
		}(byte(i))
	}

	// Wait for all writes
	for i := 0; i < numGoroutines; i++ {
		<-done
	}

	// Read should work without panic
	data := ref.Observe()
	if len(data) != bufferSize {
		t.Errorf("Buffer size = %d, want %d", len(data), bufferSize)
	}
}
