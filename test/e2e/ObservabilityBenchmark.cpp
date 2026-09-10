#include "ObservabilityBenchmark.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>

namespace topo::test::e2e {

double readObservabilitySample(const RunResult& result, const std::string& label,
                               const std::string& mode,
                               std::vector<std::string>& errors) {
    auto fail = [&](const std::string& message) {
        errors.push_back(mode + ": " + message + "\n" + result.output);
        return -1.0;
    };
    if (!result.spawned || result.exitCode != 0)
        return fail("process failed (spawned=" + std::to_string(result.spawned) +
                    ", exit=" + std::to_string(result.exitCode) + ")");

    const std::regex number(R"([0-9]+(?:\.[0-9]+)?)");
    const std::string prefix = label + "=";
    std::istringstream lines(result.output);
    std::string line;
    int matches = 0;
    double us = -1.0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) != 0) continue;
        ++matches;
        const auto value = line.substr(prefix.size());
        if (!std::regex_match(value, number)) return fail("invalid " + label);
        try {
            us = std::stod(value);
        } catch (const std::exception& e) {
            return fail("invalid " + label + ": " + e.what());
        }
    }
    if (matches != 1 || !std::isfinite(us) || us <= 0.0)
        return fail("expected one positive finite " + label + " value");
    return us;
}

ObservabilityAssessment assessObservability(
    const std::vector<ObservabilityBatch>& batches) {
    ObservabilityAssessment result;
    bool executionError = batches.size() != kObserveBatches;
    if (executionError) result.reasons.push_back("expected exactly two batches");
    bool inconclusive = false;

    for (std::size_t i = 0; i < batches.size(); ++i) {
        const auto& batch = batches[i];
        const std::string prefix = "batch " + std::to_string(i + 1) + ": ";
        for (const auto& error : batch.executionErrors) {
            executionError = true;
            result.reasons.push_back(prefix + error);
        }
        const std::array<const BenchStats*, 3> modes = {
            &batch.base, &batch.autoStats, &batch.forced};
        const std::array<const char*, 3> names = {"base", "auto", "forced"};
        for (std::size_t mode = 0; mode < modes.size(); ++mode) {
            const auto& stats = *modes[mode];
            const auto valid = std::count_if(stats.samples.begin(), stats.samples.end(),
                [](const BenchSample& sample) {
                    return std::isfinite(sample.us) && sample.us > 0.0;
                });
            const std::string context = prefix + names[mode] + ": ";
            if (stats.runs != static_cast<int>(stats.samples.size()) ||
                valid < kObserveMinSamples ||
                valid != static_cast<int>(stats.samples.size()) ||
                stats.runs > kObserveMaxRounds ||
                !std::isfinite(stats.median) || stats.median <= 0.0 ||
                !std::isfinite(stats.mean) || stats.mean <= 0.0 ||
                !std::isfinite(stats.cv) || stats.cv < 0.0) {
                executionError = true;
                result.reasons.push_back(context + "missing or invalid required samples/statistics");
            }
            if (stats.cv > kObserveCvTarget || stats.resampleCapHit) {
                inconclusive = true;
                result.reasons.push_back(context +
                    (stats.resampleCapHit ? "sampling cap reached without stability" :
                                           "CV exceeds 0.05"));
            }
        }
        if (batch.base.median < kObserveNoiseFloorUs) {
            inconclusive = true;
            result.reasons.push_back(prefix + "base median below 20000 us noise floor");
        }
        if (i < result.autoBaseRatios.size() &&
            std::isfinite(batch.base.median) && batch.base.median > 0.0 &&
            std::isfinite(batch.autoStats.median) && batch.autoStats.median > 0.0) {
            result.autoBaseRatios[i] = batch.autoStats.median / batch.base.median;
        }
    }

    if (executionError) return result;
    if (inconclusive) {
        result.outcome = ObservabilityOutcome::Inconclusive;
        return result;
    }
    const bool firstExceeded = *result.autoBaseRatios[0] > kObserveRatioLimit;
    const bool secondExceeded = *result.autoBaseRatios[1] > kObserveRatioLimit;
    if (firstExceeded != secondExceeded) {
        result.outcome = ObservabilityOutcome::Inconclusive;
        result.reasons.push_back("batches disagree across the 1.25 ratio limit");
    } else {
        result.outcome = firstExceeded ? ObservabilityOutcome::Regression :
                                         ObservabilityOutcome::Pass;
    }
    return result;
}

const char* observabilityOutcomeName(ObservabilityOutcome outcome) {
    switch (outcome) {
    case ObservabilityOutcome::Pass: return "pass";
    case ObservabilityOutcome::Regression: return "regression";
    case ObservabilityOutcome::Inconclusive: return "inconclusive";
    case ObservabilityOutcome::ExecutionError: return "execution_error";
    }
    return "execution_error";
}

ObservabilitySummary summarizeObservability(
    const std::vector<ObservabilityOutcome>& outcomes) {
    ObservabilitySummary summary;
    for (const auto outcome : outcomes) {
        switch (outcome) {
        case ObservabilityOutcome::Pass: ++summary.passed; break;
        case ObservabilityOutcome::Regression: ++summary.regressions; break;
        case ObservabilityOutcome::Inconclusive: ++summary.unverified; break;
        case ObservabilityOutcome::ExecutionError: ++summary.executionErrors; break;
        }
    }
    return summary;
}

std::string observabilityReport(const std::vector<ObservabilityBatch>& batches,
                                const ObservabilityAssessment& assessment) {
    using nlohmann::json;
    auto statsJson = [](const BenchStats& stats) {
        json samples = json::array();
        int valid = 0;
        for (const auto& sample : stats.samples) {
            samples.push_back({{"round", sample.runIdx}, {"us", sample.us}});
            if (std::isfinite(sample.us) && sample.us > 0.0) ++valid;
        }
        return json{{"samples", samples}, {"runs", stats.runs},
                    {"valid_samples", valid}, {"median_us", stats.median},
                    {"mean_us", stats.mean}, {"stdev_us", stats.stdev},
                    {"cv", stats.cv}, {"resample_cap_hit", stats.resampleCapHit}};
    };
    json measured = json::array();
    for (std::size_t i = 0; i < batches.size(); ++i) {
        const auto& batch = batches[i];
        json ratio = nullptr;
        if (i < assessment.autoBaseRatios.size() && assessment.autoBaseRatios[i])
            ratio = *assessment.autoBaseRatios[i];
        measured.push_back({{"batch", i + 1}, {"base", statsJson(batch.base)},
            {"auto", statsJson(batch.autoStats)}, {"forced", statsJson(batch.forced)},
            {"auto_base_ratio", ratio}, {"execution_errors", batch.executionErrors}});
    }
    const auto summary = summarizeObservability({assessment.outcome});
    return json{{"benchmark", "observability"},
        {"outcome", observabilityOutcomeName(assessment.outcome)},
        {"performance_verified", assessment.outcome == ObservabilityOutcome::Pass ||
                                 assessment.outcome == ObservabilityOutcome::Regression},
        {"limits", {{"batches", kObserveBatches}, {"min_valid_samples", kObserveMinSamples},
                    {"max_rounds", kObserveMaxRounds}, {"cv", kObserveCvTarget},
                    {"base_floor_us", kObserveNoiseFloorUs}, {"auto_base", kObserveRatioLimit}}},
        {"batches", measured}, {"reasons", assessment.reasons},
        {"summary", {{"passed", summary.passed}, {"regressions", summary.regressions},
                     {"unverified", summary.unverified},
                     {"execution_errors", summary.executionErrors}}}}.dump();
}

} // namespace topo::test::e2e
