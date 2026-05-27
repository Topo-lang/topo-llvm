// Benchmark: TopoReorderPass + TopoLayoutPass — stage ordering and icache locality.
//
// Friendly:   many function calls in stage order → icache-friendly layout helps.
// Unfriendly: random-order calls → icache layout irrelevant.

#include "app.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

// --- Friendly workload: repeated ordered calls (stage 1→2→3) ---

static void friendly_work() {
    app::startup();
}

// --- Unfriendly workload: calls in reverse order ---

static void unfriendly_work() {
    // Call functions in reverse stage order — defeats layout optimization.
    app::start_services();
    app::load_plugins();
    app::init_config();
    app::init_logging();
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
    constexpr int WARMUP = 100;
    constexpr int ITERS = 50000;

    // Correctness check
    {
        app::startup();
        const auto& log = app::get_init_log();
        assert(log.size() == 4);

        bool plugins_after_init = false;
        bool services_after_plugins = false;
        for (size_t i = 0; i < log.size(); ++i) {
            if (log[i] == "plugins") {
                bool found_logging = false, found_config = false;
                for (size_t j = 0; j < i; ++j) {
                    if (log[j] == "logging") found_logging = true;
                    if (log[j] == "config") found_config = true;
                }
                plugins_after_init = found_logging && found_config;
            }
            if (log[i] == "services") {
                bool found_plugins = false;
                for (size_t j = 0; j < i; ++j) {
                    if (log[j] == "plugins") found_plugins = true;
                }
                services_after_plugins = found_plugins;
            }
        }
        assert(plugins_after_init);
        assert(services_after_plugins);
        std::printf("02_stages: all assertions passed\n");
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

    return 0;
}
