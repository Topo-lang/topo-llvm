// Benchmark: IREmbed + JIT specialize.
// (merged from 13_jit_specialize)
//
// Friendly:   pipeline with JIT specialization opportunity.
//             embed_ir=true embeds IR for JIT runtime; specialize prunes paths.
// Unfriendly: full pipeline without pruning — just overhead of embedded sections.

#include "processing.h"
#include <topo/jit.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Friendly: repeated pipeline calls (JIT can specialize) ---

static int friendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i)
        sink = processing::run(i);
    return sink;
}

// --- Unfriendly: direct stage calls (no pipeline optimization) ---

static int unfriendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i) {
        int d = processing::decode(i);
        int t = processing::transform(d);
        sink = processing::encode(t);
    }
    return sink;
}

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
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[rounds / 2];
}

int main() {
    constexpr int ROUNDS = 7;
    constexpr int WARMUP = 50;
    constexpr int ITERS = 100;

    // Correctness check (from original 12 + 13)
    {
        int input = 42;
        int result = processing::run(input);
        std::printf("processing::run(%d) = %d\n", input, result);

        // JIT availability check (from 13)
        if (topo::jit::available()) {
            std::printf("JIT available: embedded IR present\n");
        } else {
            std::printf("JIT not available\n");
        }
    }

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        friendly_work();
        unfriendly_work();
    }

    auto friendly_us = benchmark(ROUNDS, ITERS, friendly_work);
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    auto unfriendly_us = benchmark(ROUNDS, ITERS, unfriendly_work);
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    return 0;
}
