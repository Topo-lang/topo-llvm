#ifndef TOPO_TRANSFORMS_TOPOPARALLELPASS_H
#define TOPO_TRANSFORMS_TOPOPARALLELPASS_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <string>
#include <vector>

namespace topo {

class TopoParallelPass {
public:
    /// Transform sequential pipeline code into parallel task spawns.
    ///
    /// For each pipeline stage with multiple independent, non-excluded nodes:
    ///   1. Generate wrapper + topo_task_spawn_ret for each node
    ///   2. Insert topo_cost_begin/end for sampling (if instrumented)
    ///   3. Replace sequential calls with spawn/await pattern
    ///
    /// The decision of whether parallelization is beneficial is made by
    /// variant benchmark in PassPipeline (auto mode), not by this pass.
    ///
    /// Returns the number of stages parallelized.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const ParallelConfig& config);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_TOPOPARALLELPASS_H
