//go:build linux

package mental

import (
	"fmt"

	"git.enigmaneering.org/mental/api"
)

// enumerateDevices detects available GPU devices on Linux.
// Tries Vulkan first, then falls back to OpenCL if Vulkan is unavailable.
func enumerateDevices() ([]Info, error) {
	// Try Vulkan first (preferred for modern systems)
	devices, err := enumerateVulkanDevices()
	if err == nil && len(devices) > 0 {
		// Assign indices to devices
		for i := range devices {
			devices[i].Index = i
		}
		return devices, nil
	}

	// Fall back to OpenCL (broader compatibility, including CPU fallback)
	devices, err = enumerateOpenCLDevices()
	if err == nil && len(devices) > 0 {
		// Assign indices to devices
		for i := range devices {
			devices[i].Index = i
		}
		return devices, nil
	}

	// If both failed, return helpful error
	return nil, fmt.Errorf("Vulkan unavailable and OpenCL not found - please install either to proceed. If operating without a GPU, you may install PoCL for CPU-based compute")
}

// createBackend creates the appropriate backend for Linux based on device API.
func createBackend(dev Info) (backend, error) {
	switch dev.API {
	case api.Vulkan:
		return newVulkanBackend(dev)
	case api.OpenCL:
		return newOpenCLBackend(dev)
	default:
		return nil, fmt.Errorf("unsupported API on Linux: %v", dev.API)
	}
}
