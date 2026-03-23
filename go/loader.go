package mental

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
)

// funcTable holds resolved C function pointers for the mental library.
type funcTable struct {
	deviceCount      uintptr
	deviceGet        uintptr
	deviceName       uintptr
	deviceAPI        uintptr
	deviceAPIName    uintptr
	alloc            uintptr
	write            uintptr
	read             uintptr
	size             uintptr
	clone            uintptr
	finalize         uintptr
	compile          uintptr
	dispatch         uintptr
	kernelFinalize   uintptr
	viewportAttach   uintptr
	viewportPres     uintptr
	viewportDet      uintptr
	getError         uintptr
	getErrorMsg      uintptr
	setToolPath      uintptr
	getToolPath      uintptr
	mentalAtexit     uintptr
	stdlink           uintptr
	stdlinkPeer       uintptr
	stdlinkSend       uintptr
	stdlinkRecv       uintptr
}

var (
	ft   funcTable
	libH uintptr
)

// libName returns the platform-specific shared library filename.
func libName() string {
	switch runtime.GOOS {
	case "darwin":
		return "libmental.dylib"
	case "windows":
		return "mental.dll"
	default:
		return "libmental.so"
	}
}

// init loads the mental shared library, resolves symbols, configures tools,
// and sets up automatic cleanup via the C library's atexit mechanism.
//
// Search order for the shared library:
//  1. System library paths (standard dlopen / LoadLibrary search)
//  2. "external/" directory relative to the current working directory
//  3. Embedded library extracted to a temporary file
func init() {
	if err := doInit(); err != nil {
		panic(err)
	}
}

func doInit() error {
	name := libName()

	// Try sources in priority order: system, external/, embedded.
	h, err := openLibrary(name)
	if err != nil {
		if dir, e := os.Getwd(); e == nil {
			h, err = openLibrary(filepath.Join(dir, "external", name))
		}
	}
	if err != nil {
		h, err = loadFromEmbed()
		if err != nil {
			return fmt.Errorf("mental: unable to load %s: system, external/, and embed all failed: %w", name, err)
		}
	}

	libH = h
	if err := resolveSymbols(h); err != nil {
		return err
	}
	configureTools()
	if embedTmpPath != "" {
		path := embedTmpPath
		Defer(func(_ *sync.WaitGroup) {
			os.Remove(path)
		})
	}
	setupLifecycle()
	return nil
}

// symbol names in the order they appear in funcTable.
var symbolNames = [...]struct {
	name   string
	offset uintptr
}{
	{"mental_device_count", offsetOf_deviceCount},
	{"mental_device_get", offsetOf_deviceGet},
	{"mental_device_name", offsetOf_deviceName},
	{"mental_device_api", offsetOf_deviceAPI},
	{"mental_device_api_name", offsetOf_deviceAPIName},
	{"mental_alloc", offsetOf_alloc},
	{"mental_write", offsetOf_write},
	{"mental_read", offsetOf_read},
	{"mental_size", offsetOf_size},
	{"mental_clone", offsetOf_clone},
	{"mental_finalize", offsetOf_finalize},
	{"mental_compile", offsetOf_compile},
	{"mental_dispatch", offsetOf_dispatch},
	{"mental_kernel_finalize", offsetOf_kernelFinalize},
	{"mental_viewport_attach", offsetOf_viewportAttach},
	{"mental_viewport_present", offsetOf_viewportPres},
	{"mental_viewport_detach", offsetOf_viewportDet},
	{"mental_get_error", offsetOf_getError},
	{"mental_get_error_message", offsetOf_getErrorMsg},
	{"mental_set_tool_path", offsetOf_setToolPath},
	{"mental_get_tool_path", offsetOf_getToolPath},
	{"mental_atexit", offsetOf_mentalAtexit},
	{"mental_stdlink", offsetOf_stdlink},
	{"mental_stdlink_peer", offsetOf_stdlinkPeer},
	{"mental_stdlink_send", offsetOf_stdlinkSend},
	{"mental_stdlink_recv", offsetOf_stdlinkRecv},
}

// Field offsets computed via unsafe.Offsetof — kept in a single place
// so the symbol table and struct stay in sync.
var (
	offsetOf_deviceCount      = ptrOffset(0)
	offsetOf_deviceGet        = ptrOffset(1)
	offsetOf_deviceName       = ptrOffset(2)
	offsetOf_deviceAPI        = ptrOffset(3)
	offsetOf_deviceAPIName    = ptrOffset(4)
	offsetOf_alloc            = ptrOffset(5)
	offsetOf_write            = ptrOffset(6)
	offsetOf_read             = ptrOffset(7)
	offsetOf_size             = ptrOffset(8)
	offsetOf_clone            = ptrOffset(9)
	offsetOf_finalize         = ptrOffset(10)
	offsetOf_compile          = ptrOffset(11)
	offsetOf_dispatch         = ptrOffset(12)
	offsetOf_kernelFinalize   = ptrOffset(13)
	offsetOf_viewportAttach   = ptrOffset(14)
	offsetOf_viewportPres     = ptrOffset(15)
	offsetOf_viewportDet      = ptrOffset(16)
	offsetOf_getError         = ptrOffset(17)
	offsetOf_getErrorMsg      = ptrOffset(18)
	offsetOf_setToolPath      = ptrOffset(19)
	offsetOf_getToolPath      = ptrOffset(20)
	offsetOf_mentalAtexit = ptrOffset(21)
	offsetOf_stdlink       = ptrOffset(22)
	offsetOf_stdlinkPeer   = ptrOffset(23)
	offsetOf_stdlinkSend   = ptrOffset(24)
	offsetOf_stdlinkRecv   = ptrOffset(25)
)

func ptrOffset(index int) uintptr {
	return uintptr(index) * ptrSize
}

const ptrSize = 8 // all supported platforms are 64-bit

func resolveSymbols(handle uintptr) error {
	base := (*[26]uintptr)(unsafePointer(&ft))
	for i, sym := range symbolNames {
		addr, err := lookupSymbol(handle, sym.name)
		if err != nil {
			return fmt.Errorf("mental: symbol %s: %w", sym.name, err)
		}
		base[i] = addr
	}
	return nil
}
