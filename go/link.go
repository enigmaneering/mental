package mental

import (
	"fmt"
	"unsafe"
)

// Linkable is the interface for types that can be sent and received
// over a [Link].  To be Linkable you must implement both halves —
// serialization and deserialization.
//
// For fixed-size types (structs of primitives, arrays, etc.), Linkable
// is NOT required — they are serialized automatically via their
// in-memory representation.
type Linkable interface {
	Marshal() []byte
	Unmarshal([]byte) error
}

// marshalBytes converts a value to raw bytes for wire transmission.
// Priority:
//  1. Linkable interface → Marshal()
//  2. Disclosable interface → Disclose()
//  3. string → raw bytes
//  4. []byte → pass through
//  5. Fixed-size primitives → in-memory representation
//  6. Fixed-size structs/arrays → unsafe raw bytes
func marshalBytes(v any) []byte {
	if m, ok := v.(Linkable); ok {
		return m.Marshal()
	}
	return discloseBytes(v)
}

// Link is a typed bidirectional channel over the stdlink fd pair.
//
// TMessage is the type transmitted in both directions.  It can be:
//   - A fixed-size type (struct, array, primitive) — auto-serialized
//   - A type implementing [Linkable] — custom marshal/unmarshal
//   - A string — sent as raw bytes
//
// Link wraps the raw StdlinkSend/StdlinkRecv with type safety:
//
//	link := mental.NewLink[Vec3]()
//	link.Send(Vec3{1.0, 2.0, 3.0})
//	msg, err := link.Recv()  // msg is Vec3
//
// The channel is bidirectional — both parent and child can Send and Recv.
// Records are length-prefixed on the wire, so messages are framed.
type Link[TMessage any] struct{}

// NewLink creates a typed link over the process's stdlink channel.
// The underlying fd pair is created lazily on first use.
func NewLink[TMessage any]() *Link[TMessage] {
	return &Link[TMessage]{}
}

// Send serializes and transmits a message to the peer.
// Blocks until the full record is written.
func (l *Link[TMessage]) Send(msg TMessage) error {
	data := marshalBytes(msg)
	return StdlinkSend(data)
}

// Recv reads the next message from the peer.
// Blocks until a complete record arrives.
func (l *Link[TMessage]) Recv() (TMessage, error) {
	var zero TMessage

	// Determine receive buffer size.
	// For Linkable types, use a generous buffer and let Unmarshal parse.
	// For fixed-size types, we know exactly.
	var bufSize int

	if _, ok := any(&zero).(Linkable); ok {
		bufSize = 64 * 1024 // 64KB max for variable-size messages
	} else if _, ok := any(zero).(string); ok {
		bufSize = 64 * 1024 // strings are variable-length
	} else {
		// Fixed-size: buffer is exactly the type's size
		bufSize = int(unsafe.Sizeof(zero))
		if bufSize == 0 {
			bufSize = 1
		}
	}

	buf := make([]byte, bufSize)
	n, err := StdlinkRecv(buf)
	if err != nil {
		return zero, err
	}
	buf = buf[:n]

	// Reconstitute the value

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
	if size > 0 && n >= size {
		result := *(*TMessage)(unsafe.Pointer(&buf[0]))
		return result, nil
	}

	return zero, fmt.Errorf("mental: cannot unmarshal %d bytes into %T (expected %d)", n, zero, size)
}

// Fd returns the local stdlink file descriptor.
func (l *Link[TMessage]) Fd() int {
	return Stdlink()
}

// PeerFd returns the peer stdlink file descriptor.
// Pass this to a sparked child process.
func (l *Link[TMessage]) PeerFd() int {
	return StdlinkPeer()
}
