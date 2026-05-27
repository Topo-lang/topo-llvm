#ifndef TOPO_IR_TOPOFLATTENPASS_H
#define TOPO_IR_TOPOFLATTENPASS_H

#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Module.h>

#include <vector>

namespace topo {

class TopoFlattenPass {
public:
    // Demote visibility of private/protected functions to InternalLinkage,
    // allowing LLVM's GlobalDCE to remove them in the standard pipeline.
    // Dev mode: demote private functions only.
    // Aggressive mode: also demote protected functions
    //   (safe because all TUs are merged).
    //
    // If `report` is non-null, appends the qualified names of every function
    // demoted on this invocation.
    //
    // Returns the number of functions demoted.
    static int run(llvm::Module& module,
                   const std::vector<VisibilityEntry>& entries,
                   const SymbolMapping& mapping,
                   BuildMode mode = BuildMode::Dev,
                   backend::TopoFlattenReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_IR_TOPOFLATTENPASS_H
