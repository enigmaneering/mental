package mental

/*
#include "mental.h"

// Forward declaration — defined in cgo.go via //export.
extern void mentalGoAtexitCallback(void);
*/
import "C"
import (
	"sync"
	"unsafe"
)

// atexitSignalFd is the write end of the signal pipe (Unix fd / Windows HANDLE).
// The atexit callback writes here to wake the cleanup goroutine.
var atexitSignalFd uintptr

// atexitDoneFd is the read end of the done pipe.
// The atexit callback blocks reading here until cleanup completes.
var atexitDoneFd uintptr

var (
	deferredMu    sync.Mutex
	deferredFns   []func(*sync.WaitGroup)
	lifecycleOnce sync.Once
)

// ensureLifecycle triggers lazy lifecycle setup on first call.
func ensureLifecycle() {
	lifecycleOnce.Do(setupLifecycle)
}

// Defer registers a function to be called during graceful process exit.
// Functions are called in LIFO order (last registered, first called).
// Each function receives a shared [sync.WaitGroup] that can be used to
// coordinate parallel cleanup work. After all functions return, the
// system waits on the WaitGroup before proceeding with final cleanup.
func Defer(fn func(*sync.WaitGroup)) {
	ensureLifecycle()
	deferredMu.Lock()
	deferredFns = append(deferredFns, fn)
	deferredMu.Unlock()
}

// runDeferred executes all registered Defer callbacks in LIFO order.
func runDeferred() {
	deferredMu.Lock()
	fns := make([]func(*sync.WaitGroup), len(deferredFns))
	copy(fns, deferredFns)
	deferredMu.Unlock()

	var wg sync.WaitGroup
	for i := len(fns) - 1; i >= 0; i-- {
		fns[i](&wg)
	}
	wg.Wait()
}

// setupLifecycle creates the atexit pipe pair, starts the cleanup goroutine,
// and registers the Go callback with mental_atexit in the C library.
func setupLifecycle() {
	setupAtexitPipes() // platform-specific: creates pipes, starts goroutine

	// Register the Go atexit callback with the C library.
	// (*[0]byte) is Go's representation of a C function pointer.
	C.mental_atexit((*[0]byte)(unsafe.Pointer(C.mentalGoAtexitCallback)))
}
