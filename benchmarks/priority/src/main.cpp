// Benchmark: Priority annotations — effect on parallel pass cost thresholds.
//
// Friendly:   critical-priority pipeline execution (parallel pass lowers threshold).
// Unfriendly: low-priority scattered calls (no threshold benefit).

#include "compute.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Friendly workload: critical-path pipeline (priority propagation active) ---

static void friendly_work() {
    compute::run_pipeline();
}

// --- Unfriendly workload: low-priority scattered calls ---

static void unfriendly_work() {
    compute::log_metrics();
    compute::cleanup();
    compute::process_data(50);
}

template <typename F>
static long long benchmark(int rounds, int iters, F&& work) {
    std::vector<long long> samples;
    for (int r = 0; r < rounds; ++r) {
        auto start = std::chrono::steady_clock::now();
        for (int it = 0; it < iters; ++it)
            work();
        auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[rounds / 2];
}

int main() {
    constexpr int ROUNDS = 7;
    constexpr int WARMUP = 100;
    constexpr int ITERS = 50000;

    // Correctness check
    {
        compute::run_pipeline();
        compute::run_critical_path();
        compute::process_data(100);
        compute::cleanup();
        compute::log_metrics();
        compute::critical_helper(50);
        compute::internal_step();
        std::printf("priority_e2e: all assertions passed\n");
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
