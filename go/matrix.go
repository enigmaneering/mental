// Matrix provides digit-by-digit parallel arithmetic on GPU.
//
// Instead of dispatching one kernel per element, the matrix decomposes
// values into their digit representation in an arbitrary base, then
// dispatches one kernel per digit position across all elements
// simultaneously.  Carry propagation happens during recomposition.
//
// Two inputs → element-wise arithmetic (A[i] op B[i] → C[i]).
// One input  → reduction (fold N elements to a single result).
package mental

/*
#include "mental.h"
#include <stdlib.h>
*/
import "C"
import (
	"fmt"
	"math"
	"runtime"
	"unsafe"
)

// Matrixable lets custom types declare their placeholder width in a
// given base.  Fixed-size numeric types (uint32, float32, etc.) are
// handled implicitly — you only need this for custom types.
type Matrixable interface {
	Matrix(base byte) uint
}

// MatrixOp identifies the arithmetic operation.
type MatrixOp int

const (
	MatrixAdd MatrixOp = iota
	MatrixSub
)

// Matrix is a reusable digit-by-digit arithmetic engine.
// Create once with [CreateMatrix], then call [Matrix.Execute] or
// [Matrix.Reduce] repeatedly with different data.
type Matrix[T any] struct {
	dev  Device
	op   MatrixOp
	base int
	n    int // element count
	p    int // placeholder count (digits)

	// Stride = N+1 (extra slot per digit for carry/overflow).
	stride int

	// Intermediate GPU buffers (raw C references), allocated once.
	digitsA uintptr // P * stride uint32s
	digitsB uintptr // P * stride uint32s
	results uintptr // P * stride uint32s

	// Compiled kernels (reused across Execute calls).
	kDecompose  Kernel
	kOps        []Kernel // P kernels, one per digit position
	kRecompose  Kernel
	kRecomposeR Kernel // recompose variant for reduction (0 if unused)
}

// CreateMatrix compiles the kernels for a given operation, base, and
// element count.  The compiled state is cached — call Execute()
// repeatedly with different data.
//
// T must be a fixed-size numeric type or implement [Matrixable].
func CreateMatrix[T any](dev Device, op MatrixOp, base byte, n int) (*Matrix[T], error) {
	if base < 2 {
		return nil, fmt.Errorf("mental: matrix base must be >= 2, got %d", base)
	}
	if n < 1 {
		return nil, fmt.Errorf("mental: matrix element count must be >= 1, got %d", n)
	}

	p := matrixPlaceholders[T](base)
	stride := n + 1
	b := int(base)

	m := &Matrix[T]{
		dev:    dev,
		op:     op,
		base:   b,
		n:      n,
		p:      p,
		stride: stride,
	}

	// Allocate intermediate buffers (raw C references).
	bufSize := C.size_t(p * stride * 4) // uint32 per entry
	var err error
	m.digitsA, err = matrixAllocBuf(dev, bufSize)
	if err != nil {
		return nil, err
	}
	m.digitsB, err = matrixAllocBuf(dev, bufSize)
	if err != nil {
		m.Finalize()
		return nil, err
	}
	m.results, err = matrixAllocBuf(dev, bufSize)
	if err != nil {
		m.Finalize()
		return nil, err
	}

	// Compile decompose kernel.
	decompSrc := matrixDecomposeShader(b, p, stride, n)
	m.kDecompose, err = Compile(dev, decompSrc)
	if err != nil {
		m.Finalize()
		return nil, fmt.Errorf("mental: matrix decompose compile: %w", err)
	}

	// Compile P operate kernels (one per digit position).
	m.kOps = make([]Kernel, p)
	for d := 0; d < p; d++ {
		opSrc := matrixOperateShader(b, stride, n, d, op)
		m.kOps[d], err = Compile(dev, opSrc)
		if err != nil {
			m.Finalize()
			return nil, fmt.Errorf("mental: matrix operate[%d] compile: %w", d, err)
		}
	}

	// Compile recompose kernel (element-wise).
	recompSrc := matrixRecomposeShader(b, p, stride, n, op)
	m.kRecompose, err = Compile(dev, recompSrc)
	if err != nil {
		m.Finalize()
		return nil, fmt.Errorf("mental: matrix recompose compile: %w", err)
	}

	return m, nil
}

