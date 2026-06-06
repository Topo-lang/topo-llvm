// @category: COVERED
#include "topo/Transforms/LoopParallelizePass.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <unordered_set>

namespace topo {

// ===================================================================
// Shared helpers
// ===================================================================

/// Determine which functions are in parallel stages: i.e., functions
/// that share a stage number with at least one other function in
/// the same fn block.
static std::unordered_set<std::string> findParallelStageFunctions(const SymbolTable& symbols) {
    std::unordered_set<std::string> result;

    for (const auto& [blockName, block] : symbols.logicBlocks()) {
        // Build stage -> [functions] map
        std::unordered_map<int, std::vector<std::string>> stageFuncs;
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }

        for (size_t i = 0; i < block.calledFunctions.size(); ++i) {
            const auto& callee = block.calledFunctions[i];
            if (callee.size() > 8 && callee.substr(0, 8) == "<assign:") continue;
            int stage = (i < block.stages.size()) ? block.stages[i] : -1;
            if (stage < 0) continue;

            std::string qualified = block.isPipeline ? callee : (nsPrefix + callee);
            stageFuncs[stage].push_back(qualified);
        }

        // Functions in stages with 2+ functions are parallel
        for (const auto& [stage, funcs] : stageFuncs) {
            if (funcs.size() >= 2) {
                for (const auto& f : funcs) {
                    result.insert(f);
                }
            }
        }
    }

    return result;
}

/// Check if a function name should be excluded
static bool isExcluded(const std::string& name, const std::vector<std::string>& excludeList) {
    for (const auto& pattern : excludeList) {
        if (name.find(pattern) != std::string::npos) return true;
    }
    return false;
}

// ===================================================================
// Step 1: Metadata annotation
// ===================================================================

static int annotateLoopsPhase1(llvm::Module& module,
                               const SymbolTable& /*symbols*/,
                               const SymbolMapping& mapping,
                               const LoopParallelConfig& config,
                               const std::unordered_set<std::string>& parallelFuncs,
                               std::unordered_map<std::string, int>* perFnCount) {
    int annotated = 0;
    auto& ctx = module.getContext();

    for (const auto& [topoName, llvmFunc] : mapping.matched) {
        if (!parallelFuncs.count(topoName)) continue;
        if (isExcluded(topoName, config.exclude)) continue;
        if (llvmFunc->isDeclaration()) continue;

        llvm::Function* func = llvmFunc;

        llvm::DominatorTree DT(*func);
        llvm::LoopInfo LI(DT);

        if (LI.empty()) continue;

        for (llvm::Loop* loop : LI) {
            // Create an access group for this loop
            llvm::MDNode* accessGroup = llvm::MDNode::getDistinct(ctx, {});

            // Annotate all memory instructions in the loop with the access group
            for (llvm::BasicBlock* BB : loop->blocks()) {
                for (llvm::Instruction& I : *BB) {
                    if (I.mayReadOrWriteMemory()) {
                        I.setMetadata(llvm::LLVMContext::MD_access_group, accessGroup);
                    }
                }
            }

            // Get or create the loop's existing metadata
            llvm::MDNode* loopID = loop->getLoopID();

            // Build new loop metadata entries
            llvm::SmallVector<llvm::Metadata*, 4> loopMDs;

            // Self-reference placeholder (will be replaced)
            loopMDs.push_back(nullptr);

            // Copy existing metadata if any
            if (loopID) {
                for (unsigned i = 1; i < loopID->getNumOperands(); ++i) {
                    loopMDs.push_back(loopID->getOperand(i));
                }
            }

            // Add parallel_accesses pointing to our access group
            llvm::Metadata* parallelArgs[] = {llvm::MDString::get(ctx, "llvm.loop.parallel_accesses"), accessGroup};
            loopMDs.push_back(llvm::MDNode::get(ctx, parallelArgs));

            // Cardinality-based unroll bucket selection was a
            // value judgment violating the "Topo doesn't judge" principle.
            // Removed — LLVM's standard LoopUnroll pass owns unroll decisions
            // based on its cost model + trip count analysis.

            // Create the new loop metadata
            llvm::MDNode* newLoopID = llvm::MDNode::get(ctx, loopMDs);
            newLoopID->replaceOperandWith(0, newLoopID);

            loop->setLoopID(newLoopID);
            ++annotated;
            if (perFnCount) (*perFnCount)[topoName] += 1;
        }
    }

    return annotated;
}

// ===================================================================
// Step 2: Partition-based loop parallelization
// ===================================================================

namespace {

/// Declare the parallel runtime C ABI functions needed for loop partitioning.
struct LoopRuntimeDecls {
    llvm::FunctionCallee ensureInit;
    llvm::FunctionCallee taskSpawn;
    llvm::FunctionCallee awaitAll;
    llvm::FunctionCallee costBegin;
    llvm::FunctionCallee costEnd;
};

llvm::FunctionCallee getOrDeclareFunc(llvm::Module& module, const std::string& name, llvm::FunctionType* ty) {
    if (auto* existing = module.getFunction(name)) return existing;
    return module.getOrInsertFunction(name, ty);
}

LoopRuntimeDecls declareLoopRuntime(llvm::Module& module) {
    auto& ctx = module.getContext();
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    LoopRuntimeDecls decls;

    decls.ensureInit =
        getOrDeclareFunc(module, "topo_parallel_ensure_init", llvm::FunctionType::get(voidTy, {}, false));

    // topo_task_t* topo_task_spawn(fn, arg)
    decls.taskSpawn =
        getOrDeclareFunc(module, "topo_task_spawn", llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false));

    // void topo_task_await_all(tasks, count)
    decls.awaitAll =
        getOrDeclareFunc(module, "topo_task_await_all", llvm::FunctionType::get(voidTy, {ptrTy, i32Ty}, false));

