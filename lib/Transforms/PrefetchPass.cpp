// @category: COVERED
#include "topo/Transforms/PrefetchPass.h"
#include "topo/Analysis/PipelineAnalysis.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <unordered_map>

namespace topo {

/// Build a map from function name -> AccessPattern for all functions
/// that have a non-None access pattern in the SymbolTable.
static std::unordered_map<std::string, AccessPattern> buildAccessPatternMap(const SymbolTable& symbols) {
    std::unordered_map<std::string, AccessPattern> result;

    for (const auto& [blockName, block] : symbols.logicBlocks()) {
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }

        for (const auto& callee : block.calledFunctions) {
            if (callee.size() > 8 && callee.substr(0, 8) == "<assign:") continue;

            std::string qualified = block.isPipeline ? callee : (nsPrefix + callee);
            auto* fnSym = symbols.findFunction(qualified);
            if (fnSym && fnSym->accessPattern != AccessPattern::None) {
                result[qualified] = fnSym->accessPattern;
            }
        }
    }

    return result;
}

/// Determine whether a given access pattern should receive prefetch
/// insertion. Topo does not gate on AccessPattern value —
/// the .topo declaration is structural information, not a Topo-side
/// "is this worth prefetching" judgment. All declared patterns receive
/// the prefetch intrinsic; LLVM/HW determine actual benefit.
/// `AccessPattern::None` (no declaration) means user did not opt in —
/// emit nothing.
static bool shouldPrefetch(AccessPattern pattern, FeatureMode /*mode*/) {
    return pattern != AccessPattern::None;
}

/// Find a pointer suitable for preheader prefetch insertion. Walks the first
/// memory instruction's pointer operand upward through GEPs until reaching
/// a value defined outside the loop (loop-invariant, available at preheader)
/// — inserting a prefetch that references a loop-local pointer in the
/// preheader creates a use-before-def and corrupts LICM/MemorySSA later in
/// the standard O2 pipeline.
static llvm::Value* findFirstMemoryPointer(llvm::Loop* loop) {
    auto isLoopInvariant = [&](llvm::Value* v) -> bool {
        if (auto* inst = llvm::dyn_cast<llvm::Instruction>(v)) {
            return !loop->contains(inst->getParent());
        }
        // Constants, arguments, globals are loop-invariant
        return true;
    };

    auto stripToInvariantBase = [&](llvm::Value* ptr) -> llvm::Value* {
        // Walk back through GEPs; stop at a loop-invariant base or give up.
        for (int guard = 0; guard < 16 && ptr; ++guard) {
            if (isLoopInvariant(ptr)) return ptr;
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                ptr = gep->getPointerOperand();
                continue;
            }
            // Unknown loop-local producer: unsafe to reference in preheader.
            return nullptr;
        }
        return nullptr;
    };

    for (llvm::BasicBlock* BB : loop->blocks()) {
        for (llvm::Instruction& I : *BB) {
            llvm::Value* ptr = nullptr;
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&I))
                ptr = load->getPointerOperand();
            else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&I))
                ptr = store->getPointerOperand();
            if (!ptr) continue;
            if (auto* base = stripToInvariantBase(ptr)) return base;
        }
    }
    return nullptr;
}

/// Insert an llvm.prefetch intrinsic in the loop preheader (or header
/// if no preheader exists) that prefetches `distance` bytes ahead of
/// the given pointer.
///
/// llvm.prefetch signature: void @llvm.prefetch(ptr, i32 rw, i32 locality, i32 cache_type)
///   rw: 0 = read, 1 = write
///   locality: 0-3, higher = keep in cache longer (3 = L1)
///   cache_type: 1 = data cache
static bool insertPrefetch(llvm::Loop* loop, llvm::Value* basePtr, int distance, llvm::Module& module) {
    // Prefer preheader; fall back to header
    llvm::BasicBlock* insertBB = loop->getLoopPreheader();
    if (!insertBB) insertBB = loop->getHeader();
    if (!insertBB) return false;

    // Insert before the terminator of the chosen block
    llvm::Instruction* insertPt = insertBB->getTerminator();
    if (!insertPt) return false;

    llvm::IRBuilder<> builder(insertPt);
    auto& ctx = module.getContext();

    // Compute prefetch address: GEP i8 from base pointer by distance bytes
    llvm::Value* prefetchAddr = builder.CreateGEP(llvm::Type::getInt8Ty(ctx),
                                                  basePtr,
                                                  llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), distance),
                                                  "prefetch.addr");

    // Get or declare llvm.prefetch intrinsic
    llvm::Function* prefetchFn = llvm::Intrinsic::getOrInsertDeclaration(
        &module, llvm::Intrinsic::prefetch, {llvm::PointerType::getUnqual(ctx)});

    // Arguments: ptr, rw=0(read), locality=3(L1), cache_type=1(data)
    llvm::Value* args[] = {
        prefetchAddr,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), // read
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 3), // L1 locality
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1), // data cache
    };

    builder.CreateCall(prefetchFn, args);
    return true;
}

