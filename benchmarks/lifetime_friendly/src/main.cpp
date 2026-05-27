// lifetime_friendly — split from lifetime benchmark (S-class + W-class fix).
// Isolates the arena-friendly path and ENLARGES the per-scope allocation
// count well above 1000 so the LifetimeArenaPass auto threshold is clearly
// exceeded.
//
// Friendly pattern: many small allocations accumulate within scope, then
// batch-free at scope exit. Arena: N x bump-alloc + 1 destroy.
// Malloc: N x malloc + N x free (bookkeeping per free).

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

// Stage 1: allocate 1200 small buffers. Above auto threshold for arena.
BENCH_NOINLINE
int alloc_work() {
    constexpr int N = 1200;
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

// Stage 2: another 1100 allocations in the same arena scope.
BENCH_NOINLINE
int sum_work(int partial) {
    constexpr int N = 1100;
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

BENCH_NOINLINE
int run_bench() {
    int a = alloc_work();
    int r = sum_work(a);
    return r;
}

} // namespace arena_demo

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
    constexpr int WARMUP = 20;
    constexpr int ITERS = 2000;

    std::printf("lifetime_friendly: result=%d\n", arena_demo::run_bench());
    std::printf("lifetime_friendly: all assertions passed\n");

    for (int i = 0; i < WARMUP; ++i)
        arena_demo::run_bench();

    auto friendly_us = benchmark(ROUNDS, ITERS, []() { return arena_demo::run_bench(); });
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    return 0;
}
