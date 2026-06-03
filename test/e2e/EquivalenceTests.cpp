#include "E2eHarness.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace topo::test::e2e {

namespace fs = std::filesystem;

// ============================================================================
// B2: IR Semantic Equivalence Tests
//
// For each test: build with vanilla clang++ -O2 AND with topo-build,
// run both binaries, and verify deterministic output lines are identical.
// Lines starting with "RESULT_US" are stripped (timing-dependent).
//
// Test naming convention:
//   <PassName>_<Mode>MatchesVanilla
//
//   <Mode> is one of:
//     - Default   — topo build with project's default Topo.toml
//     - Base      — topo build with Topo-base.toml swapped in (Pass off)
//     - Forced    — topo build with Topo-forced.toml swapped in (Pass force-on)
//
// Each test is an independent ctest case so a single broken Pass shows up
// against exactly its own name.
// ============================================================================

using Equivalence = E2eFixture;

// Strip timing-dependent lines from benchmark output.
//
// Benchmarks print three families of timing lines that are inherently
// non-deterministic across runs and must NOT participate in equivalence
// comparisons:
//
//   1. RESULT_US_* prefixed lines      — wall-clock microseconds per phase
//   2. "<label>: <N> (ns|us|ms|s) avg" — per-call timing reports
//   3. "Stabilization time: <N> ms"    — adaptive monitor warm-up time
//   4. Any line containing " ms" / " ns" / " us" surrounded by digits
//
// Functional output (results, counts, status) MUST remain unstripped so
// the equivalence assertion catches real semantic divergence.
static std::string stripTimingLines(const std::string& output) {
    static const std::regex unitRegex(R"(\b\d+(\.\d+)?\s+(ns|us|ms|µs)\b)",
                                      std::regex::ECMAScript);
    static const std::regex stabRegex(R"(^\s*Stabilization time:\s*\d+\s*ms\b)",
                                      std::regex::ECMAScript);

    std::istringstream iss(output);
    std::string line;
    std::string result;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // 1. RESULT_US_* prefix
        if (line.rfind("RESULT_US", 0) == 0) continue;

        // 2. Stabilization time
        if (std::regex_search(line, stabRegex)) continue;

        // 3. "<N> ns/us/ms" anywhere on the line — typically benchmark timing
        //    reports such as "  light_work: 3167 ns avg".
        if (std::regex_search(line, unitRegex)) continue;

        if (!result.empty()) result += "\n";
        result += line;
    }
    return result;
}

// Pass-event NDJSON noise filter shared by every Pass that
// injects a topo_pass_event_emit* call (AdaptiveDispatch / LifetimeArena
// / TopoParallel). A Pass-on build legitimately emits MORE stdout than a
// Pass-off build — the pass-event records are intentional side-effect
// output, not a semantic divergence — so they are stripped before the
// functional-output equivalence comparison. countPassEventLines is the
// paired emission-presence guard so the strip can never collapse two
// outputs into trivially-equal sides while masking a pass that never
// emitted. Defined here (next to stripTimingLines) rather than beside
// the LifetimeArenaPass tests so the TopoParallelPass firing guard on
// pipeline/forced (earlier in this file) can reuse them too.
static std::string stripPassEventNoise(const std::string& output) {
    static const std::regex peRegex(R"(^\s*\{\"kind\":\"pass_event\")",
                                    std::regex::ECMAScript);
    std::istringstream iss(output);
    std::string line;
    std::string result;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, peRegex)) continue; // strip pass events
        if (!result.empty()) result += "\n";
        result += line;
    }
    return result;
}

static size_t countPassEventLines(const std::string& output) {
    static const std::regex peRegex(R"(^\s*\{\"kind\":\"pass_event\")",
                                    std::regex::ECMAScript);
    std::istringstream iss(output);
    std::string line;
    size_t count = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, peRegex)) ++count;
    }
    return count;
}

// Dump all symbol names from a compiled binary using llvm-nm. Used by the
// functional symbol-presence tests migrated from PassBenchmarkTests.cpp
// (benchmark files measure performance only; artifact checks live here).
static std::string dumpAllSymbols(E2eFixture* self,
                                  const std::string& project,
                                  const std::string& outputName) {
    fs::path binPath = self->binaryPath(project, outputName);
    if (!fs::exists(binPath)) return "";
    std::string nm = (self->llvmBinDir_ / ("llvm-nm" + std::string(platform::ExeSuffix))).generic_string();
    auto r = platform::runProcessCapture(nm, {"-a", binPath.generic_string()});
    if (r.exitCode != 0) return "";
    return r.stdoutOutput;
}

static bool binaryHasSymbol(E2eFixture* self,
                            const std::string& project,
                            const std::string& outputName,
                            const std::string& needle) {
    std::string syms = dumpAllSymbols(self, project, outputName);
    return syms.find(needle) != std::string::npos;
}

