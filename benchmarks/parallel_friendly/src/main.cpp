// parallel_friendly — split from parallel benchmark (S-class fix).
// Isolates the independent-task path: many heavy tasks, no dependencies.
// TopoParallelPass auto heuristic should see a clean signal favoring parallel.

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

// Friendly: NTASKS independent heavy tasks.
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
        int a = 0;
        auto* t1 = topo_task_spawn(heavy_work, &a);
        topo_task_await(t1);
        std::printf("parallel_friendly: a=%d\n", a);
    }

    for (int i = 0; i < WARMUP; ++i)
        friendly_work();

    auto friendly_us = benchmark(ROUNDS, ITERS, friendly_work);
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    topo::parallel::shutdown();
    return 0;
}
