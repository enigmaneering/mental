package mental

import (
	"bytes"
	"runtime"
	"sync"
	"testing"
	"time"

	"git.enigmaneering.org/mental/language"
)

// TestShaders contains the same test kernels implemented in all supported languages.
// Each backend should be able to compile and execute any of these via transpilation.
type TestShaders struct {
	// SimpleKernel - a no-op kernel for basic compilation testing
	SimpleKernelGLSL string
	SimpleKernelHLSL string
	SimpleKernelMSL  string
	SimpleKernelWGSL string

	// CopyKernel - copies input to output for data verification
	CopyKernelGLSL string
	CopyKernelHLSL string
	CopyKernelMSL  string
	CopyKernelWGSL string

	// DoubleKernel - doubles each element for computation testing
	DoubleKernelGLSL string
	DoubleKernelHLSL string
	DoubleKernelMSL  string
	DoubleKernelWGSL string
}

var Shaders = TestShaders{
	// Simple kernels
	SimpleKernelGLSL: `
#version 450
layout(local_size_x = 256) in;
void main() {}
`,
	SimpleKernelHLSL: `
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {}
`,
	SimpleKernelMSL: `
#include <metal_stdlib>
using namespace metal;
kernel void main_kernel(uint id [[thread_position_in_grid]]) {}
`,
	SimpleKernelWGSL: `
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {}
`,

	// Copy kernels
	CopyKernelGLSL: `
#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) buffer InputBuffer { uint data[]; } inputBuf;
layout(std430, binding = 1) buffer OutputBuffer { uint data[]; } outputBuf;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    outputBuf.data[idx] = inputBuf.data[idx];
}
`,
	CopyKernelHLSL: `
RWStructuredBuffer<uint> input : register(u0);
RWStructuredBuffer<uint> output : register(u1);
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    output[id.x] = input[id.x];
}
`,
	CopyKernelMSL: `
#include <metal_stdlib>
using namespace metal;
kernel void copy_kernel(
    device const uint *input [[buffer(0)]],
    device uint *output [[buffer(1)]],
    uint id [[thread_position_in_grid]])
{
    output[id] = input[id];
}
`,
	CopyKernelWGSL: `
@group(0) @binding(0) var<storage, read> input: array<u32>;
@group(0) @binding(1) var<storage, read_write> output: array<u32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    output[id.x] = input[id.x];
}
`,

	// Double kernels
	DoubleKernelGLSL: `
#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) buffer InputBuffer { float data[]; } input;
layout(std430, binding = 1) buffer OutputBuffer { float data[]; } output;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    output.data[idx] = input.data[idx] * 2.0;
}
`,
	DoubleKernelHLSL: `
RWStructuredBuffer<float> input : register(u0);
RWStructuredBuffer<float> output : register(u1);
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    output[id.x] = input[id.x] * 2.0;
}
`,
	DoubleKernelMSL: `
#include <metal_stdlib>
using namespace metal;
kernel void double_kernel(
    device const float *input [[buffer(0)]],
    device float *output [[buffer(1)]],
    uint id [[thread_position_in_grid]])
{
    output[id] = input[id] * 2.0;
}
`,
	DoubleKernelWGSL: `
@group(0) @binding(0) var<storage, read> input: array<f32>;
@group(0) @binding(1) var<storage, read_write> output: array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    output[id.x] = input[id.x] * 2.0;
}
`,
}

// SharedTestSuite contains all the core device functionality tests.
// Each platform's test file should call these functions with appropriate
// shader sources for their backend.
type SharedTestSuite struct {
	t *testing.T
}

func NewSharedTestSuite(t *testing.T) *SharedTestSuite {
	return &SharedTestSuite{t: t}
}

// TestDeviceEnumeration tests device discovery
func (s *SharedTestSuite) TestDeviceEnumeration() {
	devices := List()
	if len(devices) == 0 {
		s.t.Fatal("no devices found")
	}

	// Verify device indices are sequential
	for i, dev := range devices {
		if dev.Index != i {
			s.t.Errorf("device %d: expected index %d, got %d", i, i, dev.Index)
		}
		if dev.Name == "" {
			s.t.Errorf("device %d: empty name", i)
		}
	}

	s.t.Logf("Found %d device(s)", len(devices))
	for i, dev := range devices {
		s.t.Logf("  [%d] %s (%s, %s)", i, dev.Name, dev.Type, dev.API)
	}
}

