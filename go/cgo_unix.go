//go:build linux || darwin

package mental

/*
// This file provides the platform-specific atexit callback implementation.
*/
import "C"
import "syscall"

//export mentalGoAtexitCallback
func mentalGoAtexitCallback() {
	buf := []byte{1}
	syscall.Write(int(atexitSignalFd), buf)
	readBuf := make([]byte, 1)
	syscall.Read(int(atexitDoneFd), readBuf)
}
