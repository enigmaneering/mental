package mental

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
	return int(int32(call0(ft.stdlink)))
}

// StdlinkPeer returns the peer (far) end of the stdlink channel.
// When sparking a child process, this fd is passed to the child as
// its own stdlink.  Returns -1 if the channel is not available.
func StdlinkPeer() int {
	return int(int32(call0(ft.stdlinkPeer)))
}

// StdlinkSend writes a length-prefixed record to the local stdlink fd.
// The wire format is [4 bytes: uint32 big-endian length][payload].
// Returns nil on success.
func StdlinkSend(data []byte) error {
	var p unsafe.Pointer
	if len(data) > 0 {
		p = unsafe.Pointer(&data[0])
	}
	rc := int32(call2(ft.stdlinkSend, uintptr(p), uintptr(len(data))))
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
	var outLen uint64
	rc := int32(call3(ft.stdlinkRecv,
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(len(buf)),
		uintptr(unsafe.Pointer(&outLen)),
	))
	if rc < 0 {
		return 0, getLibError()
	}
	return int(outLen), nil
}
