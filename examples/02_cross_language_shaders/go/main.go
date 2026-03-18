package main

import (
	"fmt"
	"runtime"

	"git.enigmaneering.org/mental"
)

// This example demonstrates cross-language shader compilation.
// You can write shaders in GLSL, HLSL, WGSL, or MSL and they will be
// automatically transpiled to the native format for your GPU.
func main() {
	fmt.Println("=== Cross-Language Shader Compilation Example ===")
	fmt.Println()

	// Show current platform and device
	fmt.Printf("Platform: %s/%s\n", runtime.GOOS, runtime.GOARCH)
	device := mental.Get(0)
	fmt.Printf("Device: %s (%s)\n", device.Name, device.API)
	fmt.Println()

	// Allocate buffers
	const size = 256
	input := mental.Thought.Alloc(size)
	output := mental.Thought.Alloc(size)

	// Initialize input with sequential values
	input.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = byte(i)
		}
		return data
})

	// Example 1: GLSL (cross-platform, transpiled to native format)
	fmt.Println("--- Testing GLSL Shader ---")
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
	testShader("GLSL", glslShader, input, output, size)

	// Example 2: HLSL (cross-platform, transpiled to native format)
	fmt.Println("--- Testing HLSL Shader ---")
	hlslShader := `
RWStructuredBuffer<uint> input : register(u0);
RWStructuredBuffer<uint> output : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    output[id.x] = input[id.x];
}
`
	testShader("HLSL", hlslShader, input, output, size)

	// Example 3: WGSL (cross-platform, WebGPU Shading Language)
	fmt.Println("--- Testing WGSL Shader ---")
	wgslShader := `
@group(0) @binding(0) var<storage, read> input: array<u32>;
@group(0) @binding(1) var<storage, read_write> output: array<u32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    output[id.x] = input[id.x];
}
`
	testShader("WGSL", wgslShader, input, output, size)

	// Example 4: MSL (macOS only - cannot transpile FROM MSL)
	if runtime.GOOS == "darwin" {
		fmt.Println("--- Testing MSL Shader (macOS native) ---")
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
		testShader("MSL", mslShader, input, output, size)
	} else {
		fmt.Println("--- MSL Shader (skipped - macOS only) ---")
		fmt.Println("MSL is Apple's Metal Shading Language and only works on macOS")
	}

	fmt.Println()
	fmt.Println("=== Complete ===")
	fmt.Println()
	fmt.Println("Key Takeaway:")
	fmt.Println("  - GLSL, HLSL, and WGSL work on all platforms via automatic transpilation")
	fmt.Println("  - MSL only works on macOS (Metal's native language)")
	fmt.Println("  - All shaders produce identical results despite different syntax")
}

func testShader(language, source string, input, output *mental.Reference, size int) {
	// Compile shader
	kernel, err := mental.Program.Compile(source)
	if err != nil {
		fmt.Printf("  ❌ Failed to compile: %v\n\n", err)
		return
	}

	// Execute
	err = kernel.Dispatch([]*mental.Reference{input}, output, size)
	if err != nil {
		fmt.Printf("  ❌ Failed to execute: %v\n\n", err)
		return
	}

	// Verify results (check first 5 values - should match input since we're just copying)
	result := output.Observe()
	allCorrect := true
	for i := 0; i < 5; i++ {
		expected := byte(i)
		if result[i] != expected {
			allCorrect = false
			fmt.Printf("  ❌ Mismatch at index %d: expected %d, got %d\n", i, expected, result[i])
			break
		}
	}

	if allCorrect {
		fmt.Printf("  ✅ %s shader compiled and executed successfully\n", language)
		fmt.Printf("     Results: [%d, %d, %d, %d, %d, ...] (copied from input)\n",
			result[0], result[1], result[2], result[3], result[4])
	} else {
		fmt.Printf("  ❌ Results incorrect\n")
	}
	fmt.Println()
}
