package mental

import "testing"

func TestNextCountMonotonic(t *testing.T) {
	skipIfNoLibrary(t)
	a := NextCount()
	b := NextCount()
	c := NextCount()

	if b <= a || c <= b {
		t.Errorf("NextCount not monotonic: %d, %d, %d", a, b, c)
	}
	t.Logf("NextCount: %d, %d, %d", a, b, c)
}

func TestCounterCreateEmpty(t *testing.T) {
	ctr := NewCounter()
	if ctr == nil {
		t.Fatal("NewCounter returned nil")
	}
	if !ctr.Empty() {
		t.Error("new counter should be empty")
	}
}

func TestCounterIncrementDecrement(t *testing.T) {
	ctr := NewCounter()

	// Increment from empty → transitions to non-empty.
	val := ctr.Increment(5)
	t.Logf("after Increment(5): %d", val)

	val = ctr.Increment(3)
	t.Logf("after Increment(3): %d", val)

	if ctr.Empty() {
		t.Error("counter should not be empty after increment")
	}

	val = ctr.Decrement(2)
	t.Logf("after Decrement(2): %d", val)
}

func TestCounterReset(t *testing.T) {
	ctr := NewCounter()

	ctr.Increment(10)
	ctr.Increment(5)

	prev := ctr.Reset()
	t.Logf("Reset() returned previous: %d", prev)

	if ctr.Empty() {
		t.Error("counter should not be empty after Reset()")
	}
}

func TestCounterValue(t *testing.T) {
	ctr := NewCounter()

	_, empty := ctr.Value()
	if !empty {
		t.Error("new counter Value should report empty")
	}

	ctr.Increment(42)
	val, empty := ctr.Value()
	if empty {
		t.Error("counter should not be empty after increment")
	}
	t.Logf("Value after Increment(42): %d", val)
}

func TestCountMarshalUnmarshal(t *testing.T) {
	c := Count(12345)
	data := c.Marshal()
	if len(data) != 8 {
		t.Fatalf("Marshal() = %d bytes, want 8", len(data))
	}

	var c2 Count
	if err := c2.Unmarshal(data); err != nil {
		t.Fatalf("Unmarshal: %v", err)
	}
	if c2 != c {
		t.Errorf("round-trip: got %d, want %d", c2, c)
	}
}
