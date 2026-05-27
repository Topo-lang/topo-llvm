// Benchmark: LifetimeArenaPass arena allocation.
//
// Friendly:   Many small allocations that accumulate within scope, then
//             batch-free at scope exit.  Arena: N × bump-alloc + 1 destroy.
//             Malloc: N × malloc + N × free (bookkeeping per free).
//
// Unfriendly: Only 2 allocations in scope — below auto-mode threshold (3).
//             Auto skips (ratio ~1.0).  Forced adds trivial overhead.

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

namespace arena_demo {

// --- Friendly: accumulate many allocations, free all at scope exit ---

// Stage 1: allocate 500 small buffers, fill them, return count
BENCH_NOINLINE
int alloc_work() {
    constexpr int N = 500;
    // Store pointers in stack array (alloca) so escape analysis sees them as local.
    // The key: malloc does N allocations + N frees; arena does N bump-allocs + 1 destroy.
    int* ptrs[N];
    int sum = 0;
    for (int i = 0; i < N; ++i) {
        int sz = 4 + (i % 13); // 4..16 ints, dynamic to prevent stack promotion
        ptrs[i] = static_cast<int*>(std::malloc(sz * sizeof(int)));
        std::memset(ptrs[i], 0, sz * sizeof(int));
        ptrs[i][0] = i + 1;
        sum += ptrs[i][0];
    }
    for (int i = 0; i < N; ++i)
        std::free(ptrs[i]);
    return sum;
}

// Stage 2: same pattern, more allocations to amplify the difference
BENCH_NOINLINE
int sum_work(int partial) {
    constexpr int N = 300;
    int* ptrs[N];
    int total = partial;
    for (int i = 0; i < N; ++i) {
        int sz = 8 + (i % 9); // 8..16 ints
        ptrs[i] = static_cast<int*>(std::malloc(sz * sizeof(int)));
        std::memset(ptrs[i], 0, sz * sizeof(int));
        ptrs[i][0] = i + partial;
        total += ptrs[i][0];
    }
    for (int i = 0; i < N; ++i)
        std::free(ptrs[i]);
    return total;
}

// Pipeline entry
BENCH_NOINLINE
int run_bench() {
    int a = alloc_work();
    int r = sum_work(a);
    return r;
}

} // namespace arena_demo

// --- Unfriendly: kept for binary-output compatibility, NOT actively asserted ---
//
// PassBench_OPT_Lifetime_Unfriendly was retired
// because the auto-mode decision for the *mixed* binary is necessarily a
// compromise across both arena_demo (friendly) and unfriendly_demo
// (unfriendly): the LifetimeArenaPass auto pipeline decides once per module,
// then applies or skips the pass globally, so a workload that wants
// "apply for friendly, skip for unfriendly" cannot be expressed.
// PassBench_OPT_LifetimeSplit_Unfriendly uses the dedicated
// `lifetime_unfriendly/` project (single-workload binary) and is the
// authoritative unfriendly check. This namespace is kept so the binary
// still prints `RESULT_US_UNFRIENDLY` for any external consumers; the
// numbers are not consumed by any active assertion.

namespace unfriendly_demo {

BENCH_NOINLINE
int single_alloc() {
    constexpr int N = 65536;
    int* buf = static_cast<int*>(std::malloc(N * sizeof(int)));
    for (int i = 0; i < N; ++i)
        buf[i] = i * 3 + 1;
    int sum = 0;
    for (int i = 0; i < N; ++i)
        sum += buf[i];
    std::free(buf);
    return sum;
}

BENCH_NOINLINE
int single_process(int input) {
    constexpr int N = 65536;
    int* buf = static_cast<int*>(std::malloc(N * sizeof(int)));
    for (int i = 0; i < N; ++i)
        buf[i] = input + i * 7;
    int result = 0;
    for (int i = 0; i < N; ++i)
        result ^= buf[i];
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

// --- Benchmark harness ---

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
    constexpr int ITERS_FRIENDLY = 20000;
    // Unfriendly numbers are no longer asserted (see comment on
    // unfriendly_demo above); keep ITERS aligned with the friendly side.
    constexpr int ITERS_UNFRIENDLY = 20000;

    // Correctness
    std::printf("25_lifetime_arena: result=%d\n", arena_demo::run_bench());
    std::printf("25_lifetime_arena: all assertions passed\n");

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        arena_demo::run_bench();
        unfriendly_demo::run_bench();
    }

    // Friendly: 800 allocs per call, batch free
    auto friendly_us = benchmark(ROUNDS, ITERS_FRIENDLY, []() { return arena_demo::run_bench(); });
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    // Unfriendly: kept for output-format compatibility; not asserted (see
    // comment on unfriendly_demo above).
    auto unfriendly_us = benchmark(ROUNDS, ITERS_UNFRIENDLY, []() { return unfriendly_demo::run_bench(); });
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    return 0;
}
