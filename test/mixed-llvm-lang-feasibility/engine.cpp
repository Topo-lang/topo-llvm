#include <cstdio>

extern "C" int rust_add(int a, int b);

int main() {
    int result = rust_add(10, 20);
    // rust_add(10,20) = helper(10) + helper(20) = 31 + 61 = 92
    printf("rust_add(10, 20) = %d\n", result);
    return (result == 92) ? 0 : 1;
}
