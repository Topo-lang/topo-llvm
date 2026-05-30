#include "topo/adaptive.h"
#include "topo/rt/adaptive_rt.h"
#include "topo/rt/pass_event_rt.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

// The three force-specialize tests below need a real LLVM bitcode blob and
// the JIT engine; both only exist when TOPO_ENABLE_LLVM=ON. With LLVM off
// the other ~14 tests in this file still compile and run — they exercise
// the adaptive runtime's CABI surface without invoking JIT compilation.
#ifdef TOPO_TEST_HAS_LLVM_BITCODE
#include "test_pipeline_bitcode.h"
#endif

namespace {

class AdaptiveMonitorTest : public ::testing::Test {
protected:
    void TearDown() override {
        // shutdown() is only effective when initialized; call init() first
        // to guarantee pipeline entries registered during this test are
        // cleared out of g_pipelines before the next test runs.
        topo::adaptive::init();
        topo::adaptive::shutdown();
    }
};

TEST_F(AdaptiveMonitorTest, InitAndShutdown) {
    // Should not crash with default config
    topo::adaptive::init();
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
    EXPECT_EQ(s.deoptimizations, 0u);
    EXPECT_EQ(s.active_jit, 0u);
}

TEST_F(AdaptiveMonitorTest, DoubleInitIsSafe) {
    topo::adaptive::init();
    topo::adaptive::init(); // second call is a no-op
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
}

TEST_F(AdaptiveMonitorTest, ShutdownWithoutInitIsSafe) {
    topo::adaptive::shutdown(); // no-op
}

TEST_F(AdaptiveMonitorTest, RegisterPipeline) {
    void* jitPtr = nullptr;
    topo_adaptive_register("_Z7processv", "pipeline::process", reinterpret_cast<void**>(&jitPtr), 42);

    // Registration should succeed without crash
    // jitPtr should still be null (no specialization yet)
    EXPECT_EQ(jitPtr, nullptr);
}

TEST_F(AdaptiveMonitorTest, StatsAfterInit) {
    topo::adaptive::Config cfg;
    cfg.warmup_calls = 10;
    cfg.monitor_ms = 100;
    cfg.max_versions = 3;

    topo::adaptive::init(cfg);
    auto s = topo::adaptive::stats();

    EXPECT_EQ(s.specializations, 0u);
    EXPECT_EQ(s.deoptimizations, 0u);
    EXPECT_EQ(s.active_jit, 0u);
}

TEST_F(AdaptiveMonitorTest, MaxVersionsRespected) {
    topo::adaptive::Config cfg;
    cfg.max_versions = 0; // No specializations allowed

    topo::adaptive::init(cfg);

    // force_specialize should be a no-op when max_versions is 0
    void* jitPtr = nullptr;
    topo_adaptive_register("_Z4testv", "test::func", reinterpret_cast<void**>(&jitPtr), 100);

    // This should not crash, and should not specialize
    // (force_specialize checks max_versions)
    topo::adaptive::force_specialize("test::func");

    EXPECT_EQ(jitPtr, nullptr);
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
}

// ============================================================
// Edge-case coverage
// ============================================================

// API gap note: the public adaptive runtime surface is register/init/
// shutdown/stats/force_specialize. There is no user-visible
// `check_dispatch` entry point — AdaptiveDispatchPass emits raw
// atomic loads of the __jit_ptr global directly into client IR, so
// the race scenarios below exercise the monitor thread via the
// stats/register/force_specialize APIs that clients actually call.
// Deoptimization is likewise internal to the monitor state machine
// (triggered by cost-sample deviation during VERIFYING/ACTIVE) and
// cannot be forced from the C ABI — covered only indirectly by the
// MaxVersionsRespected path above. Reentry from a specialized
// callback is not testable because the API exposes no user
// callbacks. These scenarios are documented here and must be
// reconsidered if the API surface grows.

// ---- Rapid init/shutdown cycles (static-state leak detection) ----
TEST_F(AdaptiveMonitorTest, RapidInitShutdownCycles) {
    topo::adaptive::Config cfg;
    cfg.monitor_ms = 10; // minimize monitor thread idle time

    for (int i = 0; i < 10; ++i) {
        topo::adaptive::init(cfg);
        auto s = topo::adaptive::stats();
        EXPECT_EQ(s.specializations, 0u) << "iteration " << i;
        EXPECT_EQ(s.deoptimizations, 0u) << "iteration " << i;
        EXPECT_EQ(s.active_jit, 0u) << "iteration " << i;
        topo::adaptive::shutdown();
    }
}

// ---- Register/force_specialize before init ----
TEST_F(AdaptiveMonitorTest, RegisterBeforeInitIsSafe) {
    // Registration without init must not crash. The entry will be
    // picked up by the monitor thread once init() is called.
    void* jitPtr = nullptr;
    topo_adaptive_register("_Z11preinit_fnv", "preinit::fn",
                           reinterpret_cast<void**>(&jitPtr), 50);

    // stats() before init should report zeroes, not crash.
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
    EXPECT_EQ(s.deoptimizations, 0u);
    EXPECT_EQ(s.active_jit, 0u);

    // force_specialize before init: no crash, silent no-op (no
    // monitor thread, no JIT engine available in the test binary).
    topo::adaptive::force_specialize("preinit::fn");
    EXPECT_EQ(jitPtr, nullptr);

    // Bring the adaptive monitor online to verify the preregistered
    // entry is reachable after init without touching dangling state.
    topo::adaptive::init();
    s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
    // TearDown will shutdown and clear g_pipelines.
}

// ---- Register/force_specialize after shutdown ----
TEST_F(AdaptiveMonitorTest, CallsAfterShutdownAreSafe) {
    void* jitPtr = nullptr;

    topo::adaptive::init();
    topo_adaptive_register("_Z10post_shutv", "post::shut",
                           reinterpret_cast<void**>(&jitPtr), 75);
    topo::adaptive::shutdown();

    // After shutdown, g_pipelines is cleared. Calling force_specialize
    // for an unknown pipeline must be a silent no-op, not a crash.
    topo::adaptive::force_specialize("post::shut");
    EXPECT_EQ(jitPtr, nullptr);

    // stats() after shutdown reports zeroes.
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
    EXPECT_EQ(s.deoptimizations, 0u);
    EXPECT_EQ(s.active_jit, 0u);

    // Registering again after shutdown must not crash. The entry is
    // buffered until the next init() — TearDown cleans it up.
    void* jitPtr2 = nullptr;
    topo_adaptive_register("_Z11post_shut2v", "post::shut2",
                           reinterpret_cast<void**>(&jitPtr2), 75);
    EXPECT_EQ(jitPtr2, nullptr);
}

// ---- Many registered pipelines ----
TEST_F(AdaptiveMonitorTest, ManyRegisteredPipelines) {
    topo::adaptive::Config cfg;
    cfg.monitor_ms = 50;
    topo::adaptive::init(cfg);

    constexpr int kNumPipelines = 1000;
    std::vector<void*> jitPtrs(kNumPipelines, nullptr);
    std::vector<std::string> names;
    names.reserve(kNumPipelines);

    for (int i = 0; i < kNumPipelines; ++i) {
        names.emplace_back("bulk::pipeline_" + std::to_string(i));
    }

    for (int i = 0; i < kNumPipelines; ++i) {
        std::string mangled = "_Z4bulk" + std::to_string(i) + "v";
        topo_adaptive_register(mangled.c_str(),
                               names[i].c_str(),
                               &jitPtrs[i],
                               static_cast<uint64_t>(100 + i));
    }

    // stats must still work after bulk registration.
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
    EXPECT_EQ(s.deoptimizations, 0u);
    EXPECT_EQ(s.active_jit, 0u);

    // force_specialize for each name must not crash. In the unit
    // test environment the JIT engine DLL is absent, so every call
    // is a silent no-op and jitPtrs stay null. The invariant we
    // actually care about is: no corruption under iteration.
    for (const auto& name : names) {
        topo::adaptive::force_specialize(name);
    }

    for (int i = 0; i < kNumPipelines; ++i) {
        EXPECT_EQ(jitPtrs[i], nullptr) << "pipeline " << i;
    }

    s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
}

// ---- Monitor start/stop race ----
TEST_F(AdaptiveMonitorTest, MonitorStartStopRace) {
    // Init with a fast monitor interval so the monitor thread loops
    // hot, then have worker threads hammer the client-visible API
    // surface (force_specialize + stats) while main calls shutdown.
    // TSan will flag any missed synchronization; ASan will flag any
    // use-after-free during teardown.
    topo::adaptive::Config cfg;
    cfg.monitor_ms = 5;
    topo::adaptive::init(cfg);

    // Register a handful of pipelines so the monitor thread actually
    // has work to iterate over each tick.
    std::array<void*, 8> jitPtrs{};
    std::array<std::string, 8> names;
    for (size_t i = 0; i < jitPtrs.size(); ++i) {
        names[i] = "race::pipe_" + std::to_string(i);
        std::string mangled = "_Z4race" + std::to_string(i) + "v";
        topo_adaptive_register(mangled.c_str(), names[i].c_str(),
                               &jitPtrs[i], 1000);
    }

    constexpr int kNumThreads = 4;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([t, &stop, &names]() {
            int iter = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const auto& name = names[(t + iter) % names.size()];
                topo::adaptive::force_specialize(name);
                (void)topo::adaptive::stats();
                ++iter;
            }
        });
    }

    // Let the workers run a few monitor ticks, then tear down
    // concurrently with active callers.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    topo::adaptive::shutdown();
    stop.store(true, std::memory_order_release);

    for (auto& th : threads)
        th.join();

    // Post-shutdown, stats must still be callable without crashing.
    auto s = topo::adaptive::stats();
    EXPECT_EQ(s.specializations, 0u);
    EXPECT_EQ(s.deoptimizations, 0u);
    EXPECT_EQ(s.active_jit, 0u);
}