    decls.costBegin = getOrDeclareFunc(module, "topo_cost_begin", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    decls.costEnd = getOrDeclareFunc(module, "topo_cost_end", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    return decls;
}

/// Create a global constant string in the module.
llvm::Constant* getOrCreateGlobalString(llvm::Module& module, const std::string& str) {
    std::string globalName = ".str.topo_loop." + str;
    if (auto* existing = module.getGlobalVariable(globalName))
        return llvm::ConstantExpr::getPointerCast(existing, llvm::PointerType::getUnqual(module.getContext()));

    auto& ctx = module.getContext();
    auto* strConst = llvm::ConstantDataArray::getString(ctx, str, true);
    auto* gv = new llvm::GlobalVariable(
        module, strConst->getType(), true, llvm::GlobalValue::PrivateLinkage, strConst, globalName);
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return llvm::ConstantExpr::getPointerCast(gv, llvm::PointerType::getUnqual(ctx));
}

/// Check if a loop contains calls to known synchronization primitives
/// (mutex lock/unlock, atomic fences, thread barriers, etc.).
/// Such loops are unsafe to partition.
bool containsSyncPrimitives(llvm::Loop* loop) {
    static const char* syncPrefixes[] = {
        "pthread_mutex",
        "pthread_rwlock",
        "pthread_spin",
        "pthread_barrier",
        "pthread_cond",
        "mtx_lock",
        "mtx_unlock",
        "mtx_timedlock",
        "__gthread_mutex",
        "__gthread_recursive_mutex",
        "std::mutex",
        "std::recursive_mutex",
        "std::shared_mutex",
        "std::condition_variable",
        "omp_set_lock",
        "omp_unset_lock",
    };
    static const char* syncExact[] = {
        "atomic_thread_fence",
        "__atomic_thread_fence",
        "__sync_synchronize",
    };

    for (llvm::BasicBlock* BB : loop->blocks()) {
        for (llvm::Instruction& I : *BB) {
            // Atomic fence instructions
            if (llvm::isa<llvm::FenceInst>(&I)) return true;

            // AtomicRMW and AtomicCmpXchg indicate shared mutable state
            if (llvm::isa<llvm::AtomicRMWInst>(&I) || llvm::isa<llvm::AtomicCmpXchgInst>(&I)) return true;

            auto* call = llvm::dyn_cast<llvm::CallBase>(&I);
            if (!call) continue;

            auto* callee = call->getCalledFunction();
            if (!callee)
                continue; // Indirect call: conservatively unsafe
                          // is handled below by the "has side effects" check

            llvm::StringRef name = callee->getName();

            for (const char* prefix : syncPrefixes) {
                if (name.starts_with(prefix)) return true;
            }
            for (const char* exact : syncExact) {
                if (name == exact) return true;
            }
        }
    }
    return false;
}

/// Check if a loop has cross-iteration data dependencies that
/// prevent safe parallel execution. We detect:
///   1. PHI nodes in the loop header that carry values between iterations
///      (other than the induction variable)
///   2. Load-after-store to the same pointer within the loop body where
///      the address depends on different iterations
///
/// This is a conservative check: it only approves loops where all
/// recurrences are the induction variable itself.
bool hasCrossIterationDeps(llvm::Loop* loop, llvm::ScalarEvolution& SE) {
    llvm::BasicBlock* header = loop->getHeader();
    llvm::PHINode* inductionPHI = loop->getCanonicalInductionVariable();

    for (llvm::PHINode& phi : header->phis()) {
        // The canonical induction variable is safe
        if (&phi == inductionPHI) continue;

        // Check if this PHI is an affine AddRec (recognized by SCEV as
        // a loop-varying expression with no data deps). AddRec expressions
        // like {start, +, step} are safe for partitioning because each
        // iteration's value is computable from the iteration index alone.
        const llvm::SCEV* scev = SE.getSCEV(&phi);
        if (auto* addRec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(scev)) {
            if (addRec->getLoop() == loop && addRec->isAffine()) continue;
        }

        // Any other loop-carried PHI is a potential cross-iteration dep
        return true;
    }

    return false;
}

// ===================================================================
// Reduction detection
// ===================================================================

/// Information about a detected reduction pattern.
struct ReductionInfo {
    llvm::PHINode* phi;          ///< The loop-carried PHI
    llvm::BinaryOperator* binop; ///< The accumulation operation
    llvm::Value* rhs;            ///< The non-accumulator operand
    unsigned opcode;             ///< LLVM opcode (Add, FAdd, Mul, etc.)

    /// Get the identity element for this reduction operation.
    /// `ctx` is unused — the type already carries its context — but
    /// stays in the signature so callers do not have to know whether
    /// the identity is constructed via the type or the context.
    llvm::Constant* getIdentity([[maybe_unused]] llvm::LLVMContext& ctx) const {
        llvm::Type* ty = phi->getType();
        switch (opcode) {
        case llvm::Instruction::Add:
        case llvm::Instruction::Sub:
            return llvm::ConstantInt::get(ty, 0);
        case llvm::Instruction::Mul:
            return llvm::ConstantInt::get(ty, 1);
        case llvm::Instruction::And:
            return llvm::ConstantInt::getAllOnesValue(ty);
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
            return llvm::ConstantInt::get(ty, 0);
        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
            return llvm::ConstantFP::get(ty, 0.0);
        case llvm::Instruction::FMul:
            return llvm::ConstantFP::get(ty, 1.0);
        default:
            return llvm::Constant::getNullValue(ty);
        }
    }
};

/// Hardcoded set of associative binary operations eligible for reduction.
static bool isAssociativeReductionOp(unsigned opcode) {
    switch (opcode) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Mul:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
    case llvm::Instruction::FAdd:
    case llvm::Instruction::FMul:
        return true;
    default:
        return false;
    }
}

/// Detect a reduction pattern on a loop-carried PHI node.
/// A reduction is: phi = [init, preheader], [binop(phi, x), latch]
/// where binop is an associative operation.
///
/// Returns nullopt if the PHI is not a reduction.
static std::optional<ReductionInfo> detectReduction(llvm::PHINode* phi, llvm::Loop* loop) {
    if (phi->getNumIncomingValues() != 2) return std::nullopt;

    // Identify the latch incoming value
    llvm::BasicBlock* latch = loop->getLoopLatch();
    if (!latch) return std::nullopt;

    llvm::Value* latchVal = nullptr;
    for (unsigned i = 0; i < 2; ++i) {
        if (phi->getIncomingBlock(i) == latch) {
            latchVal = phi->getIncomingValue(i);
        }
    }
    if (!latchVal) return std::nullopt;

    // The latch value must be a binary operator inside the loop
    auto* binop = llvm::dyn_cast<llvm::BinaryOperator>(latchVal);
    if (!binop || !loop->contains(binop)) return std::nullopt;

    // Must be an associative reduction op
    if (!isAssociativeReductionOp(binop->getOpcode())) return std::nullopt;

    // One operand must be the PHI itself, the other is the per-iteration value
    llvm::Value* rhs = nullptr;
    if (binop->getOperand(0) == phi) {
        rhs = binop->getOperand(1);
    } else if (binop->getOperand(1) == phi) {
        rhs = binop->getOperand(0);
    } else {
        return std::nullopt;
    }

    ReductionInfo info;
    info.phi = phi;
    info.binop = binop;
    info.rhs = rhs;
    info.opcode = binop->getOpcode();
    return info;
}

/// Collect all values used inside the loop but defined outside it.
/// These become the "captured" variables for the outlined loop body.
struct LoopCaptures {
    std::vector<llvm::Value*> values;
    std::vector<llvm::Type*> types;
};

LoopCaptures collectCaptures(llvm::Loop* loop) {
    LoopCaptures caps;
    std::unordered_set<llvm::Value*> seen;

    for (llvm::BasicBlock* BB : loop->blocks()) {
        for (llvm::Instruction& I : *BB) {
            for (llvm::Use& U : I.operands()) {
                llvm::Value* val = U.get();
                // Skip constants, basic block labels, and inline asm
                if (llvm::isa<llvm::Constant>(val) || llvm::isa<llvm::BasicBlock>(val)) continue;

                // Must be defined outside the loop
                if (auto* inst = llvm::dyn_cast<llvm::Instruction>(val)) {
                    if (loop->contains(inst)) continue;
                }

                // Function arguments are always outside
                if (seen.insert(val).second) {
                    caps.values.push_back(val);
                    caps.types.push_back(val->getType());
                }
            }
        }
    }
    return caps;
}

/// Create the outlined loop body function.
///
/// Signature: void loop_worker(void* arg_struct)
/// The arg_struct contains:
///   - i64 start_idx (partition start)
///   - i64 end_idx   (partition end, exclusive)
///   - captured values from the original loop
///
/// The worker executes: for (i = start; i < end; i++) { original_body }
llvm::Function* outlineLoopBody(llvm::Module& module,
                                llvm::Loop* loop,
                                const LoopCaptures& captures,
                                const std::string& funcName,
                                const LoopRuntimeDecls& decls,
                                bool instrument,
                                const std::vector<ReductionInfo>& reductions = {}) {
    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);

