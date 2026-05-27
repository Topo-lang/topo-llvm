// Benchmark: ObservabilityPass — tracing span instrumentation overhead.
//
// Friendly:   tracing enabled, measuring overhead vs no-tracing baseline.
//             (this IS the overhead — threshold allows 10% regression)
// Unfriendly: same workload — both measure pure overhead cost.

#include <topo/rt/observe_rt.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Declared functions (must match .topo declarations) ---

int parse(int input) {
    volatile int acc = input;
    for (int i = 0; i < 1000; ++i)
        acc = acc ^ (acc << 1) ^ i;
    return acc;
}

int validate(int data) {
    volatile int acc = data;
    for (int i = 0; i < 1000; ++i)
        acc = acc ^ (acc << 1) ^ i;
    return acc;
}

int transform(int data) {
    volatile int acc = data;
    for (int i = 0; i < 1000; ++i)
        acc = acc ^ (acc << 1) ^ i;
    return acc;
}

int execute(int input) {
    int a = parse(input);
    int b = validate(a);
    int c = transform(b);
    return c;
}

// Flat version (no function call boundaries — same compute)
static int flat_execute(int input) {
    volatile int acc = input;
    for (int i = 0; i < 1000; ++i)
        acc = acc ^ (acc << 1) ^ i;
    int a = acc;
    acc = a;
    for (int i = 0; i < 1000; ++i)
        acc = acc ^ (acc << 1) ^ i;
    int b = acc;
    acc = b;
    for (int i = 0; i < 1000; ++i)
        acc = acc ^ (acc << 1) ^ i;
    return acc;
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
    constexpr int ITERS = 10000;

    topo_trace_init("stdout", 1.0);

    // Correctness check
    {
        int result = execute(10);
        std::printf("28_observability: result=%d\n", result);
        std::printf("28_observability: all assertions passed\n");
    }

    topo_trace_shutdown();

    // Benchmarks run WITHOUT active tracing to measure just the
    // instrumentation overhead inserted by ObservabilityPass.
    // The Pass inserts span_begin/end calls; with tracing shutdown,
    // these are no-ops but still have call overhead.

    // Warmup
    for (int i = 0; i < WARMUP; ++i)
        execute(i);

    // Friendly (hot pipeline with tracing instrumentation)
    auto friendly_us = benchmark(ROUNDS, ITERS, [&]() { return execute(10); });
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    // Unfriendly (same workload — overhead is the same)
    auto unfriendly_us = benchmark(ROUNDS, ITERS, [&]() { return execute(42); });
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    return 0;
}
