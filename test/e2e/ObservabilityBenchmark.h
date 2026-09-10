#ifndef TOPO_TEST_OBSERVABILITY_BENCHMARK_H
#define TOPO_TEST_OBSERVABILITY_BENCHMARK_H

#include "E2eHarness.h"

#include <array>
#include <optional>

namespace topo::test::e2e {

// These limits apply to the observability overhead check. Other benchmark
// categories retain their existing contracts.
constexpr int kObserveBatches = 2;
constexpr int kObserveMinSamples = 3;
constexpr int kObserveMaxRounds = 10;
constexpr double kObserveCvTarget = 0.05;
constexpr double kObserveNoiseFloorUs = 20000.0;
constexpr double kObserveRatioLimit = 1.25;

struct ObservabilityBatch {
    BenchStats base;
    BenchStats autoStats;
    BenchStats forced;
    std::vector<std::string> executionErrors;
};

enum class ObservabilityOutcome { Pass, Regression, Inconclusive, ExecutionError };

struct ObservabilityAssessment {
    ObservabilityOutcome outcome = ObservabilityOutcome::ExecutionError;
    std::array<std::optional<double>, kObserveBatches> autoBaseRatios;
    std::vector<std::string> reasons;
};

struct ObservabilitySummary {
    int passed = 0;
    int regressions = 0;
    int unverified = 0;
    int executionErrors = 0;
};

// Preserve failures even when subsequent samples happen to succeed. Parsing
// requires one complete positive timing line, rather than a numeric prefix.
double readObservabilitySample(const RunResult& result, const std::string& label,
                               const std::string& mode,
                               std::vector<std::string>& errors);

ObservabilityAssessment assessObservability(
    const std::vector<ObservabilityBatch>& batches);
const char* observabilityOutcomeName(ObservabilityOutcome outcome);
ObservabilitySummary summarizeObservability(
    const std::vector<ObservabilityOutcome>& outcomes);
std::string observabilityReport(const std::vector<ObservabilityBatch>& batches,
                                const ObservabilityAssessment& assessment);

} // namespace topo::test::e2e

#endif
