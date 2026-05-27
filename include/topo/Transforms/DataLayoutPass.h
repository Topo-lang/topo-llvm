#ifndef TOPO_TRANSFORMS_DATALAYOUTPASS_H
#define TOPO_TRANSFORMS_DATALAYOUTPASS_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <string>
#include <vector>

namespace topo {

/// A pair of AoS (original) and SoA (heap-allocated columns) pipeline variants
/// produced by DataLayoutPass::generateVariants(). The caller benchmarks both
/// and keeps the winner.
struct LayoutVariantPair {
    llvm::Function* aosFn;
    llvm::Function* soaFn;
    std::string pipelineName;
};

class DataLayoutPass {
public:
    /// Legacy mode: unconditional in-place AoS→SoA transform (stack-allocated
    /// columns with scatter/gather).
    /// Returns the number of arrays transformed.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const DataLayoutConfig& config);

    /// Auto-select mode: for each qualifying pipeline function, clone
    /// it and apply SoA transform with heap-allocated columns (aligned_alloc)
    /// to the clone. Returns variant pairs for benchmarking.
    /// The original functions are left untouched (AoS variants).
    static std::vector<LayoutVariantPair> generateVariants(llvm::Module& module,
                                                           const SymbolTable& symbols,
                                                           const SymbolMapping& mapping,
                                                           const DataLayoutConfig& config);

    /// Forced SoA mode: unconditionally apply SoA transform to all qualifying
    /// topo::array<T,N> accesses without benchmarking. Delegates to runGlobal().
    /// Returns the number of arrays transformed.
    static int runForceSoA(llvm::Module& module, const DataLayoutConfig& config);

    /// Module-wide SoA rewrite: rewrite ALL topo::array<T,N> accesses from AoS
    /// layout to SoA layout. No scatter/gather — the physical memory layout
    /// changes globally so all functions see consistent SoA access patterns.
    /// Returns the number of arrays transformed.
    static int runGlobal(llvm::Module& module, const DataLayoutConfig& config);

private:
    /// Implementation: unconditionally applies SoA transform to all qualifying
    /// topo::array<T,N>. Auto-vs-Force decision is delegated to PassPipeline's
    /// VariantBenchmark; once here, the Pass does not make cost/benefit
    /// judgments.
    static int runGlobalImpl(llvm::Module& module);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_DATALAYOUTPASS_H
