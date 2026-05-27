// class_template_constraint — Step 4 micro-benchmark.

#include "constraint.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

static bool approx(double a, double b, double eps = 0.001) {
    return std::fabs(a - b) < eps;
}

BENCH_NOINLINE
static int friendly_bench(int seed) {
    collection::Vector<int32_t> data;
    for (int i = 0; i < 40; ++i) data.push_back(seed + i);
    int s = traits::sum(data);
    s += traits::clamp(seed, 0, 100);
    return s;
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    // Pointer/int specialization dispatch — constant-fold path only.
    int s = traits::TypeTraits<int32_t>::max_value() & 0xFF;
    if (traits::TypeTraits<int32_t*>::is_pointer())
        s += static_cast<int>(traits::TypeTraits<int32_t*>::pointer_size());
    s += static_cast<int>(traits::double_add(seed, seed + 1.5));
    return s;
}

int main() {
    std::printf("class_template_constraint: tests...\n");

    assert(approx(traits::double_add(1.5, 2.5), 4.0));
    assert(approx(traits::double_multiply(3.0, 4.0), 12.0));
    assert(approx(traits::double_zero, 0.0));

    collection::Vector<int32_t> data;
    data.push_back(10); data.push_back(20); data.push_back(30); data.push_back(40);
    assert(traits::sum(data) == 100);

    assert(traits::clamp(5, 0, 10) == 5);
    assert(traits::clamp(-5, 0, 10) == 0);

    assert(traits::TypeTraits<int32_t>::is_integral() == true);
    assert(traits::TypeTraits<int32_t>::max_value() == 2147483647);
    assert(traits::TypeTraits<double>::is_integral() == false);
    assert(traits::TypeTraits<int32_t*>::is_pointer() == true);

    std::printf("class_template_constraint: all assertions passed\n");

    constexpr int ROUNDS = 7, WARMUP = 50, ITERS = 50000;
    auto bench = [](int rounds, int iters, auto&& work) -> long long {
        std::vector<long long> samples;
        for (int r = 0; r < rounds; ++r) {
            auto start = std::chrono::steady_clock::now();
            volatile int sink = 0;
            for (int it = 0; it < iters; ++it)
                sink = work(it);
            (void)sink;
            auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[rounds / 2];
    };

    for (int i = 0; i < WARMUP; ++i) {
        friendly_bench(i);
        unfriendly_bench(i);
    }
    std::printf("RESULT_US_FRIENDLY=%lld\n", bench(ROUNDS, ITERS, friendly_bench));
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", bench(ROUNDS, ITERS, unfriendly_bench));
    return 0;
}
