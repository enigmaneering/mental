/*
 * Mental - Count & Counter
 *
 * mental_count:   Global, lock-free, monotonic uint64 (1, 2, 3, …).
 * mental_counter: Heap-allocated, lock-free, atomic uint64 instance.
 */

#include "mental.h"
#include <stdlib.h>

/* ── Global monotonic count ─────────────────────────────────────── */

#ifdef _MSC_VER
#include <intrin.h>
static volatile long long g_count = 0;

uint64_t mental_count(void) {
    return (uint64_t)_InterlockedIncrement64(&g_count);
}
#else
#include <stdatomic.h>
static _Atomic uint64_t g_count = 0;

uint64_t mental_count(void) {
    return atomic_fetch_add_explicit(&g_count, 1, memory_order_relaxed) + 1;
}
#endif

/* ── Counter instances ──────────────────────────────────────────── */

struct mental_counter_t {
#ifdef _MSC_VER
    volatile long long value;
#else
    _Atomic uint64_t value;
#endif
};

mental_counter mental_counter_create(void) {
    mental_counter ctr = calloc(1, sizeof(*ctr));
    return ctr; /* NULL on failure */
}

#ifdef _MSC_VER

uint64_t mental_counter_increment(mental_counter ctr, uint64_t delta) {
    return (uint64_t)_InterlockedExchangeAdd64(&ctr->value, (long long)delta) + delta;
}

uint64_t mental_counter_decrement(mental_counter ctr, uint64_t delta) {
    /* CAS loop to saturate at 0 */
    for (;;) {
        long long cur = ctr->value;
        long long next = (uint64_t)cur >= delta ? cur - (long long)delta : 0;
        if (_InterlockedCompareExchange64(&ctr->value, next, cur) == cur)
            return (uint64_t)next;
    }
}

uint64_t mental_counter_reset(mental_counter ctr) {
    return (uint64_t)_InterlockedExchange64(&ctr->value, 0);
}

#else /* C11 atomics */

uint64_t mental_counter_increment(mental_counter ctr, uint64_t delta) {
    return atomic_fetch_add_explicit(&ctr->value, delta, memory_order_relaxed) + delta;
}

uint64_t mental_counter_decrement(mental_counter ctr, uint64_t delta) {
    /* CAS loop to saturate at 0 */
    uint64_t cur = atomic_load_explicit(&ctr->value, memory_order_relaxed);
    for (;;) {
        uint64_t next = cur >= delta ? cur - delta : 0;
        if (atomic_compare_exchange_weak_explicit(
                &ctr->value, &cur, next,
                memory_order_relaxed, memory_order_relaxed))
            return next;
    }
}

uint64_t mental_counter_reset(mental_counter ctr) {
    return atomic_exchange_explicit(&ctr->value, 0, memory_order_relaxed);
}

#endif

void mental_counter_finalize(mental_counter ctr) {
    free(ctr);
}
