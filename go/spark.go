package mental

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"os/exec"
	"regexp"
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
	fd int // raw file descriptor
}

// Send serializes and transmits a message to the peer.
// Blocks until the full record is written to the pipe.
func (l *SparkLink[TMessage]) Send(msg TMessage) error {
	if l.fd < 0 {
		return fmt.Errorf("mental: send on closed spark link")
	}
	data := marshalBytes(msg)
	return l.sendRaw(data)
}

// sendRaw writes a length-prefixed record: [4-byte big-endian length][payload].
func (l *SparkLink[TMessage]) sendRaw(data []byte) error {
	var hdr [4]byte
	binary.BigEndian.PutUint32(hdr[:], uint32(len(data)))
	if _, err := writeAll(l.fd, hdr[:]); err != nil {
		return err
	}
	if len(data) > 0 {
		if _, err := writeAll(l.fd, data); err != nil {
			return err
		}
	}
	return nil
}

// Recv reads the next message from the peer.
// Blocks efficiently in the kernel until data arrives.
// Returns an error (EOF) if the peer process has died.
func (l *SparkLink[TMessage]) Recv() (TMessage, error) {
	var zero TMessage
	if l.fd < 0 {
		return zero, fmt.Errorf("mental: recv on closed spark link")
	}

	data, err := l.recvRaw()
	if err != nil {
		return zero, err
	}

	// String special case
	if s, ok := any(&zero).(*string); ok {
		*s = string(data)
		return zero, nil
	}

	// []byte special case
	if b, ok := any(&zero).(*[]byte); ok {
		*b = data
		return zero, nil
	}

	// Linkable — call Unmarshal
	if u, ok := any(&zero).(Linkable); ok {
		if err := u.Unmarshal(data); err != nil {
			return zero, err
		}
		return zero, nil
	}

	// Fixed-size: copy raw bytes into the value
	size := int(unsafe.Sizeof(zero))
	if size > 0 && len(data) >= size {
		result := *(*TMessage)(unsafe.Pointer(&data[0]))
		return result, nil
	}

	return zero, fmt.Errorf("mental: cannot unmarshal %d bytes into %T (expected %d)",
		len(data), zero, size)
}

// recvRaw reads one length-prefixed record from the fd.
func (l *SparkLink[TMessage]) recvRaw() ([]byte, error) {
	var hdr [4]byte
	if _, err := readAll(l.fd, hdr[:]); err != nil {
		return nil, err
	}
	n := binary.BigEndian.Uint32(hdr[:])
	if n == 0 {
		return nil, nil
	}
	buf := make([]byte, n)
	if _, err := readAll(l.fd, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

// close releases the link handle.  Called by finalizer.
func (l *SparkLink[TMessage]) close() {
	if l.fd >= 0 {
		syscall.Close(l.fd)
		l.fd = -1
	}
}

// wrapSparkLink wraps a raw file descriptor into a typed SparkLink
// and registers a finalizer for automatic cleanup.
func wrapSparkLink[T any](fd int) *SparkLink[T] {
	if fd < 0 {
		return nil
	}
	link := &SparkLink[T]{fd: fd}
	runtime.SetFinalizer(link, (*SparkLink[T]).close)
	return link
}

// sparkFD is the well-known file descriptor for sparked children.
// fd 3 is the first ExtraFiles entry (ExtraFiles[0] -> fd 3 in child).
const sparkFD = 3

// ── I/O helpers ─────────────────────────────────────────────────

// writeAll writes all of buf to fd, retrying on EINTR and short writes.
func writeAll(fd int, buf []byte) (int, error) {
	written := 0
	for written < len(buf) {
		n, err := syscall.Write(fd, buf[written:])
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return written, fmt.Errorf("mental: write: %w", err)
		}
		if n == 0 {
			return written, fmt.Errorf("mental: write: zero bytes written")
		}
		written += n
	}
	return written, nil
}

// readAll reads exactly len(buf) bytes from fd, retrying on EINTR and short reads.
func readAll(fd int, buf []byte) (int, error) {
	read := 0
	for read < len(buf) {
		n, err := syscall.Read(fd, buf[read:])
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return read, fmt.Errorf("mental: read: %w", err)
		}
		if n == 0 {
			return read, fmt.Errorf("mental: read: EOF")
		}
		read += n
	}
	return read, nil
}

