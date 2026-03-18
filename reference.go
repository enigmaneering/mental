package mental

import (
	"sync"
	"unsafe"
)

// Reference is an observable GPU memory reference that provides synchronized access
// to GPU buffers. Multiple observers can read from a Reference concurrently, while
// mutations are exclusive.
//
// Reference implements the "irrational reference" pattern where observers see
// whatever state exists at the moment of observation, with no guarantees
// about intermediate states between observations.
//
// GPU memory is automatically released when the Reference is garbage collected.
// Users do not need to manually manage GPU memory.
//
// IMPORTANT: Reference instances must be created via proper factory methods.
// Do not instantiate Reference directly.
type Reference struct {
	backend     backend
	handle      unsafe.Pointer
	mu          sync.RWMutex
	size        int
	deviceIndex int  // Which device this reference is allocated on
	freed       bool // Track if manually freed to avoid double-free
	created     bool // Sanity check to ensure proper construction
}

// sanityCheck verifies this Reference was created properly via factory methods.
func (r *Reference) sanityCheck() {
	if !r.created {
		panic("mental.Reference must be created via proper factory methods (e.g., mental.Thought.Alloc() or mental.Thought.From())")
	}
}

// Observe returns a snapshot of the current GPU memory state.
// Multiple observers can call Observe() concurrently.
//
// The returned data is a copy of the current state. Changes to the returned
// slice do not affect GPU memory. Between observations, the GPU memory may
// have changed multiple times - you only see the current state.
func (r *Reference) Observe() []byte {
	r.sanityCheck()
	r.mu.RLock()
	defer r.mu.RUnlock()

	data := make([]byte, r.size)
	r.backend.read(r, data)
	return data
}

// Mutate provides exclusive access to modify GPU memory.
// The function receives the current state and can modify it.
// Changes are written back to GPU memory when the function returns.
//
// If the mutation changes the size of the data (via append, reslicing, etc.),
// the GPU buffer is automatically reallocated to match the new size.
//
// Only one mutation can occur at a time. Other observers and mutators
// will block until the mutation completes.
func (r *Reference) Mutate(fn func([]byte) []byte) {
	r.sanityCheck()
	r.mu.Lock()
	defer r.mu.Unlock()

	data := make([]byte, r.size)
	r.backend.read(r, data)
	data = fn(data)

	// If size changed, reallocate GPU buffer
	if len(data) != r.size {
		r.backend.free(r)
		newRef := getBackend(r.deviceIndex).alloc(len(data))
		r.handle = newRef.handle
		r.backend = newRef.backend
		r.size = len(data)
	}

	r.backend.write(r, data)
}

// Write directly writes data to GPU memory.
// If the data size differs from the current buffer size, the GPU buffer
// is automatically reallocated to match.
//
// The write operation is exclusive - other operations will block until complete.
func (r *Reference) Write(data []byte) {
	r.sanityCheck()
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.freed {
		panic("device: write to freed reference")
	}

	// If size changed, reallocate GPU buffer
	if len(data) != r.size {
		r.backend.free(r)
		newRef := getBackend(r.deviceIndex).alloc(len(data))
		r.handle = newRef.handle
		r.backend = newRef.backend
		r.size = len(data)
	}

	r.backend.write(r, data)
}

// Read directly reads data from GPU memory into the provided buffer.
// This is a convenience method for reading without allocating a new slice.
//
// Multiple readers can read concurrently.
func (r *Reference) Read(dest []byte) {
	r.sanityCheck()
	r.mu.RLock()
	defer r.mu.RUnlock()
	if r.freed {
		panic("device: read from freed reference")
	}
	r.backend.read(r, dest)
}

// Size returns the size of the GPU memory allocation in bytes.
func (r *Reference) Size() int {
	r.sanityCheck()
	return r.size
}

// Device returns the device index this reference is allocated on.
func (r *Reference) Device() int {
	r.sanityCheck()
	return r.deviceIndex
}

// Lock acquires an exclusive lock on this reference.
// No other operations (reads or writes) can access this reference until Unlock is called.
//
// IMPORTANT: Always call Unlock to release the lock, typically with defer:
//
//	ref.Lock()
//	defer ref.Unlock()
//	// ... work with reference ...
func (r *Reference) Lock() {
	r.sanityCheck()
	r.mu.Lock()
}

// Unlock releases the exclusive lock acquired by Lock.
// Must be called after Lock to allow other operations to proceed.
func (r *Reference) Unlock() {
	r.sanityCheck()
	r.mu.Unlock()
}

// RLock acquires a shared read lock on this reference.
// Multiple readers can hold RLock simultaneously, but writers will block.
//
// IMPORTANT: Always call RUnlock to release the lock, typically with defer:
//
//	ref.RLock()
//	defer ref.RUnlock()
//	// ... read from reference ...
func (r *Reference) RLock() {
	r.sanityCheck()
	r.mu.RLock()
}

// RUnlock releases the shared read lock acquired by RLock.
// Must be called after RLock to allow writers to proceed.
func (r *Reference) RUnlock() {
	r.sanityCheck()
	r.mu.RUnlock()
}

// Clone creates a copy of this reference's GPU memory.
// The clone is allocated on the same device and has the same size.
// GPU memory for the clone is automatically managed via finalizer.
//
// The original reference is locked during the clone operation to ensure
// a consistent copy is made.
//
// The optional target parameter specifies which device to allocate the clone on.
// If omitted, defaults to the source reference's device.
func (r *Reference) Clone(target ...Info) *Reference {
	r.sanityCheck()
	r.mu.RLock()
	defer r.mu.RUnlock()

	// Default to source device if no target specified
	deviceIndex := r.deviceIndex
	if len(target) > 0 {
		deviceIndex = target[0].Index
	}

	clone := r.backend.clone(r)
	clone.deviceIndex = deviceIndex
	clone.created = true
	return clone
}

// free is the internal cleanup function called by the finalizer or manual Free().
func (r *Reference) free() {
	r.mu.Lock()
	defer r.mu.Unlock()

	if !r.freed {
		r.backend.free(r)
		r.freed = true
	}
}
