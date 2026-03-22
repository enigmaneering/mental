#include "textflag.h"

// Windows ARM64 uses AAPCS64 (same register convention as Unix ARM64).
// Args in R0-R7, return in R0.

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

// C-callable atexit trampoline for Windows ARM64.
// Calls WriteFile/ReadFile through resolved kernel32 addresses.
// Must save/restore LR (R30) since BL overwrites it.
//
// func atexitTrampoline()
TEXT ·atexitTrampoline(SB), NOSPLIT, $0-0
	SUB  $16, RSP, RSP
	MOVD R30, (RSP)
	// WriteFile(signalHandle, &buf, 1, &written, NULL)
	MOVD ·atexitSignalFd(SB), R0
	MOVD $·atexitBuf(SB), R1
	MOVD $1, R2
	MOVD $·atexitWritten(SB), R3
	MOVD $0, R4
	MOVD ·writeFileAddr(SB), R9
	BL   (R9)
	// ReadFile(doneHandle, &buf, 1, &written, NULL)  — blocks until Go signals done
	MOVD ·atexitDoneFd(SB), R0
	MOVD $·atexitBuf(SB), R1
	MOVD $1, R2
	MOVD $·atexitWritten(SB), R3
	MOVD $0, R4
	MOVD ·readFileAddr(SB), R9
	BL   (R9)
	MOVD (RSP), R30
	ADD  $16, RSP, RSP
	RET
