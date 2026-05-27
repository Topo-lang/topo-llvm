#ifndef TOPO_TEST_E2E_HARNESS_H
#define TOPO_TEST_E2E_HARNESS_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace topo::test::e2e {

struct RunResult {
    int exitCode = -1;
    std::string output;
};

// ---------------------------------------------------------------------------
// Benchmark sampling primitives
// ---------------------------------------------------------------------------

/// One measurement of a benchmark workload.
struct BenchSample {
    double us = 0.0; // single-run measured microseconds
    int runIdx = 0;  // 0-based sample index
};

/// Aggregate statistics over a sequence of `BenchSample`s.
///
/// Produced by `measureWithVarianceAdapt`. `resampleCapHit` is true iff the
/// `maxRuns` cap was reached before the coefficient of variation fell at or
/// below the target threshold — callers that care about signal reliability
/// MUST inspect this flag.
struct BenchStats {
    double median = 0.0;
    double mean = 0.0;
    double stdev = 0.0;            // sample stdev, sqrt(Σ(xᵢ-μ)²/(n-1))
    double cv = 0.0;               // stdev / mean
    int runs = 0;
    bool resampleCapHit = false;   // true if maxRuns reached before CV ≤ target
    std::vector<BenchSample> samples;
};

/// Measure `fn` repeatedly with variance adaptation.
///
/// - Run at least `minRuns`
/// - If CV > `cvTarget`, continue until CV ≤ cvTarget or `maxRuns`
/// - Returns `BenchStats` with `resampleCapHit=true` if cap reached
///
/// `fn` returns microseconds for one run; a return value ≤ 0 indicates the
/// sample failed (missing output / run error) and is recorded but excluded
/// from statistics. If all samples fail the resulting stats are zero-valued
/// and `resampleCapHit` is true.
BenchStats measureWithVarianceAdapt(std::function<double()> fn,
                                    int minRuns = 3,
                                    int maxRuns = 10,
                                    double cvTarget = 0.05);

/// Interleaved variance-adaptive measurement of N labelled probes.
///
/// Same per-probe semantics as `measureWithVarianceAdapt`, but instead of
/// taking all samples of `fn[0]` first and only then moving on to `fn[1]`,
/// this rotates through every probe on each round:
///
///   round 1: fn[0]() fn[1]() ... fn[k-1]()
///   round 2: fn[0]() fn[1]() ... fn[k-1]()
///   ...
///
/// Why: the sequential variant lets transient background CPU load
/// (Spotlight indexer, Time Machine, browser tab) bias one mode versus
/// another — when vanilla is measured during a quiet 200 ms window and
/// auto is measured during a noisy 200 ms window, the resulting
/// ratio is a property of when each window opened, not of the
/// compilation pipeline. Round-robin averages background drift across
/// all probes so the *relative* numbers a ratio depends on stay
/// comparable run-over-run.
///
/// Convergence rule: stop after `minRuns` rounds once every probe with
/// at least two valid samples has `cv <= cvTarget`. Otherwise cap at
/// `maxRuns` rounds (each round = one sample per probe) and set
/// `resampleCapHit = true` on every returned `BenchStats`.
///
/// Returns a vector of `BenchStats` in the same order as `fns`. Empty
/// probes (e.g. an optional vanilla slot) are still represented as
/// zero-runs `BenchStats` entries so caller indexing stays stable.
std::vector<BenchStats> measureWithVarianceAdaptInterleaved(
    std::vector<std::function<double()>> fns,
    int minRuns = 3,
    int maxRuns = 10,
    double cvTarget = 0.05);

/// Benchmark seed accessor.
///
/// Resolved lazily from the `TOPO_BENCH_SEED` env var (or `std::random_device`
/// when unset). Printed once via `[ SEED   ]` on first call and propagated to
/// `::testing::GTEST_FLAG(random_seed)` so GTest test-order shuffling (when
/// enabled via `--gtest_shuffle`) reproduces.
///
/// IMPORTANT — what the seed does NOT control:
///   * benchmark workload timings (no benchmark consumes this seed; the
///     workloads themselves are deterministic in computation),
///   * variance between vanilla/base/auto/forced runs of the same binary
///     (driven by OS scheduler, CPU frequency / thermals, page-cache
///     state — none of which a seed reaches).
///
/// In other words: setting the same seed across runs reproduces the
/// *order in which tests execute* and any seeded RNG inside test bodies
/// (currently none), but it does NOT reproduce absolute microsecond
/// timings or ratios. Reproducing those requires environmental
/// stability (idle machine, fixed CPU governor, no background indexers)
/// — fundamentally outside the scope of an in-process PRNG seed.
std::uint32_t benchSeed();

