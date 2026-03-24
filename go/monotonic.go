package mental

import (
	"encoding/binary"
	"fmt"
	"math"
	"sync/atomic"
)

// Count is a single monotonic value — an ID, a sequence number, a ticket.
// The most primitive mental reference: just a uint64 you can pass over
// a [Link] to identify yourself, claim a slot, or coordinate with peers.
//
// Count implements [Linkable] — it marshals as 8 little-endian bytes.
type Count uint64

// Marshal serializes the count as 8 little-endian bytes.
func (c Count) Marshal() []byte {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint64(b, uint64(c))
	return b
}

// Unmarshal reads 8 little-endian bytes into the count.
func (c *Count) Unmarshal(data []byte) error {
	if len(data) < 8 {
		return fmt.Errorf("mental: Count.Unmarshal: need 8 bytes, got %d", len(data))
	}
	*c = Count(binary.LittleEndian.Uint64(data))
	return nil
}

// counterEmptySentinel is UINT64_MAX — the "empty" state.
// A Counter starts empty.  Incrementing from empty treats it as 0.
// Decrementing below zero transitions back to empty.
// This value is never returned as a valid Count.
const counterEmptySentinel = math.MaxUint64

// Counter is a lock-free atomic counter that produces [Count] values.
// It is the source of identification — one system exposes its Counter
// so others can take IDs from it, or shares a known Counter for peers
// to coordinate against.
//
// A Counter starts in the "empty" state — distinct from zero.
// Incrementing from empty treats it as 0 (empty + 1 → 1).
// Decrementing below zero transitions back to empty.
// Use [Counter.Empty] to test for the empty state.
//
// Counter implements [Linkable] — marshaling snapshots the current
// value; unmarshaling recreates a counter initialized to that value.
type Counter struct {
	val atomic.Uint64
}

// NewCounter creates a new counter in the empty state.
func NewCounter() *Counter {
	c := &Counter{}
	c.val.Store(counterEmptySentinel)
	return c
}

// Increment atomically adds delta and returns the new Count.
// If the counter is empty, it transitions to a non-empty state —
// even when delta is 0 (empty + 0 → 0).
func (c *Counter) Increment(delta uint64) Count {
	for {
		old := c.val.Load()
		var base uint64
		if old != counterEmptySentinel {
			base = old
		}
		next := base + delta
		if c.val.CompareAndSwap(old, next) {
			return Count(next)
		}
	}
}

// Decrement atomically subtracts delta and returns the new Count.
// If the current value is less than delta, the counter becomes empty
// and Count(0) is returned.  Decrementing an already-empty counter
// returns Count(0).
func (c *Counter) Decrement(delta uint64) Count {
	for {
		old := c.val.Load()
		if old == counterEmptySentinel {
			return Count(0)
		}
		if old < delta {
			if c.val.CompareAndSwap(old, counterEmptySentinel) {
				return Count(0)
			}
			continue
		}
		next := old - delta
		if c.val.CompareAndSwap(old, next) {
			return Count(next)
		}
	}
}

// Value returns the current count without modifying it.
// Returns Count(0) and true if the counter is empty.
func (c *Counter) Value() (Count, bool) {
	v := c.val.Load()
	if v == counterEmptySentinel {
		return 0, true
	}
	return Count(v), false
}

// Empty reports whether the counter is in the empty state.
func (c *Counter) Empty() bool {
	return c.val.Load() == counterEmptySentinel
}

// Reset atomically resets the counter and returns the previous value.
// If the counter was empty, returns Count(0).
//
// By default, Reset sets the counter to 0.  Pass true to reset to the
// empty state instead:
//
//	c.Reset()      // → 0
//	c.Reset(true)  // → empty
func (c *Counter) Reset(toEmpty ...bool) Count {
	target := uint64(0)
	if len(toEmpty) > 0 && toEmpty[0] {
		target = counterEmptySentinel
	}
	old := c.val.Swap(target)
	if old == counterEmptySentinel {
		return Count(0)
	}
	return Count(old)
}

// Marshal snapshots the counter's current state as 9 bytes:
// 1 byte empty flag + 8 bytes little-endian value.
func (c *Counter) Marshal() []byte {
	b := make([]byte, 9)
	v := c.val.Load()
	if v == counterEmptySentinel {
		b[0] = 1 // empty flag
		// value bytes stay zero
	} else {
		b[0] = 0
		binary.LittleEndian.PutUint64(b[1:], v)
	}
	return b
}

// Unmarshal restores a counter from a marshaled snapshot.
func (c *Counter) Unmarshal(data []byte) error {
	if len(data) < 9 {
		return fmt.Errorf("mental: Counter.Unmarshal: need 9 bytes, got %d", len(data))
	}
	if data[0] != 0 {
		c.val.Store(counterEmptySentinel)
	} else {
		c.val.Store(binary.LittleEndian.Uint64(data[1:]))
	}
	return nil
}

// NextCount returns the next value from the process-global monotonic
// counter (1, 2, 3, …).  Never returns 0.
//
// Thread-safe, lock-free.  For globally unique IDs across a spark
// cluster, combine with process identity.
func NextCount() Count {
	return Count(globalCounter.Add(1))
}

// globalCounter is the process-wide monotonic source, starting at 0
// so the first NextCount() returns 1.
var globalCounter atomic.Uint64
