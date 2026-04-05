package mental

/*
#include "mental.h"
#include <stdlib.h>
*/
import "C"
import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"runtime"
	"sync"
	"unsafe"
)

// ── Process UUID + Listener ──────────────────────────────────────

var (
	processUUID     string
	processUUIDOnce sync.Once
	listener        net.Listener
)

// UUID returns this process's unique identifier (32 lowercase hex chars).
// Generated once on first call, stable for the process lifetime.
// Also initializes the Unix domain socket listener for cross-process linking.
func UUID() string {
	processUUIDOnce.Do(initUUID)
	return processUUID
}

func initUUID() {
	// Generate a v4 UUID
	var buf [16]byte
	_, err := rand.Read(buf[:])
	if err != nil {
		// Fallback: use pid + some entropy from the address space
		buf[0] = byte(os.Getpid())
		buf[1] = byte(os.Getpid() >> 8)
	}
	buf[6] = (buf[6] & 0x0F) | 0x40 // version 4
	buf[8] = (buf[8] & 0x3F) | 0x80 // variant 1
	processUUID = hex.EncodeToString(buf[:])

	// Bind a Unix domain socket for peer-to-peer linking
	path := "/tmp/m-" + processUUID
	var listenErr error
	listener, listenErr = net.Listen("unix", path)
	if listenErr != nil {
		// Non-fatal: linking won't work but sparking still will
		listener = nil
		return
	}

	// Register cleanup to remove the socket file on exit
	runtime.SetFinalizer(&listener, func(_ *net.Listener) {
		cleanupListener()
	})
}

// cleanupListener closes the listener and removes the socket file.
func cleanupListener() {
	if listener != nil {
		path := "/tmp/m-" + processUUID
		listener.Close()
		os.Remove(path)
		listener = nil
	}
}

// Disclosable is the interface for custom disclosure credential types.
// Any type that implements Disclose() can be used as a TDisclosure
// type parameter.  The returned bytes are stored and compared as-is.
//
// For Go primitives (string, integers, floats, bool, fixed-size arrays),
// Disclosable is NOT required -- they are serialized automatically via
// their in-memory representation.
type Disclosable interface {
	Disclose() []byte
}

// Disclosure controls observer access to a [Reference].
//
// The disclosure lives in shared memory so the owner can change it
// on-the-fly and every observer sees the change immediately.
type Disclosure int32

const (
	// RelationallyOpen grants full read/write access to all observers.
	// No credential required.  This is the default.
	RelationallyOpen Disclosure = 0

	// RelationallyInclusive grants read-only access without a credential.
	// Write access requires the credential.
	RelationallyInclusive Disclosure = 1

	// RelationallyExclusive denies all access without the credential.
	// Without it, [Reference.Data] returns nil.
	RelationallyExclusive Disclosure = 2
)

// Reference is a handle to a named, UUID-scoped shared memory region
// that can optionally be pinned to a GPU device for compute operations.
//
// TData is the type stored in the reference -- its size determines the
// allocation.  TDisclosure is the credential type used for access
// control (string, [32]byte, a custom struct implementing [Disclosable],
// etc.).
//
// Reference is a process-local data buffer, optionally GPU-pinned.
// Access is controlled by disclosure rules set at creation time.
// To modify disclosure, use the DisclosureHandle returned by ReferenceCreate.
type Reference[TData any, TDisclosure any] struct {
	ptr uintptr
	mu  sync.Mutex
}

// DisclosureHandle is the authority to modify a reference's access rules.
// Without this handle, you cannot change disclosure mode or credentials.
type DisclosureHandle[TDisclosure any] struct {
	ptr uintptr // C mental_disclosure
}

