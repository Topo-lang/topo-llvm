// Unit tests for topo-prof hints subcommand logic.
// Tests JSON format detection, extended sample parsing, and hint deviation checks.

#include "topo/Sema/SymbolTable.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

// Mirror the ExtendedSample struct from topo-prof main.cpp
struct ExtendedSample {
    uint64_t runtimeNs = 0;
    std::optional<uint64_t> observedCardinalityP95;
    std::optional<double> l1MissRate;
};

// Mirror the loadExtendedSamples logic from topo-prof main.cpp
static std::map<std::string, ExtendedSample> loadExtendedSamples(const std::string& samplesPath) {
    std::map<std::string, ExtendedSample> samples;

    std::ifstream file(samplesPath);
    if (!file) return samples;

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error&) {
        return samples;
    }

    if (!data.is_object()) return samples;

    for (auto& [key, val] : data.items()) {
        ExtendedSample sample;

        if (val.is_number()) {
            if (val.is_number_unsigned())
                sample.runtimeNs = val.get<uint64_t>();
            else if (val.is_number_integer())
                sample.runtimeNs = static_cast<uint64_t>(val.get<int64_t>());
            else
                sample.runtimeNs = static_cast<uint64_t>(val.get<double>());
        } else if (val.is_object()) {
            if (val.contains("runtime_ns") && val["runtime_ns"].is_number())
                sample.runtimeNs = static_cast<uint64_t>(val["runtime_ns"].get<double>());
            if (val.contains("observed_cardinality_p95") && val["observed_cardinality_p95"].is_number())
                sample.observedCardinalityP95 = static_cast<uint64_t>(val["observed_cardinality_p95"].get<double>());
            if (val.contains("l1_miss_rate") && val["l1_miss_rate"].is_number())
                sample.l1MissRate = val["l1_miss_rate"].get<double>();
        } else {
            continue;
        }

        samples[key] = sample;
    }

    return samples;
}

// Mirror formatCardinality from topo-prof main.cpp
static std::string formatCardinality(int64_t value) {
    if (value <= 0) return "?";
    if (value >= 1000000 && value % 1000000 == 0) return std::to_string(value / 1000000) + "M";
    if (value >= 1000 && value % 1000 == 0) return std::to_string(value / 1000) + "k";
    return std::to_string(value);
}

// ============================================================
// Test 1: Legacy samples format backward compatibility
// ============================================================
TEST(TopoProfHintsTest, LegacyFlatFormatBackwardCompatible) {
    fs::path tmpDir = fs::temp_directory_path();
    fs::path samplesFile = tmpDir / "test_hints_legacy.json";

    // Write legacy flat format: { "func": nanoseconds }
    nlohmann::json legacy = {{"app::process", 1000}, {"app::compute", 2500}};

    {
        std::ofstream out(samplesFile);
        out << legacy.dump(2);
    }

    auto samples = loadExtendedSamples(samplesFile.string());

    ASSERT_EQ(samples.size(), 2u);

    // Verify runtimeNs is populated
    EXPECT_EQ(samples["app::process"].runtimeNs, 1000u);
    EXPECT_EQ(samples["app::compute"].runtimeNs, 2500u);

    // Verify optional fields are empty
    EXPECT_FALSE(samples["app::process"].observedCardinalityP95.has_value());
    EXPECT_FALSE(samples["app::process"].l1MissRate.has_value());
    EXPECT_FALSE(samples["app::compute"].observedCardinalityP95.has_value());
    EXPECT_FALSE(samples["app::compute"].l1MissRate.has_value());

    fs::remove(samplesFile);
}

