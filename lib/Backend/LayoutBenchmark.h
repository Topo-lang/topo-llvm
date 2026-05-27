#ifndef TOPO_BACKEND_LAYOUTBENCHMARK_H
#define TOPO_BACKEND_LAYOUTBENCHMARK_H

#include "topo/Transforms/DataLayoutPass.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <optional>

namespace topo {

struct LayoutBenchmarkResult {
    enum Winner { AoS, SoA };
    Winner winner;
    double aosMedianNs;
    double soaMedianNs;
    double speedup; // aos/soa ratio (>1 means SoA is faster)
};

class LayoutBenchmark {
public:
    /// Run a micro-benchmark to compare AoS and SoA pipeline variants.
    /// Three-level fallback chain (always returns a result):
    ///   1. Native target: AOT-compile both variants, run, measure
    ///   2. Cross-compilation or benchmark failure: TTI-based static cost model
    ///   3. TTI failure: conservative default (AoS, speedup=1.0)
    ///
    /// When a cardinality hint is provided, the benchmark data size is
    /// derived from the hint (geometric mean of min/max, capped at 256K
    /// elements) instead of a fixed 256KB buffer.
    static std::optional<LayoutBenchmarkResult> run(llvm::Module& module,
                                                    const LayoutVariantPair& variant,
                                                    int warmup,
                                                    int iterations,
                                                    std::optional<CardinalityHint> hint = std::nullopt,
                                                    AccessPattern accessPattern = AccessPattern::None);
};

} // namespace topo

#endif // TOPO_BACKEND_LAYOUTBENCHMARK_H
