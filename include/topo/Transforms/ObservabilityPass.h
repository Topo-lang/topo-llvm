#ifndef TOPO_TRANSFORMS_OBSERVABILITYPASS_H
#define TOPO_TRANSFORMS_OBSERVABILITYPASS_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class ObservabilityPass {
public:
    /// Insert tracing instrumentation at stage boundaries.
    ///
    /// For pipeline functions: insert topo_trace_span_begin/end around
    /// each stage group of calls.
    /// For fn blocks with stages: insert span_begin/end at stage transitions.
    ///
    /// Returns the number of spans instrumented.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const ObservabilityConfig& config);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_OBSERVABILITYPASS_H
