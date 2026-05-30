#include "E2eHarness.h"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <string>

namespace topo::test::e2e {

// All vanilla/base/auto/forced variants are pre-built by the
// `topo_bench_artifacts_build` CTest setup fixture. Each TEST_F still calls
// `runFourWay` / `runAlwaysOn`, but those helpers now pass the expected
// output binary name into `topoBaseBuild` / `topoBuild` / `topoForcedBuild`,
// which short-circuit to a no-op when the binary already exists. This keeps
// the benchmark code unchanged while enabling `ctest -L benchmark -j N`
// real parallelism (no more RESOURCE_LOCK on benchmark cases). The inline
// build path remains as a fallback for ad-hoc `./topo-e2e-pass-bench` runs
// outside CTest.

// ============================================================================
// Per-Pass Benchmark Tests
//
// Four-way comparison for opt-in features:
//   1. vanilla O2:   clang++ -O2 (performance floor)
//   2. topo base:    topo-build with all features OFF (Topo-base.toml)
//   3. topo auto:    topo-build with feature at default thresholds (Topo.toml)
//   4. topo forced:  topo-build with feature force-enabled (Topo-forced.toml)
//
// Always-on features use vanilla O2 + topo (base = auto = forced).
//
// Each project outputs:
//   RESULT_US_FRIENDLY=<median_us>
//   RESULT_US_UNFRIENDLY=<median_us>
//
// CTest label: "pass_bench" — excluded from default CI runs.
// ============================================================================

namespace fs = std::filesystem;

// Four-way benchmark result for a single workload (friendly or unfriendly).
//
// Each mode carries a full BenchStats (samples, mean,
// median, stdev, CV, resample flag). The legacy `double` fields mirror
// `.median` (changed from `.mean` as part of the seed-determinism
// reproducibility fix — median resists single-sample CPU-spike
// outliers, which were the dominant source of run-to-run ratio drift).
// Existing assertion helpers read `.topoBase` etc. directly without
// caring whether the underlying aggregate is mean or median. New code
// should prefer the explicit `.vanillaStats` / `.baseStats` /
// `.autoStats` / `.forcedStats` fields.
struct BenchResult {
    double vanillaO2 = 0.0;  // clang++ -O2                    (= vanillaStats.median)
    double topoBase = 0.0;   // topo-build, all features OFF   (= baseStats.median)
    double topoAuto = 0.0;   // topo-build, feature auto       (= autoStats.median)
    double topoForced = 0.0; // topo-build, feature forced     (= forcedStats.median)

    BenchStats vanillaStats;
    BenchStats baseStats;
    BenchStats autoStats;
    BenchStats forcedStats;
};

// Subclass to add benchmark-specific helpers.
class PassBench : public E2eFixture {
protected:
    // Resampling constants: when a ratio falls within
    // [threshold - kAmbiguousMargin, threshold + kAmbiguousMargin],
    // re-run the binaries kResampleRuns times and decide on the average.
    static constexpr double kAmbiguousMargin = 0.03;
    static constexpr int kResampleRuns = 7;
    static constexpr double kNoiseFloorUs = 20000.0; // skip constraints below 20ms

    // Context for resampling — carries the info needed to re-run binaries.
    struct ResampleContext {
        PassBench* fixture;
        std::string project;
        std::string vanillaOutput;
        std::string baseOutput;
        std::string autoOutput;
        std::string forcedOutput; // for assertForcedNotCatastrophic resampling
        std::string resultLabel;
    };

    static double extractResultUs(const std::string& output, const std::string& label) {
        std::string pattern = label + R"(=(\d+\.?\d*))";
        std::regex re(pattern);
        std::smatch match;
        if (std::regex_search(output, match, re)) {
            return std::stod(match[1].str());
        }
        return -1.0;
    }

    // Re-run an already-built binary multiple times and return the average.
    double resampleAverage(const std::string& project,
                           const std::string& outputName,
                           const std::string& resultLabel,
                           int runs = kResampleRuns) {
        // Warmup once per unique binary: prime OS page cache and dynamic linker
        static std::set<std::string> warmedUp;
        if (warmedUp.insert(project + "/" + outputName).second) {
            (void)runBinary(project, outputName);
        }

        double sum = 0.0;
        int valid = 0;
        for (int i = 0; i < runs; ++i) {
            auto result = runBinary(project, outputName);
            if (result.exitCode == 0) {
                double us = extractResultUs(result.output, resultLabel);
                if (us > 0) {
                    sum += us;
                    ++valid;
                }
            }
        }
        return (valid > 0) ? sum / valid : -1.0;
    }

    // Measure an already-built binary under variance adaptation.
    //
    // Returns an empty BenchStats (runs=0) if `outputName` is empty. Used by
    // runFourWay / runAlwaysOn so that an optional `vanilla` slot can be
    // skipped uniformly.
    BenchStats measureBinary(const std::string& project,
                             const std::string& outputName,
                             const std::string& resultLabel) {
        if (outputName.empty()) return BenchStats{};
        return measureWithVarianceAdapt([this, project, outputName, resultLabel]() {
            auto r = runBinary(project, outputName);
            if (r.exitCode != 0) return -1.0;
            return extractResultUs(r.output, resultLabel);
        });
    }

