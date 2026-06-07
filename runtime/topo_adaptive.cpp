#include "topo/adaptive.h"
#include "topo/rt/adaptive_rt.h"
#include "topo/rt/pass_event_rt.h"
#include "topo/parallel.h"
#include "topo/jit.h"
#include "topo_adaptive_internal.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace topo::parallel {
// Scoped cost-sample reset, defined in topo_parallel.cpp (linked in
// transitively via topo-jit-api). Declared here rather than in the public
// stable <topo/parallel.h> so the adaptive monitor can clear ONLY the
// pipeline it just specialized instead of every sibling's samples. Resolved
// at link time against libtopo-parallel.
void reset_cost_samples(const std::string& name);
} // namespace topo::parallel

namespace topo::adaptive::internal {

// Definitions for shared state declared in topo_adaptive_internal.h.
// Lives here (not in topo_adaptive_register.cpp) so that the linker has
// a resolution for these globals from the user-facing API side, which is
// always pulled in when any of topo::adaptive::{init,shutdown,stats,...}
// is referenced.

std::mutex g_mutex;
std::vector<PipelineEntry> g_pipelines;
bool g_initialized = false;
bool g_shutdown = false;
std::thread g_monitorThread;
std::condition_variable g_cv;
topo::adaptive::Config g_config;

std::atomic<uint32_t> g_specializations{0};
std::atomic<uint32_t> g_deoptimizations{0};
std::atomic<uint32_t> g_activeJit{0};

} // namespace topo::adaptive::internal

namespace {

using topo::adaptive::internal::g_mutex;
using topo::adaptive::internal::g_pipelines;
using topo::adaptive::internal::g_initialized;
using topo::adaptive::internal::g_shutdown;
using topo::adaptive::internal::g_monitorThread;
using topo::adaptive::internal::g_cv;
using topo::adaptive::internal::g_config;
using topo::adaptive::internal::g_specializations;
using topo::adaptive::internal::g_deoptimizations;
using topo::adaptive::internal::g_activeJit;
using topo::adaptive::internal::PipelineEntry;
using topo::adaptive::internal::PipelineState;

// ============================================================
// Debug logging (enabled via TOPO_ADAPTIVE_VERBOSE=1)
// ============================================================

static bool verboseEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("TOPO_ADAPTIVE_VERBOSE");
        return env && env[0] == '1';
    }();
    return enabled;
}

// Forward decl; defined after the user-facing API section.
// monitorLoop (here) and commitSpecialization (below) both emit the
// variant-switch pass-event through this single shaper.
static void emitVariantSwitch(const PipelineEntry& entry,
                              const char* from,
                              const char* to);

// ============================================================
// Constraint building from cost data
// ============================================================

/// Build a JIT Context populated with cost-based constraints.
///
/// For each pipeline stage that has a runtime cost sample, we compare it
/// against the mean cost across all sampled stages of this pipeline.
/// Stages whose cost is below `cold_ratio` of the mean are considered
/// cold — their incoming edges become candidates for pruning, because
/// the JIT engine can skip scheduling them as separate parallel tasks.
static topo::jit::Context buildConstraintsFromCosts(
    const PipelineEntry& entry,
    const std::unordered_map<std::string, uint64_t>& costs) {

    topo::jit::Context ctx;

    // Collect per-stage costs for this pipeline.
    // Convention: stage cost keys are "<pipeline>::<stage>".
    const std::string prefix = entry.pipelineName + "::";
    std::vector<std::pair<std::string, uint64_t>> stageCosts;

    for (const auto& [name, ns] : costs) {
        if (name.compare(0, prefix.size(), prefix) == 0) {
            std::string stage = name.substr(prefix.size());
            stageCosts.push_back({stage, ns});
        }
    }

    if (stageCosts.empty()) return ctx;

    // Compute mean cost across stages.
    uint64_t total = 0;
    for (const auto& [_, ns] : stageCosts)
        total += ns;
    double mean = static_cast<double>(total) / static_cast<double>(stageCosts.size());

    // Cold-stage threshold: stages costing less than 10% of mean are cold.
    constexpr double cold_ratio = 0.10;
    double threshold = mean * cold_ratio;

    for (const auto& [stage, ns] : stageCosts) {
        if (static_cast<double>(ns) < threshold) {
            // Prune edges INTO the cold stage — the JIT engine will
            // remove it from the parallel DAG, inlining it sequentially.
            ctx.prune_edge("*", stage);

            if (verboseEnabled()) {
                fprintf(stderr, "[topo-adaptive] pipeline '%s': pruning cold stage '%s' "
                        "(cost %llu ns, threshold %.0f ns)\n",
                        entry.pipelineName.c_str(), stage.c_str(),
                        static_cast<unsigned long long>(ns), threshold);
            }
        }
    }

    // Pass the AOT TTI cost as a context parameter so the engine can
    // compare AOT assumptions against runtime reality.
    ctx.set("aot_tti_cost", std::to_string(entry.aotTTICost));

    // Pass pipeline-level runtime cost for engine heuristics.
    auto pipelineIt = costs.find(entry.pipelineName);
    if (pipelineIt != costs.end()) {
        ctx.set("runtime_cost_ns", std::to_string(pipelineIt->second));
    }

    if (verboseEnabled()) {
        fprintf(stderr, "[topo-adaptive] pipeline '%s': built context with %zu pruned edges, "
                "aot_tti=%llu\n",
                entry.pipelineName.c_str(), ctx.prunedEdges().size(),
                static_cast<unsigned long long>(entry.aotTTICost));
    }

    return ctx;
}

