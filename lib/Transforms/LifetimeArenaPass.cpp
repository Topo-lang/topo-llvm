// @category: OPT
#include "topo/Transforms/LifetimeArenaPass.h"
#include "topo/Analysis/LifetimeAnalysis.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace topo {

namespace {

// Allocation site descriptor
struct AllocSite {
    llvm::CallInst* call;
    llvm::Value* size; // total byte count
    bool isCalloc;     // needs memset after arena_alloc
    std::vector<llvm::CallInst*> matchedFrees;
};

// Returns true if the callee name is a heap allocator we can replace.
bool isAllocFunction(llvm::StringRef name) {
    return name == "malloc" || name == "_Znwm" || name == "_Znwj" || name == "_Znam" || name == "_Znaj" ||
           name == "calloc";
}

// Returns true if the callee name is a free/delete we can remove.
bool isFreeFunction(llvm::StringRef name) {
    return name == "free" || name == "_ZdlPv" || name == "_ZdaPv" || name == "_ZdlPvm" || name == "_ZdaPvm";
}

/// Get or declare an external C function (mirrors AdaptiveDispatchPass).
llvm::FunctionCallee getOrDeclareFunc(llvm::Module& module, const std::string& name, llvm::FunctionType* ty) {
    if (auto* existing = module.getFunction(name)) return existing;
    return module.getOrInsertFunction(name, ty);
}

/// Create (or reuse) a private global constant C string, returning an
/// i8* to its first element. Same shape as AdaptiveDispatchPass's helper
/// so the injected `pass`/`subject`/state arguments are interned once.
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

// Scan a function for heap allocation call sites.
std::vector<AllocSite> findAllocSites(llvm::Function* func) {
    std::vector<AllocSite> sites;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
            if (!call) continue;
            auto* callee = call->getCalledFunction();
            if (!callee) continue;
            auto name = callee->getName();
            if (!isAllocFunction(name)) continue;

            AllocSite site;
            site.call = call;
            site.isCalloc = (name == "calloc");

            if (site.isCalloc) {
                // calloc(count, size) -> total = count * size
                // We'll create the mul instruction during replacement
                site.size = nullptr; // placeholder, handled during replacement
            } else {
                site.size = call->getArgOperand(0);
            }
            sites.push_back(site);
        }
    }
    return sites;
}