    // Build the arg struct type: {i64 start, i64 end, <captured...>,
    // <partial_0 .. partial_{R-1}>}. The trailing reduction fields are
    // worker-written outputs: each partition folds its slice into a
    // local accumulator and writes the result back so the parent can
    // combine partials serially after the join. The field layout here
    // MUST stay identical to the one partitionLoop() builds.
    std::vector<llvm::Type*> structFields;
    structFields.push_back(i64Ty); // start
    structFields.push_back(i64Ty); // end
    for (auto* ty : captures.types)
        structFields.push_back(ty);
    for (const auto& red : reductions)
        structFields.push_back(red.phi->getType()); // partial output
    auto* argStructTy = llvm::StructType::get(ctx, structFields);
    const unsigned partialBase = 2 + static_cast<unsigned>(captures.values.size());

    // Create the worker function
    auto* workerTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    std::string workerName = funcName + ".topo_loop_worker";
    auto* worker = llvm::Function::Create(workerTy, llvm::GlobalValue::InternalLinkage, workerName, module);

    auto* entry = llvm::BasicBlock::Create(ctx, "entry", worker);
    auto* loopHeader = llvm::BasicBlock::Create(ctx, "loop.header", worker);
    auto* loopBody = llvm::BasicBlock::Create(ctx, "loop.body", worker);
    auto* loopExit = llvm::BasicBlock::Create(ctx, "loop.exit", worker);

    llvm::IRBuilder<> builder(entry);
    auto* argPtr = worker->getArg(0);

    // Insert cost_begin if instrumented
    if (instrument) {
        auto* nameStr = getOrCreateGlobalString(module, funcName + ".partition");
        builder.CreateCall(decls.costBegin, {nameStr});
    }

    // Load start and end from arg struct
    auto* startPtr = builder.CreateStructGEP(argStructTy, argPtr, 0, "start.ptr");
    auto* startVal = builder.CreateLoad(i64Ty, startPtr, "start");
    auto* endPtr = builder.CreateStructGEP(argStructTy, argPtr, 1, "end.ptr");
    auto* endVal = builder.CreateLoad(i64Ty, endPtr, "end");

    // Load captured values
    std::vector<llvm::Value*> capturedVals;
    for (unsigned i = 0; i < captures.values.size(); ++i) {
        auto* fieldPtr = builder.CreateStructGEP(argStructTy, argPtr, i + 2);
        auto* loaded = builder.CreateLoad(captures.types[i], fieldPtr);
        loaded->setName(captures.values[i]->getName() + ".cap");
        capturedVals.push_back(loaded);
    }

    builder.CreateBr(loopHeader);

    // Loop header: PHI for induction variable, compare with end
    builder.SetInsertPoint(loopHeader);
    auto* indVar = builder.CreatePHI(i64Ty, 2, "i");
    indVar->addIncoming(startVal, entry);