// Emit a variant-switch pass-event. Centralized so every
// jitPtr flip (commit / deopt / degradation) produces an identically-
// shaped record on the same stdout stream topo-profile drains. The
// `subject` is the pipeline's qualified name so consumers can attribute
// the switch; the discriminator/pass name are fixed by this Pass.
static void emitVariantSwitch(const PipelineEntry& entry,
                              const char* from,
                              const char* to) {
    topo_pass_event_emit("AdaptiveDispatchPass", from, to,
                          entry.pipelineName.c_str());
}

// ============================================================
// Monitoring logic
// ============================================================

/// Check whether a pipeline's runtime costs deviate enough from
/// what the AOT compiler assumed to warrant re-specialization.
static bool shouldRespecialize(const PipelineEntry& entry, const std::unordered_map<std::string, uint64_t>& costs) {
    // Look for pipeline-level cost sample
    auto it = costs.find(entry.pipelineName);
    if (it == costs.end()) return false;

    uint64_t runtimeNS = it->second;

    // If TTI cost is very low but runtime is high, or vice versa,
    // the AOT decision may be wrong. Compare against configured ratio.
    // We use a simple heuristic: if the runtime cost per call is
    // significantly different from what we'd expect based on TTI,
    // trigger re-specialization.
    //
    // Since TTI units and nanoseconds are different dimensions,
    // we check absolute thresholds: if runtime > 10µs and we haven't
    // specialized yet, it's worth trying.
    if (entry.jitVersions == 0) {
        // First specialization: trigger if pipeline takes significant time
        return runtimeNS > g_config.min_trigger_ns;
    }

    // Subsequent: check if runtime changed significantly from baseline
    if (entry.aotBaselineNS > 0) {
        double ratio = static_cast<double>(runtimeNS) / static_cast<double>(entry.aotBaselineNS);
        return ratio > g_config.deviation_ratio || ratio < (1.0 / g_config.deviation_ratio);
    }

    return false;
}

