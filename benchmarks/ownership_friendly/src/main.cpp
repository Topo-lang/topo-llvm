// ownership_friendly — split from ownership benchmark (S-class fix).
// Isolates the owned/unique_ptr path so auto heuristic sees NO volatile
// g_escape artifact in this binary.
//
// Friendly path: unique_ptr allocations that do NOT escape. Full indirection
// optimization is expected.

#include "scene.h"
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

int run_test() {
    // Correctness
    auto node = std::make_unique<Node>();
    node->value = 10;
    node->extra = 20;
    int owned_result = process_owned(std::move(node));
    // a=10, b=20, c=10+20=30 => 10+20+30 = 60
    assert(owned_result == 60);

    std::printf("ownership_friendly: all assertions passed\n");
    return 0;
}

template <typename F>
static long long bench(int rounds, int iters, F&& work) {
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

BENCH_NOINLINE
static int friendly_work(int seed) {
    // No volatile store / no g_escape. Each unique_ptr is local-only and
    // consumed by process_owned(std::move(node)). This gives IndirectionPass
    // + inlining + promotion the full unobstructed signal.
    int sum = 0;
    for (int i = 0; i < 10; ++i) {
        auto node = std::make_unique<Node>();
        node->value = i + seed;
        node->extra = i * 2 + seed;
        sum += process_owned(std::move(node));
    }
    return sum;
}

int main() {
    run_test();

    constexpr int ROUNDS = 7, WARMUP = 50, ITERS = 200000;
    for (int i = 0; i < WARMUP; ++i) {
        friendly_work(i);
    }
    std::printf("RESULT_US_FRIENDLY=%lld\n", bench(ROUNDS, ITERS, []() { return friendly_work(1); }));
    return 0;
}