// Conservative escape analysis for an allocation result.
// Returns true if the pointer escapes the scope (unsafe to arena-allocate).
// Also collects matched free/delete calls into matchedFrees.
bool escapesScope(llvm::Value* allocResult,
                  const std::unordered_set<llvm::Function*>& scopeFunctions,
                  std::vector<llvm::CallInst*>& matchedFrees) {
    std::vector<llvm::Value*> worklist;
    std::unordered_set<llvm::Value*> visited;
    worklist.push_back(allocResult);

    while (!worklist.empty()) {
        auto* val = worklist.back();
        worklist.pop_back();
        if (!visited.insert(val).second) continue;

        for (auto* user : val->users()) {
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                // GEP is safe, but track its uses
                worklist.push_back(gep);
                continue;
            }
            if (auto* bitcast = llvm::dyn_cast<llvm::BitCastInst>(user)) {
                worklist.push_back(bitcast);
                continue;
            }
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
                worklist.push_back(phi);
                continue;
            }
            if (auto* select = llvm::dyn_cast<llvm::SelectInst>(user)) {
                worklist.push_back(select);
                continue;
            }
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user)) {
                // Loading from the pointer is safe (reading data)
                (void)load;
                continue;
            }
            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
                // Storing TO the pointer (ptr is the address) is safe
                if (store->getPointerOperand() == val) continue;
                // Storing the pointer value somewhere — check if destination
                // is a local alloca (or GEP into one)
                auto* dest = store->getPointerOperand()->stripInBoundsOffsets();
                if (auto* allocaBase = llvm::dyn_cast<llvm::AllocaInst>(dest)) {
                    // The pointer was spilled into a local slot. Track loads
                    // from this alloca (through any GEP) so we can find
                    // matching free() calls that reload the pointer.
                    //
                    // The slot itself must NOT escape: if the slot's ADDRESS
                    // leaks (stored elsewhere, passed to a call, returned,
                    // ptrtoint'd, …) then whoever holds the address can reload
                    // and outlive the pointer past arena teardown — a dangling
                    // pointer. So every user of the slot (or a GEP into it)
                    // other than a load or an address-into-slot store is
                    // treated as an escape, mirroring the conservative default
                    // on the main worklist. Bailing out here keeps the site
                    // un-converted, which is always safe.
                    std::vector<llvm::Value*> allocaWork = {allocaBase};
                    std::unordered_set<llvm::Value*> allocaVisited;
                    while (!allocaWork.empty()) {
                        auto* av = allocaWork.back();
                        allocaWork.pop_back();
                        if (!allocaVisited.insert(av).second) continue;
                        for (auto* au : av->users()) {
                            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(au)) {
                                // Reloading the pointer — continue the escape
                                // walk from the loaded value.
                                worklist.push_back(ld);
                            } else if (llvm::isa<llvm::GetElementPtrInst>(au)) {
                                // Address arithmetic within the slot — recurse.
                                allocaWork.push_back(au);
                            } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(au)) {
                                // Writing INTO the slot (slot is the address)
                                // is the normal spill; it stays local. But
                                // storing the slot's ADDRESS as a value
                                // somewhere leaks it — escape.
                                if (st->getValueOperand() == av) return true;
                            } else {
                                // call(&slot), ret &slot, ptrtoint &slot, or any
                                // other use of the slot address — escape.
                                return true;
                            }
                        }
                    }
                    continue;
                }
                // Store to global or non-local — escapes
                return true;
            }
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(user)) {
                auto* callee = call->getCalledFunction();
                if (!callee) return true; // indirect call — escapes

                auto calleeName = callee->getName();
                // free/delete: matched free, safe. Dedup within this walk:
                // a free is terminal and never enters `visited`, so a single
                // free reachable through two traversed values (direct +
                // reloaded-from-alloca, or aliasing bitcast/GEP) would
                // otherwise be recorded — and later erased — twice.
                if (isFreeFunction(calleeName)) {
                    if (std::find(matchedFrees.begin(), matchedFrees.end(), call) == matchedFrees.end())
                        matchedFrees.push_back(call);
                    continue;
                }
                // llvm.lifetime/memcpy/memset/memmove intrinsics are safe
                if (calleeName.starts_with("llvm.")) continue;
                // Passing to a scope function is safe
                if (scopeFunctions.count(callee)) continue;
                // Passing to any other function — escapes
                return true;
            }
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(user)) {
                // Returning the pointer from the owner — escapes
                (void)ret;
                return true;
            }
            // Any other use — conservatively escapes
            return true;
        }
    }
    return false;
}

/// Shared scope discovery: finds the owner function, start/end calls,
/// scope IR functions, and safe allocation sites for a lifetime scope.
struct ScopeInfo {
    llvm::Function* ownerFunc = nullptr;
    llvm::CallInst* startCall = nullptr;
    llvm::CallInst* endCall = nullptr;
    std::unordered_set<llvm::Function*> scopeFuncsIR;
    std::vector<AllocSite> safeSites;
    // free/delete calls reached on the def-use walk of allocations that
    // were NOT converted (escaping sites). A free in this set is the
    // genuine deallocation of an allocation the arena does not own, so it
    // must never be erased even if a converted site also appears to match
    // it (e.g. via a reassigned alloca slot). Conservative leak-avoidance.
    std::unordered_set<llvm::CallInst*> escapingFrees;
};

