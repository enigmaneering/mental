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
	deviceCount   uintptr
	deviceGet     uintptr
	deviceName    uintptr
	deviceAPI     uintptr
	deviceAPIName uintptr

	compile        uintptr
	dispatch       uintptr
	kernelFinalize uintptr

	viewportAttach uintptr
	viewportPres   uintptr
	viewportDet    uintptr

	getError    uintptr
	getErrorMsg uintptr
	setToolPath uintptr
	getToolPath uintptr

	mentalAtexit uintptr
	stdlink      uintptr
	stdlinkPeer  uintptr
	stdlinkSend  uintptr
	stdlinkRecv  uintptr

	count            uintptr
	counterCreate    uintptr
	counterIncrement uintptr
	counterDecrement uintptr
	counterEmpty     uintptr
	counterReset     uintptr
	counterFinalize  uintptr

	uuid                           uintptr
	referenceCreate                uintptr
	referenceOpen                  uintptr
	referenceData                  uintptr
	referenceSize                  uintptr
	referenceWritable              uintptr
	referenceGetDisclosure         uintptr
	referenceSetDisclosure         uintptr
	referenceSetCredential         uintptr
	referenceSetCredentialProvider uintptr
	referenceIsOwner               uintptr
	referenceClone                 uintptr
	referenceClose                 uintptr
	referencePin                   uintptr
	referenceWrite                 uintptr
	referenceRead                  uintptr
	referenceIsPinned              uintptr
	referenceDevice                uintptr
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
	{"mental_device_count", ptrOffset(0)},
	{"mental_device_get", ptrOffset(1)},
	{"mental_device_name", ptrOffset(2)},
	{"mental_device_api", ptrOffset(3)},
	{"mental_device_api_name", ptrOffset(4)},

	{"mental_compile", ptrOffset(5)},
	{"mental_dispatch", ptrOffset(6)},
	{"mental_kernel_finalize", ptrOffset(7)},

	{"mental_viewport_attach", ptrOffset(8)},
	{"mental_viewport_present", ptrOffset(9)},
	{"mental_viewport_detach", ptrOffset(10)},

	{"mental_get_error", ptrOffset(11)},
	{"mental_get_error_message", ptrOffset(12)},
	{"mental_set_tool_path", ptrOffset(13)},
	{"mental_get_tool_path", ptrOffset(14)},

	{"mental_atexit", ptrOffset(15)},
	{"mental_stdlink", ptrOffset(16)},
	{"mental_stdlink_peer", ptrOffset(17)},
	{"mental_stdlink_send", ptrOffset(18)},
	{"mental_stdlink_recv", ptrOffset(19)},

	{"mental_count", ptrOffset(20)},
	{"mental_counter_create", ptrOffset(21)},
	{"mental_counter_increment", ptrOffset(22)},
	{"mental_counter_decrement", ptrOffset(23)},
	{"mental_counter_empty", ptrOffset(24)},
	{"mental_counter_reset", ptrOffset(25)},
	{"mental_counter_finalize", ptrOffset(26)},

	{"mental_uuid", ptrOffset(27)},
	{"mental_reference_create", ptrOffset(28)},
	{"mental_reference_open", ptrOffset(29)},
	{"mental_reference_data", ptrOffset(30)},
	{"mental_reference_size", ptrOffset(31)},
	{"mental_reference_writable", ptrOffset(32)},
	{"mental_reference_get_disclosure", ptrOffset(33)},
	{"mental_reference_set_disclosure", ptrOffset(34)},
	{"mental_reference_set_credential", ptrOffset(35)},
	{"mental_reference_set_credential_provider", ptrOffset(36)},
	{"mental_reference_is_owner", ptrOffset(37)},
	{"mental_reference_clone", ptrOffset(38)},
	{"mental_reference_close", ptrOffset(39)},
	{"mental_reference_pin", ptrOffset(40)},
	{"mental_reference_write", ptrOffset(41)},
	{"mental_reference_read", ptrOffset(42)},
	{"mental_reference_is_pinned", ptrOffset(43)},
	{"mental_reference_device", ptrOffset(44)},
}

func ptrOffset(index int) uintptr {
	return uintptr(index) * ptrSize
}

const ptrSize = 8 // all supported platforms are 64-bit

func resolveSymbols(handle uintptr) error {
	base := (*[45]uintptr)(unsafePointer(&ft))
	for i, sym := range symbolNames {
		addr, err := lookupSymbol(handle, sym.name)
		if err != nil {
			return fmt.Errorf("mental: symbol %s: %w", sym.name, err)
		}
		base[i] = addr
	}
	return nil
}
