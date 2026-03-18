// Package transpile provides automatic shader language transpilation using SPIRV
// as an intermediate representation. It enables cross-platform shader development
// by converting between GLSL, HLSL, MSL, WGSL, and SPIRV formats.
//
// The transpilation pipeline:
//  1. Source language (GLSL/HLSL/WGSL) → SPIRV (compilation)
//  2. SPIRV → Target language (GLSL/HLSL/MSL) (transpilation)
//
// This package uses:
//   - glslang for GLSL → SPIRV
//   - DXC for HLSL → SPIRV
//   - Naga for WGSL → SPIRV
//   - SPIRV-Cross for SPIRV → GLSL/HLSL/MSL
//
// Note: MSL → SPIRV is not supported as MSL is designed as a compilation target only.
//
// Build the required C++ libraries by running:
//
//	go generate git.enigmaneering.org/mental/transpile
//
// Usage:
//
//	// Transpile between languages
//	msl, err := transpile.To(glslSource, language.GLSL, language.MSL)
//
//	// Compile to SPIRV bytecode
//	spirv, err := transpile.ToSPIRV(hlslSource, language.HLSL)
package transpile

//go:generate bash -c "cd $(dirname $GOFILE) && ./build.sh"

import (
	"fmt"

	"git.enigmaneering.org/mental/language"
)

// To converts shader source from one language to another via SPIRV.
// It automatically detects the source language if not specified.
//
// Parameters:
//   - source: Shader source code
//   - srcLang: Source language (use language.Auto for detection)
//   - dstLang: Target language
//
// Returns:
//   - Transpiled source code
//   - Error if transpilation fails
//
// Example:
//
//	msl, err := transpile.To(glslSource, language.GLSL, language.MSL)
func To(source string, srcLang, dstLang language.Language) (string, error) {
	// Auto-detect source language if needed
	if srcLang == language.Auto {
		srcLang = language.Detect(source)
		if srcLang == language.Auto {
			return "", fmt.Errorf("transpile: could not detect source language")
		}
	}

	// No transpilation needed if source and destination are the same
	if srcLang == dstLang {
		return source, nil
	}

	// If source is already SPIRV, just transpile to target
	if srcLang == language.SPIRV {
		return spirvToTarget([]byte(source), dstLang)
	}

	// If destination is SPIRV, compile source to SPIRV
	if dstLang == language.SPIRV {
		spirv, err := sourceToSPIRV(source, srcLang)
		if err != nil {
			return "", err
		}
		return string(spirv), nil
	}

	// Otherwise: Source → SPIRV → Target
	spirv, err := sourceToSPIRV(source, srcLang)
	if err != nil {
		return "", fmt.Errorf("transpile: %w", err)
	}

	result, err := spirvToTarget(spirv, dstLang)
	if err != nil {
		return "", fmt.Errorf("transpile: %w", err)
	}

	return result, nil
}

// ToSPIRV compiles shader source to SPIRV bytecode.
// Automatically detects the source language if not specified.
//
// Parameters:
//   - source: Shader source code
//   - srcLang: Source language (use language.Auto for detection)
//
// Returns:
//   - SPIRV bytecode as uint32 slice
//   - Error if compilation fails
//
// Example:
//
//	spirv, err := transpile.ToSPIRV(glslSource, language.GLSL)
func ToSPIRV(source string, srcLang language.Language) ([]uint32, error) {
	// Auto-detect source language if needed
	if srcLang == language.Auto {
		srcLang = language.Detect(source)
		if srcLang == language.Auto {
			return nil, fmt.Errorf("transpile: could not detect source language")
		}
	}

	// Compile to SPIRV bytes
	spirvBytes, err := sourceToSPIRV(source, srcLang)
	if err != nil {
		return nil, fmt.Errorf("transpile: %w", err)
	}

	// Convert bytes to uint32 slice (SPIRV is uint32-aligned)
	if len(spirvBytes)%4 != 0 {
		return nil, fmt.Errorf("transpile: SPIRV bytecode not 4-byte aligned")
	}

	spirv := make([]uint32, len(spirvBytes)/4)
	for i := 0; i < len(spirv); i++ {
		spirv[i] = uint32(spirvBytes[i*4]) |
			uint32(spirvBytes[i*4+1])<<8 |
			uint32(spirvBytes[i*4+2])<<16 |
			uint32(spirvBytes[i*4+3])<<24
	}

	return spirv, nil
}

// ToDXIL compiles shader source to DXIL bytecode for Direct3D 12.
// Automatically transpiles non-HLSL sources via SPIRV.
//
// Parameters:
//   - source: Shader source code
//   - srcLang: Source language (use language.Auto for detection)
//
// Returns:
//   - DXIL bytecode (binary shader blob)
//   - Error if compilation fails
//
// Example:
//
//	dxil, err := transpile.ToDXIL(glslSource, language.GLSL)
func ToDXIL(source string, srcLang language.Language) ([]byte, error) {
	// Auto-detect source language if needed
	if srcLang == language.Auto {
		srcLang = language.Detect(source)
		if srcLang == language.Auto {
			return nil, fmt.Errorf("transpile: could not detect source language")
		}
	}

	var hlslSource string
	var err error

	// Convert to HLSL if not already
	if srcLang == language.HLSL {
		hlslSource = source
	} else if srcLang == language.GLSL {
		// GLSL → SPIRV → HLSL
		hlslSource, err = To(source, language.GLSL, language.HLSL)
		if err != nil {
			return nil, fmt.Errorf("transpile: failed to convert GLSL to HLSL: %w", err)
		}
	} else {
		return nil, fmt.Errorf("transpile: unsupported source language for DXIL: %v", srcLang)
	}

	// Compile HLSL to DXIL using DXC
	dxil, err := compileDXIL(hlslSource)
	if err != nil {
		return nil, fmt.Errorf("transpile: DXIL compilation failed: %w", err)
	}

	return dxil, nil
}
