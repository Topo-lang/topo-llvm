// @category: OPT
#include "topo/Transforms/IndirectionPass.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace topo {

namespace {

// ============================================================================
// Helper: Query ownership metadata from LLVM function
// ============================================================================

/// Query the SymbolTable for ownership info on a specific function parameter.
OwnershipKind getParamOwnershipFromSymbols(const SymbolTable& symbols, const std::string& funcName, unsigned paramIdx) {
    const auto* funcSym = symbols.findFunction(funcName);
    if (!funcSym || paramIdx >= funcSym->params.size()) return OwnershipKind::None;
    return funcSym->params[paramIdx].type.ownership;
}

/// Check if an alloca is backed by a function argument with 'owned' ownership.
/// Traces store instructions to find which argument feeds the alloca.
bool isOwnedAlloca(llvm::AllocaInst* alloca,
                   llvm::Function& func,
                   const SymbolTable& symbols,
                   const std::string& funcName) {
    (void)func;
    for (auto* user : alloca->users()) {
        auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
        if (!store || store->getPointerOperand() != alloca) continue;
        auto* arg = llvm::dyn_cast<llvm::Argument>(store->getValueOperand());
        if (!arg) continue;
        auto ownership = getParamOwnershipFromSymbols(symbols, funcName, arg->getArgNo());
        if (ownership == OwnershipKind::Owned) return true;
    }
    return false;
}

/// Check if an alloca is backed by a function argument with 'shared' ownership.
bool isSharedAlloca(llvm::AllocaInst* alloca,
                    llvm::Function& func,
                    const SymbolTable& symbols,
                    const std::string& funcName) {
    (void)func;
    for (auto* user : alloca->users()) {
        auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
        if (!store || store->getPointerOperand() != alloca) continue;
        auto* arg = llvm::dyn_cast<llvm::Argument>(store->getValueOperand());
        if (!arg) continue;
        auto ownership = getParamOwnershipFromSymbols(symbols, funcName, arg->getArgNo());
        if (ownership == OwnershipKind::Shared) return true;
    }
    return false;
}

// ============================================================================
// Helper: Check if a struct type name contains a specific smart pointer pattern
// ============================================================================

bool isSmartPtrType(llvm::StructType* sty, const std::string& pattern) {
    if (!sty || !sty->hasName()) return false;
    return sty->getName().contains(pattern);
}

/// Recursively strip nested struct wrappers to find the inner pointer member.
/// unique_ptr internally has: { __compressed_pair { T*, deleter } } or { T* }
/// Returns true if a pointer field was found, populating indexPath.
bool findInnerPointer(llvm::StructType* sty, std::vector<unsigned>& indexPath) {
    if (!sty) return false;

    for (unsigned i = 0; i < sty->getNumElements(); ++i) {
        auto* elemTy = sty->getElementType(i);

        // Direct pointer member
        if (elemTy->isPointerTy()) {
            indexPath.push_back(i);
            return true;
        }

        // Nested struct -- recurse
        if (auto* nested = llvm::dyn_cast<llvm::StructType>(elemTy)) {
            indexPath.push_back(i);
            if (findInnerPointer(nested, indexPath)) return true;
            indexPath.pop_back();
        }
    }
    return false;
}

/// Check if a value escapes the function (passed to calls or stored to
/// non-local memory).
bool escapesFunction(llvm::Value* val, llvm::Function& /*func*/) {
    for (auto* user : val->users()) {
        // Stored to memory that might be read elsewhere
        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            // If storing the value (not storing TO the value), it escapes
            if (store->getValueOperand() == val) return true;
        }
        // Passed to a function call
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(user)) {
            // If the callee is unknown, conservatively say it escapes
            if (call->getCalledFunction() == nullptr) return true;
            // Check if the value is an argument (not the callee)
            for (unsigned i = 0; i < call->arg_size(); ++i) {
                if (call->getArgOperand(i) == val) return true;
            }
        }
        // PHI nodes -- could escape through merge
        if (llvm::isa<llvm::PHINode>(user)) return true;
        // Select -- could escape
        if (llvm::isa<llvm::SelectInst>(user)) return true;
    }
    return false;
}

/// Check if any store instruction in the function writes to the internal
/// pointer of a smart pointer (indicating reset/move/release).
bool hasStoreToInternalPtr(llvm::Function& func,
                           llvm::Value* smartPtrVal,
                           llvm::StructType* /*smartPtrTy*/,
                           const std::vector<unsigned>& ptrFieldPath) {
    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst);
            if (!store) continue;

            // Check if the store destination is a GEP into the smart pointer
            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
            if (!gep) continue;

            // Check if GEP base points to our smart pointer value
            llvm::Value* base = gep->getPointerOperand();
            if (base != smartPtrVal) continue;

            // Check if the GEP indices match the pointer field path
            // (indicating a store to the internal pointer = reset/move)
            if (gep->getNumIndices() >= ptrFieldPath.size() + 1) {
                bool matches = true;
                unsigned idx = 1; // skip the first index (array deref)
                for (unsigned fieldIdx : ptrFieldPath) {
                    if (idx >= gep->getNumIndices()) {
                        matches = false;
                        break;
                    }
                    auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(idx + 1));
                    if (!ci || ci->getZExtValue() != fieldIdx) {
                        matches = false;
                        break;
                    }
                    ++idx;
                }
                if (matches) return true;
            }
        }
    }
    return false;
}

// ============================================================================
// 10a: unique_ptr -> reference promotion
// ============================================================================

