package mental

import (
	"sync"
	"testing"
	"unsafe"
)

// testData is a fixed-size type for reference tests.
type testData [256]byte

func TestUUID(t *testing.T) {
	skipIfNoLibrary(t)
	uuid := UUID()
	if len(uuid) != 32 {
		t.Fatalf("UUID() = %q (len=%d), want 32 hex chars", uuid, len(uuid))
	}
	for _, c := range uuid {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			t.Errorf("UUID contains non-hex char: %q in %q", string(c), uuid)
			break
		}
	}
	t.Logf("UUID: %s", uuid)
}

func TestUUIDIdempotent(t *testing.T) {
	skipIfNoLibrary(t)
	a := UUID()
	b := UUID()
	if a != b {
		t.Errorf("UUID not idempotent: %q != %q", a, b)
	}
}

func TestReferenceCreateClose(t *testing.T) {
	skipIfNoLibrary(t)
	ref, _ := ReferenceCreate[testData, struct{}](RelationallyOpen)
	if ref == nil || ref.Handle() == 0 {
		t.Fatal("ReferenceCreate returned nil")
	}
	t.Logf("created reference: handle=%x, size=%d", ref.Handle(), ref.Size())

	if ref.Size() != int(unsafe.Sizeof(testData{})) {
		t.Errorf("Size() = %d, want %d", ref.Size(), unsafe.Sizeof(testData{}))
	}
	if !ref.IsOwner() {
		t.Error("creator should be owner")
	}

	ref.Close()
}

func TestReferenceDataReadWrite(t *testing.T) {
	skipIfNoLibrary(t)
	ref, _ := ReferenceCreate[testData, struct{}](RelationallyOpen)
	if ref == nil || ref.Handle() == 0 {
		t.Fatal("ReferenceCreate returned nil")
	}
	defer ref.Close()

	data := ref.Data()
	if data == nil {
		t.Fatal("Data() returned nil")
	}

	// Write a pattern.
	for i := range data {
		data[i] = byte(i & 0xFF)
	}

	// Read back and verify.
	for i := range data {
		if data[i] != byte(i&0xFF) {
			t.Errorf("data[%d] = %d, want %d", i, data[i], byte(i&0xFF))
			break
		}
	}
}

func TestReferenceObservability(t *testing.T) {
	skipIfNoLibrary(t)

	type slot struct{ Value float32 }

	ref, _ := ReferenceCreate[slot, struct{}](RelationallyOpen)
	if ref == nil || ref.Handle() == 0 {
		t.Fatal("ReferenceCreate returned nil")
	}
	defer ref.Close()

	known := []float32{0.0, 1.0, 42.0, -1.0, 999.0}

	data := ref.Data()
	if data == nil {
		t.Fatal("Data() returned nil")
	}
	data.Value = known[0]

	const iterations = 5000

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < iterations; i++ {
			data.Value = known[i%len(known)]
		}
	}()

	sawInvalid := false
	for i := 0; i < iterations; i++ {
		observed := data.Value
		valid := false
		for _, k := range known {
			if observed == k {
				valid = true
				break
			}
		}
		if !valid {
			t.Errorf("observer saw invalid value: %f", observed)
			sawInvalid = true
			break
		}
	}

	wg.Wait()
	if !sawInvalid {
		t.Log("observability: all reads returned known values")
	}
}