// Helper: vanilla build + topo build (default mode) + run + compare.
// `vanillaName` is the binary name produced by vanillaBuild ("baseline").
// `topoName` matches the project's [build] output field.
static void assertDefaultEquivalence(E2eFixture* self,
                                     const std::string& project,
                                     const std::string& topoName) {
    auto vanilla = self->vanillaBuild(project);
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = self->topoBuild(project);
    ASSERT_EQ(topo.exitCode, 0) << "Topo build failed:\n" << topo.output;

    auto baselineRun = self->runBinary(project, "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = self->runBinary(project, topoName);
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Semantic divergence detected!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo output:\n"
        << topoRun.output;
}

// Helper: vanilla build + topo base build (Pass off) + run + compare.
static void assertBaseEquivalence(E2eFixture* self,
                                  const std::string& project,
                                  const std::string& topoBaseName) {
    auto vanilla = self->vanillaBuild(project);
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = self->topoBaseBuild(project);
    ASSERT_EQ(topo.exitCode, 0) << "Topo base build failed:\n" << topo.output;

    auto baselineRun = self->runBinary(project, "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = self->runBinary(project, topoBaseName);
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo base run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Pass-off (base) mode broke semantics!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo base output:\n"
        << topoRun.output;
}

// Helper: vanilla build + topo forced build (Pass force-on) + run + compare.
static void assertForcedEquivalence(E2eFixture* self,
                                    const std::string& project,
                                    const std::string& topoForcedName) {
    auto vanilla = self->vanillaBuild(project);
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = self->topoForcedBuild(project);
    ASSERT_EQ(topo.exitCode, 0) << "Topo forced build failed:\n" << topo.output;

    auto baselineRun = self->runBinary(project, "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = self->runBinary(project, topoForcedName);
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo forced run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Pass-forced mode broke semantics!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo forced output:\n"
        << topoRun.output;
}

// Helper: intra-Topo differential (Base vs Forced).
// Used when the benchmark source directly references topo runtime API
// (e.g. `topo::parallel::init`, `topo_trace_init`) and therefore cannot be
// vanilla-built. Both Base and Forced go through topo-build so runtime
// linkage is identical; only the Pass behavior differs between them.
static void assertBaseVsForcedEquivalence(E2eFixture* self,
                                          const std::string& project,
                                          const std::string& topoBaseName,
                                          const std::string& topoForcedName) {
    auto base = self->topoBaseBuild(project);
    ASSERT_EQ(base.exitCode, 0) << "Topo base build failed:\n" << base.output;
    auto baseRun = self->runBinary(project, topoBaseName);
    ASSERT_EQ(baseRun.exitCode, 0) << "Topo base run failed:\n" << baseRun.output;
    std::string baseStripped = stripTimingLines(baseRun.output);

    auto forced = self->topoForcedBuild(project);
    ASSERT_EQ(forced.exitCode, 0) << "Topo forced build failed:\n" << forced.output;
    auto forcedRun = self->runBinary(project, topoForcedName);
    ASSERT_EQ(forcedRun.exitCode, 0) << "Topo forced run failed:\n" << forcedRun.output;
    std::string forcedStripped = stripTimingLines(forcedRun.output);

    EXPECT_EQ(baseStripped, forcedStripped)
        << "Pass-on (forced) diverged from Pass-off (base)!\n"
        << "Base output:\n"
        << baseRun.output << "\n"
        << "Forced output:\n"
        << forcedRun.output;
}

// Helper: intra-Topo differential (Base vs Default/Auto).
// Same rationale as Base-vs-Forced: avoid vanilla baseline for benchmarks that
// reference runtime API directly.
static void assertBaseVsDefaultEquivalence(E2eFixture* self,
                                           const std::string& project,
                                           const std::string& topoBaseName,
                                           const std::string& topoDefaultName) {
    auto base = self->topoBaseBuild(project);
    ASSERT_EQ(base.exitCode, 0) << "Topo base build failed:\n" << base.output;
    auto baseRun = self->runBinary(project, topoBaseName);
    ASSERT_EQ(baseRun.exitCode, 0) << "Topo base run failed:\n" << baseRun.output;
    std::string baseStripped = stripTimingLines(baseRun.output);

    auto def = self->topoBuild(project);
    ASSERT_EQ(def.exitCode, 0) << "Topo default build failed:\n" << def.output;
    auto defRun = self->runBinary(project, topoDefaultName);
    ASSERT_EQ(defRun.exitCode, 0) << "Topo default run failed:\n" << defRun.output;
    std::string defStripped = stripTimingLines(defRun.output);

    EXPECT_EQ(baseStripped, defStripped)
        << "Pass-on (default) diverged from Pass-off (base)!\n"
        << "Base output:\n"
        << baseRun.output << "\n"
        << "Default output:\n"
        << defRun.output;
}

// --- E1: VisibilityPreservesSemantics ---

TEST_F(Equivalence, VisibilityPreservesSemantics) {
    auto vanilla = vanillaBuild("visibility");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoBuild("visibility");
    ASSERT_EQ(topo.exitCode, 0) << "Topo build failed:\n" << topo.output;

    auto baselineRun = runBinary("visibility", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("visibility", "hello_visibility");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Semantic divergence detected!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo output:\n"
        << topoRun.output;
}

// --- E2: StageReorderPreservesSemantics ---

TEST_F(Equivalence, StageReorderPreservesSemantics) {
    auto vanilla = vanillaBuild("stages");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoBuild("stages");
    ASSERT_EQ(topo.exitCode, 0) << "Topo build failed:\n" << topo.output;

    auto baselineRun = runBinary("stages", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("stages", "stages");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Semantic divergence detected!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo output:\n"
        << topoRun.output;
}

// --- E3: PipelineCodeGenPreservesSemantics ---

TEST_F(Equivalence, PipelineCodeGenPreservesSemantics) {
    auto vanilla = vanillaBuild("pipeline");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoBuild("pipeline");
    ASSERT_EQ(topo.exitCode, 0) << "Topo build failed:\n" << topo.output;

    auto baselineRun = runBinary("pipeline", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("pipeline", "pipeline");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Semantic divergence detected!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo output:\n"
        << topoRun.output;
}

// --- E4: MultiReturnPreservesSemantics ---

TEST_F(Equivalence, MultiReturnPreservesSemantics) {
    auto vanilla = vanillaBuild("multireturn");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoBuild("multireturn");
    ASSERT_EQ(topo.exitCode, 0) << "Topo build failed:\n" << topo.output;

    auto baselineRun = runBinary("multireturn", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("multireturn", "multi_return");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Semantic divergence detected!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo output:\n"
        << topoRun.output;
}

// --- E5: ClassTemplatePreservesSemantics ---

TEST_F(Equivalence, ClassTemplatePreservesSemantics) {
    auto vanilla = vanillaBuild("class_template");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoBuild("class_template");
    ASSERT_EQ(topo.exitCode, 0) << "Topo build failed:\n" << topo.output;

    auto baselineRun = runBinary("class_template", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("class_template", "class_template");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo run failed:\n" << topoRun.output;

    EXPECT_EQ(stripTimingLines(baselineRun.output), stripTimingLines(topoRun.output))
        << "Semantic divergence detected!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo output:\n"
        << topoRun.output;
}

// ============================================================================
// Base/Forced variant tests for the original 5 covered passes
// ============================================================================
//
// The original 5 tests above exercise the default Topo.toml. This block adds
// `Topo-base.toml` (`[builder] mode = "dev"`) and `Topo-forced.toml`
// (`[builder] mode = "aggressive"`) stubs for benchmarks that previously had
// only Topo.toml, so each Pass gets the full {Default, Base, Forced} triple.
//
// The builder-mode axis is meaningful even though these passes are not
// individually toml-toggleable: TopoInlinePass / TopoReorderPass / SymbolObfuscator
// and friends behave differently between dev and aggressive modes.
// ============================================================================

// --- TopoInlinePass → visibility ---

TEST_F(Equivalence, TopoInlinePass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "visibility", "hello_visibility_base");
}
TEST_F(Equivalence, TopoInlinePass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "visibility", "hello_visibility_forced");
}

// --- TopoReorderPass → stages ---
//
// TopoReorderPass is architecturally always-on — PassPipeline calls it
// unconditionally whenever symbols+mapping are present. It annotates
// stage-call sites with `!topo.stage` metadata; it does not physically
// reorder IR.
//
// PassPipeline runs TopoReorderPass on the freshly-compiled IR, BEFORE the
// standard LLVM optimization pipeline (the inliner). The call-sites are
// therefore always present when the pass runs, so the fired-marker is
// deterministic across builder modes and host toolchains — it no longer
// depends on whether the dev/aggressive inliner happened to leave a stage
// callee un-inlined. (Earlier the pass ran post-inline, and the marker count
// was inliner-threshold-dependent: 1 on macOS, 0 on the linux CI runner.)

TEST_F(Equivalence, TopoReorderPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "stages", "stages_base");
    // Marker guard is carried by the Forced variant below (one guard per
    // pass is enough); base only checks vanilla equivalence here.
}
TEST_F(Equivalence, TopoReorderPass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "stages", "stages_forced");
    assertPassFired("stages", "stages_forced", "TopoReorderPass");
}

// --- PipelineCodeGenPass → pipeline ---
//
// `pipeline/Topo-forced.toml` also enables `[parallel] mode = "force"`,
// which makes `TopoParallelPass` transform the generated pipeline body
// into `topo_task_spawn`/`topo_task_await` calls. This is the one
// workload in the repo that exercises BOTH `PipelineCodeGenPass` and
// `TopoParallelPass` on the forced variant, so the
// `TopoParallelPass` firing guard (previously attempted on `parallel/`
// but removed because that workload never gives the pass candidates)
// lives here.
//
// TopoParallelPass injects a topo_pass_event_emit()
// call at the task spawn and join moments, so the forced variant
// legitimately emits MORE stdout than the vanilla baseline — the
// pass-event NDJSON records are intentional side-effect output, not a
// semantic divergence. This test therefore can no longer route through
// the shared assertForcedEquivalence helper (it only strips timing); it
// is inlined here with stripPassEventNoise applied to the forced side
// and a countPassEventLines emission-presence guard, mirroring exactly
// how LifetimeArenaPass_BaseMatchesForced is structured. The shared
// helper is intentionally left untouched so the many other forced
// equivalence tests (where no pass-event is expected) still fail loudly
// if a pass-event leaks in.

TEST_F(Equivalence, PipelineCodeGenPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "pipeline", "pipeline_base");
}
TEST_F(Equivalence, PipelineCodeGenPass_ForcedMatchesVanilla) {
    auto vanilla = vanillaBuild("pipeline");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoForcedBuild("pipeline");
    ASSERT_EQ(topo.exitCode, 0) << "Topo forced build failed:\n" << topo.output;

    auto baselineRun = runBinary("pipeline", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("pipeline", "pipeline_forced");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo forced run failed:\n" << topoRun.output;

    // Functional output must match once the injected pass-event NDJSON
    // lines are stripped from the forced side (vanilla never links the
    // wire so its output carries none; stripping the baseline too is a
    // harmless no-op that keeps the comparison symmetric).
    EXPECT_EQ(stripPassEventNoise(stripTimingLines(baselineRun.output)),
              stripPassEventNoise(stripTimingLines(topoRun.output)))
        << "Pass-forced mode broke semantics (excluding pass-event records)!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo forced output:\n"
        << topoRun.output;

    // Emission-presence guard: the forced variant must actually emit
    // TopoParallelPass spawn + join pass-events (>=1 spawn + >=1 join),
    // and the vanilla baseline must emit none — otherwise the strip
    // above could mask a pass that never engaged. Mirrors the incr-2
    // LifetimeArenaPass open+close >=2 guard.
    EXPECT_GE(countPassEventLines(topoRun.output), 2u)
        << "Forced pipeline must emit >=1 TopoParallelPass spawn + >=1 join "
           "pass-event";
    EXPECT_EQ(countPassEventLines(baselineRun.output), 0u)
        << "Vanilla baseline must not emit any pass-event";

    // Guard: forced variant must actually exercise TopoParallelPass.
    // Without this, a silent heuristic regression that makes the pass
    // skip its transform would go undetected in equivalence alone.
    assertPassFired("pipeline", "pipeline_forced", "TopoParallelPass");
}

// --- ReturnSpecializationPass → multireturn ---

TEST_F(Equivalence, ReturnSpecializationPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "multireturn", "multi_return_base");
}
TEST_F(Equivalence, ReturnSpecializationPass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "multireturn", "multi_return_forced");
}

// --- ClassTemplate → class_template ---
//
// NOTE (equivalence framework cannot verify a pass fired by output alone):
// class_template.topo declares only types — no `stage<N>`, `access()`,
// `parallel`, or `pipeline` constructs. No Topo optimization Pass has a
// target here, so every pass is *expected* to be a no-op. The equivalence
// assertion below remains valuable as a "building types doesn't break
// vanilla semantics" guard, but it cannot verify any Pass preserved
// behavior because no Pass fired in the first place.
//
// We explicitly record the no-op expectation: the representative
// optimization passes must NOT leave fired markers. A positive marker
// would indicate pipeline infrastructure somehow engaged on a type-only
// translation unit, signalling a pass gating bug.

TEST_F(Equivalence, ClassTemplate_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "class_template", "class_template_base");
    // Pass no-op expected on a type-only workload: assert not fired for the
    // passes flagged by the issue as vulnerable to trivial satisfaction.
    assertPassNotFired("class_template", "class_template_base", "DataLayoutPass");
    assertPassNotFired("class_template", "class_template_base", "TopoParallelPass");
    assertPassNotFired("class_template", "class_template_base", "LifetimeArenaPass");
    assertPassNotFired("class_template", "class_template_base", "ObservabilityPass");
    assertPassNotFired("class_template", "class_template_base", "PipelineCodeGenPass");
}
TEST_F(Equivalence, ClassTemplate_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "class_template", "class_template_forced");
    // Same no-op expectation under forced — class_template has no
    // pipelines/stages/access patterns for the passes to target even when
    // forced on. If any of these fire it means a pass is running unguarded.
    assertPassNotFired("class_template", "class_template_forced", "DataLayoutPass");
    assertPassNotFired("class_template", "class_template_forced", "TopoParallelPass");
    assertPassNotFired("class_template", "class_template_forced", "LifetimeArenaPass");
    assertPassNotFired("class_template", "class_template_forced", "ObservabilityPass");
    assertPassNotFired("class_template", "class_template_forced", "PipelineCodeGenPass");
}

