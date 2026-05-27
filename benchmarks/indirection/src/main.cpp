// Benchmark: IndirectionPass optimization.
//
// Three-way comparison:
//   1. vanilla O2:  clang++ -O2
//   2. topo OFF:    topo-build, [optimize.indirection] enabled = false
//   3. topo ON:     topo-build, [optimize.indirection] enabled = true
//
// Workloads:
//   - Friendly:   unique_ptr promotion + vector→span lowering at scale.
//                  IndirectionPass eliminates heap indirection overhead.
//   - Unfriendly: raw pointer arithmetic — already optimal, no indirection to remove.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "types.h"

// --- Declared functions (must match .topo declarations) ---

int process_unique(std::unique_ptr<Buffer> buf) {
    int sum = 0;
    for (int i = 0; i < buf->size; ++i)
        sum += buf->data[i];
    return sum;
}

int process_vector(int count) {
    std::vector<int> v(count);
    for (int i = 0; i < count; ++i)
        v[i] = i * 2;
    int sum = 0;
    for (int i = 0; i < count; ++i)
        sum += v[i];
    return sum;
}

int run_test() {
    // unique_ptr path
    auto buf = std::make_unique<Buffer>();
    int arr[] = {100};
    buf->data = arr;
    buf->size = 1;
    int r1 = process_unique(std::move(buf));
    assert(r1 == 100);

    // vector path
    int r2 = process_vector(5);
    assert(r2 == 20); // 0+2+4+6+8

    return r1 + r2;
}

// --- Friendly workload: unique_ptr + vector at scale ---

static int friendly_work() {
    auto buf = std::make_unique<Buffer>();
    std::vector<int> storage(256);
    for (int i = 0; i < 256; ++i)
        storage[i] = i;
    buf->data = storage.data();
    buf->size = 256;
    int r1 = process_unique(std::move(buf));

    int r2 = process_vector(256);
    return r1 + r2;
}

// --- Unfriendly workload: raw pointer arithmetic (already optimal) ---

static int unfriendly_work() {
    int arr[256];
    for (int i = 0; i < 256; ++i)
        arr[i] = i * 3;
    int* p = arr;
    int sum = 0;
    for (int i = 0; i < 256; ++i)
        sum += p[i];
    return sum;
}

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
    constexpr int ITERS_FRIENDLY = 100000;
    constexpr int ITERS_UNFRIENDLY = 100000;

    // Correctness check
    int result = run_test();
    std::printf("23_indirection: result=%d\n", result);
    std::printf("23_indirection: all assertions passed\n");

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        friendly_work();
        unfriendly_work();
    }

    // Benchmark friendly
    auto friendly_us = benchmark(ROUNDS, ITERS_FRIENDLY, friendly_work);
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    // Benchmark unfriendly
    auto unfriendly_us = benchmark(ROUNDS, ITERS_UNFRIENDLY, unfriendly_work);
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    return 0;
}
