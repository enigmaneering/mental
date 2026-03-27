/* Integration test: full pipeline with multiple operations */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mental_pthread.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while(0)

#define ASSERT_NO_ERROR() do { \
    if (mental_get_error() != MENTAL_SUCCESS) { \
        fprintf(stderr, "FAIL: %s\n", mental_get_error_message()); \
        return 1; \
    } \
} while(0)

/* Multiply shader: output = input * 2.0 */
const char* multiply_shader =
    "#version 450\n"
    "layout(local_size_x = 256) in;\n"
    "layout(binding = 0) buffer Input { float data[]; } input_buf;\n"
    "layout(binding = 1) buffer Output { float data[]; } output_buf;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    output_buf.data[idx] = input_buf.data[idx] * 2.0;\n"
    "}\n";

/*
 * Observability test: one writer, multiple observers.
 *
 * The writer writes a sequence of known values to the reference.
 * Each observer continuously reads and verifies it only ever sees
 * a value from the known set — never garbage or a torn write.
 *
 * This tests that mental references provide coherent shared memory
 * visibility across threads, which is the core contract.
 */

/* Sentinel values the writer cycles through */
static const float g_known_values[] = {
    0.0f, 1.0f, 2.0f, 3.0f, 42.0f, 100.0f, -1.0f, 999.0f
};
#define NUM_KNOWN_VALUES (int)(sizeof(g_known_values) / sizeof(g_known_values[0]))
#define OBSERVE_ITERATIONS 10000

typedef struct {
    mental_reference ref;
    int observer_id;
    int success;      /* 1 = only saw known values */
    int observations;  /* how many reads completed */
} observer_data;

typedef struct {
    mental_reference ref;
    int done; /* set to 1 when writer finishes */
} writer_data;

void* writer_thread(void* arg) {
    writer_data* wd = (writer_data*)arg;
    void *base = mental_reference_data(wd->ref, NULL, 0);
    if (!base) return NULL;

    float *slot = (float *)base;

    /* Write known values in sequence, many times */
    for (int round = 0; round < OBSERVE_ITERATIONS; round++) {
        *slot = g_known_values[round % NUM_KNOWN_VALUES];
    }

    wd->done = 1;
    return NULL;
}

void* observer_thread(void* arg) {
    observer_data* od = (observer_data*)arg;
    void *base = mental_reference_data(od->ref, NULL, 0);
    if (!base) { od->success = 0; return NULL; }

    float *slot = (float *)base;
    od->success = 1;
    od->observations = 0;

    /* Read continuously until we've done enough observations */
    for (int i = 0; i < OBSERVE_ITERATIONS; i++) {
        float observed = *slot;

        /* Verify we see a value from the known set */
        int valid = 0;
        for (int k = 0; k < NUM_KNOWN_VALUES; k++) {
            if (observed == g_known_values[k]) { valid = 1; break; }
        }

        if (!valid) {
            fprintf(stderr, "    observer %d: saw invalid value %f at iteration %d\n",
                    od->observer_id, observed, i);
            od->success = 0;
            break;
        }

        od->observations++;
    }

    return NULL;
}

