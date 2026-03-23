//go:build linux || darwin

package mental

import (
	"runtime"
	"syscall"
)

func setupAtexitPipes() {
	var sig [2]int
	if err := syscall.Pipe(sig[:]); err != nil {
		return
	}
	var done [2]int
	if err := syscall.Pipe(done[:]); err != nil {
		syscall.Close(sig[0])
		syscall.Close(sig[1])
		return
	}

	atexitSignalFd = uintptr(sig[1]) // trampoline writes here
	atexitDoneFd = uintptr(done[0])  // trampoline reads here

	sigR := sig[0]
	doneW := done[1]

	go func() {
		runtime.LockOSThread()
		buf := make([]byte, 1)
		syscall.Read(sigR, buf)
		runDeferred()
		syscall.Write(doneW, []byte{1})
	}()
}