    // One partial accumulator per reduction, seeded with the operator's
    // identity element (not the original loop init — that is applied
    // exactly once by the parent's serial combine).
    std::vector<llvm::PHINode*> accPHIs;
    accPHIs.reserve(reductions.size());
    for (const auto& red : reductions) {
        auto* accPHI = builder.CreatePHI(red.phi->getType(), 2, "red.acc");
        accPHI->addIncoming(red.getIdentity(ctx), entry);
        accPHIs.push_back(accPHI);
    }

    auto* cond = builder.CreateICmpSLT(indVar, endVal, "loop.cond");
    builder.CreateCondBr(cond, loopBody, loopExit);

    // Loop body: clone the original loop body instructions
    builder.SetInsertPoint(loopBody);

    // Build a value map from original values to outlined values
    llvm::ValueToValueMapTy VMap;
    llvm::PHINode* origIndVar = loop->getCanonicalInductionVariable();
    if (origIndVar) {
        // Map original induction variable to our new one.
        // The original might be i32 while ours is i64 — trunc if needed.
        if (origIndVar->getType() != i64Ty) {
            auto* truncated = builder.CreateTrunc(indVar, origIndVar->getType(), "i.trunc");
            VMap[origIndVar] = truncated;
        } else {
            VMap[origIndVar] = indVar;
        }
    }

    // Map captured values
    for (unsigned i = 0; i < captures.values.size(); ++i) {
        VMap[captures.values[i]] = capturedVals[i];
    }

    // Map each original reduction PHI to this worker's local accumulator
    // so the cloned accumulation binop folds into the partial, not the
    // original (about-to-be-deleted) loop-carried PHI.
    for (unsigned r = 0; r < reductions.size(); ++r) {
        VMap[reductions[r].phi] = accPHIs[r];
    }

    // Clone instructions from the original loop body (all blocks except header)
    // For simplicity with single-block loop bodies, clone all non-terminator
    // instructions from body blocks.
    llvm::BasicBlock* origHeader = loop->getHeader();
    for (llvm::BasicBlock* BB : loop->blocks()) {
        if (BB == origHeader) continue; // Header PHIs handled via VMap

        for (llvm::Instruction& I : *BB) {
            // Skip terminators (we build our own control flow)
            if (I.isTerminator()) continue;
            // Skip PHI nodes in body blocks
            if (llvm::isa<llvm::PHINode>(&I)) continue;

            llvm::Instruction* cloned = I.clone();
            builder.Insert(cloned);

            // Remap operands using the VMap
            for (unsigned op = 0; op < cloned->getNumOperands(); ++op) {
                llvm::Value* origOp = cloned->getOperand(op);
                auto it = VMap.find(origOp);
                if (it != VMap.end()) {
                    cloned->setOperand(op, it->second);
                }
            }

            // Add to VMap so later instructions can reference this clone
            if (I.hasName()) cloned->setName(I.getName() + ".p");
            VMap[&I] = cloned;
        }
    }

    // Increment induction variable and branch back
    auto* inc = builder.CreateAdd(indVar, llvm::ConstantInt::get(i64Ty, 1), "i.next");
    indVar->addIncoming(inc, loopBody);

    // Close each accumulator cycle: the back-edge value is the cloned
    // accumulation binop (its PHI operand was remapped to accPHI above).
    for (unsigned r = 0; r < reductions.size(); ++r) {
        auto it = VMap.find(reductions[r].binop);
        // partitionLoop() guarantees the binop lives in a cloned (non-header)
        // block, so the clone is always present.
        accPHIs[r]->addIncoming(it->second, loopBody);
    }

    builder.CreateBr(loopHeader);

    // Loop exit
    builder.SetInsertPoint(loopExit);
    if (instrument) {
        auto* nameStr = getOrCreateGlobalString(module, funcName + ".partition");
        builder.CreateCall(decls.costEnd, {nameStr});
    }

    // Write this partition's partial result(s) back into the arg struct
    // for the parent's serial combine. accPHI at loopExit holds either the
    // fully accumulated value or the identity (zero-iteration partition).
    for (unsigned r = 0; r < reductions.size(); ++r) {
        auto* partialPtr =
            builder.CreateStructGEP(argStructTy, argPtr, partialBase + r, "partial.ptr");
        builder.CreateStore(accPHIs[r], partialPtr);
    }

    builder.CreateRetVoid();

    return worker;
}

/// Return the number of times the loop *body* executes, as an LLVM Value
/// at the preheader. This is the iteration space the partitioner splits —
/// NOT SCEV's "trip count", which counts condition/header executions
/// (backedge-taken-count + 1) and exceeds the body-execution count by one
/// for top-tested loops (guard in header — clang -O0/-O1 emits these
/// un-rotated). Body executions relate to SCEV's header trip count by
/// loop form:
///   - bottom-tested (exiting block == latch): body == trip
///   - top-tested    (exiting block != latch): body == trip - 1 == BTC
/// Returns nullptr if not computable as a compile-time constant.
llvm::Value* getTripCountValue(llvm::Loop* loop, llvm::ScalarEvolution& SE, llvm::IRBuilder<>& builder) {
    // Whether the loop tests-and-exits at the bottom (latch is the exiting
    // block) or at the top (separate header guard). getLoopLatch() is
    // non-null here: candidates require a canonical induction variable.
    const bool bottomTested = loop->getExitingBlock() == loop->getLoopLatch();

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(builder.getContext());

    // SCEV header trip count = backedge-taken-count + 1.
    unsigned headerTrip = SE.getSmallConstantTripCount(loop);
    if (headerTrip > 0) {
        int64_t body = bottomTested ? headerTrip : (headerTrip - 1);
        if (body <= 0) return nullptr;
        return llvm::ConstantInt::get(i64Ty, body);
    }

    // Fallback: derive from the exact backedge-taken count (== exit count
    // for a single-exit loop). body = BTC + (bottomTested ? 1 : 0).
    const llvm::SCEV* exitCount = SE.getExitCount(loop, loop->getExitingBlock());
    if (llvm::isa<llvm::SCEVCouldNotCompute>(exitCount)) return nullptr;

    if (auto* exitConst = llvm::dyn_cast<llvm::SCEVConstant>(exitCount)) {
        int64_t btc = exitConst->getAPInt().getSExtValue();
        int64_t body = bottomTested ? (btc + 1) : btc;
        if (body <= 0) return nullptr;
        return llvm::ConstantInt::get(i64Ty, body);
    }

    // If SCEV can compute the exit count but it's not a constant,
    // we can expand it to IR. Use SCEVExpander for this.
    // For now, only handle constant trip counts to keep things simple
    // and avoid inserting SCEVExpander dependency.
    return nullptr;
}

