#ifndef TOPO_IR_TOPOINLINEPASS_H
#define TOPO_IR_TOPOINLINEPASS_H

#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Module.h>

#include <vector>

namespace topo {

struct InlineConfig;

class TopoInlinePass {
public:
    // Set LLVM inline attributes based on .topo visibility declarations.
    // Internal → alwaysinline (erase symbol from binary);
    // single-caller private → alwaysinline; multi-caller private → inlinehint;
    // protected at O2+ → inlinehint; public → no annotation (defer to LLVM).
    //
    // When symbols is provided, pipeline functor inline expansion is enabled:
    // all private/internal transitive callees of pipeline functors are forced
    // to alwaysinline regardless of call-site count, giving LLVM maximum
    // optimization scope per functor. Recursive callees are skipped.
    //
    // Size-aware heuristic: functor callees exceeding the instruction count
    // threshold (default 10000) receive inlinehint instead of alwaysinline,
    // preventing code bloat from large function bodies. Cross-module
    // declarations (no body) are skipped gracefully.
    //
    // If `report` is non-null, populates entries[] with one record per
    // annotated function (`{callee, reason}`).
    //
    // Returns the number of functions annotated.
    static int run(llvm::Module& module,
                   OptLevel level,
                   const std::vector<VisibilityEntry>& entries,
                   const SymbolMapping& mapping,
                   const SymbolTable* symbols = nullptr,
                   const InlineConfig* config = nullptr,
                   backend::TopoInlineReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_IR_TOPOINLINEPASS_H
