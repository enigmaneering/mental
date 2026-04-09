/*
 * Exhaustive transpilation tests.
 *
 * Pure-logic sections (detection, mapping, tool paths) run on every platform.
 * Source-to-SPIRV and SPIRV-to-target sections use the embedded glslang /
 * spirv-cross libraries and also run everywhere.
 * Full pipeline execution tests discover the device at runtime and only
 * exercise paths that make sense for the current backend.
 *
 * Platform matrix (CI targets):
 *   darwin  / amd64, arm64  -> Metal  (+ optional OpenCL)
 *   linux   / amd64, arm64  -> Vulkan (+ optional OpenCL)
 *   windows / amd64, arm64  -> D3D12  (+ optional OpenCL)
 */

#include "../mental.h"
#include "../mental_internal.h"   /* mental_transpile */
#include "../transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <limits.h>
#else
#include <io.h>
#include <stdlib.h>
#define access _access
#define F_OK 0
#endif

/* Resolve a relative path to absolute (needed for popen on Windows) */
static const char* resolve_path(const char *rel, char *buf, size_t buf_len) {
#ifdef _WIN32
    if (_fullpath(buf, rel, buf_len)) return buf;
#else
    if (realpath(rel, buf)) return buf;
#endif
    return rel; /* fallback to original */
}

/*
 * Auto-detect DXC and Naga tool paths.
 *
 * Checks (in order):
 *   1. MENTAL_DXC_PATH / MENTAL_NAGA_PATH environment variables
 *   2. Common relative paths from the build directory
 *   3. Leaves unconfigured if not found (tests that need them will skip/fail)
 */
static void auto_detect_tools(void) {
    char resolved[4096];

    /* DXC */
    if (!mental_get_tool_path(MENTAL_TOOL_DXC)) {
        const char *env = getenv("MENTAL_DXC_PATH");
        if (env && access(env, F_OK) == 0) {
            mental_set_tool_path(MENTAL_TOOL_DXC, resolve_path(env, resolved, sizeof(resolved)));
        } else {
            static const char *dxc_candidates[] = {
                "../external/dxc/bin/dxc",
                "../../external/dxc/bin/dxc",
                "../external/dxc/bin/dxc.exe",
                "../../external/dxc/bin/dxc.exe",
                "../external/dxc/dxc",
                "../../external/dxc/dxc",
                "../external/dxc/dxc.exe",
                "../../external/dxc/dxc.exe",
                NULL
            };
            for (int i = 0; dxc_candidates[i]; i++) {
                if (access(dxc_candidates[i], F_OK) == 0) {
                    mental_set_tool_path(MENTAL_TOOL_DXC, resolve_path(dxc_candidates[i], resolved, sizeof(resolved)));
                    break;
                }
            }
        }
    }

    /* Naga */
    if (!mental_get_tool_path(MENTAL_TOOL_NAGA)) {
        const char *env = getenv("MENTAL_NAGA_PATH");
        if (env && access(env, F_OK) == 0) {
            mental_set_tool_path(MENTAL_TOOL_NAGA, resolve_path(env, resolved, sizeof(resolved)));
        } else {
            static const char *naga_candidates[] = {
                "../external/naga/bin/naga",
                "../../external/naga/bin/naga",
                "../external/naga/bin/naga.exe",
                "../../external/naga/bin/naga.exe",
                NULL
            };
            for (int i = 0; naga_candidates[i]; i++) {
                if (access(naga_candidates[i], F_OK) == 0) {
                    mental_set_tool_path(MENTAL_TOOL_NAGA, resolve_path(naga_candidates[i], resolved, sizeof(resolved)));
                    break;
                }
            }
        }
    }

    const char *dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    const char *naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    if (dxc) printf("  DXC: %s\n", dxc);
    else printf("  DXC: not found (HLSL tests will be skipped)\n");
    if (naga) printf("  Naga: %s\n", naga);
    else printf("  Naga: not found (WGSL tests will be skipped)\n");
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_fail++; \
        return 1; \
    } \
} while (0)

#define ASSERT_NO_ERROR() do { \
    if (mental_get_error() != MENTAL_SUCCESS) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", mental_get_error_message(), __LINE__); \
        g_fail++; \
        return 1; \
    } \
} while (0)

#define PASS(name) do { printf("  PASS: %s\n", name); g_pass++; return 0; } while (0)
#define SKIP(name, reason) do { printf("  SKIP: %s (%s)\n", name, reason); g_skip++; return 0; } while (0)

/* ------------------------------------------------------------------ */
/*  Shader sources used across many tests                             */
/* ------------------------------------------------------------------ */

