// @category: INFRA
#include "topo/Transforms/TopoLayoutPass.h"

#include <unordered_map>
#include <unordered_set>

namespace topo {

namespace {

/// Build a platform-appropriate section name for stage layout.
/// ELF: ".text.topo.stage0", Mach-O: "__TEXT,__topo_stg0", PE: ".text.topo.stage0$"
std::string makeStageSectionName(int stage, const llvm::Module& module) {
    std::string suffix = std::to_string(stage);
    auto triple = module.getTargetTriple().str();
    if (triple.find("apple") != std::string::npos || triple.find("darwin") != std::string::npos ||
        triple.find("macos") != std::string::npos) {
        // Mach-O: segment,section,type,attributes — mark as code
        return "__TEXT,__topo_stg" + suffix + ",regular,pure_instructions";
    }
    if (triple.find("win32") != std::string::npos || triple.find("windows") != std::string::npos ||
        triple.find("mingw") != std::string::npos) {
        return ".text.topo.stage" + suffix + "$";
    }
    return ".text.topo.stage" + suffix;
}

} // anonymous namespace

int TopoLayoutPass::run(llvm::Module& module, const SymbolTable& symbols, const SymbolMapping& mapping) {
    // Build reverse map: Function* → qualified name
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        funcToName[func] = name;
    }

    // Build callee name → minimum stage map from logic blocks
    std::unordered_map<std::string, int> calleeMinStage;
    for (const auto& [blockName, block] : symbols.logicBlocks()) {
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }
        for (size_t i = 0; i < block.calledFunctions.size(); ++i) {
            const auto& callee = block.calledFunctions[i];
            if (callee.size() > 8 && callee.substr(0, 8) == "<assign:") {
                continue;
            }
            std::string qualifiedCallee = nsPrefix + callee;
            int stage = block.stages[i];
            auto it = calleeMinStage.find(qualifiedCallee);
            if (it == calleeMinStage.end() || stage < it->second) {
                calleeMinStage[qualifiedCallee] = stage;
            }
        }
    }

    int sectionCount = 0;

    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        auto nameIt = funcToName.find(&func);
        if (nameIt == funcToName.end()) continue;

        auto stageIt = calleeMinStage.find(nameIt->second);
        if (stageIt == calleeMinStage.end()) continue;

        std::string section = makeStageSectionName(stageIt->second, module);
        func.setSection(section);
        ++sectionCount;
    }

    return sectionCount;
}

} // namespace topo
