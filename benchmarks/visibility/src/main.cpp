// Benchmark: TopoInlinePass + TopoFlattenPass — visibility-based inlining.
//
// Friendly:   deep call chain through protected/private functions.
//             InlinePass gives InternalLinkage → enables cross-TU inlining.
// Unfriendly: only call public functions → no extra inlining opportunity.

#include "calculator.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Friendly: repeated calls through protected/private chain ---

static int friendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i) {
        int r = calc::core::add(i, i + 1);
        r = calc::core::subtract(r, 1);
        sink = r;
    }
    return sink;
}

// --- Unfriendly: only public function, minimal call depth ---

static int unfriendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 10000; ++i) {
        sink = calc::core::get_last_result();
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
    constexpr int ITERS = 500;

    // Correctness check (from original 01 + 03)
    {
        using namespace calc::core;
        int sum = add(3, 4);
        assert(sum == 7);
        assert(get_last_result() == 7);

        int diff = subtract(10, 3);
        assert(diff == 7);
        assert(get_last_result() == 7);

        reset_state();
        assert(get_last_result() == 0);
        std::printf("01_hello_visibility: all assertions passed\n");
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