static const char* glsl_shader =
    "#version 450\n"
    "layout(local_size_x = 256) in;\n"
    "layout(binding = 0) buffer Input0 { float data[]; } input0;\n"
    "layout(binding = 1) buffer Input1 { float data[]; } input1;\n"
    "layout(binding = 2) buffer Output { float data[]; } output_buf;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    output_buf.data[idx] = input0.data[idx] + input1.data[idx];\n"
    "}\n";

static const char* hlsl_shader =
    "RWStructuredBuffer<float> input0 : register(u0);\n"
    "RWStructuredBuffer<float> input1 : register(u1);\n"
    "RWStructuredBuffer<float> output_buf : register(u2);\n"
    "\n"
    "[numthreads(256, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    output_buf[id.x] = input0[id.x] + input1[id.x];\n"
    "}\n";

static const char* msl_shader =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "kernel void main0(\n"
    "    device float* input0 [[buffer(0)]],\n"
    "    device float* input1 [[buffer(1)]],\n"
    "    device float* output [[buffer(2)]],\n"
    "    uint id [[thread_position_in_grid]])\n"
    "{\n"
    "    output[id] = input0[id] + input1[id];\n"
    "}\n";

static const char* wgsl_shader =
    "@group(0) @binding(0) var<storage, read> input0: array<f32>;\n"
    "@group(0) @binding(1) var<storage, read> input1: array<f32>;\n"
    "@group(0) @binding(2) var<storage, read_write> output: array<f32>;\n"
    "\n"
    "@compute @workgroup_size(256)\n"
    "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "    output[id.x] = input0[id.x] + input1[id.x];\n"
    "}\n";

/* Minimal SPIR-V magic number (little-endian) */
static const unsigned char spirv_magic[4] = { 0x03, 0x02, 0x23, 0x07 };

/* ================================================================== */
/*  1. Language Detection                                             */
/* ================================================================== */

static int test_detect_glsl_version(void) {
    mental_language lang = mental_detect_language(glsl_shader, strlen(glsl_shader));
    ASSERT(lang == MENTAL_LANG_GLSL, "GLSL shader detected as GLSL");
    PASS("detect_glsl_version");
}

