#include <gtest/gtest.h>
#include <topo/jit.h>

using namespace topo::jit;

// The runtime test binary has the engine DLL copied alongside it by
// CMake (see topo-llvm/runtime/test/CMakeLists.txt), so available()
// MUST now return true.  These tests verify the engine-absent surface
// degrades the way production code will still need when a stripped
// deployment omits topo-jit-engine, and that section-less binaries
// still fail cleanly.

TEST(JITApiStubTest, AvailableReturnsTrueWithColocatedEngine) {
    // loadEngine() searches alongside the executable first; CMake
    // POST_BUILD copies libtopo-jit-engine next to topo-runtime-tests.
    // The colocation step itself is gated on TOPO_ENABLE_LLVM (the engine
    // target only exists with the LLVM backend), so this assertion only
    // applies when the bitcode-test bundle is also active — same gate as
    // JITRealPathTest. Without LLVM, the engine is intentionally absent
    // and available() correctly returns false; skip rather than fail.
#ifdef TOPO_TEST_HAS_LLVM_BITCODE
    EXPECT_TRUE(available());
#else
    GTEST_SKIP() << "topo-jit-engine not built (TOPO_ENABLE_LLVM=OFF)";
#endif
}

TEST(JITApiStubTest, SpecializeReturnsNullptrWithoutEmbeddedIR) {
    // The test binary has no .topo_ir section, so specialize must
    // still fail cleanly — even with the engine loadable.
    Context ctx;
    auto future = specialize("nonexistent::pipeline", ctx);
    void* result = future.get();
    EXPECT_EQ(result, nullptr);
}

TEST(JITApiStubTest, DumpIRReturnsDiagnosticStringWithoutEmbeddedIR) {
    // Section-reading path fails → diagnostic string, never crash.
    Context ctx;
    auto ir = dump_ir("nonexistent::pipeline", ctx);
    EXPECT_FALSE(ir.empty());
}

TEST(JITApiStubTest, ContextMethodsWorkWithoutEngine) {
    // Context is pure C++, no engine dependency.
    Context ctx;
    ctx.prune_edge("a", "b");
    ctx.narrow_returns("f", {"x", "y"});
    ctx.set("key", "value");

    EXPECT_EQ(ctx.prunedEdges().size(), 1u);
    EXPECT_EQ(ctx.narrowedReturns().size(), 1u);
    EXPECT_EQ(ctx.params().size(), 1u);
}
