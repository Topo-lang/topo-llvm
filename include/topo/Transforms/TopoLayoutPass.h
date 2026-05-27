#ifndef TOPO_IR_TOPOLAYOUTPASS_H
#define TOPO_IR_TOPOLAYOUTPASS_H

#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class TopoLayoutPass {
public:
    // Assign functions to sections based on their stage grouping.
    // Functions called in the same stage are placed in the same section
    // for instruction cache locality.
    // Returns the number of functions with section assignments.
    static int run(llvm::Module& module, const SymbolTable& symbols, const SymbolMapping& mapping);
};

} // namespace topo

#endif // TOPO_IR_TOPOLAYOUTPASS_H
