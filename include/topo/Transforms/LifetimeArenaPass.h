#ifndef TOPO_TRANSFORMS_LIFETIMEARENAPASS_H
#define TOPO_TRANSFORMS_LIFETIMEARENAPASS_H

#include "topo/Backend/SymbolMapper.h"
#include "topo/Build/PassConfig.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <string>
#include <vector>

namespace topo {

class LifetimeArenaPass {
public:
    /// Replace heap allocations in lifetime-scoped functions with arena
    /// allocations, and insert arena create/destroy at scope boundaries.
    /// Used by force mode — applies unconditionally.
    /// Used by auto mode on the variant clone inside VariantBenchmark.
    /// Returns the number of allocations converted.
    static int run(llvm::Module& module,
                   const SymbolTable& symbols,
                   const SymbolMapping& mapping,
                   const LifetimeConfig& config);

    /// List the mangled LLVM names of owner functions that have at least
    /// one lifetime scope with arena-eligible (non-escaping) allocations.
    /// Used by auto mode as the set of candidate benchmark targets.
    static std::vector<std::string> collectOwnerFunctions(llvm::Module& module,
                                                          const SymbolTable& symbols,
                                                          const SymbolMapping& mapping);
};

} // namespace topo

#endif // TOPO_TRANSFORMS_LIFETIMEARENAPASS_H
