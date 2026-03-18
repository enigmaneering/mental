package main

/*
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Opaque handle types
typedef void* MentalDevice;
typedef void* MentalReference;
typedef void* MentalKernel;

// Device info structure
typedef struct {
    int index;
    char* name;
    int device_type;  // 0=Other, 1=Integrated, 2=Discrete, 3=Virtual
    int api;          // 0=Metal, 1=Vulkan, 2=D3D12, 3=OpenCL
} MentalDeviceInfo;

// Error codes
typedef enum {
    MENTAL_OK = 0,
    MENTAL_ERROR_INVALID_HANDLE = -1,
    MENTAL_ERROR_ALLOCATION_FAILED = -2,
    MENTAL_ERROR_COMPILATION_FAILED = -3,
    MENTAL_ERROR_DISPATCH_FAILED = -4,
    MENTAL_ERROR_INVALID_SIZE = -5,
    MENTAL_ERROR_DEVICE_NOT_FOUND = -6,
} MentalError;
*/
import "C"
import (
	"runtime"
	"sync"
	"unsafe"

	"git.enigmaneering.org/mental"
)

// Global handle registry to prevent Go GC from collecting objects still referenced by C
var (
	handleMutex sync.RWMutex
	nextHandle  uintptr = 1
	references  = make(map[uintptr]*mental.Reference)
	kernels     = make(map[uintptr]*mental.Kernel)
)

// registerReference stores a Reference and returns a C-compatible handle.
// The Reference will NOT have a finalizer - C caller must call mental_release_reference.
func registerReference(ref *mental.Reference) C.MentalReference {
	handleMutex.Lock()
	defer handleMutex.Unlock()

	handle := nextHandle
	nextHandle++
	references[handle] = ref

	return C.MentalReference(unsafe.Pointer(handle))
}

// getReference retrieves a Reference from a handle.
func getReference(handle C.MentalReference) (*mental.Reference, bool) {
	handleMutex.RLock()
	defer handleMutex.RUnlock()

	h := uintptr(handle)
	ref, ok := references[h]
	return ref, ok
}

// unregisterReference removes a Reference from the registry.
func unregisterReference(handle C.MentalReference) bool {
	handleMutex.Lock()
	defer handleMutex.Unlock()

	h := uintptr(handle)
	_, ok := references[h]
	if ok {
		delete(references, h)
	}
	return ok
}

// registerKernel stores a Kernel and returns a C-compatible handle.
// The Kernel will NOT have a finalizer - C caller must call mental_release_kernel.
func registerKernel(kernel *mental.Kernel) C.MentalKernel {
	handleMutex.Lock()
	defer handleMutex.Unlock()

	handle := nextHandle
	nextHandle++
	kernels[handle] = kernel

	return C.MentalKernel(unsafe.Pointer(handle))
}

// getKernel retrieves a Kernel from a handle.
func getKernel(handle C.MentalKernel) (*mental.Kernel, bool) {
	handleMutex.RLock()
	defer handleMutex.RUnlock()

	h := uintptr(handle)
	kernel, ok := kernels[h]
	return kernel, ok
}

// unregisterKernel removes a Kernel from the registry.
func unregisterKernel(handle C.MentalKernel) bool {
	handleMutex.Lock()
	defer handleMutex.Unlock()

	h := uintptr(handle)
	_, ok := kernels[h]
	if ok {
		delete(kernels, h)
	}
	return ok
}

//export mental_init
func mental_init() C.int {
	// Initialize the runtime if needed
	runtime.LockOSThread()
	return C.int(C.MENTAL_OK)
}

//export mental_get_device_count
func mental_get_device_count() C.int {
	devices := mental.List()
	return C.int(len(devices))
}

//export mental_get_device_info
func mental_get_device_info(index C.int, info *C.MentalDeviceInfo) C.int {
	devices := mental.List()
	idx := int(index)
	if idx < 0 || idx >= len(devices) {
		return C.int(C.MENTAL_ERROR_DEVICE_NOT_FOUND)
	}

	device := devices[idx]
	info.index = C.int(device.Index)
	info.name = C.CString(device.Name)
	info.device_type = C.int(device.Type)
	info.api = C.int(device.API)

	return C.int(C.MENTAL_OK)
}