// ============================================================================
// Group A: Vanilla-baseline equivalence
// ============================================================================
//
// Benchmarks in this group do NOT reference Topo runtime API in their source,
// so vanilla clang++ -O2 can compile and link them. The Pass adds optimization
// (Pass-injected runtime calls) on top, but the observable output must remain
// identical to the vanilla baseline.
//
// Pattern (3 tests per Pass):
//   <PassName>_DefaultMatchesVanilla   — auto mode (Topo.toml)
//   <PassName>_BaseMatchesVanilla      — Pass off  (Topo-base.toml)
//   <PassName>_ForcedMatchesVanilla    — Pass on   (Topo-forced.toml)
// ============================================================================

// --- DataLayoutPass → data_layout ---

TEST_F(Equivalence, DataLayoutPass_DefaultMatchesVanilla) {
    assertDefaultEquivalence(this, "data_layout", "data_layout_perf");
}
TEST_F(Equivalence, DataLayoutPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "data_layout", "data_layout_base");
    // Base config disables data-layout: pass must not fire.
    assertPassNotFired("data_layout", "data_layout_base", "DataLayoutPass");
}
TEST_F(Equivalence, DataLayoutPass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "data_layout", "data_layout_forced");
    // Forced config runs runForceSoA on every qualifying topo::array —
    // the friendly path inside the benchmark MUST trigger the transform.
    // A stdout-only equivalence check cannot confirm the transform ran, so
    // this fired-marker guard is necessary even when stdout matches.
    assertPassFired("data_layout", "data_layout_forced", "DataLayoutPass");
}

// --- IndirectionPass → indirection ---

TEST_F(Equivalence, IndirectionPass_DefaultMatchesVanilla) {
    assertDefaultEquivalence(this, "indirection", "indirection");
}
TEST_F(Equivalence, IndirectionPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "indirection", "indirection_base");
}
TEST_F(Equivalence, IndirectionPass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "indirection", "indirection_forced");
}

// --- LoopParallelizePass → loop_parallel ---

TEST_F(Equivalence, LoopParallelizePass_DefaultMatchesVanilla) {
    assertDefaultEquivalence(this, "loop_parallel", "loop_parallel");
}
TEST_F(Equivalence, LoopParallelizePass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "loop_parallel", "loop_parallel_base");
}
TEST_F(Equivalence, LoopParallelizePass_ForcedMatchesVanilla) {
    // Benchmark was updated to give `scale` and `negate` private buffers so
    // they are truly independent siblings, matching the .topo declaration.
    assertForcedEquivalence(this, "loop_parallel", "loop_parallel_forced");
}

// --- LoopParallelizePass reduction combine → loop_reduction ---
// reduce_sum (single '+') and reduce_combo (two reductions, '+' and '*',
// in one loop) are same-stage siblings. Forced partitioning splits each
// reduction loop and combines per-partition partials; integer reductions
// make the result bit-identical to the serial build.

TEST_F(Equivalence, LoopReductionCombine_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "loop_reduction", "loop_reduction_base");
}
TEST_F(Equivalence, LoopReductionCombine_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "loop_reduction", "loop_reduction_forced");
    // Guard against trivial satisfaction: the partition path must have
    // actually run, not been skipped with stdout matching by accident.
    assertPassFired("loop_reduction", "loop_reduction_forced", "LoopParallelizePass");
}

// --- PrefetchPass → hints ---
//
// hints is C-class: HW prefetchers on modern CPUs cover sequential-stride
// access, so SW prefetch intrinsics produce no measurable runtime speedup
// over vanilla O2 or the base variant. Functional assertions replace the
// (absent) speedup assertion: the pass must fire when streaming access is
// declared + forced/auto, and must NOT fire under base. (Two pre-existing
// PrefetchPass bugs were fixed 2026-04-18, and the workload was
// reclassified from W to C for insufficient signal strength.)

