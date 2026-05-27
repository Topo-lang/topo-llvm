// @category: INFRA
#include "topo/Transforms/TopoFlattenPass.h"

#include <unordered_map>
#include <vector>

namespace topo {

int TopoFlattenPass::run(llvm::Module& module,
                         const std::vector<VisibilityEntry>& entries,
                         const SymbolMapping& mapping,
                         BuildMode mode,
                         backend::TopoFlattenReport* report) {
    // Build set of removable functions based on build mode:
    //   Dev: private only
    //   Aggressive: private + protected (all TUs merged, InternalLinkage set)
    std::unordered_map<llvm::Function*, std::string> candidates;
    for (const auto& entry : entries) {
        bool eligible = (entry.visibility == Visibility::Private) ||
                        (mode == BuildMode::Aggressive && entry.visibility == Visibility::Protected);
        if (!eligible) continue;
        auto it = mapping.matched.find(entry.qualifiedName);
        if (it != mapping.matched.end()) {
            candidates[it->second] = entry.qualifiedName;
        }
    }

    int demotedCount = 0;

    for (auto& [func, name] : candidates) {
        // Skip if the function no longer exists in the module
        // (may have been removed by previous passes)
        if (func->getParent() != &module) continue;

        // Set internal linkage so LLVM's GlobalDCE can remove unused
        // functions during the standard optimization pipeline.
        if (!func->hasInternalLinkage()) {
            func->setLinkage(llvm::GlobalValue::InternalLinkage);
            ++demotedCount;
            if (report) report->demotedFunctions.push_back(name);
        }
    }

    return demotedCount;
}

} // namespace topo