int promoteUniquePtr(llvm::Module& module,
                     const std::vector<VisibilityEntry>& /*entries*/,
                     const SymbolMapping& mapping,
                     const SymbolTable& symbols,
                     const std::unordered_set<llvm::Function*>* allowed = nullptr) {
    int promoted = 0;

    // Build reverse mapping: LLVM Function* → qualified name
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        if (func) funcToName[func] = name;
    }

    for (auto& func : module) {
        if (func.isDeclaration()) continue;
        if (allowed && !allowed->count(&func)) continue;

        auto nameIt = funcToName.find(&func);

        // Find alloca/args that are unique_ptr types
        std::vector<std::pair<llvm::Value*, llvm::StructType*>> candidates;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
                if (!alloca) continue;

                auto* sty = llvm::dyn_cast<llvm::StructType>(alloca->getAllocatedType());
                if (!sty) continue;

                // Per-alloca ownership: check if this specific alloca is fed
                // by an 'owned' argument, OR fall back to name matching
                bool isOwned =
                    (nameIt != funcToName.end()) ? isOwnedAlloca(alloca, func, symbols, nameIt->second) : false;
                if (isOwned || isSmartPtrType(sty, "unique_ptr")) {
                    candidates.push_back({alloca, sty});
                }
            }
        }

        for (auto& [val, sty] : candidates) {
            // Find the internal pointer field path
            std::vector<unsigned> ptrFieldPath;
            if (!findInnerPointer(sty, ptrFieldPath)) continue;

            // Safety check: no store to internal pointer (no reset/move)
            if (hasStoreToInternalPtr(func, val, sty, ptrFieldPath)) continue;

            // Safety check: value doesn't escape
            if (escapesFunction(val, func)) continue;

            // Declarative approach: mark all loads of the internal pointer
            // with !invariant.load metadata so LLVM's GVN/EarlyCSE can
            // deduplicate and hoist them automatically.
            //
            // Collect all GEP+Load sequences that read the internal pointer.
            std::vector<llvm::LoadInst*> ptrLoads;
            for (auto* user : val->users()) {
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
                if (!gep) continue;

                // Check if this GEP accesses the pointer field
                bool matchesPath = true;
                if (gep->getNumIndices() < ptrFieldPath.size() + 1) {
                    matchesPath = false;
                } else {
                    unsigned idx = 1;
                    for (unsigned fieldIdx : ptrFieldPath) {
                        if (idx >= gep->getNumIndices()) {
                            matchesPath = false;
                            break;
                        }
                        auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(idx + 1));
                        if (!ci || ci->getZExtValue() != fieldIdx) {
                            matchesPath = false;
                            break;
                        }
                        ++idx;
                    }
                }
                if (!matchesPath) continue;

                for (auto* gepUser : gep->users()) {
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(gepUser)) {
                        ptrLoads.push_back(load);
                    }
                }
            }

            if (ptrLoads.empty()) continue;

            // Mark each load with !invariant.load and !nonnull metadata.
            // LLVM GVN will deduplicate invariant loads; EarlyCSE will
            // hoist them to the dominating block.
            auto* emptyMD = llvm::MDNode::get(module.getContext(), {});
            for (auto* load : ptrLoads) {
                load->setMetadata("invariant.load", emptyMD);
                load->setMetadata("nonnull", emptyMD);
            }

            // Add dereferenceable attribute to the underlying argument if
            // the alloca is backed by a function parameter.
            if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(val)) {
                for (auto* user : allocaInst->users()) {
                    auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (!store || store->getPointerOperand() != allocaInst) continue;
                    if (auto* arg = llvm::dyn_cast<llvm::Argument>(store->getValueOperand())) {
                        if (arg->getType()->isPointerTy() && !arg->hasAttribute(llvm::Attribute::NonNull)) {
                            arg->addAttr(llvm::Attribute::NonNull);
                        }
                    }
                }
            }

            ++promoted;
        }
    }

    return promoted;
}

// ============================================================================
// 10b: shared_ptr exclusive interval detection
// ============================================================================

/// Check if a stage has exclusive access (no concurrent stages).
/// `matchedKey` is the actual key under which this function's stage was
/// found (may be the simple-name fallback rather than `calledFn`). We must
/// skip BOTH so the function's own stage entry never self-matches and
/// wrongly reports the stage as non-exclusive — mirrors the noalias guard
/// in inferPointerAttrs (`otherName == stageIt->first`).
bool isExclusiveStage(const PipelineAnalysis& analysis, const std::string& calledFn,
                      const std::string& matchedKey, int thisStage) {
    for (const auto& [otherName, otherStage] : analysis.stages) {
        if (otherName == calledFn || otherName == matchedKey) continue;
        if (otherStage == thisStage) return false;
    }
    return true;
}

/// Find the stage number for a function in the pipeline analysis.
/// Returns -1 if not found. On success, `matchedKey` is set to the actual
/// stage-map key that matched (the qualified name or the simple-name
/// fallback) so the caller can exclude the function's own self-match when
/// testing stage exclusivity.
int findStageNumber(const PipelineAnalysis& analysis, const std::string& calledFn, std::string& matchedKey) {
    auto stageIt = analysis.stages.find(calledFn);
    if (stageIt != analysis.stages.end()) {
        matchedKey = stageIt->first;
        return stageIt->second;
    }

    // Try simple name
    auto lastSep = calledFn.rfind("::");
    std::string simple = (lastSep != std::string::npos) ? calledFn.substr(lastSep + 2) : calledFn;
    stageIt = analysis.stages.find(simple);
    if (stageIt != analysis.stages.end()) {
        matchedKey = stageIt->first;
        return stageIt->second;
    }

    return -1;
}

/// Downgrade adjacent sub-1 / add-1 AtomicRMW pairs on the same pointer
/// to non-atomic load+op+store sequences. Removing atomicity from cancelling
/// pairs lets LLVM's DCE recognize them as dead (the load+sub+store followed
/// by load+add+store on the same address with no other uses is trivially dead).
int eliminateBatchRefcount(llvm::Function& func) {
    int eliminated = 0;

    // Collect pairs to downgrade (cannot modify while iterating)
    struct AtomicPair {
        llvm::AtomicRMWInst* sub;
        llvm::AtomicRMWInst* add;
    };
    std::vector<AtomicPair> pairs;

    for (auto& bb : func) {
        llvm::AtomicRMWInst* pendingSub = nullptr;

        for (auto& inst : bb) {
            auto* atomicRMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst);
            if (!atomicRMW) {
                // Any intervening memory op or memory-ordering op invalidates
                // a pending sub: downgrading a sub/add pair that straddles a
                // fence (or cmpxchg) would silently drop the ordering edge the
                // fence/cmpxchg established. Include FenceInst and
                // AtomicCmpXchgInst alongside call/load/store.
                if (llvm::isa<llvm::CallBase>(inst) || llvm::isa<llvm::LoadInst>(inst) ||
                    llvm::isa<llvm::StoreInst>(inst) || llvm::isa<llvm::FenceInst>(inst) ||
                    llvm::isa<llvm::AtomicCmpXchgInst>(inst)) {
                    pendingSub = nullptr;
                }
                continue;
            }

            // Check for Sub 1
            if (atomicRMW->getOperation() == llvm::AtomicRMWInst::Sub) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(atomicRMW->getValOperand());
                if (ci && ci->isOne()) {
                    pendingSub = atomicRMW;
                    continue;
                }
            }

            // Check for Add 1 following a Sub 1 on same pointer
            if (atomicRMW->getOperation() == llvm::AtomicRMWInst::Add && pendingSub) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(atomicRMW->getValOperand());
                if (ci && ci->isOne() && atomicRMW->getPointerOperand() == pendingSub->getPointerOperand()) {
                    pairs.push_back({pendingSub, atomicRMW});
                    eliminated += 2;
                    pendingSub = nullptr;
                    continue;
                }
            }

            pendingSub = nullptr;
        }
    }

    // Replace each atomic op with non-atomic load+op+store.
    // LLVM DCE will clean up the resulting dead code.
    for (auto& [subInst, addInst] : pairs) {
        auto* ptrOp = subInst->getPointerOperand();
        auto* valTy = subInst->getType();
        auto* one = llvm::ConstantInt::get(valTy, 1);

        // Replace Sub 1 atomic with: load, sub 1, store (non-atomic)
        {
            llvm::IRBuilder<> builder(subInst);
            auto* loaded = builder.CreateLoad(valTy, ptrOp, "refcnt.load.sub");
            auto* result = builder.CreateSub(loaded, one, "refcnt.dec");
            builder.CreateStore(result, ptrOp);
            // The original AtomicRMW returned the old value
            subInst->replaceAllUsesWith(loaded);
            subInst->eraseFromParent();
        }

        // Replace Add 1 atomic with: load, add 1, store (non-atomic)
        {
            llvm::IRBuilder<> builder(addInst);
            auto* loaded = builder.CreateLoad(valTy, ptrOp, "refcnt.load.add");
            auto* result = builder.CreateAdd(loaded, one, "refcnt.inc");
            builder.CreateStore(result, ptrOp);
            addInst->replaceAllUsesWith(loaded);
            addInst->eraseFromParent();
        }
    }

    return eliminated;
}

