// class_template_variadic — Step 6 micro-benchmark.

#include "variadic.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

BENCH_NOINLINE
static int friendly_bench(int seed) {
    // Fold-expression sum over heterogeneous types — compiler expands at
    // compile time; should fold to a single add chain.
    int s = variadic::sum_ints(seed, seed + 1, seed + 2, seed + 3, seed + 4);
    variadic::Tuple<int32_t, float, double> t3;
    s += static_cast<int>(t3.component_count());
    return s;
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    // Alternating tuple sizes — no reorder/unroll opportunity.
    int s = 0;
    variadic::Tuple<> t0;
    variadic::Tuple<int32_t> t1;
    variadic::Tuple<int32_t, float> t2;
    s += static_cast<int>(t0.component_count());
    s += static_cast<int>(t1.component_count());
    s += static_cast<int>(t2.component_count());
    s += seed & 7;
    return s;
}

int main() {
    std::printf("class_template_variadic: tests...\n");

    variadic::Tuple<int32_t, float, double> t3;
    assert(t3.component_count() == 3);
    variadic::Tuple<int32_t> t1;
    assert(t1.component_count() == 1);
    variadic::Tuple<> t0;
    assert(t0.component_count() == 0);

    assert(variadic::sum_ints(1, 2, 3) == 6);

    std::printf("class_template_variadic: all assertions passed\n");

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
