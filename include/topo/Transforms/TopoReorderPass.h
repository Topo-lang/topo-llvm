#ifndef TOPO_IR_TOPOREORDERPASS_H
#define TOPO_IR_TOPOREORDERPASS_H

#include "topo/Analysis/StageAnalysis.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class TopoReorderPass {
public:
    // Annotate call instructions with !topo.stage metadata based on stage
    // analysis. Attaches stage number as metadata rather than physically
    // reordering instructions (which is LLVM's scheduler's responsibility).
    // Returns the number of calls annotated.
    //
    // Accepts pre-computed StageAnalysisResult from topo-core/Analysis.
    static int run(llvm::Module& module,
                   const analysis::StageAnalysisResult& stageAnalysis,
                   const SymbolMapping& mapping);

    // Legacy overload: computes stage analysis internally.
    static int run(llvm::Module& module, const SymbolTable& symbols, const SymbolMapping& mapping);
};

} // namespace topo

#endif // TOPO_IR_TOPOREORDERPASS_H