static int test_detect_glsl_layout(void) {
    const char* src = "layout(local_size_x = 64) in;\nvoid main() {}\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_GLSL, "layout( triggers GLSL");
    PASS("detect_glsl_layout");
}

static int test_detect_glsl_builtin(void) {
    const char* src = "void main() { uint i = gl_GlobalInvocationID.x; }\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_GLSL, "gl_ triggers GLSL");
    PASS("detect_glsl_builtin");
}

static int test_detect_hlsl_numthreads(void) {
    ASSERT(mental_detect_language(hlsl_shader, strlen(hlsl_shader)) == MENTAL_LANG_HLSL,
           "HLSL shader detected as HLSL");
    PASS("detect_hlsl_numthreads");
}

static int test_detect_hlsl_rwbuffer(void) {
    const char* src = "RWStructuredBuffer<int> buf : register(u0);\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_HLSL, "RWStructuredBuffer triggers HLSL");
    PASS("detect_hlsl_rwbuffer");
}

static int test_detect_hlsl_cbuffer(void) {
    const char* src = "cbuffer Constants : register(b0) { float4 val; };\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_HLSL, "cbuffer triggers HLSL");
    PASS("detect_hlsl_cbuffer");
}

static int test_detect_msl_kernel_void(void) {
    ASSERT(mental_detect_language(msl_shader, strlen(msl_shader)) == MENTAL_LANG_MSL,
           "MSL shader detected as MSL");
    PASS("detect_msl_kernel_void");
}

static int test_detect_msl_kernel_attr(void) {
    const char* src = "[[kernel]] void foo(device float* a [[buffer(0)]]) {}\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_MSL, "[[kernel]] triggers MSL");
    PASS("detect_msl_kernel_attr");
}

static int test_detect_msl_threadgroup(void) {
    const char* src = "threadgroup float shared[64];\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_MSL, "threadgroup triggers MSL");
    PASS("detect_msl_threadgroup");
}

static int test_detect_wgsl_compute(void) {
    ASSERT(mental_detect_language(wgsl_shader, strlen(wgsl_shader)) == MENTAL_LANG_WGSL,
           "WGSL shader detected as WGSL");
    PASS("detect_wgsl_compute");
}

static int test_detect_wgsl_vertex(void) {
    const char* src = "@vertex\nfn vs_main() -> @builtin(position) vec4<f32> { return vec4(0.0); }\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_WGSL, "@vertex triggers WGSL");
    PASS("detect_wgsl_vertex");
}

static int test_detect_wgsl_fn(void) {
    const char* src = "fn helper(x: f32) -> f32 { return x * 2.0; }\n";
    ASSERT(mental_detect_language(src, strlen(src)) == MENTAL_LANG_WGSL, "fn triggers WGSL");
    PASS("detect_wgsl_fn");
}

static int test_detect_spirv_magic(void) {
    ASSERT(mental_detect_language((const char*)spirv_magic, 4) == MENTAL_LANG_SPIRV,
           "SPIR-V magic number detected");
    PASS("detect_spirv_magic");
}

static int test_detect_null_input(void) {
    ASSERT(mental_detect_language(NULL, 0) == MENTAL_LANG_UNKNOWN, "NULL returns UNKNOWN");
    ASSERT(mental_detect_language("", 0) == MENTAL_LANG_UNKNOWN, "empty returns UNKNOWN");
    PASS("detect_null_input");
}

static int test_detect_ambiguous_defaults_glsl(void) {
    const char* src = "void main() { }";
    mental_language lang = mental_detect_language(src, strlen(src));
    ASSERT(lang == MENTAL_LANG_GLSL, "ambiguous source defaults to GLSL");
    PASS("detect_ambiguous_defaults_glsl");
}

/* ================================================================== */
/*  2. API-to-Language Mapping                                        */
/* ================================================================== */

static int test_api_mapping(void) {
    ASSERT(mental_api_to_language(MENTAL_API_METAL)  == MENTAL_LANG_MSL,  "Metal -> MSL");
    ASSERT(mental_api_to_language(MENTAL_API_D3D12)  == MENTAL_LANG_HLSL, "D3D12 -> HLSL");
    ASSERT(mental_api_to_language(MENTAL_API_VULKAN) == MENTAL_LANG_SPIRV, "Vulkan -> SPIRV");
    ASSERT(mental_api_to_language(MENTAL_API_OPENCL) == MENTAL_LANG_SPIRV, "OpenCL -> SPIRV");
    ASSERT(mental_api_to_language(MENTAL_API_OPENGL) == MENTAL_LANG_GLSL, "OpenGL -> GLSL");
    ASSERT(mental_api_to_language(MENTAL_API_POCL)   == MENTAL_LANG_SPIRV, "PoCL -> SPIRV");
    PASS("api_to_language_mapping");
}

/* ================================================================== */
/*  3. Tool Path Configuration                                        */
/* ================================================================== */

static int test_tool_paths(void) {
    /* Save any auto-detected paths so we can restore them after the test */
    const char *saved_dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    const char *saved_naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    char dxc_backup[4096] = {0}, naga_backup[4096] = {0};
    if (saved_dxc) strncpy(dxc_backup, saved_dxc, sizeof(dxc_backup) - 1);
    if (saved_naga) strncpy(naga_backup, saved_naga, sizeof(naga_backup) - 1);

    /* Clear to test from a clean state */
    mental_set_tool_path(MENTAL_TOOL_DXC, NULL);
    mental_set_tool_path(MENTAL_TOOL_NAGA, NULL);

    /* Initially NULL */
    ASSERT(mental_get_tool_path(MENTAL_TOOL_DXC) == NULL, "DXC path initially NULL");
    ASSERT(mental_get_tool_path(MENTAL_TOOL_NAGA) == NULL, "Naga path initially NULL");

    /* Set and retrieve */
    mental_set_tool_path(MENTAL_TOOL_DXC, "/usr/local/bin/dxc");
    ASSERT(strcmp(mental_get_tool_path(MENTAL_TOOL_DXC), "/usr/local/bin/dxc") == 0, "DXC path set");

    mental_set_tool_path(MENTAL_TOOL_NAGA, "/usr/local/bin/naga");
    ASSERT(strcmp(mental_get_tool_path(MENTAL_TOOL_NAGA), "/usr/local/bin/naga") == 0, "Naga path set");

    /* Clear with NULL */
    mental_set_tool_path(MENTAL_TOOL_DXC, NULL);
    ASSERT(mental_get_tool_path(MENTAL_TOOL_DXC) == NULL, "DXC path cleared");

    /* Naga should be unaffected */
    ASSERT(mental_get_tool_path(MENTAL_TOOL_NAGA) != NULL, "Naga path still set");

    /* Restore original auto-detected paths */
    mental_set_tool_path(MENTAL_TOOL_DXC, dxc_backup[0] ? dxc_backup : NULL);
    mental_set_tool_path(MENTAL_TOOL_NAGA, naga_backup[0] ? naga_backup : NULL);

    PASS("tool_path_configuration");
}

/* ================================================================== */
/*  4. Source -> SPIR-V Compilation (embedded glslang, all platforms)  */
/* ================================================================== */

static int test_glsl_to_spirv(void) {
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_glsl_to_spirv(glsl_shader, strlen(glsl_shader),
                                                &spirv_len, error, sizeof(error));
    ASSERT(spirv != NULL, error[0] ? error : "GLSL -> SPIR-V returned NULL");
    ASSERT(spirv_len >= 4, "SPIR-V output has at least header");
    ASSERT(spirv[0] == 0x03 && spirv[1] == 0x02 && spirv[2] == 0x23 && spirv[3] == 0x07,
           "SPIR-V output has correct magic number");
    free(spirv);
    PASS("glsl_to_spirv");
}

static int test_hlsl_to_spirv(void) {
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_hlsl_to_spirv(hlsl_shader, strlen(hlsl_shader),
                                                &spirv_len, error, sizeof(error));
    ASSERT(spirv != NULL, error[0] ? error : "HLSL -> SPIR-V returned NULL");
    ASSERT(spirv_len >= 4, "SPIR-V output has at least header");
    ASSERT(spirv[0] == 0x03 && spirv[1] == 0x02 && spirv[2] == 0x23 && spirv[3] == 0x07,
           "SPIR-V output has correct magic number");
    free(spirv);
    PASS("hlsl_to_spirv");
}

static int test_wgsl_to_spirv(void) {
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_wgsl_to_spirv(wgsl_shader, strlen(wgsl_shader),
                                                &spirv_len, error, sizeof(error));
    ASSERT(spirv != NULL, error[0] ? error : "WGSL -> SPIR-V returned NULL");
    ASSERT(spirv_len >= 4, "SPIR-V output has at least header");
    ASSERT(spirv[0] == 0x03 && spirv[1] == 0x02 && spirv[2] == 0x23 && spirv[3] == 0x07,
           "SPIR-V output has correct magic number");
    free(spirv);
    PASS("wgsl_to_spirv");
}

static int test_invalid_glsl_to_spirv(void) {
    const char* bad = "this is not a shader at all!!!";
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_glsl_to_spirv(bad, strlen(bad),
                                                &spirv_len, error, sizeof(error));
    ASSERT(spirv == NULL, "invalid GLSL returns NULL");
    ASSERT(error[0] != '\0', "error message populated for invalid GLSL");
    PASS("invalid_glsl_to_spirv");
}

static int test_invalid_hlsl_to_spirv(void) {
    const char* bad = "complete nonsense {}{}{}";
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_hlsl_to_spirv(bad, strlen(bad),
                                                &spirv_len, error, sizeof(error));
    ASSERT(spirv == NULL, "invalid HLSL returns NULL");
    ASSERT(error[0] != '\0', "error message populated for invalid HLSL");
    PASS("invalid_hlsl_to_spirv");
}

/* ================================================================== */
/*  5. SPIR-V -> Target Language (embedded spirv-cross, all platforms) */
/* ================================================================== */

/* Helper: compile GLSL to SPIR-V once, reuse in multiple tests */
static unsigned char* g_spirv = NULL;
static size_t g_spirv_len = 0;

static int ensure_spirv(void) {
    if (g_spirv) return 1;
    char error[1024] = {0};
    g_spirv = mental_glsl_to_spirv(glsl_shader, strlen(glsl_shader),
                                   &g_spirv_len, error, sizeof(error));
    return g_spirv != NULL;
}

static int test_spirv_to_glsl(void) {
    ASSERT(ensure_spirv(), "need SPIR-V for this test");
    size_t out_len = 0;
    char error[1024] = {0};
    char* result = mental_spirv_to_glsl(g_spirv, g_spirv_len, &out_len, error, sizeof(error));
    ASSERT(result != NULL, error[0] ? error : "SPIR-V -> GLSL returned NULL");
    ASSERT(out_len > 0, "GLSL output non-empty");
    /* Verify output looks like GLSL */
    ASSERT(strstr(result, "#version") != NULL || strstr(result, "layout") != NULL ||
           strstr(result, "gl_") != NULL || strstr(result, "main") != NULL,
           "GLSL output contains GLSL markers");
    mental_transpile_free(result);
    PASS("spirv_to_glsl");
}

static int test_spirv_to_hlsl(void) {
    ASSERT(ensure_spirv(), "need SPIR-V for this test");
    size_t out_len = 0;
    char error[1024] = {0};
    char* result = mental_spirv_to_hlsl(g_spirv, g_spirv_len, &out_len, error, sizeof(error));
    ASSERT(result != NULL, error[0] ? error : "SPIR-V -> HLSL returned NULL");
    ASSERT(out_len > 0, "HLSL output non-empty");
    /* HLSL typically has RWStructuredBuffer or similar */
    ASSERT(strstr(result, "Buffer") != NULL || strstr(result, "void") != NULL ||
           strstr(result, "numthreads") != NULL || strstr(result, "SV_") != NULL,
           "HLSL output contains HLSL markers");
    mental_transpile_free(result);
    PASS("spirv_to_hlsl");
}

static int test_spirv_to_msl(void) {
    ASSERT(ensure_spirv(), "need SPIR-V for this test");
    size_t out_len = 0;
    char error[1024] = {0};
    char* result = mental_spirv_to_msl(g_spirv, g_spirv_len, &out_len, error, sizeof(error));
    ASSERT(result != NULL, error[0] ? error : "SPIR-V -> MSL returned NULL");
    ASSERT(out_len > 0, "MSL output non-empty");
    ASSERT(strstr(result, "metal") != NULL || strstr(result, "kernel") != NULL ||
           strstr(result, "device") != NULL || strstr(result, "thread") != NULL,
           "MSL output contains Metal markers");
    mental_transpile_free(result);
    PASS("spirv_to_msl");
}

static int test_spirv_to_wgsl(void) {
    ASSERT(ensure_spirv(), "need SPIR-V for this test");
    size_t out_len = 0;
    char error[1024] = {0};
    char* result = mental_spirv_to_wgsl(g_spirv, g_spirv_len, &out_len, error, sizeof(error));
    ASSERT(result != NULL, error[0] ? error : "SPIR-V -> WGSL returned NULL");
    ASSERT(out_len > 0, "WGSL output non-empty");
    ASSERT(strstr(result, "fn ") != NULL || strstr(result, "var<") != NULL ||
           strstr(result, "@") != NULL || strstr(result, "array<") != NULL,
           "WGSL output contains WGSL markers");
    mental_transpile_free(result);
    PASS("spirv_to_wgsl");
}

static int test_spirv_invalid_input(void) {
    const unsigned char junk[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00 };
    size_t out_len = 0;
    char error[1024] = {0};
    char* result = mental_spirv_to_glsl(junk, sizeof(junk), &out_len, error, sizeof(error));
    ASSERT(result == NULL, "invalid SPIR-V returns NULL for GLSL target");

    memset(error, 0, sizeof(error));
    result = mental_spirv_to_hlsl(junk, sizeof(junk), &out_len, error, sizeof(error));
    ASSERT(result == NULL, "invalid SPIR-V returns NULL for HLSL target");

    memset(error, 0, sizeof(error));
    result = mental_spirv_to_msl(junk, sizeof(junk), &out_len, error, sizeof(error));
    ASSERT(result == NULL, "invalid SPIR-V returns NULL for MSL target");

    memset(error, 0, sizeof(error));
    result = mental_spirv_to_wgsl(junk, sizeof(junk), &out_len, error, sizeof(error));
    ASSERT(result == NULL, "invalid SPIR-V returns NULL for WGSL target");

    PASS("spirv_invalid_input");
}

/* ================================================================== */
/*  6. Cross-language Round-trips via mental_transpile()              */
/* ================================================================== */

static int test_transpile_passthrough_glsl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(glsl_shader, strlen(glsl_shader),
                                    MENTAL_API_VULKAN, &out_len);
    ASSERT(result != NULL, "GLSL to Vulkan (SPIR-V) succeeded");
    /* Vulkan target produces SPIR-V binary — verify magic number */
    ASSERT(out_len >= 4, "output has at least SPIR-V header");
    unsigned char *bytes = (unsigned char *)result;
    ASSERT(bytes[0] == 0x03 && bytes[1] == 0x02 && bytes[2] == 0x23 && bytes[3] == 0x07,
           "output is valid SPIR-V");
    mental_transpile_free(result);
    PASS("transpile_passthrough_glsl");
}

