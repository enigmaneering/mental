//go:build windows

package mental

/*
#cgo LDFLAGS: -ld3d12 -ldxgi
#include "backend_d3d12_windows.h"
#include <stdlib.h>
#include <string.h>
*/
import "C"
import (
	"fmt"
	"runtime"
	"unsafe"

	"git.enigmaneering.org/mental/api"
	"git.enigmaneering.org/mental/language"
	"git.enigmaneering.org/mental/transpile"
)

// enumerateD3D12Devices returns all Direct3D 12 capable devices.
func enumerateD3D12Devices() ([]Info, error) {
	var count C.int
	devicesPtr := C.d3d12_enumerate_devices(&count)
	if devicesPtr == nil || count == 0 {
		return nil, fmt.Errorf("no D3D12 devices found")
	}
	defer C.d3d12_free_device_list(devicesPtr)

	// Convert C array to Go slice
	devices := (*[1 << 30]C.D3D12DeviceInfo)(devicesPtr)[:count:count]
	result := make([]Info, count)

	for i := 0; i < int(count); i++ {
		result[i] = Info{
			Index: i,
			Name:  C.GoString(&devices[i].name[0]),
			API:   api.Direct3D12,
		}

		// Map device type
		switch devices[i].device_type {
		case 0:
			result[i].Type = Discrete
		case 1:
			result[i].Type = Integrated
		case 2:
			result[i].Type = Virtual
		default:
			result[i].Type = Other
		}
	}

	return result, nil
}

// d3d12Backend implements the backend interface using Direct3D 12 API.
type d3d12Backend struct {
	device unsafe.Pointer
	devInfo Info
}

// newD3D12Backend creates a new Direct3D 12 backend for the specified device.
func newD3D12Backend(deviceIndex int) (backend, error) {
	// Get device info for this index
	devices, err := enumerateD3D12Devices()
	if err != nil {
		return nil, err
	}
	if deviceIndex >= len(devices) {
		return nil, fmt.Errorf("device index %d out of range (have %d devices)", deviceIndex, len(devices))
	}

	device := C.d3d12_create_device(C.int(deviceIndex))
	if device == nil {
		return nil, fmt.Errorf("failed to create D3D12 device")
	}

	b := &d3d12Backend{
		device:  device,
		devInfo: devices[deviceIndex],
	}

	runtime.SetFinalizer(b, func(b *d3d12Backend) {
		if b.device != nil {
			C.d3d12_destroy_device(b.device)
		}
	})

	return b, nil
}

// alloc allocates GPU memory using D3D12 resources.
func (d *d3d12Backend) alloc(size int) *Reference {
	buffer := C.d3d12_alloc_buffer(d.device, C.size_t(size))
	if buffer == nil {
		return nil
	}

	ref := &Reference{
		size:    size,
		handle:  buffer,
		backend: d,
	}

	// Note: Finalizer is set by AllocRef() in api.go, not here
	return ref
}

// free releases D3D12 resource.
func (d *d3d12Backend) free(ref *Reference) {
	if ref.handle != nil {
		C.d3d12_free_buffer(ref.handle)
		ref.handle = nil
	}
}

// read copies data from D3D12 buffer to CPU memory.
func (d *d3d12Backend) read(ref *Reference, dest []byte) {
	if len(dest) == 0 {
		return
	}

	// Copy from DEFAULT to READBACK and get mapped pointer
	ptr := C.d3d12_read_buffer(ref.handle)
	if ptr != nil {
		C.memcpy(unsafe.Pointer(&dest[0]), ptr, C.size_t(len(dest)))
		// Unmap the readback buffer after reading
		// (readbackBuffer is remapped on each read call)
	}
}

// write copies data from CPU memory to D3D12 buffer.
func (d *d3d12Backend) write(ref *Reference, src []byte) {
	if len(src) == 0 {
		return
	}

	ptr := C.d3d12_get_buffer_pointer(ref.handle)
	if ptr != nil {
		C.memcpy(ptr, unsafe.Pointer(&src[0]), C.size_t(len(src)))
		C.d3d12_flush_buffer(ref.handle)
	}
}

