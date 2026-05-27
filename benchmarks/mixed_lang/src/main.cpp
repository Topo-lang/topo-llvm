// Benchmark: mixed_lang — C++/Rust cross-language compilation.
//
// Friendly:   tiny Rust leaf function in tight C++ loop — cross-language
//             inlining via LLVM IR merge eliminates call overhead.
// Unfriendly: single heavy Rust computation — call overhead negligible.

#include "engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

template <typename F>
static long long benchmark(int rounds, int iters, F&& work) {
    std::vector<long long> samples;
    for (int r = 0; r < rounds; ++r) {
        auto start = std::chrono::steady_clock::now();
        volatile int sink = 0;
        for (int it = 0; it < iters; ++it)
            sink = work();
        (void)sink;
        auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[rounds / 2];
}

int main() {
    constexpr int ROUNDS = 5;
    constexpr int WARMUP = 3;
    constexpr int ITERS_FRIENDLY = 50;
    constexpr int ITERS_UNFRIENDLY = 50;

    // --- Correctness check (preserved from original) ---
    engine::run();
    std::printf("mixed_lang: all assertions passed\n");

    // Use volatile to prevent constant propagation into benchmarked functions.
    // Without this, LLVM computes loop sums as closed-form expressions.
    volatile int friendly_n = 500000;
    volatile int unfriendly_n = 100000;

    // --- Warmup ---
    volatile int warmup_sink = 0;
    for (int i = 0; i < WARMUP; ++i) {
        warmup_sink = engine::bench_friendly(friendly_n);
        warmup_sink = engine::bench_unfriendly(unfriendly_n);
    }
    (void)warmup_sink;

    // --- Benchmark: friendly ---
    auto friendly_us = benchmark(ROUNDS, ITERS_FRIENDLY, [&]() {
        return engine::bench_friendly(friendly_n);
    });
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    // --- Benchmark: unfriendly ---
    auto unfriendly_us = benchmark(ROUNDS, ITERS_UNFRIENDLY, [&]() {
        return engine::bench_unfriendly(unfriendly_n);
    });
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    return 0;
}
