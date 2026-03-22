#include "textflag.h"

// SysV AMD64 ABI: args in RDI, RSI, RDX, RCX, R8, R9. Return in RAX.
// Frame size 16 ensures correct 16-byte stack alignment for C calls
// (accounts for Go's automatic BP save on frame > 0).

// func call0(fn uintptr) uintptr
TEXT ·call0(SB), NOSPLIT, $16-16
	MOVQ fn+0(FP), R10
	CALL R10
	MOVQ AX, ret+8(FP)
	RET

// func call1(fn, a1 uintptr) uintptr
TEXT ·call1(SB), NOSPLIT, $16-24
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), DI
	CALL R10
	MOVQ AX, ret+16(FP)
	RET

// func call2(fn, a1, a2 uintptr) uintptr
TEXT ·call2(SB), NOSPLIT, $16-32
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), DI
	MOVQ a2+16(FP), SI
	CALL R10
	MOVQ AX, ret+24(FP)
	RET

// func call3(fn, a1, a2, a3 uintptr) uintptr
TEXT ·call3(SB), NOSPLIT, $16-40
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), DI
	MOVQ a2+16(FP), SI
	MOVQ a3+24(FP), DX
	CALL R10
	MOVQ AX, ret+32(FP)
	RET

// func call4(fn, a1, a2, a3, a4 uintptr) uintptr
TEXT ·call4(SB), NOSPLIT, $16-48
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), DI
	MOVQ a2+16(FP), SI
	MOVQ a3+24(FP), DX
	MOVQ a4+32(FP), CX
	CALL R10
	MOVQ AX, ret+40(FP)
	RET

// func call5(fn, a1, a2, a3, a4, a5 uintptr) uintptr
TEXT ·call5(SB), NOSPLIT, $16-56
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), DI
	MOVQ a2+16(FP), SI
	MOVQ a3+24(FP), DX
	MOVQ a4+32(FP), CX
	MOVQ a5+40(FP), R8
	CALL R10
	MOVQ AX, ret+48(FP)
	RET

// Dynamic loader trampolines for dlopen/dlsym/dlclose/dlerror.
// These call through the symbols imported via //go:cgo_import_dynamic
// in open_linux.go.

// func sysDlopen(path *byte, flags int32) uintptr
TEXT ·sysDlopen(SB), NOSPLIT, $16-24
	MOVQ path+0(FP), DI
	MOVL flags+8(FP), SI
	XORQ AX, AX
	CALL libc_dlopen(SB)
	MOVQ AX, ret+16(FP)
	RET

// func sysDlsym(handle uintptr, symbol *byte) uintptr
TEXT ·sysDlsym(SB), NOSPLIT, $16-24
	MOVQ handle+0(FP), DI
	MOVQ symbol+8(FP), SI
	XORQ AX, AX
	CALL libc_dlsym(SB)
	MOVQ AX, ret+16(FP)
	RET

// func sysDlclose(handle uintptr) int32
TEXT ·sysDlclose(SB), NOSPLIT, $16-12
	MOVQ handle+0(FP), DI
	XORQ AX, AX
	CALL libc_dlclose(SB)
	MOVL AX, ret+8(FP)
	RET

// func sysDlerror() uintptr
TEXT ·sysDlerror(SB), NOSPLIT, $16-8
	XORQ AX, AX
	CALL libc_dlerror(SB)
	MOVQ AX, ret+0(FP)
	RET

// C-callable atexit trampoline. Writes to atexitSignalFd to wake the Go
// cleanup goroutine, then blocks on atexitDoneFd until cleanup completes.
// Uses raw Linux syscalls — no Go runtime or libc dependency.
//
// func atexitTrampoline()
TEXT ·atexitTrampoline(SB), NOSPLIT, $0-0
	// write(atexitSignalFd, &atexitBuf, 1)
	MOVQ $1, AX                      // SYS_write
	MOVQ ·atexitSignalFd(SB), DI
	LEAQ ·atexitBuf(SB), SI
	MOVQ $1, DX
	SYSCALL
	// read(atexitDoneFd, &atexitBuf, 1)  — blocks until Go signals done
	XORQ AX, AX                      // SYS_read = 0
	MOVQ ·atexitDoneFd(SB), DI
	LEAQ ·atexitBuf(SB), SI
	MOVQ $1, DX
	SYSCALL
	RET
