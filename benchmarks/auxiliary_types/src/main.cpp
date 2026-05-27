// Benchmark: SDK auxiliary types (topo::array, topo::span, topo::slot)
// vs std equivalents runtime performance.
//
// Friendly:   topo::span iteration + topo::slot lookup (optimizable by topo).
// Unfriendly: raw pointer iteration (already optimal, no SDK type benefit).

#include "auxiliary_types.h"
#include <topo/array.h>
#include <topo/span.h>
#include <topo/slot.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

static constexpr int N = 4096;
static int g_data[N];

// --- Friendly: topo SDK types ---

static int friendly_work() {
    topo::span<int> s(g_data, N);
    int sum = 0;
    for (int x : s)
        sum += x;

    topo::slot<int> result;
    result.emplace(sum);
    return result.value_or(0);
}

// --- Unfriendly: raw pointer (no SDK type overhead to optimize) ---

static int unfriendly_work() {
    int sum = 0;
    for (int i = 0; i < N; ++i)
        sum += g_data[i];
    return sum;
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
    constexpr int WARMUP = 100;
    constexpr int ITERS = 50000;

    // Init test data
    for (int i = 0; i < N; ++i)
        g_data[i] = i;

    // Correctness checks
    {
        topo::array<int, 4> arr = {10, 20, 30, 40};
        assert(arr.size() == 4 && arr[0] == 10);

        topo::span<int> s(arr);
        assert(s.size() == 4 && s[2] == 30);

        topo::slot<int> empty;
        assert(!empty.has_value());
        topo::slot<int> full(42);
        assert(full.value() == 42);

        int data[] = {1, 2, 3, 4, 5};
        assert(aux::sum_array(data, 5) == 15);
        assert(aux::find_or_default(data, 5, 3, -1) == 3);

        std::printf("09_auxiliary_types: all assertions passed\n");
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