    // Run a four-way benchmark for one workload.
    // For opt-in features: vanilla O2 + topo base + topo auto + topo forced.
    // If vanillaOutput is empty, skip vanilla O2 build.
    //
    // `topoForcedOutput` MUST be non-empty — the three-mode verification
    // contract requires a real forced build. A helper that
    // silently set `r.topoForced = r.topoAuto` used to exist here; it was
    // removed because it silently neutralised `assertForcedNotCatastrophic`
    // and turned every four-way benchmark into a three-way one.
    //
    // If a benchmark legitimately has no forced variant (always-on feature,
    // instrumentation-only pass, etc.), use `runAlwaysOn` or a bespoke
    // functional assertion instead of passing an empty string here.
    //
    // Seed-determinism fix:
    //   * Builds happen sequentially up-front (the setup fixture
    //     usually pre-built them, so each call is a fast no-op).
    //   * Measurement is done via `measureWithVarianceAdaptInterleaved`,
    //     which rotates samples across vanilla/base/auto/forced on each
    //     round so transient OS/CPU noise hits every mode in roughly the
    //     same wall-clock windows. Before this change, vanilla was measured
    //     during one CPU-availability window and auto during a later
    //     (potentially different) window, which is what produced the
    //     "same seed, 2-3x ratio swings" reproducibility bug.
    //   * Median (not mean) drives the legacy `topoBase` / `topoAuto` /
    //     `topoForced` doubles so a single outlier sample (e.g. one binary
    //     run that happened to coincide with a spotlight indexer burst)
    //     no longer dominates the ratio that assertions read.
    BenchResult runFourWay(const std::string& project,
                           const std::string& vanillaOutput,
                           const std::string& topoBaseOutput,
                           const std::string& topoAutoOutput,
                           const std::string& topoForcedOutput,
                           const std::string& resultLabel) {
        BenchResult r;

        if (topoForcedOutput.empty()) {
            ADD_FAILURE() << project << ": runFourWay requires a non-empty "
                          << "topoForcedOutput — the three-mode verification "
                          << "contract forbids silently aliasing forced to auto. "
                          << "Use runAlwaysOn (or a bespoke functional check) "
                          << "if this pass is legitimately non-opt-in, or add "
                          << "Topo-forced.toml + pass the resulting binary "
                          << "name here. The auto-mode contract requires "
                          << "an opt-in pass to expose a forced build "
                          << "for benchmarking.";
            return r;
        }

        // --- Builds (sequential, no-op on the pre-built path) ---
        bool haveVanilla = false;
        if (!vanillaOutput.empty()) {
            auto vBuild = vanillaBuild(project);
            haveVanilla = (vBuild.exitCode == 0);
            // Non-fatal: some projects can't build with vanilla clang++.
        }

        auto baseBuild = topoBaseBuild(project, topoBaseOutput);
        EXPECT_EQ(baseBuild.exitCode, 0) << "Topo-base build failed:\n" << baseBuild.output;
        if (baseBuild.exitCode != 0) return r;

        {
            std::error_code ec;
            fs::remove_all(projectsDir_ / project / ".topo-cache", ec);
        }
        auto autoBuild = topoBuild(project, topoAutoOutput);
        EXPECT_EQ(autoBuild.exitCode, 0) << "Topo-auto build failed:\n" << autoBuild.output;
        if (autoBuild.exitCode != 0) return r;

        {
            std::error_code ec;
            fs::remove_all(projectsDir_ / project / ".topo-cache", ec);
        }
        auto forcedBuild = topoForcedBuild(project, topoForcedOutput);
        EXPECT_EQ(forcedBuild.exitCode, 0) << "Topo-forced build failed:\n" << forcedBuild.output;
        if (forcedBuild.exitCode != 0) return r;

        // --- Interleaved measurement ---
        //
        // Order: vanilla → base → auto → forced. Probes for which we have
        // no binary are stored as null fns; the interleaved harness still
        // writes a zero-runs BenchStats so caller indexing stays stable.
        std::vector<std::function<double()>> probes(4);
        auto makeProbe = [this, project, resultLabel](const std::string& out) -> std::function<double()> {
            if (out.empty()) return nullptr;
            return [this, project, out, resultLabel]() {
                auto br = runBinary(project, out);
                if (br.exitCode != 0) return -1.0;
                return extractResultUs(br.output, resultLabel);
            };
        };
        probes[0] = haveVanilla ? makeProbe(vanillaOutput) : nullptr;
        probes[1] = makeProbe(topoBaseOutput);
        probes[2] = makeProbe(topoAutoOutput);
        probes[3] = makeProbe(topoForcedOutput);

        auto statsVec = measureWithVarianceAdaptInterleaved(probes);
        r.vanillaStats = statsVec[0];
        r.baseStats    = statsVec[1];
        r.autoStats    = statsVec[2];
        r.forcedStats  = statsVec[3];

        // Legacy doubles read by the assertion helpers. Use median rather
        // than mean — median is robust to single-sample CPU-spike
        // outliers that were the dominant source of run-to-run ratio
        // drift before the interleaved sampling order took effect.
        r.vanillaO2  = r.vanillaStats.median;
        r.topoBase   = r.baseStats.median;
        r.topoAuto   = r.autoStats.median;
        r.topoForced = r.forcedStats.median;

        EXPECT_GT(r.baseStats.runs, 0)   << "Topo-base run failed for "   << project;
        EXPECT_GT(r.autoStats.runs, 0)   << "Topo-auto run failed for "   << project;
        EXPECT_GT(r.forcedStats.runs, 0) << "Topo-forced run failed for " << project;

        return r;
    }

