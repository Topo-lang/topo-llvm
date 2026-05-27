// @category: INFRA
#include "topo/Transforms/TopoReorderPass.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include <unordered_map>

namespace topo {

int TopoReorderPass::run(llvm::Module& module,
                         const analysis::StageAnalysisResult& stageAnalysis,
                         const SymbolMapping& mapping) {
    // Build reverse map: Function* -> qualified name
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        funcToName[func] = name;
    }

    int annotatedCount = 0;
    auto& ctx = module.getContext();

    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        // Check if this function has a logic block
        auto nameIt = funcToName.find(&func);
        if (nameIt == funcToName.end()) continue;
        if (stageAnalysis.logicBlockFunctions.count(nameIt->second) == 0) continue;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) continue;
                auto* callee = call->getCalledFunction();
                if (!callee) continue;

                auto cIt = funcToName.find(callee);
                if (cIt == funcToName.end()) continue;

                auto sIt = stageAnalysis.calleeStageMap.find(cIt->second);
                if (sIt == stageAnalysis.calleeStageMap.end()) continue;

                // Attach !topo.stage metadata with the stage number
                auto* stageVal = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), sIt->second);
                auto* md = llvm::MDNode::get(ctx, {llvm::ConstantAsMetadata::get(stageVal)});
                call->setMetadata("topo.stage", md);
                ++annotatedCount;
            }
        }
    }

    return annotatedCount;
}

int TopoReorderPass::run(llvm::Module& module, const SymbolTable& symbols, const SymbolMapping& mapping) {
    auto stageAnalysis = analysis::analyzeStages(symbols);
    return run(module, stageAnalysis, mapping);
}

} // namespace topo