/// Perform auto-deref on shared_ptr allocas in exclusive stages.
/// Extracts data pointer once at entry, replaces redundant loads.
int derefSharedPtr(llvm::Module& module, llvm::Function& func, llvm::AllocaInst* alloca, llvm::StructType* sty) {
    std::vector<unsigned> ptrFieldPath;
    if (!findInnerPointer(sty, ptrFieldPath)) return 0;

    // Safety: no store to internal pointer
    if (hasStoreToInternalPtr(func, alloca, sty, ptrFieldPath)) return 0;

    // Safety: value doesn't escape
    if (escapesFunction(alloca, func)) return 0;

    // Collect GEP+Load sequences reading the internal pointer
    std::vector<llvm::LoadInst*> ptrLoads;
    for (auto* user : alloca->users()) {
        auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
        if (!gep) continue;

        bool matchesPath = true;
        if (gep->getNumIndices() < ptrFieldPath.size() + 1) {
            matchesPath = false;
        } else {
            unsigned idx = 1;
            for (unsigned fieldIdx : ptrFieldPath) {
                if (idx >= gep->getNumIndices()) {
                    matchesPath = false;
                    break;
                }
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(idx + 1));
                if (!ci || ci->getZExtValue() != fieldIdx) {
                    matchesPath = false;
                    break;
                }
                ++idx;
            }
        }
        if (!matchesPath) continue;

        for (auto* gepUser : gep->users()) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(gepUser)) ptrLoads.push_back(load);
        }
    }

    if (ptrLoads.empty()) return 0;

    // Declarative approach: mark loads with !invariant.load metadata so
    // LLVM's GVN/EarlyCSE can deduplicate and hoist them automatically.
    auto* emptyMD = llvm::MDNode::get(module.getContext(), {});
    for (auto* load : ptrLoads) {
        load->setMetadata("invariant.load", emptyMD);
        load->setMetadata("nonnull", emptyMD);
    }

    return 1;
}

struct SharedPtrStats {
    int atomicDowngraded = 0;
    int dereferenced = 0;
    int refcountEliminated = 0;
};

SharedPtrStats optimizeSharedPtr(llvm::Module& module,
                                 const SymbolTable& symbols,
                                 const SymbolMapping& mapping,
                                 const std::unordered_set<llvm::Function*>* allowed = nullptr) {
    SharedPtrStats stats;

    // Build reverse mapping for alloca-level ownership checks
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        if (func) funcToName[func] = name;
    }

    for (const auto& [name, lb] : symbols.logicBlocks()) {
        if (!lb.isPipeline || !lb.pipelineAnalysis) continue;

        const auto& analysis = *lb.pipelineAnalysis;

        for (const auto& calledFn : lb.calledFunctions) {
            auto it = mapping.matched.find(calledFn);
            if (it == mapping.matched.end() || !it->second) continue;

            llvm::Function* func = it->second;
            if (func->isDeclaration()) continue;
            if (allowed && !allowed->count(func)) continue;

            std::string matchedKey;
            int thisStage = findStageNumber(analysis, calledFn, matchedKey);
            if (thisStage < 0) continue;
            if (!isExclusiveStage(analysis, calledFn, matchedKey, thisStage)) continue;

            // Step 1: Batch refcount elimination (before ordering downgrade)
            stats.refcountEliminated += eliminateBatchRefcount(*func);

            // Step 2: Atomic ordering downgrade + AtomicCmpXchg
            for (auto& bb : *func) {
                for (auto& inst : bb) {
                    if (auto* atomicRMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst)) {
                        if (atomicRMW->getOrdering() == llvm::AtomicOrdering::SequentiallyConsistent ||
                            atomicRMW->getOrdering() == llvm::AtomicOrdering::AcquireRelease) {
                            atomicRMW->setOrdering(llvm::AtomicOrdering::Monotonic);
                            ++stats.atomicDowngraded;
                        }
                    }
                    if (auto* cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst)) {
                        if (cmpxchg->getSuccessOrdering() == llvm::AtomicOrdering::SequentiallyConsistent ||
                            cmpxchg->getSuccessOrdering() == llvm::AtomicOrdering::AcquireRelease) {
                            cmpxchg->setSuccessOrdering(llvm::AtomicOrdering::Monotonic);
                            cmpxchg->setFailureOrdering(llvm::AtomicOrdering::Monotonic);
                            ++stats.atomicDowngraded;
                        }
                    }
                }
            }

            // Step 3: Auto-deref for shared_ptr allocas
            for (auto& bb : *func) {
                for (auto& inst : bb) {
                    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
                    if (!alloca) continue;

                    auto* sty = llvm::dyn_cast<llvm::StructType>(alloca->getAllocatedType());
                    if (!sty) continue;

                    // Per-alloca ownership or name-match fallback
                    auto fnIt = funcToName.find(func);
                    bool isShared =
                        (fnIt != funcToName.end()) ? isSharedAlloca(alloca, *func, symbols, fnIt->second) : false;
                    if (!isShared && !isSmartPtrType(sty, "shared_ptr")) continue;

                    stats.dereferenced += derefSharedPtr(module, *func, alloca, sty);
                }
            }
        }
    }

    return stats;
}

