#include "textflag.h"

// AAPCS64 ABI: args in R0-R7, return in R0.
// R9 used as scratch for function pointer (not an arg register).
// Frame size 16 for Go's automatic (FP, LR) save.

// func call0(fn uintptr) uintptr
TEXT ·call0(SB), NOSPLIT, $16-16
	MOVD fn+0(FP), R9
	BL   (R9)
	MOVD R0, ret+8(FP)
	RET

// func call1(fn, a1 uintptr) uintptr
TEXT ·call1(SB), NOSPLIT, $16-24
	MOVD fn+0(FP), R9
	MOVD a1+8(FP), R0
	BL   (R9)
	MOVD R0, ret+16(FP)
	RET

// func call2(fn, a1, a2 uintptr) uintptr
TEXT ·call2(SB), NOSPLIT, $16-32
	MOVD fn+0(FP), R9
	MOVD a1+8(FP), R0
	MOVD a2+16(FP), R1
	BL   (R9)
	MOVD R0, ret+24(FP)
	RET

// func call3(fn, a1, a2, a3 uintptr) uintptr
TEXT ·call3(SB), NOSPLIT, $16-40
	MOVD fn+0(FP), R9
	MOVD a1+8(FP), R0
	MOVD a2+16(FP), R1
	MOVD a3+24(FP), R2
	BL   (R9)
	MOVD R0, ret+32(FP)
	RET

// func call4(fn, a1, a2, a3, a4 uintptr) uintptr
TEXT ·call4(SB), NOSPLIT, $16-48
	MOVD fn+0(FP), R9
	MOVD a1+8(FP), R0
	MOVD a2+16(FP), R1
	MOVD a3+24(FP), R2
	MOVD a4+32(FP), R3
	BL   (R9)
	MOVD R0, ret+40(FP)
	RET

// func call5(fn, a1, a2, a3, a4, a5 uintptr) uintptr
TEXT ·call5(SB), NOSPLIT, $16-56
	MOVD fn+0(FP), R9
	MOVD a1+8(FP), R0
	MOVD a2+16(FP), R1
	MOVD a3+24(FP), R2
	MOVD a4+32(FP), R3
	MOVD a5+40(FP), R4
	BL   (R9)
	MOVD R0, ret+48(FP)
	RET

// func sysDlopen(path *byte, flags int32) uintptr
TEXT ·sysDlopen(SB), NOSPLIT, $16-24
	MOVD  path+0(FP), R0
	MOVW  flags+8(FP), R1
	BL    libc_dlopen(SB)
	MOVD  R0, ret+16(FP)
	RET

// func sysDlsym(handle uintptr, symbol *byte) uintptr
TEXT ·sysDlsym(SB), NOSPLIT, $16-24
	MOVD handle+0(FP), R0
	MOVD symbol+8(FP), R1
	BL   libc_dlsym(SB)
	MOVD R0, ret+16(FP)
	RET

// func sysDlclose(handle uintptr) int32
TEXT ·sysDlclose(SB), NOSPLIT, $16-12
	MOVD handle+0(FP), R0
	BL   libc_dlclose(SB)
	MOVW R0, ret+8(FP)
	RET

// func sysDlerror() uintptr
TEXT ·sysDlerror(SB), NOSPLIT, $16-8
	BL   libc_dlerror(SB)
	MOVD R0, ret+0(FP)
	RET

// C-callable atexit trampoline. Writes to atexitSignalFd to wake the Go
// cleanup goroutine, then blocks on atexitDoneFd until cleanup completes.
// Uses raw Linux arm64 syscalls — no Go runtime or libc dependency.
//
// func atexitTrampoline()
TEXT ·atexitTrampoline(SB), NOSPLIT, $0-0
	// write(atexitSignalFd, &atexitBuf, 1)
	MOVD ·atexitSignalFd(SB), R0
	MOVD $·atexitBuf(SB), R1
	MOVD $1, R2
	MOVD $64, R8                      // SYS_write
	SVC
	// read(atexitDoneFd, &atexitBuf, 1)  — blocks until Go signals done
	MOVD ·atexitDoneFd(SB), R0
	MOVD $·atexitBuf(SB), R1
	MOVD $1, R2
	MOVD $63, R8                      // SYS_read
	SVC
	RET
