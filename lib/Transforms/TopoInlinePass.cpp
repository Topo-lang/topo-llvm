// @category: INFRA
#include "topo/Transforms/TopoInlinePass.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Build/PassConfig.h"

#include <llvm/IR/Instructions.h>

#include <unordered_map>
#include <unordered_set>

namespace topo {

/// Collect all transitive callees of `root` via DFS on the LLVM call graph.
/// Functions involved in call cycles are added to `recursive` and excluded
/// from the result set.
static void collectTransitiveCallees(llvm::Function* root,
                                     std::unordered_set<llvm::Function*>& callees,
                                     std::unordered_set<llvm::Function*>& recursive) {
    std::unordered_set<llvm::Function*> visited;
    std::unordered_set<llvm::Function*> onStack;
    // Iterative DFS with explicit stack (pair: function, phase).
    // phase=false → pre-visit (push children), phase=true → post-visit (pop from stack set).
    std::vector<std::pair<llvm::Function*, bool>> stack;
    stack.push_back({root, false});

    while (!stack.empty()) {
        auto [func, postVisit] = stack.back();
        stack.pop_back();

        if (postVisit) {
            onStack.erase(func);
            continue;
        }

        if (visited.count(func)) continue;
        visited.insert(func);
        onStack.insert(func);
        // Push post-visit marker
        stack.push_back({func, true});

        for (auto& bb : *func) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) continue;
                auto* callee = call->getCalledFunction();
                if (!callee || callee->isDeclaration()) continue;

                if (onStack.count(callee)) {
                    // Cycle detected — mark both participants as recursive
                    recursive.insert(callee);
                    recursive.insert(func);
                    continue;
                }

                callees.insert(callee);

                if (!visited.count(callee)) stack.push_back({callee, false});
            }
        }
    }
}

int TopoInlinePass::run(llvm::Module& module,
                        OptLevel level,
                        const std::vector<VisibilityEntry>& entries,
                        const SymbolMapping& mapping,
                        const SymbolTable* symbols,
                        const InlineConfig* config,
                        backend::TopoInlineReport* report) {
    InlineConfig defaultConfig;
    if (!config) config = &defaultConfig;

    // Build reverse map: Function* → Visibility, and Function* → qualified name
    // (the latter only for report attribution).
    std::unordered_map<llvm::Function*, Visibility> funcVis;
    std::unordered_map<llvm::Function*, std::string> funcQualName;
    for (const auto& entry : entries) {
        auto it = mapping.matched.find(entry.qualifiedName);
        if (it != mapping.matched.end()) {
            funcVis[it->second] = entry.visibility;
            funcQualName[it->second] = entry.qualifiedName;
        }
    }

    auto recordEntry = [&](llvm::Function* func, const char* reason) {
        if (!report) return;
        backend::TopoInlineEntry e;
        auto it = funcQualName.find(func);
        e.callee = (it != funcQualName.end()) ? it->second : func->getName().str();
        e.reason = reason;
        report->entries.push_back(std::move(e));
    };

    // Count call sites per callee to detect single-caller functions
    std::unordered_map<llvm::Function*, int> callCount;
    for (auto& func : module) {
        if (func.isDeclaration()) continue;
        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) continue;
                auto* callee = call->getCalledFunction();
                if (callee && !callee->isDeclaration()) ++callCount[callee];
            }
        }
    }

    int annotated = 0;

    // Step 1: Set LLVM inline attributes based on visibility and call-site count.
    // Let LLVM's own inliner make the actual inline/no-inline decisions.
    for (auto& [func, vis] : funcVis) {
        if (func->isDeclaration()) continue;
        // Skip if already annotated
        if (func->hasFnAttribute(llvm::Attribute::AlwaysInline) || func->hasFnAttribute(llvm::Attribute::InlineHint) ||
            func->hasFnAttribute(llvm::Attribute::NoInline))
            continue;

        int uses = callCount.count(func) ? callCount[func] : 0;

        if (vis == Visibility::Internal) {
            // Internal: must be inlined to erase the symbol from the binary
            func->addFnAttr(llvm::Attribute::AlwaysInline);
            ++annotated;
            recordEntry(func, "internal");
        } else if (vis == Visibility::Private) {
            if (uses <= 1) {
                // Single call site: zero code bloat, always inline
                func->addFnAttr(llvm::Attribute::AlwaysInline);
                recordEntry(func, "private_single_caller");
            } else {
                // Multiple call sites: hint LLVM, let its cost model decide
                func->addFnAttr(llvm::Attribute::InlineHint);
                recordEntry(func, "private_multi_caller");
            }
            ++annotated;
        } else if (vis == Visibility::Protected && level >= OptLevel::O2) {
            // Protected at O2+: hint for small functions only
            func->addFnAttr(llvm::Attribute::InlineHint);
            ++annotated;
            recordEntry(func, "protected");
        }
        // Public: no annotation, defer entirely to LLVM
    }

    // Step 2: Pipeline functor inline expansion.
    // For each pipeline functor, collect all transitive callees from the LLVM
    // call graph and upgrade private/internal ones to alwaysinline. This gives
    // LLVM maximum optimization scope per functor (constant propagation, dead
    // code elimination, register allocation all operate on the entire functor).
    if (symbols) {
        std::unordered_set<llvm::Function*> functorCallees;
        std::unordered_set<llvm::Function*> recursive;

        for (const auto& [name, lb] : symbols->logicBlocks()) {
            if (!lb.isPipeline) continue;
            auto it = mapping.matched.find(lb.qualifiedName);
            if (it == mapping.matched.end() || !it->second) continue;
            if (it->second->isDeclaration()) continue;

            collectTransitiveCallees(it->second, functorCallees, recursive);
        }

        // Upgrade private/internal functor callees to alwaysinline. The
        // Pass does not gate on callee size — LLVM's standard
        // inliner is the cost model authority, and Topo only emits the
        // declaration-driven AlwaysInline hint.
        for (auto* callee : functorCallees) {
            if (recursive.count(callee)) continue;

            // Cross-module (LTO): declarations have no body to inline
            if (callee->isDeclaration()) continue;

            auto visIt = funcVis.find(callee);
            if (visIt == funcVis.end()) continue;
            auto vis = visIt->second;
            if (vis != Visibility::Private && vis != Visibility::Internal) continue;

            // Already alwaysinline — nothing to do
            if (callee->hasFnAttribute(llvm::Attribute::AlwaysInline)) continue;

            // Upgrade from inlinehint (multi-caller private) to alwaysinline
            if (callee->hasFnAttribute(llvm::Attribute::InlineHint)) callee->removeFnAttr(llvm::Attribute::InlineHint);

            callee->addFnAttr(llvm::Attribute::AlwaysInline);
            ++annotated;
            recordEntry(callee, "pipeline_functor");
        }
    }

    return annotated;
}

} // namespace topo
