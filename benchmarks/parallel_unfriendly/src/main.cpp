// parallel_unfriendly — split from parallel benchmark (S-class fix).
// Isolates the sequential-dependency path. TopoParallelPass auto heuristic
// should see a clean signal for "do not parallelize."

#include <topo/parallel.h>
#include <topo/rt/parallel_rt.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

static void heavy_work(void* arg) {
    topo_cost_begin("heavy_work");
    volatile int sum = 0;
    for (int i = 0; i < 1000000; ++i)
        sum += i;
    *static_cast<int*>(arg) = static_cast<int>(sum);
    topo_cost_end("heavy_work");
}

static void light_work(void* arg) {
    topo_cost_begin("light_work");
    *static_cast<int*>(arg) = 42;
    topo_cost_end("light_work");
}

// Unfriendly: strictly sequential dependency chain (b depends on a).
static int unfriendly_work() {
    int a = 0;
    heavy_work(&a);
    int b = 0;
    // b consumes a — simulate dependency by feeding into light_work input.
    int seed = a & 0x7;
    light_work(&b);
    return a + b + seed;
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
    constexpr int ROUNDS = 5;
    constexpr int WARMUP = 5;
    constexpr int ITERS = 10;

    topo::parallel::init();

    // Correctness
    {
        int a = 0, b = 0;
        heavy_work(&a);
        light_work(&b);
        assert(b == 42);
        std::printf("parallel_unfriendly: a=%d, b=%d\n", a, b);
    }

    for (int i = 0; i < WARMUP; ++i)
        unfriendly_work();

    auto unfriendly_us = benchmark(ROUNDS, ITERS, unfriendly_work);
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    topo::parallel::shutdown();
    return 0;
}