TEST_F(Equivalence, PrefetchPass_DefaultMatchesVanilla) {
    assertDefaultEquivalence(this, "hints", "hints");
    assertPassFired("hints", "hints", "PrefetchPass");
}
TEST_F(Equivalence, PrefetchPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "hints", "hints_base");
    assertPassNotFired("hints", "hints_base", "PrefetchPass");
}
TEST_F(Equivalence, PrefetchPass_ForcedMatchesVanilla) {
    // `hints/Topo-forced.toml` also enables
    // `[parallel] mode="force"` over the `sim::run` pipeline, so
    // TopoParallelPass now fires here and injects task spawn/join
    // `topo_pass_event_emit()` calls — the forced binary legitimately
    // emits MORE stdout than the vanilla baseline. Inlined with
    // stripPassEventNoise + a countPassEventLines emission-presence
    // guard (the shared assertForcedEquivalence helper only strips
    // timing and is intentionally left untouched), mirroring the
    // PipelineCodeGenPass_ForcedMatchesVanilla / incr-2 LifetimeArena
    // rewrites.
    auto vanilla = vanillaBuild("hints");
    ASSERT_EQ(vanilla.exitCode, 0) << "Vanilla build failed:\n" << vanilla.output;

    auto topo = topoForcedBuild("hints");
    ASSERT_EQ(topo.exitCode, 0) << "Topo forced build failed:\n" << topo.output;

    auto baselineRun = runBinary("hints", "baseline");
    ASSERT_EQ(baselineRun.exitCode, 0) << "Baseline run failed:\n" << baselineRun.output;

    auto topoRun = runBinary("hints", "hints_forced");
    ASSERT_EQ(topoRun.exitCode, 0) << "Topo forced run failed:\n" << topoRun.output;

    EXPECT_EQ(stripPassEventNoise(stripTimingLines(baselineRun.output)),
              stripPassEventNoise(stripTimingLines(topoRun.output)))
        << "Pass-forced mode broke semantics (excluding pass-event records)!\n"
        << "Baseline output:\n"
        << baselineRun.output << "\n"
        << "Topo forced output:\n"
        << topoRun.output;
    // Forced enables [parallel] mode="force" over a pipeline, so
    // TopoParallelPass must inject >=1 spawn + >=1 join; vanilla never
    // links the wire so it emits none.
    EXPECT_GE(countPassEventLines(topoRun.output), 2u)
        << "Forced hints must emit >=1 TopoParallelPass spawn + >=1 join "
           "pass-event";
    EXPECT_EQ(countPassEventLines(baselineRun.output), 0u)
        << "Vanilla baseline must not emit any pass-event";

    assertPassFired("hints", "hints_forced", "PrefetchPass");
}

// ============================================================================
// Group B: Intra-Topo equivalence (runtime-dependent benchmarks)
// ============================================================================
//
// Benchmarks in this group use Topo runtime API directly in their source
// (e.g. `topo::parallel::init`, `topo_trace_init`), so vanilla clang++ cannot
// compile them. Equivalence is verified internally between Pass-off (base) and
// Pass-on (forced/default) builds.
// ============================================================================

// --- TopoParallelPass → parallel ---
//
// Classification: COVERED. Parallel speedup depends on task grain × hardware
// cores × task structure; Topo cannot promise OPT-grade `forced/base ≤ 0.90`
// on arbitrary friendly workloads — the OPT label was downgraded to COVERED
// because Topo passes must not make workload-side cost judgments.
//
// The `parallel` benchmark calls `topo_task_spawn` / `topo_task_await`
// directly from C++ source — its .topo file declares no pipeline
// (see `topo-llvm/benchmarks/parallel/topo/main.topo`).  Therefore
// TopoParallelPass finds zero `pipeline_placeholder` candidates to
// transform on this workload, regardless of variant — `parallel/` is a
// runtime-API baseline, not a pass-transformation signal. The pass-fired
// firing guard for TopoParallelPass lives on `pipeline/forced` (where
// `[parallel] mode = "force"` causes the pass to transform the generated
// pipeline body), see `PipelineCodeGenPass_ForcedMatchesVanilla` above.
// Here only `assertPassNotFired` invariants are valid.
//
// TopoParallelPass injects topo_pass_event_emit() at
// spawn/join sites, but ONLY when it actually transforms a pipeline.
// Since the pass never fires on this runtime-API workload, NO pass-event
// is emitted on any variant here — the existing stripTimingLines-only
// comparisons remain correct. We additionally assert a zero pass-event
// count so a regression that wrongly makes the pass fire on the
// runtime-API baseline (and thus inject spawn/join events) is caught,
// keeping the strip-vs-emission anti-masking contract symmetric with the
// pipeline/forced guard above.

TEST_F(Equivalence, TopoParallelPass_BaseMatchesDefault) {
    assertBaseVsDefaultEquivalence(this, "parallel", "parallel_base", "parallel_runtime");
    // Base has parallel=off: pass must not fire.
    assertPassNotFired("parallel", "parallel_base", "TopoParallelPass");
}
TEST_F(Equivalence, TopoParallelPass_BaseMatchesForced) {
    assertBaseVsForcedEquivalence(this, "parallel", "parallel_base", "parallel_forced");
    // Only the pass-NOT-fired half of the contract is valid here — the
    // workload never gives the pass candidates to transform, so
    // assertPassFired on `parallel_forced` would always fail; the firing
    // guard is carried by `PipelineCodeGenPass_ForcedMatchesVanilla` on
    // the `pipeline/forced` workload above.
    assertPassNotFired("parallel", "parallel_base", "TopoParallelPass");
    // The pass does not fire here, so neither variant emits a pass-event.
    EXPECT_EQ(countPassEventLines(runBinary("parallel", "parallel_base").output), 0u)
        << "Base parallel must not emit any TopoParallelPass pass-event";
    EXPECT_EQ(countPassEventLines(runBinary("parallel", "parallel_forced").output), 0u)
        << "Forced parallel (runtime-API workload, no pipeline candidates) "
           "must not emit any TopoParallelPass pass-event";
}
TEST_F(Equivalence, TopoParallelPass_DefaultMatchesForced) {
    auto def = topoBuild("parallel");
    ASSERT_EQ(def.exitCode, 0) << def.output;
    auto defRun = runBinary("parallel", "parallel_runtime");
    ASSERT_EQ(defRun.exitCode, 0);

    auto forced = topoForcedBuild("parallel");
    ASSERT_EQ(forced.exitCode, 0) << forced.output;
    auto forcedRun = runBinary("parallel", "parallel_forced");
    ASSERT_EQ(forcedRun.exitCode, 0);

    EXPECT_EQ(stripTimingLines(defRun.output), stripTimingLines(forcedRun.output))
        << "Default and forced parallel modes produced different outputs";
    // No pipeline candidates on this workload -> pass never fires ->
    // zero pass-events on either variant (anti-masking invariant).
    EXPECT_EQ(countPassEventLines(defRun.output), 0u)
        << "Default parallel must not emit any TopoParallelPass pass-event";
    EXPECT_EQ(countPassEventLines(forcedRun.output), 0u)
        << "Forced parallel must not emit any TopoParallelPass pass-event";
    // assertPassFired deferred to a future `parallel_pipeline/`
    // benchmark — current `parallel/` workload provides no candidates.
}

