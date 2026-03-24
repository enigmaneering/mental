package mental

import "runtime"

// Count returns the next value from the process-global, lock-free,
// monotonic 64-bit counter (1, 2, 3, …).  Never returns 0.
//
// Thread-safe.  For globally unique IDs across a spark cluster,
// combine with process identity (e.g., Stdlink() fd or os.Getpid()).
// Each sparked subprocess starts its own counter at 1.
func Count() uint64 {
	return uint64(call0(ft.count))
}

// Counter is a heap-allocated, lock-free, atomic uint64 counter.
// Unlike the global Count, each Counter is an independent instance.
//
// The underlying C counter is freed automatically via GC finalizer,
// or explicitly via Close.
type Counter struct {
	ptr uintptr
}

// NewCounter creates a new counter initialized to 0.
func NewCounter() *Counter {
	ptr := call0(ft.counterCreate)
	if ptr == 0 {
		return nil
	}
	c := &Counter{ptr: ptr}
	runtime.SetFinalizer(c, (*Counter).Close)
	return c
}

// Increment atomically adds delta and returns the new value.
func (c *Counter) Increment(delta uint64) uint64 {
	return uint64(call2(ft.counterIncrement, c.ptr, uintptr(delta)))
}

// Decrement atomically subtracts delta and returns the new value.
// Saturates at 0 (will not wrap below zero).
func (c *Counter) Decrement(delta uint64) uint64 {
	return uint64(call2(ft.counterDecrement, c.ptr, uintptr(delta)))
}

// Reset atomically sets the counter to 0 and returns the previous value.
func (c *Counter) Reset() uint64 {
	return uint64(call1(ft.counterReset, c.ptr))
}

// Close destroys the underlying C counter. Safe to call multiple times.
// After Close, the Counter must not be used.
func (c *Counter) Close() {
	if c.ptr != 0 {
		call1(ft.counterFinalize, c.ptr)
		c.ptr = 0
		runtime.SetFinalizer(c, nil)
	}
}
