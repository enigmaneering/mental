#include "../../../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

// Shared reference and mutex
typedef struct {
    MentalReference ref;
    pthread_mutex_t mutex;
} SharedData;

// Helper to sum byte array
int sum_bytes(uint8_t* data, size_t size) {
    int s = 0;
    for (size_t i = 0; i < size; i++) {
        s += data[i];
    }
    return s;
}

// Observer 1: Reads every 100ms
void* observer1_thread(void* arg) {
    SharedData* shared = (SharedData*)arg;

    for (int i = 0; i < 5; i++) {
        usleep(100000); // 100ms

        uint8_t* data = malloc(64);
        pthread_mutex_lock(&shared->mutex);
        mental_reference_read(shared->ref, data, 64);
        pthread_mutex_unlock(&shared->mutex);

        printf("Observer 1 sees: [%d, %d, %d, %d, %d, %d, %d, %d] (sum=%d)\n",
               data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
               sum_bytes(data, 64));
        free(data);
    }

    return NULL;
}

// Observer 2: Reads every 150ms
void* observer2_thread(void* arg) {
    SharedData* shared = (SharedData*)arg;

    for (int i = 0; i < 3; i++) {
        usleep(150000); // 150ms

        uint8_t* data = malloc(64);
        pthread_mutex_lock(&shared->mutex);
        mental_reference_read(shared->ref, data, 64);
        pthread_mutex_unlock(&shared->mutex);

        printf("Observer 2 sees: [%d, %d, %d, %d, %d, %d, %d, %d] (sum=%d)\n",
               data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
               sum_bytes(data, 64));
        free(data);
    }

    return NULL;
}

// Mutator: Increments all values every 50ms
void* mutator_thread(void* arg) {
    SharedData* shared = (SharedData*)arg;

    for (int i = 0; i < 10; i++) {
        usleep(50000); // 50ms

        uint8_t* data = malloc(64);

        pthread_mutex_lock(&shared->mutex);
        // Read current state
        mental_reference_read(shared->ref, data, 64);

        // Increment all values
        for (int j = 0; j < 64; j++) {
            data[j]++;
        }

        // Write back
        mental_reference_write(shared->ref, data, 64);
        pthread_mutex_unlock(&shared->mutex);

        printf("Mutator: incremented all values\n");
        free(data);
    }

    return NULL;
}

int main() {
    printf("=== Observable References Example (C) ===\n\n");

    // Initialize Mental
    mental_init();

    // Allocate shared GPU buffer
    MentalReference ref = mental_create_reference(64, 0);
    if (!ref) {
        fprintf(stderr, "Failed to allocate GPU buffer\n");
        return 1;
    }

    // Initialize with zeros
    uint8_t* zeros = calloc(64, 1);
    mental_reference_write(ref, zeros, 64);
    free(zeros);

    // Setup shared data
    SharedData shared;
    shared.ref = ref;
    pthread_mutex_init(&shared.mutex, NULL);

    // Create threads
    pthread_t observer1, observer2, mutator;

    pthread_create(&observer1, NULL, observer1_thread, &shared);
    pthread_create(&observer2, NULL, observer2_thread, &shared);
    pthread_create(&mutator, NULL, mutator_thread, &shared);

    // Wait for all threads
    pthread_join(observer1, NULL);
    pthread_join(observer2, NULL);
    pthread_join(mutator, NULL);

    // Final observation
    uint8_t* final_data = malloc(64);
    mental_reference_read(ref, final_data, 64);
    printf("\nFinal state: [%d, %d, %d, %d, %d, %d, %d, %d] (sum=%d)\n",
           final_data[0], final_data[1], final_data[2], final_data[3],
           final_data[4], final_data[5], final_data[6], final_data[7],
           sum_bytes(final_data, 64));
    free(final_data);

    printf("\n=== Complete ===\n\n");
    printf("Note: Observers saw different states at different times.\n");
    printf("Between observations, the data changed multiple times.\n");
    printf("This is the 'irrational reference' pattern - no history tracking,\n");
    printf("only current observable state.\n");

    // Cleanup
    pthread_mutex_destroy(&shared.mutex);
    mental_release_reference(ref);

    return 0;
}
