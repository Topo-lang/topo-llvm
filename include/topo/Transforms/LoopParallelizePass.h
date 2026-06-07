#ifndef TOPO_TRANSFORMS_LOOPPARALLELIZEPASS_H
#define TOPO_TRANSFORMS_LOOPPARALLELIZEPASS_H

#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Build/PassConfig.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class LoopParallelizePass {
public:
    /// Step A: Annotate loops in parallel-stage functions with parallel
    /// access metadata and cardinality-based unroll hints.
    ///
    /// For each function that belongs to a parallel stage (same stage
    /// as another function in the same fn block):
    ///   1. Run DominatorTree + LoopInfo analysis
    ///   2. For each natural loop, inject:
    ///      - !llvm.loop.parallel_accesses with access groups
    ///      - !llvm.access.group on memory instructions
    ///   3. If the function has a cardinality hint:
    ///      - max <= 16: emit !llvm.loop.unroll.full
    ///      - max in (16, 256]: emit !llvm.loop.unroll.count = min(max/4, 8)
    ///      - max > 256 or unspecified: let LLVM decide
    ///
    /// Step B (when config.partitionEnabled): Partition loop iteration
    /// space across cores via topo_task_spawn/await.
    ///
    /// For each eligible loop (computable trip count, no cross-iteration
    /// dependencies, no sync primitives):
    ///   1. Outline loop body into a worker function
    ///   2. Partition iteration range [0, N) into chunks
    ///   3. Spawn one task per partition via topo_task_spawn
    ///   4. Insert topo_task_await_all barrier
    ///
    /// Step C (when config.reductionEnabled): Reduction loops — loops
    /// with associative accumulations (add, mul, and, or, xor, fadd,
    /// fmul) that would otherwise be rejected for cross-iteration
    /// dependencies — ARE partitioned. `detectReduction()` accepts a
    /// loop-carried accumulator only when its single in-loop use is the
    /// recurrence binop (the use-restriction guard), and
    /// `ReductionInfo::getIdentity()` supplies the operator identity.
    /// Each spawned partition runs with a partial-accumulator seeded to
    /// that identity; after `topo_task_await_all`, a serial combine loop
    /// folds every partition's partial into one value, then applies the
    /// original loop init exactly once and routes the combined result to
    /// the loop's exit (LCSSA) uses. FP reductions preserve the original
    /// binop's fast-math flags (reassociation is licensed by the
    /// parallel-stage declaration). When config.reductionEnabled is off,
    /// reduction loops are rejected by the cross-iteration gate (Step A
    /// metadata only).
    ///
    /// Safety: loops with cross-iteration data deps, calls to
    /// synchronization primitives, or non-computable trip counts
    /// are left unchanged (Step A metadata only).
    ///
    /// If `report` is non-null, appends one entry per host function whose
    /// loops received Step A annotations (`{host_function, annotated_loops}`).
    /// Step B partitioning is not separately reported — `annotated_loops`
    /// counts Step A metadata injections only.
    ///
    /// Returns the number of loops processed (annotated + partitioned).
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const LoopParallelConfig& config,
                   backend::LoopParallelizeReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_LOOPPARALLELIZEPASS_H
