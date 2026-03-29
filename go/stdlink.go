package mental

/*
#include "mental.h"
*/
import "C"
import "unsafe"

// Stdlink returns the local end of the standard link channel.
// The returned file descriptor is backed by a socketpair (Unix) or pipe
// pair (Windows) and is created once on first call.
//
// Data written to this fd (via Send or raw write) is readable on the
// peer fd, and vice versa — forming a bidirectional channel suitable
// for parent↔child coordination after sparking.
//
// Returns -1 if the channel could not be created.
func Stdlink() int {
	return int(C.mental_stdlink())
}

// StdlinkPeer returns the peer (far) end of the stdlink channel.
// When sparking a child process, this fd is passed to the child as
// its own stdlink.  Returns -1 if the channel is not available.
func StdlinkPeer() int {
	return int(C.mental_stdlink_peer())
}

// StdlinkSend writes a length-prefixed record to the local stdlink fd.
// The wire format is [4 bytes: uint32 big-endian length][payload].
// Returns nil on success.
func StdlinkSend(data []byte) error {
	var p unsafe.Pointer
	if len(data) > 0 {
		p = unsafe.Pointer(&data[0])
	}
	rc := C.mental_stdlink_send(p, C.size_t(len(data)))
	if rc < 0 {
		return getLibError()
	}
	return nil
}

// StdlinkRecv reads the next length-prefixed record from the local
// stdlink fd.  Blocks until a complete record arrives.
// Returns the payload (up to len(buf) bytes) and the full record
// length.  If the record exceeds buf, excess bytes are discarded.
func StdlinkRecv(buf []byte) (n int, err error) {
	if len(buf) == 0 {
		return 0, nil
	}
	var outLen C.size_t
	rc := C.mental_stdlink_recv(
		unsafe.Pointer(&buf[0]),
		C.size_t(len(buf)),
		&outLen,
	)
	if rc < 0 {
		return 0, getLibError()
	}
	return int(outLen), nil
}
