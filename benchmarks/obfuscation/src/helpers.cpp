#include "obf.h"

namespace obf {

int helper_a(int x) { return x * 3 + 1; }
int helper_b(int x) { return x ^ 0x5a; }

int compute_inner(int x) {
    return helper_a(x) + helper_b(x);
}

int run_benchmark() {
    int sum = 0;
    for (int i = 0; i < 100; ++i) {
        sum += compute_inner(i);
    }
    return sum;
}

} // namespace obf
