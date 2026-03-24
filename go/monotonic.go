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
// A Counter starts in the "empty" state — distinct from zero.
// Incrementing from empty treats it as 0 (empty + 1 = 1).
// Decrementing below zero transitions back to empty.
// Use [Counter.Empty] to test for the empty state.
//
// The underlying C counter is freed automatically via GC finalizer,
// or explicitly via Close.
type Counter struct {
	ptr uintptr
}

// NewCounter creates a new counter in the empty state.
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
// If the counter is empty, it transitions to a non-empty state —
// even when delta is 0.
func (c *Counter) Increment(delta uint64) uint64 {
	return uint64(call2(ft.counterIncrement, c.ptr, uintptr(delta)))
}

// Decrement atomically subtracts delta and returns the new value.
// If the current value is less than delta, the counter becomes empty
// and 0 is returned. Decrementing an already-empty counter returns 0.
func (c *Counter) Decrement(delta uint64) uint64 {
	return uint64(call2(ft.counterDecrement, c.ptr, uintptr(delta)))
}

// Empty reports whether the counter is in the empty state.
// A counter starts empty and returns to empty only by decrementing
// below zero. Reset sets the counter to 0, not empty.
func (c *Counter) Empty() bool {
	return call1(ft.counterEmpty, c.ptr) != 0
}

// Reset atomically resets the counter and returns the previous value.
// If the counter was empty, returns 0.
//
// By default, Reset sets the counter to 0. Pass true to reset to the
// empty state instead:
//
//	c.Reset()      // → 0
//	c.Reset(true)  // → empty
func (c *Counter) Reset(toEmpty ...bool) uint64 {
	var flag uintptr
	if len(toEmpty) > 0 && toEmpty[0] {
		flag = 1
	}
	return uint64(call2(ft.counterReset, c.ptr, flag))
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
