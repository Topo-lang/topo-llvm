// @category: OPT
#include "topo/Transforms/ReturnSpecializationPass.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace topo {

namespace {

/// Return the sret parameter (param 0 with sret attribute), or nullptr.
llvm::Argument* getSretParam(llvm::Function& func) {
    if (func.arg_size() == 0) return nullptr;
    auto* arg = func.getArg(0);
    return arg->hasStructRetAttr() ? arg : nullptr;
}

/// Get the struct type from the sret attribute.
llvm::StructType* getSretStructType(llvm::Argument* sretArg) {
    auto* ty = sretArg->getParamStructRetType();
    return llvm::dyn_cast_or_null<llvm::StructType>(ty);
}

/// Recursively collect store instructions reachable through a GEP chain.
void collectStoresFromGEP(llvm::Value* v, std::vector<llvm::Instruction*>& stores) {
    for (auto* user : v->users()) {
        if (auto* si = llvm::dyn_cast<llvm::StoreInst>(user)) {
            if (si->getPointerOperand() == v) stores.push_back(si);
        } else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
            collectStoresFromGEP(gep, stores);
        }
    }
}

/// Find all store instructions in the function that write to
/// sretParam[0][fieldIdx] via GEP chains.
std::vector<llvm::Instruction*> collectFieldStores(llvm::Argument* sretParam, unsigned fieldIdx) {
    std::vector<llvm::Instruction*> stores;

    for (auto* user : sretParam->users()) {
        auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
        if (!gep || gep->getNumIndices() < 2) continue;

        auto* idx0 = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
        auto* idx1 = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
        if (!idx0 || idx0->getZExtValue() != 0) continue;
        if (!idx1 || idx1->getZExtValue() != fieldIdx) continue;

        collectStoresFromGEP(gep, stores);
    }

    return stores;
}

/// Collect insertvalue instructions that build the return aggregate for a
/// specific field index. These are candidates for elimination when the
/// field is dead.
std::vector<llvm::Instruction*> collectInsertValueStores(llvm::Function& func, unsigned fieldIdx) {
    std::vector<llvm::Instruction*> insts;

    for (auto& BB : func) {
        for (auto& I : BB) {
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
                // Walk the insertvalue chain back from the return value
                llvm::Value* retVal = ret->getReturnValue();
                if (!retVal) continue;

                // Follow insertvalue chain to find the one inserting fieldIdx
                llvm::Value* current = retVal;
                while (auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(current)) {
                    if (iv->getNumIndices() == 1 && iv->getIndices()[0] == fieldIdx) {
                        insts.push_back(iv);
                    }
                    current = iv->getAggregateOperand();
                }
            }
        }
    }

    return insts;
}

// ============================================================================
// SymbolTable-driven analysis
// ============================================================================

