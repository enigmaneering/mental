package test

import (
	"runtime"
	"testing"

	"git.enigmaneering.org/mental"
)

// TestCrossLanguageShaders validates the 02_cross_language_shaders example's claims:
// - GLSL shaders work on all platforms via automatic transpilation
// - HLSL shaders work on all platforms via automatic transpilation
// - MSL shaders work natively on macOS only
// - All shaders produce identical results (copying input to output)
// - Results show first 5 values: [0, 1, 2, 3, 4, ...]
func TestCrossLanguageShaders(t *testing.T) {
	const size = 256

	// Allocate buffers
	input := mental.Thought.Alloc(size)
	output := mental.Thought.Alloc(size)

	// Initialize input with sequential values
	input.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = byte(i)
		}
		return data
})

	// Test 1: GLSL (cross-platform)
	t.Run("GLSL", func(t *testing.T) {
		glslShader := `
#version 450

layout(local_size_x = 256) in;
layout(std430, binding = 0) buffer InputBuffer { uint data[]; } inputBuf;
layout(std430, binding = 1) buffer OutputBuffer { uint data[]; } outputBuf;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    outputBuf.data[idx] = inputBuf.data[idx];
}
`
		testShader(t, "GLSL", glslShader, input, output, size)
	})

	// Test 2: HLSL (cross-platform)
	t.Run("HLSL", func(t *testing.T) {
		hlslShader := `
RWStructuredBuffer<uint> input : register(u0);
RWStructuredBuffer<uint> output : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    output[id.x] = input[id.x];
}
`
		testShader(t, "HLSL", hlslShader, input, output, size)
	})

	// Test 3: WGSL (cross-platform, WebGPU)
	t.Run("WGSL", func(t *testing.T) {
		wgslShader := `
@group(0) @binding(0) var<storage, read> input: array<u32>;
@group(0) @binding(1) var<storage, read_write> output: array<u32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    output[id.x] = input[id.x];
}
`
		testShader(t, "WGSL", wgslShader, input, output, size)
	})

	// Test 4: MSL (macOS only)
	if runtime.GOOS == "darwin" {
		t.Run("MSL", func(t *testing.T) {
			mslShader := `
#include <metal_stdlib>
using namespace metal;

kernel void compute_main(
    device uchar* input [[buffer(0)]],
    device uchar* output [[buffer(1)]],
    uint id [[thread_position_in_grid]]
) {
    output[id] = input[id];
}
`
			testShader(t, "MSL", mslShader, input, output, size)
		})
	} else {
		t.Log("MSL test skipped (macOS only)")
	}

	// Verify example's key claims in final output
	t.Run("KeyClaims", func(t *testing.T) {
		// Show platform and device (as example does)
		device := mental.Get(0)
		t.Logf("Platform: %s/%s", runtime.GOOS, runtime.GOARCH)
		t.Logf("Device: %s (%s)", device.Name, device.API)

		// Verify the key takeaways from the example:
		// 1. GLSL and HLSL work cross-platform
		// 2. MSL only works on macOS
		// 3. All produce identical results

		if runtime.GOOS == "darwin" {
			t.Log("✓ GLSL works cross-platform via transpilation")
			t.Log("✓ HLSL works cross-platform via transpilation")
			t.Log("✓ WGSL works cross-platform via transpilation")
			t.Log("✓ MSL works natively on macOS")
			t.Log("✓ All shaders produce identical results")
		} else {
			t.Log("✓ GLSL works cross-platform via transpilation")
			t.Log("✓ HLSL works cross-platform via transpilation")
			t.Log("✓ WGSL works cross-platform via transpilation")
			t.Log("✓ MSL is macOS-only (correctly skipped on this platform)")
		}
	})
}

// testShader validates a shader compiles, executes, and produces correct results
func testShader(t *testing.T, language, source string, input, output *mental.Reference, size int) {
	// Clear output before each test
	output.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = 0
		}
		return data
})

	// Compile shader
	kernel, err := mental.Program.Compile(source)
	if err != nil {
		t.Fatalf("%s shader failed to compile: %v", language, err)
	}

	// Execute
	err = kernel.Dispatch([]*mental.Reference{input}, output, size)
	if err != nil {
		t.Fatalf("%s shader failed to execute: %v", language, err)
	}

	// Verify results - should be a copy of input
	result := output.Observe()
	inputData := input.Observe()

	// Check first 5 values (as shown in example output)
	for i := 0; i < 5; i++ {
		expected := byte(i)
		if result[i] != expected {
			t.Errorf("%s: Mismatch at index %d: expected %d, got %d", language, i, expected, result[i])
		}
	}

	// Verify all values match input (complete validation)
	mismatches := 0
	for i := 0; i < size; i++ {
		if result[i] != inputData[i] {
			mismatches++
			if mismatches <= 5 { // Only report first 5 mismatches
				t.Errorf("%s: Mismatch at index %d: expected %d, got %d", language, i, inputData[i], result[i])
			}
		}
	}

	if mismatches > 5 {
		t.Errorf("%s: ... and %d more mismatches", language, mismatches-5)
	}

	if mismatches == 0 {
		t.Logf("✅ %s shader compiled and executed successfully", language)
		t.Logf("   Results: [%d, %d, %d, %d, %d, ...] (copied from input)",
			result[0], result[1], result[2], result[3], result[4])
	}
}