/// Build a reverse lookup from llvm::Function* to its Topo qualified name.
static std::unordered_map<llvm::Function*, std::string> buildReverseFunctionMap(const SymbolMapping& mapping) {
    std::unordered_map<llvm::Function*, std::string> result;
    for (const auto& [name, func] : mapping.matched) {
        if (func) result[func] = name;
    }
    return result;
}

/// Insert prefetch intrinsics at pipeline stage boundaries.
///
/// For each pipeline function body, scan call instructions in order.
/// When a call to a stage N function is followed by a call to a
/// stage N+1 function, insert llvm.prefetch for each pointer argument
/// of the stage N+1 call, placed immediately after the stage N call.
static int insertStageBoundaryPrefetch(llvm::Module& module,
                                       const SymbolTable& symbols,
                                       const SymbolMapping& mapping,
                                       const PrefetchConfig& config,
                                       const std::unordered_map<std::string, AccessPattern>& accessMap,
                                       std::unordered_map<std::string, int>* perFnCount) {
    auto reverseMap = buildReverseFunctionMap(mapping);
    int inserted = 0;

    for (const auto& [blockName, block] : symbols.logicBlocks()) {
        if (!block.isPipeline || !block.pipelineAnalysis) continue;

        const auto& pipelineAnalysis = *block.pipelineAnalysis;

        // Find the pipeline function in the mapping
        auto pipelineIt = mapping.matched.find(block.qualifiedName);
        if (pipelineIt == mapping.matched.end() || !pipelineIt->second) continue;

        llvm::Function* pipelineFunc = pipelineIt->second;
        if (pipelineFunc->isDeclaration()) continue;

        // Collect all call instructions in the pipeline body, in order,
        // along with their stage numbers.
        struct StagedCall {
            llvm::CallInst* call;
            int stage;
            std::string topoName;
        };
        std::vector<StagedCall> stagedCalls;

        for (auto& BB : *pipelineFunc) {
            for (auto& I : BB) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
                if (!call) continue;

                auto* callee = call->getCalledFunction();
                if (!callee) continue;

                // Skip intrinsics (including any previously inserted prefetch)
                if (callee->isIntrinsic()) continue;

                // Look up this callee's Topo name
                auto nameIt = reverseMap.find(callee);
                if (nameIt == reverseMap.end()) continue;

                // Look up its stage number in the pipeline analysis
                auto stageIt = pipelineAnalysis.stages.find(nameIt->second);
                if (stageIt == pipelineAnalysis.stages.end()) {
                    // Try resolving via the simple callee name
                    std::string simpleName = nameIt->second;
                    auto sep = simpleName.rfind("::");
                    if (sep != std::string::npos) simpleName = simpleName.substr(sep + 2);

                    // Check if any node name in stages matches this simple name
                    for (const auto& [node, stage] : pipelineAnalysis.stages) {
                        auto nodeSep = node.rfind("::");
                        std::string nodeSimple = (nodeSep != std::string::npos) ? node.substr(nodeSep + 2) : node;
                        if (nodeSimple == simpleName) {
                            stagedCalls.push_back({call, stage, nameIt->second});
                            break;
                        }
                    }
                } else {
                    stagedCalls.push_back({call, stageIt->second, nameIt->second});
                }
            }
        }

        // Insert prefetch at each stage transition: after stage N call,
        // prefetch pointer arguments of the stage N+1 call
        for (size_t i = 0; i + 1 < stagedCalls.size(); ++i) {
            int currentStage = stagedCalls[i].stage;
            int nextStage = stagedCalls[i + 1].stage;

            if (nextStage <= currentStage) continue;

            // Gate on access pattern of the next-stage function.
            // Try accessMap first, then fall back to direct SymbolTable lookup
            // (pipeline blocks may store callees under simple names in the map).
            AccessPattern nextPattern = AccessPattern::None;
            auto patIt = accessMap.find(stagedCalls[i + 1].topoName);
            if (patIt != accessMap.end()) {
                nextPattern = patIt->second;
            } else {
                auto* fnSym = symbols.findFunction(stagedCalls[i + 1].topoName);
                if (fnSym) nextPattern = fnSym->accessPattern;
            }

            if (!shouldPrefetch(nextPattern, config.mode)) continue;

            llvm::CallInst* nextCall = stagedCalls[i + 1].call;

            // Insert prefetch after the current stage's call completes
            llvm::IRBuilder<> builder(nextCall);
            auto& ctx = module.getContext();

            llvm::Function* prefetchFn = llvm::Intrinsic::getOrInsertDeclaration(
                &module, llvm::Intrinsic::prefetch, {llvm::PointerType::getUnqual(ctx)});

            // Prefetch each pointer argument of the next-stage call
            for (unsigned argIdx = 0; argIdx < nextCall->arg_size(); ++argIdx) {
                llvm::Value* arg = nextCall->getArgOperand(argIdx);
                if (!arg->getType()->isPointerTy()) continue;

                // Compute prefetch address: base + distance
                llvm::Value* prefetchAddr =
                    builder.CreateGEP(llvm::Type::getInt8Ty(ctx),
                                      arg,
                                      llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), config.distance),
                                      "stage.prefetch.addr");

                llvm::Value* args[] = {
                    prefetchAddr,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 3),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1),
                };
                builder.CreateCall(prefetchFn, args);
                ++inserted;
                if (perFnCount) (*perFnCount)[blockName] += 1;
            }
        }
    }

    return inserted;
}