static int test_transpile_passthrough_hlsl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(hlsl_shader, strlen(hlsl_shader),
                                    MENTAL_API_D3D12, &out_len);
    ASSERT(result != NULL, "HLSL pass-through to D3D12 succeeded");
    ASSERT(out_len == strlen(hlsl_shader), "pass-through preserves size");
    ASSERT(memcmp(result, hlsl_shader, out_len) == 0, "pass-through preserves content");
    mental_transpile_free(result);
    PASS("transpile_passthrough_hlsl");
}

static int test_transpile_passthrough_msl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(msl_shader, strlen(msl_shader),
                                    MENTAL_API_METAL, &out_len);
    ASSERT(result != NULL, "MSL pass-through to Metal succeeded");
    ASSERT(out_len == strlen(msl_shader), "pass-through preserves size");
    ASSERT(memcmp(result, msl_shader, out_len) == 0, "pass-through preserves content");
    mental_transpile_free(result);
    PASS("transpile_passthrough_msl");
}

static int test_transpile_glsl_to_hlsl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(glsl_shader, strlen(glsl_shader),
                                    MENTAL_API_D3D12, &out_len);
    ASSERT(result != NULL, "GLSL -> D3D12 (HLSL) transpilation succeeded");
    ASSERT(out_len > 0, "HLSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_glsl_to_hlsl");
}

