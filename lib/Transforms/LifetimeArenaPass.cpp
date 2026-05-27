// @category: OPT
#include "topo/Transforms/LifetimeArenaPass.h"
#include "topo/Analysis/LifetimeAnalysis.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <string>
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
                    // Track loads from this alloca (through any GEP) so we
                    // can find matching free() calls that reload the pointer.
                    std::vector<llvm::Value*> allocaWork = {allocaBase};
                    std::unordered_set<llvm::Value*> allocaVisited;
                    while (!allocaWork.empty()) {
                        auto* av = allocaWork.back();
                        allocaWork.pop_back();
                        if (!allocaVisited.insert(av).second) continue;
                        for (auto* au : av->users()) {
                            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(au))
                                worklist.push_back(ld);
                            else if (llvm::isa<llvm::GetElementPtrInst>(au))
                                allocaWork.push_back(au);
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
                // free/delete: matched free, safe
                if (isFreeFunction(calleeName)) {
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
                          const std::string& groupName,
                          std::vector<AllocSite>& safeSites,
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

    // Insert arena_destroy before every ReturnInst in the owner function
    // and after every LandingPadInst
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
    std::vector<llvm::Instruction*> closeInsertPts;
    for (auto& bb : *ownerFunc) {
        auto* term = bb.getTerminator();
        if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(term)) {
            closeInsertPts.push_back(ret);
        }
        // Handle landing pads (exception cleanup)
        if (auto* lp = llvm::dyn_cast<llvm::LandingPadInst>(&*bb.getFirstNonPHIIt())) {
            auto insertIt = lp->getIterator();
            ++insertIt;
            closeInsertPts.push_back(&*insertIt);
        }
    }
    for (auto* pt : closeInsertPts)
        injectClose(pt);

    // Replace allocations and remove frees
    int converted = 0;
    for (auto& site : safeSites) {
        llvm::IRBuilder<> builder(site.call);
        auto* arena = builder.CreateLoad(ptrTy, global);
        auto* alignment = llvm::ConstantInt::get(sizeTy, 16);

        llvm::Value* totalSize;
        if (site.isCalloc) {
            // calloc(count, elemSize) -> arena_alloc(arena, count*elemSize, 16)
            auto* count = site.call->getArgOperand(0);
            auto* elemSize = site.call->getArgOperand(1);
            if (count->getType() != sizeTy) count = builder.CreateZExt(count, sizeTy);
            if (elemSize->getType() != sizeTy) elemSize = builder.CreateZExt(elemSize, sizeTy);
            totalSize = builder.CreateMul(count, elemSize);
        } else {
            totalSize = site.size;
            if (totalSize->getType() != sizeTy) totalSize = builder.CreateZExt(totalSize, sizeTy);
        }

        auto* newAlloc = builder.CreateCall(allocFn, {arena, totalSize, alignment});

        // calloc zeroes memory — insert memset
        if (site.isCalloc) {
            builder.CreateMemSet(newAlloc, builder.getInt8(0), totalSize, llvm::MaybeAlign(16));
        }

        site.call->replaceAllUsesWith(newAlloc);
        site.call->eraseFromParent();

        // Erase matched free/delete calls
        for (auto* freeCall : site.matchedFrees) {
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
            module, scopeInfo->ownerFunc, scopeInfo->startCall, groupName, scopeInfo->safeSites, config);
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