// ── Child Spark ──────────────────────────────────────────────────

// Spark spawns a new process at path with a pre-established link.
// The child retrieves its end via [Sparked].
//
// Optional [*os.File] arguments redirect the child's standard streams:
//
//	mental.Spark[T](path)                          // inherit parent's stdin/stdout/stderr
//	mental.Spark[T](path, nil, outFile, errFile)   // custom stdout/stderr, no stdin
//	mental.Spark[T](path, inFile)                  // custom stdin, inherit stdout/stderr
//
// The order is: stdin, stdout, stderr.  Omitted or nil entries inherit
// the parent's corresponding stream.
//
// Returns the parent's side of the link, or an error.
func Spark[T any](path string, stdio ...*os.File) (*SparkLink[T], error) {
	// Create a bidirectional socket pair.
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		return nil, fmt.Errorf("mental: socketpair: %w", err)
	}
	parentFD := fds[0]
	childFD := fds[1]

	// Resolve stdin/stdout/stderr: use provided files or inherit parent's.
	childIn := os.Stdin
	childOut := os.Stdout
	childErr := os.Stderr
	if len(stdio) > 0 && stdio[0] != nil {
		childIn = stdio[0]
	}
	if len(stdio) > 1 && stdio[1] != nil {
		childOut = stdio[1]
	}
	if len(stdio) > 2 && stdio[2] != nil {
		childErr = stdio[2]
	}

	// Build the child command.  The child's end of the socketpair
	// is passed as ExtraFiles[0], which becomes fd 3 in the child.
	// MENTAL_SPARK=3 tells the child's Sparked() which fd to use.
	// Split path into executable + args, respecting quotes and backslash escapes.
	parts := shellSplit(path)
	if len(parts) == 0 {
		syscall.Close(parentFD)
		syscall.Close(childFD)
		return nil, fmt.Errorf("mental: empty spark path")
	}

	childFile := os.NewFile(uintptr(childFD), "spark-link")
	cmd := exec.Command(parts[0], parts[1:]...)
	cmd.Stdin = childIn
	cmd.Stdout = childOut
	cmd.Stderr = childErr
	cmd.ExtraFiles = []*os.File{childFile}
	cmd.Env = append(os.Environ(), "MENTAL_SPARK=3")

	if err := cmd.Start(); err != nil {
		syscall.Close(parentFD)
		childFile.Close()
		return nil, fmt.Errorf("mental: spark %s: %w", path, err)
	}

	// Close the child's end in the parent — the child has it now.
	childFile.Close()

	// Wait for child in background so it doesn't become a zombie.
	go cmd.Wait()

	return wrapSparkLink[T](parentFD), nil
}

// sparkedLink caches the singleton sparked link fd (-1 = not checked, -2 = none).
var sparkedLink int = -1

// Sparked returns the inherited link if this process was sparked by
// another process via [Spark].  Returns nil if this process was
// started normally.  Safe to call multiple times (result is cached).
//
// Each child sees exactly one parent link (singleton).  Children
// can spark their own children, forming a tree of links.
func Sparked[T any]() *SparkLink[T] {
	if sparkedLink == -1 {
		env := os.Getenv("MENTAL_SPARK")
		if env == "" {
			sparkedLink = -2
		} else {
			fd := 0
			for _, c := range env {
				if c >= '0' && c <= '9' {
					fd = fd*10 + int(c-'0')
				}
			}
			if fd > 0 {
				sparkedLink = fd
			} else {
				sparkedLink = -2
			}
		}
	}
	if sparkedLink < 0 {
		return nil
	}
	// Don't set finalizer — the parent manages this fd's lifetime
	return &SparkLink[T]{fd: sparkedLink}
}

// shellSplitRe matches: "double quoted" | 'single quoted' | backslash-escaped\ chars | bare words
var shellSplitRe = regexp.MustCompile(`"([^"\\]*(?:\\.[^"\\]*)*)"|'([^']*)'|(\\.|\S)+`)