// --- AdaptiveDispatchPass → adaptive ---
//
// The `adaptive` benchmark's forced config
// (`adaptive/Topo-forced.toml`) ALSO enables `[parallel] mode="force"`
// over a pipeline, so TopoParallelPass now fires on `adaptive_forced`
// and injects task spawn/join `topo_pass_event_emit()` calls — the
// forced variant legitimately emits MORE stdout than the parallel-off
// `adaptive_base`. Exactly like the LifetimeArenaPass rewrite,
// the Base-vs-Forced and Default-vs-Forced adaptive comparisons are
// inlined here with stripPassEventNoise + a countPassEventLines
// emission-presence guard, instead of routing through the shared
// stripTimingLines-only helpers (which are intentionally left untouched
// so other tests still fail loudly on an unexpected pass-event). The
// `adaptive_base` (parallel off) side must emit zero pass-events.

TEST_F(Equivalence, AdaptiveDispatchPass_BaseMatchesDefault) {
    assertBaseVsDefaultEquivalence(this, "adaptive", "adaptive_base", "adaptive_parallel");
}
TEST_F(Equivalence, AdaptiveDispatchPass_BaseMatchesForced) {
    auto base = topoBaseBuild("adaptive");
    ASSERT_EQ(base.exitCode, 0) << "Topo base build failed:\n" << base.output;
    auto baseRun = runBinary("adaptive", "adaptive_base");
    ASSERT_EQ(baseRun.exitCode, 0) << "Topo base run failed:\n" << baseRun.output;

    auto forced = topoForcedBuild("adaptive");
    ASSERT_EQ(forced.exitCode, 0) << "Topo forced build failed:\n" << forced.output;
    auto forcedRun = runBinary("adaptive", "adaptive_forced");
    ASSERT_EQ(forcedRun.exitCode, 0) << "Topo forced run failed:\n" << forcedRun.output;

    EXPECT_EQ(stripPassEventNoise(stripTimingLines(baseRun.output)),
              stripPassEventNoise(stripTimingLines(forcedRun.output)))
        << "Pass-on (forced) diverged from Pass-off (base) functionally "
           "(excluding pass-event records)!\n"
        << "Base output:\n"
        << baseRun.output << "\n"
        << "Forced output:\n"
        << forcedRun.output;
    // Forced enables [parallel] mode="force" over a pipeline, so
    // TopoParallelPass must inject >=1 spawn + >=1 join; base has
    // parallel off so it must emit none — otherwise the strip could
    // mask a pass that never engaged.
    EXPECT_GE(countPassEventLines(forcedRun.output), 2u)
        << "Forced adaptive must emit >=1 TopoParallelPass spawn + >=1 join "
           "pass-event";
    EXPECT_EQ(countPassEventLines(baseRun.output), 0u)
        << "Base adaptive (parallel off) must not emit any pass-event";
}
TEST_F(Equivalence, AdaptiveDispatchPass_DefaultMatchesForced) {
    auto def = topoBuild("adaptive");
    ASSERT_EQ(def.exitCode, 0) << def.output;
    auto defRun = runBinary("adaptive", "adaptive_parallel");
    ASSERT_EQ(defRun.exitCode, 0);

    auto forced = topoForcedBuild("adaptive");
    ASSERT_EQ(forced.exitCode, 0) << forced.output;
    auto forcedRun = runBinary("adaptive", "adaptive_forced");
    ASSERT_EQ(forcedRun.exitCode, 0);

    // Default is parallel=auto (TopoParallelPass may or may not engage,
    // and at a non-deterministic count) while forced is parallel=force;
    // strip pass-event noise from both sides so only functional output
    // is compared. The forced side's emission is the deterministic one
    // we guard (mirrors LifetimeArenaPass_DefaultMatchesForced).
    EXPECT_EQ(stripPassEventNoise(stripTimingLines(defRun.output)),
              stripPassEventNoise(stripTimingLines(forcedRun.output)))
        << "Default and forced adaptive modes produced different "
           "functional outputs (excluding pass-event records)";
    EXPECT_GE(countPassEventLines(forcedRun.output), 2u)
        << "Forced adaptive must emit >=1 TopoParallelPass spawn + >=1 join "
           "pass-event";
}

// --- ObservabilityPass → observability ---
//
// ObservabilityPass injects tracing instrumentation that emits JSON trace
// events as side-effect output. The whole point of the Pass is to produce
// MORE output, so a naive intra-Topo Base-vs-Forced comparison would always
// diverge: Base has no traces, Forced is full of them. The equivalence
// dimension we care about is "functional output (program-printed lines, not
// trace events) is unchanged when instrumentation is added". To express that
// cleanly we strip JSON trace event lines before comparing.

static std::string stripObservabilityNoise(const std::string& output) {
    static const std::regex traceRegex(R"(^\s*\{\"name\":)", std::regex::ECMAScript);
    std::istringstream iss(output);
    std::string line;
    std::string result;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, traceRegex)) continue; // strip trace events
        if (!result.empty()) result += "\n";
        result += line;
    }
    return result;
}

// Count JSON trace event lines (the ones stripObservabilityNoise removes).
// Used as an emission-absence guard: if ObservabilityPass is expected to
// fire (forced mode), the pre-strip output must contain at least one trace
// line. Otherwise stripObservabilityNoise could collapse two empty outputs
// into trivially-equal sides, masking a pass that never ran.
static size_t countObservabilityTraceLines(const std::string& output) {
    static const std::regex traceRegex(R"(^\s*\{\"name\":)", std::regex::ECMAScript);
    std::istringstream iss(output);
    std::string line;
    size_t count = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, traceRegex)) ++count;
    }
    return count;
}

TEST_F(Equivalence, ObservabilityPass_BaseMatchesDefault) {
    auto base = topoBaseBuild("observability");
    ASSERT_EQ(base.exitCode, 0) << base.output;
    auto baseRun = runBinary("observability", "observability_base");
    ASSERT_EQ(baseRun.exitCode, 0);

    auto def = topoBuild("observability");
    ASSERT_EQ(def.exitCode, 0) << def.output;
    auto defRun = runBinary("observability", "observability");
    ASSERT_EQ(defRun.exitCode, 0);

    EXPECT_EQ(stripObservabilityNoise(stripTimingLines(baseRun.output)),
              stripObservabilityNoise(stripTimingLines(defRun.output)))
        << "ObservabilityPass changed program functional output between base "
           "and default modes (excluding trace events)";

    // Marker guard: base (pass off) must not fire; default (auto) must fire.
    // The trace-line count already catches emission-absence at runtime, but
    // the marker confirms the pass transformed the IR at build time (tracing
    // backend init failures could mask runtime trace emission while IR was
    // in fact instrumented).
    assertPassNotFired("observability", "observability_base", "ObservabilityPass");
    assertPassFired("observability", "observability", "ObservabilityPass");
}

