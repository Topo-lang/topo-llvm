#ifndef TOPO_IR_VISIBILITYAPPLIER_H
#define TOPO_IR_VISIBILITYAPPLIER_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Analysis/VisibilityPolicy.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Module.h>

#include <vector>

namespace topo {

class VisibilityApplier {
public:
    // Apply Topo visibility rules to LLVM functions.
    // Functions not in the mapping are left unchanged.
    // OutputType controls public symbol treatment:
    //   Shared + COFF -> dllexport, Shared + ELF -> DefaultVisibility
    //   Exe/Static -> dso_local (current behavior)
    ApplyStats apply(llvm::Module& module,
                     const SymbolMapping& mapping,
                     const std::vector<VisibilityEntry>& entries,
                     OutputType outputType = OutputType::Exe,
                     bool debugInternal = false);

    // Apply using pre-computed visibility actions from VisibilityPolicy.
    // actions[i] corresponds to entries[i].
    ApplyStats apply(llvm::Module& module,
                     const SymbolMapping& mapping,
                     const std::vector<VisibilityEntry>& entries,
                     const std::vector<analysis::VisibilityAction>& actions);
};

} // namespace topo

#endif // TOPO_IR_VISIBILITYAPPLIER_H