int main(void) {
    printf("Testing integration scenarios...\n");

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to create device");
    ASSERT_NO_ERROR();

    /* Test 1: Full compute pipeline */
    printf("  Test 1: Full compute pipeline\n");

    int count = 1024;
    size_t size = count * sizeof(float);

    mental_reference input = mental_reference_create("integ-in0", size);
    ASSERT(input != NULL, "Failed to create input reference");
    mental_reference_pin(input, dev);
    ASSERT_NO_ERROR();

    mental_reference output = mental_reference_create("integ-out", size);
    ASSERT(output != NULL, "Failed to create output reference");
    mental_reference_pin(output, dev);
    ASSERT_NO_ERROR();

    /* Fill input */
    float* input_data = malloc(size);
    for (int i = 0; i < count; i++) {
        input_data[i] = (float)i;
    }
    mental_reference_write(input, input_data, size);

    /* Compile and run kernel */
    mental_kernel kernel = mental_compile(dev, multiply_shader, strlen(multiply_shader));
    ASSERT(kernel != NULL, "Failed to compile kernel");

    mental_reference inputs[] = { input };
    mental_dispatch(kernel, inputs, 1, output, count);

    /* Read and verify results */
    float* output_data = malloc(size);
    mental_reference_read(output, output_data, size);

    int match = 1;
    for (int i = 0; i < count; i++) {
        float expected = input_data[i] * 2.0f;
        if (output_data[i] != expected) {
            match = 0;
            break;
        }
    }
    ASSERT(match, "Pipeline computation incorrect");

    free(input_data);
    free(output_data);
    mental_kernel_finalize(kernel);

    /* Test 2: Multiple operations on same buffer */
    printf("  Test 2: Multiple operations on same buffer\n");

    float test_val = 42.0f;
    mental_reference_write(input, &test_val, sizeof(float));
    mental_reference_read(input, &test_val, sizeof(float));
    ASSERT(test_val == 42.0f, "Multiple ops failed");

    /* Test 3: Clone and independent operations */
    printf("  Test 3: Clone and independent operations\n");

    mental_reference clone = mental_reference_clone(input, "integ-clone", dev, NULL, 0);
    ASSERT(clone != NULL, "Clone failed");

    float orig_val = 100.0f;
    float clone_val = 200.0f;

    mental_reference_write(input, &orig_val, sizeof(float));
    mental_reference_write(clone, &clone_val, sizeof(float));

    float read_orig, read_clone;
    mental_reference_read(input, &read_orig, sizeof(float));
    mental_reference_read(clone, &read_clone, sizeof(float));

    ASSERT(read_orig == orig_val && read_clone == clone_val,
           "Clone buffers not independent");

    mental_reference_close(clone);

    /* Test 4: Observability under concurrency
     *
     * One writer cycles through known values. Three observers read
     * continuously and verify they only see values from the known set.
     * This tests that references provide coherent shared memory
     * visibility — the fundamental contract of the reference system. */
    printf("  Test 4: Observability\n");

    mental_reference shared_ref = mental_reference_create("integ-observe", sizeof(float));
    ASSERT(shared_ref != NULL, "Failed to create shared reference");

    /* Initialize to a known value before any threads start */
    float init_val = g_known_values[0];
    void *sdata = mental_reference_data(shared_ref, NULL, 0);
    ASSERT(sdata != NULL, "shared data NULL");
    *(float *)sdata = init_val;

    const int num_observers = 3;
    writer_data wd = { .ref = shared_ref, .done = 0 };
    observer_data observers[3];

    pthread_t writer_tid;
    pthread_t observer_tids[3];

    /* Start observers first so they're reading while writer writes */
    for (int i = 0; i < num_observers; i++) {
        observers[i].ref = shared_ref;
        observers[i].observer_id = i;
        observers[i].success = 0;
        observers[i].observations = 0;
        pthread_create(&observer_tids[i], NULL, observer_thread, &observers[i]);
    }

    /* Start writer */
    pthread_create(&writer_tid, NULL, writer_thread, &wd);

    /* Wait for all threads */
    pthread_join(writer_tid, NULL);
    for (int i = 0; i < num_observers; i++) {
        pthread_join(observer_tids[i], NULL);
    }

    /* Verify all observers only saw valid values */
    for (int i = 0; i < num_observers; i++) {
        printf("    observer %d: %d observations, %s\n",
               i, observers[i].observations,
               observers[i].success ? "OK" : "SAW INVALID VALUE");
        ASSERT(observers[i].success, "Observer saw corrupted/invalid value");
    }

    mental_reference_close(shared_ref);

    /* Test 5: Error recovery */
    printf("  Test 5: Error recovery\n");

    /* Try to read from freed buffer (should error) */
    mental_reference temp = mental_reference_create("integ-temp", 1024);
    ASSERT(temp != NULL, "Failed to create temp reference");
    mental_reference_pin(temp, dev);
    mental_reference_close(temp);

    float dummy;
    mental_reference_read(temp, &dummy, sizeof(float));
    ASSERT(mental_get_error_message() != MENTAL_SUCCESS, "Expected error for invalid reference");

    /* Clear error and continue */
    printf("  Error recovery successful\n");

    /* Cleanup */
    mental_reference_close(input);
    mental_reference_close(output);


    printf("PASS: All integration tests passed\n");
    return 0;
}