int PrefetchPass::run(llvm::Module& module,
                      const SymbolTable& symbols,
                      const SymbolMapping& mapping,
                      const PrefetchConfig& config,
                      backend::PrefetchReport* report) {
    if (!config.isEnabled()) return 0;

    auto accessMap = buildAccessPatternMap(symbols);
    int inserted = 0;
    std::unordered_map<std::string, int> perFnCount;

    // 1. Loop-based prefetch: insert at streaming loop entries
    for (const auto& [topoName, llvmFunc] : mapping.matched) {
        if (!llvmFunc || llvmFunc->isDeclaration()) continue;

        // Determine access pattern for this function
        AccessPattern pattern = AccessPattern::None;
        auto it = accessMap.find(topoName);
        if (it != accessMap.end()) {
            pattern = it->second;
        }

        // Check whether this function's access pattern warrants prefetch
        if (!shouldPrefetch(pattern, config.mode)) continue;

        // Build loop info
        llvm::Function* func = llvmFunc;
        llvm::DominatorTree DT(*func);
        llvm::LoopInfo LI(DT);

        if (LI.empty()) continue;

        for (llvm::Loop* loop : LI) {
            llvm::Value* ptr = findFirstMemoryPointer(loop);
            if (!ptr) continue;

            if (insertPrefetch(loop, ptr, config.distance, module)) {
                ++inserted;
                perFnCount[topoName] += 1;
            }
        }
    }

    // 2. Stage boundary prefetch: insert between pipeline stage transitions
    inserted += insertStageBoundaryPrefetch(module, symbols, mapping, config, accessMap, &perFnCount);

    if (report) {
        for (const auto& [name, count] : perFnCount) {
            backend::PrefetchEntry e;
            e.hostFunction = name;
            e.insertedHints = count;
            e.distance = config.distance;
            report->entries.push_back(std::move(e));
        }
    }

    return inserted;
}

} // namespace topo