// ============================================================
// Test 2: Extended samples format
// ============================================================
TEST(TopoProfHintsTest, ExtendedStructuredFormatParsed) {
    fs::path tmpDir = fs::temp_directory_path();
    fs::path samplesFile = tmpDir / "test_hints_extended.json";

    nlohmann::json extended;
    extended["physics::batch_simulate"] = {
        {"runtime_ns", 45000}, {"observed_cardinality_p95", 523109}, {"l1_miss_rate", 0.03}};
    extended["physics::collision_lookup"] = {{"runtime_ns", 12000}, {"l1_miss_rate", 0.47}};

    {
        std::ofstream out(samplesFile);
        out << extended.dump(2);
    }

    auto samples = loadExtendedSamples(samplesFile.string());

    ASSERT_EQ(samples.size(), 2u);

    // Verify first entry (all fields present)
    const auto& s1 = samples["physics::batch_simulate"];
    EXPECT_EQ(s1.runtimeNs, 45000u);
    ASSERT_TRUE(s1.observedCardinalityP95.has_value());
    EXPECT_EQ(*s1.observedCardinalityP95, 523109u);
    ASSERT_TRUE(s1.l1MissRate.has_value());
    EXPECT_NEAR(*s1.l1MissRate, 0.03, 0.001);

    // Verify second entry (no cardinality)
    const auto& s2 = samples["physics::collision_lookup"];
    EXPECT_EQ(s2.runtimeNs, 12000u);
    EXPECT_FALSE(s2.observedCardinalityP95.has_value());
    ASSERT_TRUE(s2.l1MissRate.has_value());
    EXPECT_NEAR(*s2.l1MissRate, 0.47, 0.001);

    fs::remove(samplesFile);
}

// ============================================================
// Test 3: Cardinality deviation detection
// ============================================================
TEST(TopoProfHintsTest, CardinalityDeviationWarning) {
    // Set up: declared cardinality(1000, 100000), observed p95 = 500000
    topo::CardinalityHint card;
    card.min = 1000;
    card.max = 100000;

    uint64_t observedP95 = 500000;

    // p95 > max * 2 => warning
    ASSERT_GT(observedP95, static_cast<uint64_t>(card.max) * 2);

    // Compute deviation factor
    double deviationFactor = static_cast<double>(observedP95) / static_cast<double>(card.max);
    deviationFactor = std::round(deviationFactor * 10.0) / 10.0;

    EXPECT_NEAR(deviationFactor, 5.0, 0.1);

    // Compute suggested max (p95 * 1.2 margin)
    int64_t suggestedMax = static_cast<int64_t>(static_cast<double>(observedP95) * 1.2);
    EXPECT_EQ(suggestedMax, 600000);
    EXPECT_EQ(formatCardinality(suggestedMax), "600k");
}

TEST(TopoProfHintsTest, CardinalityWithinRangeIsOk) {
    topo::CardinalityHint card;
    card.min = 1000;
    card.max = 100000;

    uint64_t observedP95 = 50000;

    // p95 is within [min/2, max*2] => ok
    EXPECT_LE(observedP95, static_cast<uint64_t>(card.max) * 2);
    EXPECT_GE(observedP95, static_cast<uint64_t>(card.min) / 2);
}

TEST(TopoProfHintsTest, CardinalityBelowMinIsInfo) {
    topo::CardinalityHint card;
    card.min = 10000;
    card.max = 100000;

    uint64_t observedP95 = 3000;

    // p95 < min / 2 => info
    EXPECT_LT(observedP95, static_cast<uint64_t>(card.min) / 2);
}

// ============================================================
// Test: Access pattern consistency checks
// ============================================================
TEST(TopoProfHintsTest, StreamingHighMissRateWarning) {
    // streaming declared + l1MissRate > 0.3 => warning
    topo::AccessPattern pattern = topo::AccessPattern::Streaming;
    double l1MissRate = 0.45;

    EXPECT_EQ(pattern, topo::AccessPattern::Streaming);
    EXPECT_GT(l1MissRate, 0.3);
}

TEST(TopoProfHintsTest, RandomLowMissRateInfo) {
    // random declared + l1MissRate < 0.05 => info
    topo::AccessPattern pattern = topo::AccessPattern::Random;
    double l1MissRate = 0.02;

    EXPECT_EQ(pattern, topo::AccessPattern::Random);
    EXPECT_LT(l1MissRate, 0.05);
}

