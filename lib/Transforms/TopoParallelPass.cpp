// @category: COVERED
#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Analysis/PriorityAnalysis.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace topo {

namespace {

/// Check if a function name is in the exclude list.
bool isExcluded(const std::string& name, const std::vector<std::string>& exclude) {
    for (const auto& ex : exclude) {
        if (name == ex) return true;
        // Also check simple name match (after last ::)
        auto pos = name.rfind("::");
        if (pos != std::string::npos && name.substr(pos + 2) == ex) return true;
    }
    return false;
}

/// Get or declare an external C function in the module.
llvm::FunctionCallee getOrDeclareFunc(llvm::Module& module, const std::string& name, llvm::FunctionType* ty) {
    if (auto* existing = module.getFunction(name)) return existing;
    return module.getOrInsertFunction(name, ty);
}

/// Group logic block nodes by stage number.
std::vector<std::pair<int, std::vector<std::string>>> groupNodesByStage(const PipelineAnalysis& analysis) {
    std::map<int, std::vector<std::string>> stageMap;
    for (const auto& [node, stage] : analysis.stages)
        stageMap[stage].push_back(node);

    std::vector<std::pair<int, std::vector<std::string>>> result;
    result.reserve(stageMap.size());
    for (auto& [stage, nodes] : stageMap)
        result.emplace_back(stage, std::move(nodes));
    return result;
}

/// Resolve a simple node name to a qualified function name.
std::string resolveNodeToQualified(const std::string& nodeName, const LogicBlockEntry& logicBlock) {
    for (const auto& calledFunc : logicBlock.calledFunctions) {
        auto pos = calledFunc.rfind("::");
        std::string simpleName = (pos != std::string::npos) ? calledFunc.substr(pos + 2) : calledFunc;
        if (simpleName == nodeName) return calledFunc;
    }
    return nodeName;
}

/// Create a global constant string in the module (for function name literals).
llvm::Constant* getOrCreateGlobalString(llvm::Module& module, const std::string& str) {
    // Look for existing global
    std::string globalName = ".str.topo_cost." + str;
    if (auto* existing = module.getGlobalVariable(globalName))
        return llvm::ConstantExpr::getPointerCast(existing, llvm::PointerType::getUnqual(module.getContext()));

    auto& ctx = module.getContext();
    auto* strConst = llvm::ConstantDataArray::getString(ctx, str, true);
    auto* gv = new llvm::GlobalVariable(
        module, strConst->getType(), true, llvm::GlobalValue::PrivateLinkage, strConst, globalName);
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return llvm::ConstantExpr::getPointerCast(gv, llvm::PointerType::getUnqual(ctx));
}

/// Create (or reuse) a private constant C string under an explicit
/// prefix, returning an i8* to its first element. Same shape as the
/// LifetimeArenaPass / AdaptiveDispatchPass pass-event helpers so the
/// injected `pass`/`subject`/state literals are interned once per
/// module. A distinct prefix (vs the `.str.topo_cost.` one above) keeps
/// the pass-event literals from being conflated with the cost-tracking
/// name globals.
llvm::Constant* getOrCreateGlobalStringPrefixed(llvm::Module& module,
                                                const std::string& str,
                                                const std::string& prefix) {
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

/// Declare the parallel runtime C ABI functions in the module.
struct RuntimeDecls {
    llvm::FunctionCallee ensureInit;
    llvm::FunctionCallee spawnRet;
    llvm::FunctionCallee spawnRetPri;
    llvm::FunctionCallee awaitAll;
    llvm::FunctionCallee costBegin;
    llvm::FunctionCallee costEnd;
    llvm::FunctionCallee passEvent;
};

RuntimeDecls declareRuntime(llvm::Module& module) {
    auto& ctx = module.getContext();
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);

    RuntimeDecls decls;

    // void topo_parallel_ensure_init()
    decls.ensureInit =
        getOrDeclareFunc(module, "topo_parallel_ensure_init", llvm::FunctionType::get(voidTy, {}, false));

    // topo_task_t* topo_task_spawn_ret(fn, arg, result_buf, result_size)
    decls.spawnRet = getOrDeclareFunc(
        module, "topo_task_spawn_ret", llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, i64Ty}, false));

    // topo_task_t* topo_task_spawn_ret_pri(fn, arg, result_buf, result_size, priority)
    decls.spawnRetPri = getOrDeclareFunc(
        module, "topo_task_spawn_ret_pri", llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, i64Ty, i32Ty}, false));

    // void topo_task_await_all(tasks, count)
    decls.awaitAll =
        getOrDeclareFunc(module, "topo_task_await_all", llvm::FunctionType::get(voidTy, {ptrTy, i32Ty}, false));

    // void topo_cost_begin(const char*)
    decls.costBegin = getOrDeclareFunc(module, "topo_cost_begin", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    // void topo_cost_end(const char*)
    decls.costEnd = getOrDeclareFunc(module, "topo_cost_end", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    // Reusable pass-event wire (4-arg form, no size).
    // void topo_pass_event_emit(const char* pass, const char* from,
    //                           const char* to, const char* subject)
    // Plain external declared exactly like topo_cost_begin above. Symbol
    // resolution is guaranteed in the same link domain: this Pass only
    // runs when [parallel] is enabled (run() early-returns otherwise),
    // and TopoParallelPass injects topo_task_spawn*/topo_task_await_all
    // calls into the same TU, so injectAutoLinkLibs() always links
    // -ltopo-parallel; topo-pass-event is wired as its transitive dep
    // (see topo-core/include/topo/Build/AutoLink.h + runtime CMake),
    // mirroring the topo-adaptive / topo-arena precedent. The simpler
    // 4-arg form is used (no natural numeric magnitude at a spawn/join
    // moment) so AdaptiveDispatch/Arena byte layout is irrelevant here.
    decls.passEvent =
        getOrDeclareFunc(module, "topo_pass_event_emit",
                         llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, ptrTy, ptrTy}, false));

    return decls;
}

