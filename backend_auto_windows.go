//go:build windows

package mental

import (
	"fmt"

	"git.enigmaneering.org/mental/api"
)

// enumerateDevices detects available GPU devices on Windows.
// Tries Direct3D 12 first, then falls back to OpenCL for older systems.
func enumerateDevices() ([]Info, error) {
	// Try D3D12 first (available on Windows 10+ since 2015)
	devices, err := enumerateD3D12Devices()
	if err == nil && len(devices) > 0 {
		// Assign indices to devices
		for i := range devices {
			devices[i].Index = i
		}
		return devices, nil
	}

	// Fall back to OpenCL (broader compatibility, including Windows 7/8)
	devices, err = enumerateOpenCLDevices()
	if err == nil && len(devices) > 0 {
		// Assign indices to devices
		for i := range devices {
			devices[i].Index = i
		}
		return devices, nil
	}

	// If both failed, return helpful error
	return nil, fmt.Errorf("Direct3D 12 unavailable and OpenCL not found - please install either to proceed. If operating without a GPU, you may install PoCL for CPU-based compute")
}

// createBackend creates the appropriate backend for Windows based on device API.
func createBackend(dev Info) (backend, error) {
	switch dev.API {
	case api.Direct3D12:
		return newD3D12Backend(dev.Index)
	case api.OpenCL:
		return newOpenCLBackend(dev)
	default:
		return nil, fmt.Errorf("unsupported API on Windows: %v", dev.API)
	}
}
