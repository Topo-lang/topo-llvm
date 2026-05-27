#include "LayoutBenchmark.h"
#include "LayoutCostModel.h"
#include "VariantBenchmark.h"

#include <cmath>

namespace topo {

/// Compute a representative element count from a cardinality hint.
/// Uses the geometric mean of min and max, capped at 256K elements.
static std::optional<uint64_t> computeBufferElements(const std::optional<CardinalityHint>& hint) {
    if (!hint) return std::nullopt;
    if (hint->min <= 0 && hint->max <= 0) return std::nullopt;

    int64_t lo = hint->min > 0 ? hint->min : 1;
    int64_t hi = hint->max > 0 ? hint->max : lo;
    if (lo > hi) std::swap(lo, hi);

    auto geoMean = static_cast<uint64_t>(std::sqrt(static_cast<double>(lo) * static_cast<double>(hi)));
    if (geoMean == 0) geoMean = 1;

    constexpr uint64_t maxElements = 256 * 1024;
    return std::min(geoMean, maxElements);
}

std::optional<LayoutBenchmarkResult> LayoutBenchmark::run(llvm::Module& module,
                                                          const LayoutVariantPair& variant,
                                                          int warmup,
                                                          int iterations,
                                                          std::optional<CardinalityHint> hint,
                                                          AccessPattern accessPattern) {
    // Fallback chain:
    //   1. Native target: micro-benchmark (most accurate)
    //   2. Cross-compilation: TTI-based static cost model
    //   3. TTI model failure: conservative default (AoS)

    if (VariantBenchmark::isNativeTarget(module)) {
        // --- Level 1: Micro-benchmark ---
        auto bufferElements = computeBufferElements(hint);

        auto aosFuncName = variant.aosFn->getName().str();
        auto soaFuncName = variant.soaFn->getName().str();

        auto aosMedianNs = VariantBenchmark::compileAndMeasure(
            module, aosFuncName, warmup, iterations, /*optLevel=*/2, bufferElements);
        if (!aosMedianNs) {
            // Benchmark compilation failed on native target; fall through
            // to TTI model rather than giving up entirely.
            auto ttiResult = LayoutCostModel::estimate(module, variant, hint, accessPattern);
            if (ttiResult) return ttiResult;
            // --- Level 3: Conservative default (AoS) ---
            return LayoutBenchmarkResult{LayoutBenchmarkResult::AoS, 0.0, 0.0, 1.0};
        }

        auto soaMedianNs = VariantBenchmark::compileAndMeasure(
            module, soaFuncName, warmup, iterations, /*optLevel=*/2, bufferElements);
        if (!soaMedianNs) {
            auto ttiResult = LayoutCostModel::estimate(module, variant, hint, accessPattern);
            if (ttiResult) return ttiResult;
            return LayoutBenchmarkResult{LayoutBenchmarkResult::AoS, 0.0, 0.0, 1.0};
        }

        if (*aosMedianNs <= 0 || *soaMedianNs <= 0) {
            return LayoutBenchmarkResult{LayoutBenchmarkResult::AoS, 0.0, 0.0, 1.0};
        }

        // Conservative threshold: SoA must be >10% faster to be selected
        double speedup = *aosMedianNs / *soaMedianNs;
        LayoutBenchmarkResult::Winner winner =
            (speedup > 1.10) ? LayoutBenchmarkResult::SoA : LayoutBenchmarkResult::AoS;

        return LayoutBenchmarkResult{winner, *aosMedianNs, *soaMedianNs, speedup};
    }

    // --- Level 2: TTI-based static cost model (cross-compilation) ---
    auto ttiResult = LayoutCostModel::estimate(module, variant, hint, accessPattern);
    if (ttiResult) return ttiResult;

    // --- Level 3: Conservative default (AoS) ---
    // TTI analysis failed (e.g., unknown target). Default to AoS which is
    // always safe — it preserves the original memory layout.
    return LayoutBenchmarkResult{LayoutBenchmarkResult::AoS, 0.0, 0.0, 1.0};
}

} // namespace topo
