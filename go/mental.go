// Package mental provides Go bindings for the Mental GPU compute library.
//
// Mental is loaded dynamically at runtime with no cgo dependency.
// The library is initialized automatically on package import via init().
//
// The search order for the shared library is:
//  1. System library paths (standard dlopen / LoadLibrary)
//  2. ./external/ directory relative to the working directory
//  3. Embedded prebuilt library (if present at compile time in lib/)
//
// Cleanup of temporary files is automatic at process exit via atexit.
// Use [Defer] to register additional cleanup callbacks.
//
// Assembly shims translate Go's calling convention to the platform C ABI
// (SysV AMD64, AAPCS64, or Windows x64) for each supported target.
package mental

import (
	"runtime"
	"unsafe"
)

// unsafePointer is a helper to avoid repeated unsafe.Pointer casts.
func unsafePointer(p interface{}) unsafe.Pointer {
	switch v := p.(type) {
	case *funcTable:
		return unsafe.Pointer(v)
	default:
		panic("unsafePointer: unexpected type")
	}
}

// APIType identifies the GPU backend in use.
type APIType int32

const (
	APIMetal  APIType = 0
	APID3D12  APIType = 1
	APIVulkan APIType = 2
	APIOpenCL APIType = 3
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
	return int(call0(ft.deviceCount))
}

// DeviceGet returns the device at the given index.
func DeviceGet(index int) Device {
	return Device(call1(ft.deviceGet, uintptr(index)))
}

// Name returns the human-readable device name.
func (d Device) Name() string {
	p := call1(ft.deviceName, uintptr(d))
	if p == 0 {
		return ""
	}
	return goStringFromPtr(p)
}

// API returns the backend API type for this device.
func (d Device) API() APIType {
	return APIType(call1(ft.deviceAPI, uintptr(d)))
}

// APIName returns the backend API name as a string.
func (d Device) APIName() string {
	p := call1(ft.deviceAPIName, uintptr(d))
	if p == 0 {
		return ""
	}
	return goStringFromPtr(p)
}

// Compile compiles shader source for the given device.
// The shader language is auto-detected and transpiled as needed.
// Returns the compiled kernel and any error from the C library.
func Compile(dev Device, source string) (Kernel, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	src := unsafe.StringData(source)
	k := Kernel(call3(ft.compile, uintptr(dev), uintptr(unsafe.Pointer(src)), uintptr(len(source))))
	if k == 0 {
		return 0, getLibError()
	}
	return k, nil
}

// Dispatch executes a kernel with the given input and output references.
// All references must be pinned to a GPU device.
//
// Pass reference handles via [Reference.Handle]:
//
//	mental.Dispatch(kernel,
//	    []uintptr{input1.Handle(), input2.Handle()},
//	    output.Handle(), 1024)
func Dispatch(kernel Kernel, inputs []uintptr, output uintptr, workSize int) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var inputsPtr uintptr
	if len(inputs) > 0 {
		inputsPtr = uintptr(unsafe.Pointer(&inputs[0]))
	}
	call5(ft.dispatch, uintptr(kernel), inputsPtr, uintptr(len(inputs)), output, uintptr(workSize))
}

// Finalize frees the compiled kernel. Must be called explicitly.
func (k Kernel) Finalize() {
	call1(ft.kernelFinalize, uintptr(k))
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

	v := Viewport(call2(ft.viewportAttach, ref, surface))
	if v == 0 {
		return 0, getLibError()
	}
	return v, nil
}

// Present renders the attached buffer to the surface.
func (v Viewport) Present() {
	call1(ft.viewportPres, uintptr(v))
}

// Detach disconnects and cleans up the viewport.
func (v Viewport) Detach() {
	call1(ft.viewportDet, uintptr(v))
}

// GetError returns the last error code from the C library (thread-local).
func GetError() Error {
	return Error(call0(ft.getError))
}

// GetErrorMessage returns the last error message from the C library (thread-local).
func GetErrorMessage() string {
	p := call0(ft.getErrorMsg)
	if p == 0 {
		return ""
	}
	return goStringFromPtr(p)
}

func getLibError() error {
	code := Error(call0(ft.getError))
	if code == Success {
		return nil
	}
	p := call0(ft.getErrorMsg)
	msg := ""
	if p != 0 {
		msg = goStringFromPtr(p)
	}
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

// goStringFromPtr reads a null-terminated C string from the given address.
func goStringFromPtr(p uintptr) string {
	if p == 0 {
		return ""
	}
	ptr := unsafe.Pointer(p)
	var n int
	for *(*byte)(unsafe.Add(ptr, n)) != 0 {
		n++
	}
	return string(unsafe.Slice((*byte)(ptr), n))
}
