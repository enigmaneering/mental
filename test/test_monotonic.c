/* monotonic counter test: basic increment + thread safety */
#include "../mental.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while(0)

#define N_THREADS  4
#define PER_THREAD 10000

#ifndef _WIN32

static void *counter_thread(void *arg) {
    uint64_t *results = (uint64_t *)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        results[i] = mental_monotonic();
    }
    return NULL;
}

#endif /* _WIN32 */

int main(void) {
    printf("Testing monotonic counter...\n");

    /* Basic: first call returns 1, strictly increasing */
    uint64_t a = mental_monotonic();
    uint64_t b = mental_monotonic();
    uint64_t c = mental_monotonic();
    printf("  first three values: %llu, %llu, %llu\n",
           (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    ASSERT(a >= 1, "first value must be >= 1");
    ASSERT(b == a + 1, "must be strictly increasing");
    ASSERT(c == b + 1, "must be strictly increasing");

#ifndef _WIN32
    /* Thread safety: N_THREADS threads each grab PER_THREAD values.
     * All values across all threads must be unique. */
    uint64_t all[N_THREADS][PER_THREAD];
    pthread_t tids[N_THREADS];

    for (int t = 0; t < N_THREADS; t++) {
        ASSERT(pthread_create(&tids[t], NULL, counter_thread, all[t]) == 0,
               "Failed to create thread");
    }
    for (int t = 0; t < N_THREADS; t++) {
        pthread_join(tids[t], NULL);
    }

    /* Each thread's own sequence must be strictly increasing */
    for (int t = 0; t < N_THREADS; t++) {
        for (int i = 1; i < PER_THREAD; i++) {
            ASSERT(all[t][i] > all[t][i-1],
                   "per-thread sequence must be strictly increasing");
        }
    }

    /* Collect all values and verify uniqueness via sorting */
    uint64_t flat[N_THREADS * PER_THREAD];
    for (int t = 0; t < N_THREADS; t++) {
        memcpy(&flat[t * PER_THREAD], all[t], PER_THREAD * sizeof(uint64_t));
    }

    /* Simple shell sort — good enough for 40k elements in a test */
    size_t n = N_THREADS * PER_THREAD;
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            uint64_t tmp = flat[i];
            size_t j = i;
            while (j >= gap && flat[j - gap] > tmp) {
                flat[j] = flat[j - gap];
                j -= gap;
            }
            flat[j] = tmp;
        }
    }

    for (size_t i = 1; i < n; i++) {
        ASSERT(flat[i] != flat[i-1], "all values must be unique across threads");
    }

    printf("  %d threads x %d calls: all %d values unique\n",
           N_THREADS, PER_THREAD, N_THREADS * PER_THREAD);
#else
    printf("  (thread test skipped on Windows)\n");
#endif

    printf("PASS: monotonic\n");
    return 0;
}
