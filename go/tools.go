package mental

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"unsafe"
)

// Tool IDs matching the C enum mental_tool.
const (
	toolDXC  = 0 // MENTAL_TOOL_DXC
	toolNaga = 1 // MENTAL_TOOL_NAGA
)

// toolSpec describes an external tool to locate.
type toolSpec struct {
	id   int
	name string // bare executable name (without .exe)
}

var tools = []toolSpec{
	{toolDXC, "dxc"},
	{toolNaga, "naga"},
}

// configureTools is called once during Init after symbols are resolved.
// For each tool it checks: system PATH first, then embedded fallback.
func configureTools() {
	for _, t := range tools {
		exeName := t.name
		if runtime.GOOS == "windows" {
			exeName += ".exe"
		}

		// 1. Prefer system-installed version.
		if path, err := exec.LookPath(exeName); err == nil {
			setToolPath(t.id, path)
			continue
		}

		// 2. Extract embedded binary to cache directory.
		platform := runtime.GOOS + "-" + runtime.GOARCH
		embedPath := filepath.Join("lib", platform, exeName)
		data, err := libFS.ReadFile(embedPath)
		if err != nil {
			continue // No embedded binary — tool simply won't be available.
		}

		cacheDir, err := toolCacheDir()
		if err != nil {
			continue
		}

		toolPath := filepath.Join(cacheDir, exeName)
		if needsWrite(toolPath, int64(len(data))) {
			if err := os.WriteFile(toolPath, data, 0755); err != nil {
				continue
			}
		}

		setToolPath(t.id, toolPath)
	}
}

// setToolPath calls mental_set_tool_path in the C library.
func setToolPath(tool int, path string) {
	cstr := append([]byte(path), 0) // null-terminated
	call2(ft.setToolPath, uintptr(tool), uintptr(unsafe.Pointer(&cstr[0])))
}

// toolCacheDir returns (and creates) the cache directory for extracted tools.
func toolCacheDir() (string, error) {
	base, err := os.UserCacheDir()
	if err != nil {
		// Fall back to temp directory if no user cache is available.
		base = os.TempDir()
	}
	dir := filepath.Join(base, "mental", "tools")
	return dir, os.MkdirAll(dir, 0755)
}

// needsWrite returns true if the file at path is missing or a different size.
func needsWrite(path string, size int64) bool {
	info, err := os.Stat(path)
	return err != nil || info.Size() != size
}
