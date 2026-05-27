#ifndef TOPO_IR_IREMBED_H
#define TOPO_IR_IREMBED_H

#include "topo/Sema/SymbolTable.h"
#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Backend/SymbolMapper.h"

#include <llvm/IR/Module.h>

#include <cstdint>
#include <string>
#include <vector>

namespace topo {

class IREmbed {
public:
    /// Clone pipeline functions and their callees, serialize as LLVM bitcode.
    static std::vector<uint8_t> serializePipelineIR(const llvm::Module& module,
                                                    const SymbolTable& symbols,
                                                    const SymbolMapping& mapping);

    /// Serialize pre-codegen snapshot: pipeline stubs + stage functions
    /// + transitive callees. Call BEFORE PassPipeline::run() to capture
    /// IR that JIT can re-codegen (stubs still contain pipeline_placeholder).
    static std::vector<uint8_t> serializePreCodegenIR(const llvm::Module& module,
                                                      const SymbolTable& symbols,
                                                      const SymbolMapping& mapping);

    /// Serialize pipeline metadata (DAG structure + parallel/adaptive config) as JSON.
    static std::string serializeMetadata(const SymbolTable& symbols,
                                         const ParallelConfig& parallelCfg,
                                         const AdaptiveConfig* adaptiveCfg = nullptr);

    /// Embed bitcode and metadata as .topo_ir / .topo_meta sections
    /// in the LLVM module.
    static void embed(llvm::Module& module, const std::vector<uint8_t>& bitcode, const std::string& metadata);
};

} // namespace topo

#endif // TOPO_IR_IREMBED_H
