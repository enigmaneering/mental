package api

import (
	"git.enigmaneering.org/mental/language"
)

// API represents GPU programming interfaces that provide access to graphics hardware.
// The glitter package automatically selects the appropriate API based on platform
// and available hardware, abstracting away platform-specific differences.
//
// Examples of API usage:
//   - Metal is used on macOS, iOS, and other Apple platforms
//   - Vulkan is cross-platform (Linux, Windows, macOS via MoltenVK)
//   - Direct3D12 is used on Windows and Xbox
//
// See API, Metal, Vulkan, and Direct3D12.
type API byte

const (
	// Metal is Apple's modern GPU API used on macOS, iOS, iPadOS, tvOS, and visionOS.
	// It provides low-level access to GPU hardware with minimal driver overhead.
	//
	// Metal is the only GPU API available on Apple Silicon and modern Apple platforms.
	//
	// See API and Metal.
	Metal API = iota

	// Vulkan is a cross-platform GPU API developed by the Khronos Group.
	// It provides explicit control over GPU operations and is available on
	// Windows, Linux, Android, and macOS (via MoltenVK translation layer).
	//
	// Vulkan offers excellent portability and is widely supported across vendors
	// (NVIDIA, AMD, Intel, ARM, Qualcomm).
	//
	// See API and Vulkan.
	Vulkan

	// Direct3D12 is Microsoft's modern GPU API for Windows and Xbox platforms.
	// It provides low-level hardware access and is optimized for Windows systems.
	//
	// Direct3D12 is the native API on Windows and offers the best performance
	// and feature support on Microsoft platforms.
	//
	// See API and Direct3D12.
	Direct3D12

	// OpenCL is a cross-platform parallel computing API developed by the Khronos Group.
	// It provides compute-focused operations and is widely available across platforms
	// and vendors, including CPU fallback implementations.
	//
	// OpenCL is used as a fallback when Vulkan is unavailable on Linux systems.
	// It offers broad compatibility and can run on GPUs, CPUs, and accelerators.
	//
	// See API and OpenCL.
	OpenCL
)

// String returns a concise representation of the API.
func (a API) String() string {
	switch a {
	case Metal:
		return "Metal"
	case Vulkan:
		return "Vulkan"
	case Direct3D12:
		return "Direct3D 12"
	case OpenCL:
		return "OpenCL"
	default:
		return "Unknown"
	}
}

// StringFull returns a full representation of the API.
func (a API) StringFull() string {
	switch a {
	case Metal:
		return "Apple Metal Graphics API"
	case Vulkan:
		return "Khronos Vulkan Graphics API"
	case Direct3D12:
		return "Microsoft Direct3D 12 Graphics API"
	case OpenCL:
		return "Khronos OpenCL Compute API"
	default:
		return "Unknown Graphics API"
	}
}

// NativeLanguage returns the shader language natively used by this API.
func (a API) NativeLanguage() language.Language {
	switch a {
	case Metal:
		return language.MSL
	case Vulkan:
		return language.GLSL
	case Direct3D12:
		return language.HLSL
	case OpenCL:
		return language.GLSL // OpenCL C is similar to C99, but we'll use GLSL as source
	default:
		return language.Auto
	}
}
