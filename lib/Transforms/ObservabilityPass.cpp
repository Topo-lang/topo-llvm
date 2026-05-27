// @category: INSTRUMENT
#include "topo/Transforms/ObservabilityPass.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace topo {

namespace {

/// Get or declare an external C function in the module.
llvm::FunctionCallee getOrDeclareFunc(llvm::Module& module, const std::string& name, llvm::FunctionType* ty) {
    if (auto* existing = module.getFunction(name)) return existing;
    return module.getOrInsertFunction(name, ty);
}

/// Create a global constant string.
llvm::Constant* getOrCreateGlobalString(llvm::Module& module, const std::string& str, const std::string& prefix) {
    std::string globalName = prefix + str;
    if (auto* existing = module.getGlobalVariable(globalName))
        return llvm::ConstantExpr::getPointerCast(existing, llvm::PointerType::getUnqual(module.getContext()));

    auto& ctx = module.getContext();
    auto* strConst = llvm::ConstantDataArray::getString(ctx, str, true);
    auto* gv = new llvm::GlobalVariable(
        module, strConst->getType(), true, llvm::GlobalValue::PrivateLinkage, strConst, globalName);
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return llvm::ConstantExpr::getPointerCast(gv, llvm::PointerType::getUnqual(ctx));
}

/// Group calls in a function's entry block by their stage number.
/// Returns pairs of (stage_number, {first_call_index, last_call_index}).
struct StageGroup {
    int stageNumber;
    std::vector<llvm::CallInst*> calls;
};

/// Build stage groups from a logic block's stage info and matching IR calls.
std::vector<StageGroup> buildStageGroups(llvm::Function& func,
                                         const LogicBlockEntry& lb,
                                         const SymbolMapping& mapping) {
    // Collect all call instructions in order from the function
    std::vector<llvm::CallInst*> allCalls;
    for (auto& BB : func) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = call->getCalledFunction();
                if (!callee) continue;
                // Check if this is a topo-declared function (in the mapping)
                std::string calleeName = callee->getName().str();
                bool isMapped = false;
                for (const auto& [topoName, irFunc] : mapping.matched) {
                    if (irFunc && irFunc->getName() == calleeName) {
                        isMapped = true;
                        break;
                    }
                }
                if (isMapped) allCalls.push_back(call);
            }
        }
    }

    // Map called functions to their stage numbers
    // lb.calledFunctions and lb.stages are parallel arrays
    std::unordered_map<std::string, int> funcToStage;
    for (size_t i = 0; i < lb.calledFunctions.size() && i < lb.stages.size(); ++i) {
        funcToStage[lb.calledFunctions[i]] = lb.stages[i];
    }

    // Build a reverse map: IR function name -> stage number
    std::unordered_map<std::string, int> irNameToStage;
    for (const auto& [topoName, stage] : funcToStage) {
        auto it = mapping.matched.find(topoName);
        if (it != mapping.matched.end() && it->second) irNameToStage[it->second->getName().str()] = stage;
    }

    // Group calls by stage
    std::vector<StageGroup> groups;
    int currentStage = -1;

    for (auto* call : allCalls) {
        auto* callee = call->getCalledFunction();
        if (!callee) continue;

        auto it = irNameToStage.find(callee->getName().str());
        int stage = (it != irNameToStage.end()) ? it->second : -1;

        if (stage < 0) continue;

        if (groups.empty() || stage != currentStage) {
            StageGroup sg;
            sg.stageNumber = stage;
            sg.calls.push_back(call);
            groups.push_back(std::move(sg));
            currentStage = stage;
        } else {
            groups.back().calls.push_back(call);
        }
    }

    return groups;
}

} // anonymous namespace

int ObservabilityPass::run(llvm::Module& module,
                           const SymbolTable& symbols,
                           const SymbolMapping& mapping,
                           const ObservabilityConfig& config) {
    if (!config.isEnabled()) return 0;

    // Auto-mode benefit evaluation: observability tracing always adds overhead.
    // ObservabilityPass is INSTRUMENT class — auto = force
    // semantics (declaration-driven always-on instrumentation, no Pass-side
    // value judgment about whether stage count is "worth" instrumenting).
    // The previous "skip when no multi-stage pipeline" gate was a
    // structural heuristic that violated the "Topo doesn't judge" principle.
    // Behavior: if the user enabled observability (mode != Off) and any
    // declaration triggers instrumentation, always emit. The
    // `topo/vanilla < 1.10` overhead ceiling enforced by the INSTRUMENT
    // contract bounds any unwanted cost.

    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Runtime functions are declared lazily (only if actually needed)
    llvm::FunctionCallee spanBeginCallee;
    llvm::FunctionCallee spanEndCallee;
    bool runtimeDeclared = false;

    auto ensureRuntimeDeclared = [&]() {
        if (runtimeDeclared) return;
        spanBeginCallee =
            getOrDeclareFunc(module, "topo_trace_span_begin", llvm::FunctionType::get(voidTy, {ptrTy}, false));
        spanEndCallee = getOrDeclareFunc(module, "topo_trace_span_end", llvm::FunctionType::get(voidTy, {}, false));
        runtimeDeclared = true;
    };

    int instrumented = 0;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        // Find the corresponding IR function
        auto mapIt = mapping.matched.find(logicBlock.qualifiedName);
        if (mapIt == mapping.matched.end()) continue;

        auto* func = mapIt->second;
        if (!func || func->isDeclaration()) continue;

        // Skip internal stages unless config says otherwise
        if (!config.internalStages) {
            // Check if function has internal linkage
            if (func->hasInternalLinkage() || func->hasPrivateLinkage()) continue;
        }

        // Need stages to instrument
        if (logicBlock.stages.empty()) continue;

        // Build stage groups
        auto groups = buildStageGroups(*func, logicBlock, mapping);
        if (groups.empty()) continue;

        // Instrument each stage group
        for (const auto& group : groups) {
            if (group.calls.empty()) continue;

            // Build span name: "namespace::function::stageN"
            std::string spanName = logicBlock.qualifiedName + "::stage" + std::to_string(group.stageNumber);

            ensureRuntimeDeclared();

            auto* spanNameStr = getOrCreateGlobalString(module, spanName, ".str.topo_trace.");

            // Insert span_begin before the first call in the group
            auto* firstCall = group.calls.front();
            llvm::IRBuilder<> beginBuilder(firstCall);
            beginBuilder.CreateCall(spanBeginCallee, {spanNameStr});

            // Insert span_end after the last call in the group
            auto* lastCall = group.calls.back();
            llvm::IRBuilder<> endBuilder(lastCall->getNextNode());
            endBuilder.CreateCall(spanEndCallee, {});

            ++instrumented;
        }
    }

    if (instrumented > 0) {
        // At least one tracing span was wired in — the emitted IR now
        // calls topo_trace_span_begin / topo_trace_span_end against
        // libtopo-observe. Wire the one-time ABI-version check matching
        // the pattern in topo-llvm/runtime/ABI-COMPAT.md.
        injectAbiCheckCtor(module, "observe", "topo_trace_version", abi::kObserveVersion);
    }

    return instrumented;
}

} // namespace topo
