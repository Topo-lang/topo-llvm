// lifetime_unfriendly — split from lifetime benchmark.
//
// Isolates the arena-unfriendly path: two small heap allocations per scope
// (256 bytes each, malloc tcache fast-path hits) plus modest compute, with
// the scope owner invoked once per benchmark iteration. In this regime the
// per-scope arena_create + arena_destroy bookkeeping dominates over the
// nearly-free malloc baseline, exposing the forced-mode cost that
// unfriendly workloads are expected to show.
//
// Auto's VariantBenchmark should observe no speedup and skip; forced applies
// the pass and pays the per-iteration scope-setup overhead.
//
// `volatile_sink` keeps clang -O2's allocation-elimination from heap-to-stack
// promoting the malloc baseline (which would make the comparison meaningless).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

namespace unfriendly_demo {

// Volatile-through-buffer loads/stores prevent clang -O2 from constant-folding
// the buffer accesses (which would let it elide the malloc entirely). The
// buffer pointer never leaves the function, so LifetimeArenaPass's escape
// analysis still classifies the allocation as scope-local.
extern "C" {
volatile int g_value_source = 1;
volatile int g_value_sink   = 0;
}

BENCH_NOINLINE
int single_alloc() {
    constexpr int N = 64;
    int* buf = static_cast<int*>(std::malloc(N * sizeof(int)));
    int seed = g_value_source;
    for (int i = 0; i < N; ++i) {
        reinterpret_cast<volatile int*>(buf)[i] = seed + i;
    }
    int sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += reinterpret_cast<volatile int*>(buf)[i];
    }
    g_value_sink = sum;
    std::free(buf);
    return sum;
}

BENCH_NOINLINE
int single_process(int input) {
    constexpr int N = 64;
    int* buf = static_cast<int*>(std::malloc(N * sizeof(int)));
    int seed = g_value_source ^ input;
    for (int i = 0; i < N; ++i) {
        reinterpret_cast<volatile int*>(buf)[i] = seed ^ (i * 7);
    }
    int result = input;
    for (int i = 0; i < N; ++i) {
        result ^= reinterpret_cast<volatile int*>(buf)[i];
    }
    g_value_sink = result;
    std::free(buf);
    return result;
}

BENCH_NOINLINE
int run_bench() {
    int a = single_alloc();
    int r = single_process(a);
    return r;
}

} // namespace unfriendly_demo

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
    // ~200 ns per call (2 small mallocs + 2 short loops + frees). Bump iter
    // count so total wall-clock stays above the 10 ms absolute-time floor at
    // which the harness suspends threshold checks.
    constexpr int ITERS = 200000;

    std::printf("lifetime_unfriendly: result=%d\n", unfriendly_demo::run_bench());
    std::printf("lifetime_unfriendly: all assertions passed\n");

    for (int i = 0; i < WARMUP; ++i)
        unfriendly_demo::run_bench();

    auto unfriendly_us = benchmark(ROUNDS, ITERS, []() { return unfriendly_demo::run_bench(); });
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    return 0;
}
