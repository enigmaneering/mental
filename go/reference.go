package mental

import (
	"fmt"
	"runtime"
	"sync"
	"unsafe"
)

// UUID returns this process's unique identifier (32 lowercase hex chars).
// Generated once on first call, stable for the process lifetime.
func UUID() string {
	p := call0(ft.uuid)
	if p == 0 {
		return ""
	}
	return goStringFromPtr(p)
}

// Disclosable is the interface for custom disclosure credential types.
// Any type that implements Disclose() can be used as a TDisclosure
// type parameter.  The returned bytes are stored and compared as-is.
//
// For Go primitives (string, integers, floats, bool, fixed-size arrays),
// Disclosable is NOT required — they are serialized automatically via
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
// TData is the type stored in the reference — its size determines the
// allocation.  TDisclosure is the credential type used for access
// control (string, [32]byte, a custom struct implementing [Disclosable],
// etc.).
//
// Shared memory provides the cross-process data plane.
// GPU pinning provides the compute plane.
// Disclosure controls the access plane.
//
// The creating process (owner) allocates the region with [ReferenceCreate].
// Sparked children or peers observe it with [ReferenceOpen], providing the
// owner's UUID.  When the owner process exits, all its regions are
// automatically unlinked.
type Reference[TData any, TDisclosure any] struct {
	ptr          uintptr
	name         string             // reference name (for clone name generation)
	credentialFn func() TDisclosure // evaluated fresh each access check
	mu           sync.Mutex         // serializes provider evaluation + C call
}

// ReferenceCreate allocates a named shared memory region sized to hold TData.
// The name is scoped to this process's UUID namespace.
// Disclosure defaults to [RelationallyOpen] with no credential.
//
// Returns nil if the name is empty or allocation fails.
func ReferenceCreate[TData any, TDisclosure any](name string) *Reference[TData, TDisclosure] {
	if name == "" {
		return nil
	}
	size := unsafe.Sizeof(*new(TData))
	if size == 0 {
		size = 1 // zero-size types still get a region
	}
	cname := append([]byte(name), 0)
	ptr := call2(ft.referenceCreate, uintptr(unsafe.Pointer(&cname[0])), uintptr(size))
	if ptr == 0 {
		return nil
	}
	ref := &Reference[TData, TDisclosure]{ptr: ptr, name: name}
	runtime.SetFinalizer(ref, (*Reference[TData, TDisclosure]).Close)
	return ref
}

// ReferenceOpen maps an existing shared memory region created by another process.
// Returns nil gracefully if the owner has exited or the reference doesn't exist.
func ReferenceOpen[TData any, TDisclosure any](peerUUID, name string) *Reference[TData, TDisclosure] {
	if peerUUID == "" || name == "" {
		return nil
	}
	cuuid := append([]byte(peerUUID), 0)
	cname := append([]byte(name), 0)
	ptr := call2(ft.referenceOpen, uintptr(unsafe.Pointer(&cuuid[0])), uintptr(unsafe.Pointer(&cname[0])))
	if ptr == 0 {
		return nil
	}
	ref := &Reference[TData, TDisclosure]{ptr: ptr, name: name}
	runtime.SetFinalizer(ref, (*Reference[TData, TDisclosure]).Close)
	return ref
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

// resolveCredential evaluates the credential provider (if set) and
// refreshes the shm credential, then returns the credential bytes to
// present for the access check.  Must be called under r.mu.
func (r *Reference[TData, TDisclosure]) resolveCredential(explicit []TDisclosure) (uintptr, uintptr) {
	if r.credentialFn != nil {
		cred := r.credentialFn()
		b := discloseBytes(cred)
		if len(b) > 0 {
			call3(ft.referenceSetCredential, r.ptr,
				uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
			return uintptr(unsafe.Pointer(&b[0])), uintptr(len(b))
		}
	}
	if len(explicit) > 0 {
		b := discloseBytes(explicit[0])
		if len(b) > 0 {
			return uintptr(unsafe.Pointer(&b[0])), uintptr(len(b))
		}
	}
	return 0, 0
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
	p := call3(ft.referenceData, r.ptr, cp, cl)
	r.mu.Unlock()
	if p == 0 {
		return nil
	}
	return (*TData)(unsafe.Pointer(p))
}

// Size returns the size of the mapped region in bytes.
func (r *Reference[TData, TDisclosure]) Size() int {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return int(call1(ft.referenceSize, r.ptr))
}

// Bytes returns the mapped region as a byte slice.
// The returned slice shares memory with the reference — writes are visible
// to all processes that have the region open.
//
// Returns nil if disclosure denies access.
func (r *Reference[TData, TDisclosure]) Bytes(credential ...TDisclosure) []byte {
	if r == nil || r.ptr == 0 {
		return nil
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	p := call3(ft.referenceData, r.ptr, cp, cl)
	size := int(call1(ft.referenceSize, r.ptr))
	r.mu.Unlock()
	if p == 0 || size == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), size)
}

// Writable reports whether write access is permitted under the current
// disclosure mode.  The owner always returns true.
func (r *Reference[TData, TDisclosure]) Writable(credential ...TDisclosure) bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	result := call3(ft.referenceWritable, r.ptr, cp, cl)
	r.mu.Unlock()
	return result != 0
}

// IsOwner reports whether this handle owns the reference (i.e., this process
// created it).  Owner handles have full access regardless of disclosure
// and control the reference's lifecycle — closing an owner handle unlinks
// the shared memory.
func (r *Reference[TData, TDisclosure]) IsOwner() bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	return call1(ft.referenceIsOwner, r.ptr) != 0
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
	result := call2(ft.referencePin, r.ptr, uintptr(device))
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
	return call1(ft.referenceIsPinned, r.ptr) != 0
}

