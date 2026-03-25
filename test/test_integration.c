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

/* Thread-safety test data */
typedef struct {
    mental_reference ref;
    int thread_id;
    int success;
} thread_test_data;

void* thread_worker(void* arg) {
    thread_test_data* data = (thread_test_data*)arg;

    /* Each thread writes and reads from the same buffer */
    float value = (float)data->thread_id;
    mental_reference_write(data->ref, &value, sizeof(float));

    float read_value;
    mental_reference_read(data->ref, &read_value, sizeof(float));

    /* If locking works, we should read back what we wrote */
    data->success = (read_value == value);

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

    /* Test 4: Thread safety */
    printf("  Test 4: Thread safety\n");

    const int num_threads = 4;
    pthread_t threads[num_threads];
    thread_test_data thread_data[num_threads];

    mental_reference shared_ref = mental_reference_create("integ-thread-shared", sizeof(float));
    ASSERT(shared_ref != NULL, "Failed to create shared reference");
    mental_reference_pin(shared_ref, dev);
    ASSERT_NO_ERROR();

    for (int i = 0; i < num_threads; i++) {
        thread_data[i].ref = shared_ref;
        thread_data[i].thread_id = i;
        thread_data[i].success = 0;
        pthread_create(&threads[i], NULL, thread_worker, &thread_data[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        ASSERT(thread_data[i].success, "Thread safety test failed");
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