// Execute runs the element-wise pipeline: A[i] op B[i] → out[i].
// All three references must be pinned to the matrix's device and
// hold at least n elements of T.
func (m *Matrix[T]) Execute(a, b uintptr, out uintptr) error {
	if a == 0 || b == 0 || out == 0 {
		return ErrInvalidReference
	}

	// Build pipe.
	pipe, err := CreatePipe(m.dev)
	if err != nil {
		return err
	}

	// Stage 1: Decompose A and B.
	if err := pipe.Add(m.kDecompose, []uintptr{a}, []uintptr{m.digitsA}, m.n); err != nil {
		pipe.Finalize()
		return fmt.Errorf("mental: matrix decompose A: %w", err)
	}
	if err := pipe.Add(m.kDecompose, []uintptr{b}, []uintptr{m.digitsB}, m.n); err != nil {
		pipe.Finalize()
		return fmt.Errorf("mental: matrix decompose B: %w", err)
	}

	// Stage 2: Operate on each digit position.
	for d := 0; d < m.p; d++ {
		if err := pipe.Add(m.kOps[d], []uintptr{m.digitsA, m.digitsB}, []uintptr{m.results}, m.n); err != nil {
			pipe.Finalize()
			return fmt.Errorf("mental: matrix operate[%d]: %w", d, err)
		}
	}

	// Stage 3: Recompose.
	if err := pipe.Add(m.kRecompose, []uintptr{m.results}, []uintptr{out}, m.n); err != nil {
		pipe.Finalize()
		return fmt.Errorf("mental: matrix recompose: %w", err)
	}

	// Execute the full pipeline.
	if err := pipe.Execute(); err != nil {
		pipe.Finalize()
		return fmt.Errorf("mental: matrix execute: %w", err)
	}
	pipe.Finalize()

	return nil
}

// Finalize releases all compiled kernels and intermediate buffers.
func (m *Matrix[T]) Finalize() {
	if m.kDecompose != 0 {
		m.kDecompose.Finalize()
		m.kDecompose = 0
	}
	for i, k := range m.kOps {
		if k != 0 {
			k.Finalize()
			m.kOps[i] = 0
		}
	}
	if m.kRecompose != 0 {
		m.kRecompose.Finalize()
		m.kRecompose = 0
	}
	if m.kRecomposeR != 0 {
		m.kRecomposeR.Finalize()
		m.kRecomposeR = 0
	}
	matrixFreeBuf(m.digitsA)
	m.digitsA = 0
	matrixFreeBuf(m.digitsB)
	m.digitsB = 0
	matrixFreeBuf(m.results)
	m.results = 0
}

// ---------------------------------------------------------------------------
//  Placeholder calculation
// ---------------------------------------------------------------------------

// matrixPlaceholders returns the number of digit positions needed to
// represent type T in the given base.
func matrixPlaceholders[T any](base byte) int {
	var zero T
	if m, ok := any(&zero).(Matrixable); ok {
		return int(m.Matrix(base))
	}
	bits := int(unsafe.Sizeof(zero)) * 8
	return placeholdersFromBits(bits, int(base))
}

func placeholdersFromBits(bits int, base int) int {
	switch base {
	case 256:
		return (bits + 7) / 8
	case 2:
		return bits
	default:
		return int(math.Ceil(float64(bits) * math.Log(2) / math.Log(float64(base))))
	}
}

// ---------------------------------------------------------------------------
//  Raw C buffer helpers
// ---------------------------------------------------------------------------

func matrixAllocBuf(dev Device, size C.size_t) (uintptr, error) {
	runtime.LockOSThread()
	ref := C.mental_reference_create(size, C.MENTAL_RELATIONALLY_OPEN, nil, 0, nil)
	runtime.UnlockOSThread()
	if ref == nil {
		return 0, fmt.Errorf("mental: matrix buffer alloc failed (%d bytes)", size)
	}
	runtime.LockOSThread()
	rc := C.mental_reference_pin(ref, C.mental_device(unsafe.Pointer(dev)))
	runtime.UnlockOSThread()
	if rc != 0 {
		C.mental_reference_close(ref)
		return 0, fmt.Errorf("mental: matrix buffer pin failed")
	}
	return uintptr(unsafe.Pointer(ref)), nil
}

func matrixFreeBuf(h uintptr) {
	if h == 0 {
		return
	}
	C.mental_reference_close(C.mental_reference(unsafe.Pointer(h)))
}

// MatrixMakeBuffer creates a GPU-pinned buffer of n uint32 elements
// and writes the provided data into it.  Returns a raw handle for
// use with [Matrix.Execute].
func MatrixMakeBuffer(dev Device, n int, data []uint32) uintptr {
	size := C.size_t(n * 4)
	runtime.LockOSThread()
	ref := C.mental_reference_create(size, C.MENTAL_RELATIONALLY_OPEN, nil, 0, nil)
	runtime.UnlockOSThread()
	if ref == nil {
		return 0
	}
	// Write data.
	C.mental_reference_write(ref, unsafe.Pointer(&data[0]), size)
	// Pin to device.
	runtime.LockOSThread()
	C.mental_reference_pin(ref, C.mental_device(unsafe.Pointer(dev)))
	runtime.UnlockOSThread()
	return uintptr(unsafe.Pointer(ref))
}

// MatrixMakeEmptyBuffer creates a GPU-pinned buffer of n uint32
// elements, initialized to zero.
func MatrixMakeEmptyBuffer(dev Device, n int) uintptr {
	h, _ := matrixAllocBuf(dev, C.size_t(n*4))
	return h
}

// MatrixReadBuffer reads n uint32 values from a raw reference handle.
func MatrixReadBuffer(h uintptr, n int) []uint32 {
	out := make([]uint32, n)
	C.mental_reference_read(
		C.mental_reference(unsafe.Pointer(h)),
		unsafe.Pointer(&out[0]),
		C.size_t(n*4),
	)
	return out
}