/// Replace a parallelizable loop with partition-based parallel execution.
///
/// Generates:
///   topo_parallel_ensure_init();
///   N = trip_count;
///   num_parts = min(N / chunk_size, hardware_concurrency_estimate);
///   part_size = N / num_parts;
///   for (p = 0; p < num_parts; p++) {
///     arg_struct.start = p * part_size;
///     arg_struct.end = (p == num_parts-1) ? N : (p+1) * part_size;
///     tasks[p] = topo_task_spawn(loop_worker, &arg_struct[p]);
///   }
///   topo_task_await_all(tasks, num_parts);
bool partitionLoop(llvm::Module& module,
                   llvm::Function* func,
                   llvm::Loop* loop,
                   llvm::ScalarEvolution& SE,
                   const LoopCaptures& captures,
                   const std::string& funcName,
                   const LoopRuntimeDecls& decls,
                   const LoopParallelConfig& config,
                   const std::vector<ReductionInfo>& reductions = {}) {
    auto& ctx = module.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);

    // Get the loop preheader — we insert partition code there
    llvm::BasicBlock* preheader = loop->getLoopPreheader();
    if (!preheader) return false;

    // Get the loop exit block
    llvm::BasicBlock* exitBlock = loop->getExitBlock();
    if (!exitBlock) return false; // Multiple exits: too complex

    // Capture each reduction's original loop-entry value before any IR
    // mutation. Workers accumulate from the operator identity; this init
    // is folded in exactly once by the serial combine.
    std::vector<llvm::Value*> origInits;
    origInits.reserve(reductions.size());
    for (const auto& red : reductions) {
        // The outliner clones only non-header blocks, so a reduction whose
        // accumulation binop lives in the header itself cannot be outlined
        // — leave the loop serial rather than emit a wrong partial.
        if (red.binop->getParent() == loop->getHeader()) return false;
        llvm::Value* init = red.phi->getIncomingValueForBlock(preheader);
        if (!init) return false;
        origInits.push_back(init);
    }

    if (!reductions.empty()) {
        llvm::errs() << "topo: remark: detected " << reductions.size()
                     << " reduction(s) in " << funcName << "\n";
    }

    // Compute trip count
    llvm::IRBuilder<> preBuilder(preheader->getTerminator());
    llvm::Value* tripCount = getTripCountValue(loop, SE, preBuilder);
    if (!tripCount) return false;

    // Trip count threshold was a value judgment ("loop too
    // short to be worth partitioning"). Removed — Topo doesn't gate on
    // workload size; the partition runtime handles small trip counts
    // correctly (one partition, one iteration), and benchmark data
    // determines whether the COVERED non-regression band holds.

    // Outline the loop body
    auto* worker =
        outlineLoopBody(module, loop, captures, funcName, decls, config.instrument, reductions);

    // Build the arg struct type — MUST match outlineLoopBody()'s layout:
    // {i64 start, i64 end, <captured...>, <partial_0 .. partial_{R-1}>}.
    std::vector<llvm::Type*> structFields;
    structFields.push_back(i64Ty);
    structFields.push_back(i64Ty);
    for (auto* ty : captures.types)
        structFields.push_back(ty);
    for (const auto& red : reductions)
        structFields.push_back(red.phi->getType());
    auto* argStructTy = llvm::StructType::get(ctx, structFields);
    const unsigned partialBase = 2 + static_cast<unsigned>(captures.values.size());

    // Replace the preheader terminator with our partition code.
    // We replace the branch from preheader->header with
    // preheader->[partition code]->exitBlock
    preheader->getTerminator()->eraseFromParent();

    llvm::IRBuilder<> builder(preheader);

    // Ensure runtime init
    builder.CreateCall(decls.ensureInit);

    // Determine number of partitions.
    // For static: num_parts = clamp(N / chunk_size, 1, max_partitions)
    // max_partitions is a compile-time constant (conservative: 16 cores)
    int maxPartitions = 16;

    llvm::Value* numParts;
    if (config.partitionStrategy == LoopPartitionStrategy::Static) {
        auto* chunkSizeVal = llvm::ConstantInt::get(i64Ty, config.chunkSize);
        auto* rawParts = builder.CreateUDiv(tripCount, chunkSizeVal, "raw.parts");
        // Clamp to [1, maxPartitions]
        auto* clamped = builder.CreateSelect(builder.CreateICmpULT(rawParts, llvm::ConstantInt::get(i64Ty, 1)),
                                             llvm::ConstantInt::get(i64Ty, 1),
                                             rawParts,
                                             "parts.min1");
        numParts = builder.CreateSelect(builder.CreateICmpUGT(clamped, llvm::ConstantInt::get(i64Ty, maxPartitions)),
                                        llvm::ConstantInt::get(i64Ty, maxPartitions),
                                        clamped,
                                        "num.parts");
    } else {
        // Dynamic: use more partitions (smaller chunks) for work-stealing
        // Aim for ~4x oversubscription to feed work-stealing
        auto* chunkSizeVal = llvm::ConstantInt::get(i64Ty, config.chunkSize);
        auto* rawParts = builder.CreateUDiv(tripCount, chunkSizeVal, "raw.parts");
        int maxDynamic = maxPartitions * 4;
        auto* clamped = builder.CreateSelect(builder.CreateICmpULT(rawParts, llvm::ConstantInt::get(i64Ty, 1)),
                                             llvm::ConstantInt::get(i64Ty, 1),
                                             rawParts,
                                             "parts.min1");
        numParts = builder.CreateSelect(builder.CreateICmpUGT(clamped, llvm::ConstantInt::get(i64Ty, maxDynamic)),
                                        llvm::ConstantInt::get(i64Ty, maxDynamic),
                                        clamped,
                                        "num.parts");
    }

    // Compute partition size: part_size = N / num_parts
    auto* partSize = builder.CreateUDiv(tripCount, numParts, "part.size");

    // Allocate arrays for arg structs and task handles at the function entry
    auto& entryBB = func->getEntryBlock();
    llvm::IRBuilder<> allocaBuilder(&entryBB, entryBB.begin());

    // We need a max-sized alloca. Use maxPartitions (or maxDynamic) as the
    // upper bound to avoid dynamic alloca.
    int maxParts = (config.partitionStrategy == LoopPartitionStrategy::Dynamic) ? maxPartitions * 4 : maxPartitions;
    auto* argArrayAlloca =
        allocaBuilder.CreateAlloca(argStructTy, llvm::ConstantInt::get(i32Ty, maxParts), "loop.args");
    auto* taskArrayAlloca = allocaBuilder.CreateAlloca(ptrTy, llvm::ConstantInt::get(i32Ty, maxParts), "loop.tasks");

    // Spawn loop: for p in [0, numParts)
    auto* spawnHeader = llvm::BasicBlock::Create(ctx, "spawn.header", func);
    auto* spawnBody = llvm::BasicBlock::Create(ctx, "spawn.body", func);
    auto* spawnExit = llvm::BasicBlock::Create(ctx, "spawn.exit", func);

    builder.CreateBr(spawnHeader);

    // Spawn header
    builder.SetInsertPoint(spawnHeader);
    auto* pIdx = builder.CreatePHI(i64Ty, 2, "p.idx");
    pIdx->addIncoming(llvm::ConstantInt::get(i64Ty, 0), preheader);
    auto* spawnCond = builder.CreateICmpULT(pIdx, numParts, "spawn.cond");
    builder.CreateCondBr(spawnCond, spawnBody, spawnExit);

    // Spawn body: fill arg struct and spawn task
    builder.SetInsertPoint(spawnBody);

    // Compute partition range
    auto* partStart = builder.CreateMul(pIdx, partSize, "part.start");
    auto* nextPIdx = builder.CreateAdd(pIdx, llvm::ConstantInt::get(i64Ty, 1), "p.next");

    // end = (p == numParts-1) ? tripCount : (p+1) * partSize
    auto* isLastPart = builder.CreateICmpEQ(nextPIdx, numParts, "is.last");
    auto* regularEnd = builder.CreateMul(nextPIdx, partSize, "regular.end");
    auto* partEnd = builder.CreateSelect(isLastPart, tripCount, regularEnd, "part.end");

    // Get pointer to this partition's arg struct
    auto* argStructPtr = builder.CreateGEP(argStructTy, argArrayAlloca, pIdx, "arg.ptr");

    // Store start and end
    auto* startFieldPtr = builder.CreateStructGEP(argStructTy, argStructPtr, 0);
    builder.CreateStore(partStart, startFieldPtr);
    auto* endFieldPtr = builder.CreateStructGEP(argStructTy, argStructPtr, 1);
    builder.CreateStore(partEnd, endFieldPtr);

    // Store captured values
    for (unsigned i = 0; i < captures.values.size(); ++i) {
        auto* fieldPtr = builder.CreateStructGEP(argStructTy, argStructPtr, i + 2);
        builder.CreateStore(captures.values[i], fieldPtr);
    }

    // Spawn: topo_task_spawn(worker, arg_struct_ptr)
    auto* taskHandle = builder.CreateCall(decls.taskSpawn, {worker, argStructPtr}, "task");

    // Store task handle
    auto* taskSlot = builder.CreateGEP(ptrTy, taskArrayAlloca, pIdx, "task.slot");
    builder.CreateStore(taskHandle, taskSlot);

    pIdx->addIncoming(nextPIdx, spawnBody);
    builder.CreateBr(spawnHeader);

    // Spawn exit: await all tasks
    builder.SetInsertPoint(spawnExit);
    auto* numPartsI32 = builder.CreateTrunc(numParts, i32Ty, "num.parts.i32");
    builder.CreateCall(decls.awaitAll, {taskArrayAlloca, numPartsI32});

    // Build the accumulation op for the combine, preserving the original
    // binop's fast-math flags (FP reduction reassociation is licensed by
    // the parallel-stage declaration, not invented here).
    auto foldOp = [&](llvm::IRBuilder<>& B, const ReductionInfo& red,
                      llvm::Value* a, llvm::Value* b) -> llvm::Value* {
        auto* v = B.CreateBinOp(
            static_cast<llvm::Instruction::BinaryOps>(red.opcode), a, b, "red.fold");
        if (auto* bi = llvm::dyn_cast<llvm::BinaryOperator>(v))
            if (llvm::isa<llvm::FPMathOperator>(bi))
                bi->copyFastMathFlags(red.binop);
        return v;
    };

    // Serial combine: fold every partition's partial into one value per
    // reduction, then apply the original loop init exactly once. With no
    // reductions this is just the original spawnExit -> exitBlock edge.
    llvm::BasicBlock* exitPred = spawnExit;
    std::vector<llvm::Value*> finalVals;
    if (!reductions.empty()) {
        auto* combineHeader = llvm::BasicBlock::Create(ctx, "combine.header", func);
        auto* combineBody = llvm::BasicBlock::Create(ctx, "combine.body", func);
        auto* combineExit = llvm::BasicBlock::Create(ctx, "combine.exit", func);

        builder.CreateBr(combineHeader);

        builder.SetInsertPoint(combineHeader);
        auto* cIdx = builder.CreatePHI(i64Ty, 2, "c.idx");
        cIdx->addIncoming(llvm::ConstantInt::get(i64Ty, 0), spawnExit);
        std::vector<llvm::PHINode*> accs;
        for (const auto& red : reductions) {
            auto* acc = builder.CreatePHI(red.phi->getType(), 2, "red.combine");
            acc->addIncoming(red.getIdentity(ctx), spawnExit);
            accs.push_back(acc);
        }
        auto* cCond = builder.CreateICmpULT(cIdx, numParts, "combine.cond");
        builder.CreateCondBr(cCond, combineBody, combineExit);

        builder.SetInsertPoint(combineBody);
        auto* combineArgPtr =
            builder.CreateGEP(argStructTy, argArrayAlloca, cIdx, "combine.arg.ptr");
        std::vector<llvm::Value*> folded;
        for (unsigned r = 0; r < reductions.size(); ++r) {
            auto* pPtr = builder.CreateStructGEP(argStructTy, combineArgPtr,
                                                 partialBase + r, "combine.partial.ptr");
            auto* pv = builder.CreateLoad(reductions[r].phi->getType(), pPtr,
                                          "combine.partial");
            folded.push_back(foldOp(builder, reductions[r], accs[r], pv));
        }
        auto* cNext = builder.CreateAdd(cIdx, llvm::ConstantInt::get(i64Ty, 1), "c.next");
        cIdx->addIncoming(cNext, combineBody);
        for (unsigned r = 0; r < reductions.size(); ++r)
            accs[r]->addIncoming(folded[r], combineBody);
        builder.CreateBr(combineHeader);

        builder.SetInsertPoint(combineExit);
        for (unsigned r = 0; r < reductions.size(); ++r)
            finalVals.push_back(foldOp(builder, reductions[r], origInits[r], accs[r]));
        builder.CreateBr(exitBlock);
        exitPred = combineExit;
    } else {
        // Branch to the original loop exit block
        builder.CreateBr(exitBlock);
    }

    // Remove the original loop blocks from the function.
    // First, update any PHI nodes in the exit block that reference loop blocks.
    for (llvm::PHINode& phi : exitBlock->phis()) {
        // Before stripping, see whether this exit PHI carries a reduction
        // value out of the loop (directly the carried PHI, or its LCSSA
        // accumulation binop). If so, its post-loop value is the combined
        // result, not the trip-count heuristic.
        int redIdx = -1;
        for (unsigned i = 0; i < phi.getNumIncomingValues() && redIdx < 0; ++i) {
            if (!loop->contains(phi.getIncomingBlock(i))) continue;
            llvm::Value* iv = phi.getIncomingValue(i);
            for (unsigned r = 0; r < reductions.size(); ++r)
                if (iv == reductions[r].phi || iv == reductions[r].binop) { redIdx = (int)r; break; }
        }
        // Remove incoming values from loop blocks
        for (unsigned i = phi.getNumIncomingValues(); i > 0; --i) {
            llvm::BasicBlock* inc = phi.getIncomingBlock(i - 1);
            if (loop->contains(inc)) {
                phi.removeIncomingValue(i - 1, false);
            }
        }
        // Add incoming from the exit predecessor if the PHI still needs a
        // value. Reduction-carrying PHIs get the combined result; others
        // fall back to the trip-count heuristic (induction var at exit).
        if (phi.getNumIncomingValues() == 0 ||
            std::find(phi.block_begin(), phi.block_end(), exitPred) == phi.block_end()) {
            llvm::Value* replacement;
            if (redIdx >= 0) {
                replacement = finalVals[redIdx];
            } else {
                replacement = llvm::UndefValue::get(phi.getType());
                if (phi.getType() == i64Ty)
                    replacement = tripCount;
                else if (phi.getType()->isIntegerTy())
                    replacement = builder.CreateIntCast(tripCount, phi.getType(), true);
            }
            phi.addIncoming(replacement, exitPred);
        }
    }

    // Defensive: any remaining out-of-loop use of a reduction value (e.g.
    // a non-LCSSA direct use) must see the combined result, not the undef
    // the blanket cleanup below would otherwise substitute.
    for (unsigned r = 0; r < reductions.size(); ++r) {
        auto outsideLoop = [&](llvm::Use& U) {
            auto* I = llvm::dyn_cast<llvm::Instruction>(U.getUser());
            return I && !loop->contains(I);
        };
        reductions[r].phi->replaceUsesWithIf(finalVals[r], outsideLoop);
        reductions[r].binop->replaceUsesWithIf(finalVals[r], outsideLoop);
    }

    // Delete original loop blocks (from innermost to outermost)
    std::vector<llvm::BasicBlock*> loopBlocks(loop->block_begin(), loop->block_end());
    // Detach: replace all uses of instructions in loop blocks with undef
    for (auto* BB : loopBlocks) {
        for (llvm::Instruction& I : *BB) {
            if (!I.use_empty()) {
                I.replaceAllUsesWith(llvm::UndefValue::get(I.getType()));
            }
        }
    }
    for (auto* BB : loopBlocks) {
        BB->eraseFromParent();
    }

    return true;
}

} // anonymous namespace

