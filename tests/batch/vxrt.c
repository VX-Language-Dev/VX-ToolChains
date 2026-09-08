/* VX batch-test runtime support.
 * `out` is a VX builtin (wraps puts, adds a newline) provided by the
 * translator; do NOT redefine it here (duplicate symbol).
 * These helpers give newline-free, typed output for self-checking tests. */
#include <stdio.h>
#include <stdlib.h>

void out_raw(const char* s) { printf("%s", s); }
void out_int(int v)         { printf("%d", v); }
void out_bool(int v)        { printf("%s", v ? "true" : "false"); }
void out_float(double v)    { printf("%g", v); }
