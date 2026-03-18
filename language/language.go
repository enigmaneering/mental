package language

import "strings"

// Language represents shader programming languages that can be compiled or transpiled
// for GPU execution. The glitter package automatically detects the source language
// and transpiles to the target platform's native format when needed.
//
// Examples of language usage:
//   - GLSL is used by OpenGL and Vulkan
//   - HLSL is used by Direct3D on Windows
//   - MSL is used by Metal on macOS/iOS
//   - SPIRV is a portable intermediate representation
//
// See Language, Auto, GLSL, HLSL, MSL, and SPIRV.
type Language byte

const (
	// Auto indicates automatic language detection. The glitter package will attempt
	// to detect the shader language from the source code syntax and automatically
	// transpile to the native format required by the current backend.
	//
	// This is the recommended option for most use cases as it provides maximum
	// portability across platforms.
	//
	// See Language and Auto.
	Auto Language = iota

	// GLSL (OpenGL Shading Language) is used by OpenGL and Vulkan.
	// Version 450+ is recommended for compute shaders.
	//
	// Example:
	//	#version 450
	//	layout(local_size_x = 256) in;
	//	void main() { ... }
	//
	// See Language and GLSL.
	GLSL

	// HLSL (High-Level Shading Language) is Microsoft's shader language used by Direct3D.
	// Commonly used on Windows platforms.
	//
	// Example:
	//	[numthreads(256, 1, 1)]
	//	void main(uint3 id : SV_DispatchThreadID) { ... }
	//
	// See Language and HLSL.
	HLSL

	// MSL (Metal Shading Language) is Apple's shader language used by Metal.
	// Required for macOS, iOS, and other Apple platforms.
	//
	// Example:
	//	kernel void compute(uint id [[thread_position_in_grid]]) { ... }
	//
	// See Language and MSL.
	MSL

	// SPIRV (Standard Portable Intermediate Representation - V) is a binary intermediate
	// language for representing graphical-shader stages and compute kernels.
	// It serves as a portable compilation target that can be transpiled to other languages.
	//
	// See Language and SPIRV.
	SPIRV

	// WGSL (WebGPU Shading Language) is the shader language for WebGPU.
	// Used for web-based GPU compute and graphics.
	//
	// Example:
	//	@compute @workgroup_size(256)
	//	fn main(@builtin(global_invocation_id) id: vec3<u32>) { ... }
	//
	// See Language and WGSL.
	WGSL
)

// String returns a concise representation of the Language.
func (l Language) String() string {
	switch l {
	case Auto:
		return "Auto"
	case GLSL:
		return "GLSL"
	case HLSL:
		return "HLSL"
	case MSL:
		return "MSL"
	case SPIRV:
		return "SPIR-V"
	case WGSL:
		return "WGSL"
	default:
		return "Unknown"
	}
}

// StringFull returns a full representation of the Language.
func (l Language) StringFull() string {
	switch l {
	case Auto:
		return "Automatic Detection"
	case GLSL:
		return "OpenGL Shading Language"
	case HLSL:
		return "High-Level Shading Language"
	case MSL:
		return "Metal Shading Language"
	case SPIRV:
		return "Standard Portable Intermediate Representation - V"
	case WGSL:
		return "WebGPU Shading Language"
	default:
		return "Unknown"
	}
}

// Detect analyzes shader source code and attempts to identify the language.
// It examines syntax patterns and language-specific markers to determine
// which shader language the source is written in.
//
// Detection patterns:
//   - GLSL: Presence of #version directive
//   - HLSL: Presence of [numthreads] attribute or semantic annotations (:SV_)
//   - MSL: Presence of kernel keyword or [[ ]] attributes
//   - SPIRV: Binary format starting with magic number
//
// If detection is ambiguous or fails, returns Auto.
//
// Example:
//
//	source := `
//	    #version 450
//	    layout(local_size_x = 256) in;
//	    void main() { ... }
//	`
//	lang := language.Detect(source)  // Returns language.GLSL
func Detect(source string) Language {
	// Trim whitespace for cleaner analysis
	trimmed := strings.TrimSpace(source)

	// Empty source
	if len(trimmed) == 0 {
		return Auto
	}

	// Check for SPIR-V magic number (binary format)
	// SPIR-V magic: 0x07230203
	if len(source) >= 4 {
		if source[0] == 0x03 && source[1] == 0x02 && source[2] == 0x23 && source[3] == 0x07 {
			return SPIRV
		}
	}

	// GLSL detection: #version directive
	if strings.Contains(trimmed, "#version") {
		return GLSL
	}

	// MSL detection: kernel keyword or Metal-specific attributes
	if strings.Contains(trimmed, "kernel ") ||
	   strings.Contains(trimmed, "[[thread_position_in_grid]]") ||
	   strings.Contains(trimmed, "[[buffer(") ||
	   strings.Contains(trimmed, "[[texture(") {
		return MSL
	}

	// HLSL detection: [numthreads] attribute or semantic annotations
	if strings.Contains(trimmed, "[numthreads") ||
	   strings.Contains(trimmed, ": SV_") ||
	   strings.Contains(trimmed, "SV_DispatchThreadID") ||
	   strings.Contains(trimmed, "SV_GroupThreadID") {
		return HLSL
	}

	// WGSL detection: @compute, @group, @binding, or fn keyword
	if strings.Contains(trimmed, "@compute") ||
	   strings.Contains(trimmed, "@group") ||
	   strings.Contains(trimmed, "@binding") ||
	   strings.Contains(trimmed, "@builtin") ||
	   (strings.Contains(trimmed, "fn ") && strings.Contains(trimmed, "->")) {
		return WGSL
	}

	// Could not definitively detect
	return Auto
}
