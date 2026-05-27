// class_template_comptime — Step 5 micro-benchmark.

#include "comptime.h"
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
    // Heavy work through a Stack<T, Vector>.
    meta::Stack<int, collection::Vector> st;
    for (int i = 0; i < 30; ++i) st.push(seed + i);
    int s = st.top();
    // Exercise constexpr factorial at runtime.
    s += meta::factorial(seed & 7);
    return s;
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    // Compile-time constant loops — factorial(10) folds; Wider<T>::type
    // size is constant. Nothing to reorder.
    int s = static_cast<int>(sizeof(typename meta::Wider<float>::type));
    s += static_cast<int>(sizeof(typename meta::Wider<int32_t>::type));
    s += meta::factorial(10) & 0xFFFF;
    s += seed & 3;
    return s;
}

int main() {
    std::printf("class_template_comptime: tests...\n");

    meta::small_path();
    meta::large_path();
    static_assert(meta::factorial(0) == 1, "0!");
    static_assert(meta::factorial(1) == 1, "1!");
    static_assert(meta::factorial(5) == 120, "5!");
    static_assert(meta::factorial(10) == 3628800, "10!");
    static_assert(sizeof(meta::Wider<float>::type) == sizeof(double), "Wider<float>");
    static_assert(sizeof(meta::Wider<int32_t>::type) == sizeof(size_t), "Wider<int>");

    meta::Stack<int32_t, collection::Vector> stack;
    assert(stack.empty());
    stack.push(42);
    assert(!stack.empty());
    assert(stack.top() == 42);
    stack.push(99);
    assert(stack.top() == 99);

    std::printf("class_template_comptime: all assertions passed\n");

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
