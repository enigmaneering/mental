package main

import (
	"fmt"
	"sync"
	"time"

	"git.enigmaneering.org/mental"
)

// This example demonstrates the "irrational reference" pattern where
// multiple observers see different temporal slices of the same GPU memory.
func main() {
	fmt.Println("=== Observable References Example ===")

	// Allocate shared GPU buffer (automatically freed by GC)
	ref := mental.Thought.Alloc(64)

	// Initialize with zeros
	ref.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = 0
		}
		return data
})

	var wg sync.WaitGroup

	// Observer 1: Reads every 100ms
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			time.Sleep(100 * time.Millisecond)
			data := ref.Observe()
			fmt.Printf("Observer 1 sees: %v (sum=%d)\n", data[:8], sum(data))
		}
	}()

	// Observer 2: Reads every 150ms
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 3; i++ {
			time.Sleep(150 * time.Millisecond)
			data := ref.Observe()
			fmt.Printf("Observer 2 sees: %v (sum=%d)\n", data[:8], sum(data))
		}
	}()

	// Mutator: Increments all values every 50ms
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 10; i++ {
			time.Sleep(50 * time.Millisecond)
			ref.Mutate(func(data []byte) []byte {
				for j := range data {
					data[j]++
				}
				return data
})
			fmt.Printf("Mutator: incremented all values\n")
		}
	}()

	wg.Wait()

	// Final observation
	final := ref.Observe()
	fmt.Printf("\nFinal state: %v (sum=%d)\n", final[:8], sum(final))
	fmt.Println("\n=== Complete ===")
	fmt.Println("\nNote: Observers saw different states at different times.")
	fmt.Println("Between observations, the data changed multiple times.")
	fmt.Println("This is the 'irrational reference' pattern - no history tracking,")
	fmt.Println("only current observable state.")
}

func sum(data []byte) int {
	s := 0
	for _, v := range data {
		s += int(v)
	}
	return s
}
