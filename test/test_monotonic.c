/* count + counter tests: basic increment, thread safety, counter ops */
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

static void *count_thread(void *arg) {
    uint64_t *results = (uint64_t *)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        results[i] = mental_count();
    }
    return NULL;
}

#endif /* _WIN32 */

int main(void) {
    /* ── mental_count (global monotonic) ────────────────────────── */
    printf("Testing mental_count...\n");

    uint64_t a = mental_count();
    uint64_t b = mental_count();
    uint64_t c = mental_count();
    printf("  first three values: %llu, %llu, %llu\n",
           (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    ASSERT(a >= 1, "first value must be >= 1");
    ASSERT(b == a + 1, "must be strictly increasing");
    ASSERT(c == b + 1, "must be strictly increasing");

#ifndef _WIN32
    /* Thread safety: all values across all threads must be unique. */
    uint64_t all[N_THREADS][PER_THREAD];
    pthread_t tids[N_THREADS];

    for (int t = 0; t < N_THREADS; t++) {
        ASSERT(pthread_create(&tids[t], NULL, count_thread, all[t]) == 0,
               "Failed to create thread");
    }
    for (int t = 0; t < N_THREADS; t++) {
        pthread_join(tids[t], NULL);
    }

    for (int t = 0; t < N_THREADS; t++) {
        for (int i = 1; i < PER_THREAD; i++) {
            ASSERT(all[t][i] > all[t][i-1],
                   "per-thread sequence must be strictly increasing");
        }
    }

    uint64_t flat[N_THREADS * PER_THREAD];
    for (int t = 0; t < N_THREADS; t++) {
        memcpy(&flat[t * PER_THREAD], all[t], PER_THREAD * sizeof(uint64_t));
    }

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

    printf("PASS: mental_count\n\n");

    /* ── mental_counter (instance counter) ──────────────────────── */
    printf("Testing mental_counter...\n");

    mental_counter ctr = mental_counter_create();
    ASSERT(ctr != NULL, "counter creation must succeed");

    /* Increment by various deltas */
    uint64_t v;
    v = mental_counter_increment(ctr, 1);
    ASSERT(v == 1, "increment(1) from 0 should return 1");

    v = mental_counter_increment(ctr, 5);
    ASSERT(v == 6, "increment(5) from 1 should return 6");

    v = mental_counter_increment(ctr, 100);
    ASSERT(v == 106, "increment(100) from 6 should return 106");
    printf("  increment: 0 +1=1, +5=6, +100=106  OK\n");

    /* Decrement */
    v = mental_counter_decrement(ctr, 6);
    ASSERT(v == 100, "decrement(6) from 106 should return 100");

    v = mental_counter_decrement(ctr, 50);
    ASSERT(v == 50, "decrement(50) from 100 should return 50");
    printf("  decrement: 106 -6=100, -50=50  OK\n");

    /* Decrement saturation at 0 */
    v = mental_counter_decrement(ctr, 9999);
    ASSERT(v == 0, "decrement past 0 should saturate at 0");
    printf("  saturating decrement: 50 -9999=0  OK\n");

    /* Reset */
    mental_counter_increment(ctr, 42);
    v = mental_counter_reset(ctr);
    ASSERT(v == 42, "reset should return previous value");

    v = mental_counter_increment(ctr, 1);
    ASSERT(v == 1, "after reset, increment(1) should return 1");
    printf("  reset: set to 42, reset returns 42, +1=1  OK\n");

    mental_counter_finalize(ctr);

    printf("PASS: mental_counter\n");
    return 0;
}