/// Build a mapping from qualified function name to the set of live field
/// indices, derived from CallSiteInfo.usedReturns and FunctionSymbol's
/// `with returns(...)` ceiling in the SymbolTable.
///
/// For each callee with CallSiteInfo entries, union all usedReturns across
/// call sites and map return parameter names to field indices using the
/// FunctionSymbol's returnParams ordering.
///
/// Additionally: when a FunctionSymbol declares a selective-return ceiling
/// via `with returns(...)`, seed its entry with **exactly** the ceiling.
/// This covers functions that have no recorded call sites (e.g., library
/// entry points, or direct callers outside pipelines that never populate
/// CallSiteInfo).  The per-call-site union still applies on top.
std::unordered_map<std::string, std::set<unsigned>> buildSymbolTableDemand(const SymbolTable& symbols) {
    std::unordered_map<std::string, std::set<unsigned>> demand;

    auto paramNameToIdx = [](const FunctionSymbol& fs, const std::string& name) -> int {
        for (unsigned i = 0; i < fs.returnParams.size(); ++i) {
            if (fs.returnParams[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };

    // Pass 1: seed from every multi-return function's declared ceiling.
    // Functions without a ceiling get no seed (fall through to the call-site
    // union or the pass's conservative-keep-all default).
    for (const auto& [qname, fs] : symbols.functions()) {
        if (!fs.isMultiReturn) continue;
        if (!fs.hasUsedReturnsClause) continue;
        auto& liveSet = demand[qname];
        for (const auto& name : fs.usedReturns) {
            int idx = paramNameToIdx(fs, name);
            if (idx >= 0) liveSet.insert(static_cast<unsigned>(idx));
        }
    }

    // Pass 2: union per-call-site demand.
    for (const auto& cs : symbols.callSites()) {
        const auto* funcSym = symbols.findFunction(cs.callee);
        if (!funcSym || !funcSym->isMultiReturn) continue;

        auto& liveSet = demand[cs.callee];
        if (cs.usedReturns.empty() && !funcSym->hasUsedReturnsClause) {
            // No usage info and no ceiling -> all live (Full style).
            for (unsigned i = 0; i < funcSym->returnParams.size(); ++i)
                liveSet.insert(i);
            continue;
        }

        for (const auto& name : cs.usedReturns) {
            int idx = paramNameToIdx(*funcSym, name);
            if (idx >= 0) liveSet.insert(static_cast<unsigned>(idx));
        }
    }

    return demand;
}

} // anonymous namespace

int ReturnSpecializationPass::run(llvm::Module& module,
                                  const std::vector<VisibilityEntry>& /*entries*/,
                                  const SymbolMapping& mapping,
                                  const SymbolTable* symbols,
                                  backend::ReturnSpecializationReport* report) {
    int eliminatedCount = 0;

    // Pre-compute SymbolTable demand if available
    std::unordered_map<std::string, std::set<unsigned>> symbolDemand;
    if (symbols) symbolDemand = buildSymbolTableDemand(*symbols);

    // Build reverse mapping: llvm::Function* -> qualified name
    std::unordered_map<llvm::Function*, std::string> funcToQualName;
    for (const auto& [name, func] : mapping.matched)
        funcToQualName[func] = name;

    for (auto& func : module) {
        if (func.isDeclaration() || !func.hasInternalLinkage()) continue;
        if (func.use_empty()) continue;

        // Determine return mode: sret pointer or direct struct return
        auto* sretParam = getSretParam(func);
        llvm::StructType* sty = nullptr;
        bool isSretMode = false;

        if (sretParam) {
            sty = getSretStructType(sretParam);
            isSretMode = true;
        } else {
            // Check for direct struct return
            auto* retTy = func.getReturnType();
            sty = llvm::dyn_cast<llvm::StructType>(retTy);
        }

        if (!sty) continue;

        unsigned numFields = sty->getNumElements();
        if (numFields == 0) continue;

        // Step 1: Determine which fields are live across all callers.
        std::set<unsigned> liveFields;
        bool conservative = false;

        // Try SymbolTable demand first (more precise)
        bool usedSymbolDemand = false;
        if (symbols && !symbolDemand.empty()) {
            auto it = funcToQualName.find(&func);
            if (it != funcToQualName.end()) {
                auto demandIt = symbolDemand.find(it->second);
                if (demandIt != symbolDemand.end()) {
                    liveFields = demandIt->second;
                    usedSymbolDemand = true;
                }
            }
        }

        // Without SymbolTable demand info, conservatively keep all fields.
        // IR-level use-chain analysis duplicates LLVM's alias analysis + DSE.
        if (!usedSymbolDemand) {
            conservative = true;
        }

        if (conservative || liveFields.size() == numFields) continue;

        if (report) {
            backend::ReturnSpecializationEntry entry;
            auto qnameIt = funcToQualName.find(&func);
            entry.hostFunction = (qnameIt != funcToQualName.end())
                                 ? qnameIt->second
                                 : func.getName().str();
            for (unsigned i = 0; i < numFields; ++i) {
                if (liveFields.count(i)) entry.keptFieldIndices.push_back(static_cast<int>(i));
                else                     entry.eliminatedFieldIndices.push_back(static_cast<int>(i));
            }
            report->entries.push_back(std::move(entry));
        }

        // Step 2: Replace dead field values with undef, letting LLVM DSE
        // handle the actual store elimination.
        if (isSretMode) {
            for (unsigned i = 0; i < numFields; ++i) {
                if (liveFields.count(i)) continue;

                auto deadStores = collectFieldStores(sretParam, i);
                for (auto* inst : deadStores) {
                    // Replace stored value with undef so LLVM DSE can
                    // identify and remove the dead store
                    auto* store = llvm::cast<llvm::StoreInst>(inst);
                    auto* undefVal = llvm::UndefValue::get(store->getValueOperand()->getType());
                    store->setOperand(0, undefVal);
                    ++eliminatedCount;
                }
            }
        } else {
            // Direct struct return: replace dead insertvalue with undef
            for (unsigned i = 0; i < numFields; ++i) {
                if (liveFields.count(i)) continue;

                auto deadInserts = collectInsertValueStores(func, i);
                for (auto* inst : deadInserts) {
                    auto* iv = llvm::cast<llvm::InsertValueInst>(inst);
                    // Replace the inserted value with undef to allow DCE
                    // of the computation that produced it
                    auto* undefVal = llvm::UndefValue::get(iv->getInsertedValueOperand()->getType());
                    iv->setOperand(1, undefVal);
                    ++eliminatedCount;
                }
            }
        }
    }

    return eliminatedCount;
}

} // namespace topo
