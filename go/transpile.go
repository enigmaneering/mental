package mental

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
	return Language(call2(ft.detectLanguage, uintptr(unsafe.Pointer(&source[0])), uintptr(len(source))))
}

// APIToLanguage returns the native shader language for the given backend API.
func APIToLanguage(api APIType) Language {
	return Language(call1(ft.apiToLanguage, uintptr(api)))
}

// GLSLToSPIRV compiles GLSL source to SPIR-V binary.
func GLSLToSPIRV(source string) ([]byte, error) {
	return compileToSPIRV(ft.glslToSpirv, source)
}

// HLSLToSPIRV compiles HLSL source to SPIR-V binary via DXC.
// Requires DXC to be configured (auto-detected or via SetToolPath).
func HLSLToSPIRV(source string) ([]byte, error) {
	return compileToSPIRV(ft.hlslToSpirv, source)
}

// WGSLToSPIRV compiles WGSL source to SPIR-V binary via Naga.
// Requires Naga to be configured (auto-detected or via SetToolPath).
func WGSLToSPIRV(source string) ([]byte, error) {
	return compileToSPIRV(ft.wgslToSpirv, source)
}

// compileToSPIRV is the shared implementation for *ToSPIRV functions.
// C signature: unsigned char* fn(const char* source, size_t source_len,
//
//	size_t* out_len, char* error, size_t error_len)
func compileToSPIRV(fn uintptr, source string) ([]byte, error) {
	src := unsafe.StringData(source)
	var outLen uintptr
	var errBuf [1024]byte

	result := call5(fn,
		uintptr(unsafe.Pointer(src)),
		uintptr(len(source)),
		uintptr(unsafe.Pointer(&outLen)),
		uintptr(unsafe.Pointer(&errBuf[0])),
		uintptr(len(errBuf)))

	if result == 0 {
		msg := goStringFromPtr(uintptr(unsafe.Pointer(&errBuf[0])))
		if msg == "" {
			msg = "compilation failed"
		}
		return nil, &libError{code: ErrCompilationFailed, msg: msg}
	}

	spirv := make([]byte, outLen)
	copy(spirv, unsafe.Slice((*byte)(unsafe.Pointer(result)), outLen))

	// Free the C-allocated result.
	call1(ft.transpileFree, result)

	return spirv, nil
}

// SPIRVToGLSL transpiles SPIR-V binary to GLSL source.
func SPIRVToGLSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(ft.spirvToGlsl, spirv)
}

// SPIRVToHLSL transpiles SPIR-V binary to HLSL source.
func SPIRVToHLSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(ft.spirvToHlsl, spirv)
}

// SPIRVToMSL transpiles SPIR-V binary to Metal Shading Language source.
func SPIRVToMSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(ft.spirvToMsl, spirv)
}

// SPIRVToWGSL transpiles SPIR-V binary to WGSL source via Naga.
// Requires Naga to be configured.
func SPIRVToWGSL(spirv []byte) (string, error) {
	return transpileFromSPIRV(ft.spirvToWgsl, spirv)
}

// transpileFromSPIRV is the shared implementation for SPIRVTo* functions.
// C signature: char* fn(const unsigned char* spirv, size_t spirv_len,
//
//	size_t* out_len, char* error, size_t error_len)
func transpileFromSPIRV(fn uintptr, spirv []byte) (string, error) {
	if len(spirv) == 0 {
		return "", &libError{code: ErrCompilationFailed, msg: "empty SPIR-V input"}
	}

	var outLen uintptr
	var errBuf [1024]byte

	result := call5(fn,
		uintptr(unsafe.Pointer(&spirv[0])),
		uintptr(len(spirv)),
		uintptr(unsafe.Pointer(&outLen)),
		uintptr(unsafe.Pointer(&errBuf[0])),
		uintptr(len(errBuf)))

	if result == 0 {
		msg := goStringFromPtr(uintptr(unsafe.Pointer(&errBuf[0])))
		if msg == "" {
			msg = "transpilation failed"
		}
		return "", &libError{code: ErrCompilationFailed, msg: msg}
	}

	output := string(unsafe.Slice((*byte)(unsafe.Pointer(result)), outLen))

	// Free the C-allocated result.
	call1(ft.transpileFree, result)

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
	p := call1(ft.getToolPath, uintptr(tool))
	if p == 0 {
		return ""
	}
	return goStringFromPtr(p)
}
