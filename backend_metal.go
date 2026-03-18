//go:build darwin

package mental

/*
#cgo LDFLAGS: -framework Metal -framework Foundation
#include <stdlib.h>
#include <string.h>

int metal_enumerate_devices(void ***devices_out);
const char* metal_device_name(void *device);
int metal_device_is_low_power(void *device);
int metal_device_is_headless(void *device);
void metal_device_release(void *device);
void* metal_create_command_queue(void *device);
void metal_release_command_queue(void *queue);
void* metal_alloc_buffer(void *device, size_t size);
void metal_release_buffer(void *buffer);
void* metal_buffer_contents(void *buffer);
size_t metal_buffer_length(void *buffer);
void* metal_compile_shader(void *device, const char *source, char **error_out);
void metal_release_shader(void *function);
void* metal_create_pipeline_state(void *device, void *function, char **error_out);
void metal_release_pipeline_state(void *pipelineState);
int metal_dispatch_compute(void *queue, void *pipelineState, void **buffers, int bufferCount, int workSize, char **error_out);
*/
import "C"

import (
	"fmt"
	"runtime"
	"sync"
	"unsafe"

	"git.enigmaneering.org/mental/api"
	"git.enigmaneering.org/mental/language"
)

// Store device handles globally for backend creation
var (
	metalDeviceHandles   []unsafe.Pointer
	metalDeviceHandlesMu sync.Mutex
)

// enumerateMetalDevices returns all Metal-capable devices on macOS.
func enumerateMetalDevices() ([]Info, error) {
	var devicesPtr **C.void
	count := int(C.metal_enumerate_devices((**unsafe.Pointer)(unsafe.Pointer(&devicesPtr))))

	if count == 0 {
		return nil, fmt.Errorf("no Metal devices found")
	}

	devices := make([]Info, count)
	devicePtrs := (*[1 << 30]*C.void)(unsafe.Pointer(devicesPtr))[:count:count]

	metalDeviceHandlesMu.Lock()
	metalDeviceHandles = make([]unsafe.Pointer, count)
	metalDeviceHandlesMu.Unlock()

	for i := 0; i < count; i++ {
		devicePtr := devicePtrs[i]
		namePtr := C.metal_device_name(unsafe.Pointer(devicePtr))
		name := C.GoString(namePtr)
		C.free(unsafe.Pointer(namePtr)) // Free the strdup'd string
		isLowPower := C.metal_device_is_low_power(unsafe.Pointer(devicePtr)) != 0
		isHeadless := C.metal_device_is_headless(unsafe.Pointer(devicePtr)) != 0

		deviceType := Discrete
		if isLowPower {
			deviceType = Integrated
		}
		if isHeadless {
			deviceType = Virtual // Headless GPUs are often used for compute/virtual
		}

		devices[i] = Info{
			Index: i,
			Name:  name,
			Type:  deviceType,
			API:   api.Metal,
		}

		// Store device handle for backend creation
		metalDeviceHandlesMu.Lock()
		metalDeviceHandles[i] = unsafe.Pointer(devicePtr)
		metalDeviceHandlesMu.Unlock()
	}

	C.free(unsafe.Pointer(devicesPtr))
	return devices, nil
}

// metalBackend implements the backend interface using Apple's Metal API.
type metalBackend struct {
	deviceIndex int
	devInfo     Info
	device      unsafe.Pointer // id<MTLDevice>
	queue       unsafe.Pointer // id<MTLCommandQueue>
}

// newMetalBackend creates a new Metal backend for the specified device.
func newMetalBackend(deviceIndex int) (backend, error) {
	metalDeviceHandlesMu.Lock()
	if deviceIndex >= len(metalDeviceHandles) {
		metalDeviceHandlesMu.Unlock()
		return nil, fmt.Errorf("invalid device index: %d", deviceIndex)
	}
	deviceHandle := metalDeviceHandles[deviceIndex]
	metalDeviceHandlesMu.Unlock()

	queue := C.metal_create_command_queue(deviceHandle)
	if queue == nil {
		return nil, fmt.Errorf("failed to create Metal command queue")
	}

	mb := &metalBackend{
		deviceIndex: deviceIndex,
		device:      deviceHandle,
		queue:       queue,
	}

	runtime.SetFinalizer(mb, (*metalBackend).cleanup)
	return mb, nil
}

// cleanup releases Metal resources
func (m *metalBackend) cleanup() {
	if m.queue != nil {
		C.metal_release_command_queue(m.queue)
		m.queue = nil
	}
	// Device is released during enumeration cleanup
}

// alloc allocates GPU memory using Metal buffers.
func (m *metalBackend) alloc(size int) *Reference {
	buffer := C.metal_alloc_buffer(m.device, C.size_t(size))
	if buffer == nil {
		panic("Metal: failed to allocate buffer")
	}

	return &Reference{
		backend: m,
		handle:  buffer,
		size:    size,
	}
}

