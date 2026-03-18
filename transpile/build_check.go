package transpile

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
)

// NOTE: Library downloading is now handled by git.enigmaneering.org/enigmatic
// via mental/init.go. This file is kept for legacy compatibility and manual build checks.

// checkLibrariesBuilt verifies that the C++ libraries have been built.
// This is no longer called automatically - libraries are downloaded via external.EnsureLibraries() in mind/init.go
func checkLibrariesBuilt() {
	// Get the path to this source file
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		// Can't determine source path - CGo will fail with linker errors if libs are missing
		return
	}

	// This file is in: life/soul/core/mind/transpile/
	// Libraries are in: life/external/
	transpileDir := filepath.Dir(filename)
	externalDir := filepath.Join(transpileDir, "..", "..", "..", "..", "external")

	// Check for build completion markers
	missingLibs := []string{}

	glslangMarker := filepath.Join(externalDir, "glslang", "build", ".build_complete")
	if _, err := os.Stat(glslangMarker); os.IsNotExist(err) {
		missingLibs = append(missingLibs, "glslang")
	}

	// DXC is downloaded as pre-built binary, not built from source
	// Check for dxc executable instead of build marker
	dxcBinary := filepath.Join(externalDir, "dxc", "bin", "dxc.exe")
	if runtime.GOOS != "windows" {
		dxcBinary = filepath.Join(externalDir, "dxc", "bin", "dxc")
	}
	if _, err := os.Stat(dxcBinary); os.IsNotExist(err) {
		missingLibs = append(missingLibs, "DXC")
	}

	spirvCrossMarker := filepath.Join(externalDir, "spirv-cross", "build", ".build_complete")
	if _, err := os.Stat(spirvCrossMarker); os.IsNotExist(err) {
		missingLibs = append(missingLibs, "SPIRV-Cross")
	}

	// If any libraries are missing, try downloading pre-built libraries first
	if len(missingLibs) > 0 {
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "================================================================\n")
		fmt.Fprintf(os.Stderr, "Missing C++ transpilation libraries: %v\n", missingLibs)
		fmt.Fprintf(os.Stderr, "================================================================\n")
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "Attempting to download pre-built libraries...\n")
		fmt.Fprintf(os.Stderr, "\n")

		// Try downloading with retries
		err := tryDownloadWithRetry(3)
		if err == nil {
			// Download succeeded, verify libraries are now present
			fmt.Fprintf(os.Stderr, "Verifying installation...\n")
			missingAfterDownload := []string{}
			if _, err := os.Stat(glslangMarker); os.IsNotExist(err) {
				missingAfterDownload = append(missingAfterDownload, "glslang")
			}
			if _, err := os.Stat(dxcBinary); os.IsNotExist(err) {
				missingAfterDownload = append(missingAfterDownload, "DXC")
			}
			if _, err := os.Stat(spirvCrossMarker); os.IsNotExist(err) {
				missingAfterDownload = append(missingAfterDownload, "SPIRV-Cross")
			}

			if len(missingAfterDownload) == 0 {
				fmt.Fprintf(os.Stderr, "All libraries installed successfully!\n")
				fmt.Fprintf(os.Stderr, "================================================================\n")
				fmt.Fprintf(os.Stderr, "\n")
				return // Success!
			}
			missingLibs = missingAfterDownload
		}

		// Download failed or incomplete - show manual build instructions
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "================================================================\n")
		fmt.Fprintf(os.Stderr, "ERROR: Could not download pre-built libraries\n")
		fmt.Fprintf(os.Stderr, "================================================================\n")
		fmt.Fprintf(os.Stderr, "\n")
		if err != nil {
			fmt.Fprintf(os.Stderr, "Download error: %v\n", err)
			fmt.Fprintf(os.Stderr, "\n")
		}
		fmt.Fprintf(os.Stderr, "Still missing: %v\n", missingLibs)
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "Please build the required libraries manually:\n")
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "  1. Install cmake:\n")
		fmt.Fprintf(os.Stderr, "     macOS:   brew install cmake\n")
		fmt.Fprintf(os.Stderr, "     Linux:   sudo apt install cmake\n")
		fmt.Fprintf(os.Stderr, "     Windows: choco install cmake\n")
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "  2. Build libraries:\n")
		fmt.Fprintf(os.Stderr, "     go generate git.enigmaneering.org/mental/transpile\n")
		fmt.Fprintf(os.Stderr, "\n")
		fmt.Fprintf(os.Stderr, "Build time: ~2-5 minutes (first time only)\n")
		fmt.Fprintf(os.Stderr, "================================================================\n")
		fmt.Fprintf(os.Stderr, "\n")

		panic("transpile: C++ libraries not available - see error message above")
	}
}
