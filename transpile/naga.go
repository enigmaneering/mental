package transpile

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// getNagaPath returns the path to the Naga compiler binary
func getNagaPath() (string, error) {
	// Try to find Naga in the vendored location relative to this source file
	// This file is in life/soul/core/mental/transpile/
	// Naga is in life/external/naga/bin/naga (Unix) or life/external/naga/naga.exe (Windows)
	_, filename, _, ok := runtime.Caller(0)
	if ok {
		// Get the directory containing this source file
		sourceDir := filepath.Dir(filename)
		nagaBinary := "naga"
		if runtime.GOOS == "windows" {
			nagaBinary = "naga.exe"
		}

		// Try external/naga/bin/naga first (Unix build layout)
		nagaPath := filepath.Join(sourceDir, "..", "..", "..", "..", "external", "naga", "bin", nagaBinary)
		nagaPath = filepath.Clean(nagaPath)
		if _, err := os.Stat(nagaPath); err == nil {
			return nagaPath, nil
		}

		// Fall back to external/naga/naga.exe (Windows layout)
		nagaPath = filepath.Join(sourceDir, "..", "..", "..", "..", "external", "naga", nagaBinary)
		nagaPath = filepath.Clean(nagaPath)
		if _, err := os.Stat(nagaPath); err == nil {
			return nagaPath, nil
		}
	}

	// Fall back to system PATH
	return exec.LookPath("naga")
}

// compileWGSLWithNaga compiles WGSL compute shader source to SPIRV binary.
// Uses Naga CLI from the wgpu project for WGSL to SPIRV compilation.
func compileWGSLWithNaga(source string) ([]byte, error) {
	nagaPath, err := getNagaPath()
	if err != nil {
		return nil, fmt.Errorf("Naga compiler not found: %w (did you run transpile/build.sh?)", err)
	}

	// Create temporary directory for compilation
	tmpDir, err := os.MkdirTemp("", "wgsl-compile-*")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp directory: %w", err)
	}
	defer os.RemoveAll(tmpDir)

	srcFile := filepath.Join(tmpDir, "shader.wgsl")
	if err := os.WriteFile(srcFile, []byte(source), 0644); err != nil {
		return nil, fmt.Errorf("failed to write WGSL source: %w", err)
	}

	outFile := filepath.Join(tmpDir, "shader.spv")

	// Compile WGSL to SPIRV using Naga
	// Naga CLI: naga <input.wgsl> <output.spv>
	cmd := exec.Command(nagaPath, srcFile, outFile)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("Naga compilation failed: %w\nOutput: %s", err, string(output))
	}

	// Read compiled SPIRV binary
	spirv, err := os.ReadFile(outFile)
	if err != nil {
		return nil, fmt.Errorf("failed to read SPIRV output: %w", err)
	}

	return spirv, nil
}
