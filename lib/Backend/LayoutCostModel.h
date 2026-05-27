#ifndef TOPO_BACKEND_LAYOUTCOSTMODEL_H
#define TOPO_BACKEND_LAYOUTCOSTMODEL_H

#include "topo/Transforms/DataLayoutPass.h"
#include "topo/Sema/SymbolTable.h"
#include "LayoutBenchmark.h"

#include <llvm/IR/Module.h>

#include <optional>
#include <string>

namespace topo {

/// Granular TTI cost breakdown for optimization remarks.
struct LayoutCostBreakdown {
    /// Per-instruction-class costs for the AoS variant.
    uint64_t aosMemoryCost = 0;     // load/store via getMemoryOpCost()
    uint64_t aosArithmeticCost = 0; // ALU via getArithmeticInstrCost()
    uint64_t aosOtherCost = 0;      // remaining instructions

    /// Per-instruction-class costs for the SoA variant.
    uint64_t soaMemoryCost = 0;
    uint64_t soaArithmeticCost = 0;
    uint64_t soaOtherCost = 0;

    /// Cache line utilization estimate (0.0 - 1.0).
    /// Ratio of useful bytes accessed per cache line touch.
    double aosCacheUtilization = 0.0;
    double soaCacheUtilization = 0.0;

    /// Vectorization potential: estimated vector width the target can use.
    unsigned targetVectorWidth = 0;

    /// Whether this was a cross-compilation estimate.
    bool isCrossCompilation = false;

    uint64_t aosTotalCost() const { return aosMemoryCost + aosArithmeticCost + aosOtherCost; }
    uint64_t soaTotalCost() const { return soaMemoryCost + soaArithmeticCost + soaOtherCost; }

    /// Format as human-readable string for optimization remarks.
    std::string formatRemark() const;
};

class LayoutCostModel {
public:
    /// Estimate AoS vs SoA performance using TTI cost analysis.
    /// Used as fallback when runtime benchmark is not available
    /// (cross-compilation scenario).
    /// Conservative threshold: SoA must be >30% cheaper to win.
    static std::optional<LayoutBenchmarkResult> estimate(llvm::Module& module,
                                                         const LayoutVariantPair& variant,
                                                         std::optional<CardinalityHint> hint = std::nullopt,
                                                         AccessPattern accessPattern = AccessPattern::None);

    /// Same as estimate(), but also returns the detailed cost breakdown.
    /// The breakdown is populated regardless of whether a winner can be
    /// determined (nullopt result means analysis failure, not AoS default).
    static std::optional<LayoutBenchmarkResult> estimateWithBreakdown(
        llvm::Module& module,
        const LayoutVariantPair& variant,
        LayoutCostBreakdown& breakdown,
        std::optional<CardinalityHint> hint = std::nullopt,
        AccessPattern accessPattern = AccessPattern::None);

    /// Check whether the module targets a different architecture than the host.
    static bool isCrossCompilation(const llvm::Module& module);
};

} // namespace topo

#endif // TOPO_BACKEND_LAYOUTCOSTMODEL_H
