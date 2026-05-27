#ifndef TOPO_IR_PIPELINECODEGENPASS_H
#define TOPO_IR_PIPELINECODEGENPASS_H

#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

namespace topo {

class PipelineCodeGenPass {
public:
    /// Detect pipeline stub functions (those calling pipeline_placeholder)
    /// and replace their bodies with generated code based on the DAG
    /// metadata from the SymbolTable.
    ///
    /// For each pipeline:
    ///   1. Find the stub function via SymbolMapping
    ///   2. Verify it contains a pipeline_placeholder call
    ///   3. Clear the function body
    ///   4. Generate calls in stage order based on PipelineAnalysis
    ///   5. Wire inputs/outputs between nodes
    ///   6. Return the terminal node's result
    ///
    /// Returns the number of pipeline functions generated.
    static int run(llvm::Module& module, const SymbolTable& symbols, const SymbolMapping& mapping);
};

} // namespace topo

#endif // TOPO_IR_PIPELINECODEGENPASS_H
