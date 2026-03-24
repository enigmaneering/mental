package mental

import (
	"fmt"
	"runtime"
	"sync"
	"unsafe"
)

// UUID returns this process's unique identifier (32 lowercase hex chars).
// Generated once on first call, stable for the process lifetime.
// Read-only — the returned string is safe to share across goroutines.
//
// Every mental process gets a UUID at startup which scopes its shared
// memory references under /mental-{uuid}/{name}.  This allows many
// processes to use the same ref name (e.g., "Velocity") without collision.
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

// Disclosure controls observer access to a [Ref].
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
	// Without it, [Ref.Data] returns nil.
	RelationallyExclusive Disclosure = 2
)

// Ref is a handle to a UUID-scoped shared memory region.
//
// TData is the type stored in the ref — its size determines the
// allocation.  TDisclosure is the credential type used for access
// control (string, [32]byte, a custom struct implementing [Disclosable],
// etc.).
//
// The creating process (owner) allocates the region with [RefCreate].
// Sparked children or peers observe it with [RefOpen], providing the
// owner's UUID.  When the owner process exits, all its regions are
// automatically unlinked.
type Ref[TData any, TDisclosure any] struct {
	ptr          uintptr
	name         string             // ref name (for clone name generation)
	credentialFn func() TDisclosure // evaluated fresh each access check
	mu           sync.Mutex         // serializes provider evaluation + C call
}

// RefCreate allocates a named shared memory region sized to hold TData.
// The name is scoped to this process's UUID namespace.
// Disclosure defaults to [RelationallyOpen] with no credential.
//
// Returns nil if the name is empty or allocation fails.
func RefCreate[TData any, TDisclosure any](name string) *Ref[TData, TDisclosure] {
	if name == "" {
		return nil
	}
	size := unsafe.Sizeof(*new(TData))
	if size == 0 {
		size = 1 // zero-size types still get a region
	}
	cname := append([]byte(name), 0)
	ptr := call2(ft.refCreate, uintptr(unsafe.Pointer(&cname[0])), uintptr(size))
	if ptr == 0 {
		return nil
	}
	ref := &Ref[TData, TDisclosure]{ptr: ptr, name: name}
	runtime.SetFinalizer(ref, (*Ref[TData, TDisclosure]).Close)
	return ref
}

// RefOpen maps an existing shared memory region created by another process.
// Returns nil gracefully if the owner has exited or the ref doesn't exist.
func RefOpen[TData any, TDisclosure any](peerUUID, name string) *Ref[TData, TDisclosure] {
	if peerUUID == "" || name == "" {
		return nil
	}
	cuuid := append([]byte(peerUUID), 0)
	cname := append([]byte(name), 0)
	ptr := call2(ft.refOpen, uintptr(unsafe.Pointer(&cuuid[0])), uintptr(unsafe.Pointer(&cname[0])))
	if ptr == 0 {
		return nil
	}
	ref := &Ref[TData, TDisclosure]{ptr: ptr, name: name}
	runtime.SetFinalizer(ref, (*Ref[TData, TDisclosure]).Close)
	return ref
}

// resolveCredential evaluates the credential provider (if set) and
// refreshes the shm credential, then returns the credential bytes to
// present for the access check.  Must be called under r.mu.
func (r *Ref[TData, TDisclosure]) resolveCredential(explicit []TDisclosure) (uintptr, uintptr) {
	// Evaluate provider — this IS the credential, produced fresh right now
	if r.credentialFn != nil {
		cred := r.credentialFn()
		b := discloseBytes(cred)
		if len(b) > 0 {
			// Refresh owner credential in shm (no-op if observer)
			call3(ft.refSetCredential, r.ptr,
				uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
			// Also present as observer credential
			return uintptr(unsafe.Pointer(&b[0])), uintptr(len(b))
		}
	}

	// Explicit credential passed by caller
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
// Access is subject to the ref's [Disclosure]:
//   - [RelationallyOpen]: always returns the pointer
//   - [RelationallyInclusive]: returns the pointer (read-only without credential)
//   - [RelationallyExclusive]: returns nil without the correct credential
//
// The owner always gets full access regardless of disclosure mode.
// If a credential provider is set, it is evaluated fresh each call.
//
//	ref.Data()            // no credential
//	ref.Data(myCred)      // with credential
func (r *Ref[TData, TDisclosure]) Data(credential ...TDisclosure) *TData {
	if r == nil || r.ptr == 0 {
		return nil
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	p := call3(ft.refData, r.ptr, cp, cl)
	r.mu.Unlock()
	if p == 0 {
		return nil
	}
	return (*TData)(unsafe.Pointer(p))
}

// Size returns the size of the mapped region in bytes.
func (r *Ref[TData, TDisclosure]) Size() int {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return int(call1(ft.refSize, r.ptr))
}

// Bytes returns the mapped region as a byte slice.
// The returned slice shares memory with the ref — writes are visible
// to all processes that have the region open.
//
// Returns nil if disclosure denies access.
func (r *Ref[TData, TDisclosure]) Bytes(credential ...TDisclosure) []byte {
	if r == nil || r.ptr == 0 {
		return nil
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	p := call3(ft.refData, r.ptr, cp, cl)
	size := int(call1(ft.refSize, r.ptr))
	r.mu.Unlock()
	if p == 0 || size == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), size)
}

// Writable reports whether write access is permitted under the current
// disclosure mode.  The owner always returns true.
func (r *Ref[TData, TDisclosure]) Writable(credential ...TDisclosure) bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	result := call3(ft.refWritable, r.ptr, cp, cl)
	r.mu.Unlock()
	return result != 0
}

// IsOwner reports whether this handle owns the ref (i.e., this process
// created it).  Owner handles have full access regardless of disclosure
// and control the ref's lifecycle — closing an owner handle unlinks
// the shared memory.
func (r *Ref[TData, TDisclosure]) IsOwner() bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	return call1(ft.refIsOwner, r.ptr) != 0
}

