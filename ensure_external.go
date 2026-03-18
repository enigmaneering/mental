// +build ignore

// This file is used by go:generate and the Makefile to ensure external libraries
// are downloaded before CGo compilation attempts to link against them.
//
// Usage:
//   go run soul/core/mental/ensure_external.go
//   or
//   make
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"git.enigmaneering.org/enigmatic/gpu"
)

func main() {
	// Determine the external directory relative to this file
	// This file is in: life/soul/core/mental/
	// External dir is:  life/external/
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		fmt.Fprintln(os.Stderr, "Failed to determine source file location")
		os.Exit(1)
	}

	mentalDir := filepath.Dir(filename)
	// This file is in: enigmatic/mental/
	// External dir is:  enigmatic/external/
	externalDir := filepath.Join(mentalDir, "..", "external")
	externalDir = filepath.Clean(externalDir)

	// Set environment variable for external library to use
	os.Setenv("ENIGMATIC_GOFETCH_DIRECTORY", externalDir)

	fmt.Println("================================================================")
	fmt.Println("Ensuring external shader compilation libraries are present...")
	fmt.Println("================================================================")
	fmt.Printf("Target directory: %s\n\n", externalDir)

	// Download libraries if missing
	if err := gpu.EnsureLibraries(); err != nil {
		fmt.Fprintf(os.Stderr, "\nERROR: Failed to ensure libraries: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("\n================================================================")
	fmt.Println("External libraries ready!")
	fmt.Println("================================================================")
}
