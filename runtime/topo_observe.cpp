#include "topo/rt/observe_rt.h"

#include "json_escape.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

// ============================================================
// Span stack entry
// ============================================================

struct SpanEntry {
    const char* name;
    std::chrono::steady_clock::time_point start;
    bool sampled; // true if this span should be recorded
};

// ============================================================
// Thread-local span stack
// ============================================================

thread_local std::vector<SpanEntry> t_spanStack;
thread_local std::mt19937 t_rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

// ============================================================
// Global configuration
// ============================================================

static std::atomic<bool> g_initialized{false};
static std::atomic<double> g_samplingRate{1.0};

// Mutex for serializing output writes
static std::mutex g_outputMutex;

// ============================================================
// Output helpers
// ============================================================

/// Get a numeric thread identifier (platform-independent).
static uint64_t getThreadId() {
    auto id = std::this_thread::get_id();
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(id));
}

/// Emit a completed span to stdout in JSON lines format.
///
/// The `name` field is routed through topo::rt::writeJsonString so a
/// span name containing `"`, `\`, or any control character (e.g. a
/// Windows-style path slipped into a TypeScript span, or a name with
/// an embedded newline) cannot break the one-record-per-stdout-line
/// framing that topo-profile depends on.
static void emitSpan(const char* name, uint64_t durationNs, uint64_t threadId) {
    // Get wall-clock timestamp for the span completion
    auto now = std::chrono::system_clock::now();
    auto epochNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::fputs("{\"name\":", stdout);
    topo::rt::writeJsonString(stdout, name);
    std::fprintf(stdout,
                 ",\"duration_ns\":%llu,"
                 "\"thread_id\":%llu,\"ts_ns\":%lld}\n",
                 static_cast<unsigned long long>(durationNs),
                 static_cast<unsigned long long>(threadId),
                 static_cast<long long>(epochNs));
}

/// Check if a span should be sampled based on the configured rate.
static bool shouldSample() {
    double rate = g_samplingRate.load(std::memory_order_relaxed);
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(t_rng) < rate;
}

} // anonymous namespace

// ============================================================
// C ABI
// ============================================================

extern "C" {

uint32_t topo_trace_version(void) {
    return TOPO_OBSERVE_ABI_VERSION;
}

void topo_trace_span_begin(const char* name) {
    // Silent no-op when the runtime is not initialized. This matches the
    // contract documented in observe_rt.h: calls made before
    // topo_trace_init or after topo_trace_shutdown do not push, pop, or
    // emit.
    if (!g_initialized.load(std::memory_order_relaxed)) return;

    bool sampled = shouldSample();
    SpanEntry entry;
    entry.name = name;
    entry.start = std::chrono::steady_clock::now();
    entry.sampled = sampled;
    t_spanStack.push_back(entry);
}

void topo_trace_span_end(void) {
    // If the runtime is not initialized, this is a silent no-op, but we
    // still pop a residual entry (if any) from the thread-local span
    // stack. This keeps begin/end pairs balanced when init/shutdown is
    // cycled on a thread that already has spans pushed — otherwise
    // pre-init begins + post-init ends would leak entries across cycles.
    if (!g_initialized.load(std::memory_order_relaxed)) {
        if (!t_spanStack.empty()) t_spanStack.pop_back();
        return;
    }

    if (t_spanStack.empty()) return;

    auto entry = t_spanStack.back();
    t_spanStack.pop_back();

    if (!entry.sampled) return;

    auto end = std::chrono::steady_clock::now();
    auto durationNs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - entry.start).count());

    emitSpan(entry.name, durationNs, getThreadId());
}

void topo_trace_init(const char* exporter, double sampling_rate) {
    // The `exporter` parameter is reserved for future routing; the runtime
    // currently only emits to stdout as JSON lines. ConfigValidator already
    // rejects any value other than "stdout" at build configure time, so
    // anything reaching here is either "stdout" or null.
    (void)exporter;
    if (g_initialized.exchange(true, std::memory_order_acquire)) return;

    g_samplingRate.store(sampling_rate, std::memory_order_relaxed);
}

void topo_trace_shutdown(void) {
    if (!g_initialized.exchange(false, std::memory_order_acquire)) return; // not initialized

    // Flush stdout to ensure all spans are written
    std::fflush(stdout);
}

} // extern "C"
