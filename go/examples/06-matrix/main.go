// Example 06: Matrix (Digit-by-Digit Parallel Arithmetic)
//
// Demonstrates the matrix system which decomposes values into digits,
// operates on each digit position in parallel across all elements,
// then recomposes with carry propagation.
//
// Tests:
//   1. Basic addition: [1,2,3,4] + [10,20,30,40] = [11,22,33,44]
//   2. Carry propagation: [999,1,50] + [1,999,950] = [1000,1000,1000]
//   3. Subtraction: [100,50,25] - [1,1,1] = [99,49,24]
//
// Run:
//
//	go build -o matrix . && ./matrix
package main

import (
	"fmt"
	"os"

	mental "git.enigmaneering.org/mental/go"
)

func main() {
	mental.On(func(e mental.Epiphany) {
		switch e.(type) {
		case mental.EpiphanyInception:
			run()
		default:
		}
	})
}

func run() {
	if mental.DeviceCount() == 0 {
		fmt.Println("no GPU devices — skipping matrix example")
		os.Exit(0)
	}

	device := mental.DeviceGet(0)
	fmt.Printf("Device: %s (%s)\n\n", device.Name(), device.APIName())

	ok := true
	ok = testBasicAdd(device) && ok
	ok = testCarryAdd(device) && ok
	ok = testSub(device) && ok

	if ok {
		fmt.Println("✓ All matrix tests passed!")
	} else {
		fmt.Println("✗ Some tests failed")
		os.Exit(1)
	}
	os.Exit(0)
}

func testBasicAdd(dev mental.Device) bool {
	fmt.Println("--- Test 1: Basic Addition ---")
	a := []uint32{1, 2, 3, 4}
	b := []uint32{10, 20, 30, 40}
	expected := []uint32{11, 22, 33, 44}
	return runTest(dev, mental.MatrixAdd, a, b, expected, "+")
}

func testCarryAdd(dev mental.Device) bool {
	fmt.Println("--- Test 2: Addition with Carries ---")
	a := []uint32{999, 1, 50}
	b := []uint32{1, 999, 950}
	expected := []uint32{1000, 1000, 1000}
	return runTest(dev, mental.MatrixAdd, a, b, expected, "+")
}

func testSub(dev mental.Device) bool {
	fmt.Println("--- Test 3: Subtraction ---")
	a := []uint32{100, 50, 25}
	b := []uint32{1, 1, 1}
	expected := []uint32{99, 49, 24}
	return runTest(dev, mental.MatrixSub, a, b, expected, "-")
}

func runTest(dev mental.Device, op mental.MatrixOp, a, b, expected []uint32, opStr string) bool {
	n := len(a)

	refA := mental.MatrixMakeBuffer(dev, n, a)
	refB := mental.MatrixMakeBuffer(dev, n, b)
	refOut := mental.MatrixMakeEmptyBuffer(dev, n)
	defer mental.MatrixFreeBuffer(refA)
	defer mental.MatrixFreeBuffer(refB)
	defer mental.MatrixFreeBuffer(refOut)

	mat, err := mental.CreateMatrix[uint32](dev, op, 10, n)
	if err != nil {
		fmt.Fprintf(os.Stderr, "  create matrix: %v\n", err)
		return false
	}
	defer mat.Finalize()

	if err := mat.Execute(refA, refB, refOut); err != nil {
		fmt.Fprintf(os.Stderr, "  execute: %v\n", err)
		return false
	}

	out := mental.MatrixReadBuffer(refOut, n)

	fmt.Printf("  A:        %v\n", a)
	fmt.Printf("  B:        %v\n", b)
	fmt.Printf("  A %s B:    %v\n", opStr, out)
	fmt.Printf("  Expected: %v\n", expected)

	ok := true
	for i := range out {
		if out[i] != expected[i] {
			fmt.Printf("  MISMATCH at [%d]: got %d, expected %d\n", i, out[i], expected[i])
			ok = false
		}
	}
	if ok {
		fmt.Println("  ✓ Pass")
	} else {
		fmt.Println("  ✗ Fail")
	}
	fmt.Println()
	return ok
}