// MatrixFreeBuffer releases a raw reference handle created by
// [MatrixMakeBuffer] or [MatrixMakeEmptyBuffer].
func MatrixFreeBuffer(h uintptr) {
	matrixFreeBuf(h)
}

// ---------------------------------------------------------------------------
//  Shader generators
// ---------------------------------------------------------------------------

func matrixDecomposeShader(base, p, stride, n int) string {
	return fmt.Sprintf(`#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer In  { uint elements[]; };
layout(std430, binding = 1) writeonly buffer Out { uint digits[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= %du) return;
    uint val = elements[i];
    for (uint d = 0u; d < %du; d++) {
        digits[d * %du + i] = val %% %du;
        val /= %du;
    }
}
`, n, p, stride, base, base)
}

func matrixOperateShader(base, stride, n, digitIdx int, op MatrixOp) string {
	var expr string
	switch op {
	case MatrixAdd:
		expr = "a[off + i] + b[off + i]"
	case MatrixSub:
		expr = fmt.Sprintf("a[off + i] - b[off + i] + %du", base)
	default:
		expr = "a[off + i] + b[off + i]"
	}

	return fmt.Sprintf(`#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer A { uint a[]; };
layout(std430, binding = 1) readonly buffer B { uint b[]; };
layout(std430, binding = 2) buffer R { uint r[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= %du) return;
    uint off = %du * %du;
    r[off + i] = %s;
}
`, n, digitIdx, stride, expr)
}

func matrixRecomposeShader(base, p, stride, n int, op MatrixOp) string {
	// For subtraction, each digit had BASE added to avoid underflow.
	// We subtract it back during carry ripple.
	var digitExpr string
	switch op {
	case MatrixSub:
		digitExpr = fmt.Sprintf("results[d * %du + i] + carry - %du", stride, base)
	default:
		digitExpr = fmt.Sprintf("results[d * %du + i] + carry", stride)
	}

	// For subtraction, carry is a borrow: if digit_sum underflowed
	// (went negative), we need to borrow from the next position.
	// Since we're working with uint, underflow wraps to a large value.
	// The add-BASE trick means: raw = (a - b + BASE).
	// digit_sum = raw - BASE + carry_in.
	// If a >= b + borrow: digit_sum >= 0, carry_out = 0, digit = digit_sum.
	// If a < b + borrow: digit_sum < 0 (wraps), need borrow.
	// Simplification: just use / and % on the raw sum — works for both
	// add and sub because the add-BASE ensures non-negative intermediate.
	// For add: carry = digit_sum / BASE, digit = digit_sum % BASE.
	// For sub: same math, but digit_sum = raw - BASE + carry.
	//   If no borrow: raw = a-b+BASE >= BASE, digit_sum = (a-b) + carry, carry = (a-b+carry)/BASE.
	//   If borrow: raw = a-b+BASE < BASE, digit_sum = (a-b+BASE-BASE+carry) = a-b+carry (negative wraps).
	// Actually for uint subtraction in GLSL, we handle it differently.
	// The key insight: for sub, raw = a_digit - b_digit + BASE (always >= 0 if digits < BASE).
	// digit_sum = raw + carry_in - BASE (undo the padding).
	// If digit_sum >= 0: carry_out = digit_sum / BASE, digit = digit_sum % BASE.
	// Since carry_in is 0 or -1 (borrow), and raw is in [0, 2*BASE-1]:
	//   digit_sum is in [-BASE, BASE-1].
	// We can reformulate: carry_out = (digit_sum >= BASE) ? 1 : 0 for add.
	// For sub: carry_out = (raw + carry_in < BASE) ? -1 : 0 ... but uint.
	//
	// Simplest correct approach for both: store raw sums, ripple with
	// / and % which handles carry naturally for addition.
	// For subtraction we use a signed intermediate.

	if op == MatrixSub {
		return fmt.Sprintf(`#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer R { uint results[]; };
layout(std430, binding = 1) writeonly buffer Out { uint elements[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= %du) return;
    int carry = 0;
    uint val = 0u;
    uint base_power = 1u;
    for (uint d = 0u; d < %du; d++) {
        int digit_sum = int(results[d * %du + i]) + carry - %d;
        if (digit_sum < 0) {
            digit_sum += %d;
            carry = -1;
        } else {
            carry = digit_sum / %d;
            digit_sum = digit_sum %% %d;
            carry = 0;
        }
        val += uint(digit_sum) * base_power;
        base_power *= %du;
    }
    elements[i] = val;
}
`, n, p, stride, base, base, base, base, base)
	}

	return fmt.Sprintf(`#version 450
layout(local_size_x = 256) in;
layout(std430, binding = 0) readonly buffer R { uint results[]; };
layout(std430, binding = 1) writeonly buffer Out { uint elements[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= %du) return;
    uint carry = 0u;
    uint val = 0u;
    uint base_power = 1u;
    for (uint d = 0u; d < %du; d++) {
        uint digit_sum = %s;
        carry = digit_sum / %du;
        val += (digit_sum %% %du) * base_power;
        base_power *= %du;
    }
    elements[i] = val;
}
`, n, p, digitExpr, base, base, base)
}
