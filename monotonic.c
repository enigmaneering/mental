/*
 * Mental - Count & Counter
 *
 * mental_count:   Global, lock-free, monotonic uint64 (1, 2, 3, …).
 * mental_counter: Heap-allocated, lock-free, atomic uint64 instance
 *                 with an "empty" state distinct from zero.
 */

#include "mental.h"
#include <stdlib.h>

/* The empty sentinel — UINT64_MAX is reserved and never a valid count. */
#define EMPTY UINT64_MAX

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
    mental_counter ctr = malloc(sizeof(*ctr));
    if (ctr) {
#ifdef _MSC_VER
        ctr->value = (long long)EMPTY;
#else
        atomic_init(&ctr->value, EMPTY);
#endif
    }
    return ctr;
}

#ifdef _MSC_VER

uint64_t mental_counter_increment(mental_counter ctr, uint64_t delta) {
    for (;;) {
        long long cur = ctr->value;
        uint64_t base = ((uint64_t)cur == EMPTY) ? 0 : (uint64_t)cur;
        long long next = (long long)(base + delta);
        if (_InterlockedCompareExchange64(&ctr->value, next, cur) == cur)
            return (uint64_t)next;
    }
}

uint64_t mental_counter_decrement(mental_counter ctr, uint64_t delta) {
    for (;;) {
        long long cur = ctr->value;
        if ((uint64_t)cur == EMPTY) {
            return 0; /* already empty, stay empty */
        }
        long long next;
        if ((uint64_t)cur < delta)
            next = (long long)EMPTY;
        else
            next = cur - (long long)delta;
        if (_InterlockedCompareExchange64(&ctr->value, next, cur) == cur)
            return ((uint64_t)next == EMPTY) ? 0 : (uint64_t)next;
    }
}

int mental_counter_empty(mental_counter ctr) {
    return ((uint64_t)ctr->value == EMPTY) ? 1 : 0;
}

uint64_t mental_counter_reset(mental_counter ctr, int to_empty) {
    long long target = to_empty ? (long long)EMPTY : 0;
    long long prev = _InterlockedExchange64(&ctr->value, target);
    return ((uint64_t)prev == EMPTY) ? 0 : (uint64_t)prev;
}

#else /* C11 atomics */

uint64_t mental_counter_increment(mental_counter ctr, uint64_t delta) {
    uint64_t cur = atomic_load_explicit(&ctr->value, memory_order_relaxed);
    for (;;) {
        uint64_t base = (cur == EMPTY) ? 0 : cur;
        uint64_t next = base + delta;
        if (atomic_compare_exchange_weak_explicit(
                &ctr->value, &cur, next,
                memory_order_relaxed, memory_order_relaxed))
            return next;
    }
}

uint64_t mental_counter_decrement(mental_counter ctr, uint64_t delta) {
    uint64_t cur = atomic_load_explicit(&ctr->value, memory_order_relaxed);
    for (;;) {
        if (cur == EMPTY)
            return 0; /* already empty, stay empty */
        uint64_t next = (cur < delta) ? EMPTY : cur - delta;
        if (atomic_compare_exchange_weak_explicit(
                &ctr->value, &cur, next,
                memory_order_relaxed, memory_order_relaxed))
            return (next == EMPTY) ? 0 : next;
    }
}

int mental_counter_empty(mental_counter ctr) {
    return (atomic_load_explicit(&ctr->value, memory_order_relaxed) == EMPTY) ? 1 : 0;
}

uint64_t mental_counter_reset(mental_counter ctr, int to_empty) {
    uint64_t target = to_empty ? EMPTY : 0;
    uint64_t prev = atomic_exchange_explicit(&ctr->value, target, memory_order_relaxed);
    return (prev == EMPTY) ? 0 : prev;
}

#endif

void mental_counter_finalize(mental_counter ctr) {
    free(ctr);
}
