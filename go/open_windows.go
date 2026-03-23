//go:build windows

package mental

import (
	"fmt"
	"syscall"
)

func openLibrary(path string) (uintptr, error) {
	h, err := syscall.LoadLibrary(path)
	if err != nil {
		return 0, fmt.Errorf("LoadLibrary %s: %w", path, err)
	}
	return uintptr(h), nil
}

func closeLibrary(handle uintptr) {
	syscall.FreeLibrary(syscall.Handle(handle))
}

func lookupSymbol(handle uintptr, name string) (uintptr, error) {
	addr, err := syscall.GetProcAddress(syscall.Handle(handle), name)
	if err != nil {
		return 0, fmt.Errorf("GetProcAddress %s: %w", name, err)
	}
	return addr, nil
}