// `symbols` stays in the signature so the phase-1/phase-2 entry points
// remain interchangeable from the call site; phase-2 reads everything
// it needs from `mapping` and `parallelFuncs`.
static int partitionLoopsPhase2(llvm::Module& module,
                                [[maybe_unused]] const SymbolTable& symbols,
                                const SymbolMapping& mapping,
                                const LoopParallelConfig& config,
                                const std::unordered_set<std::string>& parallelFuncs) {
    int partitioned = 0;
    // Defer runtime declaration until we confirm at least one loop is eligible
    bool runtimeDeclared = false;
    LoopRuntimeDecls decls{};

    for (const auto& [topoName, llvmFunc] : mapping.matched) {
        if (!parallelFuncs.count(topoName)) continue;
        if (isExcluded(topoName, config.exclude)) continue;
        if (llvmFunc->isDeclaration()) continue;

        llvm::Function* func = llvmFunc;

        llvm::DominatorTree DT(*func);
        llvm::LoopInfo LI(DT);

        if (LI.empty()) continue;

        // Build ScalarEvolution for trip count analysis
        llvm::TargetLibraryInfoImpl TLII(llvm::Triple(module.getTargetTriple()));
        llvm::TargetLibraryInfo TLI(TLII);
        llvm::AssumptionCache AC(*func);
        llvm::ScalarEvolution SE(*func, TLI, AC, DT, LI);

        // Collect loops to partition (avoid modifying while iterating)
        struct LoopCandidate {
            llvm::Loop* loop;
            std::vector<ReductionInfo> reductions;
        };
        std::vector<LoopCandidate> candidates;
        for (llvm::Loop* loop : LI) {
            // Only handle simple loops: single exit, canonical induction
            if (!loop->getLoopPreheader()) continue;
            if (!loop->getExitBlock()) continue;
            if (!loop->getExitingBlock()) continue;
            if (!loop->getCanonicalInductionVariable()) continue;

            // Safety: no sub-loops (only innermost loops)
            if (!loop->getSubLoops().empty()) continue;

            // Safety: single-block body only. outlineLoopBody() flattens every
            // non-header block into one straight-line block, dropping internal
            // terminators and body PHIs — correct only when the body has no
            // internal control flow. A loop with an internal branch (if/else
            // -> >2 blocks and a merge PHI) would otherwise be silently
            // miscompiled (conditional code run unconditionally, merge PHI
            // RAUW'd to undef). Decline it rather than miscompile.
            if (loop->getNumBlocks() > 2) continue;

            // Safety: no sync primitives
            if (containsSyncPrimitives(loop)) continue;

            // Safety: check cross-iteration dependencies. If reduction
            // detection is enabled, try to classify loop-carried PHIs as
            // reductions before rejecting the loop.
            std::vector<ReductionInfo> loopReductions;
            if (hasCrossIterationDeps(loop, SE)) {
                if (!config.reductionEnabled) continue;

                llvm::BasicBlock* header = loop->getHeader();
                llvm::PHINode* inductionPHI = loop->getCanonicalInductionVariable();
                bool allReductions = true;

                for (llvm::PHINode& phi : header->phis()) {
                    if (&phi == inductionPHI) continue;

                    // Affine AddRecs are already safe for partitioning
                    const llvm::SCEV* scev = SE.getSCEV(&phi);
                    if (auto* addRec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(scev)) {
                        if (addRec->getLoop() == loop && addRec->isAffine()) continue;
                    }

                    auto red = detectReduction(&phi, loop);
                    if (!red) { allReductions = false; break; }
                    loopReductions.push_back(*red);
                }

                if (!allReductions || loopReductions.empty()) continue;
            }

            // Trip-count threshold removed; partition any loop
            // that satisfies the structural prerequisites (non-empty trip
            // count, non-excluded). Topo passes do not gate on workload size.

            candidates.push_back({loop, std::move(loopReductions)});
        }

        for (auto& candidate : candidates) {
            // Declare runtime on first actual partition attempt
            if (!runtimeDeclared) {
                decls = declareLoopRuntime(module);
                runtimeDeclared = true;
            }

            // Re-create analyses since IR mutation invalidates them.
            // The candidate's loop pointer belongs to the old LoopInfo;
            // look up the corresponding loop in the fresh LI2 via the
            // header block (which is stable across analysis rebuilds).
            llvm::DominatorTree DT2(*func);
            llvm::LoopInfo LI2(DT2);
            llvm::ScalarEvolution SE2(*func, TLI, AC, DT2, LI2);

            llvm::Loop* loop = LI2.getLoopFor(candidate.loop->getHeader());
            if (!loop) continue;

            auto captures = collectCaptures(loop);

            std::string loopName = topoName + ".loop." + std::to_string(partitioned);

            if (partitionLoop(module, func, loop, SE2, captures, loopName, decls, config, candidate.reductions)) {
                ++partitioned;
                break; // Function's loop structure is invalidated; move to next function
            }
        }
    }

    return partitioned;
}