// free releases Metal buffer.
func (m *metalBackend) free(ref *Reference) {
	if ref.handle != nil {
		C.metal_release_buffer(ref.handle)
	}
}

// read copies data from Metal buffer to CPU memory.
func (m *metalBackend) read(ref *Reference, dest []byte) {
	if len(dest) > ref.size {
		panic(fmt.Sprintf("Metal: read destination too large: %d > %d", len(dest), ref.size))
	}

	contents := C.metal_buffer_contents(ref.handle)
	if contents == nil {
		panic("Metal: buffer contents unavailable")
	}

	// Copy from Metal buffer to Go slice
	C.memcpy(unsafe.Pointer(&dest[0]), contents, C.size_t(len(dest)))
}

// write copies data from CPU memory to Metal buffer.
func (m *metalBackend) write(ref *Reference, src []byte) {
	if len(src) > ref.size {
		panic(fmt.Sprintf("Metal: write source too large: %d > %d", len(src), ref.size))
	}

	contents := C.metal_buffer_contents(ref.handle)
	if contents == nil {
		panic("Metal: buffer contents unavailable")
	}

	// Copy from Go slice to Metal buffer
	C.memcpy(contents, unsafe.Pointer(&src[0]), C.size_t(len(src)))
}

// clone creates a copy of a Metal buffer.
func (m *metalBackend) clone(ref *Reference) *Reference {
	newRef := m.alloc(ref.size)

	srcContents := C.metal_buffer_contents(ref.handle)
	destContents := C.metal_buffer_contents(newRef.handle)

	if srcContents == nil || destContents == nil {
		panic("Metal: buffer contents unavailable for clone")
	}

	C.memcpy(destContents, srcContents, C.size_t(ref.size))
	return newRef
}

// compileShader compiles a shader for Metal.
// Expects MSL source code - transpilation from other languages happens at a higher level.
func (m *metalBackend) compileShader(source string) (*Kernel, error) {
	lang := language.Detect(source)

	// Backend only accepts MSL - transpilation should happen before calling this
	if lang != language.MSL {
		return nil, fmt.Errorf("Metal: only MSL shaders supported (detected: %v, use CompileKernel for automatic transpilation)", lang)
	}

	// Compile the MSL source
	cSource := C.CString(source)
	defer C.free(unsafe.Pointer(cSource))

	var errorPtr *C.char
	functionHandle := C.metal_compile_shader(m.device, cSource, &errorPtr)

	if functionHandle == nil {
		if errorPtr != nil {
			errMsg := C.GoString(errorPtr)
			C.free(unsafe.Pointer(errorPtr))
			return nil, fmt.Errorf("Metal shader compilation failed: %s", errMsg)
		}
		return nil, fmt.Errorf("Metal shader compilation failed: unknown error")
	}

	// Create pipeline state
	var pipelineErrorPtr *C.char
	pipelineState := C.metal_create_pipeline_state(m.device, functionHandle, &pipelineErrorPtr)

	if pipelineState == nil {
		C.metal_release_shader(functionHandle)
		if pipelineErrorPtr != nil {
			errMsg := C.GoString(pipelineErrorPtr)
			C.free(unsafe.Pointer(pipelineErrorPtr))
			return nil, fmt.Errorf("Metal pipeline state creation failed: %s", errMsg)
		}
		return nil, fmt.Errorf("Metal pipeline state creation failed: unknown error")
	}

	kernel := &Kernel{
		backend:  m,
		handle:   pipelineState,
		language: lang,
	}

	return kernel, nil
}

// freeShader releases Metal shader resources.
func (m *metalBackend) freeShader(kernel *Kernel) {
	if kernel.handle != nil {
		C.metal_release_pipeline_state(kernel.handle)
		kernel.handle = nil
	}
}

// dispatch executes a compute shader on Metal.
func (m *metalBackend) dispatch(kernel *Kernel, inputs []*Reference, output *Reference, workSize int) error {
	// Collect all buffer handles
	bufferCount := len(inputs) + 1
	buffers := make([]unsafe.Pointer, bufferCount)

	for i, input := range inputs {
		buffers[i] = input.handle
	}
	buffers[len(inputs)] = output.handle

	// Convert to C array
	var errorPtr *C.char
	result := C.metal_dispatch_compute(
		m.queue,
		kernel.handle,
		(*unsafe.Pointer)(unsafe.Pointer(&buffers[0])),
		C.int(bufferCount),
		C.int(workSize),
		&errorPtr,
	)

	if result == 0 {
		if errorPtr != nil {
			errMsg := C.GoString(errorPtr)
			C.free(unsafe.Pointer(errorPtr))
			return fmt.Errorf("Metal dispatch failed: %s", errMsg)
		}
		return fmt.Errorf("Metal dispatch failed: unknown error")
	}

	return nil
}

// deviceInfo returns information about the Metal device.
func (m *metalBackend) deviceInfo() Info {
	return m.devInfo
}
