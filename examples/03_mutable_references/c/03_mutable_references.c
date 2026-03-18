#include "../../../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to convert float to uint32 representation
uint32_t float_to_bits(float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    return conv.u;
}

// Helper to convert uint32 to float
float bits_to_float(uint32_t bits) {
    union { float f; uint32_t u; } conv;
    conv.u = bits;
    return conv.f;
}

// Helper to write float32 in little-endian
void write_float(uint8_t* buf, size_t offset, float value) {
    uint32_t bits = float_to_bits(value);
    buf[offset + 0] = (bits >> 0) & 0xFF;
    buf[offset + 1] = (bits >> 8) & 0xFF;
    buf[offset + 2] = (bits >> 16) & 0xFF;
    buf[offset + 3] = (bits >> 24) & 0xFF;
}

// Helper to read float32 in little-endian
float read_float(uint8_t* buf, size_t offset) {
    uint32_t bits = buf[offset + 0] |
                   (buf[offset + 1] << 8) |
                   (buf[offset + 2] << 16) |
                   (buf[offset + 3] << 24);
    return bits_to_float(bits);
}

// Helper to print float buffer
void print_float_buffer(const char* label, MentalReference ref) {
    size_t size = mental_reference_size(ref);
    uint8_t* data = malloc(size);
    mental_reference_read(ref, data, size);

    size_t count = size / 4;
    printf("%s (%zu bytes, %zu floats): [", label, size, count);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) printf(", ");
        printf("%.1f", read_float(data, i * 4));
    }
    printf("]\n");

    free(data);
}

int main() {
    printf("=== Mutable References Example (C) ===\n\n");
    printf("This example demonstrates manual GPU buffer resizing.\n");
    printf("In C, you explicitly create new buffers when you need different sizes,\n");
    printf("while in Go, the system handles this automatically.\n\n");

    // Initialize Mental
    mental_init();

    // List available devices
    MentalDeviceInfo device_info;
    mental_get_device_info(0, &device_info);

    const char* api_name = "Unknown";
    switch (device_info.api) {
        case MENTAL_API_METAL: api_name = "Metal"; break;
        case MENTAL_API_VULKAN: api_name = "Vulkan"; break;
        case MENTAL_API_D3D12: api_name = "D3D12"; break;
        case MENTAL_API_OPENCL: api_name = "OpenCL"; break;
    }

    printf("Using device: %s (%s)\n\n", device_info.name, api_name);
    mental_free_device_info(&device_info);

    // Example 1: Create initial buffer with 3 numbers
    printf("Creating initial buffer with 3 numbers...\n");
    MentalReference numbers = mental_create_reference(12, 0); // 3 float32s = 12 bytes
    uint8_t* data = malloc(12);
    write_float(data, 0, 1.0f);
    write_float(data, 4, 2.0f);
    write_float(data, 8, 3.0f);
    mental_reference_write(numbers, data, 12);
    free(data);
    print_float_buffer("Initial buffer", numbers);

    // Example 2: "Grow" by creating new buffer and copying data
    printf("\nExample 2: Growing buffer (adding 2 more numbers)...\n");
    MentalReference numbers_grown = mental_create_reference(20, 0); // 5 float32s
    data = malloc(20);

    // Copy old data
    uint8_t* old_data = malloc(12);
    mental_reference_read(numbers, old_data, 12);
    memcpy(data, old_data, 12);
    free(old_data);

    // Add new values
    write_float(data, 12, 4.0f);
    write_float(data, 16, 5.0f);
    mental_reference_write(numbers_grown, data, 20);
    free(data);

    // Release old buffer and use new one
    mental_release_reference(numbers);
    numbers = numbers_grown;
    print_float_buffer("After growing", numbers);

    // Example 3: Transform data (square each number and add 0)
    printf("\nExample 3: Transform (square each and add 0)...\n");
    MentalReference numbers_transformed = mental_create_reference(24, 0); // 6 float32s
    data = malloc(24);

    // Read, transform, and write
    old_data = malloc(20);
    mental_reference_read(numbers, old_data, 20);

    for (int i = 0; i < 5; i++) {
        float val = read_float(old_data, i * 4);
        write_float(data, i * 4, val * val);
    }
    write_float(data, 20, 0.0f); // Add zero at end

    mental_reference_write(numbers_transformed, data, 24);
    free(old_data);
    free(data);

    mental_release_reference(numbers);
    numbers = numbers_transformed;
    print_float_buffer("After transformation", numbers);

    // Example 4: Shrink by filtering out zeros
    printf("\nExample 4: Shrinking buffer (removing zeros)...\n");
    old_data = malloc(24);
    mental_reference_read(numbers, old_data, 24);

    // Count non-zeros
    int non_zero_count = 0;
    for (int i = 0; i < 6; i++) {
        if (read_float(old_data, i * 4) != 0.0f) {
            non_zero_count++;
        }
    }

    // Create smaller buffer
    MentalReference numbers_filtered = mental_create_reference(non_zero_count * 4, 0);
    data = malloc(non_zero_count * 4);

    int j = 0;
    for (int i = 0; i < 6; i++) {
        float val = read_float(old_data, i * 4);
        if (val != 0.0f) {
            write_float(data, j * 4, val);
            j++;
        }
    }

    mental_reference_write(numbers_filtered, data, non_zero_count * 4);
    free(old_data);
    free(data);

    mental_release_reference(numbers);
    numbers = numbers_filtered;
    print_float_buffer("After filtering", numbers);

    // Example 5: Build Fibonacci sequence
    printf("\nExample 5: Building Fibonacci sequence dynamically...\n");
    MentalReference fib = mental_create_reference(8, 0); // Start with 2 numbers
    data = malloc(8);
    write_float(data, 0, 1.0f);
    write_float(data, 4, 1.0f);
    mental_reference_write(fib, data, 8);
    free(data);

    // Generate next 8 Fibonacci numbers
    for (int i = 0; i < 8; i++) {
        size_t current_size = mental_reference_size(fib);
        size_t count = current_size / 4;

        // Read current data
        old_data = malloc(current_size);
        mental_reference_read(fib, old_data, current_size);

        // Get last two numbers
        float a = read_float(old_data, (count - 2) * 4);
        float b = read_float(old_data, (count - 1) * 4);
        float next = a + b;

        // Create new buffer with one more slot
        size_t new_size = current_size + 4;
        MentalReference fib_new = mental_create_reference(new_size, 0);
        data = malloc(new_size);

        // Copy old data and append new
        memcpy(data, old_data, current_size);
        write_float(data, current_size, next);

        mental_reference_write(fib_new, data, new_size);

        free(old_data);
        free(data);
        mental_release_reference(fib);
        fib = fib_new;
    }

    print_float_buffer("Fibonacci sequence", fib);

    printf("\n=== Complete ===\n\n");
    printf("Key takeaway: In C, you manually create new buffers for resizing.\n");
    printf("In Go, the system does this automatically via Mutate().\n");
    printf("Both approaches give you full control over GPU memory!\n");

    // Cleanup
    mental_release_reference(numbers);
    mental_release_reference(fib);

    return 0;
}
