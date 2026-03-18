package mental

// selectBestDevice selects the best GPU device from the available list.
// Preference order: discrete GPU > integrated GPU > other
func selectBestDevice(devices []Info) int {
	// Prefer discrete GPU
	for i, dev := range devices {
		if dev.Type == Discrete {
			return i
		}
	}

	// Fall back to integrated
	for i, dev := range devices {
		if dev.Type == Integrated {
			return i
		}
	}

	// Use first available
	return 0
}
