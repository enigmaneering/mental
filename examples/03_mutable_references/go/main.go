package main

import (
	"encoding/binary"
	"fmt"
	"unsafe"

	"git.enigmaneering.org/mental"
)

func main() {
	fmt.Println("=== Mutable References Example ===")
	fmt.Println()
	fmt.Println("This example demonstrates automatic GPU buffer resizing when")
	fmt.Println("modifying Reference data. The system automatically handles")
	fmt.Println("GPU memory reallocation as your data grows or shrinks.")
	fmt.Println()

	// List available devices
	devices := mental.List()
	fmt.Printf("Using device: %s (%s)\n\n", devices[0].Name, devices[0].API)

	// Create a small buffer to track a growing list of numbers
	fmt.Println("Creating initial buffer with 3 numbers...")
	numbers := mental.Thought.Alloc(12) // 3 float32s = 12 bytes
	numbers.Mutate(func(data []byte) []byte {
		binary.LittleEndian.PutUint32(data[0:4], floatBits(1.0))
		binary.LittleEndian.PutUint32(data[4:8], floatBits(2.0))
		binary.LittleEndian.PutUint32(data[8:12], floatBits(3.0))
		return data
	})
	printFloatBuffer("Initial buffer", numbers)

	// Example 1: Grow the buffer by appending new numbers
	fmt.Println("\nExample 1: Growing buffer (appending 2 more numbers)...")
	numbers.Mutate(func(data []byte) []byte {
		// Append two more float32 values
		newData := make([]byte, len(data)+8)
		copy(newData, data)
		binary.LittleEndian.PutUint32(newData[12:16], floatBits(4.0))
		binary.LittleEndian.PutUint32(newData[16:20], floatBits(5.0))
		return newData
	})
	printFloatBuffer("After growing", numbers)

	// Example 2: Transform data while growing
	fmt.Println("\nExample 2: Transform and grow (square each number and add 0)...")
	numbers.Mutate(func(data []byte) []byte {
		count := len(data) / 4
		result := make([]byte, len(data)+4) // Add one more slot

		// Square existing numbers
		for i := 0; i < count; i++ {
			val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
			squared := val * val
			binary.LittleEndian.PutUint32(result[i*4:(i+1)*4], floatBits(squared))
		}

		// Add a zero at the end
		binary.LittleEndian.PutUint32(result[count*4:(count+1)*4], floatBits(0.0))
		return result
	})
	printFloatBuffer("After transformation", numbers)

	// Example 3: Filter (shrink) by removing zeros
	fmt.Println("\nExample 3: Shrinking buffer (removing zeros)...")
	numbers.Mutate(func(data []byte) []byte {
		count := len(data) / 4
		var filtered []float32

		// Keep only non-zero values
		for i := 0; i < count; i++ {
			val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
			if val != 0.0 {
				filtered = append(filtered, val)
			}
		}

		// Convert back to bytes
		result := make([]byte, len(filtered)*4)
		for i, val := range filtered {
			binary.LittleEndian.PutUint32(result[i*4:(i+1)*4], floatBits(val))
		}
		return result
	})
	printFloatBuffer("After filtering", numbers)

	// Example 4: Simple resize via Write
	fmt.Println("\nExample 4: Direct resize via Write()...")
	newData := make([]byte, 8) // Just 2 float32s
	binary.LittleEndian.PutUint32(newData[0:4], floatBits(100.0))
	binary.LittleEndian.PutUint32(newData[4:8], floatBits(200.0))
	numbers.Write(newData)
	printFloatBuffer("After Write", numbers)

	// Example 5: Build up a sequence dynamically
	fmt.Println("\nExample 5: Building Fibonacci sequence dynamically...")
	fib := mental.Thought.Alloc(8) // Start with just 2 numbers
	fib.Write([]byte{
		byte(floatBits(1.0)), byte(floatBits(1.0) >> 8), byte(floatBits(1.0) >> 16), byte(floatBits(1.0) >> 24),
		byte(floatBits(1.0)), byte(floatBits(1.0) >> 8), byte(floatBits(1.0) >> 16), byte(floatBits(1.0) >> 24),
	})

	// Generate next 8 Fibonacci numbers
	for i := 0; i < 8; i++ {
		fib.Mutate(func(data []byte) []byte {
			count := len(data) / 4
			if count < 2 {
				return data
			}

			// Get last two numbers
			a := bitsToFloat(binary.LittleEndian.Uint32(data[(count-2)*4 : (count-1)*4]))
			b := bitsToFloat(binary.LittleEndian.Uint32(data[(count-1)*4 : count*4]))
			next := a + b

			// Append next number
			newData := make([]byte, len(data)+4)
			copy(newData, data)
			binary.LittleEndian.PutUint32(newData[count*4:(count+1)*4], floatBits(next))
			return newData
		})
	}
	printFloatBuffer("Fibonacci sequence", fib)

	fmt.Println("\n=== Complete ===")
	fmt.Println()
	fmt.Println("Key takeaway: References automatically resize on the GPU.")
	fmt.Println("You don't need to manually manage buffer allocation!")
}

// Helper functions for float32 conversion
func floatBits(f float32) uint32 {
	return binary.LittleEndian.Uint32((*(*[4]byte)(unsafe.Pointer(&f)))[:])
}

func bitsToFloat(bits uint32) float32 {
	return *(*float32)(unsafe.Pointer(&bits))
}

func printFloatBuffer(label string, ref *mental.Reference) {
	data := ref.Observe()
	count := len(data) / 4
	fmt.Printf("%s (%d bytes, %d floats): [", label, ref.Size(), count)
	for i := 0; i < count; i++ {
		val := bitsToFloat(binary.LittleEndian.Uint32(data[i*4 : (i+1)*4]))
		if i > 0 {
			fmt.Print(", ")
		}
		fmt.Printf("%.1f", val)
	}
	fmt.Println("]")
}
