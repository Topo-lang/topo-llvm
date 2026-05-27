#ifndef TOPO_TRANSFORMS_CONTAINMENTINTERCEPTIONPASS_H
#define TOPO_TRANSFORMS_CONTAINMENTINTERCEPTIONPASS_H

#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class ContainmentInterceptionPass {
public:
    /// Instrument non-external functions that call external APIs.
    /// For each call to an API classified by CapabilityCatalog, inserts a
    /// __topo_containment_violation(caller, callee) call before the original
    /// call instruction. Functions marked `external` in the .topo declaration
    /// are exempt (they are *expected* to call such APIs).
    ///
    /// If `report` is non-null, appends one entry per instrumented call site
    /// (`{caller_function, intercepted_callee}`).
    ///
    /// Returns number of instrumented call sites.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   backend::ContainmentInterceptionReport* report = nullptr);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_CONTAINMENTINTERCEPTIONPASS_H
