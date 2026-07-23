#include <cstdio>

extern "C" {

int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    return a * b;
}

void print_message(const char* msg) {
    printf("%s", msg);
}

}