static int test_transpile_glsl_to_msl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(glsl_shader, strlen(glsl_shader),
                                    MENTAL_API_METAL, &out_len);
    ASSERT(result != NULL, "GLSL -> Metal (MSL) transpilation succeeded");
    ASSERT(out_len > 0, "MSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_glsl_to_msl");
}

static int test_transpile_hlsl_to_glsl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(hlsl_shader, strlen(hlsl_shader),
                                    MENTAL_API_VULKAN, &out_len);
    ASSERT(result != NULL, "HLSL -> Vulkan (GLSL) transpilation succeeded");
    ASSERT(out_len > 0, "GLSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_hlsl_to_glsl");
}

static int test_transpile_hlsl_to_msl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(hlsl_shader, strlen(hlsl_shader),
                                    MENTAL_API_METAL, &out_len);
    ASSERT(result != NULL, "HLSL -> Metal (MSL) transpilation succeeded");
    ASSERT(out_len > 0, "MSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_hlsl_to_msl");
}

static int test_transpile_wgsl_to_glsl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(wgsl_shader, strlen(wgsl_shader),
                                    MENTAL_API_VULKAN, &out_len);
    ASSERT(result != NULL, "WGSL -> Vulkan (GLSL) transpilation succeeded");
    ASSERT(out_len > 0, "GLSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_wgsl_to_glsl");
}