// ReferenceCreate allocates a process-local data buffer sized to hold TData.
// The first parameter is the disclosure mode (Open, Inclusive, or Exclusive).
// An optional second parameter provides the credential for Inclusive/Exclusive modes.
//
// Returns the reference and a disclosure handle (the authority to modify access rules).
//
//	ref, dh := mental.ReferenceCreate[MyData, string](mental.RelationallyOpen)
//	ref, dh := mental.ReferenceCreate[MyData, string](mental.RelationallyExclusive, "secret")
func ReferenceCreate[TData any, TDisclosure any](mode Disclosure, credential ...TDisclosure) (*Reference[TData, TDisclosure], *DisclosureHandle[TDisclosure]) {
	size := unsafe.Sizeof(*new(TData))
	if size == 0 {
		size = 1
	}

	var credPtr unsafe.Pointer
	var credLen C.size_t
	if len(credential) > 0 {
		b := discloseBytes(credential[0])
		if len(b) > 0 {
			credPtr = unsafe.Pointer(&b[0])
			credLen = C.size_t(len(b))
		}
	}

	var cdh C.mental_disclosure
	cref := C.mental_reference_create(C.size_t(size),
		C.mental_relationship(mode), credPtr, credLen, &cdh)
	ptr := uintptr(unsafe.Pointer(cref))
	if ptr == 0 {
		return nil, nil
	}

	ref := &Reference[TData, TDisclosure]{ptr: ptr}
	runtime.SetFinalizer(ref, (*Reference[TData, TDisclosure]).Close)

	var dh *DisclosureHandle[TDisclosure]
	if cdh != nil {
		dh = &DisclosureHandle[TDisclosure]{ptr: uintptr(unsafe.Pointer(cdh))}
	}

	return ref, dh
}

// Handle returns the raw C pointer for use with [Dispatch] and
// [ViewportAttach].  These functions accept raw handles so that
// references with different generic type parameters can be mixed.
func (r *Reference[TData, TDisclosure]) Handle() uintptr {
	if r == nil {
		return 0
	}
	return r.ptr
}

// resolveCredential converts explicit credential args to C pointers.
func (r *Reference[TData, TDisclosure]) resolveCredential(explicit []TDisclosure) (unsafe.Pointer, C.size_t) {
	if len(explicit) > 0 {
		b := discloseBytes(explicit[0])
		if len(b) > 0 {
			return unsafe.Pointer(&b[0]), C.size_t(len(b))
		}
	}
	return nil, 0
}

// Data returns a typed pointer to the mapped shared memory.
// Valid for reads and writes until Close is called.
//
// Access is subject to the reference's [Disclosure]:
//   - [RelationallyOpen]: always returns the pointer
//   - [RelationallyInclusive]: returns the pointer (read-only without credential)
//   - [RelationallyExclusive]: returns nil without the correct credential
//
// The owner always gets full access regardless of disclosure mode.
// If a credential provider is set, it is evaluated fresh each call.
//
//	ref.Data()            // no credential
//	ref.Data(myCred)      // with credential
func (r *Reference[TData, TDisclosure]) Data(credential ...TDisclosure) *TData {
	if r == nil || r.ptr == 0 {
		return nil
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	p := C.mental_reference_data(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		cp, cl,
	)
	r.mu.Unlock()
	if p == nil {
		return nil
	}
	return (*TData)(p)
}

// Size returns the size of the mapped region in bytes.
func (r *Reference[TData, TDisclosure]) Size() int {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return int(C.mental_reference_size(C.mental_reference(unsafe.Pointer(r.ptr))))
}

// Bytes returns the mapped region as a byte slice.
// The returned slice shares memory with the reference -- writes are visible
// to all processes that have the region open.
//
// Returns nil if disclosure denies access.
func (r *Reference[TData, TDisclosure]) Bytes(credential ...TDisclosure) []byte {
	if r == nil || r.ptr == 0 {
		return nil
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	p := C.mental_reference_data(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		cp, cl,
	)
	size := int(C.mental_reference_size(C.mental_reference(unsafe.Pointer(r.ptr))))
	r.mu.Unlock()
	if p == nil || size == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(p), size)
}

// Writable reports whether write access is permitted under the current
// disclosure mode.  The owner always returns true.
func (r *Reference[TData, TDisclosure]) Writable(credential ...TDisclosure) bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	result := C.mental_reference_writable(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		cp, cl,
	)
	r.mu.Unlock()
	return result != 0
}

// IsOwner reports whether this handle owns the reference (i.e., this process
// created it).  Owner handles have full access regardless of disclosure
// and control the reference's lifecycle -- closing an owner handle unlinks
// the shared memory.
func (r *Reference[TData, TDisclosure]) IsOwner() bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	return r.ptr != 0
}

// ── GPU Pinning ───────────────────────────────────────────────────

// Pin attaches a GPU backend buffer to this reference.
// The current shared memory contents are uploaded to the GPU.
// After pinning, [Write] updates both GPU and shm, and the reference
// can participate in [Dispatch] and [ViewportAttach] operations.
//
// No-op if already pinned to the same device.
// Returns an error if the backend allocation fails.
func (r *Reference[TData, TDisclosure]) Pin(device Device) error {
	if r == nil || r.ptr == 0 {
		return ErrInvalidReference
	}
	runtime.LockOSThread()
	result := C.mental_reference_pin(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		C.mental_device(unsafe.Pointer(device)),
	)
	runtime.UnlockOSThread()
	if int(result) != 0 {
		return getLibError()
	}
	return nil
}

