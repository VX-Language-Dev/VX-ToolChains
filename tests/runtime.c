// VX Runtime support for testing
// Provides out() and out_int() implementations callable via extern
#include <stdio.h>

void out(const char* msg) {
    printf("%s", msg);
}

void out_int(int val) {
    printf("%d", val);
}