static int test_transpile_wgsl_to_hlsl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(wgsl_shader, strlen(wgsl_shader),
                                    MENTAL_API_D3D12, &out_len);
    ASSERT(result != NULL, "WGSL -> D3D12 (HLSL) transpilation succeeded");
    ASSERT(out_len > 0, "HLSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_wgsl_to_hlsl");
}

static int test_transpile_wgsl_to_msl(void) {
    size_t out_len = 0;
    char* result = mental_transpile(wgsl_shader, strlen(wgsl_shader),
                                    MENTAL_API_METAL, &out_len);
    ASSERT(result != NULL, "WGSL -> Metal (MSL) transpilation succeeded");
    ASSERT(out_len > 0, "MSL output non-empty");
    mental_transpile_free(result);
    PASS("transpile_wgsl_to_msl");
}

static int test_transpile_msl_to_non_metal_fails(void) {
    size_t out_len = 0;
    char* result = mental_transpile(msl_shader, strlen(msl_shader),
                                    MENTAL_API_VULKAN, &out_len);
    ASSERT(result == NULL, "MSL -> Vulkan correctly rejected");

    result = mental_transpile(msl_shader, strlen(msl_shader),
                              MENTAL_API_D3D12, &out_len);
    ASSERT(result == NULL, "MSL -> D3D12 correctly rejected");
    PASS("transpile_msl_to_non_metal_fails");
}

/* ================================================================== */
/*  7. Re-detection: transpiled output should detect as target lang   */
/* ================================================================== */

static int test_redetect_glsl_to_msl(void) {
    size_t out_len = 0;
    char* msl = mental_transpile(glsl_shader, strlen(glsl_shader),
                                 MENTAL_API_METAL, &out_len);
    ASSERT(msl != NULL, "GLSL -> MSL transpilation succeeded");
    mental_language detected = mental_detect_language(msl, out_len);
    ASSERT(detected == MENTAL_LANG_MSL, "transpiled MSL re-detects as MSL");
    mental_transpile_free(msl);
    PASS("redetect_glsl_to_msl");
}

static int test_redetect_glsl_to_hlsl(void) {
    size_t out_len = 0;
    char* hlsl = mental_transpile(glsl_shader, strlen(glsl_shader),
                                  MENTAL_API_D3D12, &out_len);
    ASSERT(hlsl != NULL, "GLSL -> HLSL transpilation succeeded");
    mental_language detected = mental_detect_language(hlsl, out_len);
    ASSERT(detected == MENTAL_LANG_HLSL, "transpiled HLSL re-detects as HLSL");
    mental_transpile_free(hlsl);
    PASS("redetect_glsl_to_hlsl");
}

