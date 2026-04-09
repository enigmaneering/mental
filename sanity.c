/*
 * Mental - Built-in Sanity Check
 *
 * Exercises device enumeration, buffer operations, shader compilation,
 * GPU dispatch, and result verification.  Designed to be run as a
 * pre-built binary on any target machine — no build tools required.
 *
 * Returns 0 if all checks pass, non-zero on failure.
 */

#include "mental.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ── Test tracking ───────────────────────────────────────────────── */

static int g_passed = 0;
static int g_failed = 0;
static int g_skipped = 0;

static void pass(const char *name) {
    printf("  [PASS] %s\n", name);
    g_passed++;
}

static void fail(const char *name, const char *reason) {
    printf("  [FAIL] %s — %s\n", name, reason);
    g_failed++;
}

static void skip(const char *name, const char *reason) {
    printf("  [SKIP] %s — %s\n", name, reason);
    g_skipped++;
}

/* ── GLSL shader sources ─────────────────────────────────────────── */

static const char *add_shader =
    "#version 450\n"
    "layout(local_size_x = 256) in;\n"
    "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    "layout(std430, binding = 1) readonly buffer B { float b[]; };\n"
    "layout(std430, binding = 2) writeonly buffer C { float c[]; };\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    c[i] = a[i] + b[i];\n"
    "}\n";

static const char *scale_shader =
    "#version 450\n"
    "layout(local_size_x = 256) in;\n"
    "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    "layout(std430, binding = 1) writeonly buffer B { float b[]; };\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    b[i] = a[i] * 3.0;\n"
    "}\n";

/* ── Individual checks ───────────────────────────────────────────── */

static void check_state(void) {
    mental_state *state = mental_state_get();
    if (!state) {
        fail("state", "mental_state_get() returned NULL");
        return;
    }
    if (!state->active_backend_name || state->active_backend_name[0] == '\0') {
        /* No backend is valid — could be a headless machine */
    }
    if (state->library_count < 0) {
        mental_state_free(state);
        fail("state", "negative library count");
        return;
    }
    mental_state_free(state);
    pass("state");
}

static void check_devices(int *out_count) {
    int count = mental_device_count();
    *out_count = count;

    if (count < 0) {
        fail("device enumeration", "negative device count");
        return;
    }
    if (count == 0) {
        pass("device enumeration (0 devices — graceful)");
        return;
    }

    for (int i = 0; i < count && i < 4; i++) {
        mental_device dev = mental_device_get(i);
        if (!dev) {
            fail("device enumeration", "mental_device_get returned NULL");
            return;
        }
        const char *name = mental_device_name(dev);
        const char *api = mental_device_api_name(dev);
        if (!name || !api) {
            fail("device enumeration", "NULL device name or API name");
            return;
        }
    }
    pass("device enumeration");
}

