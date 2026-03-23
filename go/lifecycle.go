package mental

import (
	"sync"
	"unsafe"
)

// atexitSignalFd is the write end of the signal pipe (Unix fd / Windows HANDLE).
// The atexit trampoline writes here to wake the cleanup goroutine.
var atexitSignalFd uintptr

// atexitDoneFd is the read end of the done pipe.
// The atexit trampoline blocks reading here until cleanup completes.
var atexitDoneFd uintptr

// atexitBuf is a scratch byte used by the atexit trampoline for pipe I/O.
var atexitBuf [1]byte

var (
	deferredMu  sync.Mutex
	deferredFns []func(*sync.WaitGroup)
)

// Defer registers a function to be called during graceful process exit.
// Functions are called in LIFO order (last registered, first called).
// Each function receives a shared [sync.WaitGroup] that can be used to
// coordinate parallel cleanup work. After all functions return, the
// system waits on the WaitGroup before proceeding with final cleanup.
func Defer(fn func(*sync.WaitGroup)) {
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
// and registers the assembly trampoline with mental_atexit in the C library.
func setupLifecycle() {
	setupAtexitPipes() // platform-specific: creates pipes, starts goroutine

	// Get the raw code address of the assembly trampoline.
	fn := atexitTrampoline
	addr := **(**uintptr)(unsafe.Pointer(&fn))
	call1(ft.mentalAtexit, addr)
}
