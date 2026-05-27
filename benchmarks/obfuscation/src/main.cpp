#include "obf.h"
#include <cstdio>

int main() {
    int result = obf::run_benchmark();
    std::printf("obfuscation: result=%d\n", result);
    return 0;
}