// ---------------------------------------------------------------------------
// Category assertion helpers
// ---------------------------------------------------------------------------
//
// Each helper applies the relevant ratio thresholds for its feature
// category, emits `[ WARN   ]` / `[ ERROR  ]` coloured output lines, and
// promotes ERRORs to `ADD_FAILURE()` so CTest treats the case as failed.
//
// Absolute rules (not category-exempt):
//   1. auto/base > 1.10                     -> unconditional ERROR
//   2. forced without IR/bytecode diff      -> enforced in equivalence layer
//   3. absolute time < 10ms (10000 us)      -> skip all threshold checks
//   4. resampleCapHit (10 runs, CV > 0.05)  -> downgrade ERROR -> WARN

/// Workload flavour — the thresholds for OPT differ between friendly
/// (expected speedup) and unfriendly (expected neutral / mild slowdown).
enum class Workload { Friendly, Unfriendly };

void assertOptCategoryContract(const BenchStats& vanilla,
                               const BenchStats& base,
                               const BenchStats& autoStats,
                               const BenchStats& forced,
                               Workload workload,
                               const char* passName);

void assertEnhanceCategoryContract(const BenchStats& vanilla,
                                   const BenchStats& base,
                                   const BenchStats& autoStats,
                                   const BenchStats& forced,
                                   Workload workload,
                                   const char* passName);

void assertCoveredCategoryContract(const BenchStats& vanilla,
                                   const BenchStats& base,
                                   const BenchStats& autoStats,
                                   const BenchStats& forced,
                                   Workload workload,
                                   const char* passName);

void assertInstrumentCategoryContract(const BenchStats& vanilla,
                                      const BenchStats& base,
                                      const BenchStats& autoStats,
                                      const BenchStats& forced,
                                      Workload workload,
                                      const char* passName);

void assertRuntimeCategoryContract(const BenchStats& vanilla,
                                   const BenchStats& base,
                                   const BenchStats& autoStats,
                                   const BenchStats& forced,
                                   Workload workload,
                                   const char* passName);

// INFRA: no benchmark helper by design — INFRA passes do not enter the
// benchmark suite. If a benchmark
// attempts to use the INFRA category via the macro below, link-time will
// fail because `assertInfraCategoryContract` is intentionally undefined.

