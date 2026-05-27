#ifndef TOPO_TRANSFORMS_PASSFIREDMARKER_H
#define TOPO_TRANSFORMS_PASSFIREDMARKER_H

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>

namespace topo {

/// Record that a Topo Pass performed a non-trivial transformation on this
/// module. The marker is attached as a module-level named metadata node
/// called "topo.fired.<name>" whose single operand is an i32 MDTuple holding
/// the number of transformations the pass performed.
///
/// The marker is intended as a pass-fired signal for the equivalence test
/// framework — a downstream consumer (E2eHarness) can inspect the emitted
/// `.ll` file and assert that an optimization pass actually did something on
/// the workload, distinguishing "pass ran and preserved semantics" from
/// "pass was a no-op and equivalence is trivially satisfied".
///
/// If `count == 0`, this is a no-op: we only record a marker for passes that
/// actually fired. A missing marker in the IR therefore means "pass did not
/// fire" (either disabled, skipped, or encountered no opportunities).
///
/// Markers accumulate across calls — calling this twice for the same pass
/// name sums the counts into the existing marker.
void markPassFired(llvm::Module& module, llvm::StringRef passName, unsigned count);

} // namespace topo

#endif // TOPO_TRANSFORMS_PASSFIREDMARKER_H
