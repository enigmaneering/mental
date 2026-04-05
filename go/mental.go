// Package mental provides Go bindings for the Mental GPU compute library.
//
// Mental is statically linked via cgo. The library is initialized
// automatically at link time -- no runtime loading is required.
//
// Cleanup of temporary files is automatic at process exit via atexit.
// Use [Defer] to register additional cleanup callbacks.
package mental

/*
#include "mental.h"
#include <stdlib.h>
*/
import "C"
import (
	"fmt"
	"runtime"
	"unsafe"
)

// APIType identifies the GPU backend in use.
type APIType int32

const (
	APINone   APIType = -1
	APIMetal  APIType = 0
	APID3D12  APIType = 1
	APIVulkan APIType = 2
	APIOpenCL APIType = 3
	APIOpenGL APIType = 4
	APIPoCL   APIType = 5
	APIWebGPU APIType = 6
)

// Error codes returned by the C library.
type Error int32

const (
	Success              Error = 0
	ErrNoDevices         Error = -1
	ErrInvalidDevice     Error = -2
	ErrAllocationFailed  Error = -3
	ErrCompilationFailed Error = -4
	ErrInvalidReference  Error = -5
	ErrInvalidKernel     Error = -6
	ErrDispatchFailed    Error = -7
	ErrBackendFailed     Error = -8
)

func (e Error) Error() string {
	switch e {
	case Success:
		return "success"
	case ErrNoDevices:
		return "no GPU devices found"
	case ErrInvalidDevice:
		return "invalid device"
	case ErrAllocationFailed:
		return "allocation failed"
	case ErrCompilationFailed:
		return "compilation failed"
	case ErrInvalidReference:
		return "invalid reference"
	case ErrInvalidKernel:
		return "invalid kernel"
	case ErrDispatchFailed:
		return "dispatch failed"
	case ErrBackendFailed:
		return "backend failed"
	default:
		return "unknown error"
	}
}

// Device is an opaque handle to a GPU device.
type Device uintptr

// Kernel is an opaque handle to a compiled compute shader.
type Kernel uintptr

// Viewport is an opaque handle to a surface presentation attachment.
type Viewport uintptr

// DeviceCount returns the number of available GPU devices.
func DeviceCount() int {
	return int(C.mental_device_count())
}

// DeviceGet returns the device at the given index.
func DeviceGet(index int) Device {
	return Device(unsafe.Pointer(C.mental_device_get(C.int(index))))
}

