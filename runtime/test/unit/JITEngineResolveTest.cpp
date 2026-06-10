#include <gtest/gtest.h>
#include <topo/rt/jit_engine_resolve.h>

#include <algorithm>

using topo::jit::detail::engineSearchCandidates;

namespace {
const std::string kLib = "libtopo-jit-engine.so";
const std::string kExe = "/opt/topo/bin";

bool containsBareName(const std::vector<std::string>& candidates,
                      const std::string& libName) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const std::string& c) { return c == libName; });
}
} // namespace

// THE security invariant: a bare leaf name must never be produced, because the
// caller hands each candidate to dlopen/LoadLibrary and a bare name would let
// the OS loader search CWD / LD_LIBRARY_PATH / DYLD_* for an attacker-planted
// engine. Pre-fix, loadEngine() fell back to load(libName) with exactly this
// bare name; this test guards against that regression.
TEST(JITEngineResolveTest, NeverProducesBareLeafName) {
    EXPECT_FALSE(containsBareName(engineSearchCandidates(kExe, kLib, nullptr), kLib));
    EXPECT_FALSE(containsBareName(engineSearchCandidates(kExe, kLib, "/custom/engine.so"), kLib));
    // The degenerate case the old code most relied on: no exe dir known.
    EXPECT_FALSE(containsBareName(engineSearchCandidates("", kLib, nullptr), kLib));
}

TEST(JITEngineResolveTest, EmptyExeDirAndNoOverrideYieldsNoCandidates) {
    // Must be empty (caller then reports engine unavailable) — never a bare name.
    EXPECT_TRUE(engineSearchCandidates("", kLib, nullptr).empty());
}

TEST(JITEngineResolveTest, IncludesDevAndInstallLayoutPaths) {
    auto candidates = engineSearchCandidates(kExe, kLib, nullptr);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0], kExe + "/" + kLib);            // dev/build: next to tool
    EXPECT_EQ(candidates[1], kExe + "/../lib/" + kLib);     // install: bin/ -> ../lib/
}

TEST(JITEngineResolveTest, OverrideTakesPriority) {
    const char* override = "/etc/topo/libtopo-jit-engine.so";
    auto candidates = engineSearchCandidates(kExe, kLib, override);
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front(), override);
}

TEST(JITEngineResolveTest, EmptyOverrideStringIsIgnored) {
    // getenv may return a non-null empty string; treat it as unset.
    auto withEmpty = engineSearchCandidates(kExe, kLib, "");
    auto withNull = engineSearchCandidates(kExe, kLib, nullptr);
    EXPECT_EQ(withEmpty, withNull);
}