// ---------------------------------------------------------------------------
// CATEGORY_BENCH_TEST_F — wraps TEST_F with a category property
// ---------------------------------------------------------------------------
//
// Expands to a derived fixture class + TEST_F on it. The category is:
//   - recorded as a gtest `category` property (visible in JUnit XML);
//   - embedded in the test-suite name (`<Suite>_<Category>_<Name>.Run`) so
//     that the CMake post-process script can append a lowercase CTest label
//     to each discovered case without parsing JSON.
//
// Usage:
//   CATEGORY_BENCH_TEST_F(OPT, PassBench, DataLayout_Friendly) {
//       auto r = runFourWay(...);
//       reportResult(...);
//       assertOptCategoryContract(...);
//   }
//
// Inside the body, `this` refers to a class derived from `Suite`, so all
// fixture members (`runFourWay`, `reportResult`, `projectsDir_`, ...) are
// accessible exactly as they are in a plain `TEST_F(Suite, Name)`.
#define CATEGORY_BENCH_TEST_F(Category, Suite, Name)                           \
    class Suite##_##Category##_##Name : public Suite {                         \
      public:                                                                  \
        void RunBody();                                                        \
        void SetUp() override {                                                \
            Suite::SetUp();                                                    \
            ::testing::Test::RecordProperty("category", #Category);            \
        }                                                                      \
    };                                                                         \
    TEST_F(Suite##_##Category##_##Name, Run) { RunBody(); }                    \
    void Suite##_##Category##_##Name::RunBody()

class E2eFixture : public ::testing::Test {
protected:
    void SetUp() override;

public:
    fs::path benchmarksDir_;
    fs::path fixturesDir_;
    fs::path projectsDir_; // backward compat alias for benchmarksDir_
    fs::path topoBuildExe_;
    fs::path llvmBinDir_;
    fs::path jvmBenchmarksDir_;

    // Build a project with topo-build.
    //
    // If `expectedOutput` is non-empty and the binary at
    // `binaryPath(projectName, expectedOutput)` already exists, return a
    // synthesised success (exit 0, empty stdout) without spawning topo-build.
    // This lets benchmark cases rely on the `topo-bench-artifacts` CTest
    // setup fixture for pre-built artefacts and run concurrently under
    // `ctest -j N` without racing each other on shared project state.
    // Leaving `expectedOutput` empty preserves the legacy inline-build
    // semantics (used by equivalence / functional suites).
    RunResult topoBuild(const std::string& projectName,
                        const std::string& expectedOutput = "");

    // Build using Topo-base.toml (all features OFF, swap, build, restore).
    // `expectedOutput` enables the pre-built artefact fast path; see
    // `topoBuild` for details.
    RunResult topoBaseBuild(const std::string& projectName,
                            const std::string& expectedOutput = "");

    // Build using Topo-forced.toml (features force-enabled, swap, build, restore).
    // `expectedOutput` enables the pre-built artefact fast path; see
    // `topoBuild` for details.
    RunResult topoForcedBuild(const std::string& projectName,
                              const std::string& expectedOutput = "");

    // Deprecated: alias for topoBaseBuild().
    RunResult topoBaselineBuild(const std::string& projectName);

    // Build a project with clang++ -O2 (vanilla baseline for B2).
    // Parses Topo.toml to extract sources, include, standard, output.
    //
    // Always produces `build/baseline` (or .exe on Windows);
    // short-circuits if that file already exists.
    RunResult vanillaBuild(const std::string& projectName);

    // Build a shared library with clang++ -O2 -shared (vanilla baseline).
    // Each TU compiled independently with -fvisibility=hidden, then linked.
    RunResult vanillaSharedBuild(const std::string& projectName, const std::vector<std::string>& extraDefines = {});

    // Run a compiled binary from a project's build/ directory.
    RunResult runBinary(const std::string& projectName, const std::string& outputName);

    // Run a JAR file via java -jar.
    RunResult runJar(const std::string& jarPath, const std::vector<std::string>& args = {});

    // Compile a single source file with clang++ and link to a shared library.
    RunResult compileDriver(const std::string& projectName,
                            const std::string& driverSource,
                            const std::string& outputName,
                            const std::vector<std::string>& includeDirs,
                            const std::string& linkLib = "");

    // Flexible output comparison.
    // Supports: {{NUM}} for numeric wildcards, ? prefix for optional lines,
    // ~ prefix for regex lines.
    void assertOutputMatches(const std::string& actual, const std::string& expected);

    // Get file size of a binary in the project's build/ directory.
    uintmax_t getBinarySize(const std::string& projectName, const std::string& outputName);

    // Count exported symbols using llvm-nm.
    int getExportedSymbolCount(const std::string& projectName, const std::string& outputName);

    // Get the full path to a binary (with platform suffix).
    fs::path binaryPath(const std::string& projectName, const std::string& outputName);

    // Get the full path to a shared library (with platform suffix).
    fs::path sharedLibPath(const std::string& projectName, const std::string& outputName);

    // Simple Topo.toml parser — extracts key build fields.
    struct TomlConfig {
        std::vector<std::string> sources;
        std::vector<std::string> include;
        std::string standard = "c++17";
        std::string output;
        std::string outputType; // "executable" or "shared"
    };
    TomlConfig parseTopoToml(const std::string& projectName);

    // --------------------------------------------------------------------
    // Pass-fired signal
    // --------------------------------------------------------------------
    //
    // topo-build emits `<output>.ll` when invoked with --dump-ir. Each Topo
    // pass that performs a non-trivial transformation writes a module-level
    // marker named `!topo.fired.<PassName> = !{!N}` where N is the count of
    // transformations.  The harness reads these markers to assert that a
    // pass actually fired on a workload, rather than being silently skipped
    // by absent pattern matches (the root cause of trivially-satisfied
    // equivalence assertions).

    // Parse `<projectName>/<outputName>.ll` and return the set of pass names
    // for which `!topo.fired.<PassName>` is present. Returns an empty set if
    // the .ll file does not exist.
    std::set<std::string> collectPassFiredMarkers(const std::string& projectName,
                                                  const std::string& outputName);

    // Same but returns the count associated with a single pass, or 0 if the
    // marker is absent / malformed / the .ll file is missing.
    unsigned getPassFiredCount(const std::string& projectName,
                               const std::string& outputName,
                               const std::string& passName);

    // Assert (EXPECT_*) that the given pass fired at least once on the
    // most recent build of `<projectName>/<outputName>`. Used in per-pass
    // equivalence tests to guard against trivial satisfaction (pass never
    // ran yet stdout matched because the workload has no pass-reachable
    // code path).
    void assertPassFired(const std::string& projectName,
                         const std::string& outputName,
                         const std::string& passName);

    // Inverse of assertPassFired — the pass *must not* have fired. Use this
    // for tests where a no-op is the documented expected behavior (e.g.
    // `*_BaseMatchesVanilla` tests which build with the pass off).
    void assertPassNotFired(const std::string& projectName,
                            const std::string& outputName,
                            const std::string& passName);
};

} // namespace topo::test::e2e

#endif // TOPO_TEST_E2E_HARNESS_H