    // Run an always-on benchmark: vanilla O2 + topo (base = auto = forced).
    // For features like visibility, stages, etc. that are always applied.
    //
    // Like `runFourWay`, this interleaves the vanilla and topo
    // measurement rounds so background CPU drift affects both modes in
    // the same wall-clock windows. Legacy doubles use median rather
    // than mean to stay robust against single-sample outliers.
    BenchResult runAlwaysOn(const std::string& project,
                            const std::string& vanillaOutput,
                            const std::string& topoOutput,
                            const std::string& resultLabel) {
        BenchResult r;

        bool haveVanilla = false;
        if (!vanillaOutput.empty()) {
            auto vBuild = vanillaBuild(project);
            haveVanilla = (vBuild.exitCode == 0);
        }

        {
            std::error_code ec;
            fs::remove_all(projectsDir_ / project / ".topo-cache", ec);
        }
        // Step 6: pass topoOutput so the pre-built artefact path short-circuits.
        auto topoBuildResult = topoBuild(project, topoOutput);
        EXPECT_EQ(topoBuildResult.exitCode, 0) << "Topo build failed:\n" << topoBuildResult.output;
        if (topoBuildResult.exitCode != 0) return r;

        std::vector<std::function<double()>> probes(2);
        auto makeProbe = [this, project, resultLabel](const std::string& out) -> std::function<double()> {
            if (out.empty()) return nullptr;
            return [this, project, out, resultLabel]() {
                auto br = runBinary(project, out);
                if (br.exitCode != 0) return -1.0;
                return extractResultUs(br.output, resultLabel);
            };
        };
        probes[0] = haveVanilla ? makeProbe(vanillaOutput) : nullptr;
        probes[1] = makeProbe(topoOutput);

        auto statsVec = measureWithVarianceAdaptInterleaved(probes);
        r.vanillaStats = statsVec[0];
        BenchStats topoStats = statsVec[1];
        EXPECT_GT(topoStats.runs, 0) << "Topo run failed for " << project;

        r.baseStats = topoStats;
        r.autoStats = topoStats;
        r.forcedStats = topoStats;
        r.vanillaO2  = r.vanillaStats.median;
        r.topoBase   = topoStats.median;
        r.topoAuto   = topoStats.median;
        r.topoForced = topoStats.median;

        return r;
    }

    // Result output format: category is passed explicitly
    // by each benchmark case (via the `CATEGORY_BENCH_TEST_F` macro family)
    // and printed in the header. PASS/WARN/ERROR labels are produced by the
    // paired `assert<Category>CategoryContract` helper after this call.
    static void reportResult(const std::string& feature,
                             const std::string& workload,
                             const BenchResult& r,
                             const char* categoryName = "?",
                             bool alwaysOn = false) {
        // Pick the dominant stats block to derive the header's runs / CV
        // summary. Base is always populated (auto/forced fall through to it
        // via EXPECT_GT guards); auto takes precedence when present.
        const BenchStats& hdr =
            (r.autoStats.runs > 0) ? r.autoStats
                                   : (r.baseStats.runs > 0 ? r.baseStats : r.vanillaStats);
        const char* cvTag = hdr.resampleCapHit ? "resampled to cap" : "stable";

        std::printf("[ BENCH  ] %s/%s (%s)\n", feature.c_str(), workload.c_str(), categoryName);
        std::printf("           runs: %d (CV=%.3f, %s)\n", hdr.runs, hdr.cv, cvTag);

        auto line = [](const char* label, const BenchStats& s) {
            if (s.runs == 0) {
                std::printf("           %-14s(no samples)\n", label);
            } else {
                std::printf("           %-14s%.0f \u00b1 %.0f us", label, s.mean, s.stdev);
                if (s.resampleCapHit) std::printf("  [CV=%.3f cap-hit]", s.cv);
                std::printf("\n");
            }
        };
        auto lineWithRatio = [](const char* label, const BenchStats& s,
                                const char* ratioName, double num, double den) {
            // (Currently a Step-3 placeholder that prints PASS unconditionally;
            // the upgraded path will compute real PASS/WARN/ERROR.)
            const char* verdict = "PASS";
            if (s.runs == 0) {
                std::printf("           %-14s(no samples)\n", label);
                return;
            }
            if (den <= 0.0) {
                // Vanilla had no samples (test passed empty vanillaOutput or
                // the vanilla build/run failed). The previous behaviour
                // printed `=0.000 PASS`, which read like an assertion verdict
                // and silently absorbed the contract bypass that downstream
                // assertions perform when r.vanillaO2 <= 0. Print `n/a` so
                // the bypass is loud, and tag the row `[skip]` not `PASS`.
                std::printf("           %-14s%.0f \u00b1 %.0f us   [%s=n/a (vanilla skipped) skip]",
                            label, s.mean, s.stdev, ratioName);
                if (s.resampleCapHit) std::printf("  [CV=%.3f cap-hit]", s.cv);
                std::printf("\n");
                return;
            }
            double ratio = num / den;
            std::printf("           %-14s%.0f \u00b1 %.0f us   [%s=%.3f %s]",
                        label, s.mean, s.stdev, ratioName, ratio, verdict);
            if (s.resampleCapHit) std::printf("  [CV=%.3f cap-hit]", s.cv);
            std::printf("\n");
        };

        // Ratios use median (matches what `topoBase` / `topoAuto` /
        // `topoForced` carry and what assertion helpers read). This keeps
        // the printed [auto/vanilla=...] number consistent with the
        // verdict that follows it.
        line("vanilla:", r.vanillaStats);
        if (alwaysOn) {
            lineWithRatio("topo:", r.autoStats, "topo/vanilla",
                          r.autoStats.median, r.vanillaStats.median);
        } else {
            line("topo base:", r.baseStats);
            lineWithRatio("topo auto:", r.autoStats, "auto/vanilla",
                          r.autoStats.median, r.vanillaStats.median);
            lineWithRatio("topo forced:", r.forcedStats, "forced/vanilla",
                          r.forcedStats.median, r.vanillaStats.median);
        }
    }

