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

// Disclosure controls observer access to a [Ref].
//
// The disclosure lives in shared memory so the owner can change it
// on-the-fly and every observer sees the change immediately.
type Disclosure int32

const (
	// RelationallyOpen grants full read/write access to all observers.
	// No passphrase required.  This is the default.
	RelationallyOpen Disclosure = 0

	// RelationallyInclusive grants read-only access without a passphrase.
	// Write access requires the passphrase.
	RelationallyInclusive Disclosure = 1

	// RelationallyExclusive denies all access without the passphrase.
	// Without it, [Ref.Data] returns nil.
	RelationallyExclusive Disclosure = 2
)

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
// Disclosure defaults to [RelationallyOpen] with no passphrase.
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
// Access is subject to the ref's [Disclosure]:
//   - [RelationallyOpen]: always returns the pointer (passphrase ignored)
//   - [RelationallyInclusive]: returns the pointer (read-only without passphrase)
//   - [RelationallyExclusive]: returns nil without the correct passphrase
//
// The owner always gets full access regardless of disclosure mode.
//
// Pass a passphrase string to authenticate. Omit for unauthenticated access:
//
//	ref.Data()              // no passphrase
//	ref.Data("secret123")   // with passphrase
func (r *Ref) Data(passphrase ...string) unsafe.Pointer {
	if r == nil || r.ptr == 0 {
		return nil
	}
	var pp uintptr
	if len(passphrase) > 0 && passphrase[0] != "" {
		cpw := append([]byte(passphrase[0]), 0)
		pp = uintptr(unsafe.Pointer(&cpw[0]))
	}
	return unsafe.Pointer(call2(ft.refData, r.ptr, pp))
}

// Size returns the size of the mapped region in bytes (user data only).
func (r *Ref) Size() int {
	if r == nil || r.ptr == 0 {
		return 0
	}
	return int(call1(ft.refSize, r.ptr))
}

// Bytes returns the mapped region as a byte slice.
// The returned slice shares memory with the ref — writes are visible
// to all processes that have the region open.
//
// Returns nil if disclosure denies access.  Pass a passphrase for
// authenticated access:
//
//	ref.Bytes()              // no passphrase
//	ref.Bytes("secret123")   // with passphrase
func (r *Ref) Bytes(passphrase ...string) []byte {
	data := r.Data(passphrase...)
	size := r.Size()
	if data == nil || size == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(data), size)
}

// Writable reports whether write access is permitted under the current
// disclosure mode.  The owner always returns true.
//
//	ref.Writable()              // check without passphrase
//	ref.Writable("secret123")   // check with passphrase
func (r *Ref) Writable(passphrase ...string) bool {
	if r == nil || r.ptr == 0 {
		return false
	}
	var pp uintptr
	if len(passphrase) > 0 && passphrase[0] != "" {
		cpw := append([]byte(passphrase[0]), 0)
		pp = uintptr(unsafe.Pointer(&cpw[0]))
	}
	return call2(ft.refWritable, r.ptr, pp) != 0
}

// GetDisclosure returns the current disclosure mode.
func (r *Ref) GetDisclosure() Disclosure {
	if r == nil || r.ptr == 0 {
		return RelationallyOpen
	}
	return Disclosure(call1(ft.refGetDisclosure, r.ptr))
}

// SetDisclosure sets the disclosure mode.  Only the owner can change it;
// observer calls are no-ops.
func (r *Ref) SetDisclosure(mode Disclosure) {
	if r == nil || r.ptr == 0 {
		return
	}
	call2(ft.refSetDisclosure, r.ptr, uintptr(mode))
}

// SetPassphrase sets the passphrase for disclosure-controlled access.
// Only the owner can set it; observer calls are no-ops.
// Pass "" to clear the passphrase.  Max 63 characters.
func (r *Ref) SetPassphrase(passphrase string) {
	if r == nil || r.ptr == 0 {
		return
	}
	if passphrase == "" {
		call1(ft.refSetPassphrase, 0)
		return
	}
	cpw := append([]byte(passphrase), 0)
	call2(ft.refSetPassphrase, r.ptr, uintptr(unsafe.Pointer(&cpw[0])))
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
