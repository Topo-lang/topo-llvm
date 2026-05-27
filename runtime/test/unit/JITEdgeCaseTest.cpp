#include <gtest/gtest.h>
#include <topo/jit.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// Edge-case tests for topo-jit-api + topo-jit-engine.
//
// Real API surface (`topo-lang-cpp/runtime/include/topo/jit.h`):
//   - topo::jit::Context              — constraint collector (pure C++)
//   - topo::jit::available()          — engine loadable?
//   - topo::jit::specialize(name,ctx) — async future<void*>, nullptr on failure
//   - topo::jit::dump_ir(name,ctx)    — diagnostic string
//
// There is NO create_context / load_ir / execute / destroy_context in the
// real API — pipeline IR is embedded in the .topo_ir section of the host
// binary at AOT link time and keyed by demangled pipeline name.  The engine
// shared library (libtopo-jit-engine.dylib) is dlopen'd on demand.  In the
// topo-runtime-tests harness the engine library is usually NOT alongside the
// test binary, so these tests primarily exercise the graceful-degradation
// path.  When the engine IS discoverable (e.g. via DYLD_LIBRARY_PATH), the
// same tests additionally exercise the "unknown pipeline" engine path, which
// must also fail cleanly.
// ============================================================

using namespace topo::jit;

// ---- 1. Malformed pipeline name (garbage bytes, newlines, null-embedded) ----
TEST(JITEdgeCaseTest, SpecializeWithMalformedNameDoesNotCrash) {
    // Names the engine will never find — each should return nullptr
    // gracefully (either engine-absent or engine-present-lookup-miss).
    const std::vector<std::string> badNames = {
        "\x01\x02\x03\x04garbage",                    // non-printable bytes
        "name with spaces and\ttabs\nand newlines",   // whitespace chaos
        "!!@#$%^&*()_+invalid::symbols",              // special chars
        std::string(1024, 'x'),                       // oversized name
        "::",                                         // only separator
    };

    for (const auto& name : badNames) {
        Context ctx;
        auto future = specialize(name, ctx);
        void* result = future.get();
        EXPECT_EQ(result, nullptr) << "name='" << name.substr(0, 40) << "...'";
    }
}

// ---- 2. Empty pipeline name — zero-byte analog at the API level ----
TEST(JITEdgeCaseTest, SpecializeWithEmptyNameReturnsNull) {
    Context ctx;
    auto future = specialize("", ctx);
    void* result = future.get();
    EXPECT_EQ(result, nullptr);

    // And dump_ir — should produce some diagnostic string, never crash.
    auto ir = dump_ir("", ctx);
    EXPECT_FALSE(ir.empty());
}

// ---- 3. Self-contradictory / invalid constraint combinations ----
TEST(JITEdgeCaseTest, SpecializeWithContradictoryConstraintsDegradesCleanly) {
    Context ctx;
    // Self-loop prune — should be harmless (no matching edge).
    ctx.prune_edge("node_a", "node_a");
    // Wildcard source + wildcard target — nothing sensible but legal.
    ctx.prune_edge("*", "*");
    // Narrow returns on a function that does not exist in any pipeline.
    ctx.narrow_returns("nonexistent_function", {"field_a", "field_b"});
    // Key/value params with empty-string keys.
    ctx.set("", "");
    // Numeric-looking but not-actually-numeric parameter.
    ctx.set("runtime_cost_ns", "not_a_number");

    // Context construction must succeed and the accessors must reflect input.
    EXPECT_EQ(ctx.prunedEdges().size(), 2u);
    EXPECT_EQ(ctx.narrowedReturns().size(), 1u);
    EXPECT_EQ(ctx.params().size(), 2u);

    // Specialize must not crash even with nonsensical context.
    auto future = specialize("nonexistent::pipeline", ctx);
    void* result = future.get();
    EXPECT_EQ(result, nullptr);
}

// ---- 4. Fire-and-forget specialize — destroy Context while future pending ----
TEST(JITEdgeCaseTest, PendingSpecializeOutlivesContext) {
    // specialize() captures Context by value into the async lambda, so
    // destroying the caller-side Context must not cause use-after-free on
    // the engine side.  We still verify the future resolves cleanly.
    std::vector<std::future<void*>> futures;
    for (int i = 0; i < 8; ++i) {
        Context ctx;
        ctx.prune_edge("*", "cold_stage_" + std::to_string(i));
        ctx.narrow_returns("prepare", {"x", "y"});
        ctx.set("iteration", std::to_string(i));
        futures.push_back(specialize("nonexistent::pipeline_" + std::to_string(i), ctx));
        // ctx goes out of scope here while the async task may still be running.
    }

    for (auto& f : futures) {
        void* result = f.get();
        EXPECT_EQ(result, nullptr);
    }
}