// TestDeviceGet tests device retrieval
func (s *SharedTestSuite) TestDeviceGet() {
	devices := List()

	dev0 := Get(0)
	if dev0.Index != 0 {
		s.t.Errorf("Get(0): expected index 0, got %d", dev0.Index)
	}

	// Only test Get(1) if we have multiple devices
	if len(devices) > 1 {
		dev1 := Get(1)
		if dev1.Index != 1 {
			s.t.Errorf("Get(1): expected index 1, got %d", dev1.Index)
		}
	}
}

// TestBufferAllocation tests basic buffer creation
func (s *SharedTestSuite) TestBufferAllocation() {
	ref := Thought.Alloc(256)
	if ref == nil {
		s.t.Fatal("Alloc returned nil")
	}
	if ref.size != 256 {
		s.t.Errorf("expected size 256, got %d", ref.size)
	}
	if !ref.created {
		s.t.Error("created flag should be true")
	}
}

// TestBufferReadWrite tests buffer I/O operations
func (s *SharedTestSuite) TestBufferReadWrite() {
	ref := Thought.Alloc(32)
	writeData := make([]byte, 32)
	for i := range writeData {
		writeData[i] = byte(i * 3)
	}

	ref.Write(writeData)

	readData := make([]byte, 32)
	ref.Read(readData)

	if !bytes.Equal(readData, writeData) {
		s.t.Errorf("read/write mismatch")
	}
}

// TestBufferClone tests buffer cloning
func (s *SharedTestSuite) TestBufferClone() {
	ref := Thought.Alloc(16)
	ref.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = byte(i + 100)
		}
		return data
})

	clone := ref.Clone()

	// Verify clone has same data
	cloneData := clone.Observe()
	origData := ref.Observe()
	if !bytes.Equal(cloneData, origData) {
		s.t.Error("clone data mismatch")
	}

	// Mutate clone and verify original unchanged
	clone.Mutate(func(data []byte) []byte {
		data[0] = 255
		return data
})

	cloneData = clone.Observe()
	origData = ref.Observe()
	if cloneData[0] != 255 || origData[0] == 255 {
		s.t.Error("clone isolation failed")
	}
}

// TestObservablePattern tests concurrent read/write access
func (s *SharedTestSuite) TestObservablePattern() {
	ref := Thought.Alloc(64)
	ref.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = 0
		}
		return data
})

	var wg sync.WaitGroup
	observations := make([]int, 0, 10)
	var obsMu sync.Mutex

	// Observer: reads periodically
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			time.Sleep(10 * time.Millisecond)
			data := ref.Observe()
			sum := 0
			for _, v := range data {
				sum += int(v)
			}
			obsMu.Lock()
			observations = append(observations, sum)
			obsMu.Unlock()
		}
	}()

	// Mutator: increments values periodically
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 10; i++ {
			time.Sleep(5 * time.Millisecond)
			ref.Mutate(func(data []byte) []byte {
				for j := range data {
					data[j]++
				}
				return data
})
		}
	}()

	wg.Wait()

	// Verify observations increased
	if len(observations) < 2 {
		s.t.Fatal("not enough observations")
	}
	for i := 1; i < len(observations); i++ {
		if observations[i] < observations[i-1] {
			s.t.Error("observations did not increase monotonically")
		}
	}
}

// TestShaderCompilation tests compiling a shader in the given language
func (s *SharedTestSuite) TestShaderCompilation(source string, expectedLang language.Language) {
	kernel, err := Program.Compile(source)
	if err != nil {
		s.t.Fatalf("Compile failed: %v", err)
	}
	if kernel == nil {
		s.t.Fatal("Compile returned nil kernel")
	}
	if !kernel.created {
		s.t.Error("kernel created flag should be true")
	}

	// Language detection might differ if transpilation occurred
	detectedLang := language.Detect(source)
	s.t.Logf("Source language: %s, Detected: %s, Kernel: %s",
		expectedLang, detectedLang, kernel.language)
}

