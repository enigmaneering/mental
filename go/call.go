package mental

// Assembly trampolines for calling C functions through resolved pointers.
// Implemented in shim_GOOS_GOARCH.s for each supported platform.

func call0(fn uintptr) uintptr
func call1(fn, a1 uintptr) uintptr
func call2(fn, a1, a2 uintptr) uintptr
func call3(fn, a1, a2, a3 uintptr) uintptr
func call4(fn, a1, a2, a3, a4 uintptr) uintptr
func call5(fn, a1, a2, a3, a4, a5 uintptr) uintptr
