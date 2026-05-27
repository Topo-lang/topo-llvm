#include "topo/Backend/VisibilityApplier.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/Support/ModRef.h>

#include <string>

namespace topo {

/// Detect COFF target (Windows) from module's target triple.
static bool isCOFFTarget(const llvm::Module& module) {
    auto triple = module.getTargetTriple();
    auto str = triple.str();
    // COFF triples contain "windows" or "win32"
    return str.find("windows") != std::string::npos || str.find("win32") != std::string::npos;
}

/// Strip O0 blocker attributes from all defined functions.
static void stripO0Blockers(llvm::Module& module) {
    for (auto& func : module) {
        if (func.isDeclaration()) continue;
        func.removeFnAttr(llvm::Attribute::OptimizeNone);
        func.removeFnAttr(llvm::Attribute::NoInline);
    }
}

/// Apply a single VisibilityAction to an LLVM function.
static void applyAction(llvm::Function& func,
                        const analysis::VisibilityAction& action,
                        bool isCOFF,
                        llvm::Module& module) {
    // Linkage
    if (action.internalLinkage) {
        func.setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    // Export handling
    if (action.isExport) {
        if (isCOFF) {
            func.setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
        } else {
            func.setVisibility(llvm::GlobalValue::DefaultVisibility);
        }
    }

    // DSO local
    if (action.dsoLocal) {
        func.setDSOLocal(true);
    }

    // Inline hints
    if (action.inlineHint) {
        func.addFnAttr(llvm::Attribute::InlineHint);
    }
    if (action.alwaysInline) {
        func.addFnAttr(llvm::Attribute::AlwaysInline);
        if (action.stripDebug) {
            func.setSubprogram(nullptr);
        } else {
            // Debug mode: rename with prefix
            std::string newName = "__topo_internal_" + func.getName().str();
            func.setName(newName);
        }
    }

    // Const propagation
    if (action.isConst) {
        func.setMemoryEffects(llvm::MemoryEffects::readOnly());
    }

    // Per-parameter attributes
    if (!action.paramConsts.empty()) {
        unsigned argIdx = 0;
        for (auto& arg : func.args()) {
            if (argIdx >= action.paramConsts.size()) break;
            const auto& pc = action.paramConsts[argIdx];
            if (pc.isConst && (pc.modifier == TypeNode::Ref || pc.modifier == TypeNode::Ptr)) {
                arg.addAttr(llvm::Attribute::ReadOnly);
            }
            if (pc.ownership != OwnershipKind::None) {
                std::string mdName = "topo.ownership.";
                mdName += ownershipKindName(pc.ownership);
                auto* md = llvm::MDNode::get(module.getContext(), {});
                func.setMetadata(mdName, md);
            }
            ++argIdx;
        }
    }
}

ApplyStats VisibilityApplier::apply(llvm::Module& module,
                                    const SymbolMapping& mapping,
                                    const std::vector<VisibilityEntry>& entries,
                                    OutputType outputType,
                                    bool debugInternal) {
    // Determine platform from module
    bool isCOFF = isCOFFTarget(module);
    auto platform = isCOFF ? analysis::TargetPlatform::COFF : analysis::TargetPlatform::ELF;

    // Map OutputType to PolicyOutputType
    analysis::PolicyOutputType policyOutput;
    switch (outputType) {
    case OutputType::Shared: policyOutput = analysis::PolicyOutputType::SharedLibrary; break;
    case OutputType::Static: policyOutput = analysis::PolicyOutputType::StaticLibrary; break;
    default: policyOutput = analysis::PolicyOutputType::Executable; break;
    }

    // Compute visibility actions via policy
    auto actions = analysis::computeVisibilityActions(entries, policyOutput, platform, debugInternal);

    return apply(module, mapping, entries, actions);
}

ApplyStats VisibilityApplier::apply(llvm::Module& module,
                                    const SymbolMapping& mapping,
                                    const std::vector<VisibilityEntry>& entries,
                                    const std::vector<analysis::VisibilityAction>& actions) {
    ApplyStats stats;
    bool isCOFF = isCOFFTarget(module);

    // Strip O0 blocker attributes
    stripO0Blockers(module);

    // Apply each entry's action
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto& action = actions[i];

        auto it = mapping.matched.find(entry.qualifiedName);
        if (it == mapping.matched.end()) continue;

        llvm::Function* func = it->second;
        applyAction(*func, action, isCOFF, module);

        // Count stats. `Ignore` entries are intentionally not bucketed —
        // they sit outside the public/protected/private/internal taxonomy
        // and ApplyStats has no `ignoreCount` field.
        switch (entry.visibility) {
        case Visibility::Public: ++stats.publicCount; break;
        case Visibility::Protected: ++stats.protectedCount; break;
        case Visibility::Private: ++stats.privateCount; break;
        case Visibility::Internal: ++stats.internalCount; break;
        case Visibility::Ignore: break;
        }
    }

    return stats;
}

} // namespace topo