/// Discover scope boundaries and safe allocation sites.
/// Returns nullopt if the scope has no owner or no safe sites.
std::optional<ScopeInfo> discoverScope(const std::string& groupName,
                                       const analysis::LifetimeScope& scope,
                                       const SymbolTable& symbols,
                                       const SymbolMapping& mapping) {
    if (scope.coveredFunctions.empty()) return std::nullopt;

    const LifetimeGroupEntry* group = symbols.findLifetimeGroup(groupName);
    if (!group) return std::nullopt;

    ScopeInfo info;

    // Find owner function: the logic block that calls the start function
    for (const auto& [blockName, block] : symbols.logicBlocks()) {
        auto mapIt = mapping.matched.find(block.qualifiedName);
        if (mapIt == mapping.matched.end()) continue;
        auto* candidate = mapIt->second;
        if (!candidate || candidate->isDeclaration()) continue;

        bool hasStart = false;
        for (const auto& calledFunc : block.calledFunctions) {
            std::string simple = calledFunc;
            auto sep = calledFunc.rfind("::");
            if (sep != std::string::npos) simple = calledFunc.substr(sep + 2);
            if (simple == group->startFunc) hasStart = true;
        }
        if (!hasStart) continue;

        info.ownerFunc = candidate;

        // Scan owner's IR for calls to start/end functions
        for (auto& bb : *info.ownerFunc) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) continue;
                auto* callee = call->getCalledFunction();
                if (!callee) continue;
                for (const auto& coveredName : scope.coveredFunctions) {
                    auto mapIt2 = mapping.matched.find(coveredName);
                    if (mapIt2 == mapping.matched.end()) continue;
                    if (mapIt2->second != callee) continue;

                    std::string simpleName = coveredName;
                    auto sep2 = coveredName.rfind("::");
                    if (sep2 != std::string::npos) simpleName = coveredName.substr(sep2 + 2);

                    if (simpleName == group->startFunc && !info.startCall) info.startCall = call;
                    if (!group->endFunc.empty() && simpleName == group->endFunc && !info.endCall) info.endCall = call;
                }
            }
        }
        break;
    }

    if (!info.ownerFunc) return std::nullopt;

    // Build the set of IR functions in this scope
    for (const auto& funcName : scope.coveredFunctions) {
        auto it = mapping.matched.find(funcName);
        if (it != mapping.matched.end() && it->second) info.scopeFuncsIR.insert(it->second);
    }
    // Owner is also in scope
    info.scopeFuncsIR.insert(info.ownerFunc);

    // Scan allocation sites in all scope functions
    std::vector<AllocSite> allSites;
    for (auto* func : info.scopeFuncsIR) {
        if (func->isDeclaration()) continue;
        auto sites = findAllocSites(func);
        allSites.insert(allSites.end(), sites.begin(), sites.end());
    }

    // Escape analysis for each allocation site
    for (auto& site : allSites) {
        std::vector<llvm::CallInst*> frees;
        if (!escapesScope(site.call, info.scopeFuncsIR, frees)) {
            site.matchedFrees = std::move(frees);
            info.safeSites.push_back(site);
        } else {
            // Escaping site: the frees its walk reached belong to an
            // allocation the arena will NOT take over. Record them so the
            // erase step never removes a free that keeps a live (non-arena)
            // allocation honest — even when a converted site shares the
            // same free through a reassigned alloca slot or alias path.
            for (auto* f : frees) info.escapingFrees.insert(f);
        }
    }

    if (info.safeSites.empty()) return std::nullopt;

    return info;
}

