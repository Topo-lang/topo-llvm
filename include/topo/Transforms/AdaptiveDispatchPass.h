#ifndef TOPO_TRANSFORMS_ADAPTIVEDISPATCHPASS_H
#define TOPO_TRANSFORMS_ADAPTIVEDISPATCHPASS_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Transforms/TopoParallelPass.h"

#include <llvm/IR/Module.h>

#include <string>
#include <unordered_map>

namespace topo {

class AdaptiveDispatchPass {
public:
    /// Insert adaptive dispatch code into pipeline functions.
    ///
    /// For each pipeline function:
    ///   1. Create a global atomic pointer `@<mangled>.__jit_ptr`
    ///   2. Insert a new entry block that loads the pointer
    ///   3. If non-null, tail-call the JIT version
    ///   4. Otherwise, fall through to the original AOT body
    ///   5. Wrap AOT body with topo_cost_begin/end for pipeline-level timing
    ///   6. Register the pointer via global constructor calling
    ///      topo_adaptive_register()
    ///
    /// If `report` is non-null, appends one entry per instrumented pipeline
    /// (`{stage_name = pipeline_qualified_name, default_variant = "aot"}`).
    ///
    /// Returns the number of pipeline functions instrumented.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const AdaptiveConfig& config,
                   const ParallelConfig* parallelCfg = nullptr,
                   const std::vector<std::string>* excludeFuncs = nullptr,
                   backend::AdaptiveDispatchReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_ADAPTIVEDISPATCHPASS_H