// ============================================================================
// 10c: vector -> span lowering
// ============================================================================

/// Check if a function modifies vector size (stores to begin/end/capacity
/// pointers).
bool hasVectorResize(llvm::Function& /*func*/, llvm::Value* vecVal, llvm::StructType* /*vecTy*/) {
    // vector internal layout: { T* begin, T* end, T* capacity }
    // A resize would store to fields 1 (end) or 2 (capacity)
    for (auto* user : vecVal->users()) {
        auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
        if (!gep) continue;

        // Check if any store goes through a GEP to field 1 or 2
        for (auto* gepUser : gep->users()) {
            auto* store = llvm::dyn_cast<llvm::StoreInst>(gepUser);
            if (!store) continue;

            // Only care about stores to this GEP (not stores of this GEP value)
            if (store->getPointerOperand() != gep) continue;

            // Check field index
            if (gep->getNumIndices() >= 2) {
                auto* fieldIdx = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
                if (fieldIdx) {
                    unsigned idx = fieldIdx->getZExtValue();
                    // Fields 1 (end) and 2 (capacity) indicate resize
                    if (idx == 1 || idx == 2) return true;
                }
            }
        }
    }
    return false;
}

int lowerVectorToSpan(llvm::Module& module,
                      const std::vector<VisibilityEntry>& /*entries*/,
                      const SymbolMapping& /*mapping*/,
                      const std::unordered_set<llvm::Function*>* allowed = nullptr) {
    int lowered = 0;

    for (auto& func : module) {
        if (func.isDeclaration()) continue;
        if (allowed && !allowed->count(&func)) continue;

        // Find vector-typed allocas/args
        std::vector<std::pair<llvm::Value*, llvm::StructType*>> candidates;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
                if (!alloca) continue;

                auto* sty = llvm::dyn_cast<llvm::StructType>(alloca->getAllocatedType());
                if (!sty) continue;

                if (isSmartPtrType(sty, "vector") && sty->getNumElements() >= 2 &&
                    sty->getElementType(0)->isPointerTy() && sty->getElementType(1)->isPointerTy()) {
                    candidates.push_back({alloca, sty});
                }
            }
        }

        for (auto& [val, sty] : candidates) {
            // Verify no-resize: no store to end/capacity fields
            if (hasVectorResize(func, val, sty)) continue;

            // Find the store that initializes the vector's begin pointer
            // (field 0). The begin/end loads must be inserted AFTER this store
            // — inserting them right after the alloca (as before) reads
            // uninitialized stack, and tagging that load `nonnull` is unsound.
            // If no initializing store to field 0 is found in this function, we
            // cannot prove the data pointer is valid/non-null, so decline the
            // lowering for this candidate rather than emit an uninitialized
            // read with a false nonnull claim.
            llvm::StoreInst* beginInitStore = nullptr;
            for (auto* user : val->users()) {
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
                if (!gep || gep->getNumIndices() < 2) continue;
                auto* fieldIdx = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
                if (!fieldIdx || fieldIdx->getZExtValue() != 0) continue;
                for (auto* gepUser : gep->users()) {
                    auto* store = llvm::dyn_cast<llvm::StoreInst>(gepUser);
                    if (store && store->getPointerOperand() == gep) beginInitStore = store;
                }
            }
            if (!beginInitStore) continue;

            // Extract data pointer and size right after the begin field is
            // initialized. This allows LLVM to treat the access pattern as a
            // simple span, enabling SIMD vectorization, while reading only
            // already-constructed values.
            llvm::IRBuilder<> builder(beginInitStore);
            if (auto* next = beginInitStore->getNextNode()) {
                builder.SetInsertPoint(next);
            }

            auto* ptrTy = llvm::PointerType::get(module.getContext(), 0);
            auto* i64Ty = llvm::Type::getInt64Ty(module.getContext());

            // GEP to data ptr (field 0: begin pointer)
            auto* dataGep = builder.CreateStructGEP(sty, val, 0, "vec.data.ptr");
            auto* dataPtr = builder.CreateLoad(ptrTy, dataGep, "vec.data");

            // GEP to end ptr (field 1)
            auto* endGep = builder.CreateStructGEP(sty, val, 1, "vec.end.ptr");
            auto* endPtr = builder.CreateLoad(ptrTy, endGep, "vec.end");

            // Size = (end - begin) via ptrdiff
            auto* beginInt = builder.CreatePtrToInt(dataPtr, i64Ty);
            auto* endInt = builder.CreatePtrToInt(endPtr, i64Ty);
            auto* byteSize = builder.CreateSub(endInt, beginInt, "vec.bytesize");
            (void)byteSize; // Size is available for downstream passes

            // Mark data pointer as nonnull only when the begin field was stored
            // a provably non-null value (a non-empty vector's begin() is
            // non-null). For other initializers we leave the load unannotated.
            if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(dataPtr)) {
                llvm::Value* stored = beginInitStore->getValueOperand();
                bool storedNonNull = llvm::isa<llvm::AllocaInst>(stored) ||
                                     llvm::isa<llvm::GlobalVariable>(stored) ||
                                     (llvm::isa<llvm::Constant>(stored) &&
                                      !llvm::isa<llvm::ConstantPointerNull>(stored));
                if (storedNonNull) {
                    auto* md = llvm::MDNode::get(module.getContext(), {});
                    loadInst->setMetadata("nonnull", md);
                }
            }

            ++lowered;
        }
    }

    return lowered;
}

// ============================================================================
// 10d: bare pointer nonnull/restrict inference
// ============================================================================