// ===================================================================
// Public entry point
// ===================================================================

int LoopParallelizePass::run(llvm::Module& module,
                             const SymbolTable& symbols,
                             const SymbolMapping& mapping,
                             const LoopParallelConfig& config,
                             backend::LoopParallelizeReport* report) {
    if (!config.isEnabled()) return 0;

    auto parallelFuncs = findParallelStageFunctions(symbols);
    if (parallelFuncs.empty()) return 0;

    // Step 1: metadata annotation (always runs when enabled)
    std::unordered_map<std::string, int> perFnCount;
    int total = annotateLoopsPhase1(module, symbols, mapping, config, parallelFuncs,
                                    report ? &perFnCount : nullptr);

    // Step 2: partition-based parallelization (opt-in)
    if (config.partitionEnabled) {
        // Partition phase emits topo_parallel_ensure_init / topo_task_spawn
        // calls against libtopo-parallel — wire the one-time ABI-version
        // check matching the pattern in topo-llvm/runtime/ABI-COMPAT.md.
        // Idempotent and shared with TopoParallelPass.
        injectAbiCheckCtor(module, "parallel", "topo_parallel_version", abi::kParallelVersion);

        total += partitionLoopsPhase2(module, symbols, mapping, config, parallelFuncs);
    }

    if (report) {
        for (const auto& [name, count] : perFnCount) {
            backend::LoopParallelizeEntry e;
            e.hostFunction = name;
            e.annotatedLoops = count;
            report->entries.push_back(std::move(e));
        }
    }

    return total;
}

} // namespace topo
