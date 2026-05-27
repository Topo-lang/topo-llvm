// variadic_template — Variadic template instantiation

#include "pack.h"
#include <cassert>
#include <cstdio>

namespace pack {

int accumulate(int a, int b) {
    return a + b;
}

// Explicit instantiation
template class Tuple<int, float, double>;

} // namespace pack

int main() {
    pack::Tuple<int, float, double> t;
    assert(t.count() == 3);

    int result = pack::sum_all(10, 20, 30);
    assert(result == 60);

    std::printf("variadic_template: count=%zu result=%d\n", t.count(), result);
    std::printf("variadic_template: all assertions passed\n");
    return 0;
}
