// @category: RUNTIME
#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

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

/// Compute TTI cost estimate for a pipeline function.
uint64_t estimatePipelineTTICost(llvm::Function& func) {
    uint64_t cost = 0;
    for (auto& BB : func) {
        for (auto& I : BB)
            cost += 1;
    }
    return cost;
}

/// Create a global constructor entry that calls a registration function.
/// Appends to @llvm.global_ctors.
void addGlobalCtor(llvm::Module& module, llvm::Function* ctorFunc, int priority = 65535) {
    auto& ctx = module.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);

    // Struct type: { i32 priority, void()* ctor, i8* data }
    auto* ctorStructTy = llvm::StructType::get(i32Ty, ptrTy, ptrTy);

    // Build the new entry
    auto* entry = llvm::ConstantStruct::get(ctorStructTy,
                                            llvm::ConstantInt::get(i32Ty, priority),
                                            ctorFunc,
                                            llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx)));

    // Collect existing entries
    std::vector<llvm::Constant*> entries;
    if (auto* existing = module.getNamedGlobal("llvm.global_ctors")) {
        if (auto* init = existing->getInitializer()) {
            if (auto* arr = llvm::dyn_cast<llvm::ConstantArray>(init)) {
                for (unsigned i = 0; i < arr->getNumOperands(); ++i)
                    entries.push_back(arr->getOperand(i));
            }
        }
        existing->eraseFromParent();
    }

    entries.push_back(entry);

    auto* arrTy = llvm::ArrayType::get(ctorStructTy, entries.size());
    auto* arrInit = llvm::ConstantArray::get(arrTy, entries);

    new llvm::GlobalVariable(module, arrTy, false, llvm::GlobalValue::AppendingLinkage, arrInit, "llvm.global_ctors");
}

} // anonymous namespace

