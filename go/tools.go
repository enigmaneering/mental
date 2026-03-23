package mental

import (
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
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
	// libs lists companion shared libraries that must be co-located with the
	// executable for it to work. Glob patterns are matched within the
	// platform's embed directory (e.g. "libdxcompiler.*").
	libs []string
}

var tools = []toolSpec{
	{toolDXC, "dxc", []string{"libdxcompiler.*", "dxcompiler.dll", "dxil.dll"}},
	{toolNaga, "naga", nil},
}

// configureTools is called once during init after symbols are resolved.
// For each tool it checks: system PATH first, then extracts the embedded
// binary (and any companion shared libraries) to a temporary directory as
// a fallback. Extracted temp dirs are automatically cleaned up at process
// exit via Defer.
func configureTools() {
	var tempDirs []string

	platform := runtime.GOOS + "-" + runtime.GOARCH

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

		// 2. Extract embedded binary to a temporary directory.
		embedDir := filepath.Join("lib", platform)
		embedPath := filepath.Join(embedDir, exeName)
		exeData, err := libFS.ReadFile(embedPath)
		if err != nil {
			continue // No embedded binary — tool won't be available.
		}

		// Create a temp directory to hold the executable and any companion libs.
		tmpDir, err := os.MkdirTemp("", "mental-"+t.name+"-*")
		if err != nil {
			continue
		}

		// DXC on Linux/Darwin expects libs at ../lib/ relative to the binary
		// (rpath is @executable_path/../lib on macOS, $ORIGIN/../lib on Linux).
		// Place executable in bin/ and libs in lib/ to match.
		binDir := filepath.Join(tmpDir, "bin")
		libDir := filepath.Join(tmpDir, "lib")
		if err := os.MkdirAll(binDir, 0755); err != nil {
			os.RemoveAll(tmpDir)
			continue
		}

		exePath := filepath.Join(binDir, exeName)
		if err := os.WriteFile(exePath, exeData, 0755); err != nil {
			os.RemoveAll(tmpDir)
			continue
		}

		// Extract companion shared libraries.
		if len(t.libs) > 0 {
			libsExtracted := extractCompanionLibs(embedDir, t.libs, libDir)
			if libsExtracted {
				if err := os.MkdirAll(libDir, 0755); err != nil {
					// libDir already created by extractCompanionLibs
				}
			}
		}

		setToolPath(t.id, exePath)
		tempDirs = append(tempDirs, tmpDir)
	}

	if len(tempDirs) > 0 {
		Defer(func(_ *sync.WaitGroup) {
			for _, d := range tempDirs {
				os.RemoveAll(d)
			}
		})
	}
}

// extractCompanionLibs extracts embedded files matching the given glob patterns
// from embedDir into destDir. Returns true if any files were extracted.
func extractCompanionLibs(embedDir string, patterns []string, destDir string) bool {
	entries, err := libFS.ReadDir(embedDir)
	if err != nil {
		return false
	}

	extracted := false
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		for _, pattern := range patterns {
			matched, _ := filepath.Match(pattern, name)
			if !matched {
				// Also match versioned sonames like libdxcompiler.so.3.7
				if strings.Contains(name, ".so.") {
					baseName := name[:strings.Index(name, ".so.")+3]
					matched, _ = filepath.Match(pattern, baseName)
				}
			}
			if matched {
				data, err := fs.ReadFile(libFS, filepath.Join(embedDir, name))
				if err != nil {
					continue
				}
				os.MkdirAll(destDir, 0755)
				os.WriteFile(filepath.Join(destDir, name), data, 0755)
				extracted = true
				break
			}
		}
	}
	return extracted
}

// setToolPath calls mental_set_tool_path in the C library.
func setToolPath(tool int, path string) {
	cstr := append([]byte(path), 0) // null-terminated
	call2(ft.setToolPath, uintptr(tool), uintptr(unsafe.Pointer(&cstr[0])))
}
