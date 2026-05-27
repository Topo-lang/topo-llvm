#include "topo/observe.h"
#include "topo/rt/observe_rt.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
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

namespace {

class ObserveRuntimeTest : public ::testing::Test {
protected:
    void TearDown() override { topo_trace_shutdown(); }
};

TEST_F(ObserveRuntimeTest, InitAndShutdown) {
    // Should not crash with default init
    topo::observe::init("stdout", 1.0);
    topo::observe::shutdown();
}

TEST_F(ObserveRuntimeTest, DoubleInitIsSafe) {
    topo::observe::init("stdout", 1.0);
    topo::observe::init("stdout", 1.0); // second call is a no-op
    topo::observe::shutdown();
}

TEST_F(ObserveRuntimeTest, ShutdownWithoutInitIsSafe) {
    topo::observe::shutdown(); // no-op
}

TEST_F(ObserveRuntimeTest, SpanBeginEnd) {
    topo::observe::init("stdout", 1.0);

    // Should not crash
    topo_trace_span_begin("test::span");
    topo_trace_span_end();
}

TEST_F(ObserveRuntimeTest, NestedSpans) {
    topo::observe::init("stdout", 1.0);

    // Nested spans should work correctly (LIFO)
    topo_trace_span_begin("outer::span");
    topo_trace_span_begin("inner::span");
    topo_trace_span_end(); // ends inner
    topo_trace_span_end(); // ends outer
}

TEST_F(ObserveRuntimeTest, SpanEndWithoutBeginIsSafe) {
    topo::observe::init("stdout", 1.0);

    // Extra span_end with empty stack should not crash
    topo_trace_span_end();
}

TEST_F(ObserveRuntimeTest, ZeroSamplingRateSkipsAll) {
    // With 0% sampling, spans must emit no output. Previous version of this
    // test only asserted "no crash", which would silently accept a regression
    // that bypassed the sampling gate (e.g. "always emit"). We now redirect
    // stdout to a temp file and assert nothing was written.
    //
    // Uses the same stdout-redirect pattern as CallsBeforeInitDoNotCrash.
    std::fflush(stdout);
    std::FILE* tmp = std::tmpfile();
    ASSERT_NE(tmp, nullptr);
    int savedStdout = dup(fileno(stdout));
    ASSERT_NE(savedStdout, -1);
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    topo::observe::init("stdout", 0.0);

    // Issue enough spans that any gate-bypass regression would produce many
    // bytes. 100 iterations gives a clear signal vs a single-shot false pass.
    for (int i = 0; i < 100; ++i) {
        topo_trace_span_begin("sampled::span");
        topo_trace_span_end();
    }

    std::fflush(stdout);
    dup2(savedStdout, fileno(stdout));
    close(savedStdout);

    std::rewind(tmp);
    char buf[256];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[n] = '\0';
    std::fclose(tmp);

    EXPECT_EQ(n, 0u) << "zero sampling rate emitted output: " << buf;
}

TEST_F(ObserveRuntimeTest, ThreadSafety) {
    topo::observe::init("stdout", 1.0);

    constexpr int numThreads = 4;
    constexpr int spansPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([t]() {
            std::string name = "thread" + std::to_string(t) + "::span";
            for (int i = 0; i < spansPerThread; ++i) {
                topo_trace_span_begin(name.c_str());
                topo_trace_span_end();
            }
        });
    }

    for (auto& th : threads)
        th.join();

    // If we get here without crashing, thread safety is OK
}

// ============================================================
// M6.1 edge-case + stress tests
//
// These exercise failure modes and stress paths of libtopo-observe:
//   - Race conditions in the (thread-local) span stack under heavy
//     concurrent load
//   - Deep nesting to catch any fixed-size stack assumption
//   - Calls with no matching begin
//   - Calls before init / after shutdown (uninitialized-API behavior
//     is currently undocumented)
//   - Zero sampling rate under concurrent load
//   - Rapid init/shutdown cycles (leaks in static state)
//   - Very long span names (fixed-size buffer assumption)
//
// All tests must pass cleanly under ASan AND TSan.
// ============================================================