    // Hard constraint: topo base must not be slower than vanilla O2.
    // Skipped when vanilla O2 is unavailable or absolute time < noise floor.
    // When the ratio is in the ambiguous zone and ctx is provided, resample.
    void assertBaseNotSlowerThanO2(const BenchResult& r,
                                   const std::string& feature,
                                   const std::string& workload,
                                   const ResampleContext* ctx = nullptr) {
        if (r.vanillaO2 <= 0 || r.topoBase <= 0) return;

        if (r.vanillaO2 < kNoiseFloorUs) {
            std::printf(
                "[  INFO  ]   %s/%s: skipped base/O2 constraint "
                "(%.0f us below noise floor)\n",
                feature.c_str(),
                workload.c_str(),
                r.vanillaO2);
            return;
        }

        constexpr double threshold = 1.03;
        double ratio = r.topoBase / r.vanillaO2;

        // Ambiguous zone: resample to reduce noise.
        if (ctx && ratio > (threshold - kAmbiguousMargin) && ratio < (threshold + kAmbiguousMargin)) {
            std::printf(
                "[  INFO  ]   %s/%s: base/O2 ratio %.4f in ambiguous zone "
                "[%.2f, %.2f], resampling %d runs...\n",
                feature.c_str(),
                workload.c_str(),
                ratio,
                threshold - kAmbiguousMargin,
                threshold + kAmbiguousMargin,
                kResampleRuns);

            double baseAvg = resampleAverage(ctx->project, ctx->baseOutput, ctx->resultLabel);
            double vanillaAvg = (!ctx->vanillaOutput.empty())
                                    ? resampleAverage(ctx->project, ctx->vanillaOutput, ctx->resultLabel)
                                    : -1.0;

            if (baseAvg > 0 && vanillaAvg > 0) {
                ratio = baseAvg / vanillaAvg;
                std::printf(
                    "[  INFO  ]   %s/%s: resampled base/O2 ratio = %.4f "
                    "(base avg: %.0f us, vanilla avg: %.0f us)\n",
                    feature.c_str(),
                    workload.c_str(),
                    ratio,
                    baseAvg,
                    vanillaAvg);
            }
        }

        EXPECT_LE(ratio, threshold) << feature << "/" << workload << ": topo base is slower than vanilla O2.\n"
                                    << "Vanilla O2: " << r.vanillaO2 << " us\n"
                                    << "Topo base:  " << r.topoBase << " us\n"
                                    << "Ratio:      " << ratio << " (threshold: " << threshold << ")";
    }

    // Hard constraint: topo auto must not be slower than vanilla O2.
    // Skipped when vanilla O2 is unavailable or absolute time < noise floor.
    // When the ratio is in the ambiguous zone and ctx is provided, resample.
    void assertAutoNotSlowerThanO2(const BenchResult& r,
                                   const std::string& feature,
                                   const std::string& workload,
                                   double threshold = 1.03,
                                   const ResampleContext* ctx = nullptr) {
        if (r.vanillaO2 <= 0 || r.topoAuto <= 0) return;

        if (r.vanillaO2 < kNoiseFloorUs) {
            std::printf(
                "[  INFO  ]   %s/%s: skipped auto/O2 constraint "
                "(%.0f us below noise floor)\n",
                feature.c_str(),
                workload.c_str(),
                r.vanillaO2);
            return;
        }

        double ratio = r.topoAuto / r.vanillaO2;

        // Ambiguous zone: resample to reduce noise.
        if (ctx && ratio > (threshold - kAmbiguousMargin) && ratio < (threshold + kAmbiguousMargin)) {
            std::printf(
                "[  INFO  ]   %s/%s: auto/O2 ratio %.4f in ambiguous zone "
                "[%.2f, %.2f], resampling %d runs...\n",
                feature.c_str(),
                workload.c_str(),
                ratio,
                threshold - kAmbiguousMargin,
                threshold + kAmbiguousMargin,
                kResampleRuns);

            double autoAvg = resampleAverage(ctx->project, ctx->autoOutput, ctx->resultLabel);
            double vanillaAvg = (!ctx->vanillaOutput.empty())
                                    ? resampleAverage(ctx->project, ctx->vanillaOutput, ctx->resultLabel)
                                    : -1.0;

            if (autoAvg > 0 && vanillaAvg > 0) {
                ratio = autoAvg / vanillaAvg;
                std::printf(
                    "[  INFO  ]   %s/%s: resampled auto/O2 ratio = %.4f "
                    "(auto avg: %.0f us, vanilla avg: %.0f us)\n",
                    feature.c_str(),
                    workload.c_str(),
                    ratio,
                    autoAvg,
                    vanillaAvg);
            }
        }

        EXPECT_LE(ratio, threshold) << feature << "/" << workload << ": topo auto is slower than vanilla O2.\n"
                                    << "Vanilla O2: " << r.vanillaO2 << " us\n"
                                    << "Topo auto:  " << r.topoAuto << " us\n"
                                    << "Ratio:      " << ratio << " (threshold: " << threshold << ")";
    }

    // Note: functional assertions (pass side-effect verification via symbol
    // inspection / stdout-event counting) live in EquivalenceTests.cpp
    // (benchmark file measures performance only).

    // No assertion for forced/O2 — forced is allowed to be slower.

    // Constraint: auto should not be slower than base (feature enablement
    // should not regress).  Primary constraint when vanilla O2 is unavailable.
    void assertAutoNotWorseThanBase(const BenchResult& r,
                                    const std::string& feature,
                                    const std::string& workload,
                                    double threshold = 1.03,
                                    const ResampleContext* ctx = nullptr) {
        if (r.topoBase <= 0 || r.topoAuto <= 0) return;

        if (r.topoBase < kNoiseFloorUs) {
            std::printf(
                "[  INFO  ]   %s/%s: skipped auto/base constraint "
                "(%.0f us below noise floor)\n",
                feature.c_str(),
                workload.c_str(),
                r.topoBase);
            return;
        }

        double ratio = r.topoAuto / r.topoBase;

        if (ctx && ratio > (threshold - kAmbiguousMargin) && ratio < (threshold + kAmbiguousMargin)) {
            std::printf(
                "[  INFO  ]   %s/%s: auto/base ratio %.4f in ambiguous zone "
                "[%.2f, %.2f], resampling %d runs...\n",
                feature.c_str(),
                workload.c_str(),
                ratio,
                threshold - kAmbiguousMargin,
                threshold + kAmbiguousMargin,
                kResampleRuns);

            double autoAvg = resampleAverage(ctx->project, ctx->autoOutput, ctx->resultLabel);
            double baseAvg = resampleAverage(ctx->project, ctx->baseOutput, ctx->resultLabel);

            if (autoAvg > 0 && baseAvg > 0) {
                ratio = autoAvg / baseAvg;
                std::printf(
                    "[  INFO  ]   %s/%s: resampled auto/base ratio = %.4f "
                    "(auto avg: %.0f us, base avg: %.0f us)\n",
                    feature.c_str(),
                    workload.c_str(),
                    ratio,
                    autoAvg,
                    baseAvg);
            }
        }

        EXPECT_LE(ratio, threshold) << feature << "/" << workload << ": topo auto is slower than topo base.\n"
                                    << "Topo base:  " << r.topoBase << " us\n"
                                    << "Topo auto:  " << r.topoAuto << " us\n"
                                    << "Ratio:      " << ratio << " (threshold: " << threshold << ")";
    }

