// ownership_unfriendly — split from ownership benchmark (S-class fix).
// Isolates the unfriendly path and replaces the synthetic volatile g_escape
// artifact with natural escape: (a) shared_ptr registry accessed through an
// external TU, and (b) an opaque function pointer.
//
// IndirectionPass is expected to be conservative here BECAUSE the escape is
// real, not because of a volatile write. Auto heuristic now sees a clean
// signal for "should not promote."

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
    // shared
    auto tex = std::make_shared<Texture>();
    tex->width = 1920;
    tex->height = 1080;
    int shared_result = process_shared(tex);
    // w + h + area
    assert(shared_result == 1920 + 1080 + 1920 * 1080);

    // weak
    std::weak_ptr<Texture> weak_tex = tex;
    assert(observe_weak(weak_tex) == 3000);
    tex.reset();
    assert(observe_weak(weak_tex) == -1);

    std::printf("ownership_unfriendly: all assertions passed\n");
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
static int unfriendly_work(int seed) {
    int sum = 0;
    auto sink = get_sink();
    for (int i = 0; i < 10; ++i) {
        auto tex = std::make_shared<Texture>();
        tex->width = i * 10 + seed;
        tex->height = i * 20 + seed;

        // Natural escape: call opaque function pointer — compiler cannot
        // see into its body at this TU, must keep the allocation alive.
        sink(tex.get());

        sum += process_shared(tex);

        // Natural escape: register in an external TU list (shared_ptr
        // copy leaves this function).
        register_texture(tex);
    }
    // Drain once per iteration so memory does not grow unbounded.
    sum += drain_registered();
    return sum;
}

int main() {
    run_test();

    constexpr int ROUNDS = 7, WARMUP = 50, ITERS = 200000;
    for (int i = 0; i < WARMUP; ++i) {
        unfriendly_work(i);
    }
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", bench(ROUNDS, ITERS, []() { return unfriendly_work(1); }));
    return 0;
}