// Name returns the human-readable device name.
func (d Device) Name() string {
	p := C.mental_device_name(C.mental_device(unsafe.Pointer(d)))
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// API returns the backend API type for this device.
func (d Device) API() APIType {
	return APIType(C.mental_device_api(C.mental_device(unsafe.Pointer(d))))
}

// APIName returns the backend API name as a string.
func (d Device) APIName() string {
	p := C.mental_device_api_name(C.mental_device(unsafe.Pointer(d)))
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// Compile compiles shader source for the given device.
// The shader language is auto-detected and transpiled as needed.
// Returns the compiled kernel and any error from the C library.
func Compile(dev Device, source string) (Kernel, error) {
	ensureTools()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	csrc := C.CString(source)
	defer C.free(unsafe.Pointer(csrc))

	k := C.mental_compile(
		C.mental_device(unsafe.Pointer(dev)),
		csrc,
		C.size_t(len(source)),
	)
	if k == nil {
		return 0, getLibError()
	}
	return Kernel(unsafe.Pointer(k)), nil
}

// Dispatch executes a kernel with the given input and output references.
// All references must be pinned to a GPU device.
//
// Pass reference handles via [Reference.Handle]:
//
//	err := mental.Dispatch(kernel,
//	    []uintptr{input1.Handle(), input2.Handle()},
//	    output.Handle(), 1024)
func Dispatch(kernel Kernel, inputs []uintptr, output uintptr, workSize int) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var inputsPtr *C.mental_reference
	if len(inputs) > 0 {
		inputsPtr = (*C.mental_reference)(unsafe.Pointer(&inputs[0]))
	}
	rc := C.mental_dispatch(
		C.mental_kernel(unsafe.Pointer(kernel)),
		inputsPtr,
		C.int(len(inputs)),
		C.mental_reference(unsafe.Pointer(output)),
		C.int(workSize),
	)
	if rc != 0 {
		return getLibError()
	}
	return nil
}

// Finalize frees the compiled kernel. Must be called explicitly.
func (k Kernel) Finalize() {
	C.mental_kernel_finalize(C.mental_kernel(unsafe.Pointer(k)))
}

// ViewportAttach attaches a reference to an OS surface for zero-copy presentation.
// The reference must be pinned to a GPU device.
// The surface parameter is platform-specific (NSView*, HWND, etc.).
//
// Pass the reference handle via [Reference.Handle]:
//
//	vp, err := mental.ViewportAttach(ref.Handle(), surface)
func ViewportAttach(ref uintptr, surface uintptr) (Viewport, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	v := C.mental_viewport_attach(
		C.mental_reference(unsafe.Pointer(ref)),
		unsafe.Pointer(surface),
	)
	if v == nil {
		return 0, getLibError()
	}
	return Viewport(unsafe.Pointer(v)), nil
}

// Present renders the attached buffer to the surface.
func (v Viewport) Present() {
	C.mental_viewport_present(C.mental_viewport(unsafe.Pointer(v)))
}

// Detach disconnects and cleans up the viewport.
func (v Viewport) Detach() {
	C.mental_viewport_detach(C.mental_viewport(unsafe.Pointer(v)))
}

// GetError returns the last error code from the C library (thread-local).
func GetError() Error {
	return Error(C.mental_get_error())
}

// GetErrorMessage returns the last error message from the C library (thread-local).
func GetErrorMessage() string {
	p := C.mental_get_error_message()
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

func getLibError() error {
	code := Error(C.mental_get_error())
	if code == Success {
		return nil
	}
	msg := C.GoString(C.mental_get_error_message())
	if msg != "" {
		return &libError{code: code, msg: msg}
	}
	return code
}

type libError struct {
	code Error
	msg  string
}

func (e *libError) Error() string { return e.msg }
func (e *libError) Code() Error   { return e.code }

// Pipe is a chained kernel dispatch pipeline. Multiple dispatches are
// recorded into a single GPU command buffer and submitted together —
// data stays on the GPU between stages with no CPU round-trips.
type Pipe uintptr

// CreatePipe creates a new pipe on the given device. The pipe begins
// recording dispatches immediately.
func CreatePipe(dev Device) (Pipe, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	p := C.mental_pipe_create(C.mental_device(unsafe.Pointer(dev)))
	if p == nil {
		return 0, getLibError()
	}
	return Pipe(unsafe.Pointer(p)), nil
}

// Add records a kernel dispatch into the pipe. The output of one
// dispatch can be the input of the next — data stays on GPU.
//
// Pass reference handles via [Reference.Handle]:
//
//	pipe.Add(kernel, []uintptr{a.Handle()}, b.Handle(), 1024)
func (p Pipe) Add(kernel Kernel, inputs []uintptr, output uintptr, workSize int) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var inputsPtr *C.mental_reference
	if len(inputs) > 0 {
		inputsPtr = (*C.mental_reference)(unsafe.Pointer(&inputs[0]))
	}
	rc := C.mental_pipe_add(
		C.mental_pipe(unsafe.Pointer(p)),
		C.mental_kernel(unsafe.Pointer(kernel)),
		inputsPtr,
		C.int(len(inputs)),
		C.mental_reference(unsafe.Pointer(output)),
		C.int(workSize),
	)
	if int(rc) != 0 {
		return getLibError()
	}
	return nil
}

// Execute submits all recorded dispatches as one GPU submission.
// Blocks until all dispatches complete.
func (p Pipe) Execute() error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	rc := C.mental_pipe_execute(C.mental_pipe(unsafe.Pointer(p)))
	if int(rc) != 0 {
		return getLibError()
	}
	return nil
}

// Finalize frees the pipe. Must be called explicitly.
func (p Pipe) Finalize() {
	C.mental_pipe_finalize(C.mental_pipe(unsafe.Pointer(p)))
}

// Shutdown tears down the GPU backend, frees devices, runs atexit
// callbacks, and clears the library registry.  Blocks until complete.
// Returns an error if any cleanup step failed.
func Shutdown() error {
	rc := C.mental_shutdown()
	if int(rc) != 0 {
		return fmt.Errorf("mental: shutdown failed")
	}
	return nil
}