// clone creates a copy of a D3D12 buffer.
func (d *d3d12Backend) clone(ref *Reference) *Reference {
	newRef := d.alloc(ref.size)
	if newRef == nil {
		return nil
	}

	// Read from source and write to destination
	srcPtr := C.d3d12_read_buffer(ref.handle)
	dstPtr := C.d3d12_get_buffer_pointer(newRef.handle)
	if srcPtr != nil && dstPtr != nil {
		C.memcpy(dstPtr, srcPtr, C.size_t(ref.size))
		C.d3d12_flush_buffer(newRef.handle)
	}

	return newRef
}

// compileShader compiles a shader for Direct3D 12.
// Automatically detects the source language and transpiles to DXIL via HLSL.
func (d *d3d12Backend) compileShader(source string) (*Kernel, error) {
	// Detect source language
	lang := language.Detect(source)

	var dxilBytecode []byte
	var err error

	// Transpile to DXIL
	switch lang {
	case language.HLSL:
		// Compile HLSL directly to DXIL
		dxilBytecode, err = transpile.ToDXIL(source, language.HLSL)
	case language.GLSL:
		// GLSL → SPIRV → HLSL → DXIL
		dxilBytecode, err = transpile.ToDXIL(source, language.GLSL)
	case language.MSL:
		return nil, fmt.Errorf("MSL not supported on D3D12 (use GLSL or HLSL)")
	default:
		return nil, fmt.Errorf("unsupported shader language: %v", lang)
	}

	if err != nil {
		return nil, fmt.Errorf("shader transpilation failed: %w", err)
	}

	// For now, assume 2 buffers (input, output) - we'll refine this
	bufferCount := 2

	var errMsg *C.char
	shader := C.d3d12_compile_shader(
		d.device,
		unsafe.Pointer(&dxilBytecode[0]),
		C.size_t(len(dxilBytecode)),
		C.int(bufferCount),
		&errMsg,
	)

	if shader == nil {
		if errMsg != nil {
			defer C.free(unsafe.Pointer(errMsg))
			return nil, fmt.Errorf("D3D12 shader compilation failed: %s", C.GoString(errMsg))
		}
		return nil, fmt.Errorf("D3D12 shader compilation failed")
	}

	kernel := &Kernel{
		handle:   shader,
		backend:  d,
		language: lang,
	}

	// Note: Finalizer is set by CompileKernel() in api.go, not here

	return kernel, nil
}

// freeShader releases D3D12 shader resources.
func (d *d3d12Backend) freeShader(kernel *Kernel) {
	if kernel.handle != nil {
		C.d3d12_free_shader(kernel.handle)
		kernel.handle = nil
	}
}

// dispatch executes a compute shader on Direct3D 12.
func (d *d3d12Backend) dispatch(kernel *Kernel, inputs []*Reference, output *Reference, workSize int) error {
	// Combine inputs and output into single buffer array
	allBuffers := append(inputs, output)
	bufferHandles := make([]unsafe.Pointer, len(allBuffers))
	for i, ref := range allBuffers {
		bufferHandles[i] = ref.handle
	}

	// Calculate workgroups (assuming workgroup size of 64)
	workgroupSize := 64
	numWorkgroups := (workSize + workgroupSize - 1) / workgroupSize

	C.d3d12_dispatch_compute(
		d.device,
		kernel.handle,
		(*unsafe.Pointer)(unsafe.Pointer(&bufferHandles[0])),
		C.int(len(bufferHandles)),
		C.int(numWorkgroups),
		1,
		1,
	)

	return nil
}

// deviceInfo returns information about the D3D12 device.
func (d *d3d12Backend) deviceInfo() Info {
	return d.devInfo
}