int AdaptiveDispatchPass::run(llvm::Module& module,
                              const SymbolTable& symbols,
                              const SymbolMapping& mapping,
                              const AdaptiveConfig& config,
                              const ParallelConfig* parallelCfg,
                              const std::vector<std::string>* excludeFuncs,
                              backend::AdaptiveDispatchReport* report) {
    if (!config.isEnabled()) return 0;

    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);

    // Declare runtime functions
    // void topo_cost_begin(const char*)
    auto costBeginCallee = getOrDeclareFunc(module, "topo_cost_begin", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    // void topo_cost_end(const char*)
    auto costEndCallee = getOrDeclareFunc(module, "topo_cost_end", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    // void topo_adaptive_register(const char*, const char*, void**, uint64_t)
    auto registerCallee = getOrDeclareFunc(
        module, "topo_adaptive_register", llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, ptrTy, i64Ty}, false));

    // Reusable pass-event wire.
    // void topo_pass_event_emit(const char* pass, const char* from,
    //                           const char* to, const char* subject)
    // Plain external, declared exactly like topo_adaptive_register /
    // topo_cost_begin above. Resolution is guaranteed in the same link
    // domain: whenever this Pass runs, [adaptive] is enabled, so
    // injectAutoLinkLibs() adds -ltopo-pass-event alongside
    // -ltopo-adaptive (see topo-core/include/topo/Build/AutoLink.h). An
    // ExternalWeakLinkage declaration was tried first but Mach-O treats
    // an unresolved weak external as a hard link error unless the
    // archive is present anyway, so weak bought nothing and only added
    // a dead null-check — a plain external matches the existing
    // adaptive-symbol contract and the "off ≡ no symbol" guarantee
    // (the Pass early-returns when disabled, so the symbol is never
    // referenced in that case).
    auto* passEventTy =
        llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, ptrTy, ptrTy}, false);
    auto passEventCallee =
        getOrDeclareFunc(module, "topo_pass_event_emit", passEventTy);
    auto* passNameStr =
        getOrCreateGlobalString(module, "AdaptiveDispatchPass", ".str.topo_pass.");
    auto* fromAotStr = getOrCreateGlobalString(module, "aot", ".str.topo_pe.");
    auto* toJitStr = getOrCreateGlobalString(module, "jit", ".str.topo_pe.");

    int instrumented = 0;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline) continue;

        auto mapIt = mapping.matched.find(logicBlock.qualifiedName);
        if (mapIt == mapping.matched.end()) continue;

        auto* pipelineFunc = mapIt->second;
        if (!pipelineFunc || pipelineFunc->isDeclaration()) continue;

        // Skip excluded functions (populated by auto-mode benchmark)
        if (excludeFuncs) {
            bool excluded = false;
            for (const auto& pattern : *excludeFuncs) {
                if (logicBlock.qualifiedName.find(pattern) != std::string::npos) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;
        }

        std::string mangledName = pipelineFunc->getName().str();
        std::string pipelineName = logicBlock.qualifiedName;

        // Pipeline-name global string. Created here (before the dispatch
        // block) because the pass-event call injected into
        // jit_path also needs it as the `subject` argument; the later
        // cost_begin/cost_end/register sites reuse the same constant.
        auto* pipelineNameStr =
            getOrCreateGlobalString(module, pipelineName, ".str.topo_adaptive.");

        // 1. Create global atomic pointer: @<mangled>.__jit_ptr
        auto* jitPtrGV = new llvm::GlobalVariable(module,
                                                  ptrTy,
                                                  false,
                                                  llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantPointerNull::get(ptrTy),
                                                  mangledName + ".__jit_ptr");

        // 2. Rename original entry block
        auto& origEntry = pipelineFunc->getEntryBlock();
        origEntry.setName("aot_entry");

        // 3. Create new entry block before aot_entry
        auto* newEntry = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc, &origEntry);

        // 4. Create jit_path block
        auto* jitBlock = llvm::BasicBlock::Create(ctx, "jit_path", pipelineFunc, &origEntry);

        // The JIT path's actual `ret` does NOT live in jit_path: after the
        // one-shot pass-event split, jit_path ends with a conditional branch
        // and the tail call + return land in the pe_done (alreadyBB) block
        // created below. The cost_end loop in step 6 must skip that block
        // too — cost_begin is only emitted in aot_entry, so any cost_end on
        // the JIT path would be unbalanced. Captured here so it is in scope
        // for the loop.
        llvm::BasicBlock* jitRetBlock = nullptr;

        // 5. Build the dispatch in the new entry block
        {
            llvm::IRBuilder<> builder(newEntry);

            // Load atomic pointer (acquire ordering)
            auto* jitPtr = builder.CreateLoad(ptrTy, jitPtrGV);
            llvm::cast<llvm::LoadInst>(jitPtr)->setAtomic(llvm::AtomicOrdering::Acquire);
            llvm::cast<llvm::LoadInst>(jitPtr)->setAlignment(llvm::Align(8));

            // Compare with null
            auto* hasJit = builder.CreateICmpNE(jitPtr, llvm::ConstantPointerNull::get(ptrTy));

            // Branch: has JIT → jit_path, else → aot_entry
            builder.CreateCondBr(hasJit, jitBlock, &origEntry);

            // In jit_path: tail call the JIT function pointer
            llvm::IRBuilder<> jitBuilder(jitBlock);

            // Variant-dispatch point. This block is reached
            // exactly when the runtime has installed a JIT variant, so
            // the first traversal is the observable AOT→JIT switch from
            // the dispatcher's vantage point. Emit the pass-event once
            // per pipeline via a one-shot i8 flag, so the record count
            // is deterministic (exactly one per pipeline that ever takes
            // the JIT path, independent of call count / JIT timing). The
            // richer runtime-side records (commit/deopt in
            // topo_adaptive.cpp) remain authoritative for full switch
            // history; this injected call satisfies the "Pass injects a
            // runtime call at the dispatch point" contract. The callee
            // is a plain external resolved by injectAutoLinkLibs (no
            // null-guard needed — same contract as topo_adaptive_register).
            {
                auto* emittedGV = new llvm::GlobalVariable(
                    module, llvm::Type::getInt8Ty(ctx), false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0),
                    mangledName + ".__pe_emitted");

                auto* alreadyBB =
                    llvm::BasicBlock::Create(ctx, "pe_done", pipelineFunc);
                auto* emitBB =
                    llvm::BasicBlock::Create(ctx, "pe_emit", pipelineFunc);

                // pe_done holds the JIT path's tail call + ret. Record it so
                // the cost_end loop skips it (see step 6) — keeping the tail
                // call immediately followed by its ret (TCO) and avoiding an
                // unbalanced cost_end on the JIT path.
                jitRetBlock = alreadyBB;

                auto* flag = jitBuilder.CreateLoad(
                    llvm::Type::getInt8Ty(ctx), emittedGV);
                auto* firstTime = jitBuilder.CreateICmpEQ(
                    flag, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0));
                jitBuilder.CreateCondBr(firstTime, emitBB, alreadyBB);

                llvm::IRBuilder<> emitBuilder(emitBB);
                emitBuilder.CreateStore(
                    llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 1),
                    emittedGV);
                emitBuilder.CreateCall(
                    passEventCallee,
                    {passNameStr, fromAotStr, toJitStr, pipelineNameStr});
                emitBuilder.CreateBr(alreadyBB);

                jitBuilder.SetInsertPoint(alreadyBB);
            }

            // Collect pipeline function arguments
            std::vector<llvm::Value*> args;
            for (auto& arg : pipelineFunc->args())
                args.push_back(&arg);

            // Cast jitPtr to the pipeline function type
            auto* funcTy = pipelineFunc->getFunctionType();
            auto* call = jitBuilder.CreateCall(funcTy, jitPtr, args);
            call->setTailCall(true);

            if (pipelineFunc->getReturnType()->isVoidTy()) {
                jitBuilder.CreateRetVoid();
            } else {
                jitBuilder.CreateRet(call);
            }
        }

        // 6. Insert cost_begin at the start of aot_entry,
        //    and cost_end before each return in the AOT path
        //    (pipelineNameStr was created earlier — reused here).
        {
            // cost_begin at start of aot_entry
            llvm::IRBuilder<> aotBuilder(&origEntry, origEntry.begin());
            aotBuilder.CreateCall(costBeginCallee, {pipelineNameStr});
        }

        // Insert cost_end before every return instruction on the AOT path.
        // Skip the whole JIT dispatch path: jit_path itself has no return,
        // and the JIT path's actual return lives in pe_done (jitRetBlock).
        // cost_begin is only emitted at the top of aot_entry, so injecting
        // cost_end on the JIT path would be unbalanced (corrupts adaptive
        // cost accounting — the runtime can consume an outer AOT begin on a
        // re-entrant same-name pipeline) and would also break the tail call
        // by interposing a call between it and its ret. pe_emit ends in an
        // unconditional branch (no return), so it needs no special-casing.
        for (auto& BB : *pipelineFunc) {
            if (&BB == jitBlock) continue;     // skip jit_path (no return)
            if (&BB == jitRetBlock) continue;  // skip pe_done (JIT-path ret)

            auto* terminator = BB.getTerminator();
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(terminator)) {
                llvm::IRBuilder<> retBuilder(ret);
                retBuilder.CreateCall(costEndCallee, {pipelineNameStr});
            }
        }

        // 7. Create a global constructor function that registers this pipeline
        uint64_t ttiCost = estimatePipelineTTICost(*pipelineFunc);

        auto* ctorFuncTy = llvm::FunctionType::get(voidTy, {}, false);
        auto* ctorFunc = llvm::Function::Create(
            ctorFuncTy, llvm::GlobalValue::InternalLinkage, mangledName + ".__adaptive_ctor", module);

        auto* ctorEntry = llvm::BasicBlock::Create(ctx, "entry", ctorFunc);
        llvm::IRBuilder<> ctorBuilder(ctorEntry);

        auto* mangledNameStr = getOrCreateGlobalString(module, mangledName, ".str.topo_mangled.");

        ctorBuilder.CreateCall(registerCallee,
                               {mangledNameStr, pipelineNameStr, jitPtrGV, llvm::ConstantInt::get(i64Ty, ttiCost)});
        ctorBuilder.CreateRetVoid();

        addGlobalCtor(module, ctorFunc);

        ++instrumented;

        if (report) {
            backend::AdaptiveDispatchEntry e;
            e.stageName = pipelineName;
            e.defaultVariant = "aot";
            report->entries.push_back(std::move(e));
        }
    }

    if (instrumented > 0) {
        // At least one adaptive-dispatch entry was instrumented — the
        // emitted IR now calls topo_adaptive_register against
        // libtopo-adaptive. Wire the one-time ABI-version check matching
        // the pattern in topo-llvm/runtime/ABI-COMPAT.md.
        injectAbiCheckCtor(module, "adaptive", "topo_adaptive_version", abi::kAdaptiveVersion);
    }

    return instrumented;
}

} // namespace topo
