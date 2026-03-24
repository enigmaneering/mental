/*
 * Mental - Monotonic Counter
 *
 * Lock-free, strictly increasing 64-bit counter.
 * Starts at 1 (zero is reserved as "no value").
 */

#include "mental.h"

#ifdef _MSC_VER
#include <intrin.h>
static volatile long long g_counter = 0;

uint64_t mental_monotonic(void) {
    return (uint64_t)_InterlockedIncrement64(&g_counter);
}
#else
#include <stdatomic.h>
static _Atomic uint64_t g_counter = 0;

uint64_t mental_monotonic(void) {
    return atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed) + 1;
}
#endif