// shellSplit splits a command string into tokens using shell-like rules:
//   - Double quotes preserve spaces, backslash escapes inside
//   - Single quotes preserve everything literally
//   - Backslash outside quotes escapes the next character
//   - Unquoted whitespace separates tokens
func shellSplit(s string) []string {
	matches := shellSplitRe.FindAllString(s, -1)
	result := make([]string, 0, len(matches))
	for _, m := range matches {
		// Strip outer quotes and process escapes.
		if len(m) >= 2 && m[0] == '"' && m[len(m)-1] == '"' {
			m = m[1 : len(m)-1]
			m = shellUnescape(m)
		} else if len(m) >= 2 && m[0] == '\'' && m[len(m)-1] == '\'' {
			m = m[1 : len(m)-1]
		} else {
			m = shellUnescape(m)
		}
		result = append(result, m)
	}
	return result
}

// shellUnescape processes backslash escapes: \x -> x for any character.
func shellUnescape(s string) string {
	var b []byte
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			i++
			b = append(b, s[i])
		} else {
			b = append(b, s[i])
		}
	}
	return string(b)
}

// ── Peer-to-Peer Linking ─────────────────────────────────────────

// LinkConnect establishes a link with another process by UUID.
// The target must be running (any process using mental listens
// automatically).  Returns a link, or an error if unreachable.
func LinkConnect[T any](uuid string) (*SparkLink[T], error) {
	path := "/tmp/m-" + uuid
	conn, err := net.Dial("unix", path)
	if err != nil {
		return nil, fmt.Errorf("mental: link connect to %s failed: %w", uuid, err)
	}
	// Extract the raw fd from the net.Conn
	raw, err := conn.(*net.UnixConn).SyscallConn()
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("mental: link connect fd: %w", err)
	}
	var fd int
	var fdErr error
	raw.Control(func(f uintptr) {
		// Dup the fd so we own it after conn is GC'd
		fd, fdErr = syscall.Dup(int(f))
	})
	if fdErr != nil {
		conn.Close()
		return nil, fmt.Errorf("mental: link connect dup: %w", fdErr)
	}
	// Close the net.Conn — we have our own dup'd fd now
	conn.Close()
	return wrapSparkLink[T](fd), nil
}

// LinkAccept blocks until another process connects to us.
// Returns a new dedicated link for that connection.
// Call in a goroutine to accept multiple connections:
//
//	go func() {
//	    for {
//	        link, err := mental.LinkAccept[[]byte]()
//	        if err != nil { return }
//	        go handlePeer(link)
//	    }
//	}()
func LinkAccept[T any]() (*SparkLink[T], error) {
	// Ensure UUID is initialized (which starts the listener)
	UUID()
	if listener == nil {
		return nil, fmt.Errorf("mental: link accept failed: no listener")
	}
	conn, err := listener.Accept()
	if err != nil {
		return nil, fmt.Errorf("mental: link accept: %w", err)
	}
	// Extract the raw fd from the net.Conn
	raw, err := conn.(*net.UnixConn).SyscallConn()
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("mental: link accept fd: %w", err)
	}
	var fd int
	var fdErr error
	raw.Control(func(f uintptr) {
		fd, fdErr = syscall.Dup(int(f))
	})
	if fdErr != nil {
		conn.Close()
		return nil, fmt.Errorf("mental: link accept dup: %w", fdErr)
	}
	conn.Close()
	return wrapSparkLink[T](fd), nil
}

// Linkable is the interface for types that can be sent and received
// over a [SparkLink].  To be Linkable you must implement both halves --
// serialization and deserialization.
//
// For fixed-size types (structs of primitives, arrays, etc.), Linkable
// is NOT required -- they are serialized automatically via their
// in-memory representation.
type Linkable interface {
	Marshal() []byte
	Unmarshal([]byte) error
}

// marshalBytes converts a value to raw bytes for wire transmission.
// Priority:
//  1. Linkable interface -> Marshal()
//  2. Disclosable interface -> Disclose()
//  3. string -> raw bytes
//  4. []byte -> pass through
//  5. Fixed-size primitives -> in-memory representation
//  6. Fixed-size structs/arrays -> unsafe raw bytes
func marshalBytes(v any) []byte {
	if m, ok := v.(Linkable); ok {
		return m.Marshal()
	}
	return discloseBytes(v)
}
