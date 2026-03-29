//go:build windows

package mental

/*
// This file provides the platform-specific atexit callback implementation.
*/
import "C"
import "syscall"

//export mentalGoAtexitCallback
func mentalGoAtexitCallback() {
	buf := []byte{1}
	syscall.Write(syscall.Handle(atexitSignalFd), buf)
	readBuf := make([]byte, 1)
	syscall.Read(syscall.Handle(atexitDoneFd), readBuf)
}