/// Create a wrapper function for a callee that conforms to the
/// void (*)(void* arg, void* result_buf) signature expected by spawn_ret.
///
/// The wrapper:
///   1. Calls topo_cost_begin(func_name)
///   2. Loads arguments from arg struct
///   3. Calls the original function
///   4. Stores result to result_buf
///   5. Calls topo_cost_end(func_name)
llvm::Function* createTaskWrapper(llvm::Module& module,
                                  llvm::Function* callee,
                                  const std::string& funcName,
                                  const RuntimeDecls& decls,
                                  bool instrument) {
    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Wrapper signature: void wrapper(void* arg_struct, void* result_buf)
    auto* wrapperTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
    std::string wrapperName = funcName + ".topo_parallel_wrap";
    auto* wrapper = llvm::Function::Create(wrapperTy, llvm::GlobalValue::InternalLinkage, wrapperName, module);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
    llvm::IRBuilder<> builder(entry);

    auto* argStructPtr = wrapper->getArg(0);
    auto* resultBufPtr = wrapper->getArg(1);

    // Insert cost_begin if instrumented
    if (instrument) {
        auto* nameStr = getOrCreateGlobalString(module, funcName);
        builder.CreateCall(decls.costBegin, {nameStr});
    }

    // Build the argument struct type from callee's parameters
    std::vector<llvm::Type*> paramTypes;
    for (unsigned i = 0; i < callee->arg_size(); ++i) {
        // Skip sret parameters
        if (callee->getArg(i)->hasStructRetAttr()) continue;
        paramTypes.push_back(callee->getArg(i)->getType());
    }

    auto* argStructTy = llvm::StructType::get(ctx, paramTypes);

    // Load each argument from the struct
    std::vector<llvm::Value*> callArgs;
    unsigned fieldIdx = 0;
    for (unsigned i = 0; i < callee->arg_size(); ++i) {
        if (callee->getArg(i)->hasStructRetAttr()) {
            // Pass result_buf as sret pointer
            callArgs.push_back(resultBufPtr);
            continue;
        }
        auto* fieldPtr = builder.CreateStructGEP(argStructTy, argStructPtr, fieldIdx);
        auto* loaded = builder.CreateLoad(paramTypes[fieldIdx], fieldPtr);
        callArgs.push_back(loaded);
        ++fieldIdx;
    }

    // Call the original function
    auto* result = builder.CreateCall(callee, callArgs);

    // Store result if the function returns non-void and no sret
    bool hasSret = callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr();
    if (!callee->getReturnType()->isVoidTy() && !hasSret) {
        builder.CreateStore(result, resultBufPtr);
    }

    // Insert cost_end if instrumented
    if (instrument) {
        auto* nameStr = getOrCreateGlobalString(module, funcName);
        builder.CreateCall(decls.costEnd, {nameStr});
    }

    builder.CreateRetVoid();
    return wrapper;
}

} // anonymous namespace

