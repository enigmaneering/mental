package mental

import (
	"embed"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
)

//go:embed all:lib
var libFS embed.FS

// loadFromEmbed extracts the embedded library for the current platform
// to a temporary file and loads it.
func loadFromEmbed() (uintptr, error) {
	name := libName()
	platform := runtime.GOOS + "-" + runtime.GOARCH
	embedPath := filepath.Join("lib", platform, name)

	data, err := libFS.ReadFile(embedPath)
	if err != nil {
		return 0, fmt.Errorf("embedded lib not found at %s: %w", embedPath, err)
	}

	// Write to a temp file. The file must persist while the library is loaded.
	tmp, err := os.CreateTemp("", "mental-*-"+name)
	if err != nil {
		return 0, fmt.Errorf("create temp file: %w", err)
	}
	tmpPath := tmp.Name()

	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		os.Remove(tmpPath)
		return 0, fmt.Errorf("write temp lib: %w", err)
	}
	tmp.Close()

	// Make executable on Unix.
	if runtime.GOOS != "windows" {
		os.Chmod(tmpPath, 0755)
	}

	h, err := openLibrary(tmpPath)
	if err != nil {
		os.Remove(tmpPath)
		return 0, fmt.Errorf("load embedded lib from %s: %w", tmpPath, err)
	}

	// Best-effort cleanup on Unix: unlink the temp file now.
	// The OS keeps the file open via the fd until the process exits.
	// On Windows we can't unlink while loaded, so record the path
	// for automatic removal via the C library's atexit handler.
	if runtime.GOOS != "windows" {
		os.Remove(tmpPath)
	} else {
		embedTmpPath = tmpPath
	}

	return h, nil
}

// embedTmpPath holds the temp library path on Windows for deferred cleanup.
var embedTmpPath string
