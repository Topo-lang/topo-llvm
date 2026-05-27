// Benchmark: topo::array<Particle, N> with pipeline + data_layout optimization.
//
// Three-way comparison:
//   1. vanilla O2:  clang++ -O2
//   2. topo OFF:    topo-build, [optimize.data-layout] enabled = false
//   3. topo ON:     topo-build, [optimize.data-layout] enabled = true + hints
//
// Workloads:
//   - Friendly:   streaming traversal (touches 6/16 fields per particle).
//                  SoA transforms 37.5% → 100% cache utilization.
//   - Unfriendly: random-access traversal with all-field reads.
//                  SoA provides no benefit (random stride dominates).

#include "particle.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

// Forward declarations for workloads
namespace physics {
void simulate(ParticleArray& particles, float dt);
}
void unfriendly_work(ParticleArray& particles, const int* indices);

static void init_particles(ParticleArray& p) {
    for (int i = 0; i < COUNT; ++i) {
        float fi = static_cast<float>(i);
        p[i].x = std::sin(fi * 0.1f) * 100.0f;
        p[i].y = std::cos(fi * 0.2f) * 100.0f;
        p[i].z = std::sin(fi * 0.3f) * 100.0f;
        p[i].vx = std::cos(fi * 0.4f) * 10.0f;
        p[i].vy = std::sin(fi * 0.5f) * 10.0f;
        p[i].vz = std::cos(fi * 0.6f) * 10.0f;
        p[i].mass = 1.0f;
        p[i].charge = 0.0f;
        p[i].radius = 0.5f;
        p[i].color = 0xFF0000;
        p[i].flags = 0;
        p[i].lifetime = 100.0f;
        p[i].energy = 50.0f;
        p[i].temperature = 293.15f;
        p[i].group_id = static_cast<int32_t>(i % 16);
        p[i].collision_count = 0;
    }
}

// Generate a deterministic pseudo-random permutation for unfriendly access.
static std::vector<int> make_random_indices(int n) {
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i)
        idx[i] = i;
    // Simple LCG shuffle (deterministic, no <random> needed)
    unsigned seed = 0xDEADBEEF;
    for (int i = n - 1; i > 0; --i) {
        seed = seed * 1664525u + 1013904223u;
        int j = static_cast<int>(seed % static_cast<unsigned>(i + 1));
        std::swap(idx[i], idx[j]);
    }
    return idx;
}

template <typename F>
static long long benchmark(int rounds, int iters, F&& work) {
    std::vector<long long> samples;
    for (int r = 0; r < rounds; ++r) {
        auto start = std::chrono::steady_clock::now();
        for (int it = 0; it < iters; ++it)
            work();
        auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[rounds / 2];
}

int main() {
    constexpr int ROUNDS = 7;
    constexpr int WARMUP = 3;
    constexpr int ITERS_FRIENDLY = 200;
    constexpr int ITERS_UNFRIENDLY = 50;
    constexpr float DT = 0.016f;

    auto particles = std::make_unique<ParticleArray>();
    auto indices = make_random_indices(COUNT);

    // Warmup
    init_particles(*particles);
    for (int i = 0; i < WARMUP; ++i) {
        physics::simulate(*particles, DT);
        unfriendly_work(*particles, indices.data());
    }

    // --- Friendly benchmark (streaming) ---
    init_particles(*particles);
    auto friendly_us = benchmark(ROUNDS, ITERS_FRIENDLY, [&]() { physics::simulate(*particles, DT); });
    std::printf("RESULT_US_FRIENDLY=%lld\n", friendly_us);

    // --- Unfriendly benchmark (random access, all fields) ---
    init_particles(*particles);
    auto unfriendly_us = benchmark(ROUNDS, ITERS_UNFRIENDLY, [&]() { unfriendly_work(*particles, indices.data()); });
    std::printf("RESULT_US_UNFRIENDLY=%lld\n", unfriendly_us);

    // Correctness check
    std::printf("22_data_layout_perf: particle[0]: x=%.2f y=%.2f vx=%.4f\n",
                (*particles)[0].x,
                (*particles)[0].y,
                (*particles)[0].vx);
    std::printf("22_data_layout_perf: done\n");
    return 0;
}