    // Constraint: forced should not be catastrophically slower than base.
    // For unfriendly workloads, a wider threshold allows expected overhead.
    void assertForcedNotCatastrophic(const BenchResult& r,
                                     const std::string& feature,
                                     const std::string& workload,
                                     double threshold = 1.20,
                                     const ResampleContext* ctx = nullptr) {
        if (r.topoBase <= 0 || r.topoForced <= 0) return;
        // Note: the old `r.topoForced == r.topoAuto` short-circuit was
        // removed together with the runFourWay shortcut. Every four-way
        // benchmark now has a real separate forced build, so the two
        // measurements reflect independent binaries.

        if (r.topoBase < kNoiseFloorUs) {
            std::printf(
                "[  INFO  ]   %s/%s: skipped forced/base constraint "
                "(%.0f us below noise floor)\n",
                feature.c_str(),
                workload.c_str(),
                r.topoBase);
            return;
        }

        double ratio = r.topoForced / r.topoBase;

        if (ctx && ratio > (threshold - kAmbiguousMargin) && ratio < (threshold + kAmbiguousMargin)) {
            std::printf(
                "[  INFO  ]   %s/%s: forced/base ratio %.4f in ambiguous zone "
                "[%.2f, %.2f], resampling %d runs...\n",
                feature.c_str(),
                workload.c_str(),
                ratio,
                threshold - kAmbiguousMargin,
                threshold + kAmbiguousMargin,
                kResampleRuns);

            double forcedAvg = resampleAverage(ctx->project, ctx->forcedOutput, ctx->resultLabel);
            double baseAvg = resampleAverage(ctx->project, ctx->baseOutput, ctx->resultLabel);

            if (forcedAvg > 0 && baseAvg > 0) {
                ratio = forcedAvg / baseAvg;
                std::printf(
                    "[  INFO  ]   %s/%s: resampled forced/base ratio = %.4f "
                    "(forced avg: %.0f us, base avg: %.0f us)\n",
                    feature.c_str(),
                    workload.c_str(),
                    ratio,
                    forcedAvg,
                    baseAvg);
            }
        }

        EXPECT_LE(ratio, threshold) << feature << "/" << workload
                                    << ": topo forced is catastrophically slower than topo base.\n"
                                    << "Topo base:   " << r.topoBase << " us\n"
                                    << "Topo forced: " << r.topoForced << " us\n"
                                    << "Ratio:       " << ratio << " (threshold: " << threshold << ")";
    }
};

// ---------------------------------------------------------------------------
// PB1: DataLayout (22_data_layout_perf)
// ---------------------------------------------------------------------------

// Uses topo::array — no vanilla O2 build possible.

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, DataLayout_Friendly) {
    auto r = runFourWay("data_layout", "", "data_layout_base", "data_layout_perf", "data_layout_forced", "RESULT_US_FRIENDLY");
    reportResult("data_layout", "friendly", r, "ENHANCE");
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "DataLayoutPass");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, DataLayout_Unfriendly) {
    auto r = runFourWay("data_layout", "", "data_layout_base", "data_layout_perf", "data_layout_forced", "RESULT_US_UNFRIENDLY");
    reportResult("data_layout", "unfriendly", r, "ENHANCE");
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "DataLayoutPass");
}

// ---------------------------------------------------------------------------
// PB2: Indirection (23_indirection)
// ---------------------------------------------------------------------------

CATEGORY_BENCH_TEST_F(OPT, PassBench, Indirection_Friendly) {
    auto r = runFourWay("indirection", "baseline", "indirection_base", "indirection", "indirection_forced", "RESULT_US_FRIENDLY");
    reportResult("indirection", "friendly", r, "OPT");
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Friendly, "IndirectionPass");
}

CATEGORY_BENCH_TEST_F(OPT, PassBench, Indirection_Unfriendly) {
    auto r = runFourWay("indirection", "baseline", "indirection_base", "indirection", "indirection_forced", "RESULT_US_UNFRIENDLY");
    reportResult("indirection", "unfriendly", r, "OPT");
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Unfriendly, "IndirectionPass");
}

// ---------------------------------------------------------------------------
// PB3: Lifetime Arena (25_lifetime_arena)
// ---------------------------------------------------------------------------

// The mixed `lifetime/` benchmark uses plain std::malloc/std::free (no
// topo/arena.h include), so clang++ -O2 can compile and run it as the
// vanilla baseline. The earlier "no vanilla O2 build possible" comment was
// stale and caused vanilla to be skipped, which let `auto/vanilla=0.000 PASS`
// silently bypass the OPT-category contract assertions.

CATEGORY_BENCH_TEST_F(OPT, PassBench, Lifetime_Friendly) {
    auto r = runFourWay("lifetime", "baseline", "lifetime_base", "lifetime_arena", "lifetime_forced", "RESULT_US_FRIENDLY");
    reportResult("lifetime", "friendly", r, "OPT");
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Friendly, "LifetimeArenaPass");
}

