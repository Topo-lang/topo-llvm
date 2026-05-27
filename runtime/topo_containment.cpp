#include "topo/rt/containment_rt.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct ViolationEntry {
    std::string caller;
    std::string callee;
};

static std::mutex g_mutex;
static std::vector<ViolationEntry> g_violations;
static std::atomic<bool> g_atexitRegistered{false};

} // anonymous namespace

extern "C" {

uint32_t topo_containment_version(void) {
    return TOPO_CONTAINMENT_ABI_VERSION;
}

void __topo_containment_violation(const char* caller, const char* callee) {
    // Register atexit dump on first call
    if (!g_atexitRegistered.exchange(true)) {
        std::atexit(__topo_containment_dump);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_violations.push_back({caller ? caller : "<unknown>", callee ? callee : "<unknown>"});
}

int __topo_containment_get_violation_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_violations.size());
}

void __topo_containment_dump(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_violations.empty()) return;

    std::fprintf(stderr, "\n=== Topo Containment Violations ===\n");
    std::fprintf(stderr, "[\n");
    for (size_t i = 0; i < g_violations.size(); ++i) {
        const auto& v = g_violations[i];
        std::fprintf(stderr, "  {\"caller\": \"%s\", \"callee\": \"%s\"}%s\n",
                     v.caller.c_str(), v.callee.c_str(),
                     (i + 1 < g_violations.size()) ? "," : "");
    }
    std::fprintf(stderr, "]\n");
    std::fprintf(stderr, "Total violations: %zu\n", g_violations.size());
}

void __topo_containment_reset(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_violations.clear();
}

} // extern "C"
