//go:build linux || darwin

package mental

import (
	"errors"
	"unsafe"
)

const rtldLazy = 0x00001

// Implemented in shim_GOOS_GOARCH.s — call through cgo_import_dynamic symbols.
func sysDlopen(path *byte, flags int32) uintptr
func sysDlsym(handle uintptr, symbol *byte) uintptr
func sysDlclose(handle uintptr) int32
func sysDlerror() uintptr

func openLibrary(path string) (uintptr, error) {
	cpath := make([]byte, len(path)+1)
	copy(cpath, path)
	handle := sysDlopen(&cpath[0], rtldLazy)
	if handle == 0 {
		return 0, errors.New("dlopen: " + path + ": " + dlerror())
	}
	return handle, nil
}

func closeLibrary(handle uintptr) {
	sysDlclose(handle)
}

func lookupSymbol(handle uintptr, name string) (uintptr, error) {
	cname := make([]byte, len(name)+1)
	copy(cname, name)
	addr := sysDlsym(handle, &cname[0])
	if addr == 0 {
		return 0, errors.New("dlsym: " + name + ": " + dlerror())
	}
	return addr, nil
}

func dlerror() string {
	p := sysDlerror()
	if p == 0 {
		return "unknown error"
	}
	return goString(p)
}

func goString(p uintptr) string {
	if p == 0 {
		return ""
	}
	ptr := unsafe.Pointer(p)
	var n int
	for *(*byte)(unsafe.Add(ptr, n)) != 0 {
		n++
	}
	return string(unsafe.Slice((*byte)(ptr), n))
}