// ============================================================
// Real JIT dispatch atomic-store coverage
//
// Covers the case where the adaptive dispatch switch otherwise never
// triggers in tests.
//
// force_specialize_bytes() routes through topo::jit::specialize_bytes()
// and performs the dispatch-pointer atomic store when the engine
// returns a valid function pointer.  Previously every specialize()
// path returned nullptr because the JIT engine DLL wasn't colocated
// and the test binary had no .topo_ir section; the atomic store step
// was therefore unreachable and any regression in the store path went
// undetected.
// ============================================================

#ifdef TOPO_TEST_HAS_LLVM_BITCODE
TEST_F(AdaptiveMonitorTest, ForceSpecializeBytesFiresAtomicDispatchStore) {
    topo::adaptive::init();

    void* jitPtr = nullptr;
    // The mangled name only matters for registry lookup; dispatch key
    // is pipelineName (second arg).
    topo_adaptive_register(
        "_ZN8topotest8pipelineEi", "topotest::pipeline",
        reinterpret_cast<void**>(&jitPtr), /*aotTTICost=*/1000);

    // Baseline: no specialization yet.
    auto before = topo::adaptive::stats();
    EXPECT_EQ(before.specializations, 0u);
    EXPECT_EQ(before.active_jit, 0u);
    EXPECT_EQ(jitPtr, nullptr);

    // Trigger the SPECIALIZING → ACTIVE path using pre-compiled bitcode.
    topo::adaptive::force_specialize_bytes(
        "topotest::pipeline",
        kTestPipelineBitcode, kTestPipelineBitcodeSize,
        /*metaJson=*/"");

    // Atomic dispatch store must have fired: pointer populated, counters
    // incremented.  If this fails, either specialize_bytes() returned
    // nullptr (JIT engine regression) or the commit path no longer
    // performs the atomic store (adaptive-side regression).
    auto after = topo::adaptive::stats();
    EXPECT_EQ(after.specializations, 1u);
    EXPECT_EQ(after.active_jit, 1u);
    EXPECT_NE(jitPtr, nullptr);

    // Invoke the dispatched function — must return the AOT-equivalent
    // answer, proving the atomic-stored pointer is both non-null AND
    // points at a real JIT-compiled function body (not uninitialized
    // memory or a stale pointer).
    if (jitPtr) {
        int seed = 7, acc = 42;
        for (int i = 0; i < 4; ++i) acc = acc * 31 + seed + i;
        auto fn = reinterpret_cast<int (*)(int)>(jitPtr);
        EXPECT_EQ(fn(42), acc);
    }
}

