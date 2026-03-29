//go:build windows

package mental

import (
	"runtime"
	"syscall"
)

func setupAtexitPipes() {
	var sigR, sigW syscall.Handle
	if err := syscall.CreatePipe(&sigR, &sigW, nil, 0); err != nil {
		return
	}
	var doneR, doneW syscall.Handle
	if err := syscall.CreatePipe(&doneR, &doneW, nil, 0); err != nil {
		syscall.CloseHandle(sigR)
		syscall.CloseHandle(sigW)
		return
	}

	atexitSignalFd = uintptr(sigW) // callback writes here
	atexitDoneFd = uintptr(doneR)  // callback reads here

	go func() {
		runtime.LockOSThread()
		buf := make([]byte, 1)
		syscall.Read(sigR, buf)
		runDeferred()
		syscall.Write(doneW, []byte{1})
	}()
}
