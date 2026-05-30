// Real-path JIT coverage for topo-jit-api + topo-jit-engine.
//
// Unlike JITApiStubTest (which verifies nullptr degradation paths) and
// JITEdgeCaseTest (which stress-tests nonsense inputs), this file drives
// the full IR → LLVM JIT → dispatch pointer atomic-store chain with a
// pre-compiled bitcode fixture embedded at build time (see
// test_pipeline_bitcode.cmake).
//
// This closes a gap where the previous test suite never exercised the
// real specialize path, so any regression in bitcode parsing / LLJIT
// compile / atomic store would have gone undetected.

#include <gtest/gtest.h>
#include <topo/jit.h>

#include <cstddef>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include "test_pipeline_bitcode.h"

namespace {

// Reference implementation that mirrors fixtures/test_pipeline_source.cpp.
// Both AOT and JIT paths must agree on this output.
int referencePipeline(int input) {
    int seed = 7;
    int acc = input;
    for (int i = 0; i < 4; ++i) {
        acc = acc * 31 + seed + i;
    }
    return acc;
}

using PipelineFn = int (*)(int);

TEST(JITRealPathTest, EngineIsAvailableWithColocatedDll) {
    ASSERT_TRUE(topo::jit::available())
        << "libtopo-jit-engine must be colocated with the test binary — "
           "see topo-llvm/runtime/test/CMakeLists.txt POST_BUILD copy.";
}

TEST(JITRealPathTest, SpecializeBytesReturnsCallablePointer) {
    topo::jit::Context ctx;

    auto future = topo::jit::specialize_bytes(
        "topotest::pipeline",
        kTestPipelineBitcode, kTestPipelineBitcodeSize,
        /*metaJson=*/"",
        ctx);
    void* ptr = future.get();
    ASSERT_NE(ptr, nullptr)
        << "JIT engine failed to compile the fixture bitcode — regression in "
           "bitcode parse or LLJIT lookup path.";

    auto fn = reinterpret_cast<PipelineFn>(ptr);
    // Sanity-check a handful of inputs: JIT output must match the
    // reference implementation exactly.
    for (int input : {0, 1, 2, 17, -3, 100, -100}) {
        int jit = fn(input);
        int aot = referencePipeline(input);
        EXPECT_EQ(jit, aot)
            << "JIT/AOT divergence at input=" << input
            << " — suggests pass pipeline corrupted IR or linkage broke.";
    }
}

TEST(JITRealPathTest, RepeatedSpecializeBytesIsStable) {
    // Re-invoking specialize_bytes must succeed repeatedly — caches and
    // per-call JITDylib allocation must not leak or collide.
    topo::jit::Context ctx;
    for (int round = 0; round < 4; ++round) {
        auto future = topo::jit::specialize_bytes(
            "topotest::pipeline",
            kTestPipelineBitcode, kTestPipelineBitcodeSize,
            "", ctx);
        void* ptr = future.get();
        ASSERT_NE(ptr, nullptr) << "round " << round;

        auto fn = reinterpret_cast<PipelineFn>(ptr);
        EXPECT_EQ(fn(round * 3 + 1), referencePipeline(round * 3 + 1))
            << "round " << round;
    }
}

TEST(JITRealPathTest, DumpIRBytesReturnsTextualIR) {
    auto ir = topo::jit::dump_ir_bytes(kTestPipelineBitcode, kTestPipelineBitcodeSize);
    EXPECT_FALSE(ir.empty());
    // A valid textual IR module should mention `define` and the
    // fixture's seed symbol.  Exact symbol mangling is implementation-
    // defined, so we check for the underlying linkage name the
    // fixture exposes via `extern "C"`.
    EXPECT_NE(ir.find("define"), std::string::npos)
        << "dump_ir_bytes did not produce textual LLVM IR; got: "
        << ir.substr(0, 200);
    EXPECT_NE(ir.find("topotest_pipeline_seed"), std::string::npos)
        << "fixture symbol missing from dumped IR";
}

TEST(JITRealPathTest, SpecializeBytesWithUnknownNameFailsCleanly) {
    topo::jit::Context ctx;
    auto future = topo::jit::specialize_bytes(
        "topotest::does_not_exist",
        kTestPipelineBitcode, kTestPipelineBitcodeSize,
        "", ctx);
    void* ptr = future.get();
    EXPECT_EQ(ptr, nullptr);
}

TEST(JITRealPathTest, SpecializeBytesWithCorruptedBitcodeFailsCleanly) {
    // First 16 bytes flipped to non-bitcode data — parseBitcodeFile
    // should reject cleanly without crashing.
    unsigned char corrupted[32] = {};
    std::memcpy(corrupted, kTestPipelineBitcode, sizeof(corrupted));
    for (int i = 0; i < 8; ++i) corrupted[i] = 0xff;

    topo::jit::Context ctx;
    auto future = topo::jit::specialize_bytes(
        "topotest::pipeline",
        corrupted, sizeof(corrupted),
        "", ctx);
    void* ptr = future.get();
    EXPECT_EQ(ptr, nullptr);
}

// Regression for the specialize_bytes pointer-lifetime fix: the original lambda
// captured the caller's `const void* irBytes` by raw value, so any
// caller that stored the future and freed the bytes before .get() had
// a latent use-after-free. The implementation now COPIES the bytes
// into a std::vector captured by the lambda; the caller is free to
// free its source buffer the instant specialize_bytes returns.
//
// This test pins the new contract: scope-allocate a copy of the
// fixture bitcode, hand it to specialize_bytes, free the scope copy
// BEFORE .get(), and assert the future still resolves to a callable
// pointer.
TEST(JITRealPathTest, SpecializeBytesCallerMayFreeBufferBeforeGet) {
    std::future<void*> future;
    {
        // scope-allocated, freed at the closing brace
        std::vector<unsigned char> scopeBuf(
            kTestPipelineBitcode,
            kTestPipelineBitcode + kTestPipelineBitcodeSize);
        topo::jit::Context ctx;
        future = topo::jit::specialize_bytes(
            "topotest::pipeline",
            scopeBuf.data(), scopeBuf.size(),
            /*metaJson=*/"",
            ctx);
        // scopeBuf goes out of scope here, BEFORE the future is awaited.
    }
    void* ptr = future.get();
    ASSERT_NE(ptr, nullptr)
        << "future.get() failed after the source buffer went out of "
           "scope — specialize_bytes is supposed to OWN the bytes via "
           "an internal copy, not borrow them.";
    auto fn = reinterpret_cast<PipelineFn>(ptr);
    EXPECT_EQ(fn(11), referencePipeline(11));
}

// Regression for the JIT counter race:
// doSpecializeImpl built the per-call JITDylib name from a static int
// counter that was incremented OUTSIDE g_jit_mutex. Concurrent
// specialize_bytes calls on the same mangled function could read the
// same counter, build identical jitNames, and the second
// createJITDylib(jitName) would fail with duplicate-key. Counter is now
// std::atomic<int> with fetch_add. Without the fix at least one of the
// concurrent specialize_bytes calls returns nullptr; with the fix all
// return distinct callable function pointers.
TEST(JITRealPathTest, ConcurrentSpecializeBytesAllSucceed) {
    constexpr int kCount = 8;
    std::vector<std::future<void*>> futures;
    futures.reserve(kCount);
    topo::jit::Context ctx;

    for (int i = 0; i < kCount; ++i) {
        futures.push_back(topo::jit::specialize_bytes(
            "topotest::pipeline",
            kTestPipelineBitcode, kTestPipelineBitcodeSize,
            /*metaJson=*/"",
            ctx));
    }

    std::vector<void*> ptrs;
    ptrs.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        void* p = futures[i].get();
        ASSERT_NE(p, nullptr)
            << "Concurrent specialize_bytes #" << i << " returned nullptr — "
               "suggests the JIT counter race regressed (duplicate jitName "
               "→ createJITDylib duplicate-key failure).";
        ptrs.push_back(p);
    }

    // Sanity-check each pointer is independently callable. Distinctness
    // is the per-JITDylib invariant; even if two ended up at the same
    // address they must each compute the reference value correctly.
    for (int i = 0; i < kCount; ++i) {
        auto fn = reinterpret_cast<PipelineFn>(ptrs[i]);
        EXPECT_EQ(fn(i), referencePipeline(i))
            << "Concurrent specialize_bytes #" << i << " produced wrong output";
    }
}

} // namespace