static int test_redetect_hlsl_to_vulkan(void) {
    size_t out_len = 0;
    char* result = mental_transpile(hlsl_shader, strlen(hlsl_shader),
                                    MENTAL_API_VULKAN, &out_len);
    ASSERT(result != NULL, "HLSL -> Vulkan (SPIR-V) transpilation succeeded");
    mental_language detected = mental_detect_language(result, out_len);
    ASSERT(detected == MENTAL_LANG_SPIRV, "transpiled output re-detects as SPIR-V");
    mental_transpile_free(result);
    PASS("redetect_hlsl_to_vulkan");
}

/* ================================================================== */
/*  8. Full Pipeline Execution (hardware-dependent)                   */
/*                                                                    */
/*  Discovers the device at runtime, then tests every source language */
/*  that should work on the current backend:                          */
/*    Metal  : GLSL, HLSL, WGSL, MSL (native)                        */
/*    D3D12  : GLSL, HLSL (native), WGSL                             */
/*    Vulkan : GLSL (native), HLSL, WGSL                             */
/*    OpenCL : GLSL (native), HLSL, WGSL                             */
/*  MSL is only accepted on Metal.                                    */
/* ================================================================== */

static int run_shader_on_device(const char* label, const char* source, size_t source_len,
                                mental_device dev, int required) {
    /* Unique reference names per shader test */
    char n0[64], n1[64], n2[64];
    snprintf(n0, sizeof(n0), "tp-%s-i0", label);
    snprintf(n1, sizeof(n1), "tp-%s-i1", label);
    snprintf(n2, sizeof(n2), "tp-%s-out", label);

    mental_kernel kernel = mental_compile(dev, source, source_len);
    if (!kernel) {
        if (!required) { SKIP(label, mental_get_error_message()); }
        ASSERT(kernel != NULL, mental_get_error_message());
    }

    size_t count = 64;
    size_t size = count * sizeof(float);

    mental_reference in0 = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(in0, dev);
    mental_reference in1 = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(in1, dev);
    mental_reference out = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(out, dev);

    ASSERT(in0 && in1 && out, "buffer allocation");

    float d0[64], d1[64];
    for (int i = 0; i < 64; i++) { d0[i] = (float)i; d1[i] = (float)(i * 3); }
    mental_reference_write(in0, d0, size);
    mental_reference_write(in1, d1, size);

    mental_reference inputs[] = { in0, in1 };
    mental_dispatch(kernel, inputs, 2, (mental_reference[]){out}, 1, (int)count);
    ASSERT(mental_get_error() == MENTAL_SUCCESS, "dispatch succeeded");

    float results[64];
    mental_reference_read(out, results, size);

    int errors = 0;
    for (int i = 0; i < 64; i++) {
        float expected = d0[i] + d1[i];
        if (results[i] != expected) errors++;
    }

    mental_reference_close(in0);
    mental_reference_close(in1);
    mental_reference_close(out);
    mental_kernel_finalize(kernel);

    ASSERT(errors == 0, "compute results match");
    PASS(label);
}

static int test_exec_glsl(mental_device dev) {
    return run_shader_on_device("exec-glsl", glsl_shader, strlen(glsl_shader), dev, 1);
}

static int test_exec_hlsl(mental_device dev) {
    return run_shader_on_device("exec-hlsl", hlsl_shader, strlen(hlsl_shader), dev, 1);
}

static int test_exec_wgsl(mental_device dev) {
    return run_shader_on_device("exec-wgsl", wgsl_shader, strlen(wgsl_shader), dev, 1);
}

static int test_exec_msl(mental_device dev, mental_api_type api) {
    if (api != MENTAL_API_METAL) {
        SKIP("exec-msl", "MSL only supported on Metal backend");
    }
    return run_shader_on_device("exec-msl", msl_shader, strlen(msl_shader), dev, 1);
}

/* ================================================================== */
/*  9. Double-hop: transpile A->B->A and verify execution             */
/*     Ensures the transpiler preserves semantic correctness through   */
/*     multiple conversions.                                          */
/* ================================================================== */

static int test_double_hop(mental_device dev, mental_api_type api) {
    /* GLSL -> MSL -> GLSL (or appropriate pair for this backend) */
    mental_api_type intermediate_api;
    const char* label;

    if (api == MENTAL_API_METAL) {
        intermediate_api = MENTAL_API_D3D12;  /* GLSL -> HLSL -> GLSL */
        label = "double-hop-glsl-hlsl-glsl";
    } else if (api == MENTAL_API_D3D12) {
        intermediate_api = MENTAL_API_VULKAN; /* HLSL -> GLSL -> HLSL */
        label = "double-hop-hlsl-glsl-hlsl";
    } else {
        intermediate_api = MENTAL_API_D3D12;  /* GLSL -> HLSL -> GLSL */
        label = "double-hop-glsl-hlsl-glsl";
    }

    /* Hop 1: source language -> intermediate */
    size_t mid_len = 0;
    char* mid = mental_transpile(glsl_shader, strlen(glsl_shader),
                                 intermediate_api, &mid_len);
    ASSERT(mid != NULL, "first hop transpilation succeeded");

    /* Hop 2: intermediate -> back to device's native language */
    size_t final_len = 0;
    char* final_src = mental_transpile(mid, mid_len, api, &final_len);
    mental_transpile_free(mid);
    ASSERT(final_src != NULL, "second hop transpilation succeeded");

    /* Execute the double-hopped shader */
    int rc = run_shader_on_device(label, final_src, final_len, dev, 1);
    mental_transpile_free(final_src);
    return rc;
}

