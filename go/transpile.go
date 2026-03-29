package mental

/*
#include "mental.h"
#include "transpile.h"
#include <stdlib.h>
*/
import "C"
import "unsafe"

// Language identifies a shader language.
type Language int32

const (
	LangUnknown Language = 0
	LangGLSL    Language = 1
	LangHLSL    Language = 2
	LangMSL     Language = 3
	LangWGSL    Language = 4
	LangSPIRV   Language = 5
)

// String returns the human-readable name for a Language.
func (l Language) String() string {
	switch l {
	case LangGLSL:
		return "GLSL"
	case LangHLSL:
		return "HLSL"
	case LangMSL:
		return "MSL"
	case LangWGSL:
		return "WGSL"
	case LangSPIRV:
		return "SPIR-V"
	default:
		return "Unknown"
	}
}

// DetectLanguage auto-detects the shader language from source code or binary.
func DetectLanguage(source []byte) Language {
	if len(source) == 0 {
		return LangUnknown
	}
	return Language(C.mental_detect_language(
		(*C.char)(unsafe.Pointer(&source[0])),
		C.size_t(len(source)),
	))
}

// APIToLanguage returns the native shader language for the given backend API.
func APIToLanguage(api APIType) Language {
	return Language(C.mental_api_to_language(C.mental_api_type(api)))
}

// GLSLToSPIRV compiles GLSL source to SPIR-V binary.
func GLSLToSPIRV(source string) ([]byte, error) {
	ensureTools()
	csrc := C.CString(source)
	defer C.free(unsafe.Pointer(csrc))
	var outLen C.size_t
	var errBuf [1024]C.char
	result := C.mental_glsl_to_spirv(csrc, C.size_t(len(source)), &outLen, &errBuf[0], 1024)
	if result == nil {
		msg := C.GoString(&errBuf[0])
		if msg == "" {
			msg = "compilation failed"
		}
		return nil, &libError{code: ErrCompilationFailed, msg: msg}
	}
	spirv := C.GoBytes(unsafe.Pointer(result), C.int(outLen))
	C.mental_transpile_free((*C.char)(unsafe.Pointer(result)))
	return spirv, nil
}

// HLSLToSPIRV compiles HLSL source to SPIR-V binary via DXC.
// Requires DXC to be configured (auto-detected or via SetToolPath).
func HLSLToSPIRV(source string) ([]byte, error) {
	ensureTools()
	csrc := C.CString(source)
	defer C.free(unsafe.Pointer(csrc))
	var outLen C.size_t
	var errBuf [1024]C.char
	result := C.mental_hlsl_to_spirv(csrc, C.size_t(len(source)), &outLen, &errBuf[0], 1024)
	if result == nil {
		msg := C.GoString(&errBuf[0])
		if msg == "" {
			msg = "compilation failed"
		}
		return nil, &libError{code: ErrCompilationFailed, msg: msg}
	}
	spirv := C.GoBytes(unsafe.Pointer(result), C.int(outLen))
	C.mental_transpile_free((*C.char)(unsafe.Pointer(result)))
	return spirv, nil
}

// WGSLToSPIRV compiles WGSL source to SPIR-V binary via Naga.
// Requires Naga to be configured (auto-detected or via SetToolPath).
func WGSLToSPIRV(source string) ([]byte, error) {
	ensureTools()
	csrc := C.CString(source)
	defer C.free(unsafe.Pointer(csrc))
	var outLen C.size_t
	var errBuf [1024]C.char
	result := C.mental_wgsl_to_spirv(csrc, C.size_t(len(source)), &outLen, &errBuf[0], 1024)
	if result == nil {
		msg := C.GoString(&errBuf[0])
		if msg == "" {
			msg = "compilation failed"
		}
		return nil, &libError{code: ErrCompilationFailed, msg: msg}
	}
	spirv := C.GoBytes(unsafe.Pointer(result), C.int(outLen))
	C.mental_transpile_free((*C.char)(unsafe.Pointer(result)))
	return spirv, nil
}

// SPIRVToGLSL transpiles SPIR-V binary to GLSL source.
func SPIRVToGLSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(spirv, func(src *C.uchar, slen C.size_t, outLen *C.size_t, errBuf *C.char, errLen C.size_t) *C.char {
		return C.mental_spirv_to_glsl(src, slen, outLen, errBuf, errLen)
	})
}

// SPIRVToHLSL transpiles SPIR-V binary to HLSL source.
func SPIRVToHLSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(spirv, func(src *C.uchar, slen C.size_t, outLen *C.size_t, errBuf *C.char, errLen C.size_t) *C.char {
		return C.mental_spirv_to_hlsl(src, slen, outLen, errBuf, errLen)
	})
}

// SPIRVToMSL transpiles SPIR-V binary to Metal Shading Language source.
func SPIRVToMSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(spirv, func(src *C.uchar, slen C.size_t, outLen *C.size_t, errBuf *C.char, errLen C.size_t) *C.char {
		return C.mental_spirv_to_msl(src, slen, outLen, errBuf, errLen)
	})
}

// SPIRVToWGSL transpiles SPIR-V binary to WGSL source via Naga.
// Requires Naga to be configured.
func SPIRVToWGSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(spirv, func(src *C.uchar, slen C.size_t, outLen *C.size_t, errBuf *C.char, errLen C.size_t) *C.char {
		return C.mental_spirv_to_wgsl(src, slen, outLen, errBuf, errLen)
	})
}

// transpileFromSPIRV is the shared implementation for SPIRVTo* functions.
func transpileFromSPIRV(spirv []byte, fn func(*C.uchar, C.size_t, *C.size_t, *C.char, C.size_t) *C.char) (string, error) {
	if len(spirv) == 0 {
		return "", &libError{code: ErrCompilationFailed, msg: "empty SPIR-V input"}
	}

	var outLen C.size_t
	var errBuf [1024]C.char

	result := fn(
		(*C.uchar)(unsafe.Pointer(&spirv[0])),
		C.size_t(len(spirv)),
		&outLen,
		&errBuf[0],
		1024,
	)

	if result == nil {
		msg := C.GoString(&errBuf[0])
		if msg == "" {
			msg = "transpilation failed"
		}
		return "", &libError{code: ErrCompilationFailed, msg: msg}
	}

	output := C.GoStringN(result, C.int(outLen))
	C.mental_transpile_free(result)

	return output, nil
}

// SetToolPath configures the path to an external tool (DXC or Naga).
// Use toolDXC (0) or toolNaga (1) as the tool ID.
func SetToolPath(tool int, path string) {
	setToolPath(tool, path)
}

// GetToolPath returns the configured path for an external tool.
// Returns empty string if not configured.
func GetToolPath(tool int) string {
	ensureTools()
	p := C.mental_get_tool_path(C.mental_tool(tool))
	if p == nil {
		return ""
	}
	return C.GoString(p)
}
