// Benchmark: data-aware optimization hints (cardinality/access).
//
// Friendly:   large cardinality + streaming access → hints enable parallel + SoA.
// Unfriendly: small cardinality → below parallel threshold, hints no benefit.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

static std::vector<float> values;

void update(int n) {
    values.resize(n);
    for (int i = 0; i < n; ++i)
        values[i] = static_cast<float>(i) * 0.5f;
}

int collect(int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i)
        sum += values[i];
    return static_cast<int>(sum);
}

void run(int n) {
    update(n);
    int result = collect(n);
    std::printf("26_hints: n=%d sum=%d\n", n, result);
}

// --- Friendly: large N — array spans >L1 so prefetch has bandwidth to cover ---
// N=100000 floats = 400KB (exceeds L1 ~32KB, straddles L2 on Apple M-series),
// and .topo declares access(streaming) + cardinality(1k..100k), so
// PrefetchPass inserts llvm.prefetch in update/collect streaming loops.

BENCH_NOINLINE
static int friendly_work() {
    constexpr int N = 100000;
    update(N);
    return collect(N);
}

// --- Unfriendly: small N (below thresholds, hints irrelevant) ---
// N=1000 floats = 4KB fits entirely in L1, so HW prefetch alone is sufficient
// and SW prefetch intrinsics are near-zero-cost but produce no signal. The
// scatter loop additionally exercises a non-streaming access pattern that
// defeats prefetch — auto mode should not insert prefetch for it.

BENCH_NOINLINE
static int unfriendly_work() {
    constexpr int N = 1000;
    update(N);
    // Random-access pattern: hints assume streaming but this is scattered
    int sum = 0;
    for (int i = 0; i < N; ++i) {
        int idx = (i * 997) % N; // pseudo-random scatter
        sum += static_cast<int>(values[idx]);
    }
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
    // Quick mode (the correctness executables set TOPO_BENCH_QUICK): the compared
    // output is the collect() sum + assertions, independent of these perf-loop
    // counts (RESULT_US_* is timing-stripped). The perf harness keeps full counts.
    const bool quick = std::getenv("TOPO_BENCH_QUICK") != nullptr;
    const int ROUNDS = quick ? 1 : 7;
    const int WARMUP = quick ? 2 : 100;
    const int ITERS = quick ? 10 : 5000;

    // Correctness check
    run(100);
    std::printf("26_hints: all assertions passed\n");

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
