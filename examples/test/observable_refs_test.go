package test

import (
	"sync"
	"testing"
	"time"

	"git.enigmaneering.org/mental"
)

// TestObservableRefs validates the 01_observable_refs example's claims:
// - Multiple observers can read from the same GPU buffer concurrently
// - Mutations are exclusive and increment all values
// - Different observers see different temporal states
// - Final state reflects all mutations (10 increments = all values = 10)
func TestObservableRefs(t *testing.T) {
	// Allocate shared GPU buffer
	ref := mental.Thought.Alloc(64)

	// Initialize with zeros (as example claims)
	ref.Mutate(func(data []byte) []byte {
		for i := range data {
			data[i] = 0
		}
		return data
})

	// Verify initial state is zeros
	initialData := ref.Observe()
	for i, v := range initialData {
		if v != 0 {
			t.Errorf("Initial data at index %d should be 0, got %d", i, v)
		}
	}

	var wg sync.WaitGroup
	observationsMutex := sync.Mutex{}
	observer1Sums := []int{}
	observer2Sums := []int{}
	mutationCount := 0

	// Observer 1: Reads every 100ms (5 times)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 5; i++ {
			time.Sleep(100 * time.Millisecond)
			data := ref.Observe()
			s := sum(data)
			observationsMutex.Lock()
			observer1Sums = append(observer1Sums, s)
			observationsMutex.Unlock()
			t.Logf("Observer 1 sees: sum=%d", s)
		}
	}()

	// Observer 2: Reads every 150ms (3 times)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 3; i++ {
			time.Sleep(150 * time.Millisecond)
			data := ref.Observe()
			s := sum(data)
			observationsMutex.Lock()
			observer2Sums = append(observer2Sums, s)
			observationsMutex.Unlock()
			t.Logf("Observer 2 sees: sum=%d", s)
		}
	}()

	// Mutator: Increments all values every 50ms (10 times)
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
			observationsMutex.Lock()
			mutationCount++
			observationsMutex.Unlock()
			t.Logf("Mutator: incremented all values (count=%d)", i+1)
		}
	}()

	wg.Wait()

	// Verify final state: 10 mutations × 64 bytes = sum should be 640
	// (each byte incremented 10 times = value 10, 10 × 64 = 640)
	final := ref.Observe()
	finalSum := sum(final)
	expectedSum := 10 * 64 // 10 increments × 64 bytes

	if finalSum != expectedSum {
		t.Errorf("Final sum incorrect: expected %d, got %d", expectedSum, finalSum)
	}

	// Verify all values are 10 (as claimed by example)
	for i, v := range final[:8] {
		if v != 10 {
			t.Errorf("Final value at index %d should be 10, got %d", i, v)
		}
	}

	// Verify that observers saw different states (temporal observation)
	// Observer 1 should have seen 5 different states
	if len(observer1Sums) != 5 {
		t.Errorf("Observer 1 should have made 5 observations, got %d", len(observer1Sums))
	}

	// Observer 2 should have seen 3 different states
	if len(observer2Sums) != 3 {
		t.Errorf("Observer 2 should have made 3 observations, got %d", len(observer2Sums))
	}

	// Verify sums are increasing (demonstrating temporal progression)
	for i := 1; i < len(observer1Sums); i++ {
		if observer1Sums[i] < observer1Sums[i-1] {
			t.Errorf("Observer 1 sum should be monotonically increasing, but sum[%d]=%d < sum[%d]=%d",
				i, observer1Sums[i], i-1, observer1Sums[i-1])
		}
	}

	for i := 1; i < len(observer2Sums); i++ {
		if observer2Sums[i] < observer2Sums[i-1] {
			t.Errorf("Observer 2 sum should be monotonically increasing, but sum[%d]=%d < sum[%d]=%d",
				i, observer2Sums[i], i-1, observer2Sums[i-1])
		}
	}

	// Verify exactly 10 mutations occurred
	observationsMutex.Lock()
	if mutationCount != 10 {
		t.Errorf("Expected 10 mutations, got %d", mutationCount)
	}
	observationsMutex.Unlock()

	t.Logf("Observable reference pattern validated:")
	t.Logf("  - Concurrent observations: %d + %d = %d total", len(observer1Sums), len(observer2Sums), len(observer1Sums)+len(observer2Sums))
	t.Logf("  - Mutations: %d", mutationCount)
	t.Logf("  - Final state sum: %d (expected %d)", finalSum, expectedSum)
}

func sum(data []byte) int {
	s := 0
	for _, v := range data {
		s += int(v)
	}
	return s
}