TEST_F(Equivalence, ObservabilityPass_BaseMatchesForced) {
    auto base = topoBaseBuild("observability");
    ASSERT_EQ(base.exitCode, 0) << base.output;
    auto baseRun = runBinary("observability", "observability_base");
    ASSERT_EQ(baseRun.exitCode, 0);

    auto forced = topoForcedBuild("observability");
    ASSERT_EQ(forced.exitCode, 0) << forced.output;
    auto forcedRun = runBinary("observability", "observability_forced");
    ASSERT_EQ(forcedRun.exitCode, 0);

    // Emission-absence guard: the whole point of ObservabilityPass in forced
    // mode is to inject trace emission. If the forced binary emits zero trace
    // lines (pass silently no-op, tracing backend failed to init, workload
    // has no instrumentable targets), the subsequent stripObservabilityNoise
    // would collapse both sides to the same stripped output and the
    // equivalence assertion would be trivially satisfied. Guard against that
    // by checking the pre-strip forced output actually contains trace lines.
    ASSERT_GT(countObservabilityTraceLines(forcedRun.output), 0u)
        << "ObservabilityPass forced mode emitted zero trace lines — pass "
           "did not fire. Stripping noise from both sides would otherwise "
           "hide this emission absence and trivially satisfy equivalence.";
    EXPECT_EQ(countObservabilityTraceLines(baseRun.output), 0u)
        << "Base build (pass off) emitted trace lines; instrumentation "
           "leaking into base breaks the equivalence dimension";

    EXPECT_EQ(stripObservabilityNoise(stripTimingLines(baseRun.output)),
              stripObservabilityNoise(stripTimingLines(forcedRun.output)))
        << "ObservabilityPass changed program functional output between base "
           "and forced modes (excluding trace events)";

    // Marker guard complements the trace-line count above. If tracing backend
    // init failed at runtime, trace lines would be zero while the pass
    // actually fired at build time; the marker distinguishes these.
    assertPassNotFired("observability", "observability_base", "ObservabilityPass");
    assertPassFired("observability", "observability_forced", "ObservabilityPass");
}

TEST_F(Equivalence, ObservabilityPass_DefaultMatchesForced) {
    auto def = topoBuild("observability");
    ASSERT_EQ(def.exitCode, 0) << def.output;
    auto defRun = runBinary("observability", "observability");
    ASSERT_EQ(defRun.exitCode, 0);

    auto forced = topoForcedBuild("observability");
    ASSERT_EQ(forced.exitCode, 0) << forced.output;
    auto forcedRun = runBinary("observability", "observability_forced");
    ASSERT_EQ(forcedRun.exitCode, 0);

    // Emission-absence guard — see ObservabilityPass_BaseMatchesForced.
    ASSERT_GT(countObservabilityTraceLines(forcedRun.output), 0u)
        << "ObservabilityPass forced mode emitted zero trace lines — pass "
           "did not fire";

    EXPECT_EQ(stripObservabilityNoise(stripTimingLines(defRun.output)),
              stripObservabilityNoise(stripTimingLines(forcedRun.output)))
        << "ObservabilityPass changed program functional output between "
           "default and forced modes (excluding trace events)";

    assertPassFired("observability", "observability", "ObservabilityPass");
    assertPassFired("observability", "observability_forced", "ObservabilityPass");
}

// --- LifetimeArenaPass → lifetime ---
//
// LifetimeArenaPass injects a
// topo_pass_event_emit_sized() call at every arena open/close site, so
// (exactly like ObservabilityPass) a Pass-on build legitimately emits
// MORE stdout than a Pass-off build — the pass-event NDJSON records are
// intentional side-effect output, not a semantic divergence. The
// equivalence dimension we care about is "program-printed functional
// output is unchanged when arena allocation is substituted". So we
// strip the `{"kind":"pass_event",...}` lines before comparing, mirror-
// ing stripObservabilityNoise, and use a count guard so the strip can
// never collapse two outputs into trivially-equal sides while masking a
// pass that never emitted (see the ObservabilityPass precedent above).
// The stripPassEventNoise / countPassEventLines helpers
// these tests use are defined once near stripTimingLines at the top of
// this file, so the TopoParallelPass pipeline/forced guard above can
// reuse the same helpers.

TEST_F(Equivalence, LifetimeArenaPass_BaseMatchesDefault) {
    // Base (pass off): no arena substitution, no pass-events.
    auto base = topoBaseBuild("lifetime");
    ASSERT_EQ(base.exitCode, 0) << base.output;
    auto baseRun = runBinary("lifetime", "lifetime_base");
    ASSERT_EQ(baseRun.exitCode, 0) << baseRun.output;

    // Default mode is auto: only fires if the benchmark accepted the
    // arena variant. Keep the auto path flexible — strip pass-event
    // noise so functional output is what's compared either way.
    auto def = topoBuild("lifetime");
    ASSERT_EQ(def.exitCode, 0) << def.output;
    auto defRun = runBinary("lifetime", "lifetime_arena");
    ASSERT_EQ(defRun.exitCode, 0) << defRun.output;

    EXPECT_EQ(stripPassEventNoise(stripTimingLines(baseRun.output)),
              stripPassEventNoise(stripTimingLines(defRun.output)))
        << "LifetimeArenaPass changed program functional output between "
           "base and default modes (excluding pass-event records)";
    EXPECT_EQ(countPassEventLines(baseRun.output), 0u)
        << "Base (pass off) must not emit any arena pass-event";
    assertPassNotFired("lifetime", "lifetime_base", "LifetimeArenaPass");
}
TEST_F(Equivalence, LifetimeArenaPass_BaseMatchesForced) {
    auto base = topoBaseBuild("lifetime");
    ASSERT_EQ(base.exitCode, 0) << base.output;
    auto baseRun = runBinary("lifetime", "lifetime_base");
    ASSERT_EQ(baseRun.exitCode, 0) << baseRun.output;

    auto forced = topoForcedBuild("lifetime");
    ASSERT_EQ(forced.exitCode, 0) << forced.output;
    auto forcedRun = runBinary("lifetime", "lifetime_forced");
    ASSERT_EQ(forcedRun.exitCode, 0) << forcedRun.output;

    EXPECT_EQ(stripPassEventNoise(stripTimingLines(baseRun.output)),
              stripPassEventNoise(stripTimingLines(forcedRun.output)))
        << "LifetimeArenaPass changed program functional output between "
           "base and forced modes (excluding pass-event records)";
    // Emission-presence guard: forced must actually emit arena
    // pass-events (open + close), and base must emit none — otherwise
    // the strip could mask a pass that never engaged.
    EXPECT_GE(countPassEventLines(forcedRun.output), 2u)
        << "Forced mode must emit >=1 arena open + >=1 close pass-event";
    EXPECT_EQ(countPassEventLines(baseRun.output), 0u)
        << "Base (pass off) must not emit any arena pass-event";
    // Forced must fire — without this guard equivalence is trivially
    // true if the arena path never engaged at build time.
    assertPassFired("lifetime", "lifetime_forced", "LifetimeArenaPass");
    assertPassNotFired("lifetime", "lifetime_base", "LifetimeArenaPass");
}
TEST_F(Equivalence, LifetimeArenaPass_DefaultMatchesForced) {
    auto def = topoBuild("lifetime");
    ASSERT_EQ(def.exitCode, 0) << def.output;
    auto defRun = runBinary("lifetime", "lifetime_arena");
    ASSERT_EQ(defRun.exitCode, 0);

    auto forced = topoForcedBuild("lifetime");
    ASSERT_EQ(forced.exitCode, 0) << forced.output;
    auto forcedRun = runBinary("lifetime", "lifetime_forced");
    ASSERT_EQ(forcedRun.exitCode, 0);

    EXPECT_EQ(stripPassEventNoise(stripTimingLines(defRun.output)),
              stripPassEventNoise(stripTimingLines(forcedRun.output)))
        << "Default and forced lifetime modes produced different "
           "functional outputs (excluding pass-event records)";
    EXPECT_GE(countPassEventLines(forcedRun.output), 2u)
        << "Forced mode must emit >=1 arena open + >=1 close pass-event";

    assertPassFired("lifetime", "lifetime_forced", "LifetimeArenaPass");
}

