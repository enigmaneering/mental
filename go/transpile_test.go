package mental

import (
	"bytes"
	"strings"
	"testing"
)

func TestDetectLanguageGLSL(t *testing.T) {
	skipIfNoLibrary(t)
	src := []byte("#version 450\nvoid main() {}\n")
	lang := DetectLanguage(src)
	if lang != LangGLSL {
		t.Errorf("DetectLanguage(GLSL) = %v, want GLSL", lang)
	}
}

func TestDetectLanguageHLSL(t *testing.T) {
	skipIfNoLibrary(t)
	src := []byte("RWStructuredBuffer<float> buf : register(u0);\n[numthreads(256,1,1)]\nvoid main(uint3 id : SV_DispatchThreadID) {}\n")
	lang := DetectLanguage(src)
	if lang != LangHLSL {
		t.Errorf("DetectLanguage(HLSL) = %v, want HLSL", lang)
	}
}

func TestDetectLanguageMSL(t *testing.T) {
	skipIfNoLibrary(t)
	src := []byte("kernel void main0(device float* buf [[buffer(0)]]) {}\n")
	lang := DetectLanguage(src)
	if lang != LangMSL {
		t.Errorf("DetectLanguage(MSL) = %v, want MSL", lang)
	}
}

func TestDetectLanguageWGSL(t *testing.T) {
	skipIfNoLibrary(t)
	src := []byte("@compute @workgroup_size(256)\nfn main(@builtin(global_invocation_id) id: vec3<u32>) {}\n")
	lang := DetectLanguage(src)
	if lang != LangWGSL {
		t.Errorf("DetectLanguage(WGSL) = %v, want WGSL", lang)
	}
}

func TestDetectLanguageSPIRV(t *testing.T) {
	skipIfNoLibrary(t)
	// SPIR-V magic number: 0x07230203 (little-endian)
	src := []byte{0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x00, 0x00}
	lang := DetectLanguage(src)
	if lang != LangSPIRV {
		t.Errorf("DetectLanguage(SPIRV) = %v, want SPIRV", lang)
	}
}

func TestDetectLanguageEmpty(t *testing.T) {
	skipIfNoLibrary(t)
	lang := DetectLanguage(nil)
	if lang != LangUnknown {
		t.Errorf("DetectLanguage(nil) = %v, want Unknown", lang)
	}
}

func TestAPIToLanguage(t *testing.T) {
	skipIfNoLibrary(t)
	tests := []struct {
		api  APIType
		want Language
	}{
		{APIMetal, LangMSL},
		{APID3D12, LangHLSL},
		{APIVulkan, LangSPIRV},
		{APIOpenCL, LangSPIRV},
		{APIOpenGL, LangGLSL},
		{APIPoCL, LangSPIRV},
	}
	for _, tt := range tests {
		got := APIToLanguage(tt.api)
		if got != tt.want {
			t.Errorf("APIToLanguage(%d) = %v, want %v", tt.api, got, tt.want)
		}
	}
}

func TestGLSLToSPIRV(t *testing.T) {
	skipIfNoLibrary(t)
	glsl := `#version 450
layout(local_size_x = 256) in;
layout(binding = 0) buffer Buf { float data[]; } buf;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    buf.data[idx] = buf.data[idx] * 2.0;
}`

	spirv, err := GLSLToSPIRV(glsl)
	if err != nil {
		t.Fatalf("GLSLToSPIRV: %v", err)
	}
	if len(spirv) < 4 {
		t.Fatalf("SPIR-V output too short: %d bytes", len(spirv))
	}
	// Verify SPIR-V magic number
	if !bytes.Equal(spirv[:4], []byte{0x03, 0x02, 0x23, 0x07}) {
		t.Errorf("SPIR-V magic mismatch: got %x", spirv[:4])
	}
	t.Logf("GLSL -> SPIR-V: %d bytes", len(spirv))
}

func TestSPIRVToGLSL(t *testing.T) {
	skipIfNoLibrary(t)
	// First compile GLSL to SPIR-V, then round-trip back.
	glsl := `#version 450
layout(local_size_x = 256) in;
layout(binding = 0) buffer Buf { float data[]; } buf;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    buf.data[idx] = buf.data[idx] * 2.0;
}`
	spirv, err := GLSLToSPIRV(glsl)
	if err != nil {
		t.Fatalf("GLSLToSPIRV: %v", err)
	}

	result, err := SPIRVToGLSL(spirv)
	if err != nil {
		t.Fatalf("SPIRVToGLSL: %v", err)
	}
	if !strings.Contains(result, "#version") {
		t.Errorf("output doesn't contain #version: %s", result[:min(200, len(result))])
	}
	t.Logf("SPIR-V -> GLSL: %d chars", len(result))
}

func TestSPIRVToHLSL(t *testing.T) {
	skipIfNoLibrary(t)
	glsl := `#version 450
layout(local_size_x = 256) in;
layout(binding = 0) buffer Buf { float data[]; } buf;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    buf.data[idx] = buf.data[idx] * 2.0;
}`
	spirv, err := GLSLToSPIRV(glsl)
	if err != nil {
		t.Fatalf("GLSLToSPIRV: %v", err)
	}

	result, err := SPIRVToHLSL(spirv)
	if err != nil {
		t.Fatalf("SPIRVToHLSL: %v", err)
	}
	if !strings.Contains(result, "Buffer") && !strings.Contains(result, "RW") {
		t.Errorf("output doesn't look like HLSL: %s", result[:min(200, len(result))])
	}
	t.Logf("SPIR-V -> HLSL: %d chars", len(result))
}

func TestSPIRVToMSL(t *testing.T) {
	skipIfNoLibrary(t)
	glsl := `#version 450
layout(local_size_x = 256) in;
layout(binding = 0) buffer Buf { float data[]; } buf;
void main() {
    uint idx = gl_GlobalInvocationID.x;
    buf.data[idx] = buf.data[idx] * 2.0;
}`
	spirv, err := GLSLToSPIRV(glsl)
	if err != nil {
		t.Fatalf("GLSLToSPIRV: %v", err)
	}

	result, err := SPIRVToMSL(spirv)
	if err != nil {
		t.Fatalf("SPIRVToMSL: %v", err)
	}
	if !strings.Contains(result, "metal") && !strings.Contains(result, "kernel") {
		t.Errorf("output doesn't look like MSL: %s", result[:min(200, len(result))])
	}
	t.Logf("SPIR-V -> MSL: %d chars", len(result))
}

func TestLanguageString(t *testing.T) {
	tests := []struct {
		lang Language
		want string
	}{
		{LangGLSL, "GLSL"},
		{LangHLSL, "HLSL"},
		{LangMSL, "MSL"},
		{LangWGSL, "WGSL"},
		{LangSPIRV, "SPIR-V"},
		{LangUnknown, "Unknown"},
		{Language(99), "Unknown"},
	}
	for _, tt := range tests {
		got := tt.lang.String()
		if got != tt.want {
			t.Errorf("Language(%d).String() = %q, want %q", tt.lang, got, tt.want)
		}
	}
}

func TestInvalidGLSLToSPIRV(t *testing.T) {
	skipIfNoLibrary(t)
	_, err := GLSLToSPIRV("this is not valid GLSL!!!")
	if err == nil {
		t.Fatal("expected error for invalid GLSL")
	}
	t.Logf("invalid GLSL error: %v", err)
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