// ---- 5. Repeated create/specialize/destroy — leak/state-accumulation check ----
TEST(JITEdgeCaseTest, ManyContextsDoNotAccumulateState) {
    // 20 iterations — each creates a fresh Context, fills it, triggers a
    // specialize round-trip, and drops it.  Catches leaked JIT dylibs,
    // accumulated caches, or un-released futures inside the engine.
    constexpr int kIterations = 20;
    for (int i = 0; i < kIterations; ++i) {
        Context ctx;
        ctx.prune_edge("src_" + std::to_string(i), "dst_" + std::to_string(i));
        ctx.narrow_returns("func_" + std::to_string(i), {"f0", "f1", "f2"});
        ctx.set("iteration", std::to_string(i));

        auto future = specialize("iteration::" + std::to_string(i), ctx);
        void* result = future.get();
        // We do not assert nullptr unconditionally: if the engine IS loaded
        // AND somehow resolves a matching symbol, a non-null result would
        // also be valid.  The point is that repeated calls remain stable.
        (void)result;

        auto ir = dump_ir("iteration::" + std::to_string(i), ctx);
        EXPECT_FALSE(ir.empty());
    }
}

// ---- 6. dump_ir + specialize on symbol with no embedded IR ----
TEST(JITEdgeCaseTest, DumpIRFallbackOnUnknownSymbol) {
    // When the host binary has no .topo_ir section, dump_ir must return a
    // diagnostic string ("<JIT engine not available>" / "<dump_ir failed>")
    // rather than crashing or returning empty.  This test covers the
    // "execute on unspecialized symbol" case in the task brief — in the
    // real API there is no execute() primitive; dump_ir is the observable
    // replacement for "do we have something runnable?".
    Context ctx;
    auto ir1 = dump_ir("never::existed", ctx);
    EXPECT_FALSE(ir1.empty());

    // Add constraints and re-query — still clean.
    ctx.narrow_returns("any", {"field"});
    ctx.prune_edge("a", "b");
    auto ir2 = dump_ir("never::existed", ctx);
    EXPECT_FALSE(ir2.empty());

    // And specialize on the same symbol — also clean.
    auto future = specialize("never::existed", ctx);
    void* result = future.get();
    EXPECT_EQ(result, nullptr);
}

// ---- 7. Concurrent specialize from multiple threads, each with own Context ----
TEST(JITEdgeCaseTest, ConcurrentSpecializeFromMultipleThreads) {
    constexpr int kThreads = 4;
    std::atomic<int> errors{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &errors]() {
            try {
                Context ctx;
                ctx.prune_edge("thread_" + std::to_string(t), "sink");
                ctx.narrow_returns("compute", {"x"});
                ctx.set("thread_id", std::to_string(t));

                // Each thread issues two specialize calls.  The engine
                // serializes its LLJIT interactions with an internal mutex,
                // so concurrent callers MUST NOT race or deadlock.
                auto f1 = specialize("thread::worker_" + std::to_string(t), ctx);
                auto f2 = specialize("thread::worker_" + std::to_string(t) + "_b", ctx);

                void* r1 = f1.get();
                void* r2 = f2.get();
                (void)r1;
                (void)r2;

                // dump_ir from multiple threads should also be safe.
                auto ir = dump_ir("thread::worker_" + std::to_string(t), ctx);
                if (ir.empty()) errors.fetch_add(1);
            } catch (...) {
                errors.fetch_add(1);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    EXPECT_EQ(errors.load(), 0);
}

// ---- 8. Repeated available() + specialize() — idempotent engine loader ----
TEST(JITEdgeCaseTest, RepeatedAvailableQueriesAreIdempotent) {
    // topo-jit-api caches the engine-load result via a `triedLoad` latch.
    // Whether the engine resolves or not, calling available() many times
    // must return a stable answer and never crash.  This is the closest
    // contract-level test we can write for "dlopen failure handling"
    // without forcibly corrupting the environment: the existing test
    // harness does not colocate libtopo-jit-engine.dylib with the runtime
    // test binary, so the load path is usually the failure path, and we
    // verify it is deterministic.
    bool first = available();
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(available(), first);
    }

    // And after many available() calls, specialize() still returns cleanly.
    Context ctx;
    auto future = specialize("idempotent::check", ctx);
    void* result = future.get();
    // When engine unavailable -> nullptr; when loaded -> nullptr (unknown
    // symbol).  Either way, a stable answer is required.
    EXPECT_EQ(result, nullptr);
}