TEST_F(AdaptiveMonitorTest, ForceSpecializeBytesRespectsMaxVersions) {
    topo::adaptive::Config cfg;
    cfg.max_versions = 1;
    topo::adaptive::init(cfg);

    void* jitPtr = nullptr;
    topo_adaptive_register(
        "_ZN8topotest8pipelineEi", "topotest::pipeline",
        reinterpret_cast<void**>(&jitPtr), 1000);

    // First call: atomic store fires.
    topo::adaptive::force_specialize_bytes(
        "topotest::pipeline",
        kTestPipelineBitcode, kTestPipelineBitcodeSize, "");
    EXPECT_EQ(topo::adaptive::stats().specializations, 1u);
    ASSERT_NE(jitPtr, nullptr);

    void* firstPtr = jitPtr;

    // Second call: blocked by max_versions.  jitPtr stays unchanged.
    topo::adaptive::force_specialize_bytes(
        "topotest::pipeline",
        kTestPipelineBitcode, kTestPipelineBitcodeSize, "");
    EXPECT_EQ(topo::adaptive::stats().specializations, 1u);
    EXPECT_EQ(jitPtr, firstPtr);
}
#endif  // TOPO_TEST_HAS_LLVM_BITCODE

// ---- Stats snapshot during active monitoring ----
TEST_F(AdaptiveMonitorTest, StatsSnapshotDuringActiveMonitoring) {
    topo::adaptive::Config cfg;
    cfg.monitor_ms = 5;
    topo::adaptive::init(cfg);

    void* jitPtr = nullptr;
    topo_adaptive_register("_Z7statsrxv", "statsr::x",
                           reinterpret_cast<void**>(&jitPtr), 500);

    constexpr int kWorkerThreads = 4;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerThreads);

    // Dispatch-generating threads: repeatedly force_specialize.
    for (int t = 0; t < kWorkerThreads; ++t) {
        workers.emplace_back([&stop]() {
            while (!stop.load(std::memory_order_acquire)) {
                topo::adaptive::force_specialize("statsr::x");
            }
        });
    }

    // Stats reader thread: races with workers + monitor thread.
    std::atomic<uint64_t> snapshots{0};
    std::thread reader([&stop, &snapshots]() {
        while (!stop.load(std::memory_order_acquire)) {
            auto s = topo::adaptive::stats();
            // Values must be internally consistent (no torn reads
            // would show up as negative specializations, since they
            // are unsigned — TSan is the real check here).
            (void)s.specializations;
            (void)s.deoptimizations;
            (void)s.active_jit;
            snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    stop.store(true, std::memory_order_release);

    reader.join();
    for (auto& th : workers)
        th.join();

    // We should have observed many snapshots — if zero, the reader
    // thread never ran and the test is not exercising the race.
    EXPECT_GT(snapshots.load(), 0u);
}

// Regression for the JSON-escape fix: emitRecord previously
// wrote `pass`, `from`, `to`, `subject` into the NDJSON line via raw
// `%s`, so any user-controlled name (pipeline / lifetime-scope)
// containing `"`, `\`, or a control char broke topo-profile's
// record-aligned framing. Fields now route through
// topo::rt::writeJsonString; this test verifies an adversarial subject
// produces a single well-formed JSON line.
TEST(PassEventEscapeTest, AdversarialSubjectProducesValidJson) {
    std::fflush(stdout);
    std::FILE* tmp = std::tmpfile();
    ASSERT_NE(tmp, nullptr);
    int savedStdout = dup(fileno(stdout));
    ASSERT_NE(savedStdout, -1);
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    // Combine every char class the previous unescaped path would have
    // mangled: embedded double quote, backslash, newline, tab, control.
    // Explicit `""` after `\x01` terminates the C++ hex escape — otherwise
    // `\x01ctrl` parses as hex 0x01c (file sep) + `trl`, defeating the test.
    const char* nastySubject = "pipe\"line\\scope\nwith\there\x01" "ctrl";
    topo_pass_event_emit("AdaptiveDispatchPass", "aot", "jit", nastySubject);

    std::fflush(stdout);
    dup2(savedStdout, fileno(stdout));
    close(savedStdout);

    std::rewind(tmp);
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
        out.append(buf, n);
    std::fclose(tmp);

    ASSERT_FALSE(out.empty()) << "no pass-event record emitted";

    // The record MUST be exactly one line. The embedded `\n` in the
    // subject would split the line into two without the escaper —
    // breaking the one-record-per-line framing pass_event_rt.h
    // documents.
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1)
        << "extra newline — subject newline leaked through unescaped: "
        << out;

    EXPECT_NE(out.find("\\\""), std::string::npos)
        << "embedded \" not escaped in: " << out;
    EXPECT_NE(out.find("\\\\"), std::string::npos)
        << "embedded \\ not escaped in: " << out;
    EXPECT_NE(out.find("\\n"), std::string::npos)
        << "embedded newline not escaped in: " << out;
    EXPECT_NE(out.find("\\t"), std::string::npos)
        << "embedded tab not escaped in: " << out;
    EXPECT_NE(out.find("\\u0001"), std::string::npos)
        << "control char (\\u0001) not escaped in: " << out;
}

// Same coverage for the sized emitter: AdversarialFromTo + bytes.
TEST(PassEventEscapeTest, AdversarialFromToInSizedRecord) {
    std::fflush(stdout);
    std::FILE* tmp = std::tmpfile();
    ASSERT_NE(tmp, nullptr);
    int savedStdout = dup(fileno(stdout));
    ASSERT_NE(savedStdout, -1);
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    topo_pass_event_emit_sized("LifetimeArenaPass",
                               "he\"ap",        // from with embedded "
                               "arena\\rooted", // to with embedded backslash
                               "scope::main",
                               4096);

    std::fflush(stdout);
    dup2(savedStdout, fileno(stdout));
    close(savedStdout);

    std::rewind(tmp);
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
        out.append(buf, n);
    std::fclose(tmp);

    ASSERT_FALSE(out.empty()) << "no pass-event record emitted";
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1) << out;
    EXPECT_NE(out.find("\\\""), std::string::npos) << out;
    EXPECT_NE(out.find("\\\\"), std::string::npos) << out;
    EXPECT_NE(out.find("\"bytes\":4096"), std::string::npos)
        << "bytes field missing/corrupted: " << out;
}

} // namespace
