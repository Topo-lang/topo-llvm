// Benchmark: TopoParallelPass — parallel task execution.
// (merged from 11_auto_parallel + 21_zero_overhead)
//
// Friendly:   many independent tasks → parallel speedup on multi-core.
// Unfriendly: sequential dependency chain → no parallelism possible.

#include <topo/parallel.h>
#include <topo/rt/parallel_rt.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Task functions ---

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

// --- Friendly: independent tasks in parallel ---

static int friendly_work() {
    constexpr int NTASKS = 8;
    int results[NTASKS] = {};
    topo_task_t* tasks[NTASKS];
    for (int i = 0; i < NTASKS; ++i)
        tasks[i] = topo_task_spawn(heavy_work, &results[i]);
    for (int i = 0; i < NTASKS; ++i)
        topo_task_await(tasks[i]);
    int sum = 0;
    for (int i = 0; i < NTASKS; ++i)
        sum += results[i];
    return sum;
}

// --- Unfriendly: sequential dependency chain ---

static int unfriendly_work() {
    int a = 0;
    heavy_work(&a);
    int b = 0;
    light_work(&b);
    return a + b;
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

    // Correctness check (from original 10)
    {
        int a = 0, b = 0;
        auto* t1 = topo_task_spawn(heavy_work, &a);
        auto* t2 = topo_task_spawn(light_work, &b);
        topo_task_await(t1);
        topo_task_await(t2);

        std::printf("results: a=%d, b=%d\n", a, b);
        assert(b == 42);

        auto samples = topo::parallel::get_cost_samples();
        for (auto& [name, ns] : samples)
            std::printf("  %s: %llu ns avg\n", name.c_str(), static_cast<unsigned long long>(ns));
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

    topo::parallel::shutdown();
    return 0;
}