// ============================================================================
// Cross-cutting passes: Flatten / Layout / SymbolObfuscator
// ============================================================================
//
// TopoFlattenPass, TopoLayoutPass, and SymbolObfuscator are cross-cutting —
// they rewrite linkage, section assignment, and symbol names rather than
// inserting runtime calls. None of them is individually toml-toggleable, but
// their effects are observable:
//
//   - TopoFlattenPass demotes Private visibility (dev) or Private+Protected
//     (aggressive) to InternalLinkage, enabling LLVM GlobalDCE to remove
//     functions that become unused after Topo's analysis.
//   - TopoLayoutPass always runs when a SymbolMapping is available and
//     assigns each function to a stage-based section.
//   - SymbolObfuscator rewrites internal symbol names via hash (normal) or
//     hash + salt (salted) when `[builder] obfuscation` is set.
//
// Flatten and Layout are exercised by the visibility benchmark: its
// public/protected/private visibility tree and the `calc::core::impl` nested
// namespace give the passes something to rewrite in both dev and aggressive
// builder modes. Default mode is already covered by VisibilityPreservesSemantics
// (E1 above), so this block only adds the Base and Forced variants for each pass.
//
// SymbolObfuscator cannot be toggled just by builder.mode — it needs the
// `[builder] obfuscation` field — so this block introduces a minimal dedicated
// `obfuscation` benchmark whose three Topo.toml variants all set obfuscation
// to a non-off mode (Topo.toml: normal, Topo-base.toml: dev+normal,
// Topo-forced.toml: aggressive+salted). Every run of the obfuscation fixture
// exercises the obfuscator; the stdout comparison catches any rename that
// leaks into observable behavior (e.g. a dropped printf call).
// ============================================================================

// --- TopoFlattenPass → visibility (Default is VisibilityPreservesSemantics) ---

TEST_F(Equivalence, TopoFlattenPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "visibility", "hello_visibility_base");
}
TEST_F(Equivalence, TopoFlattenPass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "visibility", "hello_visibility_forced");
}

// --- TopoLayoutPass → visibility (Default is VisibilityPreservesSemantics) ---

TEST_F(Equivalence, TopoLayoutPass_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "visibility", "hello_visibility_base");
}
TEST_F(Equivalence, TopoLayoutPass_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "visibility", "hello_visibility_forced");
}

// --- SymbolObfuscator → obfuscation (dedicated minimal fixture) ---

TEST_F(Equivalence, SymbolObfuscator_DefaultMatchesVanilla) {
    assertDefaultEquivalence(this, "obfuscation", "obfuscation");
}
TEST_F(Equivalence, SymbolObfuscator_BaseMatchesVanilla) {
    assertBaseEquivalence(this, "obfuscation", "obfuscation_base");
}
TEST_F(Equivalence, SymbolObfuscator_ForcedMatchesVanilla) {
    assertForcedEquivalence(this, "obfuscation", "obfuscation_forced");
}

// --- AuxTypes → auxiliary_types (always-on SDK types; no Topo-base/forced) ---
//
// clang -O2 already folds topo::span / topo::slot / topo::array into the same
// codegen as raw pointer iteration, so PassBench only asserts non-regression.
// The stdout-equivalence correctness check (migrated from PassBenchmarkTests.cpp)
// lives here: equivalence is the right home for functional behaviour
// verification.
TEST_F(Equivalence, AuxTypes_DefaultMatchesVanilla) {
    assertDefaultEquivalence(this, "auxiliary_types", "auxiliary_types");
}

// ============================================================================
// Functional artifact checks migrated from PassBenchmarkTests.cpp
//
// These passes (IREmbed, AdaptiveDispatchPass, ObservabilityPass) are
// metadata- or instrumentation-only; they produce no runtime speedup by
// design. The original "functional" TEST_F entries lived in the benchmark
// file because they reused the binary-symbol helper, but they never called
// runFourWay / reportResult. Benchmark files MUST NOT contain artifact
// checks, so these tests live here.
// ============================================================================

// --- IREmbed (12_ir_embedding) ---
//
// JIT (IREmbed) is metadata-only: it embeds LLVM bitcode in a
// `topo_embedded_ir` symbol (backed by the `.topo_ir` / `__topo_ir` section).
// Functional assertion:
//   - Topo auto build (embed_ir=true)  → binary contains `topo_embedded_ir`
//   - Topo base build (embed_ir=false) → binary does NOT contain it
TEST_F(Equivalence, JIT_IREmbed_FunctionalSymbol) {
    auto baseBuild = topoBaseBuild("jit");
    ASSERT_EQ(baseBuild.exitCode, 0) << "Topo-base build failed:\n" << baseBuild.output;
    auto baseRun = runBinary("jit", "jit_base");
    EXPECT_EQ(baseRun.exitCode, 0) << "Topo-base run failed:\n" << baseRun.output;

    {
        std::error_code ec;
        fs::remove_all(projectsDir_ / "jit" / ".topo-cache", ec);
    }

    auto autoBuild = topoBuild("jit");
    ASSERT_EQ(autoBuild.exitCode, 0) << "Topo-auto build failed:\n" << autoBuild.output;
    auto autoRun = runBinary("jit", "ir_embedding");
    EXPECT_EQ(autoRun.exitCode, 0) << "Topo-auto run failed:\n" << autoRun.output;

    // Topo auto must have embedded IR (the symbol injected by IREmbed::embed).
    bool autoHasIR = binaryHasSymbol(this, "jit", "ir_embedding", "topo_embedded_ir");
    EXPECT_TRUE(autoHasIR) << "jit: topo auto binary is missing `topo_embedded_ir` — "
                           << "IREmbed did not fire.";

    // Topo base (embed_ir=false) must NOT have the symbol — confirms the
    // distinction between base and auto is real, not accidental.
    bool baseHasIR = binaryHasSymbol(this, "jit", "jit_base", "topo_embedded_ir");
    EXPECT_FALSE(baseHasIR) << "jit: topo base binary unexpectedly contains "
                            << "`topo_embedded_ir` — IREmbed fired despite embed_ir=false.";

    std::printf("[  INFO  ] jit/IREmbed: auto has symbol = %d, base has symbol = %d\n",
                autoHasIR, baseHasIR);
}

// --- AdaptiveDispatchPass (14_adaptive_parallel) ---
//
// AdaptiveDispatchPass is instrumentation-only: it inserts a JIT dispatch
// hook at each pipeline entry (atomic pointer load → jit_path or AOT path)
// and wraps the AOT path with `topo_cost_begin`/`topo_cost_end` calls, and
// registers every pipeline via `topo_adaptive_register` at ctor time.
//
// Functional assertion: Topo auto binary must link against the runtime
// symbols `topo_cost_begin` / `topo_adaptive_register`; Topo base must not.
TEST_F(Equivalence, Adaptive_FunctionalDispatch) {
    auto baseBuild = topoBaseBuild("adaptive");
    ASSERT_EQ(baseBuild.exitCode, 0) << "Topo-base build failed:\n" << baseBuild.output;
    auto baseRun = runBinary("adaptive", "adaptive_base");
    EXPECT_EQ(baseRun.exitCode, 0) << "Topo-base run failed:\n" << baseRun.output;

    {
        std::error_code ec;
        fs::remove_all(projectsDir_ / "adaptive" / ".topo-cache", ec);
    }

    auto autoBuild = topoBuild("adaptive");
    ASSERT_EQ(autoBuild.exitCode, 0) << "Topo-auto build failed:\n" << autoBuild.output;
    auto autoRun = runBinary("adaptive", "adaptive_parallel");
    EXPECT_EQ(autoRun.exitCode, 0) << "Topo-auto run failed:\n" << autoRun.output;

    // Topo auto must reference the AdaptiveDispatchPass runtime calls.
    bool autoHasCostBegin = binaryHasSymbol(this, "adaptive", "adaptive_parallel", "topo_cost_begin");
    bool autoHasRegister = binaryHasSymbol(this, "adaptive", "adaptive_parallel", "topo_adaptive_register");
    EXPECT_TRUE(autoHasCostBegin) << "adaptive: auto binary missing `topo_cost_begin` — "
                                  << "AdaptiveDispatchPass did not insert cost instrumentation.";
    EXPECT_TRUE(autoHasRegister) << "adaptive: auto binary missing `topo_adaptive_register` — "
                                 << "AdaptiveDispatchPass did not register pipelines.";

    // Topo base (adaptive=off) should not reference the dispatch runtime at
    // all — confirms the pass is actually gated by the feature flag.
    bool baseHasCostBegin = binaryHasSymbol(this, "adaptive", "adaptive_base", "topo_cost_begin");
    bool baseHasRegister = binaryHasSymbol(this, "adaptive", "adaptive_base", "topo_adaptive_register");
    EXPECT_FALSE(baseHasCostBegin) << "adaptive: base binary contains `topo_cost_begin` "
                                   << "despite adaptive=off.";
    EXPECT_FALSE(baseHasRegister) << "adaptive: base binary contains `topo_adaptive_register` "
                                  << "despite adaptive=off.";

    std::printf("[  INFO  ] adaptive/AdaptiveDispatch: auto has {cost_begin=%d, register=%d}, "
                "base has {cost_begin=%d, register=%d}\n",
                autoHasCostBegin, autoHasRegister, baseHasCostBegin, baseHasRegister);
}