int inferPointerAttrs(llvm::Module& /*module*/,
                      const SymbolTable& symbols,
                      const SymbolMapping& mapping,
                      const std::unordered_set<llvm::Function*>* allowed = nullptr) {
    int attrsAdded = 0;

    // For each Topo-declared function, analyze call sites to infer attributes.
    for (const auto& [name, lb] : symbols.logicBlocks()) {
        if (!lb.isPipeline || !lb.pipelineAnalysis) continue;

        const auto& analysis = *lb.pipelineAnalysis;

        for (const auto& calledFn : lb.calledFunctions) {
            auto it = mapping.matched.find(calledFn);
            if (it == mapping.matched.end() || !it->second) continue;

            llvm::Function* func = it->second;
            if (func->isDeclaration()) continue;
            if (allowed && !allowed->count(func)) continue;

            // For each pointer parameter
            for (unsigned i = 0; i < func->arg_size(); ++i) {
                auto* arg = func->getArg(i);
                if (!arg->getType()->isPointerTy()) continue;

                // nonnull inference: check all call sites pass non-null values.
                // Conservative: only infer if the function is only called from
                // pipeline codegen (which always passes valid pointers).
                bool allCallsNonNull = true;
                bool hasCallers = false;

                for (auto* user : func->users()) {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (!call) {
                        allCallsNonNull = false;
                        break;
                    }

                    // `func` may appear in this CallBase as an ARGUMENT (e.g.
                    // g(func, ...)) rather than as the callee. In that case the
                    // call's own arg list is unrelated to func's signature, so
                    // indexing it with func's parameter index `i` is wrong and
                    // can read past the args (OOB). Only inspect call sites
                    // where func is actually the callee, and bounds-check i.
                    if (call->getCalledOperand() != func) continue;
                    if (i >= call->arg_size()) {
                        // Arity disagrees with func's signature — cannot prove
                        // anything about parameter i at this site.
                        allCallsNonNull = false;
                        break;
                    }
                    hasCallers = true;

                    llvm::Value* actualArg = call->getArgOperand(i);
                    // Check if the actual arg is known non-null
                    // (alloca result, GEP of non-null, or global variable)
                    if (!llvm::isa<llvm::AllocaInst>(actualArg) && !llvm::isa<llvm::GetElementPtrInst>(actualArg) &&
                        !llvm::isa<llvm::GlobalVariable>(actualArg)) {
                        allCallsNonNull = false;
                        break;
                    }
                }

                if (allCallsNonNull && hasCallers) {
                    if (!arg->hasNonNullAttr()) {
                        arg->addAttr(llvm::Attribute::NonNull);
                        ++attrsAdded;
                    }
                }

                // noalias inference: if this function is in a pipeline stage
                // and no other stage in the same level accesses the same
                // pointer, mark as noalias (restrict).
                auto stageIt = analysis.stages.find(calledFn);
                if (stageIt == analysis.stages.end()) {
                    auto lastSep = calledFn.rfind("::");
                    std::string simple = (lastSep != std::string::npos) ? calledFn.substr(lastSep + 2) : calledFn;
                    stageIt = analysis.stages.find(simple);
                }

                if (stageIt != analysis.stages.end()) {
                    int thisStage = stageIt->second;
                    bool noAlias = true;

                    for (const auto& [otherName, otherStage] : analysis.stages) {
                        if (otherName == calledFn || otherName == stageIt->first) continue;
                        if (otherStage == thisStage) {
                            noAlias = false;
                            break;
                        }
                    }

                    if (noAlias && !arg->hasAttribute(llvm::Attribute::NoAlias)) {
                        arg->addAttr(llvm::Attribute::NoAlias);
                        ++attrsAdded;
                    }
                }
            }
        }
    }

    return attrsAdded;
}

// ============================================================================
// 10e: devirtualization via type flow analysis
// ============================================================================

/// Build a mapping from base class qualified names to all known derived classes
/// and their member functions. Used to resolve virtual calls when the concrete
/// type is known from .topo declarations.
struct TypeHierarchy {
    // base class qualified name -> set of derived class qualified names
    std::unordered_map<std::string, std::vector<std::string>> derivedClasses;
    // class qualified name -> member function qualified names
    std::unordered_map<std::string, std::vector<std::string>> classMethods;

    void build(const SymbolTable& symbols) {
        for (const auto& [name, cls] : symbols.classSymbols()) {
            // Record member functions as a vtable-ordered view.
            //
            // ClassSymbol::memberFunctions is filled in source-DECLARATION
            // order and INCLUDES static methods (SemanticAnalyzer fills the
            // list and sets isStatic per function). A vtable slot only ever
            // counts instance (non-static) methods, so indexing the raw list
            // with a vtable slot mis-resolves whenever a static method
            // precedes the slot. Filter out static methods here so the slot
            // index lines up with the instance-method sequence. (Constructors
            // and the destructor live in separate ClassSymbol fields and are
            // already absent from this list.)
            //
            // Topo has no `virtual` keyword (it is rejected by the lexer), so
            // there is no finer non-virtual signal to filter on; the
            // instance-method order is the best available vtable-slot mapping.
            auto& view = classMethods[name];
            view.clear();
            for (const auto& methodQName : cls.memberFunctions) {
                const auto* fnSym = symbols.findFunction(methodQName);
                if (fnSym && fnSym->isStatic) continue; // static => no vtable slot
                view.push_back(methodQName);
            }

            // Record inheritance relationship
            if (cls.baseClass) {
                std::string baseName;
                for (size_t i = 0; i < cls.baseClass->nameParts.size(); ++i) {
                    if (i > 0) baseName += "::";
                    baseName += cls.baseClass->nameParts[i];
                }
                if (!baseName.empty()) {
                    derivedClasses[baseName].push_back(name);
                }
            }
        }
    }

    /// Given a base class name, find the unique concrete derived class.
    /// Returns empty string if there are zero or multiple derived classes.
    std::string findUniqueDerived(const std::string& baseName) const {
        auto it = derivedClasses.find(baseName);
        if (it == derivedClasses.end() || it->second.size() != 1) return {};
        return it->second[0];
    }

    /// Find the concrete method in a class that matches a given simple name.
    std::string findMethod(const std::string& className, const std::string& methodSimpleName) const {
        auto it = classMethods.find(className);
        if (it == classMethods.end()) return {};

        for (const auto& methodQName : it->second) {
            // Extract simple name from qualified: "ns::Class::method" -> "method"
            auto lastSep = methodQName.rfind("::");
            std::string simple = (lastSep != std::string::npos) ? methodQName.substr(lastSep + 2) : methodQName;
            if (simple == methodSimpleName) return methodQName;
        }
        return {};
    }
};

/// Build a mapping from function parameters to their declared concrete types.
/// When a .topo declaration specifies a parameter type that is a concrete class
/// (not an abstract base), we can use that to resolve virtual calls on that
/// parameter within the function body.
struct ConcreteTypeMap {
    // funcQualifiedName -> (paramIdx -> concrete class qualified name)
    std::unordered_map<std::string, std::unordered_map<unsigned, std::string>> paramTypes;

