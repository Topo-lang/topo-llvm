#ifndef TOPO_TRANSFORMS_INDIRECTIONPASS_H
#define TOPO_TRANSFORMS_INDIRECTIONPASS_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace topo {

struct IndirectionStats {
    int uniquePtrPromoted = 0;
    int sharedPtrOptimized = 0;
    int sharedPtrDereferenced = 0;
    int refcountEliminated = 0;
    int vectorLowered = 0;
    int pointerAttrsAdded = 0;
    int callsDevirtualized = 0;
    int vtableConstantsAnnotated = 0;

    int total() const {
        return uniquePtrPromoted + sharedPtrOptimized + sharedPtrDereferenced + refcountEliminated + vectorLowered +
               pointerAttrsAdded + callsDevirtualized + vtableConstantsAnnotated;
    }
};

class IndirectionPass {
public:
    /// Run the indirection optimization pass.
    /// Must be called after TopoInlinePass (smart pointer bodies are visible)
    /// and before TopoParallelPass (optimized IR enables better parallelization).
    ///
    /// Returns statistics about optimizations performed.
    static IndirectionStats run(llvm::Module& module,
                                const std::vector<VisibilityEntry>& entries,
                                const SymbolMapping& mapping,
                                const SymbolTable& symbols,
                                const IndirectionConfig& config,
                                const std::unordered_set<std::string>* functionFilter = nullptr);

    /// Diagnostic-only mode: scan for optimization opportunities and emit
    /// warnings via llvm::errs(), but do not modify the IR.
    /// Called when user hasn't explicitly configured [optimize.indirection].
    static void diagnose(llvm::Module& module,
                         const std::vector<VisibilityEntry>& entries,
                         const SymbolMapping& mapping,
                         const SymbolTable& symbols);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_INDIRECTIONPASS_H