//export mental_free_device_info
func mental_free_device_info(info *C.MentalDeviceInfo) {
	if info.name != nil {
		C.free(unsafe.Pointer(info.name))
		info.name = nil
	}
}

//export mental_create_reference
func mental_create_reference(size C.size_t, device_index C.int) C.MentalReference {
	deviceIdx := int(device_index)

	// Get device info to validate index
	devices := mental.List()
	if deviceIdx < 0 || deviceIdx >= len(devices) {
		return nil
	}

	ref := mental.Thought.Alloc(int(size), devices[deviceIdx])
	return registerReference(ref)
}

//export mental_create_reference_from_data
func mental_create_reference_from_data(data *C.uint8_t, size C.size_t, device_index C.int) C.MentalReference {
	deviceIdx := int(device_index)

	// Get device info to validate index
	devices := mental.List()
	if deviceIdx < 0 || deviceIdx >= len(devices) {
		return nil
	}

	// Convert C data to Go slice
	goData := C.GoBytes(unsafe.Pointer(data), C.int(size))

	ref := mental.Thought.From(goData, devices[deviceIdx])
	return registerReference(ref)
}

//export mental_reference_read
func mental_reference_read(handle C.MentalReference, buffer *C.uint8_t, size C.size_t) C.int {
	ref, ok := getReference(handle)
	if !ok {
		return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
	}

	data := ref.Observe()
	if len(data) != int(size) {
		return C.int(C.MENTAL_ERROR_INVALID_SIZE)
	}

	// Copy Go data to C buffer
	C.memcpy(unsafe.Pointer(buffer), unsafe.Pointer(&data[0]), size)
	return C.int(C.MENTAL_OK)
}

//export mental_reference_write
func mental_reference_write(handle C.MentalReference, data *C.uint8_t, size C.size_t) C.int {
	ref, ok := getReference(handle)
	if !ok {
		return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
	}

	// Convert C data to Go slice
	goData := C.GoBytes(unsafe.Pointer(data), C.int(size))
	ref.Write(goData)

	return C.int(C.MENTAL_OK)
}

//export mental_reference_size
func mental_reference_size(handle C.MentalReference) C.size_t {
	ref, ok := getReference(handle)
	if !ok {
		return 0
	}

	return C.size_t(ref.Size())
}

//export mental_release_reference
func mental_release_reference(handle C.MentalReference) C.int {
	if !unregisterReference(handle) {
		return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
	}
	// The Reference will be garbage collected by Go
	return C.int(C.MENTAL_OK)
}

//export mental_compile_kernel
func mental_compile_kernel(source *C.char, device_index C.int) C.MentalKernel {
	deviceIdx := int(device_index)

	// Get device info to validate index
	devices := mental.List()
	if deviceIdx < 0 || deviceIdx >= len(devices) {
		return nil
	}

	goSource := C.GoString(source)
	kernel, err := mental.Program.Compile(goSource, devices[deviceIdx])
	if err != nil {
		return nil
	}

	return registerKernel(kernel)
}

//export mental_kernel_dispatch
func mental_kernel_dispatch(
	kernel_handle C.MentalKernel,
	input_handles *C.MentalReference,
	input_count C.int,
	output_handle C.MentalReference,
	work_size C.int,
) C.int {
	kernel, ok := getKernel(kernel_handle)
	if !ok {
		return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
	}

	output, ok := getReference(output_handle)
	if !ok {
		return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
	}

	// Convert C array of handles to Go slice of References
	inputCount := int(input_count)
	inputs := make([]*mental.Reference, inputCount)

	if inputCount > 0 {
		inputArray := (*[1 << 30]C.MentalReference)(unsafe.Pointer(input_handles))[:inputCount:inputCount]
		for i := 0; i < inputCount; i++ {
			ref, ok := getReference(inputArray[i])
			if !ok {
				return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
			}
			inputs[i] = ref
		}
	}

	err := kernel.Dispatch(inputs, output, int(work_size))
	if err != nil {
		return C.int(C.MENTAL_ERROR_DISPATCH_FAILED)
	}

	return C.int(C.MENTAL_OK)
}

//export mental_release_kernel
func mental_release_kernel(handle C.MentalKernel) C.int {
	if !unregisterKernel(handle) {
		return C.int(C.MENTAL_ERROR_INVALID_HANDLE)
	}
	// The Kernel will be garbage collected by Go
	return C.int(C.MENTAL_OK)
}