    void build(const SymbolTable& symbols, const TypeHierarchy& hierarchy) {
        for (const auto& [name, funcSym] : symbols.functions()) {
            for (unsigned i = 0; i < funcSym.params.size(); ++i) {
                const auto& param = funcSym.params[i];
                // Build the type name from nameParts
                std::string typeName;
                for (size_t j = 0; j < param.type.nameParts.size(); ++j) {
                    if (j > 0) typeName += "::";
                    typeName += param.type.nameParts[j];
                }
                if (typeName.empty()) continue;

                // Check if this type is a known class in the symbol table
                const auto* cls = symbols.findClassSymbol(typeName);
                if (!cls) continue;

                // This type is concrete if:
                // 1. It has no derived classes (leaf class), OR
                // 2. It has a baseClass (meaning it IS a derived class)
                bool isLeaf = hierarchy.derivedClasses.find(typeName) == hierarchy.derivedClasses.end();
                bool isDerived = cls->baseClass.has_value();

                if (isLeaf || isDerived) {
                    paramTypes[name][i] = typeName;
                }
            }
        }
    }
};

/// Detect virtual call pattern in LLVM IR:
///   %vtable = load ptr, ptr %obj             ; load vtable pointer
///   %fptr   = getelementptr ptr, %vtable, i  ; index into vtable
///   %target = load ptr, ptr %fptr            ; load function pointer
///   call ... %target(...)                     ; indirect call
///
/// Returns true if this CallBase is an indirect call through a vtable.
/// Populates vtableLoad and vtableIdx on success.
bool isVtableCall(llvm::CallBase* call, llvm::LoadInst*& vtableLoad, int& vtableIdx) {
    if (!call || call->getCalledFunction()) return false; // Already a direct call

    auto* calledVal = call->getCalledOperand();

    // Pattern: load from GEP into vtable
    auto* fptrLoad = llvm::dyn_cast<llvm::LoadInst>(calledVal);
    if (!fptrLoad) return false;

    auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(fptrLoad->getPointerOperand());
    if (!gep) {
        // Simpler pattern: direct load from vtable pointer (index 0)
        auto* baseLoad = llvm::dyn_cast<llvm::LoadInst>(fptrLoad->getPointerOperand());
        if (baseLoad && baseLoad->getType()->isPointerTy()) {
            vtableLoad = baseLoad;
            vtableIdx = 0;
            return true;
        }
        return false;
    }

    // GEP base should be a load of the vtable pointer
    auto* baseLoad = llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());
    if (!baseLoad) return false;

    vtableLoad = baseLoad;

    // Extract vtable index from GEP. Only a single CONSTANT index identifies a
    // recognizable vtable slot. A non-constant (runtime) index — or any other
    // GEP shape — is NOT a slot we can resolve, so report no match rather than
    // falsely claiming slot 0 (which would devirtualize to method[0]).
    if (gep->getNumIndices() == 1) {
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1))) {
            vtableIdx = static_cast<int>(ci->getZExtValue());
            return true;
        }
    }

    return false;
}

/// Trace the object pointer from a vtable load back to a function argument.
/// vtableLoad loads the vtable from the object; we follow the pointer
/// operand chain to find which Argument feeds it.
llvm::Argument* traceToArgument(llvm::LoadInst* vtableLoad) {
    llvm::Value* ptr = vtableLoad->getPointerOperand();

    // Walk through bitcasts and GEPs to find the underlying alloca/arg
    constexpr int maxDepth = 8;
    for (int depth = 0; depth < maxDepth; ++depth) {
        if (auto* arg = llvm::dyn_cast<llvm::Argument>(ptr)) return arg;

        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
            ptr = gep->getPointerOperand();
            continue;
        }

        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(ptr)) {
            ptr = load->getPointerOperand();
            continue;
        }

        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(ptr)) {
            // Check if alloca is fed by a store from an argument
            for (auto* user : alloca->users()) {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (!store || store->getPointerOperand() != alloca) continue;
                if (auto* arg = llvm::dyn_cast<llvm::Argument>(store->getValueOperand())) return arg;
            }
            return nullptr;
        }

        break;
    }
    return nullptr;
}

// `module` is part of the call-site interface that other devirt entry
// points share; this implementation walks `mapping` instead, but the
// parameter stays so adding a module-level scan later is a body edit,
// not a signature change.
int devirtualizeCalls([[maybe_unused]] llvm::Module& module,
                      const SymbolTable& symbols,
                      const SymbolMapping& mapping,
                      const std::unordered_set<llvm::Function*>* allowed) {
    int devirtualized = 0;

    // Build type hierarchy from .topo class declarations
    TypeHierarchy hierarchy;
    hierarchy.build(symbols);

    // Build concrete type map from function parameter declarations
    ConcreteTypeMap typeMap;
    typeMap.build(symbols, hierarchy);

    // Build reverse mapping: LLVM Function* -> qualified name
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        if (func) funcToName[func] = name;
    }

    // Iterate over pipeline stage functions
    for (const auto& [lbName, lb] : symbols.logicBlocks()) {
        if (!lb.isPipeline || !lb.pipelineAnalysis) continue;

        for (const auto& calledFn : lb.calledFunctions) {
            auto it = mapping.matched.find(calledFn);
            if (it == mapping.matched.end() || !it->second) continue;

            llvm::Function* func = it->second;
            if (func->isDeclaration()) continue;
            if (allowed && !allowed->count(func)) continue;

            // Look up concrete type info for this function's parameters
            auto typeIt = typeMap.paramTypes.find(calledFn);
            if (typeIt == typeMap.paramTypes.end()) continue;
            const auto& paramConcreteTypes = typeIt->second;

            // Scan for indirect calls (vtable dispatch pattern)
            std::vector<std::pair<llvm::CallBase*, std::string>> replacements;

            for (auto& bb : *func) {
                for (auto& inst : bb) {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call) continue;

                    llvm::LoadInst* vtableLoad = nullptr;
                    int vtableIdx = 0;
                    if (!isVtableCall(call, vtableLoad, vtableIdx)) continue;

                    // Trace vtable load back to a function argument
                    auto* arg = traceToArgument(vtableLoad);
                    if (!arg) continue;

                    // Check if we know the concrete type for this argument
                    auto concreteIt = paramConcreteTypes.find(arg->getArgNo());
                    if (concreteIt == paramConcreteTypes.end()) continue;

                    const std::string& concreteClass = concreteIt->second;

                    // Find the class methods for the concrete type
                    auto methodsIt = hierarchy.classMethods.find(concreteClass);
                    if (methodsIt == hierarchy.classMethods.end()) continue;

                    // Resolve: vtableIdx maps to the nth virtual method.
                    // Since we only handle single-inheritance, vtable layout
                    // follows declaration order in the class.
                    if (vtableIdx < 0 || static_cast<size_t>(vtableIdx) >= methodsIt->second.size()) continue;

                    const std::string& targetMethod = methodsIt->second[vtableIdx];

                    // Find the LLVM function for this resolved method
                    auto targetIt = mapping.matched.find(targetMethod);
                    if (targetIt == mapping.matched.end() || !targetIt->second) continue;

                    replacements.push_back({call, targetMethod});
                }
            }

            // Apply replacements
            for (auto& [call, targetMethod] : replacements) {
                auto targetIt = mapping.matched.find(targetMethod);
                if (targetIt == mapping.matched.end() || !targetIt->second) continue;

                llvm::Function* targetFunc = targetIt->second;

                // Decline unless the resolved target's signature exactly
                // matches the indirect call's current FunctionType. Equal types
                // guarantee arity AND per-argument/return type agreement, so the
                // rewritten direct call is well-formed; any mismatch would make
                // setCalledFunction produce IR the verifier rejects (or, with
                // assertions off, silently mis-type args). Correctness first:
                // leave the indirect call when the types differ.
                if (targetFunc->getFunctionType() != call->getFunctionType()) {
                    continue;
                }

                // Replace the indirect call with a direct call.
                // setCalledFunction (not setCalledOperand) also refreshes the
                // CallBase's cached FunctionType to match the new callee —
                // setCalledOperand would leave a stale FTy that disagrees with
                // targetFunc and fails the verifier.
                call->setCalledFunction(targetFunc);

                // Add inlinehint to the devirtualized target
                if (!targetFunc->hasFnAttribute(llvm::Attribute::InlineHint)) {
                    targetFunc->addFnAttr(llvm::Attribute::InlineHint);
                }

                // Emit optimization remark
                llvm::errs() << "topo: remark: devirtualized call in " << func->getName() << " → "
                             << targetFunc->getName() << " (concrete type from .topo declaration)\n";

                ++devirtualized;
            }
        }
    }

    return devirtualized;
}