// IsPinned reports whether this reference has GPU backing.
func (r *Reference[TData, TDisclosure]) IsPinned() bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	return C.mental_reference_is_pinned(C.mental_reference(unsafe.Pointer(r.ptr))) != 0
}

// GetDevice returns the GPU device this reference is pinned to,
// or 0 if not pinned.
func (r *Reference[TData, TDisclosure]) GetDevice() Device {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return Device(unsafe.Pointer(C.mental_reference_device(C.mental_reference(unsafe.Pointer(r.ptr)))))
}

// Write copies data into the reference.
// If pinned: writes to both GPU buffer and shared memory.
// If not pinned: writes to shared memory only.
// Size is clamped to the reference's capacity.
func (r *Reference[TData, TDisclosure]) Write(data []byte) {
	if r == nil || r.ptr == 0 || len(data) == 0 {
		return
	}
	runtime.LockOSThread()
	C.mental_reference_write(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		unsafe.Pointer(&data[0]),
		C.size_t(len(data)),
	)
	runtime.UnlockOSThread()
}

// Read copies data from the reference into a host buffer.
// If pinned: reads from GPU buffer.
// If not pinned: reads from shared memory.
// Size is clamped to the reference's capacity.
func (r *Reference[TData, TDisclosure]) Read(buf []byte) {
	if r == nil || r.ptr == 0 || len(buf) == 0 {
		return
	}
	runtime.LockOSThread()
	C.mental_reference_read(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		unsafe.Pointer(&buf[0]),
		C.size_t(len(buf)),
	)
	runtime.UnlockOSThread()
}

// ── Clone ─────────────────────────────────────────────────────────

// Clone snapshots the reference into a new locally-owned region.
func (r *Reference[TData, TDisclosure]) Clone(credential ...TDisclosure) *Reference[TData, TDisclosure] {
	return r.cloneInternal(0, credential)
}

// CloneToDevice snapshots the reference and pins the clone to a GPU device.
func (r *Reference[TData, TDisclosure]) CloneToDevice(device Device, credential ...TDisclosure) *Reference[TData, TDisclosure] {
	return r.cloneInternal(device, credential)
}

func (r *Reference[TData, TDisclosure]) cloneInternal(device Device, credential []TDisclosure) *Reference[TData, TDisclosure] {
	if r == nil || r.ptr == 0 {
		return nil
	}

	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	cref := C.mental_reference_clone(
		C.mental_reference(unsafe.Pointer(r.ptr)),
		C.mental_device(unsafe.Pointer(device)),
		cp, cl,
	)
	r.mu.Unlock()

	ptr := uintptr(unsafe.Pointer(cref))
	if ptr == 0 {
		return nil
	}
	ref := &Reference[TData, TDisclosure]{ptr: ptr}
	runtime.SetFinalizer(ref, (*Reference[TData, TDisclosure]).Close)
	return ref
}

// ── Disclosure ────────────────────────────────────────────────────

// GetDisclosure returns the current disclosure mode.
func (r *Reference[TData, TDisclosure]) GetDisclosure() Disclosure {
	if r == nil || r.ptr == 0 {
		return RelationallyOpen
	}
	return Disclosure(C.mental_reference_get_disclosure(C.mental_reference(unsafe.Pointer(r.ptr))))
}

// ── Disclosure Handle ─────────────────────────────────────────────

// SetMode changes the disclosure mode.
func (dh *DisclosureHandle[TDisclosure]) SetMode(mode Disclosure) {
	if dh == nil || dh.ptr == 0 {
		return
	}
	C.mental_disclosure_set_mode(
		C.mental_disclosure(unsafe.Pointer(dh.ptr)),
		C.mental_relationship(mode),
	)
}

// SetCredential sets a fixed credential for disclosure-controlled access.
func (dh *DisclosureHandle[TDisclosure]) SetCredential(cred TDisclosure) {
	if dh == nil || dh.ptr == 0 {
		return
	}
	b := discloseBytes(cred)
	if len(b) == 0 {
		C.mental_disclosure_set_credential(
			C.mental_disclosure(unsafe.Pointer(dh.ptr)),
			nil, 0,
		)
	} else {
		C.mental_disclosure_set_credential(
			C.mental_disclosure(unsafe.Pointer(dh.ptr)),
			unsafe.Pointer(&b[0]), C.size_t(len(b)),
		)
	}
}