/// Monitor thread main loop.
static void monitorLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait_for(lock, std::chrono::milliseconds(g_config.monitor_ms), [] { return g_shutdown; });

            if (g_shutdown) return;
        }

        // Query current cost samples
        auto costs = topo::parallel::get_cost_samples();

        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto& entry : g_pipelines) {
            switch (entry.state) {
            case PipelineState::WARMUP: {
                // Approximate call count from cost samples
                auto it = costs.find(entry.pipelineName);
                if (it != costs.end() && it->second > 0) {
                    // We have samples — transition to monitoring
                    entry.aotBaselineNS = it->second;
                    entry.state = PipelineState::MONITORING;
                }
                break;
            }

            case PipelineState::MONITORING: {
                if (entry.jitVersions >= g_config.max_versions) break; // exhausted re-specialization budget

                if (shouldRespecialize(entry, costs)) {
                    // Start async JIT compilation with cost-derived constraints
                    entry.state = PipelineState::SPECIALIZING;
                    auto ctx = buildConstraintsFromCosts(entry, costs);
                    entry.jitFuture = topo::jit::specialize(entry.pipelineName, ctx);
                }
                break;
            }

            case PipelineState::SPECIALIZING: {
                // Check if JIT compilation is done
                if (entry.jitFuture.valid()) {
                    auto status = entry.jitFuture.wait_for(std::chrono::milliseconds(0));
                    if (status == std::future_status::ready) {
                        void* ptr = entry.jitFuture.get();
                        if (ptr) {
                            // Atomic store: replace function pointer
                            std::atomic_store_explicit(
                                reinterpret_cast<std::atomic<void*>*>(entry.jitPtr), ptr, std::memory_order_release);

                            entry.jitVersions++;
                            g_specializations.fetch_add(1, std::memory_order_relaxed);
                            g_activeJit.fetch_add(1, std::memory_order_relaxed);

                            // Variant switch: AOT → JIT (async path).
                            emitVariantSwitch(entry, "aot", "jit");

                            // Reset cost samples for ONLY this pipeline so we
                            // re-measure the JIT version cleanly. A global
                            // reset here would wipe every other registered
                            // pipeline's accumulated cost history each time any
                            // one pipeline specializes, corrupting their
                            // monitoring state (lost baselines, skipped deopt
                            // re-checks). Pipeline-level cost is keyed by
                            // pipelineName; per-stage keys ("<pipeline>::<stage>")
                            // are re-accumulated under VERIFYING.
                            topo::parallel::reset_cost_samples(entry.pipelineName);

                            entry.verifyCount = 0;
                            entry.state = PipelineState::VERIFYING;
                        } else {
                            // JIT failed — go back to monitoring
                            entry.state = PipelineState::MONITORING;
                        }
                    }
                }
                break;
            }

            case PipelineState::VERIFYING: {
                // Wait for enough samples under JIT, then compare
                auto it = costs.find(entry.pipelineName);
                if (it == costs.end()) break;

                entry.jitBaselineNS = it->second;
                entry.verifyCount++;

                // Need enough monitoring cycles to collect stable data.
                // Use ceil(verify_calls / 10) so the config field is respected.
                uint32_t requiredCycles = std::max(1u, (g_config.verify_calls + 9) / 10);
                if (entry.verifyCount < requiredCycles) break;

                // Compare: is JIT actually better?
                if (entry.aotBaselineNS > 0 && entry.jitBaselineNS > 0) {
                    double improvement =
                        static_cast<double>(entry.jitBaselineNS) / static_cast<double>(entry.aotBaselineNS);

                    if (improvement >= g_config.deopt_ratio) {
                        // JIT is not better (or worse) — deoptimize
                        std::atomic_store_explicit(reinterpret_cast<std::atomic<void*>*>(entry.jitPtr),
                                                   static_cast<void*>(nullptr),
                                                   std::memory_order_release);

                        g_deoptimizations.fetch_add(1, std::memory_order_relaxed);
                        g_activeJit.fetch_sub(1, std::memory_order_relaxed);

                        // Variant switch: JIT rejected, back to AOT.
                        emitVariantSwitch(entry, "jit", "aot");

                        entry.state = PipelineState::MONITORING;
                    } else {
                        // JIT is better — keep it active
                        entry.state = PipelineState::ACTIVE;
                    }
                } else {
                    // No baseline to compare — accept JIT
                    entry.state = PipelineState::ACTIVE;
                }
                break;
            }

            case PipelineState::ACTIVE: {
                // Periodic re-check: costs may change over time
                auto it = costs.find(entry.pipelineName);
                if (it != costs.end()) {
                    uint64_t currentNS = it->second;
                    // If performance degraded significantly, go back to monitoring
                    if (entry.jitBaselineNS > 0) {
                        double ratio = static_cast<double>(currentNS) / static_cast<double>(entry.jitBaselineNS);
                        if (ratio > g_config.deviation_ratio) {
                            // Deoptimize and re-evaluate
                            std::atomic_store_explicit(reinterpret_cast<std::atomic<void*>*>(entry.jitPtr),
                                                       static_cast<void*>(nullptr),
                                                       std::memory_order_release);

                            g_deoptimizations.fetch_add(1, std::memory_order_relaxed);
                            g_activeJit.fetch_sub(1, std::memory_order_relaxed);

                            // Variant switch: JIT degraded, back to AOT.
                            emitVariantSwitch(entry, "jit", "aot");

                            entry.state = PipelineState::MONITORING;
                        }
                    }
                }
                break;
            }
            } // switch
        } // for each pipeline
    } // while
}

} // anonymous namespace