/// 10f: Annotate vtable loads as constants when the concrete type is known,
/// and speculatively devirtualize when a dominant type exists.
int annotateVtableConstants(llvm::Module& module,
                            const SymbolTable& symbols,
                            const SymbolMapping& mapping,
                            const std::unordered_set<llvm::Function*>* allowed) {
    int annotated = 0;

    // Build type hierarchy (same as devirtualizeCalls)
    TypeHierarchy hierarchy;
    hierarchy.build(symbols);

    // Build concrete type map from function parameter declarations
    ConcreteTypeMap typeMap;
    typeMap.build(symbols, hierarchy);

    // Build reverse mapping: LLVM Function* -> qualified name
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        if (func) funcToName[func] = name;
    }

    auto& ctx = module.getContext();

    for (auto& func : module) {
        if (func.isDeclaration()) continue;
        if (allowed && !allowed->count(&func)) continue;

        auto nameIt = funcToName.find(&func);
        if (nameIt == funcToName.end()) continue;

        // Look up concrete type info for this function's parameters
        auto typeIt = typeMap.paramTypes.find(nameIt->second);
        if (typeIt == typeMap.paramTypes.end()) continue;
        const auto& paramConcreteTypes = typeIt->second;

        // Collect transformations to avoid iterator invalidation from BB splitting
        struct VtableAnnotation {
            llvm::LoadInst* vtableLoad;
            bool singleType; // true = constant annotation, false = speculative devirt
        };
        struct SpeculativeDevirt {
            llvm::CallBase* call;
            llvm::LoadInst* vtableLoad;
            int vtableIdx;
            std::string hotType;
        };
        std::vector<VtableAnnotation> annotations;
        std::vector<SpeculativeDevirt> specDevirts;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) continue;

                llvm::LoadInst* vtableLoad = nullptr;
                int vtableIdx = 0;
                if (!isVtableCall(call, vtableLoad, vtableIdx)) continue;

                // Already devirtualized (direct call) — skip
                if (call->getCalledFunction()) continue;

                // Trace to argument to find declared type
                auto* arg = traceToArgument(vtableLoad);
                if (!arg) continue;

                auto paramIt = paramConcreteTypes.find(arg->getArgNo());
                if (paramIt == paramConcreteTypes.end()) continue;

                const std::string& baseType = paramIt->second;

                // Find all derived types
                auto derivedIt = hierarchy.derivedClasses.find(baseType);
                if (derivedIt == hierarchy.derivedClasses.end()) continue;
                const auto& derivedTypes = derivedIt->second;

                if (derivedTypes.size() == 1) {
                    // Single concrete type — mark vtable load as constant
                    annotations.push_back({vtableLoad, true});
                } else if (!derivedTypes.empty()) {
                    // Multiple types — candidate for speculative devirt.
                    // Restrict to plain CallInst: an InvokeInst is a terminator,
                    // so the merge-split below (splitBasicBlock at the call's
                    // next node) would dereference a null next-node and corrupt
                    // the CFG. C++ virtual calls in EH scopes lower to indirect
                    // invokes, which reach this pass — skip them.
                    if (llvm::isa<llvm::CallInst>(call)) {
                        specDevirts.push_back({call, vtableLoad, vtableIdx, derivedTypes[0]});
                    }
                }
            }
        }

        // Apply constant annotations (safe — no CFG changes)
        for (auto& ann : annotations) {
            ann.vtableLoad->setMetadata("topo.vtable.constant",
                llvm::MDNode::get(ctx, {}));
            ++annotated;
        }

        // Speculative devirtualization (guarded fast path) is DECLINED.
        //
        // The intended transform splits the call site and emits a guard that
        // compares the object's loaded vtable pointer against the hot type's
        // vtable, taking a direct call on the fast path. Emitting that guard
        // correctly and safely requires three things this pass cannot
        // currently guarantee, so we conservatively leave every candidate as
        // its original indirect call (semantics-preserving):
        //
        //  1. Terminator call sites. An indirect `invoke` (a C++ virtual call
        //     in an EH scope) is a terminator; splitting at its next node
        //     dereferences a null pointer. The collection step already filters
        //     to plain CallInst, but the guard below is declined regardless.
        //  2. Correct guard target. The guard must compare against the vtable
        //     ADDRESS-POINT (past the Itanium offset-to-top + RTTI header), not
        //     the `_ZTV<name>` global base. The two differ by the header size,
        //     so a base comparison is permanently false — a dead fast path and
        //     an inflated stat. This pass has no reliable ABI model to derive
        //     the address-point offset (it varies with pointer size and
        //     inheritance shape).
        //  3. Signature/attribute fidelity. The fast-path direct call must
        //     match the indirect call's arity and carry its calling convention
        //     plus parameter/return attributes (sret/byval/zeroext/...);
        //     dropping them mis-ABIs the call.
        //
        // When a reliable address-point offset becomes available, re-enable a
        // guarded fast path here that: rejects InvokeInst, checks
        // arity/variadic compatibility against the target FunctionType, GEPs
        // `_ZTV<name>` to the address-point for the guard, and copies
        // setCallingConv + setAttributes onto the direct call.
        for (const auto& sd : specDevirts) {
            (void)sd; // candidate identified but not transformed (see above)
        }
    }

    return annotated;
}

} // anonymous namespace

