// class_template_crtp — CRTP static polymorphism path.
//
// Friendly: repeated CRTP score() calls on both Point and Counter — compiler
// should devirtualize through the static_cast in Printable.
//
// Unfriendly: interleaved types in a condition-heavy loop.

#include "crtp.h"
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
    collection::Point pt(static_cast<float>(seed), static_cast<float>(seed + 1));
    int s = pt.score();
    collection::Counter c(seed);
    for (int i = 0; i < 32; ++i) c.bump();
    s += c.score();
    return s;
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    int s = 0;
    for (int i = 0; i < 8; ++i) {
        if (i & 1) {
            collection::Point pt(static_cast<float>(seed + i), static_cast<float>(seed - i));
            s += pt.score();
        } else {
            collection::Counter c(seed + i);
            s += c.score();
        }
    }
    return s;
}

int main() {
    std::printf("class_template_crtp: tests...\n");
    collection::Point pt(3.0f, 4.0f);
    assert(pt.score() == 7);
    collection::Counter c(5);
    c.bump();
    assert(c.score() == 12);
    std::printf("class_template_crtp: all assertions passed\n");

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