/* ================================================================== */
/*  Main: run everything                                              */
/* ================================================================== */

int main(void) {
    printf("=== Transpilation Test Suite ===\n\n");

    /* Auto-detect external tools before running any tests */
    printf("[Tool Auto-Detection]\n");
    auto_detect_tools();
    printf("\n");

    /* ---- Section 1: Language Detection ---- */
    printf("[Language Detection]\n");
    test_detect_glsl_version();
    test_detect_glsl_layout();
    test_detect_glsl_builtin();
    test_detect_hlsl_numthreads();
    test_detect_hlsl_rwbuffer();
    test_detect_hlsl_cbuffer();
    test_detect_msl_kernel_void();
    test_detect_msl_kernel_attr();
    test_detect_msl_threadgroup();
    test_detect_wgsl_compute();
    test_detect_wgsl_vertex();
    test_detect_wgsl_fn();
    test_detect_spirv_magic();
    test_detect_null_input();
    test_detect_ambiguous_defaults_glsl();

    /* ---- Section 2: API Mapping ---- */
    printf("\n[API-to-Language Mapping]\n");
    test_api_mapping();

    /* ---- Section 3: Tool Paths ---- */
    printf("\n[Tool Path Configuration]\n");
    test_tool_paths();

    /* ---- Section 4: Source -> SPIR-V ---- */
    printf("\n[Source -> SPIR-V Compilation]\n");
    test_glsl_to_spirv();
    test_hlsl_to_spirv();
    test_wgsl_to_spirv();
    test_invalid_glsl_to_spirv();
    test_invalid_hlsl_to_spirv();

    /* ---- Section 5: SPIR-V -> Target Language ---- */
    printf("\n[SPIR-V -> Target Language]\n");
    test_spirv_to_glsl();
    test_spirv_to_hlsl();
    test_spirv_to_msl();
    test_spirv_to_wgsl();
    test_spirv_invalid_input();

    /* ---- Section 6: Cross-language Transpilation ---- */
    printf("\n[Cross-language Transpilation]\n");
    test_transpile_passthrough_glsl();
    test_transpile_passthrough_hlsl();
    test_transpile_passthrough_msl();
    test_transpile_glsl_to_hlsl();
    test_transpile_glsl_to_msl();
    test_transpile_hlsl_to_glsl();
    test_transpile_hlsl_to_msl();
    test_transpile_wgsl_to_glsl();
    test_transpile_wgsl_to_hlsl();
    test_transpile_wgsl_to_msl();
    test_transpile_msl_to_non_metal_fails();

    /* ---- Section 7: Re-detection after Transpilation ---- */
    printf("\n[Re-detection After Transpilation]\n");
    test_redetect_glsl_to_msl();
    test_redetect_glsl_to_hlsl();
    test_redetect_hlsl_to_vulkan();

    /* ---- Section 8 & 9: Execution (hardware-dependent) ---- */
    printf("\n[Full Pipeline Execution]\n");

    mental_device dev = mental_device_get(0);
    if (!dev) {
        printf("  SKIP: No GPU device available - skipping execution tests\n");
        g_skip += 5; /* exec-glsl, exec-hlsl, exec-wgsl, exec-msl, double-hop */
    } else {
        mental_api_type api = mental_device_api(dev);
        printf("  Device: %s (%s)\n", mental_device_name(dev), mental_device_api_name(dev));

        test_exec_glsl(dev);
        test_exec_hlsl(dev);
        test_exec_wgsl(dev);
        test_exec_msl(dev, api);

        printf("\n[Double-hop Round-trip Execution]\n");
        test_double_hop(dev, api);
    }

    /* ---- Cleanup ---- */
    if (g_spirv) { free(g_spirv); g_spirv = NULL; }

    /* ---- Summary ---- */
    printf("\n=== Results: %d passed, %d failed, %d skipped ===\n",
           g_pass, g_fail, g_skip);

    return g_fail > 0 ? 1 : 0;
}
