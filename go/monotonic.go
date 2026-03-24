package mental

// Monotonic returns the next value from a process-local, lock-free,
// strictly increasing 64-bit counter (1, 2, 3, …).  Never returns 0.
//
// Thread-safe.  For globally unique IDs across a spark cluster,
// combine with process identity (e.g., Stdlink() fd or os.Getpid()).
func Monotonic() uint64 {
	return uint64(call0(ft.monotonic))
}