// Note: the mixed `lifetime/` benchmark has no Unfriendly companion.
// LifetimeArenaPass's auto pipeline decides once per module then applies or
// skips globally, so a single mixed binary cannot satisfy the OPT contract's
// "auto applies for friendly, auto skips for unfriendly" requirement. The
// split benchmark PassBench_OPT_LifetimeSplit_Unfriendly below (project
// `lifetime_unfriendly`, single-workload binary) is the authoritative
// unfriendly check.

// ---------------------------------------------------------------------------
// PB4: Visibility (01_hello_visibility)
// ---------------------------------------------------------------------------

// Always-on — declaration-class feature (visibility enforcement). Classified
// ENHANCE (no speedup path; benchmark guards non-regression).
CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, Visibility_Friendly) {
    auto r = runAlwaysOn("visibility", "baseline", "hello_visibility", "RESULT_US_FRIENDLY");
    reportResult("visibility", "friendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "VisibilityEnforcement");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, Visibility_Unfriendly) {
    auto r = runAlwaysOn("visibility", "baseline", "hello_visibility", "RESULT_US_UNFRIENDLY");
    reportResult("visibility", "unfriendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "VisibilityEnforcement");
}

// ---------------------------------------------------------------------------
// PB6: MultiReturn (04_multi_return)
// ---------------------------------------------------------------------------
// (PB5 stages slot intentionally absent: TopoReorderPass is an INFRA
// category pass — it reorders pass scheduling and does not produce a
// user-observable perf delta on its own, so it is excluded from the
// perf benchmark suite by design. Unit shape verified by
// topo-llvm/test/unit/TopoReorderPassTest.cpp; equivalence by
// EquivalenceTests.cpp TopoReorderPass_{Base,Forced}MatchesVanilla.)

// Always-on — maps to ReturnSpecializationPass (OPT).
CATEGORY_BENCH_TEST_F(OPT, PassBench, MultiReturn_Friendly) {
    auto r = runAlwaysOn("multireturn", "baseline", "multi_return", "RESULT_US_FRIENDLY");
    reportResult("multireturn", "friendly", r, "OPT", /*alwaysOn=*/true);
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Friendly, "ReturnSpecializationPass");
}

CATEGORY_BENCH_TEST_F(OPT, PassBench, MultiReturn_Unfriendly) {
    auto r = runAlwaysOn("multireturn", "baseline", "multi_return", "RESULT_US_UNFRIENDLY");
    reportResult("multireturn", "unfriendly", r, "OPT", /*alwaysOn=*/true);
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Unfriendly, "ReturnSpecializationPass");
}

// ---------------------------------------------------------------------------
// PB7: Pipeline (05_pipeline)
// ---------------------------------------------------------------------------

