#include "ObservabilityBenchmark.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace topo::test::e2e {
namespace {

BenchStats stableStats(double median, double cv = 0.0) {
    BenchStats stats;
    stats.median = stats.mean = median;
    stats.stdev = median * cv;
    stats.cv = cv;
    stats.runs = 3;
    stats.samples = {{median * (1.0 - cv), 0}, {median, 1}, {median * (1.0 + cv), 2}};
    return stats;
}

std::vector<ObservabilityBatch> stableBatches(double firstRatio, double secondRatio,
                                             double base = 40000.0, double cv = 0.0) {
    return {{stableStats(base, cv), stableStats(base * firstRatio, cv), stableStats(base, cv), {}},
            {stableStats(base, cv), stableStats(base * secondRatio, cv), stableStats(base, cv), {}}};
}

TEST(ObservabilityBenchmark, TwoStableBatchesPass) {
    EXPECT_EQ(assessObservability(stableBatches(1.10, 1.12)).outcome,
              ObservabilityOutcome::Pass);
}

TEST(ObservabilityBenchmark, TwoStableExceedancesConfirmRegression) {
    EXPECT_EQ(assessObservability(stableBatches(1.30, 1.35)).outcome,
              ObservabilityOutcome::Regression);
}

TEST(ObservabilityBenchmark, BatchesAcrossLimitAreUnverified) {
    EXPECT_EQ(assessObservability(stableBatches(1.20, 1.30)).outcome,
              ObservabilityOutcome::Inconclusive);
    EXPECT_EQ(assessObservability(stableBatches(1.30, 1.20)).outcome,
              ObservabilityOutcome::Inconclusive);
}

TEST(ObservabilityBenchmark, RatioCvAndNoiseFloorBoundariesAreInclusive) {
    EXPECT_EQ(assessObservability(stableBatches(1.25, 1.25, 20000.0, 0.05)).outcome,
              ObservabilityOutcome::Pass);
}

TEST(ObservabilityBenchmark, EveryRequiredModeMustBeStableInBothBatches) {
    for (int batch = 0; batch < 2; ++batch) {
        for (int mode = 0; mode < 3; ++mode) {
            auto batches = stableBatches(1.10, 1.12);
            std::array<BenchStats*, 3> stats = {
                &batches[batch].base, &batches[batch].autoStats, &batches[batch].forced};
            *stats[mode] = stableStats(stats[mode]->median, 0.051);
            EXPECT_EQ(assessObservability(batches).outcome,
                      ObservabilityOutcome::Inconclusive) << batch << "/" << mode;
        }
    }
}

TEST(ObservabilityBenchmark, BaselineBelowNoiseFloorIsUnverified) {
    EXPECT_EQ(assessObservability(stableBatches(1.10, 1.12, 19999.0)).outcome,
              ObservabilityOutcome::Inconclusive);
}

TEST(ObservabilityBenchmark, SamplingCapLeavesPerformanceUnverified) {
    auto batches = stableBatches(1.30, 1.35);
    batches[1].forced.resampleCapHit = true;
    EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::Inconclusive);
}

TEST(ObservabilityBenchmark, MissingRequiredSamplesAreExecutionErrors) {
    for (int mode = 0; mode < 3; ++mode) {
        auto batches = stableBatches(1.10, 1.12);
        std::array<BenchStats*, 3> stats = {&batches[0].base, &batches[0].autoStats, &batches[0].forced};
        stats[mode]->samples.pop_back();
        stats[mode]->runs = 2;
        EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::ExecutionError);
    }
}

TEST(ObservabilityBenchmark, FailedSampleRemainsErrorAfterThreeValidSamples) {
    auto batches = stableBatches(1.10, 1.12);
    batches[0].autoStats.samples.push_back({-1.0, 3});
    batches[0].autoStats.runs = 4;
    EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::ExecutionError);
}

TEST(ObservabilityBenchmark, NonfiniteSampleIsAnExecutionError) {
    auto batches = stableBatches(1.10, 1.12);
    batches[0].autoStats.samples[0].us = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::ExecutionError);
}

TEST(ObservabilityBenchmark, ExecutionErrorTakesPrecedenceOverNoise) {
    auto batches = stableBatches(1.10, 1.12, 10000.0, 0.10);
    batches[1].executionErrors.push_back("forced build failed: linker error");
    const auto assessment = assessObservability(batches);
    EXPECT_EQ(assessment.outcome, ObservabilityOutcome::ExecutionError);
    EXPECT_NE(observabilityReport(batches, assessment).find("linker error"), std::string::npos);
}

TEST(ObservabilityBenchmark, ExactlyTwoBatchesAreRequired) {
    auto batches = stableBatches(1.10, 1.12);
    batches.pop_back();
    EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::ExecutionError);
    batches.resize(3, batches.front());
    EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::ExecutionError);
}