// ============================================================================
// IndirectionPass entry point
// ============================================================================

IndirectionStats IndirectionPass::run(llvm::Module& module,
                                      const std::vector<VisibilityEntry>& entries,
                                      const SymbolMapping& mapping,
                                      const SymbolTable& symbols,
                                      const IndirectionConfig& config,
                                      const std::unordered_set<std::string>* functionFilter) {
    IndirectionStats stats;

    if (!config.isEnabled()) return stats;

    // Build allowed-function set when filter is active
    std::unordered_set<llvm::Function*> allowed;
    const std::unordered_set<llvm::Function*>* allowedPtr = nullptr;
    if (functionFilter) {
        for (const auto& [name, func] : mapping.matched) {
            if (func && functionFilter->count(func->getName().str())) allowed.insert(func);
        }
        if (allowed.empty()) return stats;
        allowedPtr = &allowed;
    }

    // 10a: unique_ptr -> reference promotion (ownership-aware)
    if (config.uniquePtrPromotion) {
        stats.uniquePtrPromoted = promoteUniquePtr(module, entries, mapping, symbols, allowedPtr);
    }

    // 10b: shared_ptr exclusive interval detection + auto-deref + refcount elimination
    if (config.sharedPtrExclusive) {
        auto spStats = optimizeSharedPtr(module, symbols, mapping, allowedPtr);
        stats.sharedPtrOptimized = spStats.atomicDowngraded;
        stats.sharedPtrDereferenced = spStats.dereferenced;
        stats.refcountEliminated = spStats.refcountEliminated;
    }

    // 10c: vector -> span lowering
    // Decision of whether to apply is made by variant benchmark in PassPipeline
    // (auto mode) or unconditionally (force mode). The pass always applies.
    if (config.vectorSpanLowering) {
        stats.vectorLowered = lowerVectorToSpan(module, entries, mapping, allowedPtr);
    }

    // 10d: bare pointer nonnull/restrict inference — only when other
    // transforms fired. Standalone attr inference shifts LLVM alias
    // analysis without corresponding code improvements.
    if (config.pointerAttrInference && (stats.uniquePtrPromoted + stats.sharedPtrOptimized + stats.vectorLowered > 0)) {
        stats.pointerAttrsAdded = inferPointerAttrs(module, symbols, mapping, allowedPtr);
    }

    // 10e: devirtualization via type flow analysis — uses .topo class
    // declarations + inheritance hierarchy to resolve virtual calls when
    // the concrete type is known at a pipeline stage boundary.
    if (config.devirtualize) {
        stats.callsDevirtualized = devirtualizeCalls(module, symbols, mapping, allowedPtr);
    }

    // 10f: vtable constant annotation + speculative devirtualization
    if (config.vtableOptimize) {
        stats.vtableConstantsAnnotated = annotateVtableConstants(module, symbols, mapping, allowedPtr);
    }

    return stats;
}

void IndirectionPass::diagnose(llvm::Module& module,
                               const std::vector<VisibilityEntry>& /*entries*/,
                               const SymbolMapping& mapping,
                               [[maybe_unused]] const SymbolTable& symbols) {
    // `symbols` is part of the diagnose() interface peers expose; this
    // implementation reads pattern info purely from llvm::Module. Kept
    // so a future declaration-driven diagnostic is a body edit, not a
    // signature change.
    // Build reverse mapping: LLVM Function* → qualified name
    std::unordered_map<llvm::Function*, std::string> funcToName;
    for (const auto& [name, func] : mapping.matched) {
        if (func) funcToName[func] = name;
    }

    int uniquePtrFound = 0;
    int sharedPtrFound = 0;
    int vectorFound = 0;
    int rawPtrFound = 0;
    int indirectCallFound = 0;

    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
                if (!alloca) continue;

                auto* sty = llvm::dyn_cast<llvm::StructType>(alloca->getAllocatedType());
                if (!sty || !sty->hasName()) continue;

                auto name = sty->getName();
                if (name.contains("unique_ptr"))
                    ++uniquePtrFound;
                else if (name.contains("shared_ptr"))
                    ++sharedPtrFound;
                else if (name.contains("vector") && sty->getNumElements() >= 2 && sty->getElementType(0)->isPointerTy())
                    ++vectorFound;
            }
        }

        // Check raw pointer parameters
        for (unsigned i = 0; i < func.arg_size(); ++i) {
            if (func.getArg(i)->getType()->isPointerTy() && !func.getArg(i)->hasNonNullAttr()) ++rawPtrFound;
        }

        // Check for indirect calls (potential devirtualization targets)
        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) continue;
                llvm::LoadInst* vtableLoad = nullptr;
                int vtableIdx = 0;
                if (isVtableCall(call, vtableLoad, vtableIdx)) ++indirectCallFound;
            }
        }
    }

    if (uniquePtrFound > 0)
        llvm::errs() << "topo: note: found " << uniquePtrFound << " unique_ptr alloca(s) that could be stack-promoted"
                     << " (enable [optimize.indirection])\n";
    if (sharedPtrFound > 0)
        llvm::errs() << "topo: note: found " << sharedPtrFound
                     << " shared_ptr alloca(s) with potential atomic downgrade"
                     << " (enable [optimize.indirection])\n";
    if (vectorFound > 0)
        llvm::errs() << "topo: note: found " << vectorFound << " vector alloca(s) that could use span lowering"
                     << " (enable [optimize.indirection])\n";
    if (rawPtrFound > 0)
        llvm::errs() << "topo: note: found " << rawPtrFound
                     << " raw pointer param(s) that could be marked nonnull/noalias"
                     << " (enable [optimize.indirection])\n";
    if (indirectCallFound > 0)
        llvm::errs() << "topo: note: found " << indirectCallFound << " indirect call(s) that may be devirtualizable"
                     << " (enable [optimize.indirection])\n";
}

} // namespace topo
