# Plan: Abstract Away Transpilation Library Discovery

## Goal
Users shouldn't need to know about DXC/Naga or the `external/` directory. The Go
bindings handle everything: system-installed tools by default, embedded fallbacks
if missing.

## C Layer Changes

### 1. New configuration API in `transpile.h`

```c
typedef enum {
    MENTAL_TOOL_DXC = 0,
    MENTAL_TOOL_NAGA = 1
} mental_tool;

// Tell the C library exactly where a tool lives. Must be called before compile.
void mental_set_tool_path(mental_tool tool, const char* path);

// Query current configured path (NULL if not set).
const char* mental_get_tool_path(mental_tool tool);
```

### 2. Modify `transpile_other.c`

- Add static storage: `static char g_dxc_path[4096]` and `g_naga_path[4096]`
- `mental_set_tool_path()` copies into these buffers
- `mental_get_tool_path()` returns pointer or NULL
- Modify `find_dxc()` and `find_naga()`: if `g_*_path[0] != '\0'`, return that
  immediately; otherwise fall through to existing search logic as a fallback

This is fully backward-compatible — if nobody calls `mental_set_tool_path`,
the old search behavior still works.

### 3. Export in CMakeLists.txt

The new functions are just in `transpile_other.c` which is already compiled.
No CMake changes needed.

## Go Layer Changes

### 4. Embed DXC and Naga binaries

Expand the existing `go/lib/` embed directory structure:

```
go/lib/
  linux-amd64/
    libmental.so
    dxc          # <-- new
    naga         # <-- new
  darwin-amd64/
    libmental.dylib
    dxc
    naga
  ...
```

The existing `//go:embed all:lib` in `embed.go` already captures everything
under `lib/`, so no embed directive changes needed.

### 5. Add new symbols to Go function table

In `loader.go`, add two new function pointers and symbol names:

```go
type funcTable struct {
    // ... existing 19 fields ...
    setToolPath uintptr  // mental_set_tool_path
    getToolPath uintptr  // mental_get_tool_path
}
```

Update symbol names array accordingly (now 21 entries).

### 6. New `tools.go` — tool path configuration logic

```go
// Called during Init(), after library is loaded and symbols resolved.
func configureTools() {
    configureTool(0, "dxc")  // MENTAL_TOOL_DXC
    configureTool(1, "naga") // MENTAL_TOOL_NAGA
}

func configureTool(toolID int, name string) {
    // 1. Check system PATH
    if path, err := exec.LookPath(name); err == nil {
        setToolPath(toolID, path)
        return
    }

    // 2. Extract embedded binary to cache dir
    embeddedPath := filepath.Join("lib", platform(), name)
    data, err := libFS.ReadFile(embeddedPath)
    if err != nil {
        return // No embedded binary, tool just won't be available
    }

    cacheDir := filepath.Join(os.UserCacheDir(), "mental", "tools")
    os.MkdirAll(cacheDir, 0755)
    toolPath := filepath.Join(cacheDir, name)

    // Write if missing or different size (simple staleness check)
    if info, err := os.Stat(toolPath); err != nil || info.Size() != int64(len(data)) {
        os.WriteFile(toolPath, data, 0755)
    }

    setToolPath(toolID, toolPath)
}
```

### 7. Wire into Init()

In `loader.go`, after `resolveSymbols()` succeeds, call `configureTools()`.

## What This Achieves

- **Zero config for users**: `go get` + use. Tools are embedded.
- **System preference**: System-installed DXC/Naga take priority (known versions).
- **Graceful degradation**: If neither system nor embedded exist, HLSL/WGSL
  compilation returns a clear error — no crash.
- **Power users**: Can call `mental_set_tool_path` from C directly, or install
  system tools to override embedded ones.
- **Backward compatible**: Existing C users who don't call the new API get the
  same `find_*` behavior they have today.
- **No `external/` directory imposed on users**.
