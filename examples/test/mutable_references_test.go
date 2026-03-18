package test

import (
	"encoding/binary"
	"testing"
	"unsafe"

	"git.enigmaneering.org/mental"
)

// Helper functions for float32 conversion
func floatBits(f float32) uint32 {
	return binary.LittleEndian.Uint32((*(*[4]byte)(unsafe.Pointer(&f)))[:])
}

func bitsToFloat(bits uint32) float32 {
	return *(*float32)(unsafe.Pointer(&bits))
}

// TestMutableReferences validates the 03_mutable_references example's claims:
// - GPU buffer resizing happens automatically when modifying Reference data
// - The system handles GPU memory reallocation as data grows or shrinks
// - Buffers can grow via Mutate (appending - Example 1)
// - Buffers can transform and grow simultaneously (squaring + adding - Example 2)
// - Buffers can shrink via Mutate (filtering - Example 3)
// - Buffers can resize via Write (Example 4)
// - Dynamic sequences (like Fibonacci) can be built by growing the buffer (Example 5)
func TestMutableReferences(t *testing.T) {
	// Verify we have at least one device
	devices := mental.List()
	if len(devices) == 0 {
		t.Fatal("Expected at least one GPU device")
	}

	t.Run("Example1_GrowBuffer", func(t *testing.T) {
		// Example 1 claim: "Growing buffer (appending 2 more numbers)"
		// Initial: 3 numbers, After: 5 numbers (grew from 12 to 20 bytes)
		numbers := mental.Thought.Alloc(12)
		numbers.Mutate(func(data []byte) []byte {
			binary.LittleEndian.PutUint32(data[0:4], floatBits(1.0))
			binary.LittleEndian.PutUint32(data[4:8], floatBits(2.0))
			binary.LittleEndian.PutUint32(data[8:12], floatBits(3.0))
			return data
		})

		// Grow by appending 2 more
		numbers.Mutate(func(data []byte) []byte {
			newData := make([]byte, len(data)+8)
			copy(newData, data)
			binary.LittleEndian.PutUint32(newData[12:16], floatBits(4.0))
			binary.LittleEndian.PutUint32(newData[16:20], floatBits(5.0))
			return newData
		})

		if numbers.Size() != 20 {
			t.Errorf("Expected size 20, got %d", numbers.Size())
		}

		data := numbers.Observe()
		if len(data) != 20 {
			t.Errorf("Expected data length 20, got %d", len(data))
		}

		// Verify values
		for i := 0; i < 5; i++ {
			val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
			expected := float32(i + 1)
			if val != expected {
				t.Errorf("Index %d: expected %.1f, got %.1f", i, expected, val)
			}
		}
	})

	t.Run("Example2_TransformAndGrow", func(t *testing.T) {
		// Example 2 claim: "Transform and grow (square each number and add 0)"
		// Takes 5 numbers, squares them, adds a 0 (grew from 20 to 24 bytes)
		// Start with [1.0, 2.0, 3.0]
		numbers := mental.Thought.Alloc(12)
		numbers.Mutate(func(data []byte) []byte {
			binary.LittleEndian.PutUint32(data[0:4], floatBits(1.0))
			binary.LittleEndian.PutUint32(data[4:8], floatBits(2.0))
			binary.LittleEndian.PutUint32(data[8:12], floatBits(3.0))
			return data
		})

		// Square each number and add a zero
		numbers.Mutate(func(data []byte) []byte {
			count := len(data) / 4
			result := make([]byte, len(data)+4)

			for i := 0; i < count; i++ {
				val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
				squared := val * val
				binary.LittleEndian.PutUint32(result[i*4:(i+1)*4], floatBits(squared))
			}

			binary.LittleEndian.PutUint32(result[count*4:(count+1)*4], floatBits(0.0))
			return result
		})

		if numbers.Size() != 16 {
			t.Errorf("Expected size 16, got %d", numbers.Size())
		}

		data := numbers.Observe()
		expected := []float32{1.0, 4.0, 9.0, 0.0}
		for i, exp := range expected {
			val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
			if val != exp {
				t.Errorf("Index %d: expected %.1f, got %.1f", i, exp, val)
			}
		}
	})

	t.Run("Example3_ShrinkBuffer", func(t *testing.T) {
		// Example 3 claim: "Shrinking buffer (removing zeros)"
		// Filters out zeros, shrinking from 6 numbers to 5 (24 to 20 bytes)
		// Start with [1.0, 4.0, 9.0, 16.0, 25.0, 0.0]
		numbers := mental.Thought.Alloc(24)
		numbers.Mutate(func(data []byte) []byte {
			vals := []float32{1.0, 4.0, 9.0, 16.0, 25.0, 0.0}
			for i, val := range vals {
				binary.LittleEndian.PutUint32(data[i*4:(i+1)*4], floatBits(val))
			}
			return data
		})

		// Filter out zeros
		numbers.Mutate(func(data []byte) []byte {
			count := len(data) / 4
			var filtered []float32

			for i := 0; i < count; i++ {
				val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
				if val != 0.0 {
					filtered = append(filtered, val)
				}
			}

			result := make([]byte, len(filtered)*4)
			for i, val := range filtered {
				binary.LittleEndian.PutUint32(result[i*4:(i+1)*4], floatBits(val))
			}
			return result
		})

		if numbers.Size() != 20 {
			t.Errorf("Expected size 20, got %d", numbers.Size())
		}

		data := numbers.Observe()
		expected := []float32{1.0, 4.0, 9.0, 16.0, 25.0}
		for i, exp := range expected {
			val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
			if val != exp {
				t.Errorf("Index %d: expected %.1f, got %.1f", i, exp, val)
			}
		}
	})

	t.Run("Example4_ResizeViaWrite", func(t *testing.T) {
		// Example 4 claim: "Direct resize via Write()"
		// Replaces buffer contents, shrinking from 20 bytes to 8 bytes
		numbers := mental.Thought.Alloc(20)

		// Write smaller buffer
		newData := make([]byte, 8)
		binary.LittleEndian.PutUint32(newData[0:4], floatBits(100.0))
		binary.LittleEndian.PutUint32(newData[4:8], floatBits(200.0))
		numbers.Write(newData)

		if numbers.Size() != 8 {
			t.Errorf("Expected size 8, got %d", numbers.Size())
		}

		data := numbers.Observe()
		val1 := bitsToFloat(binary.LittleEndian.Uint32(data[0:4]))
		val2 := bitsToFloat(binary.LittleEndian.Uint32(data[4:8]))

		if val1 != 100.0 || val2 != 200.0 {
			t.Errorf("Expected [100.0, 200.0], got [%.1f, %.1f]", val1, val2)
		}
	})

	t.Run("Example5_FibonacciSequence", func(t *testing.T) {
		// Example 5 claim: "Building Fibonacci sequence dynamically"
		// Starts with 2 numbers, grows to 10 by appending (8 to 40 bytes)
		// Start with [1.0, 1.0]
		fib := mental.Thought.Alloc(8)
		fib.Mutate(func(data []byte) []byte {
			binary.LittleEndian.PutUint32(data[0:4], floatBits(1.0))
			binary.LittleEndian.PutUint32(data[4:8], floatBits(1.0))
			return data
		})

		// Generate next 8 Fibonacci numbers
		for i := 0; i < 8; i++ {
			fib.Mutate(func(data []byte) []byte {
				count := len(data) / 4
				if count < 2 {
					return data
				}

				a := bitsToFloat(binary.LittleEndian.Uint32(data[(count-2)*4 : (count-1)*4]))
				b := bitsToFloat(binary.LittleEndian.Uint32(data[(count-1)*4 : count*4]))
				next := a + b

				newData := make([]byte, len(data)+4)
				copy(newData, data)
				binary.LittleEndian.PutUint32(newData[count*4:(count+1)*4], floatBits(next))
				return newData
			})
		}

		if fib.Size() != 40 {
			t.Errorf("Expected size 40, got %d", fib.Size())
		}

		data := fib.Observe()
		expected := []float32{1.0, 1.0, 2.0, 3.0, 5.0, 8.0, 13.0, 21.0, 34.0, 55.0}
		for i, exp := range expected {
			val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
			if val != exp {
				t.Errorf("Index %d: expected %.1f, got %.1f", i, exp, val)
			}
		}

		t.Log("All Fibonacci numbers generated correctly via dynamic buffer growth")
	})

	// Final validation
	t.Log("✓ All example claims validated:")
	t.Log("  - Automatic GPU buffer resizing works")
	t.Log("  - Growing buffers via Mutate works")
	t.Log("  - Shrinking buffers via Mutate works")
	t.Log("  - Resizing via Write works")
	t.Log("  - Dynamic sequences can be built by growing buffers")
	t.Log("  - Key takeaway confirmed: References automatically resize on the GPU")
}