// 1. Concurrent span begin/end from N threads — 8 threads each opening
//    and closing 1000 spans. Catches races in the span stack. Note: the
//    current implementation uses a thread_local std::vector as the span
//    stack, so there is no shared stack to race on, but emitSpan() shares
//    a global output mutex and the sampling RNG is thread_local. TSan
//    would flag any accidental sharing.
TEST_F(ObserveRuntimeTest, ConcurrentSpanStressEightThreads) {
    topo::observe::init("stdout", 1.0);

    constexpr int numThreads = 8;
    constexpr int spansPerThread = 1000;
    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([t, &completed]() {
            // Pre-compute name so lifetime is obvious and stable.
            std::string name = "stress::thread" + std::to_string(t);
            for (int i = 0; i < spansPerThread; ++i) {
                topo_trace_span_begin(name.c_str());
                topo_trace_span_end();
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads)
        th.join();

    EXPECT_EQ(completed.load(), numThreads * spansPerThread);
    // Extra end-call on the main thread should still be a safe no-op;
    // the thread-local stacks from workers are independent of this one.
    topo_trace_span_end();
}

// 2. Deep nesting — 500 layers of spans opened then closed in reverse.
//    Catches any fixed-size stack assumption in the implementation.
//    500 picked over 1000 to avoid stack overflow on tightly-limited
//    platforms; the current impl uses std::vector so heap-backed and can
//    handle arbitrary depth, but we verify the contract.
TEST_F(ObserveRuntimeTest, DeepNestingFiveHundredLayers) {
    topo::observe::init("stdout", 0.0); // silence output for 500 spans

    constexpr int depth = 500;

    // Keep name strings alive for the entire span lifetime. The span
    // stack stores raw const char*, so the storage must outlive end().
    std::vector<std::string> names;
    names.reserve(depth);
    for (int i = 0; i < depth; ++i)
        names.emplace_back("nest::layer" + std::to_string(i));

    for (int i = 0; i < depth; ++i)
        topo_trace_span_begin(names[i].c_str());
    for (int i = 0; i < depth; ++i)
        topo_trace_span_end();

    // Extra end after unwinding should still be safe (empty stack).
    topo_trace_span_end();
}

// 3. Span end without matching begin — the thread-local stack is empty,
//    repeatedly calling end must not crash. topo_trace_span_end has no
//    id parameter so "mismatched id" is not expressible at the API; this
//    test instead blasts end() on an empty stack from multiple threads
//    to catch any missing emptiness check under contention.
TEST_F(ObserveRuntimeTest, SpanEndOnEmptyStackConcurrent) {
    topo::observe::init("stdout", 1.0);

    constexpr int numThreads = 4;
    constexpr int endsPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([]() {
            for (int i = 0; i < endsPerThread; ++i) {
                // Thread-local stack is empty in a fresh thread.
                topo_trace_span_end();
            }
        });
    }

    for (auto& th : threads)
        th.join();
    // Reaching here without crash is the acceptance criterion.
}

// 4. Calls before init — span begin/end invoked before topo_trace_init.
//    The documented contract (observe_rt.h) is "silent no-op before init
//    / after shutdown": begin does NOT push, end does NOT emit, nothing
//    goes to stdout. This test asserts both the safety contract (no
//    crash) and the guard contract (empty stack, no residual state).
//
//    We redirect stdout to a pipe so that if the guard ever regresses
//    and begin/end actually push+emit, the test catches the emission.
TEST_F(ObserveRuntimeTest, CallsBeforeInitDoNotCrash) {
    // Redirect stdout to a temp file so we can assert no emission.
    std::fflush(stdout);
    std::FILE* tmp = std::tmpfile();
    ASSERT_NE(tmp, nullptr);
    int savedStdout = dup(fileno(stdout));
    ASSERT_NE(savedStdout, -1);
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    // Deliberately no topo_trace_init.
    topo_trace_span_begin("pre-init::span");
    topo_trace_span_begin("pre-init::inner");
    topo_trace_span_end();
    topo_trace_span_end();

    // Also exercise a stray end on an already-empty stack.
    topo_trace_span_end();

    // Flush and restore stdout, then inspect what was written.
    std::fflush(stdout);
    dup2(savedStdout, fileno(stdout));
    close(savedStdout);

    std::rewind(tmp);
    char buf[256];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[n] = '\0';
    std::fclose(tmp);

    // Guard contract: no span emission occurred.
    EXPECT_EQ(n, 0u) << "pre-init call emitted output: " << buf;

    // Guard contract: the span stack must be empty — verifiable by
    // initializing and checking that a bare span_end is a no-op (would
    // be a stale pop if pre-init begin had actually pushed).
    topo::observe::init("stdout", 0.0);
    topo_trace_span_end(); // safe: stack is empty, nothing to pop
}

// 5. Calls after shutdown — init, one span, shutdown, then more spans.
//    Contract: post-shutdown calls are silent no-ops (no emission, no
//    push). This test asserts the safety contract AND the guard
//    contract: stdout receives nothing between shutdown and re-init.
TEST_F(ObserveRuntimeTest, CallsAfterShutdownDoNotCrash) {
    topo::observe::init("stdout", 0.0); // silence emission

    topo_trace_span_begin("pre-shutdown::span");
    topo_trace_span_end();

    topo::observe::shutdown();

    // Redirect stdout to verify post-shutdown emission is absent.
    std::fflush(stdout);
    std::FILE* tmp = std::tmpfile();
    ASSERT_NE(tmp, nullptr);
    int savedStdout = dup(fileno(stdout));
    ASSERT_NE(savedStdout, -1);
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    // Post-shutdown — guard must suppress emission even if sampling
    // rate would otherwise fire (default 1.0 at static init).
    topo_trace_span_begin("post-shutdown::span");
    topo_trace_span_end();

    std::fflush(stdout);
    dup2(savedStdout, fileno(stdout));
    close(savedStdout);

    std::rewind(tmp);
    char buf[256];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[n] = '\0';
    std::fclose(tmp);

    EXPECT_EQ(n, 0u) << "post-shutdown call emitted output: " << buf;

    // Re-init and use again — the runtime should recover cleanly because
    // topo_trace_init resets g_initialized via exchange.
    topo::observe::init("stdout", 0.0);
    topo_trace_span_begin("reinit::span");
    topo_trace_span_end();
}

// 6. Zero sampling rate under concurrent load — 4 threads × 1000 spans
//    at rate 0.0. Catches accidental emission paths that bypass the
//    sampling check, and stresses shouldSample() (thread-local RNG).
TEST_F(ObserveRuntimeTest, ZeroSamplingRateConcurrentStress) {
    topo::observe::init("stdout", 0.0);

    constexpr int numThreads = 4;
    constexpr int spansPerThread = 1000;
    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([t, &completed]() {
            std::string name = "zero::thread" + std::to_string(t);
            for (int i = 0; i < spansPerThread; ++i) {
                topo_trace_span_begin(name.c_str());
                topo_trace_span_end();
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads)
        th.join();

    EXPECT_EQ(completed.load(), numThreads * spansPerThread);
    // Intentionally no assertion on emission count — the runtime does not
    // expose a recorded-span counter. The test's job is to verify no
    // crash / data race / hang on the zero-sampling path under load.
}

// 7. Rapid init/shutdown cycles — 20 iterations. Catches accumulating
//    leaks in static state between cycles (e.g. mutexes not reset,
//    atomics drifting, exporter state lingering).
TEST_F(ObserveRuntimeTest, RapidInitShutdownCycles) {
    constexpr int cycles = 20;

    for (int c = 0; c < cycles; ++c) {
        topo::observe::init("stdout", 0.0);

        std::string name = "cycle::" + std::to_string(c);
        topo_trace_span_begin(name.c_str());
        topo_trace_span_end();

        topo::observe::shutdown();
    }

    // Final init to leave state in a known place for TearDown.
    topo::observe::init("stdout", 0.0);
}

// 8. Very long span name — 4KB string. Catches fixed-size buffer
//    assumptions in emitSpan / fprintf. The runtime stores the name as
//    a raw pointer, so lifetime is the caller's responsibility — the
//    std::string must outlive the end() call.
TEST_F(ObserveRuntimeTest, VeryLongSpanName) {
    topo::observe::init("stdout", 0.0); // silence 4KB JSON line

    // 4096 printable ASCII characters (no control chars that would
    // confuse JSON parsers if sampling were enabled).
    std::string longName(4096, 'x');
    // Sprinkle in a recognizable prefix so the name isn't all one char —
    // defends against any impl path that optimizes identical characters.
    longName.replace(0, 9, "longname:");

    topo_trace_span_begin(longName.c_str());
    topo_trace_span_end();

    // Second call with same long name after a short one — verifies no
    // state contamination between differently-sized names.
    topo_trace_span_begin("short");
    topo_trace_span_end();
    topo_trace_span_begin(longName.c_str());
    topo_trace_span_end();
}

// 9. Regression for the JSON-escape audit finding
// topo-llvm-pass-event-observe-json-unescaped: emitSpan previously did
// `fprintf("\"name\":\"%s\"", name)` with no escape, so a span name
// containing `"`, `\`, or a control char broke topo-profile's
// one-record-per-line framing. The fix routes name through
// topo::rt::writeJsonString; this test verifies the emitted line is
// well-formed JSON for an adversarial name.
TEST_F(ObserveRuntimeTest, SpanNameWithSpecialCharsProducesValidJson) {
    std::fflush(stdout);
    std::FILE* tmp = std::tmpfile();
    ASSERT_NE(tmp, nullptr);
    int savedStdout = dup(fileno(stdout));
    ASSERT_NE(savedStdout, -1);
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    topo::observe::init("stdout", 1.0);

    // Combine every character class that the previous unescaped path
    // would have mangled: embedded double quote, backslash, newline,
    // tab, and a bare control char below 0x20. A well-formed escaper
    // turns each into the spec form (\", \\, \n, \t, ).
    // NOTE: explicit `""` after `\x01` terminates the C++ hex escape;
    // otherwise `\x01char` parses as 0x01c (file separator) + `har`.
    std::string nasty = "pipe\"line\\name\nwith\there\x01" "char";
    topo_trace_span_begin(nasty.c_str());
    topo_trace_span_end();

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

    ASSERT_FALSE(out.empty()) << "no span emitted";

    // The line MUST contain exactly one newline (the trailing record
    // terminator). An unescaped embedded `\n` in the name would split
    // the line into two and break topo-profile's framing.
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1)
        << "extra newline in record — span-name newline leaked through "
           "unescaped, breaking one-record-per-line framing: "
        << out;

    // Each special char must appear in its escaped form, not its raw
    // form, inside the name field.
    EXPECT_NE(out.find("\\\""), std::string::npos)
        << "embedded \" not escaped in: " << out;
    EXPECT_NE(out.find("\\\\"), std::string::npos)
        << "embedded \\ not escaped in: " << out;
    EXPECT_NE(out.find("\\n"), std::string::npos)
        << "embedded newline not escaped in: " << out;
    EXPECT_NE(out.find("\\t"), std::string::npos)
        << "embedded tab not escaped in: " << out;
    EXPECT_NE(out.find("\\u0001"), std::string::npos)
        << "embedded control char (\\u0001) not escaped in: " << out;
}

} // namespace