// --- ObservabilityPass (28_observability) — functional artifact check ---
//
// ObservabilityPass is instrumentation-only: it injects
// `topo_trace_span_begin` / `topo_trace_span_end` calls at stage boundaries.
// Functional assertions:
//   - Topo auto build's IR carries the `!topo.fired.ObservabilityPass` marker;
//     base (observability=off) does not.
//   - Running the auto binary with tracing active emits >= 1 span event
//     (the correctness-check `execute(10)` under `topo_trace_init`).
//   - The timed RESULT_US_* portion runs with tracing shut down, so
//     the auto/base ratio is measuring raw call-site overhead. Relaxed
//     upper bound `auto/base < 1.25` — one-shot ratio has +/-5% noise band
//     on this harness.
//
// The "auto binary contains `topo_trace_span_begin` / base does not" shape
// that previously lived here is *not* a pass-fired signal: main.cpp directly
// calls `topo_trace_init` / `topo_trace_shutdown`, which are compiled in the
// same TU as `topo_trace_span_begin` / `topo_trace_span_end` inside the
// `topo-observe` static archive. The linker therefore pulls the full
// translation unit — including the unused `_span_begin` / `_span_end`
// symbols — into *every* build that uses the runtime's init API, regardless
// of whether ObservabilityPass instrumented anything.
// The honest pass-fired signal is the IR marker; symbol presence is a
// property of the link graph, not of pass firing.
//
// Note the equivalence counterpart `ObservabilityPass_BaseMatchesDefault` /
// `_ForcedMatchesVanilla` already verify stdout equivalence with trace lines
// stripped; this test is the complementary "pass fired / events emitted /
// overhead bounded" guard.
TEST_F(Equivalence, Observability_FunctionalEvents) {
    auto baseBuild = topoBaseBuild("observability");
    ASSERT_EQ(baseBuild.exitCode, 0) << "Topo-base build failed:\n" << baseBuild.output;
    auto baseRun = runBinary("observability", "observability_base");
    ASSERT_EQ(baseRun.exitCode, 0) << "Topo-base run failed:\n" << baseRun.output;

    static const std::regex resultRe(R"(RESULT_US_FRIENDLY=(\d+\.?\d*))");
    auto extractUs = [](const std::string& output) -> double {
        std::smatch m;
        if (std::regex_search(output, m, resultRe)) return std::stod(m[1].str());
        return -1.0;
    };
    double baseUs = extractUs(baseRun.output);

    {
        std::error_code ec;
        fs::remove_all(projectsDir_ / "observability" / ".topo-cache", ec);
    }

    auto autoBuild = topoBuild("observability");
    ASSERT_EQ(autoBuild.exitCode, 0) << "Topo-auto build failed:\n" << autoBuild.output;
    auto autoRun = runBinary("observability", "observability");
    ASSERT_EQ(autoRun.exitCode, 0) << "Topo-auto run failed:\n" << autoRun.output;
    double autoUs = extractUs(autoRun.output);

    // 1. Pass-fired marker — auto build must have recorded the IR marker;
    //    base (observability=off) must not. The symbol-presence shape that
    //    lived here previously was unreliable: the runtime TU that defines
    //    `topo_trace_init` also defines `topo_trace_span_begin`, so the
    //    static archive pulls both into every binary that uses the init
    //    API — regardless of whether ObservabilityPass ran. See the issue
    //    referenced in the test header for details.
    assertPassFired("observability", "observability", "ObservabilityPass");
    assertPassNotFired("observability", "observability_base", "ObservabilityPass");

    // 2. Event count — auto binary must emit >= 1 span event during the
    //    correctness-check phase (execute(10) under topo_trace_init).
    int events = 0;
    {
        std::istringstream iss(autoRun.output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("\"duration_ns\"") != std::string::npos) ++events;
        }
    }
    EXPECT_GT(events, 0) << "observability: auto binary emitted zero span events — "
                         << "tracing instrumentation is inert.";

    // 3. Overhead upper bound (relaxed — see comment above).
    if (baseUs > 0 && autoUs > 0 && baseUs >= 20000.0) {
        double ratio = autoUs / baseUs;
        EXPECT_LE(ratio, 1.25)
            << "observability: auto/base overhead " << ratio
            << " exceeds 1.25 upper bound (base=" << baseUs
            << "us, auto=" << autoUs << "us)";
        std::printf("[  INFO  ] observability/Observe: overhead auto/base = %.3f\n", ratio);
    }

    std::printf("[  INFO  ] observability/Observe: events=%d\n", events);
}

// --- ContainmentInterceptionPass ---
//
// ContainmentInterceptionPass is ENHANCE-class (declaration-class witness).
// `class_template/src/main.cpp` calls `std::printf` directly from several
// non-external functions (Step 1..6 assertion prints). With
// `[containment].mode = "force"` in class_template/Topo-forced.toml, the
// pass inserts `__topo_containment_violation(caller, callee)` before each
// of those calls.  Base / default variants have no `[containment]` section
// and therefore must not fire the pass at all.
//
// This coverage closes a gap where the pass was previously defined but
// never invoked from PassPipeline — the runtime library existed as dead
// code.
TEST_F(Equivalence, ContainmentInterceptionPass_ForcedInjectsViolationCalls) {
    assertPassFired("class_template", "class_template_forced", "ContainmentInterceptionPass");

    // Additionally assert the runtime call symbol appears in the forced IR,
    // confirming the pass injected real instrumentation rather than only
    // emitting a metadata marker.
    fs::path irPath = dumpedIRPath("class_template", "class_template_forced");
    ASSERT_TRUE(!irPath.empty() && fs::exists(irPath))
        << "class_template_forced.ll not found — was the forced variant built?";
    std::ifstream ifs(irPath);
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string ir = buf.str();
    auto found = ir.find("__topo_containment_violation");
    EXPECT_NE(found, std::string::npos)
        << "class_template_forced.ll is missing `__topo_containment_violation` — "
           "ContainmentInterceptionPass fired (marker present) but did not "
           "insert runtime calls.";
}

TEST_F(Equivalence, ContainmentInterceptionPass_BaseDoesNotFire) {
    // `[containment]` absent from Topo-base.toml → mode defaults to Off →
    // pass must stay dormant.  Guards against the pass becoming always-on
    // by accident.
    assertPassNotFired("class_template", "class_template_base", "ContainmentInterceptionPass");
    assertPassNotFired("class_template", "class_template", "ContainmentInterceptionPass");
}

} // namespace topo::test::e2e
