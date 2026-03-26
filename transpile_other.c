/*
 * Mental - HLSL and WGSL to SPIRV compilation
 *
 * Uses external compilers:
 * - HLSL: Microsoft DXC compiler
 * - WGSL: Naga from the wgpu project
 */

#include "transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <direct.h>
#define popen _popen
#define pclose _pclose
#define rmdir _rmdir

/* Windows mkdtemp: _mktemp generates name, _mkdir creates it. */
static char* win_mkdtemp(char* tmpl) {
    if (!_mktemp(tmpl)) return NULL;
    if (_mkdir(tmpl) != 0) return NULL;
    return tmpl;
}
#define mkdtemp(t) win_mkdtemp(t)
#else
#include <unistd.h>
#endif

/*
 * Portable temp directory prefix.
 * Windows: uses TEMP/TMP env var (e.g. C:\Users\...\AppData\Local\Temp)
 * Unix: /tmp
 */
static const char* get_tmp_prefix(void) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    return tmp;
#else
    return "/tmp";
#endif
}

/* Configured tool paths — set via mental_set_tool_path(). */
static char g_dxc_path[4096];
static char g_naga_path[4096];
static char g_pocl_path[4096];

void mental_set_tool_path(mental_tool tool, const char* path) {
    char* dest = NULL;
    size_t cap = 0;

    switch (tool) {
    case MENTAL_TOOL_DXC:  dest = g_dxc_path;  cap = sizeof(g_dxc_path);  break;
    case MENTAL_TOOL_NAGA: dest = g_naga_path; cap = sizeof(g_naga_path); break;
    case MENTAL_TOOL_POCL: dest = g_pocl_path; cap = sizeof(g_pocl_path); break;
    default: return;
    }

    if (path) {
        strncpy(dest, path, cap - 1);
        dest[cap - 1] = '\0';
    } else {
        dest[0] = '\0';
    }
}

const char* mental_get_tool_path(mental_tool tool) {
    switch (tool) {
    case MENTAL_TOOL_DXC:  return g_dxc_path[0]  ? g_dxc_path  : NULL;
    case MENTAL_TOOL_NAGA: return g_naga_path[0] ? g_naga_path : NULL;
    case MENTAL_TOOL_POCL: return g_pocl_path[0] ? g_pocl_path : NULL;
    default: return NULL;
    }
}

/* Helper to read file into buffer */
static unsigned char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    unsigned char* data = malloc(size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(data, 1, size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(data);
        return NULL;
    }

    *out_len = size;
    return data;
}

unsigned char* mental_hlsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    const char* dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    if (!dxc) {
        if (error) strncpy(error, "DXC compiler not configured (call mental_set_tool_path)", error_len - 1);
        return NULL;
    }

    /* Create temporary directory */
    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/mental_hlsl_XXXXXX", get_tmp_prefix());
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }

    /* Write HLSL source to temp file */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.hlsl", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write HLSL source", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Compile HLSL to SPIRV using DXC */
    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.spv", tmpdir);
    snprintf(cmd, sizeof(cmd),
             "%s -spirv -T cs_6_0 -E main -Fo %s %s 2>&1",
             dxc, out_path, src_path);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        if (error) strncpy(error, "Failed to execute DXC compiler (is DXC installed?)", error_len - 1);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    char output_buf[4096] = {0};
    fread(output_buf, 1, sizeof(output_buf) - 1, pipe);
    int status = pclose(pipe);

    if (status != 0) {
        if (error) snprintf(error, error_len, "DXC compilation failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    /* Read compiled SPIRV */
    unsigned char* spirv = read_file(out_path, out_len);

    /* Cleanup */
    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!spirv && error) {
        strncpy(error, "Failed to read SPIRV output", error_len - 1);
    }

    return spirv;
}

unsigned char* mental_wgsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    const char* naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    if (!naga) {
        if (error) strncpy(error, "Naga compiler not configured (call mental_set_tool_path)", error_len - 1);
        return NULL;
    }

    /* Create temporary directory */
    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/mental_wgsl_XXXXXX", get_tmp_prefix());
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }

    /* Write WGSL source to temp file */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.wgsl", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write WGSL source", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Compile WGSL to SPIRV using Naga */
    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.spv", tmpdir);
    snprintf(cmd, sizeof(cmd),
             "%s %s %s 2>&1",
             naga, src_path, out_path);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        if (error) strncpy(error, "Failed to execute Naga compiler (is Naga installed?)", error_len - 1);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    char output_buf[4096] = {0};
    fread(output_buf, 1, sizeof(output_buf) - 1, pipe);
    int status = pclose(pipe);

    if (status != 0) {
        if (error) snprintf(error, error_len, "Naga compilation failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    /* Read compiled SPIRV */
    unsigned char* spirv = read_file(out_path, out_len);

    /* Cleanup */
    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!spirv && error) {
        strncpy(error, "Failed to read SPIRV output", error_len - 1);
    }

    return spirv;
}

/*
 * SPIR-V -> WGSL via Naga.
 *
 * spirv-cross has no WGSL backend, so we use the same Naga tool
 * that handles WGSL -> SPIR-V, just with reversed file extensions.
 * Naga infers direction from .spv (input) and .wgsl (output).
 */
char* mental_spirv_to_wgsl(const unsigned char* spirv, size_t spirv_len,
                            size_t* out_len, char* error, size_t error_len) {
    const char* naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    if (!naga) {
        if (error) strncpy(error, "Naga compiler not configured (call mental_set_tool_path)", error_len - 1);
        return NULL;
    }

    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/mental_spv2wgsl_XXXXXX", get_tmp_prefix());
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }

    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.spv", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write SPIR-V input", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(spirv, 1, spirv_len, f);
    fclose(f);

    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.wgsl", tmpdir);
    snprintf(cmd, sizeof(cmd), "%s %s %s 2>&1", naga, src_path, out_path);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        if (error) strncpy(error, "Failed to execute Naga (is Naga installed?)", error_len - 1);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    char output_buf[4096] = {0};
    fread(output_buf, 1, sizeof(output_buf) - 1, pipe);
    int status = pclose(pipe);

    if (status != 0) {
        if (error) snprintf(error, error_len, "Naga SPIR-V to WGSL failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    size_t wgsl_len = 0;
    unsigned char* wgsl_raw = read_file(out_path, &wgsl_len);

    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!wgsl_raw) {
        if (error) strncpy(error, "Failed to read WGSL output", error_len - 1);
        return NULL;
    }

    char* result = (char*)realloc(wgsl_raw, wgsl_len + 1);
    if (!result) { free(wgsl_raw); return NULL; }
    result[wgsl_len] = '\0';
    *out_len = wgsl_len;
    return result;
}