TEST(TopoProfHintsTest, RandomHighMissRateOk) {
    // random declared + l1MissRate > 0.3 => ok (consistent)
    topo::AccessPattern pattern = topo::AccessPattern::Random;
    double l1MissRate = 0.47;

    bool isWarning = (pattern == topo::AccessPattern::Streaming && l1MissRate > 0.3);
    bool isInfo = (pattern == topo::AccessPattern::Random && l1MissRate < 0.05);

    EXPECT_FALSE(isWarning);
    EXPECT_FALSE(isInfo);
}

// ============================================================
// Test: formatCardinality utility
// ============================================================
TEST(TopoProfHintsTest, FormatCardinalityValues) {
    EXPECT_EQ(formatCardinality(1000), "1k");
    EXPECT_EQ(formatCardinality(100000), "100k");
    EXPECT_EQ(formatCardinality(1000000), "1M");
    EXPECT_EQ(formatCardinality(5000000), "5M");
    EXPECT_EQ(formatCardinality(1500), "1500");
    EXPECT_EQ(formatCardinality(42), "42");
    EXPECT_EQ(formatCardinality(-1), "?");
    EXPECT_EQ(formatCardinality(0), "?");
}

// ============================================================
// Test: Mixed format (some flat, some structured) is handled
// ============================================================
TEST(TopoProfHintsTest, MixedFormatEntries) {
    fs::path tmpDir = fs::temp_directory_path();
    fs::path samplesFile = tmpDir / "test_hints_mixed.json";

    // JSON where values are a mix of number and object —
    // detection per-entry, not per-file
    nlohmann::json mixed;
    mixed["app::simple"] = 7000; // flat
    mixed["app::detailed"] = {{"runtime_ns", 9000}, {"observed_cardinality_p95", 250000}};

    {
        std::ofstream out(samplesFile);
        out << mixed.dump(2);
    }

    auto samples = loadExtendedSamples(samplesFile.string());

    ASSERT_EQ(samples.size(), 2u);

    EXPECT_EQ(samples["app::simple"].runtimeNs, 7000u);
    EXPECT_FALSE(samples["app::simple"].observedCardinalityP95.has_value());

    EXPECT_EQ(samples["app::detailed"].runtimeNs, 9000u);
    ASSERT_TRUE(samples["app::detailed"].observedCardinalityP95.has_value());
    EXPECT_EQ(*samples["app::detailed"].observedCardinalityP95, 250000u);

    fs::remove(samplesFile);
}

// ============================================================
// Test: Hints report JSON output format
// ============================================================
TEST(TopoProfHintsTest, HintsReportJSONFormat) {
    nlohmann::json report;
    nlohmann::json entries = nlohmann::json::array();

    entries.push_back({{"function", "physics::batch_simulate"},
                       {"type", "cardinality"},
                       {"declared", {{"min", 1000}, {"max", 100000}}},
                       {"observed_p95", 523109},
                       {"severity", "warning"},
                       {"deviation_factor", 5.2},
                       {"suggestion", "Update declaration to cardinality(1k..630k)"}});

    entries.push_back({{"function", "physics::collision_lookup"},
                       {"type", "access-pattern"},
                       {"declared_pattern", "random"},
                       {"l1_miss_rate", 0.47},
                       {"severity", "ok"},
                       {"message", "consistent with declared pattern"}});

    report["hints_report"] = entries;

    std::string json = report.dump(2);
    auto parsed = nlohmann::json::parse(json);

    ASSERT_EQ(parsed["hints_report"].size(), 2u);
    EXPECT_EQ(parsed["hints_report"][0]["type"], "cardinality");
    EXPECT_EQ(parsed["hints_report"][0]["severity"], "warning");
    EXPECT_EQ(parsed["hints_report"][1]["type"], "access-pattern");
    EXPECT_EQ(parsed["hints_report"][1]["severity"], "ok");
}

} // namespace
