//go:build darwin

package mental

import (
	"fmt"

	"git.enigmaneering.org/mental/api"
)

// enumerateDevices detects available GPU devices on macOS.
// Tries Metal first, then falls back to OpenCL for older systems.
func enumerateDevices() ([]Info, error) {
	// Try Metal first (available on OS X El Capitan 10.11+ with 2012+ Macs)
	devices, err := enumerateMetalDevices()
	if err == nil && len(devices) > 0 {
		// Assign indices to devices
		for i := range devices {
			devices[i].Index = i
		}
		return devices, nil
	}

	// Fall back to OpenCL (available since OS X Lion 10.7)
	devices, err = enumerateOpenCLDevices()
	if err == nil && len(devices) > 0 {
		// Assign indices to devices
		for i := range devices {
			devices[i].Index = i
		}
		return devices, nil
	}

	// If both failed, return helpful error
	return nil, fmt.Errorf("Metal unavailable and OpenCL not found - please install either to proceed. If operating without a GPU, you may install PoCL for CPU-based compute")
}

// createBackend creates the appropriate backend for macOS based on device API.
func createBackend(dev Info) (backend, error) {
	switch dev.API {
	case api.Metal:
		return newMetalBackend(dev.Index)
	case api.OpenCL:
		return newOpenCLBackend(dev)
	default:
		return nil, fmt.Errorf("unsupported API on macOS: %v", dev.API)
	}
}
