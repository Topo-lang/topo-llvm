// Benchmark: ReturnSpecializationPass — sret field elimination.
//
// Friendly:   call multi-return function, use only 1 of 3 fields.
//             ReturnSpecializationPass eliminates unused sret fields.
// Unfriendly: use all 3 return fields — no elimination possible.

#include "color.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Friendly: use only hue from rgb_to_hsl (1 of 3 fields) ---

static int friendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 1000; ++i) {
        auto [h, s, l] = color::rgb_to_hsl(i % 256, (i * 3) % 256, (i * 7) % 256);
        sink = h; // Only use first field
        (void)s;
        (void)l;
    }
    return sink;
}

// --- Unfriendly: use all 3 fields (no elimination) ---

static int unfriendly_work() {
    volatile int sink = 0;
    for (int i = 0; i < 1000; ++i) {
        auto [h, s, l] = color::rgb_to_hsl(i % 256, (i * 3) % 256, (i * 7) % 256);
        sink = h + s + l; // Use all fields
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
    constexpr int ITERS = 2500;

    // Correctness check
    {
        auto [h, s, l] = color::rgb_to_hsl(255, 0, 0);
        assert(h == 0);
        assert(s > 0);
        assert(l > 0);

        auto [h2, s2, l2] = color::rgb_to_hsl(128, 128, 128);
        assert(h2 == 0);
        assert(s2 == 0);

        auto [r, g, b] = color::adjust_brightness(100, 100, 100, 50);
        assert(r == 50);
        assert(g == 50);
        assert(b == 50);

        assert(color::clamp(-5, 0, 255) == 0);
        assert(color::clamp(300, 0, 255) == 255);
        assert(color::clamp(128, 0, 255) == 128);

        color::demo();
        std::printf("04_multi_return: all assertions passed\n");
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
