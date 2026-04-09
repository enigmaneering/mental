// Example 05: Pipe (Chained Kernel Dispatch)
//
// Chains three kernels in a single GPU submission:
//   multiply (*2) → add (+10) → scale (*3)
//
// Data stays on the GPU between stages — no CPU round-trips.
//
// Run:
//
//	go build -o pipe . && ./pipe
package main

import (
	"fmt"
	"os"
	"unsafe"

	mental "git.enigmaneering.org/mental/go"
)

const N = 4

type vec4 = [N]float32

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
	if mental.DeviceCount() == 0 {
		fmt.Println("no GPU devices — skipping pipe example")
		os.Exit(0)
	}

	device := mental.DeviceGet(0)
	fmt.Printf("Device: %s (%s)\n\n", device.Name(), device.APIName())

	// Create references for each pipeline stage
	refA, _ := mental.ReferenceCreate[vec4, struct{}](mental.RelationallyOpen)
	refB, _ := mental.ReferenceCreate[vec4, struct{}](mental.RelationallyOpen)
	refC, _ := mental.ReferenceCreate[vec4, struct{}](mental.RelationallyOpen)
	refD, _ := mental.ReferenceCreate[vec4, struct{}](mental.RelationallyOpen)
	defer refA.Close()
	defer refB.Close()
	defer refC.Close()
	defer refD.Close()

	// Write input data
	input := vec4{1.0, 2.0, 3.0, 4.0}
	refA.Write((*[N * 4]byte)(unsafe.Pointer(&input))[:])
	fmt.Printf("Input:        %v\n", input)

	// Pin all references to the GPU
	refA.Pin(device)
	refB.Pin(device)
	refC.Pin(device)
	refD.Pin(device)

	// Compile three kernels
	multiplySrc := `#version 450
layout(local_size_x = 4) in;
layout(std430, binding = 0) readonly buffer A { float a[]; };
layout(std430, binding = 1) writeonly buffer B { float b[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    b[i] = a[i] * 2.0;
}
`
	addSrc := `#version 450
layout(local_size_x = 4) in;
layout(std430, binding = 0) readonly buffer A { float a[]; };
layout(std430, binding = 1) writeonly buffer B { float b[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    b[i] = a[i] + 10.0;
}
`
	scaleSrc := `#version 450
layout(local_size_x = 4) in;
layout(std430, binding = 0) readonly buffer A { float a[]; };
layout(std430, binding = 1) writeonly buffer B { float b[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    b[i] = a[i] * 3.0;
}
`

	kMultiply, err := mental.Compile(device, multiplySrc)
	if err != nil {
		fmt.Fprintf(os.Stderr, "compile multiply: %v\n", err)
		os.Exit(1)
	}
	defer kMultiply.Finalize()

	kAdd, err := mental.Compile(device, addSrc)
	if err != nil {
		fmt.Fprintf(os.Stderr, "compile add: %v\n", err)
		os.Exit(1)
	}
	defer kAdd.Finalize()

	kScale, err := mental.Compile(device, scaleSrc)
	if err != nil {
		fmt.Fprintf(os.Stderr, "compile scale: %v\n", err)
		os.Exit(1)
	}
	defer kScale.Finalize()

	fmt.Println("3 kernels compiled.")

	// Build a pipe: multiply → add → scale
	pipe, err := mental.CreatePipe(device)
	if err != nil {
		fmt.Fprintf(os.Stderr, "create pipe: %v\n", err)
		os.Exit(1)
	}
	defer pipe.Finalize()

	pipe.Add(kMultiply, []uintptr{refA.Handle()}, []uintptr{refB.Handle()}, N)
	pipe.Add(kAdd, []uintptr{refB.Handle()}, []uintptr{refC.Handle()}, N)
	pipe.Add(kScale, []uintptr{refC.Handle()}, []uintptr{refD.Handle()}, N)

	fmt.Println("Pipe built: 3 dispatches queued.")

	// Execute — one GPU submission
	if err := pipe.Execute(); err != nil {
		fmt.Fprintf(os.Stderr, "pipe execute: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Pipe executed.")

	// Read back the final result — the only read from the GPU.
	// Intermediate buffers (refB, refC) were never read;
	// data stayed on-device between stages.
	var result vec4
	refD.Read((*[N * 4]byte)(unsafe.Pointer(&result))[:])

	fmt.Printf("\nPipeline:     (A * 2 + 10) * 3\n")
	fmt.Printf("Result:       %v\n", result)

	// Verify: D[i] = (A[i] * 2 + 10) * 3
	var expected vec4
	ok := true
	for i := 0; i < N; i++ {
		expected[i] = (input[i]*2 + 10) * 3
		if result[i] != expected[i] {
			fmt.Printf("  mismatch at [%d]: got %.0f, expected %.0f\n", i, result[i], expected[i])
			ok = false
		}
	}
	fmt.Printf("Expected:     %v\n", expected)

	if ok {
		fmt.Println("\n✓ Pipe correct!")
	} else {
		fmt.Println("\n✗ Mismatch!")
		os.Exit(1)
	}

	os.Exit(0)
}
