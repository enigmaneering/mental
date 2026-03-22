#include "textflag.h"

// Windows x64 ABI: args in RCX, RDX, R8, R9, then stack.
// 32 bytes shadow space required at RSP+0 through RSP+24.
// Frame size 48 = 32 (shadow) + 8 (5th arg slot) + 8 (padding for alignment).

// func call0(fn uintptr) uintptr
TEXT ·call0(SB), NOSPLIT, $48-16
	MOVQ fn+0(FP), R10
	CALL R10
	MOVQ AX, ret+8(FP)
	RET

// func call1(fn, a1 uintptr) uintptr
TEXT ·call1(SB), NOSPLIT, $48-24
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), CX
	CALL R10
	MOVQ AX, ret+16(FP)
	RET

// func call2(fn, a1, a2 uintptr) uintptr
TEXT ·call2(SB), NOSPLIT, $48-32
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), CX
	MOVQ a2+16(FP), DX
	CALL R10
	MOVQ AX, ret+24(FP)
	RET

// func call3(fn, a1, a2, a3 uintptr) uintptr
TEXT ·call3(SB), NOSPLIT, $48-40
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), CX
	MOVQ a2+16(FP), DX
	MOVQ a3+24(FP), R8
	CALL R10
	MOVQ AX, ret+32(FP)
	RET

// func call4(fn, a1, a2, a3, a4 uintptr) uintptr
TEXT ·call4(SB), NOSPLIT, $48-48
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), CX
	MOVQ a2+16(FP), DX
	MOVQ a3+24(FP), R8
	MOVQ a4+32(FP), R9
	CALL R10
	MOVQ AX, ret+40(FP)
	RET

// func call5(fn, a1, a2, a3, a4, a5 uintptr) uintptr
TEXT ·call5(SB), NOSPLIT, $48-56
	MOVQ fn+0(FP), R10
	MOVQ a1+8(FP), CX
	MOVQ a2+16(FP), DX
	MOVQ a3+24(FP), R8
	MOVQ a4+32(FP), R9
	MOVQ a5+40(FP), R11
	MOVQ R11, 32(SP)
	CALL R10
	MOVQ AX, ret+48(FP)
	RET
