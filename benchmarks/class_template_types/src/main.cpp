// class_template_types — exercises only the non-template class layer of the
// original class_template benchmark (inheritance, cached state, statics).
//
// Friendly path: constructing and destructing Circle objects exercises
// TopoReorderPass on the ctor body.
//
// Unfriendly path: repeatedly reading area() (no reorder opportunity).

#include "types.h"
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

static bool approx(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) < eps;
}

BENCH_NOINLINE
static int friendly_bench(int seed) {
    // Fresh allocations in each iteration — exercises ctor reorder path.
    container::Circle c(static_cast<float>(seed % 100 + 1));
    float a = c.area();
    float p = c.perimeter();
    return static_cast<int>(a) + static_cast<int>(p);
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    // Stable object; hot loop reads cached values only — no reorder leverage.
    static container::Circle c(5.0f);
    float a = c.area();
    return static_cast<int>(a) + (seed & 7);
}

int main() {
    std::printf("class_template_types: tests...\n");
    // Correctness
    assert(approx(container::MathConstants::pi(), 3.14159265f));
    assert(approx(container::MathConstants::e(), 2.71828183f));
    container::Circle c(5.0f);
    assert(approx(c.radius(), 5.0f));
    assert(approx(c.area(), container::MathConstants::pi() * 25.0f));
    std::printf("class_template_types: all assertions passed\n");

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
