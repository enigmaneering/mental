/*
 * Mental - Sanity Check Binary
 *
 * Standalone executable that runs the built-in self-test suite.
 * Ship alongside the library to verify it works on any target machine.
 *
 * Usage: ./sanity-check
 * Exit code: 0 = all checks passed, 1 = one or more checks failed
 */

#include "mental.h"

int main(void) {
    return mental_sanity_check();
}