// Clone snapshots the ref into a new locally-owned region.
//
// If this is a cross-process observer handle, Clone breaks the linkage:
// the returned ref is an independent local copy under this process's
// UUID namespace — changes to the original are no longer visible.
//
// If this is already an owner handle, Clone creates a sibling snapshot
// with its own name and lifecycle.
//
// The clone's disclosure defaults to [RelationallyOpen] with no credential.
//
//	local := observed.Clone()           // open source
//	local := observed.Clone(myCred)     // exclusive source
func (r *Ref[TData, TDisclosure]) Clone(credential ...TDisclosure) *Ref[TData, TDisclosure] {
	if r == nil || r.ptr == 0 {
		return nil
	}

	// Generate a unique name for the clone
	seq := NextCount()
	cloneName := fmt.Sprintf("%s-%d", r.name, seq)
	cname := append([]byte(cloneName), 0)

	r.mu.Lock()
	cp, cl := r.resolveCredential(credential)
	ptr := call4(ft.refClone, r.ptr,
		uintptr(unsafe.Pointer(&cname[0])),
		cp, cl)
	r.mu.Unlock()

	if ptr == 0 {
		return nil
	}
	ref := &Ref[TData, TDisclosure]{ptr: ptr, name: cloneName}
	runtime.SetFinalizer(ref, (*Ref[TData, TDisclosure]).Close)
	return ref
}

// GetDisclosure returns the current disclosure mode.
func (r *Ref[TData, TDisclosure]) GetDisclosure() Disclosure {
	if r == nil || r.ptr == 0 {
		return RelationallyOpen
	}
	return Disclosure(call1(ft.refGetDisclosure, r.ptr))
}

// SetDisclosure sets the disclosure mode.  Only the owner can change it;
// observer calls are no-ops.
func (r *Ref[TData, TDisclosure]) SetDisclosure(mode Disclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	call2(ft.refSetDisclosure, r.ptr, uintptr(mode))
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
func (r *Ref[TData, TDisclosure]) SetCredentialProvider(fn func() TDisclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	r.mu.Lock()
	r.credentialFn = fn
	// Immediately evaluate so credential is fresh right now
	if fn != nil {
		cred := fn()
		b := discloseBytes(cred)
		if len(b) > 0 {
			call3(ft.refSetCredential, r.ptr,
				uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
		}
	}
	r.mu.Unlock()
}

// SetCredential sets a fixed credential for disclosure-controlled access.
// This is sugar for a credential provider that always returns the same value.
// Only the owner can set it; observer calls are no-ops.
// Max 128 bytes.
func (r *Ref[TData, TDisclosure]) SetCredential(cred TDisclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	r.mu.Lock()
	// Clear provider — raw credential takes precedence
	r.credentialFn = nil
	b := discloseBytes(cred)
	if len(b) == 0 {
		call3(ft.refSetCredential, r.ptr, 0, 0)
	} else {
		call3(ft.refSetCredential, r.ptr, uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	}
	r.mu.Unlock()
}

// ClearCredential removes the credential and any provider.  Owner only.
func (r *Ref[TData, TDisclosure]) ClearCredential() {
	if r == nil || r.ptr == 0 {
		return
	}
	r.mu.Lock()
	r.credentialFn = nil
	call3(ft.refSetCredential, r.ptr, 0, 0)
	r.mu.Unlock()
}

// Close unmaps and releases the ref handle.
// If this process owns the ref, the shared memory is unlinked (destroyed).
// Observer handles are simply unmapped.  Safe to call multiple times.
func (r *Ref[TData, TDisclosure]) Close() {
	if r != nil && r.ptr != 0 {
		r.mu.Lock()
		r.credentialFn = nil
		call1(ft.refClose, r.ptr)
		r.ptr = 0
		runtime.SetFinalizer(r, nil)
		r.mu.Unlock()
	}
}

// discloseBytes converts a TDisclosure value to raw bytes.
//
// Priority:
//  1. If it implements Disclosable, call Disclose()
//  2. If it's a string, use its bytes
//  3. If it's a fixed-size primitive, use its in-memory representation
//  4. Panic — the type is not serializable
func discloseBytes(v any) []byte {
	// 1. Disclosable interface
	if d, ok := v.(Disclosable); ok {
		return d.Disclose()
	}

	// 2. String — most common case
	if s, ok := v.(string); ok {
		return []byte(s)
	}

	// 3. []byte — pass through
	if b, ok := v.([]byte); ok {
		return b
	}

	// 4. Fixed-size primitives — use in-memory bytes
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

	// 5. Fixed-size arrays — try via unsafe.Sizeof
	// This catches [N]byte, [32]byte, etc.
	size := unsafe.Sizeof(v)
	if size > 0 && size <= 128 {
		return unsafe.Slice((*byte)((*ifaceHeader)(unsafe.Pointer(&v)).data), size)
	}

	panic(fmt.Sprintf("mental: TDisclosure type %T is not Disclosable and not a supported primitive", v))
}

// ifaceHeader is the runtime layout of an interface value.
type ifaceHeader struct {
	typ  uintptr
	data unsafe.Pointer
}