TEST(ObservabilityBenchmark, TimingParserAcceptsCompletePositiveLine) {
    std::vector<std::string> errors;
    EXPECT_DOUBLE_EQ(readObservabilitySample({0, "events\nRESULT_US_FRIENDLY=23456.5\r\n"},
        "RESULT_US_FRIENDLY", "auto", errors), 23456.5);
    EXPECT_TRUE(errors.empty());
}

TEST(ObservabilityBenchmark, ProcessFaultRetainsOutputEvenWithValidTiming) {
    for (const auto& result : {RunResult{1, "RESULT_US_FRIENDLY=30000\nfatal error"},
                               RunResult{0, "spawn failure", false}}) {
        std::vector<std::string> errors;
        EXPECT_LT(readObservabilitySample(result, "RESULT_US_FRIENDLY", "auto", errors), 0);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find(result.output), std::string::npos);
    }
}

TEST(ObservabilityBenchmark, MissingMalformedAndDuplicateTimingsAreErrors) {
    for (const std::string output : {"no timing output", "RESULT_US_FRIENDLY=0",
             "RESULT_US_FRIENDLY=nan", "RESULT_US_FRIENDLY=30000oops",
             "RESULT_US_FRIENDLY=30000\nRESULT_US_FRIENDLY=30000"}) {
        std::vector<std::string> errors;
        EXPECT_LT(readObservabilitySample({0, output}, "RESULT_US_FRIENDLY", "auto", errors), 0);
        EXPECT_EQ(errors.size(), 1u) << output;
    }
}

TEST(ObservabilityBenchmark, InterleavedSamplingStopsAtTheRoundCap) {
    std::vector<int> order;
    auto probe = [&](int mode) {
        return [&, mode]() {
            const auto round = order.size() / 3;
            order.push_back(mode);
            return round % 2 == 0 ? 30000.0 : 60000.0;
        };
    };
    for (int batch = 0; batch < kObserveBatches; ++batch) {
        auto stats = measureWithVarianceAdaptInterleaved({probe(0), probe(1), probe(2)},
            kObserveMinSamples, kObserveMaxRounds, kObserveCvTarget);
        for (const auto& mode : stats) {
            EXPECT_EQ(mode.runs, 10);
            EXPECT_TRUE(mode.resampleCapHit);
        }
    }
    ASSERT_EQ(order.size(), 60u);
    for (std::size_t i = 0; i < order.size(); ++i) EXPECT_EQ(order[i], i % 3);
}

TEST(ObservabilityBenchmark, SamplerCannotHideInvalidSamplesAsStable) {
    int calls = 0;
    auto stats = measureWithVarianceAdaptInterleaved({[&]() {
        return ++calls == 1 ? 30000.0 : -1.0;
    }});
    auto batches = stableBatches(1.10, 1.12);
    batches[0].autoStats = stats.front();
    EXPECT_EQ(assessObservability(batches).outcome, ObservabilityOutcome::ExecutionError);
}

TEST(ObservabilityBenchmark, ReportRetainsEvidenceAndCountsInconclusiveAsUnverified) {
    const auto batches = stableBatches(1.20, 1.30);
    const auto report = nlohmann::json::parse(observabilityReport(batches, assessObservability(batches)));
    EXPECT_EQ(report["outcome"], "inconclusive");
    EXPECT_EQ(report["performance_verified"], false);
    EXPECT_EQ(report["summary"]["passed"], 0);
    EXPECT_EQ(report["summary"]["unverified"], 1);
    ASSERT_EQ(report["batches"].size(), 2u);
    EXPECT_DOUBLE_EQ(report["batches"][1]["auto_base_ratio"].get<double>(), 1.30);
    EXPECT_FALSE(report["reasons"].empty());
    for (const auto& batch : report["batches"]) {
        for (const auto* mode : {"base", "auto", "forced"}) {
            EXPECT_EQ(batch[mode]["samples"].size(), 3u);
            EXPECT_EQ(batch[mode]["valid_samples"], 3);
            EXPECT_EQ(batch[mode]["runs"], 3);
            EXPECT_EQ(batch[mode]["cv"], 0.0);
            EXPECT_GT(batch[mode]["median_us"].get<double>(), 0.0);
        }
    }
}

TEST(ObservabilityBenchmark, SummarySeparatesAllFourOutcomes) {
    const auto summary = summarizeObservability({ObservabilityOutcome::Pass,
        ObservabilityOutcome::Regression, ObservabilityOutcome::Inconclusive,
        ObservabilityOutcome::Inconclusive, ObservabilityOutcome::ExecutionError});
    EXPECT_EQ(summary.passed, 1);
    EXPECT_EQ(summary.regressions, 1);
    EXPECT_EQ(summary.unverified, 2);
    EXPECT_EQ(summary.executionErrors, 1);
}

} // namespace
} // namespace topo::test::e2e
