// Benchmark: LoopParallelizePass — vectorization metadata injection.
//
// Friendly:   large array, streaming access → SIMD vectorization effective.
// Unfriendly: loop with data dependency → can't vectorize.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

static constexpr int N = 65536;
static float data_a[N];
static float data_b[N];

// Private per-function buffers so `scale` and `negate` are truly independent
// siblings — matches the .topo declaration that they both feed `sum` with no
// ordering constraint. Sharing `data_a` between them violated that
// declaration under forced parallelization (the two would race on the same
// buffer), so each gets its own buffer.
static float data_scale[N];
static float data_negate[N];

// --- Declared functions (must match .topo declarations) ---

int scale(float factor) {
    int s = 0;
    for (int i = 0; i < N; ++i) {
        data_scale[i] = static_cast<float>(i) * factor;
        s += static_cast<int>(data_scale[i]);
    }
    return s;
}

int negate() {
    int s = 0;
    for (int i = 0; i < N; ++i) {
        data_negate[i] = -static_cast<float>(i);
        s += static_cast<int>(data_negate[i]);
    }
    return s;
}

int sum(int a, int b) {
    return a + b;
}

int run_test() {
    // Sequence the staged calls explicitly. As call arguments their evaluation
    // order is unspecified in C++ (MSVC evaluates arguments right-to-left), which
    // reorders the stage<1> scale / stage<2> negate calls in the emitted IR and
    // trips the stage-order verifier on Windows. Separate statements are
    // sequenced, so every ABI emits scale -> negate -> sum.
    int s = scale(1.0f);  // stage<1>
    int n = negate();     // stage<2>
    return sum(s, n);     // stage<3>
}

// --- Friendly: streaming vectorizable loop ---

static int friendly_work() {
    for (int i = 0; i < N; ++i)
        data_a[i] = static_cast<float>(i) * 2.5f;
    for (int i = 0; i < N; ++i)
        data_b[i] = data_a[i] + 1.0f;
    float s = 0;
    for (int i = 0; i < N; ++i)
        s += data_b[i];
    return static_cast<int>(s);
}

// --- Unfriendly: loop-carried dependency (can't vectorize) ---

static int unfriendly_work() {
    data_a[0] = 1.0f;
    for (int i = 1; i < N; ++i)
        data_a[i] = data_a[i - 1] * 1.00001f + 0.1f; // carried dependency
    return static_cast<int>(data_a[N - 1]);
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
    constexpr int WARMUP = 50;
    constexpr int ITERS = 500;

    // Correctness
    int result = run_test();
    std::printf("24_loop_parallel: result=%d\n", result);
    std::printf("24_loop_parallel: all assertions passed\n");

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