/// Apply arena replacement to a function: insert arena create/destroy
/// and replace allocations with arena_alloc. Returns count of conversions.
int applyArenaReplacement(llvm::Module& module,
                          llvm::Function* ownerFunc,
                          llvm::CallInst* startCall,
                          llvm::CallInst* endCall,
                          const std::string& groupName,
                          std::vector<AllocSite>& safeSites,
                          const std::unordered_set<llvm::CallInst*>& escapingFrees,
                          const LifetimeConfig& config) {
    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* sizeTy = llvm::Type::getInt64Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // topo_arena_create(size_t) -> ptr
    auto* createFnTy = llvm::FunctionType::get(ptrTy, {sizeTy}, false);
    auto createFn = module.getOrInsertFunction("topo_arena_create", createFnTy);

    // topo_arena_alloc(ptr, size_t, size_t) -> ptr
    auto* allocFnTy = llvm::FunctionType::get(ptrTy, {ptrTy, sizeTy, sizeTy}, false);
    auto allocFn = module.getOrInsertFunction("topo_arena_alloc", allocFnTy);

    // topo_arena_destroy(ptr) -> void
    auto* destroyFnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    auto destroyFn = module.getOrInsertFunction("topo_arena_destroy", destroyFnTy);

    // topo_arena_bytes_used(ptr) -> size_t (close-event size payload)
    auto* bytesUsedFnTy = llvm::FunctionType::get(sizeTy, {ptrTy}, false);
    auto bytesUsedFn = module.getOrInsertFunction("topo_arena_bytes_used", bytesUsedFnTy);

    // Reusable pass-event wire (sized variant).
    // void topo_pass_event_emit_sized(const char* pass, const char* from,
    //         const char* to, const char* subject, int64_t bytes)
    // Plain external declared exactly like topo_arena_create above.
    // Resolution is guaranteed in the same link domain: whenever this
    // Pass runs, [lifetime] is enabled, so injectAutoLinkLibs() adds
    // -ltopo-pass-event alongside -ltopo-arena (see
    // topo-core/include/topo/Build/AutoLink.h). Mirrors the
    // AdaptiveDispatch "off ≡ no symbol" guarantee — the Pass only runs
    // (and thus only references this symbol) when the feature is on.
    auto* passEventTy =
        llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, sizeTy}, false);
    auto passEventFn = getOrDeclareFunc(module, "topo_pass_event_emit_sized", passEventTy);
    auto* peName = getOrCreateGlobalString(module, "LifetimeArenaPass", ".str.topo_pass.");
    auto* peScope = getOrCreateGlobalString(module, groupName, ".str.topo_pe.scope.");
    auto* peHeap = getOrCreateGlobalString(module, "heap", ".str.topo_pe.");
    auto* peArena = getOrCreateGlobalString(module, "arena", ".str.topo_pe.");
    auto* peFreed = getOrCreateGlobalString(module, "freed", ".str.topo_pe.");

    // One-shot guard so the open/close pass-events are emitted exactly
    // once per scope regardless of how many times the owner function
    // runs or how many return / landing-pad exits it has — the same
    // determinism contract AdaptiveDispatchPass uses for its dispatch-
    // point event (deterministic, CTest-reproducible record count).
    auto* i8Ty = llvm::Type::getInt8Ty(ctx);
    auto* peOpenGV = new llvm::GlobalVariable(
        module, i8Ty, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i8Ty, 0), "topo.arena.pe.open." + groupName);
    auto* peCloseGV = new llvm::GlobalVariable(
        module, i8Ty, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i8Ty, 0), "topo.arena.pe.close." + groupName);

    // Emit a one-shot guarded topo_pass_event_emit_sized() call right
    // before `anchor`. `flagGV` is the per-direction one-shot byte;
    // `from`/`to` carry the open/close moment; `bytesArg` is the arena-
    // size payload. SplitBlockAndInsertIfThen builds the
    // `if (flag == 0) { flag = 1; emit(...); }` diamond and rejoins —
    // the same deterministic at-most-once contract AdaptiveDispatchPass
    // applies at its dispatch point, robust to loops / multiple exits.
    auto emitArenaEvent = [&](llvm::Instruction* anchor,
                              llvm::GlobalVariable* flagGV,
                              llvm::Constant* from, llvm::Constant* to,
                              llvm::Value* bytesArg) {
        llvm::IRBuilder<> b(anchor);
        auto* flag = b.CreateLoad(i8Ty, flagGV);
        auto* first = b.CreateICmpEQ(flag, llvm::ConstantInt::get(i8Ty, 0));
        llvm::Instruction* thenTerm =
            llvm::SplitBlockAndInsertIfThen(first, anchor, /*Unreachable=*/false);
        llvm::IRBuilder<> eb(thenTerm);
        eb.CreateStore(llvm::ConstantInt::get(i8Ty, 1), flagGV);
        eb.CreateCall(passEventFn, {peName, from, to, peScope, bytesArg});
    };

    // Create a global arena pointer for this scope
    std::string globalName = "topo.arena." + groupName;
    auto* global = new llvm::GlobalVariable(
        module, ptrTy, false, llvm::GlobalValue::InternalLinkage, llvm::ConstantPointerNull::get(ptrTy), globalName);

    // Insert arena_create before the scope entry point
    {
        llvm::Instruction* insertPt = nullptr;
        if (startCall) {
            insertPt = startCall;
        } else {
            auto& entryBB = ownerFunc->getEntryBlock();
            insertPt = &*entryBB.getFirstInsertionPt();
        }

        llvm::IRBuilder<> builder(insertPt);
        auto* arenaSize = llvm::ConstantInt::get(sizeTy, config.defaultArenaSize);
        auto* arena = builder.CreateCall(createFn, {arenaSize});
        builder.CreateStore(arena, global);

        // Arena-open pass-event: heap -> arena, size = requested
        // capacity. Anchored at `insertPt` (the create+store were
        // inserted *before* it), so the guard diamond runs right after
        // the arena becomes live, fired at most once per program run.
        emitArenaEvent(insertPt, peOpenGV, peHeap, peArena, arenaSize);
    }

    // Insert arena_destroy at the scope's end boundary. When a declared
    // end-of-lifetime call exists, that is the single teardown point;
    // otherwise the arena spans the whole owner function and is torn down at
    // each return / landing-pad exit (see the endCall branch below).
    // Inject arena-close pass-event just before destroy: arena ->
    // freed, size = bytes actually used (queried while still live). The
    // one-shot guard for the close direction makes the record count
    // deterministic across multiple return / landing-pad exits.
    auto injectClose = [&](llvm::Instruction* destroyInsertPt) {
        llvm::IRBuilder<> builder(destroyInsertPt);
        auto* arena = builder.CreateLoad(ptrTy, global);
        auto* used = builder.CreateCall(bytesUsedFn, {arena});
        auto* destroyCall = builder.CreateCall(destroyFn, {arena});
        // Guard diamond anchored at the destroy call; emits before the
        // arena is torn down so `used` is valid.
        emitArenaEvent(destroyCall, peCloseGV, peArena, peFreed, used);
    };

    // Collect insertion points FIRST: injectClose splits blocks (creates
    // new BBs via SplitBlockAndInsertIfThen), so mutating the function
    // while range-iterating its block list would be UB.
    //
    // Per-block dedup: a single block can both start with a landing pad and
    // end with a return (`landingpad; …; ret`). Pushing a close point for
    // each would inject `topo_arena_destroy(arena)` twice in the same block
    // — a runtime double-free, since the destroy itself is not one-shot
    // guarded (only the pass-event emit is). At most one teardown per block.
    std::vector<llvm::Instruction*> closeInsertPts;
    std::unordered_set<llvm::BasicBlock*> closedBlocks;
    auto addClosePt = [&](llvm::Instruction* pt) {
        if (closedBlocks.insert(pt->getParent()).second) closeInsertPts.push_back(pt);
    };

    if (endCall) {
        // A declared end-of-lifetime call was found: the arena's lifetime
        // ends there, before the owner returns. Tear it down right after the
        // end call so allocations made after the declared end no longer hit a
        // live arena (the create..end range semantics). We deliberately do
        // NOT also inject teardown at returns/landing-pads — that would
        // destroy the same global arena twice (use-after-free / double-free).
        auto insertIt = endCall->getIterator();
        ++insertIt; // place destroy after the end call executes
        addClosePt(&*insertIt);
    } else {
        // No declared end point: the arena spans the whole owner function, so
        // tear it down on each exit (return) and exceptional cleanup
        // (landing pad). The return and landing-pad branches are made
        // mutually exclusive per block by addClosePt's dedup, so a block that
        // both lands and returns gets exactly one destroy.
        for (auto& bb : *ownerFunc) {
            auto* term = bb.getTerminator();
            if (llvm::isa<llvm::ReturnInst>(term)) {
                addClosePt(term);
                continue; // already closed this block — skip landing-pad check
            }
            // Handle landing pads (exception cleanup)
            if (auto* lp = llvm::dyn_cast<llvm::LandingPadInst>(&*bb.getFirstNonPHIIt())) {
                auto insertIt = lp->getIterator();
                ++insertIt;
                addClosePt(&*insertIt);
            }
        }
    }
    for (auto* pt : closeInsertPts)
        injectClose(pt);

    // Decide which sites are safe to convert BEFORE mutating anything.
    //
    // A free may legitimately be reached by more than one allocation:
    //   - two converted sites sharing it (reassigned alloca slot, or a
    //     `cond ? a : b` PHI/select merge) — erasing it once per site is a
    //     double-erase (use-after-free of the LLVM instruction), and a
    //     single retained free over an arena pointer is also wrong;
    //   - a converted site AND a non-converted (escaping) allocation
    //     sharing the slot — the free dynamically targets either an arena
    //     pointer or a real heap pointer, so neither erasing (leaks the
    //     escaping allocation) nor retaining (frees an arena pointer) is
    //     correct.
    //
    // A free is "ambiguous" if it is claimed by more than one safe site or
    // if it is also the genuine free of an escaping allocation. Converting
    // an allocation whose matched free is ambiguous cannot be done
    // correctly, so we DECLINE that site: it stays a real heap allocation
    // with its real free untouched. A missed arena optimization is
    // acceptable; a miscompile is not.
    std::unordered_map<llvm::CallInst*, int> freeClaimCount;
    for (auto& site : safeSites) {
        // matchedFrees is deduped per site by escapesScope, so each entry
        // here is a distinct converted-site claim.
        for (auto* f : site.matchedFrees) ++freeClaimCount[f];
    }

    auto siteIsConvertible = [&](const AllocSite& site) {
        for (auto* f : site.matchedFrees) {
            if (freeClaimCount[f] != 1) return false; // shared with another site
            if (escapingFrees.count(f)) return false; // also frees an escaping alloc
        }
        return true;
    };

    // Replace allocations and remove frees
    int converted = 0;
    std::unordered_set<llvm::CallInst*> erasedFrees;
    for (auto& site : safeSites) {
        if (!siteIsConvertible(site)) continue; // decline — leave heap alloc + free intact

        llvm::IRBuilder<> builder(site.call);
        auto* arena = builder.CreateLoad(ptrTy, global);
        auto* alignment = llvm::ConstantInt::get(sizeTy, 16);

        llvm::Value* totalSize;
        llvm::Value* memsetSize = nullptr; // bytes to zero (calloc only)
        if (site.isCalloc) {
            // calloc(count, elemSize) -> arena_alloc(arena, count*elemSize, 16)
            //
            // count*elemSize can overflow size_t. C requires calloc to return
            // NULL on that overflow; lowering with a plain CreateMul instead
            // wraps to a small product, so the arena hands back a short buffer
            // while the program believes it owns count*elemSize bytes — a
            // miscompile (under-allocation -> heap overflow on later writes).
            // Detect the overflow with llvm.umul.with.overflow and, when it
            // occurs, request SIZE_MAX (which topo_arena_alloc rejects ->
            // returns NULL, matching calloc's contract) and zero 0 bytes (so
            // the trailing memset on the NULL result is a well-defined no-op).
            auto* count = site.call->getArgOperand(0);
            auto* elemSize = site.call->getArgOperand(1);
            if (count->getType() != sizeTy) count = builder.CreateZExt(count, sizeTy);
            if (elemSize->getType() != sizeTy) elemSize = builder.CreateZExt(elemSize, sizeTy);

            auto* umulOvf = llvm::Intrinsic::getOrInsertDeclaration(
                &module, llvm::Intrinsic::umul_with_overflow, {sizeTy});
            auto* mulRes = builder.CreateCall(umulOvf, {count, elemSize});
            auto* product = builder.CreateExtractValue(mulRes, 0);
            auto* overflow = builder.CreateExtractValue(mulRes, 1);

            auto* sizeMax = llvm::ConstantInt::get(sizeTy, ~uint64_t(0));
            auto* zero = llvm::ConstantInt::get(sizeTy, 0);
            totalSize = builder.CreateSelect(overflow, sizeMax, product);
            memsetSize = builder.CreateSelect(overflow, zero, product);
        } else {
            totalSize = site.size;
            if (totalSize->getType() != sizeTy) totalSize = builder.CreateZExt(totalSize, sizeTy);
        }

        auto* newAlloc = builder.CreateCall(allocFn, {arena, totalSize, alignment});

        // calloc zeroes memory — insert memset (0 bytes when the size
        // multiply overflowed, so this stays a no-op on the NULL result).
        if (site.isCalloc) {
            builder.CreateMemSet(newAlloc, builder.getInt8(0), memsetSize, llvm::MaybeAlign(16));
        }

        site.call->replaceAllUsesWith(newAlloc);
        site.call->eraseFromParent();

        // Erase this site's matched free/delete calls. Each is claimed by
        // exactly this site and frees no escaping allocation (verified by
        // siteIsConvertible), so erasing once is correct. erasedFrees is a
        // final belt-and-suspenders guard against any residual aliasing.
        for (auto* freeCall : site.matchedFrees) {
            if (!erasedFrees.insert(freeCall).second) continue;
            freeCall->eraseFromParent();
        }

        ++converted;
    }

    return converted;
}

} // anonymous namespace