// ClearCredential removes the credential.
func (dh *DisclosureHandle[TDisclosure]) ClearCredential() {
	if dh == nil || dh.ptr == 0 {
		return
	}
	C.mental_disclosure_set_credential(
		C.mental_disclosure(unsafe.Pointer(dh.ptr)),
		nil, 0,
	)
}

// Lock freezes the disclosure rules permanently.  After locking, the
// mode and credential can never be changed again.  The reference
// continues to function normally under the frozen rules.
func (dh *DisclosureHandle[TDisclosure]) Lock() {
	if dh != nil && dh.ptr != 0 {
		C.mental_disclosure_close(C.mental_disclosure(unsafe.Pointer(dh.ptr)))
		dh.ptr = 0
	}
}

// Close shuts down disclosed access permanently.  All future access
// checks on the reference will be denied, regardless of credentials.
// Like closing a Go channel -- the resource is done.
func (dh *DisclosureHandle[TDisclosure]) Close() {
	if dh == nil || dh.ptr == 0 {
		return
	}
	// Set to exclusive with no credential -- impossible to access
	C.mental_disclosure_set_mode(
		C.mental_disclosure(unsafe.Pointer(dh.ptr)),
		C.MENTAL_RELATIONALLY_EXCLUSIVE,
	)
	C.mental_disclosure_set_credential(
		C.mental_disclosure(unsafe.Pointer(dh.ptr)),
		nil, 0,
	)
	// Then lock it so nobody can reopen
	C.mental_disclosure_close(C.mental_disclosure(unsafe.Pointer(dh.ptr)))
	dh.ptr = 0
}

// ── Lifecycle ─────────────────────────────────────────────────────

// Close unmaps and releases the reference handle.
// If this process owns the reference, the shared memory is unlinked
// (destroyed) and any GPU buffer is freed.
// Observer handles are simply unmapped.  Safe to call multiple times.
func (r *Reference[TData, TDisclosure]) Close() {
	if r != nil && r.ptr != 0 {
		r.mu.Lock()
		C.mental_reference_close(C.mental_reference(unsafe.Pointer(r.ptr)))
		r.ptr = 0
		runtime.SetFinalizer(r, nil)
		r.mu.Unlock()
	}
}

// ── discloseBytes ─────────────────────────────────────────────────

// discloseBytes converts a TDisclosure value to raw bytes.
func discloseBytes(v any) []byte {
	if d, ok := v.(Disclosable); ok {
		return d.Disclose()
	}
	if s, ok := v.(string); ok {
		return []byte(s)
	}
	if b, ok := v.([]byte); ok {
		return b
	}

	switch val := v.(type) {
	case int8:
		return []byte{byte(val)}
	case uint8:
		return []byte{val}
	case int16:
		return (*[2]byte)(unsafe.Pointer(&val))[:]
	case uint16:
		return (*[2]byte)(unsafe.Pointer(&val))[:]
	case int32:
		return (*[4]byte)(unsafe.Pointer(&val))[:]
	case uint32:
		return (*[4]byte)(unsafe.Pointer(&val))[:]
	case int64:
		return (*[8]byte)(unsafe.Pointer(&val))[:]
	case uint64:
		return (*[8]byte)(unsafe.Pointer(&val))[:]
	case int:
		return (*[8]byte)(unsafe.Pointer(&val))[:]
	case uint:
		return (*[8]byte)(unsafe.Pointer(&val))[:]
	case float32:
		return (*[4]byte)(unsafe.Pointer(&val))[:]
	case float64:
		return (*[8]byte)(unsafe.Pointer(&val))[:]
	case complex64:
		return (*[8]byte)(unsafe.Pointer(&val))[:]
	case complex128:
		return (*[16]byte)(unsafe.Pointer(&val))[:]
	case bool:
		if val {
			return []byte{1}
		}
		return []byte{0}
	}

	size := unsafe.Sizeof(v)
	if size > 0 && size <= 128 {
		return unsafe.Slice((*byte)((*ifaceHeader)(unsafe.Pointer(&v)).data), size)
	}

	panic(fmt.Sprintf("mental: TDisclosure type %T is not Disclosable and not a supported primitive", v))
}

type ifaceHeader struct {
	typ  uintptr
	data unsafe.Pointer
}
