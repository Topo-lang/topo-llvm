// cross_module_adapter — Cross-module type adapter usage

#include "adapter.h"
#include <cassert>
#include <cstdio>

namespace types {

int pair_sum(const Pair& a, const Pair& b) {
    return a.first() + a.second() + b.first() + b.second();
}

} // namespace types

namespace app {

int combine(const types::Pair& p) {
    return p.first() * 10 + p.second();
}

int run_adapter(int a, int b, int c, int d) {
    types::Pair p1(a, b);
    types::Pair p2(c, d);
    int sum = types::pair_sum(p1, p2);
    int combined = combine(p1) + combine(p2);
    return sum + combined;
}

} // namespace app

int main() {
    // run_adapter(1, 2, 3, 4):
    //   pair_sum(Pair(1,2), Pair(3,4)) = 1+2+3+4 = 10
    //   combine(Pair(1,2)) = 1*10+2 = 12
    //   combine(Pair(3,4)) = 3*10+4 = 34
    //   result = 10 + 12 + 34 = 56
    int result = app::run_adapter(1, 2, 3, 4);
    assert(result == 56);

    std::printf("cross_module_adapter: result=%d\n", result);
    std::printf("cross_module_adapter: all assertions passed\n");
    return 0;
}
