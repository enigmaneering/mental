package mental

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
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

// configureTools is called once during init after symbols are resolved.
// For each tool it checks: system PATH first, then extracts the embedded
// binary to a temporary file as a fallback. Extracted temp files are
// automatically cleaned up at process exit via Defer.
func configureTools() {
	var tempPaths []string

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

		// 2. Extract embedded binary to a temporary file.
		platform := runtime.GOOS + "-" + runtime.GOARCH
		embedPath := filepath.Join("lib", platform, exeName)
		data, err := libFS.ReadFile(embedPath)
		if err != nil {
			continue // No embedded binary — tool won't be available.
		}

		tmp, err := os.CreateTemp("", "mental-*-"+exeName)
		if err != nil {
			continue
		}
		tmpPath := tmp.Name()

		if _, err := tmp.Write(data); err != nil {
			tmp.Close()
			os.Remove(tmpPath)
			continue
		}
		tmp.Close()

		// Make executable on Unix.
		if runtime.GOOS != "windows" {
			os.Chmod(tmpPath, 0755)
		}

		setToolPath(t.id, tmpPath)
		tempPaths = append(tempPaths, tmpPath)
	}

	if len(tempPaths) > 0 {
		Defer(func(_ *sync.WaitGroup) {
			for _, p := range tempPaths {
				os.Remove(p)
			}
		})
	}
}

// setToolPath calls mental_set_tool_path in the C library.
func setToolPath(tool int, path string) {
	cstr := append([]byte(path), 0) // null-terminated
	call2(ft.setToolPath, uintptr(tool), uintptr(unsafe.Pointer(&cstr[0])))
}
