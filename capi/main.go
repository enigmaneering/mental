package main

// This file exists solely to satisfy Go's requirement that c-shared builds
// have a main package. The actual C API is exported via //export directives
// in capi.go.

func main() {
	// Empty - this is never called when used as a C library
}