int LifetimeArenaPass::run(llvm::Module& module,
                           const SymbolTable& symbols,
                           const SymbolMapping& mapping,
                           const LifetimeConfig& config) {
    auto analysis = analysis::analyzeLifetimes(symbols);
    if (analysis.scopes.empty()) return 0;

    int converted = 0;

    for (const auto& [groupName, scope] : analysis.scopes) {
        auto scopeInfo = discoverScope(groupName, scope, symbols, mapping);
        if (!scopeInfo) continue;

        converted += applyArenaReplacement(
            module, scopeInfo->ownerFunc, scopeInfo->startCall, scopeInfo->endCall, groupName,
            scopeInfo->safeSites, scopeInfo->escapingFrees, config);
    }

    if (converted > 0) {
        // At least one arena scope was wired in — the emitted IR now
        // calls topo_arena_create / topo_arena_alloc / topo_arena_destroy
        // against libtopo-arena. Wire the one-time ABI-version check
        // matching the pattern in topo-llvm/runtime/ABI-COMPAT.md.
        injectAbiCheckCtor(module, "arena", "topo_arena_version", abi::kArenaVersion);
    }

    return converted;
}

std::vector<std::string> LifetimeArenaPass::collectOwnerFunctions(llvm::Module& module,
                                                                  const SymbolTable& symbols,
                                                                  const SymbolMapping& mapping) {
    (void)module;
    std::vector<std::string> names;
    auto analysis = analysis::analyzeLifetimes(symbols);
    if (analysis.scopes.empty()) return names;

    std::unordered_set<llvm::Function*> seen;
    for (const auto& [groupName, scope] : analysis.scopes) {
        auto scopeInfo = discoverScope(groupName, scope, symbols, mapping);
        if (!scopeInfo) continue; // no owner, no safe sites, or no lifetime group entry
        if (!scopeInfo->ownerFunc) continue;
        if (!seen.insert(scopeInfo->ownerFunc).second) continue;
        names.push_back(scopeInfo->ownerFunc->getName().str());
    }
    return names;
}

} // namespace topo
