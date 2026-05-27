// Benchmark: PipelineCodeGenPass — DAG-optimized pipeline code generation.
// (merged from 07_perf_comparison)
//
// Friendly:   pipeline with fork/join (enhance+detect parallel) at scale.
//             PipelineCodeGenPass eliminates call overhead and schedules optimally.
// Unfriendly: manual sequential calls (no pipeline DAG benefit).

#include "pipeline.h"
#include <topo/parallel.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Friendly: pipeline-generated path ---

static int friendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i) {
        sink = imaging::process(i);
    }
    return sink;
}

// --- Unfriendly: manual sequential calls (same computation) ---

static int unfriendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i) {
        int loaded = imaging::load(i);
        int decoded = imaging::decode(loaded);
        int enhanced = imaging::enhance(decoded);
        int detected = imaging::detect(decoded);
        sink = imaging::compose(enhanced, detected);
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

    topo::parallel::init();

    // Correctness check (original 05 + 07)
    {
        int loaded = imaging::load(10);
        int decoded = imaging::decode(loaded);
        int enhanced = imaging::enhance(decoded);
        int detected = imaging::detect(decoded);
        int result = imaging::compose(enhanced, detected);
        assert(result == 125);

        std::printf("05_pipeline: pipeline result = %d\n", result);
        std::printf("05_pipeline: all assertions passed\n");
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

    topo::parallel::shutdown();
    return 0;
}
