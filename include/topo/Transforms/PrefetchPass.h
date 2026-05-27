#ifndef TOPO_TRANSFORMS_PREFETCHPASS_H
#define TOPO_TRANSFORMS_PREFETCHPASS_H

#include "topo/Build/PassConfig.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class PrefetchPass {
public:
    /// Insert llvm.prefetch intrinsics at two insertion points:
    ///
    /// 1. **Streaming loop entries**: For each function with
    ///    accessPattern == Streaming (or Tiled):
    ///      - Run DominatorTree + LoopInfo analysis
    ///      - Insert a prefetch intrinsic in the loop preheader targeting
    ///        the first memory operand in the loop body
    ///
    /// 2. **Pipeline stage boundaries**: After each stage N function call
    ///    completes in a pipeline body, prefetch pointer arguments of the
    ///    stage N+1 function call, so the next stage's input data is in
    ///    cache before execution begins.
    ///
    /// In Auto mode, only Streaming and Tiled patterns are prefetched.
    /// In Force mode, all loops in mapped functions are prefetched
    /// (except Random and GatherScatter, which are always skipped).
    /// Stage boundary prefetch is inserted for all pipeline functions
    /// when the pass is enabled (Auto or Force).
    ///
    /// If `report` is non-null, appends one entry per host function touched
    /// (`{host_function, inserted_hints, distance}`).
    ///
    /// Returns the number of prefetch intrinsics inserted.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const PrefetchConfig& config,
                   backend::PrefetchReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_PREFETCHPASS_H