// GetDevice returns the GPU device this reference is pinned to,
// or 0 if not pinned.
func (r *Reference[TData, TDisclosure]) GetDevice() Device {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return Device(call1(ft.referenceDevice, r.ptr))
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
	call3(ft.referenceWrite, r.ptr,
		uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)))
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
	call3(ft.referenceRead, r.ptr,
		uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	runtime.UnlockOSThread()
}

// ── Clone ─────────────────────────────────────────────────────────

// Clone snapshots the reference into a new locally-owned region.
//
// If this is a cross-process observer handle, Clone breaks the linkage:
// the returned reference is an independent local copy under this process's
// UUID namespace — changes to the original are no longer visible.
//
// If this is already an owner handle, Clone creates a sibling snapshot
// with its own name and lifecycle.
//
// The clone's disclosure defaults to [RelationallyOpen] with no credential.
//
// Options:
//   - credential: passed as the first variadic arg for disclosure-checked access
//   - device: if provided via [CloneToDevice], the clone is pinned to that GPU
//
//	local := observed.Clone()                          // CPU-only clone
//	local := observed.Clone(myCred)                    // with credential
//	local := observed.CloneToDevice(dev)               // clone and pin
//	local := observed.CloneToDevice(dev, myCred)       // clone, pin, with credential
func (r *Reference[TData, TDisclosure]) Clone(credential ...TDisclosure) *Reference[TData, TDisclosure] {
	return r.cloneInternal(0, credential)
}

// CloneToDevice snapshots the reference and pins the clone to a GPU device.
// This enables cross-boundary clone-and-pin: observe a remote reference,
// then clone it directly onto a local GPU — even if the source device differs.
func (r *Reference[TData, TDisclosure]) CloneToDevice(device Device, credential ...TDisclosure) *Reference[TData, TDisclosure] {
	return r.cloneInternal(device, credential)
}

func (r *Reference[TData, TDisclosure]) cloneInternal(device Device, credential []TDisclosure) *Reference[TData, TDisclosure] {
	if r == nil || r.ptr == 0 {
		return nil
	}

	seq := NextCount()
	cloneName := fmt.Sprintf("%s-%d", r.name, seq)
	cname := append([]byte(cloneName), 0)

	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	ptr := call5(ft.referenceClone, r.ptr,
		uintptr(unsafe.Pointer(&cname[0])),
		uintptr(device),
		cp, cl)
	r.mu.Unlock()

	if ptr == 0 {
		return nil
	}
	ref := &Reference[TData, TDisclosure]{ptr: ptr, name: cloneName}
	runtime.SetFinalizer(ref, (*Reference[TData, TDisclosure]).Close)
	return ref
}

// ── Disclosure ────────────────────────────────────────────────────

// GetDisclosure returns the current disclosure mode.
func (r *Reference[TData, TDisclosure]) GetDisclosure() Disclosure {
	if r == nil || r.ptr == 0 {
		return RelationallyOpen
	}
	return Disclosure(call1(ft.referenceGetDisclosure, r.ptr))
}

// SetDisclosure sets the disclosure mode.  Only the owner can change it;
// observer calls are no-ops.
func (r *Reference[TData, TDisclosure]) SetDisclosure(mode Disclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	call2(ft.referenceSetDisclosure, r.ptr, uintptr(mode))
}

// SetCredentialProvider sets a function that produces the credential
// on demand.  The provider is evaluated under a mutex each time an
// access check occurs (Data, Bytes, Writable), guaranteeing the
// comparison always uses a fresh credential — no stale cache.
//
// For the owner: the provider's result is written to the shm header,
// keeping the stored credential in sync with the source of truth.
//
// For an observer: the provider's result is presented as the observer's
// credential for the access check.
//
// Pass nil to clear the provider.
func (r *Reference[TData, TDisclosure]) SetCredentialProvider(fn func() TDisclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	r.mu.Lock()
	r.credentialFn = fn
	if fn != nil {
		cred := fn()
		b := discloseBytes(cred)
		if len(b) > 0 {
			call3(ft.referenceSetCredential, r.ptr,
				uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
		}
	}
	r.mu.Unlock()
}

// SetCredential sets a fixed credential for disclosure-controlled access.
// Only the owner can set it; observer calls are no-ops.
// Max 128 bytes.
func (r *Reference[TData, TDisclosure]) SetCredential(cred TDisclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	r.mu.Lock()
	r.credentialFn = nil
	b := discloseBytes(cred)
	if len(b) == 0 {
		call3(ft.referenceSetCredential, r.ptr, 0, 0)
	} else {
		call3(ft.referenceSetCredential, r.ptr, uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	}
	r.mu.Unlock()
}

// ClearCredential removes the credential and any provider.  Owner only.
func (r *Reference[TData, TDisclosure]) ClearCredential() {
	if r == nil || r.ptr == 0 {
		return
	}
	r.mu.Lock()
	r.credentialFn = nil
	call3(ft.referenceSetCredential, r.ptr, 0, 0)
	r.mu.Unlock()
}

// ── Lifecycle ─────────────────────────────────────────────────────

// Close unmaps and releases the reference handle.
// If this process owns the reference, the shared memory is unlinked
// (destroyed) and any GPU buffer is freed.
// Observer handles are simply unmapped.  Safe to call multiple times.
func (r *Reference[TData, TDisclosure]) Close() {
	if r != nil && r.ptr != 0 {
		r.mu.Lock()
		r.credentialFn = nil
		call1(ft.referenceClose, r.ptr)
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