static void check_reference_lifecycle(void) {
    /* Create a reference, write data, read it back, verify, close */
    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float readback[4] = {0};
    size_t size = sizeof(data);

    mental_reference ref = mental_reference_create(
        size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    if (!ref) {
        fail("reference lifecycle", "create failed");
        return;
    }

    mental_reference_write(ref, data, size);
    mental_reference_read(ref, readback, size);

    for (int i = 0; i < 4; i++) {
        if (readback[i] != data[i]) {
            fail("reference lifecycle", "read-back mismatch");
            mental_reference_close(ref);
            return;
        }
    }

    mental_reference_close(ref);
    pass("reference lifecycle");
}

static void check_disclosure(void) {
    /* Open mode — should be writable without credential */
    mental_disclosure dh = NULL;
    mental_reference ref = mental_reference_create(
        16, MENTAL_RELATIONALLY_OPEN, NULL, 0, &dh);
    if (!ref) {
        fail("disclosure", "create open ref failed");
        return;
    }
    if (!mental_reference_writable(ref, NULL, 0)) {
        fail("disclosure", "open ref should be writable");
        mental_reference_close(ref);
        return;
    }
    mental_reference_close(ref);

    /* Exclusive mode — should NOT be writable without credential */
    const char *cred = "secret";
    ref = mental_reference_create(
        16, MENTAL_RELATIONALLY_EXCLUSIVE, cred, strlen(cred), &dh);
    if (!ref) {
        fail("disclosure", "create exclusive ref failed");
        return;
    }
    if (mental_reference_writable(ref, NULL, 0)) {
        fail("disclosure", "exclusive ref should NOT be writable without credential");
        mental_reference_close(ref);
        return;
    }
    if (!mental_reference_writable(ref, cred, strlen(cred))) {
        fail("disclosure", "exclusive ref should be writable WITH credential");
        mental_reference_close(ref);
        return;
    }
    mental_reference_close(ref);
    pass("disclosure");
}

static void check_gpu_buffer(mental_device dev) {
    float data[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    float readback[4] = {0};
    size_t size = sizeof(data);

    mental_reference ref = mental_reference_create(
        size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    if (!ref) {
        fail("GPU buffer", "create failed");
        return;
    }

    mental_reference_write(ref, data, size);

    if (mental_reference_pin(ref, dev) != 0) {
        fail("GPU buffer", mental_get_error_message());
        mental_reference_close(ref);
        return;
    }

    mental_reference_read(ref, readback, size);
    for (int i = 0; i < 4; i++) {
        if (readback[i] != data[i]) {
            fail("GPU buffer", "pin + read-back mismatch");
            mental_reference_close(ref);
            return;
        }
    }

    mental_reference_close(ref);
    pass("GPU buffer");
}

static void check_compile(mental_device dev, mental_kernel *out_kernel) {
    *out_kernel = NULL;

    mental_kernel k = mental_compile(dev, add_shader, strlen(add_shader));
    if (!k) {
        fail("shader compile", mental_get_error_message());
        return;
    }

    *out_kernel = k;
    pass("shader compile");
}

static void check_dispatch(mental_device dev, mental_kernel kernel) {
    #define N 256
    float a[N], b[N], c[N];
    for (int i = 0; i < N; i++) {
        a[i] = (float)i;
        b[i] = (float)(i * 10);
    }

    size_t size = N * sizeof(float);
    mental_reference ref_a = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_b = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_c = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

    if (!ref_a || !ref_b || !ref_c) {
        fail("dispatch", "reference creation failed");
        if (ref_a) mental_reference_close(ref_a);
        if (ref_b) mental_reference_close(ref_b);
        if (ref_c) mental_reference_close(ref_c);
        return;
    }

    mental_reference_write(ref_a, a, size);
    mental_reference_write(ref_b, b, size);
    mental_reference_pin(ref_a, dev);
    mental_reference_pin(ref_b, dev);
    mental_reference_pin(ref_c, dev);

    mental_reference inputs[2] = {ref_a, ref_b};
    mental_reference out_refs[1] = {ref_c};
    int rc = mental_dispatch(kernel, inputs, 2, out_refs, 1, N);
    if (rc != 0) {
        fail("dispatch", mental_get_error_message());
        mental_reference_close(ref_a);
        mental_reference_close(ref_b);
        mental_reference_close(ref_c);
        return;
    }

    mental_reference_read(ref_c, c, size);

    int ok = 1;
    for (int i = 0; i < N; i++) {
        float expected = (float)i + (float)(i * 10);
        if (fabsf(c[i] - expected) > 0.01f) {
            char msg[128];
            snprintf(msg, sizeof(msg), "c[%d] = %.1f, expected %.1f", i, c[i], expected);
            fail("dispatch", msg);
            ok = 0;
            break;
        }
    }
    if (ok) pass("dispatch");

    mental_reference_close(ref_a);
    mental_reference_close(ref_b);
    mental_reference_close(ref_c);
    #undef N
}

static void check_pipe(mental_device dev) {
    #define N 256
    float input[N], result[N];
    for (int i = 0; i < N; i++) {
        input[i] = (float)(i + 1);
    }

    size_t size = N * sizeof(float);
    mental_reference ref_a = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_b = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_c = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

    if (!ref_a || !ref_b || !ref_c) {
        fail("pipe", "reference creation failed");
        if (ref_a) mental_reference_close(ref_a);
        if (ref_b) mental_reference_close(ref_b);
        if (ref_c) mental_reference_close(ref_c);
        return;
    }

    mental_reference_write(ref_a, input, size);
    mental_reference_pin(ref_a, dev);
    mental_reference_pin(ref_b, dev);
    mental_reference_pin(ref_c, dev);

    /* Compile two kernels: add (a+a) then scale (*3) */
    mental_kernel k_add = mental_compile(dev, add_shader, strlen(add_shader));
    mental_kernel k_scale = mental_compile(dev, scale_shader, strlen(scale_shader));
    if (!k_add || !k_scale) {
        fail("pipe", "kernel compilation failed");
        if (k_add) mental_kernel_finalize(k_add);
        if (k_scale) mental_kernel_finalize(k_scale);
        mental_reference_close(ref_a);
        mental_reference_close(ref_b);
        mental_reference_close(ref_c);
        return;
    }

    mental_pipe pipe = mental_pipe_create(dev);
    if (!pipe) {
        fail("pipe", "pipe creation failed");
        mental_kernel_finalize(k_add);
        mental_kernel_finalize(k_scale);
        mental_reference_close(ref_a);
        mental_reference_close(ref_b);
        mental_reference_close(ref_c);
        return;
    }

    /* Stage 1: B = A + A (doubling) */
    mental_reference add_inputs[2] = {ref_a, ref_a};
    mental_reference add_outputs[1] = {ref_b};
    mental_pipe_add(pipe, k_add, add_inputs, 2, add_outputs, 1, N);

    /* Stage 2: C = B * 3 */
    mental_reference scale_inputs[1] = {ref_b};
    mental_reference scale_outputs[1] = {ref_c};
    mental_pipe_add(pipe, k_scale, scale_inputs, 1, scale_outputs, 1, N);

    if (mental_pipe_execute(pipe) != 0) {
        fail("pipe", mental_get_error_message());
        mental_pipe_finalize(pipe);
        mental_kernel_finalize(k_add);
        mental_kernel_finalize(k_scale);
        mental_reference_close(ref_a);
        mental_reference_close(ref_b);
        mental_reference_close(ref_c);
        return;
    }

    mental_reference_read(ref_c, result, size);

    /* Verify: C[i] = (input[i] + input[i]) * 3 = input[i] * 6 */
    int ok = 1;
    for (int i = 0; i < N; i++) {
        float expected = input[i] * 6.0f;
        if (fabsf(result[i] - expected) > 0.01f) {
            char msg[128];
            snprintf(msg, sizeof(msg), "result[%d] = %.1f, expected %.1f", i, result[i], expected);
            fail("pipe", msg);
            ok = 0;
            break;
        }
    }
    if (ok) pass("pipe");

    mental_pipe_finalize(pipe);
    mental_kernel_finalize(k_add);
    mental_kernel_finalize(k_scale);
    mental_reference_close(ref_a);
    mental_reference_close(ref_b);
    mental_reference_close(ref_c);
    #undef N
}

/* ── Public API ──────────────────────────────────────────────────── */

int mental_sanity_check(void) {
    g_passed = 0;
    g_failed = 0;
    g_skipped = 0;

    printf("mental sanity check\n");
    printf("===================\n\n");

    /* CPU-only checks (always run) */
    check_state();
    int device_count = 0;
    check_devices(&device_count);
    check_reference_lifecycle();
    check_disclosure();

    /* GPU checks (skipped if no devices) */
    if (device_count > 0) {
        mental_device dev = mental_device_get(0);
        printf("\n  Device: %s (%s)\n\n", mental_device_name(dev), mental_device_api_name(dev));

        check_gpu_buffer(dev);

        mental_kernel kernel = NULL;
        check_compile(dev, &kernel);

        if (kernel) {
            check_dispatch(dev, kernel);
            mental_kernel_finalize(kernel);
        } else {
            skip("dispatch", "shader compilation failed");
        }

        check_pipe(dev);
    } else {
        skip("GPU buffer", "no devices");
        skip("shader compile", "no devices");
        skip("dispatch", "no devices");
        skip("pipe", "no devices");
    }

    /* Summary */
    printf("\n---\n");
    printf("%d passed, %d failed, %d skipped\n",
           g_passed, g_failed, g_skipped);

    if (g_failed > 0) {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nOK\n");
    return 0;
}
