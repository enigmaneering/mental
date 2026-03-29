package mental

/*
#include "mental.h"
*/
import "C"
import (
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"syscall"
	"unsafe"
)

// SparkLink is a bidirectional communication channel between a parent
// and its sparked child, backed by an inherited pipe.
//
// Both sides can Send and Recv.  The link is established via [Spark]
// (parent side) or [Sparked] (child side).
//
// When either side dies, the other detects it via EOF on recv.
//
// The Close method is intentionally unexposed — the library manages
// link lifecycle via finalizers and process-exit cleanup.
type SparkLink[TMessage any] struct {
	ptr uintptr // mental_link handle
}

// Send serializes and transmits a message to the peer.
// Blocks until the full record is written to the pipe.
func (l *SparkLink[TMessage]) Send(msg TMessage) error {
	if l.ptr == 0 {
		return fmt.Errorf("mental: send on closed spark link")
	}
	data := marshalBytes(msg)
	var p unsafe.Pointer
	if len(data) > 0 {
		p = unsafe.Pointer(&data[0])
	}
	rc := C.mental_link_send(
		C.mental_link(unsafe.Pointer(l.ptr)),
		p,
		C.size_t(len(data)),
	)
	if rc < 0 {
		return getLibError()
	}
	return nil
}

// Recv reads the next message from the peer.
// Blocks efficiently in the kernel until data arrives.
// Returns an error (EOF) if the peer process has died.
func (l *SparkLink[TMessage]) Recv() (TMessage, error) {
	var zero TMessage
	if l.ptr == 0 {
		return zero, fmt.Errorf("mental: recv on closed spark link")
	}

	// Determine receive buffer size.
	var bufSize int
	if _, ok := any(&zero).(Linkable); ok {
		bufSize = 64 * 1024
	} else if _, ok := any(zero).(string); ok {
		bufSize = 64 * 1024
	} else {
		bufSize = int(unsafe.Sizeof(zero))
		if bufSize == 0 {
			bufSize = 1
		}
	}

	buf := make([]byte, bufSize)
	var outLen C.size_t
	rc := C.mental_link_recv(
		C.mental_link(unsafe.Pointer(l.ptr)),
		unsafe.Pointer(&buf[0]),
		C.size_t(len(buf)),
		&outLen,
	)
	if rc < 0 {
		return zero, getLibError()
	}
	buf = buf[:int(outLen)]

	// String special case
	if s, ok := any(&zero).(*string); ok {
		*s = string(buf)
		return zero, nil
	}

	// []byte special case
	if b, ok := any(&zero).(*[]byte); ok {
		*b = buf
		return zero, nil
	}

	// Linkable — call Unmarshal
	if u, ok := any(&zero).(Linkable); ok {
		if err := u.Unmarshal(buf); err != nil {
			return zero, err
		}
		return zero, nil
	}

	// Fixed-size: copy raw bytes into the value
	size := int(unsafe.Sizeof(zero))
	if size > 0 && int(outLen) >= size {
		result := *(*TMessage)(unsafe.Pointer(&buf[0]))
		return result, nil
	}

	return zero, fmt.Errorf("mental: cannot unmarshal %d bytes into %T (expected %d)",
		outLen, zero, size)
}

// close releases the link handle.  Called by finalizer.
func (l *SparkLink[TMessage]) close() {
	if l.ptr != 0 {
		C.mental_link_close(C.mental_link(unsafe.Pointer(l.ptr)))
		l.ptr = 0
	}
}

// wrapSparkLink wraps a raw C mental_link pointer into a typed SparkLink
// and registers a finalizer for automatic cleanup.
func wrapSparkLink[T any](ptr uintptr) *SparkLink[T] {
	if ptr == 0 {
		return nil
	}
	link := &SparkLink[T]{ptr: ptr}
	runtime.SetFinalizer(link, (*SparkLink[T]).close)
	return link
}

// sparkFD is the well-known file descriptor for sparked children.
// fd 3 is the first ExtraFiles entry (ExtraFiles[0] → fd 3 in child).
const sparkFD = 3

// ── Child Spark ──────────────────────────────────────────────────

// Spark spawns a new process at path with a pre-established link.
// The child retrieves its end via [Sparked].
//
// Process spawning uses Go's os/exec package, which cooperates with
// the Go runtime's poller, GC, and thread manager.  The link itself
// is a socketpair — the child's end is placed at fd 42.
//
// Returns the parent's side of the link, or an error.
func Spark[T any](path string) (*SparkLink[T], error) {
	// Create a bidirectional socket pair.
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		return nil, fmt.Errorf("mental: socketpair: %w", err)
	}
	parentFD := fds[0]
	childFD := fds[1]

	// Build the child command.  The child's end of the socketpair
	// is passed as ExtraFiles[0], which becomes fd 3 in the child.
	// MENTAL_SPARK=3 tells the child's mental_sparked() which fd to use.
	childFile := os.NewFile(uintptr(childFD), "spark-link")
	cmd := exec.Command(path)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.ExtraFiles = []*os.File{childFile}
	cmd.Env = append(os.Environ(), "MENTAL_SPARK=3")

	if err := cmd.Start(); err != nil {
		syscall.Close(parentFD)
		childFile.Close()
		return nil, fmt.Errorf("mental: spark %s: %w", path, err)
	}

	// Close the child's end in the parent — the child has it now.
	childFile.Close()

	// Wrap the parent's fd as a mental_link via the C library.
	clink := C.mental_link_wrap_fd(C.int(parentFD))
	ptr := uintptr(unsafe.Pointer(clink))
	if ptr == 0 {
		syscall.Close(parentFD)
		return nil, fmt.Errorf("mental: failed to wrap spark link fd")
	}

	// Wait for child in background so it doesn't become a zombie.
	go cmd.Wait()

	return wrapSparkLink[T](ptr), nil
}

// Sparked returns the inherited link if this process was sparked by
// another process via [Spark].  Returns nil if this process was
// started normally.  Safe to call multiple times (result is cached).
//
// Each child sees exactly one parent link (singleton).  Children
// can spark their own children, forming a tree of links.
func Sparked[T any]() *SparkLink[T] {
	clink := C.mental_sparked()
	ptr := uintptr(unsafe.Pointer(clink))
	if ptr == 0 {
		return nil
	}
	// Don't set finalizer — the C library caches this and manages its lifetime
	return &SparkLink[T]{ptr: ptr}
}
