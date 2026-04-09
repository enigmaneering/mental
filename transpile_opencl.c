/*
 * Mental - GLSL to OpenCL C transpilation
 *
 * Converts the GLSL output from spirv-cross into OpenCL C source code.
 * This enables the OpenCL and PoCL backends to execute compute shaders
 * written in GLSL without requiring OpenCL SPIR-V support.
 *
 * The transform is mechanical — it only handles the subset of GLSL that
 * mental's compute API produces:
 *   - SSBOs (layout std430) → kernel __global pointer arguments
 *   - gl_GlobalInvocationID.x → get_global_id(0)
 *   - Struct-wrapped buffer access → direct pointer access
 *
 * Pipeline: GLSL → SPIR-V (glslang) → GLSL (spirv-cross) → OpenCL C (this)
 */

#include "transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Maximum number of buffer bindings we support */
#define MAX_BINDINGS 32

typedef struct {
    int binding;
    char type[64];       /* e.g. "float" */
    char member[64];     /* e.g. "a" (the array member name) */
    char instance[64];   /* e.g. "_27" (the struct instance name) */
    int is_readonly;
} BufferBinding;

/* Parse a multi-line buffer declaration from spirv-cross output:
 *
 * layout(set = 0, binding = 0, std430) readonly buffer A
 * {
 *     float a[];
 * } _27;
 *
 * The 'start' pointer should point to the beginning of a line containing
 * "layout" and "buffer".  Returns the pointer past the closing ";",
 * or NULL on failure. */
static const char* parse_buffer_decl(const char* start, BufferBinding* out) {
    memset(out, 0, sizeof(*out));

    /* Must contain "buffer" on this line */
    const char* line_end = strchr(start, '\n');
    if (!line_end) line_end = start + strlen(start);

    /* Check for "buffer" keyword */
    const char* buf_pos = strstr(start, "buffer");
    if (!buf_pos || buf_pos > line_end) return NULL;

    /* Extract binding number */
    const char* bind = strstr(start, "binding");
    if (!bind || bind > line_end) return NULL;
    bind = strchr(bind, '=');
    if (!bind) return NULL;
    out->binding = atoi(bind + 1);

    /* Check readonly */
    char line_buf[512];
    size_t llen = line_end - start;
    if (llen >= sizeof(line_buf)) llen = sizeof(line_buf) - 1;
    memcpy(line_buf, start, llen);
    line_buf[llen] = '\0';
    out->is_readonly = (strstr(line_buf, "readonly") != NULL);

    /* Find the opening brace (may be on this line or next) */
    const char* brace_open = strchr(start, '{');
    if (!brace_open) return NULL;

    /* Find the closing brace */
    const char* brace_close = strchr(brace_open + 1, '}');
    if (!brace_close) return NULL;

    /* Parse content between braces: "    float a[];\n" */
    const char* inner = brace_open + 1;
    while (*inner == ' ' || *inner == '\n' || *inner == '\r' || *inner == '\t') inner++;

    /* Get type */
    const char* type_end = inner;
    while (*type_end && !isspace(*type_end)) type_end++;
    size_t type_len = type_end - inner;
    if (type_len >= sizeof(out->type)) type_len = sizeof(out->type) - 1;
    strncpy(out->type, inner, type_len);
    out->type[type_len] = '\0';

    /* Get member name */
    const char* name_start = type_end;
    while (*name_start == ' ') name_start++;
    const char* name_end = name_start;
    while (*name_end && *name_end != '[' && *name_end != ';' && *name_end != ' ') name_end++;
    size_t name_len = name_end - name_start;
    if (name_len >= sizeof(out->member)) name_len = sizeof(out->member) - 1;
    strncpy(out->member, name_start, name_len);
    out->member[name_len] = '\0';

    /* Extract instance name after "}" — e.g. "} _27;" */
    const char* inst_start = brace_close + 1;
    while (*inst_start == ' ' || *inst_start == '\n') inst_start++;
    const char* inst_end = inst_start;
    while (*inst_end && *inst_end != ';' && *inst_end != ' ' && *inst_end != '\n') inst_end++;
    size_t inst_len = inst_end - inst_start;
    if (inst_len >= sizeof(out->instance)) inst_len = sizeof(out->instance) - 1;
    strncpy(out->instance, inst_start, inst_len);
    out->instance[inst_len] = '\0';

    /* Return pointer past the semicolon */
    const char* semi = strchr(brace_close, ';');
    return semi ? semi + 1 : brace_close + 1;
}

/* Replace all occurrences of 'from' with 'to' in src, writing to dst.
 * Returns number of bytes written. dst must be large enough. */
static size_t str_replace_all(char* dst, size_t dst_size,
                               const char* src,
                               const char* from, const char* to) {
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t written = 0;

    while (*src && written < dst_size - 1) {
        if (strncmp(src, from, from_len) == 0) {
            if (written + to_len >= dst_size - 1) break;
            memcpy(dst + written, to, to_len);
            written += to_len;
            src += from_len;
        } else {
            dst[written++] = *src++;
        }
    }
    dst[written] = '\0';
    return written;
}