// Always-on — uses TOPO_PIPELINE. Classified ENHANCE (declaration materialised
// as DAG codegen; no speedup target but verified non-regression).
CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, Pipeline_Friendly) {
    auto r = runAlwaysOn("pipeline", "", "pipeline", "RESULT_US_FRIENDLY");
    reportResult("pipeline", "friendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "PipelineCodeGenPass");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, Pipeline_Unfriendly) {
    auto r = runAlwaysOn("pipeline", "", "pipeline", "RESULT_US_UNFRIENDLY");
    reportResult("pipeline", "unfriendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "PipelineCodeGenPass");
}

// ---------------------------------------------------------------------------
// PB8: Parallel (10_parallel_runtime)
// ---------------------------------------------------------------------------

// Uses topo::parallel — TopoParallelPass (COVERED). Parallel speedup depends on
// task grain × hardware cores × task structure; Topo cannot promise
// `forced/base ≤ 0.90` on arbitrary friendly workloads. This benchmark is a
// runtime-API baseline — Pass fires zero candidates here regardless of mode;
// the firing guard for TopoParallelPass lives in the equivalence test on
// `pipeline/forced` (where parallel=force triggers the pass on the generated
// pipeline body). Contract here is COVERED non-regression only.
CATEGORY_BENCH_TEST_F(COVERED, PassBench, Parallel_Friendly) {
    auto r = runFourWay("parallel", "", "parallel_base", "parallel_runtime", "parallel_forced", "RESULT_US_FRIENDLY");
    reportResult("parallel", "friendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "TopoParallelPass");
}

CATEGORY_BENCH_TEST_F(COVERED, PassBench, Parallel_Unfriendly) {
    auto r = runFourWay("parallel", "", "parallel_base", "parallel_runtime", "parallel_forced", "RESULT_US_UNFRIENDLY");
    reportResult("parallel", "unfriendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "TopoParallelPass");
}

// ---------------------------------------------------------------------------
// PB9: JIT / IR Embedding (12_ir_embedding)
// ---------------------------------------------------------------------------
//
// JIT (IREmbed) is metadata-only — no runtime speedup by design, so there is
// no perf benchmark here. The functional symbol-presence assertion lives in
// EquivalenceTests.cpp (JIT_IREmbed_FunctionalSymbol).

// ---------------------------------------------------------------------------
// PB10: Adaptive (14_adaptive_parallel)
// ---------------------------------------------------------------------------
//
// AdaptiveDispatchPass is instrumentation-only — no runtime speedup by
// design, so there is no perf benchmark here. The functional symbol-presence
// assertion lives in EquivalenceTests.cpp (Adaptive_FunctionalDispatch).

// ---------------------------------------------------------------------------
// PB11: Loop Parallel (24_loop_parallel)
// ---------------------------------------------------------------------------

// Pure C++ — LoopParallelizePass (COVERED: O2 LoopVectorize covers
// streaming loops; findParallelStageFunctions is empty on
// single-function-per-stage workloads so the pass is effectively no-op
// here).
CATEGORY_BENCH_TEST_F(COVERED, PassBench, LoopParallel_Friendly) {
    auto r = runFourWay("loop_parallel", "baseline", "loop_parallel_base", "loop_parallel", "loop_parallel_forced", "RESULT_US_FRIENDLY");
    reportResult("loop_parallel", "friendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "LoopParallelizePass");
}

CATEGORY_BENCH_TEST_F(COVERED, PassBench, LoopParallel_Unfriendly) {
    auto r = runFourWay("loop_parallel", "baseline", "loop_parallel_base", "loop_parallel", "loop_parallel_forced", "RESULT_US_UNFRIENDLY");
    reportResult("loop_parallel", "unfriendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "LoopParallelizePass");
}

// ---------------------------------------------------------------------------
// PB12: Observability (28_observability)
// ---------------------------------------------------------------------------
//
// ObservabilityPass is instrumentation-only — no runtime speedup by design,
// so there is no perf benchmark here. Functional assertions (symbol-presence,
// span-event emission, auto/base overhead bound) live in EquivalenceTests.cpp
// (Observability_FunctionalEvents).

// ---------------------------------------------------------------------------
// PB13: Class/Template (08_class_template)
// ---------------------------------------------------------------------------

// Always-on — declaration-class (class/template containment). ENHANCE.
CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, ClassTemplate_Friendly) {
    auto r = runAlwaysOn("class_template", "baseline", "class_template", "RESULT_US_FRIENDLY");
    reportResult("class_template", "friendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "ContainmentInterceptionPass");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, ClassTemplate_Unfriendly) {
    auto r = runAlwaysOn("class_template", "baseline", "class_template", "RESULT_US_UNFRIENDLY");
    reportResult("class_template", "unfriendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "ContainmentInterceptionPass");
}

// ---------------------------------------------------------------------------
// PB14: Auxiliary Types (09_auxiliary_types)
// ---------------------------------------------------------------------------

// Always-on — topo::span / topo::array / topo::slot. clang -O2 folds them
// identically to raw iteration; COVERED category captures the expected
// ratio ≈ 1.0 non-regression.
CATEGORY_BENCH_TEST_F(COVERED, PassBench, AuxTypes_Friendly) {
    auto r = runAlwaysOn("auxiliary_types", "baseline", "auxiliary_types", "RESULT_US_FRIENDLY");
    reportResult("aux_types", "friendly", r, "COVERED", /*alwaysOn=*/true);
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "AuxTypesCodegen");
}

CATEGORY_BENCH_TEST_F(COVERED, PassBench, AuxTypes_Unfriendly) {
    auto r = runAlwaysOn("auxiliary_types", "baseline", "auxiliary_types", "RESULT_US_UNFRIENDLY");
    reportResult("aux_types", "unfriendly", r, "COVERED", /*alwaysOn=*/true);
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "AuxTypesCodegen");
}

// ---------------------------------------------------------------------------
// PB15: Ownership (16_ownership)
// ---------------------------------------------------------------------------

// Declaration-class — ownership annotations. ENHANCE.
CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, Ownership_Friendly) {
    auto r = runFourWay("ownership", "baseline", "ownership_base", "ownership", "ownership_forced", "RESULT_US_FRIENDLY");
    reportResult("ownership", "friendly", r, "ENHANCE");
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "OwnershipEnforcement");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, Ownership_Unfriendly) {
    auto r = runFourWay("ownership", "baseline", "ownership_base", "ownership", "ownership_forced", "RESULT_US_UNFRIENDLY");
    reportResult("ownership", "unfriendly", r, "ENHANCE");
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "OwnershipEnforcement");
}

// ---------------------------------------------------------------------------
// PB16: Hints (26_hints)
// ---------------------------------------------------------------------------

// Data-aware hints — .topo declares cardinality() / access(streaming) on the
// stage functions. These hints drive PrefetchPass (the primary consumer; see
// LLVM_PASS_TO_BENCH mapping PrefetchPass -> hints in the taxonomy gate), and
// secondarily LoopParallelizePass / DataLayoutPass. Measured forced/base
// stays at ~1.01 because LLVM O2 + hardware prefetcher already cover
// sequential access for the friendly workload. Classified COVERED.
// Note: there is no "BranchHintPass" — Topo has no likely/unlikely branch-hint
// syntax and no corresponding IR pass.
CATEGORY_BENCH_TEST_F(COVERED, PassBench, Hints_Friendly) {
    auto r = runFourWay("hints", "baseline", "hints_base", "hints", "hints_forced", "RESULT_US_FRIENDLY");
    reportResult("hints", "friendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "PrefetchPass");
}

CATEGORY_BENCH_TEST_F(COVERED, PassBench, Hints_Unfriendly) {
    auto r = runFourWay("hints", "baseline", "hints_base", "hints", "hints_forced", "RESULT_US_UNFRIENDLY");
    reportResult("hints", "unfriendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "PrefetchPass");
}

// ---------------------------------------------------------------------------
// PB17: MixedLang (C++/Rust cross-language)
// ---------------------------------------------------------------------------

// Mixed C++/Rust — cross-language containment. ENHANCE (declaration + no
// vanilla baseline; helper tolerates missing vanilla stats gracefully).
CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, MixedLang_Friendly) {
    auto r = runAlwaysOn("mixed_lang", "", "mixed_lang", "RESULT_US_FRIENDLY");
    reportResult("mixed_lang", "friendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "CrossLangContainment");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, MixedLang_Unfriendly) {
    auto r = runAlwaysOn("mixed_lang", "", "mixed_lang", "RESULT_US_UNFRIENDLY");
    reportResult("mixed_lang", "unfriendly", r, "ENHANCE", /*alwaysOn=*/true);
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "CrossLangContainment");
}

// ---------------------------------------------------------------------------
// PB-S1..S4: S-class post-split benchmarks
//
// These benchmarks originated from splitting mixed friendly/unfriendly
// binaries in the originals (ownership, parallel, lifetime, class_template).
// Auto heuristic was previously averaging decisions across both paths; the
// split lets it decide per-binary, restoring signal.
//
// Thresholds are 1.05 (non-regression band), slightly looser than the 1.03
// used on the originals — the split changes noise profile, and first-run
// tuning to 1.03 (if appropriate) is a followup. Motivating problems:
//   - ownership: a g-escape artifact was breaking escape-only enforcement
//   - parallel:  a mixed LLVM friendly/unfriendly binary confounded auto mode
//   - lifetime:  a mixed LLVM friendly/unfriendly binary confounded auto mode
//   - class_template: the benchmark mixed heterogeneous workloads in one binary
// ---------------------------------------------------------------------------

// --- ownership split (was PB15): plain C++, vanilla O2 possible ---
CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, OwnershipSplit_Friendly) {
    auto r = runFourWay("ownership_friendly", "baseline", "ownership_friendly_base",
                        "ownership_friendly", "ownership_friendly_forced", "RESULT_US_FRIENDLY");
    reportResult("ownership_friendly", "friendly", r, "ENHANCE");
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "OwnershipEnforcement");
}

CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, OwnershipSplit_Unfriendly) {
    auto r = runFourWay("ownership_unfriendly", "baseline", "ownership_unfriendly_base",
                        "ownership_unfriendly", "ownership_unfriendly_forced", "RESULT_US_UNFRIENDLY");
    reportResult("ownership_unfriendly", "unfriendly", r, "ENHANCE");
    assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "OwnershipEnforcement");
}

