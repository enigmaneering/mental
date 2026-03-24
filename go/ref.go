package mental

import (
	"runtime"
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

// Ref is a handle to a UUID-scoped shared memory region.
//
// The creating process (owner) allocates the region with [RefCreate].
// Sparked children or peers observe it with [RefOpen], providing the
// owner's UUID.  When the owner process exits, all its regions are
// automatically unlinked.
//
// The intended pattern is for refs to flow down the spark chain:
// parent creates, children observe.  Sharing outside the spark chain
// works but returns nil gracefully if the owner has already exited.
type Ref struct {
	ptr uintptr
}

// RefCreate allocates a named shared memory region of the given size.
// The name is scoped to this process's UUID namespace.
//
// Returns nil if the name is empty, size is 0, or allocation fails.
// Creating a ref with a name that already exists in this process's
// namespace returns nil (names must be unique per process).
func RefCreate(name string, size int) *Ref {
	if name == "" || size <= 0 {
		return nil
	}
	cname := append([]byte(name), 0)
	ptr := call2(ft.refCreate, uintptr(unsafe.Pointer(&cname[0])), uintptr(size))
	if ptr == 0 {
		return nil
	}
	ref := &Ref{ptr: ptr}
	runtime.SetFinalizer(ref, (*Ref).Close)
	return ref
}

// RefOpen maps an existing shared memory region created by another process.
// Returns nil gracefully if the owner has exited or the ref doesn't exist.
func RefOpen(peerUUID, name string) *Ref {
	if peerUUID == "" || name == "" {
		return nil
	}
	cuuid := append([]byte(peerUUID), 0)
	cname := append([]byte(name), 0)
	ptr := call2(ft.refOpen, uintptr(unsafe.Pointer(&cuuid[0])), uintptr(unsafe.Pointer(&cname[0])))
	if ptr == 0 {
		return nil
	}
	ref := &Ref{ptr: ptr}
	runtime.SetFinalizer(ref, (*Ref).Close)
	return ref
}

// Data returns an unsafe.Pointer to the mapped shared memory.
// Valid for reads and writes until Close is called.
//
// Callers should use unsafe.Slice to create a typed view:
//
//	ref := mental.RefCreate("Velocity", 8)
//	data := unsafe.Slice((*float64)(ref.Data()), 1)
//	data[0] = 42.0
func (r *Ref) Data() unsafe.Pointer {
	if r == nil || r.ptr == 0 {
		return nil
	}
	return unsafe.Pointer(call1(ft.refData, r.ptr))
}

// Size returns the size of the mapped region in bytes.
func (r *Ref) Size() int {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return int(call1(ft.refSize, r.ptr))
}

// Bytes returns the mapped region as a byte slice.
// The returned slice shares memory with the ref — writes are visible
// to all processes that have the region open.
func (r *Ref) Bytes() []byte {
	data := r.Data()
	size := r.Size()
	if data == nil || size == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(data), size)
}

// Close unmaps and releases the ref handle.
// If this process owns the ref, the shared memory is unlinked (destroyed).
// Observer handles are simply unmapped.  Safe to call multiple times.
func (r *Ref) Close() {
	if r != nil && r.ptr != 0 {
		call1(ft.refClose, r.ptr)
		r.ptr = 0
		runtime.SetFinalizer(r, nil)
	}
}
