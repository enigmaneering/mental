// Example 03: GPU Compute via Manifest
//
// Demonstrates GPU compute using the manifest system:
//   1. Create a manifest bound to the GPU
//   2. Describe input thoughts (data pinned to GPU automatically)
//   3. Compile and dispatch a compute shader
//   4. Observe the output
//
// Run:
//
//	go build -o gpu-compute . && ./gpu-compute
package main

import (
	"fmt"
	"os"
	"unsafe"

	mental "git.enigmaneering.org/mental/go"
)

const vectorSize = 4

func main() {
	mental.On(func(e mental.Epiphany) {
		switch e.(type) {
		case mental.EpiphanyInception:
			run()
		default:
		}
	})
}

func run() {
	fmt.Println(mental.GetState())

	if mental.DeviceCount() == 0 {
		fmt.Println("no GPU devices — skipping compute example")
		os.Exit(0)
	}

	device := mental.DeviceGet(0)
	fmt.Printf("using device: %s (%s)\n\n", device.Name(), device.APIName())

	manifest := mental.CreateManifest("compute", device)
	if manifest == nil {
		fmt.Fprintf(os.Stderr, "failed to create manifest\n")
		os.Exit(1)
	}
	defer manifest.Close()

	inputA := [vectorSize]float32{1.0, 2.0, 3.0, 4.0}
	inputB := [vectorSize]float32{10.0, 20.0, 30.0, 40.0}
	output := [vectorSize]float32{0, 0, 0, 0}

	mental.Describe(manifest, "input-a", &inputA)
	mental.Describe(manifest, "input-b", &inputB)
	mental.Describe(manifest, "output", &output)

	fmt.Printf("input-a:  %v\n", inputA)
	fmt.Printf("input-b:  %v\n", inputB)

	refA := mental.Recall[[vectorSize]float32](manifest, "input-a")
	refB := mental.Recall[[vectorSize]float32](manifest, "input-b")
	refOut := mental.Recall[[vectorSize]float32](manifest, "output")

	if refA == nil || refB == nil || refOut == nil {
		fmt.Fprintf(os.Stderr, "failed to recall references\n")
		os.Exit(1)
	}

	source := `#version 450
layout(local_size_x = 4) in;
layout(std430, binding = 0) readonly buffer A { float a[]; };
layout(std430, binding = 1) readonly buffer B { float b[]; };
layout(std430, binding = 2) writeonly buffer C { float c[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    c[i] = a[i] + b[i];
}
`
	kernel, err := mental.Compile(device, source)
	if err != nil {
		fmt.Fprintf(os.Stderr, "compile failed: %v\n", err)
		os.Exit(1)
	}
	defer kernel.Finalize()

	fmt.Println("kernel compiled successfully")

	if err = mental.Dispatch(kernel,
		[]uintptr{refA.Handle(), refB.Handle()},
		[]uintptr{refOut.Handle()}, vectorSize); err != nil {
		fmt.Fprintf(os.Stderr, "dispatch failed: %v\n", err)
		os.Exit(1)
	}

	var result [vectorSize]float32
	refOut.Read((*[vectorSize * 4]byte)(unsafe.Pointer(&result))[:])

	fmt.Printf("output:   %v\n", result)
	fmt.Printf("expected: [11 22 33 44]\n")

	expected := [vectorSize]float32{11.0, 22.0, 33.0, 44.0}
	if result == expected {
		fmt.Println("\n✓ GPU compute successful!")
	} else {
		fmt.Println("\n✗ result mismatch")
		os.Exit(1)
	}

	os.Exit(0)
}
