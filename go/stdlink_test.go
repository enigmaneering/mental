package mental

import "testing"

func TestStdlinkCreation(t *testing.T) {
	skipIfNoLibrary(t)
	fd := Stdlink()
	if fd < 0 {
		t.Fatalf("Stdlink() = %d, want >= 0", fd)
	}
	t.Logf("stdlink fd = %d", fd)

	peer := StdlinkPeer()
	if peer < 0 {
		t.Fatalf("StdlinkPeer() = %d, want >= 0", peer)
	}
	t.Logf("stdlink peer = %d", peer)

	if fd == peer {
		t.Error("stdlink fd and peer should differ")
	}
}

func TestStdlinkIdempotent(t *testing.T) {
	skipIfNoLibrary(t)
	a := Stdlink()
	b := Stdlink()
	if a != b {
		t.Errorf("Stdlink() not idempotent: %d != %d", a, b)
	}

	pa := StdlinkPeer()
	pb := StdlinkPeer()
	if pa != pb {
		t.Errorf("StdlinkPeer() not idempotent: %d != %d", pa, pb)
	}
}

func TestStdlinkSendRecv(t *testing.T) {
	skipIfNoLibrary(t)

	// Send a message on the near end.
	msg := []byte("hello from Go")
	if err := StdlinkSend(msg); err != nil {
		t.Fatalf("StdlinkSend: %v", err)
	}

	// To receive, we'd need a reader on the peer end.
	// In a real test with threads this would round-trip.
	// For now, just verify send doesn't error.
	t.Log("StdlinkSend succeeded (round-trip requires peer reader)")
}