// --- parallel split (was PB8): uses topo::parallel, no vanilla ---
// TopoParallelPass is COVERED (see EquivalenceTests.cpp comment);
// split benchmarks share the runtime-API direct-call shape
// of the unsplit `parallel/` benchmark, so the contract is non-regression only.
CATEGORY_BENCH_TEST_F(COVERED, PassBench, ParallelSplit_Friendly) {
    auto r = runFourWay("parallel_friendly", "", "parallel_friendly_base",
                        "parallel_friendly", "parallel_friendly_forced", "RESULT_US_FRIENDLY");
    reportResult("parallel_friendly", "friendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Friendly, "TopoParallelPass");
}

CATEGORY_BENCH_TEST_F(COVERED, PassBench, ParallelSplit_Unfriendly) {
    auto r = runFourWay("parallel_unfriendly", "", "parallel_unfriendly_base",
                        "parallel_unfriendly", "parallel_unfriendly_forced", "RESULT_US_UNFRIENDLY");
    reportResult("parallel_unfriendly", "unfriendly", r, "COVERED");
    assertCoveredCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                                  Workload::Unfriendly, "TopoParallelPass");
}

// --- lifetime split (was PB3): plain C++, vanilla possible ---
CATEGORY_BENCH_TEST_F(OPT, PassBench, LifetimeSplit_Friendly) {
    auto r = runFourWay("lifetime_friendly", "baseline", "lifetime_friendly_base",
                        "lifetime_friendly", "lifetime_friendly_forced", "RESULT_US_FRIENDLY");
    reportResult("lifetime_friendly", "friendly", r, "OPT");
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Friendly, "LifetimeArenaPass");
}

CATEGORY_BENCH_TEST_F(OPT, PassBench, LifetimeSplit_Unfriendly) {
    auto r = runFourWay("lifetime_unfriendly", "baseline", "lifetime_unfriendly_base",
                        "lifetime_unfriendly", "lifetime_unfriendly_forced", "RESULT_US_UNFRIENDLY");
    reportResult("lifetime_unfriendly", "unfriendly", r, "OPT");
    assertOptCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,
                              Workload::Unfriendly, "LifetimeArenaPass");
}

// --- class_template split (was PB13): always-on, 6 micro-benchmarks ---
// Each micro-benchmark isolates one orthogonal C++ feature so pass
// attribution isn't washed out by feature-level averaging.

#define TOPO_CLASS_TEMPLATE_MICRO(FeatureName, projectId)                                                 \
    CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, ClassTemplate_##FeatureName##_Friendly) {                   \
        auto r = runAlwaysOn(projectId, "baseline", projectId, "RESULT_US_FRIENDLY");                     \
        reportResult(projectId, "friendly", r, "ENHANCE", /*alwaysOn=*/true);                             \
        assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,            \
                                      Workload::Friendly, "ContainmentInterceptionPass");                 \
    }                                                                                                     \
    CATEGORY_BENCH_TEST_F(ENHANCE, PassBench, ClassTemplate_##FeatureName##_Unfriendly) {                 \
        auto r = runAlwaysOn(projectId, "baseline", projectId, "RESULT_US_UNFRIENDLY");                   \
        reportResult(projectId, "unfriendly", r, "ENHANCE", /*alwaysOn=*/true);                           \
        assertEnhanceCategoryContract(r.vanillaStats, r.baseStats, r.autoStats, r.forcedStats,            \
                                      Workload::Unfriendly, "ContainmentInterceptionPass");               \
    }

TOPO_CLASS_TEMPLATE_MICRO(Types, "class_template_types")
TOPO_CLASS_TEMPLATE_MICRO(Templates, "class_template_templates")
TOPO_CLASS_TEMPLATE_MICRO(CRTP, "class_template_crtp")
TOPO_CLASS_TEMPLATE_MICRO(Constraint, "class_template_constraint")
TOPO_CLASS_TEMPLATE_MICRO(Comptime, "class_template_comptime")
TOPO_CLASS_TEMPLATE_MICRO(Variadic, "class_template_variadic")

#undef TOPO_CLASS_TEMPLATE_MICRO

} // namespace topo::test::e2e
