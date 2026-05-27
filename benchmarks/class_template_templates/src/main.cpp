// class_template_templates — Step 2 function templates + Step 3 class
// templates (Vector / HashMap / Array). CRTP and Point live in the CRTP
// split; constraints live in the constraint split.

#include "templates.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

BENCH_NOINLINE
static int friendly_bench(int seed) {
    collection::Vector<int32_t> v;
    for (int i = 0; i < 50; ++i)
        v.push_back(seed + i);
    int s = 0;
    for (size_t i = 0; i < v.size(); ++i) s += v.at(i);

    collection::HashMap<int32_t, double> hm;
    for (int i = 0; i < 20; ++i)
        hm.insert(seed + i, static_cast<double>(i));
    s += static_cast<int>(hm.get(seed));

    s += algorithm::max(3, 7);
    s += algorithm::convert<int32_t>(3.14);
    return s;
}

BENCH_NOINLINE
static int unfriendly_bench(int seed) {
    collection::Array<float, 16> arr;
    for (int i = 0; i < 16; ++i)
        arr.set(i, static_cast<float>(seed + i));

    collection::Vector<float> vf;
    for (int i = 0; i < 20; ++i)
        vf.push_back(static_cast<float>(seed + i) * 0.5f);

    return static_cast<int>(arr.at(seed % 16)) + static_cast<int>(vf.front());
}

int main() {
    std::printf("class_template_templates: tests...\n");

    assert(algorithm::max(3, 7) == 7);
    assert(algorithm::normalize(3, 5) == 5);
    algorithm::fill_array<int, 8>(99);

    collection::Vector<int32_t> vi;
    vi.push_back(10); vi.push_back(20); vi.push_back(30);
    assert(vi.size() == 3);
    assert(vi.front() == 10);

    collection::HashMap<int32_t, double> hm;
    hm.insert(1, 100.0);
    assert(hm.contains(1));

    collection::Array<float, 16> arr;
    arr.set(0, 3.14f);
    assert(arr.at(0) > 3.1f && arr.at(0) < 3.2f);

    std::printf("class_template_templates: all assertions passed\n");

    constexpr int ROUNDS = 7, WARMUP = 50, ITERS = 50000;
    auto bench = [](int rounds, int iters, auto&& work) -> long long {
        std::vector<long long> samples;
        for (int r = 0; r < rounds; ++r) {
            auto start = std::chrono::steady_clock::now();
            volatile int sink = 0;
            for (int it = 0; it < iters; ++it)
                sink = work(it);
            (void)sink;
            auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[rounds / 2];
    };

    for (int i = 0; i < WARMUP; ++i) {
        friendly_bench(i);
        unfriendly_bench(i);
    }
    std::printf("RESULT_US_FRIENDLY=%lld\n", bench(ROUNDS, ITERS, friendly_bench));
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", bench(ROUNDS, ITERS, unfriendly_bench));
    return 0;
}