// ============================================================
// C ABI — called from global constructors
// ============================================================

// topo_adaptive_register lives in topo_adaptive_register.cpp so that the
// linker can dead-strip it when AdaptiveDispatchPass emitted no ctor
// references (i.e. [adaptive] mode = "off"). Keeping it here would pull
// the symbol into every binary that touches topo::adaptive::stats(),
// violating the "off ≡ no artifacts" contract.

extern "C" {

uint32_t topo_adaptive_version(void) {
    return TOPO_ADAPTIVE_ABI_VERSION;
}

void topo_adaptive_init(void) {
    topo::adaptive::init();
}

void topo_adaptive_shutdown(void) {
    topo::adaptive::shutdown();
}

} // extern "C"

// ============================================================
// C++ API
// ============================================================

namespace topo::adaptive {

void init(const Config& cfg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;

    g_initialized = true;
    g_shutdown = false;
    g_config = cfg;

    g_monitorThread = std::thread(monitorLoop);
}

void shutdown() {
    bool wasInitialized = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        wasInitialized = g_initialized;
        if (wasInitialized) {
            g_shutdown = true;
            g_initialized = false;
        }
    }

    if (wasInitialized) {
        g_cv.notify_all();
        if (g_monitorThread.joinable()) g_monitorThread.join();
    }

    // Clean up pipeline entries unconditionally so that entries registered
    // via topo_adaptive_register() before any init() call (or after a
    // previous shutdown) do not leak across shutdown. This keeps shutdown
    // idempotent: calling it twice is safe, and it always leaves global
    // state equivalent to "never initialized".
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pipelines.clear();
    g_specializations.store(0, std::memory_order_relaxed);
    g_deoptimizations.store(0, std::memory_order_relaxed);
    g_activeJit.store(0, std::memory_order_relaxed);
}

Stats stats() {
    return {g_specializations.load(std::memory_order_relaxed),
            g_deoptimizations.load(std::memory_order_relaxed),
            g_activeJit.load(std::memory_order_relaxed)};
}

// Shared helper: perform the dispatch-atomic-store step given a already-
// resolved JIT function pointer.  Mutex is expected to be held by caller.
static void commitSpecialization(PipelineEntry& entry, void* ptr) {
    if (!ptr) return;
    std::atomic_store_explicit(
        reinterpret_cast<std::atomic<void*>*>(entry.jitPtr), ptr, std::memory_order_release);

    entry.jitVersions++;
    g_specializations.fetch_add(1, std::memory_order_relaxed);
    g_activeJit.fetch_add(1, std::memory_order_relaxed);
    entry.state = PipelineState::ACTIVE;

    // Variant switch: AOT path was in effect, JIT pointer now live.
    emitVariantSwitch(entry, "aot", "jit");
}

void force_specialize(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& entry : g_pipelines) {
        if (entry.pipelineName != name) continue;

        if (entry.jitVersions >= g_config.max_versions) return;

        // Synchronous JIT specialization with cost-derived constraints
        auto costs = topo::parallel::get_cost_samples();
        auto ctx = buildConstraintsFromCosts(entry, costs);
        auto future = topo::jit::specialize(entry.pipelineName, ctx);
        commitSpecialization(entry, future.get());
        return;
    }
}

void force_specialize_bytes(const std::string& name,
                            const void* irBytes, std::size_t irSize,
                            const std::string& metaJson) {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& entry : g_pipelines) {
        if (entry.pipelineName != name) continue;

        if (entry.jitVersions >= g_config.max_versions) return;

        auto costs = topo::parallel::get_cost_samples();
        auto ctx = buildConstraintsFromCosts(entry, costs);
        auto future = topo::jit::specialize_bytes(entry.pipelineName, irBytes, irSize, metaJson, ctx);
        commitSpecialization(entry, future.get());
        return;
    }
}

} // namespace topo::adaptive
