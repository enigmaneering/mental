//go:build windows

package mental

import (
	"runtime"
	"syscall"
)

// Windows-specific globals for the atexit trampoline.
// The trampoline calls WriteFile/ReadFile through these resolved addresses.
var writeFileAddr uintptr
var readFileAddr uintptr
var atexitWritten uint32

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

	atexitSignalFd = uintptr(sigW) // trampoline writes via WriteFile
	atexitDoneFd = uintptr(doneR)  // trampoline reads via ReadFile

	// Resolve kernel32 functions for the assembly trampoline.
	k32, err := syscall.LoadLibrary("kernel32.dll")
	if err == nil {
		writeFileAddr, _ = syscall.GetProcAddress(k32, "WriteFile")
		readFileAddr, _ = syscall.GetProcAddress(k32, "ReadFile")
	}

	go func() {
		runtime.LockOSThread()
		buf := make([]byte, 1)
		syscall.Read(sigR, buf)
		runDeferred()
		syscall.Write(doneW, []byte{1})
	}()
}
