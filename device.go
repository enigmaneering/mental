package mental

import (
	"git.enigmaneering.org/mental/api"
)

// Type represents the category of GPU device.
type Type byte

const (
	// Other represents devices that don't fit other categories.
	// This is typically used for unknown or unclassified GPU hardware.
	//
	// See Type and Other.
	Other Type = iota

	// Integrated represents GPUs integrated with the CPU.
	// These GPUs share system memory with the CPU and are typically
	// found in laptops and low-power systems.
	//
	// Examples: Intel integrated graphics, Apple Silicon GPU, AMD APUs
	//
	// See Type and Integrated.
	Integrated

	// Discrete represents dedicated GPU cards with their own memory.
	// These are separate hardware components with dedicated VRAM and
	// typically provide higher performance than integrated GPUs.
	//
	// Examples: NVIDIA GeForce/Quadro, AMD Radeon
	//
	// See Type and Discrete.
	Discrete

	// Virtual represents virtual or emulated GPUs.
	// These are software-based GPU implementations typically used
	// in virtual machines or for testing purposes.
	//
	// Examples: SwiftShader, software renderers, VM pass-through GPUs
	//
	// See Type and Virtual.
	Virtual
)

// String returns a concise representation of the device Type.
func (dt Type) String() string {
	switch dt {
	case Integrated:
		return "Integrated"
	case Discrete:
		return "Discrete"
	case Virtual:
		return "Virtual"
	default:
		return "Other"
	}
}

// StringFull returns a full representation of the device Type.
func (dt Type) StringFull() string {
	switch dt {
	case Integrated:
		return "Integrated GPU"
	case Discrete:
		return "Discrete GPU"
	case Virtual:
		return "Virtual GPU"
	default:
		return "Other GPU Type"
	}
}

// Info contains information about a GPU device.
type Info struct {
	Index    int     // Index in the device list
	Name     string  // Device name (e.g., "Apple M1", "NVIDIA RTX 4090")
	Type     Type    // Device type (integrated, discrete, etc.)
	VendorID uint32  // PCI vendor ID
	API      api.API // Which GPU API this device uses
}