int TopoParallelPass::run(llvm::Module& module,
                          const SymbolTable& symbols,
                          const SymbolMapping& mapping,
                          const ParallelConfig& config) {
    if (!config.isEnabled()) return 0;

    int parallelized = 0;

    // Run priority analysis to get effective priorities for task scheduling
    auto priorityResult = analysis::analyzePriority(symbols);

    auto decls = declareRuntime(module);

    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline || !logicBlock.pipelineAnalysis) continue;

        const auto& analysis = *logicBlock.pipelineAnalysis;

        // Find the pipeline function in IR
        auto mapIt = mapping.matched.find(logicBlock.qualifiedName);
        if (mapIt == mapping.matched.end()) continue;
        auto* pipelineFunc = mapIt->second;
        if (!pipelineFunc || pipelineFunc->isDeclaration()) continue;

        auto stageGroups = groupNodesByStage(analysis);

        for (const auto& [stage, nodes] : stageGroups) {
            // Gather candidate nodes — all non-excluded nodes with IR functions
            struct NodeInfo {
                std::string simpleName;
                std::string qualifiedName;
                llvm::Function* irFunc;
            };
            std::vector<NodeInfo> candidates;

            for (const auto& nodeName : nodes) {
                std::string qualName = resolveNodeToQualified(nodeName, logicBlock);
                auto funcIt = mapping.matched.find(qualName);
                llvm::Function* irFunc = (funcIt != mapping.matched.end()) ? funcIt->second : nullptr;

                if (!irFunc || irFunc->isDeclaration()) continue;

                bool excluded = isExcluded(qualName, config.exclude) || isExcluded(nodeName, config.exclude);
                if (excluded) continue;

                candidates.push_back({nodeName, qualName, irFunc});
            }

            if (candidates.empty()) continue;

            // Find the call instructions for these nodes in the pipeline function.
            struct CallTarget {
                llvm::CallInst* call;
                size_t candidateIdx;
            };
            std::vector<CallTarget> targetCalls;

            for (auto& BB : *pipelineFunc) {
                for (auto& I : BB) {
                    auto* callInst = llvm::dyn_cast<llvm::CallInst>(&I);
                    if (!callInst) continue;
                    auto* calledFunc = callInst->getCalledFunction();
                    if (!calledFunc) continue;

                    for (size_t ci = 0; ci < candidates.size(); ++ci) {
                        if (calledFunc == candidates[ci].irFunc) {
                            targetCalls.push_back({callInst, ci});
                            break;
                        }
                    }
                }
            }

            if (targetCalls.empty()) continue;

            // Transform: replace calls with spawn/await pattern
            // Insert before the first call
            auto* firstCall = targetCalls[0].call;
            llvm::IRBuilder<> builder(firstCall);

            // Ensure runtime is initialized
            auto* ensureInitCall = builder.CreateCall(decls.ensureInit);

            // Task spawn/join pass-events.
            //
            // TopoParallelPass's observable runtime moments are: the
            // point a serial pipeline-node call becomes a set of spawned
            // tasks, and the point those tasks are joined back. Emit one
            // pass-event at each moment with subject = the parallelized
            // pipeline (logicBlock.qualifiedName). Encoding mirrors the
            // LifetimeArenaPass precedent's two-word state pair style:
            //   spawn:  from="serial"  to="spawned"
            //   join:   from="spawned" to="joined"
            //
            // A per-(pipeline,stage) one-shot i8 flag makes the record
            // count deterministic and CTest-reproducible: exactly one
            // spawn + one join per parallelized pipeline-stage that ever
            // executes, independent of how many times the pipeline body
            // runs or how many loop iterations drive it — the same
            // at-most-once contract AdaptiveDispatchPass/LifetimeArenaPass
            // apply at their injection points. SplitBlockAndInsertIfThen
            // builds the `if (flag==0) { flag=1; emit(...); }` diamond
            // and rejoins.
            auto* peName =
                getOrCreateGlobalStringPrefixed(module, "TopoParallelPass", ".str.topo_pass.");
            auto* peSubject = getOrCreateGlobalStringPrefixed(
                module, logicBlock.qualifiedName, ".str.topo_pe.subject.");
            auto* peSerial = getOrCreateGlobalStringPrefixed(module, "serial", ".str.topo_pe.");
            auto* peSpawned = getOrCreateGlobalStringPrefixed(module, "spawned", ".str.topo_pe.");
            auto* peJoined = getOrCreateGlobalStringPrefixed(module, "joined", ".str.topo_pe.");

            auto* i8Ty = llvm::Type::getInt8Ty(ctx);
            std::string flagSuffix = logicBlock.qualifiedName + ".s" + std::to_string(stage);
            auto* peSpawnGV = new llvm::GlobalVariable(
                module, i8Ty, false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(i8Ty, 0), "topo.parallel.pe.spawn." + flagSuffix);
            auto* peJoinGV = new llvm::GlobalVariable(
                module, i8Ty, false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(i8Ty, 0), "topo.parallel.pe.join." + flagSuffix);

            // One-shot guarded topo_pass_event_emit() just before
            // `anchor`. `flagGV` is the per-direction one-shot byte.
            auto emitParallelEvent = [&](llvm::Instruction* anchor,
                                         llvm::GlobalVariable* flagGV,
                                         llvm::Constant* from,
                                         llvm::Constant* to) {
                llvm::IRBuilder<> b(anchor);
                auto* flag = b.CreateLoad(i8Ty, flagGV);
                auto* first = b.CreateICmpEQ(flag, llvm::ConstantInt::get(i8Ty, 0));
                llvm::Instruction* thenTerm =
                    llvm::SplitBlockAndInsertIfThen(first, anchor, /*Unreachable=*/false);
                llvm::IRBuilder<> eb(thenTerm);
                eb.CreateStore(llvm::ConstantInt::get(i8Ty, 1), flagGV);
                eb.CreateCall(decls.passEvent, {peName, from, to, peSubject});
            };

            // Spawn moment: emit right after runtime init, before the
            // first task is spawned (serial -> spawned).
            {
                auto spawnAnchorIt = ensureInitCall->getIterator();
                ++spawnAnchorIt;
                emitParallelEvent(&*spawnAnchorIt, peSpawnGV, peSerial, peSpawned);
            }

            // For each call, create wrapper + spawn
            std::vector<llvm::Value*> taskHandles;
            std::vector<llvm::AllocaInst*> resultBufs;
            std::vector<llvm::CallInst*> originalCalls;

            // Insert allocas at function entry
            auto& entryBB = pipelineFunc->getEntryBlock();
            llvm::IRBuilder<> allocaBuilder(&entryBB, entryBB.begin());

            for (auto& tc : targetCalls) {
                auto& cand = candidates[tc.candidateIdx];
                auto* call = tc.call;
                originalCalls.push_back(call);

                auto* callee = call->getCalledFunction();

                // Create argument struct
                std::vector<llvm::Type*> argFieldTypes;
                std::vector<llvm::Value*> argValues;
                for (unsigned ai = 0; ai < call->arg_size(); ++ai) {
                    if (callee->getArg(ai)->hasStructRetAttr()) continue;
                    argFieldTypes.push_back(call->getArgOperand(ai)->getType());
                    argValues.push_back(call->getArgOperand(ai));
                }

                auto* argStructTy = llvm::StructType::get(ctx, argFieldTypes);
                auto* argAlloca = allocaBuilder.CreateAlloca(argStructTy);

                // Store arguments into struct
                for (unsigned fi = 0; fi < argValues.size(); ++fi) {
                    auto* fieldPtr = builder.CreateStructGEP(argStructTy, argAlloca, fi);
                    builder.CreateStore(argValues[fi], fieldPtr);
                }

                // Create result buffer
                auto* resultTy = callee->getReturnType();
                bool hasSret = callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr();
                llvm::Type* storeTy = resultTy;
                if (hasSret) storeTy = callee->getArg(0)->getParamStructRetType();

                auto* resultAlloca =
                    allocaBuilder.CreateAlloca(storeTy->isVoidTy() ? llvm::Type::getInt8Ty(ctx) : storeTy);
                resultBufs.push_back(resultAlloca);

                // Create wrapper function
                auto* wrapper = createTaskWrapper(module, callee, cand.simpleName, decls, config.instrument);

                // Calculate result size
                auto& DL = module.getDataLayout();
                uint64_t resultSize = storeTy->isVoidTy() ? 0 : DL.getTypeAllocSize(storeTy);

                // Determine effective priority for this candidate
                PriorityLevel candPriority = PriorityLevel::Normal;
                auto candPriIt = priorityResult.effectivePriority.find(cand.qualifiedName);
                if (candPriIt != priorityResult.effectivePriority.end()) candPriority = candPriIt->second;

                // Spawn the task (priority-aware if non-Normal)
                llvm::Value* taskHandle;
                if (candPriority != PriorityLevel::Normal) {
                    taskHandle = builder.CreateCall(decls.spawnRetPri,
                                                    {wrapper,
                                                     argAlloca,
                                                     resultAlloca,
                                                     llvm::ConstantInt::get(i64Ty, resultSize),
                                                     llvm::ConstantInt::get(i32Ty, static_cast<int>(candPriority))});
                } else {
                    taskHandle = builder.CreateCall(
                        decls.spawnRet, {wrapper, argAlloca, resultAlloca, llvm::ConstantInt::get(i64Ty, resultSize)});
                }
                taskHandles.push_back(taskHandle);
            }

            // Create task array and await_all
            auto* taskArrayAlloca =
                allocaBuilder.CreateAlloca(ptrTy, llvm::ConstantInt::get(i32Ty, taskHandles.size()));
            for (size_t i = 0; i < taskHandles.size(); ++i) {
                auto* slot = builder.CreateGEP(ptrTy, taskArrayAlloca, llvm::ConstantInt::get(i32Ty, i));
                builder.CreateStore(taskHandles[i], slot);
            }

            auto* awaitAllCall = builder.CreateCall(
                decls.awaitAll, {taskArrayAlloca, llvm::ConstantInt::get(i32Ty, taskHandles.size())});

            // Join moment: emit right after the tasks are awaited
            // (spawned -> joined). Anchored at the instruction following
            // await_all so the event fires once the join has completed.
            {
                auto joinAnchorIt = awaitAllCall->getIterator();
                ++joinAnchorIt;
                emitParallelEvent(&*joinAnchorIt, peJoinGV, peSpawned, peJoined);
            }

            // Replace original call results with loads from result buffers.
            // Two-pass: first create loads and replace uses, then erase calls.
            for (size_t i = 0; i < originalCalls.size(); ++i) {
                auto* call = originalCalls[i];
                auto* callee = call->getCalledFunction();
                auto* resultTy = callee->getReturnType();
                bool hasSret = callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr();

                if (!resultTy->isVoidTy() && !hasSret) {
                    auto* loaded = builder.CreateLoad(resultTy, resultBufs[i]);
                    call->replaceAllUsesWith(loaded);
                }
            }
            // Now safe to erase
            for (auto* call : originalCalls) {
                call->eraseFromParent();
            }

            ++parallelized;
        }
    }

    if (parallelized > 0) {
        // At least one pipeline stage was parallelized — the emitted IR
        // now calls topo_task_spawn* / topo_task_await_all against
        // libtopo-parallel. Wire the one-time ABI-version check matching
        // the pattern in topo-llvm/runtime/ABI-COMPAT.md. Idempotent and
        // shared with LoopParallelizePass — both touch libtopo-parallel.
        injectAbiCheckCtor(module, "parallel", "topo_parallel_version", abi::kParallelVersion);
    }

    return parallelized;
}

} // namespace topo