char* mental_glsl_to_opencl_c(const char* glsl_source, size_t glsl_len,
                               size_t* out_len, char* error, size_t error_len) {
    /* Parse all buffer bindings */
    BufferBinding bindings[MAX_BINDINGS];
    int binding_count = 0;

    /* Work line by line */
    char* src_copy = malloc(glsl_len + 1);
    if (!src_copy) {
        if (error) snprintf(error, error_len, "Memory allocation failed");
        return NULL;
    }
    memcpy(src_copy, glsl_source, glsl_len);
    src_copy[glsl_len] = '\0';

    /* First pass: scan for buffer declarations */
    const char* scan = src_copy;
    while (scan && *scan && binding_count < MAX_BINDINGS) {
        /* Look for "layout" followed by "buffer" on the same line */
        const char* layout = strstr(scan, "layout");
        if (!layout) break;

        /* Check if "buffer" appears before the next newline or within a few lines */
        const char* buf_kw = strstr(layout, "buffer");
        if (!buf_kw) break;

        /* Make sure this is on the same logical line (within ~200 chars) */
        if (buf_kw - layout > 200) {
            scan = layout + 1;
            continue;
        }

        BufferBinding b;
        const char* after = parse_buffer_decl(layout, &b);
        if (after) {
            bindings[binding_count++] = b;
            scan = after;
        } else {
            scan = layout + 1;
        }
    }

    if (binding_count == 0) {
        if (error) snprintf(error, error_len, "No buffer bindings found in GLSL source");
        free(src_copy);
        return NULL;
    }

    /* Sort bindings by binding index */
    for (int i = 0; i < binding_count - 1; i++) {
        for (int j = 0; j < binding_count - 1 - i; j++) {
            if (bindings[j].binding > bindings[j + 1].binding) {
                BufferBinding tmp = bindings[j];
                bindings[j] = bindings[j + 1];
                bindings[j + 1] = tmp;
            }
        }
    }

    /* Build kernel argument list.
     * Use "argN" names to avoid collisions when multiple buffers have
     * the same member name (e.g. "data" in both input and output). */
    char args[4096] = {0};
    size_t args_pos = 0;
    for (int i = 0; i < binding_count; i++) {
        if (i > 0) {
            args_pos += snprintf(args + args_pos, sizeof(args) - args_pos, ", ");
        }
        args_pos += snprintf(args + args_pos, sizeof(args) - args_pos,
                             "__global %s* arg%d",
                             bindings[i].type, bindings[i].binding);
    }

    /* Second pass: build the OpenCL C source */
    /* Allocate generous output buffer */
    size_t out_capacity = glsl_len * 4 + 4096;
    char* output = malloc(out_capacity);
    if (!output) {
        if (error) snprintf(error, error_len, "Memory allocation failed");
        free(src_copy);
        return NULL;
    }
    size_t pos = 0;

    /* Extract the body of main() */
    const char* main_start = strstr(src_copy, "void main()");
    if (!main_start) {
        if (error) snprintf(error, error_len, "Could not find 'void main()' in GLSL source");
        free(src_copy);
        free(output);
        return NULL;
    }

    /* Find the opening brace */
    const char* body_start = strchr(main_start, '{');
    if (!body_start) {
        if (error) snprintf(error, error_len, "Could not find main() body");
        free(src_copy);
        free(output);
        return NULL;
    }
    body_start++; /* skip '{' */

    /* Find matching closing brace */
    int depth = 1;
    const char* body_end = body_start;
    while (*body_end && depth > 0) {
        if (*body_end == '{') depth++;
        if (*body_end == '}') depth--;
        if (depth > 0) body_end++;
    }

    /* Extract body */
    size_t body_len = body_end - body_start;
    char* body = malloc(body_len + 1);
    memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    /* Replace gl_GlobalInvocationID.x -> get_global_id(0) */
    char* body2 = malloc(out_capacity);
    str_replace_all(body2, out_capacity, body,
                    "gl_GlobalInvocationID.x", "get_global_id(0)");
    free(body);

    /* Replace gl_GlobalInvocationID.y -> get_global_id(1) */
    char* body3 = malloc(out_capacity);
    str_replace_all(body3, out_capacity, body2,
                    "gl_GlobalInvocationID.y", "get_global_id(1)");
    free(body2);

    /* Replace gl_GlobalInvocationID.z -> get_global_id(2) */
    char* body4 = malloc(out_capacity);
    str_replace_all(body4, out_capacity, body3,
                    "gl_GlobalInvocationID.z", "get_global_id(2)");
    free(body3);

    /* Replace struct-wrapped access: _27.a[i] -> arg0[i] */
    char* current = body4;
    for (int i = 0; i < binding_count; i++) {
        if (bindings[i].instance[0] == '\0') continue;

        /* Build "INSTANCE.MEMBER" pattern → "argN" replacement */
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "%s.%s",
                 bindings[i].instance, bindings[i].member);

        char replacement[64];
        snprintf(replacement, sizeof(replacement), "arg%d", bindings[i].binding);

        char* next_body = malloc(out_capacity);
        str_replace_all(next_body, out_capacity, current,
                        pattern, replacement);

        if (current != body4) free(current);
        current = next_body;
    }

    /* Replace 'uint' with 'unsigned int' for strict OpenCL C */
    char* final_body = malloc(out_capacity);
    /* Only replace standalone 'uint', not 'uint2' etc. */
    /* For simplicity, replace "uint " with "unsigned int " */
    str_replace_all(final_body, out_capacity, current,
                    "uint ", "unsigned int ");
    if (current != body4) free(current);

    /* Assemble the OpenCL C source */
    pos += snprintf(output + pos, out_capacity - pos,
                    "__kernel void mental_compute(%s)\n{\n%s}\n", args, final_body);

    free(body4);
    free(final_body);
    free(src_copy);

    if (out_len) *out_len = pos;
    return output;
}