// TestKernelDispatch tests executing a simple kernel
func (s *SharedTestSuite) TestKernelDispatch(source string) {
	kernel, err := Program.Compile(source)
	if err != nil {
		s.t.Fatalf("Compile failed: %v", err)
	}

	input := Thought.Alloc(256)
	output := Thought.Alloc(256)

	err = kernel.Dispatch([]*Reference{input}, output, 256)
	if err != nil {
		s.t.Fatalf("Dispatch failed: %v", err)
	}
}

// TestComputeCopy tests a kernel that copies data
func (s *SharedTestSuite) TestComputeCopy(source string) {
	input := Thought.Alloc(256)
	output := Thought.Alloc(256)

	// Populate input
	input.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = byte(i)
		}
		return data
})

	kernel, err := Program.Compile(source)
	if err != nil {
		s.t.Fatalf("Compile failed: %v", err)
	}

	err = kernel.Dispatch([]*Reference{input}, output, 64) // 64 uint32s = 256 bytes
	if err != nil {
		s.t.Fatalf("Dispatch failed: %v", err)
	}

	// Verify results
	inputData := input.Observe()
	outputData := output.Observe()
	if !bytes.Equal(inputData, outputData) {
		s.t.Error("copy kernel did not produce correct output")
	}
}

// TestBufferResize tests automatic buffer reallocation
func (s *SharedTestSuite) TestBufferResize() {
	// Start with small buffer
	ref := Thought.Alloc(10)
	ref.Write([]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10})

	// Test 1: Grow via Mutate
	ref.Mutate(func(data []byte) []byte {
		return append(data, 11, 12, 13, 14, 15)
	})
	if ref.Size() != 15 {
		s.t.Errorf("Expected size 15, got %d", ref.Size())
	}
	data := ref.Observe()
	if len(data) != 15 || data[14] != 15 {
		s.t.Error("Grow via Mutate failed")
	}

	// Test 2: Shrink via Mutate
	ref.Mutate(func(data []byte) []byte {
		return data[:5]
	})
	if ref.Size() != 5 {
		s.t.Errorf("Expected size 5, got %d", ref.Size())
	}
	data = ref.Observe()
	if len(data) != 5 || data[4] != 5 {
		s.t.Error("Shrink via Mutate failed")
	}

	// Test 3: Grow via Write
	ref.Write([]byte{100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110})
	if ref.Size() != 11 {
		s.t.Errorf("Expected size 11, got %d", ref.Size())
	}
	data = ref.Observe()
	if len(data) != 11 || data[0] != 100 {
		s.t.Error("Grow via Write failed")
	}

	// Test 4: Shrink via Write
	ref.Write([]byte{200, 201, 202})
	if ref.Size() != 3 {
		s.t.Errorf("Expected size 3, got %d", ref.Size())
	}
	data = ref.Observe()
	if len(data) != 3 || data[2] != 202 {
		s.t.Error("Shrink via Write failed")
	}

	// Test 5: Complex transformation
	ref.Mutate(func(data []byte) []byte {
		result := make([]byte, len(data)*2)
		for i, v := range data {
			result[i*2] = v
			result[i*2+1] = v + 10
		}
		return result
	})
	if ref.Size() != 6 {
		s.t.Errorf("Expected size 6, got %d", ref.Size())
	}
	data = ref.Observe()
	if len(data) != 6 || data[0] != 200 || data[1] != 210 {
		s.t.Error("Complex transformation failed")
	}
}

// TestFinalizers tests garbage collection cleanup
func (s *SharedTestSuite) TestFinalizers() {
	func() {
		ref := Thought.Alloc(256)
		_ = ref
	}()

	runtime.GC()
	time.Sleep(100 * time.Millisecond)
	runtime.GC()

	s.t.Log("Finalizer test completed")
}

// TestErrorHandling tests error cases
func (s *SharedTestSuite) TestErrorHandling() {
	// Test write after free
	ref := Thought.Alloc(64)
	ref.free()

	defer func() {
		if r := recover(); r == nil {
			s.t.Error("Write after free should panic")
		}
	}()

	ref.Write([]byte{1, 2, 3})
}
