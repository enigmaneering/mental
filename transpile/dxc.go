package transpile

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"

	"git.enigmaneering.org/mental/language"
)

// getDXCPath returns the path to the DXC compiler binary
func getDXCPath() (string, error) {
	// Try to find DXC in the vendored location relative to this source file
	// This file is in life/soul/core/mental/transpile/
	// DXC is in life/external/dxc/dxc.exe (Windows) or life/external/dxc/bin/dxc (Unix) - pre-built binary
	_, filename, _, ok := runtime.Caller(0)
	if ok {
		// Get the directory containing this source file
		sourceDir := filepath.Dir(filename)
		dxcBinary := "dxc"
		if runtime.GOOS == "windows" {
			dxcBinary = "dxc.exe"
		}

		// Try external/dxc/dxc.exe first (Windows NuGet package layout)
		dxcPath := filepath.Join(sourceDir, "..", "..", "..", "..", "external", "dxc", dxcBinary)
		dxcPath = filepath.Clean(dxcPath)
		if _, err := os.Stat(dxcPath); err == nil {
			return dxcPath, nil
		}

		// Fall back to external/dxc/bin/dxc (Unix build layout)
		dxcPath = filepath.Join(sourceDir, "..", "..", "..", "..", "external", "dxc", "bin", dxcBinary)
		dxcPath = filepath.Clean(dxcPath)
		if _, err := os.Stat(dxcPath); err == nil {
			return dxcPath, nil
		}
	}

	// Fall back to system PATH
	return exec.LookPath("dxc")
}

// compileHLSLWithDXC compiles HLSL compute shader source to SPIRV binary using DXC.
// Uses Microsoft's DirectX Shader Compiler with -spirv flag for SPIRV code generation.
func compileHLSLWithDXC(source string) ([]byte, error) {
	dxcPath, err := getDXCPath()
	if err != nil {
		return nil, fmt.Errorf("DXC compiler not found: %w (did you run transpile/build.sh?)", err)
	}

	// Create temporary file for HLSL source
	tmpDir, err := os.MkdirTemp("", "hlsl-compile-*")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp directory: %w", err)
	}
	defer os.RemoveAll(tmpDir)

	srcFile := filepath.Join(tmpDir, "shader.hlsl")
	if err := os.WriteFile(srcFile, []byte(source), 0644); err != nil {
		return nil, fmt.Errorf("failed to write HLSL source: %w", err)
	}

	outFile := filepath.Join(tmpDir, "shader.spv")

	// Compile HLSL to SPIRV using DXC
	// -spirv: Generate SPIRV output
	// -T cs_6_0: Target compute shader model 6.0
	// -E main: Entry point (we'll need to detect this or make it configurable)
	// -Fo: Output file
	cmd := exec.Command(dxcPath,
		"-spirv",
		"-T", "cs_6_0",
		"-E", "main",
		"-Fo", outFile,
		srcFile,
	)

	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("DXC compilation failed: %w\nOutput: %s", err, string(output))
	}

	// Read compiled SPIRV binary
	spirv, err := os.ReadFile(outFile)
	if err != nil {
		return nil, fmt.Errorf("failed to read SPIRV output: %w", err)
	}

	return spirv, nil
}

// compileDXIL compiles HLSL compute shader source to DXIL binary using DXC.
// DXIL is the native shader format for Direct3D 12.
func compileDXIL(source string) ([]byte, error) {
	dxcPath, err := getDXCPath()
	if err != nil {
		return nil, fmt.Errorf("DXC compiler not found: %w (did you run transpile/build.sh?)", err)
	}

	// Create temporary file for HLSL source
	tmpDir, err := os.MkdirTemp("", "hlsl-compile-dxil-*")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp directory: %w", err)
	}
	defer os.RemoveAll(tmpDir)

	srcFile := filepath.Join(tmpDir, "shader.hlsl")
	if err := os.WriteFile(srcFile, []byte(source), 0644); err != nil {
		return nil, fmt.Errorf("failed to write HLSL source: %w", err)
	}

	outFile := filepath.Join(tmpDir, "shader.dxil")

	// Compile HLSL to DXIL using DXC
	// -T cs_6_0: Target compute shader model 6.0
	// -E main: Entry point
	// -Fo: Output file
	cmd := exec.Command(dxcPath,
		"-T", "cs_6_0",
		"-E", "main",
		"-Fo", outFile,
		srcFile,
	)

	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("DXC DXIL compilation failed: %w\nOutput: %s", err, string(output))
	}

	// Read compiled DXIL binary
	dxil, err := os.ReadFile(outFile)
	if err != nil {
		return nil, fmt.Errorf("failed to read DXIL output: %w", err)
	}

	return dxil, nil
}

// sourceToSPIRV converts shader source to SPIRV binary based on the source language
func sourceToSPIRV(source string, srcLang language.Language) ([]byte, error) {
	switch srcLang {
	case language.GLSL:
		return compileGLSLWithGlslang(source)

	case language.HLSL:
		return compileHLSLWithDXC(source)

	case language.MSL:
		// MSL cannot be directly compiled to SPIRV
		// MSL is Apple's proprietary format - we can only transpile FROM SPIRV to MSL
		return nil, fmt.Errorf("MSL cannot be compiled to SPIRV - use GLSL, HLSL, or WGSL as source")

	case language.WGSL:
		return compileWGSLWithNaga(source)

	case language.SPIRV:
		// Already SPIRV
		return []byte(source), nil

	default:
		return nil, fmt.Errorf("unsupported source language: %v", srcLang)
	}
}
