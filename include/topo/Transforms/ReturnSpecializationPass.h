#ifndef TOPO_IR_RETURNSPECIALIZATIONPASS_H
#define TOPO_IR_RETURNSPECIALIZATIONPASS_H

#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <vector>

namespace topo {

class ReturnSpecializationPass {
public:
    /// Replace dead return field values with undef based on SymbolTable
    /// demand analysis. Handles both sret (pointer) and direct struct
    /// return (insertvalue) patterns.
    ///
    /// Requires a SymbolTable with CallSiteInfo.usedReturns for demand-
    /// driven live field analysis. Without SymbolTable info, conservatively
    /// keeps all fields (defers to LLVM DSE for IR-level optimization).
    ///
    /// If `report` is non-null, appends one entry per touched host function
    /// with eliminated / kept field indices. Functions skipped (no sret,
    /// conservative, or all-live) do not produce an entry.
    ///
    /// Returns number of fields neutralized.
    static int run(llvm::Module& module,
                   const std::vector<VisibilityEntry>& entries,
                   const SymbolMapping& mapping,
                   const SymbolTable* symbols = nullptr,
                   backend::ReturnSpecializationReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_IR_RETURNSPECIALIZATIONPASS_H
