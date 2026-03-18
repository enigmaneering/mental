package main

import (
	"fmt"
	"runtime"

	"git.enigmaneering.org/mental"
)

func main() {
	fmt.Println("=== Device Simple Compute Example ===")

	// List available GPU devices
	fmt.Println("Available GPU devices:")
	devices := mental.List()
	for i, dev := range devices {
		fmt.Printf("  [%d] %s (%s, %s)\n", i, dev.Name, dev.API, dev.Type)
	}
	fmt.Println()

	// Show default device (device 0)
	defaultDevice := mental.Get(0)
	fmt.Printf("Using device: %s (%s)\n\n", defaultDevice.Name, defaultDevice.API)

	// Allocate input buffer (automatically freed by GC when no longer referenced)
	fmt.Println("Allocating GPU buffers...")
	const bufferSize = 256
	input := mental.Thought.Alloc(bufferSize)
	output := mental.Thought.Alloc(bufferSize)

	// Populate input with sequential values
	fmt.Println("Writing input data...")
	input.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = byte(i)
		}
		return data
	})

	// Compile a simple compute kernel that doubles values
	// Use platform-appropriate shader language
	fmt.Println("Compiling kernel...")
	var kernelSource string

	if runtime.GOOS == "darwin" {
		// Use MSL (Metal Shading Language) on macOS for native Metal support
		kernelSource = `
			#include <metal_stdlib>
			using namespace metal;

			kernel void compute_main(
				device float* input [[buffer(0)]],
				device float* output [[buffer(1)]],
				uint id [[thread_position_in_grid]]
			) {
				output[id] = input[id] * 2.0;
			}
		`
	} else {
		// Use HLSL on other platforms (automatically transpiled to native format)
		kernelSource = `
			RWStructuredBuffer<float> input : register(u0);
			RWStructuredBuffer<float> output : register(u1);

			[numthreads(256, 1, 1)]
			void main(uint3 id : SV_DispatchThreadID) {
				output[id.x] = input[id.x] * 2.0;
			}
		`
	}

	// Kernel language is automatically detected from the source
	// Kernel resources automatically freed by GC when no longer referenced
	kernel, err := mental.Program.Compile(kernelSource)
	if err != nil {
		panic(err)
	}

	// Execute kernel on GPU
	fmt.Println("Executing compute kernel...")
	err = kernel.Dispatch([]*mental.Reference{input}, output, bufferSize)
	if err != nil {
		panic(err)
	}

	// Read results
	fmt.Println("Reading results...")
	result := output.Observe()

	// Verify first 10 values
	fmt.Println("\nResults (first 10 values):")
	fmt.Println("  Input  → Output")
	for i := 0; i < 10; i++ {
		inputVal := byte(i)
		outputVal := result[i]
		fmt.Printf("  %3d    → %3d\n", inputVal, outputVal)
	}

	fmt.Println("\n=== Complete ===")
}
