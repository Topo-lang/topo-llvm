#include "topo/Backend/IREmbed.h"
#include "topo/Sema/TypeRegistry.h"

#include <llvm/ADT/StringSet.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <nlohmann/json.hpp>

#include <set>
#include <sstream>

namespace topo {

namespace {

/// Recursively collect all functions called by the given function.
void collectCallees(llvm::Function* func, std::set<llvm::Function*>& collected) {
    if (!func || func->isDeclaration() || collected.count(func)) return;
    collected.insert(func);

    for (auto& BB : *func) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) collectCallees(callee, collected);
            }
        }
    }
}

/// Get the section name for the current platform.
std::string getSectionName(const std::string& baseName, const llvm::Module& module) {
    auto tripleStr = module.getTargetTriple().str();
    if (tripleStr.find("win32") != std::string::npos || tripleStr.find("windows") != std::string::npos ||
        tripleStr.find("mingw") != std::string::npos) {
        return baseName + "$";
    }
    if (tripleStr.find("apple") != std::string::npos || tripleStr.find("darwin") != std::string::npos ||
        tripleStr.find("macos") != std::string::npos) {
        // Mach-O requires "segment,section" format; convert .topo_ir -> __DATA,__topo_ir
        std::string sectName = baseName;
        if (!sectName.empty() && sectName[0] == '.')
            sectName = "__" + sectName.substr(1);
        else
            sectName = "__" + sectName;
        return "__DATA," + sectName;
    }
    return baseName;
}

/// Add a global variable to llvm.used to prevent DCE.
void addToUsed(llvm::Module& module, llvm::GlobalVariable* gv) {
    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);

    // Get or create @llvm.used
    auto* existing = module.getGlobalVariable("llvm.used");
    std::vector<llvm::Constant*> entries;

    if (existing) {
        auto* init = existing->getInitializer();
        if (auto* arr = llvm::dyn_cast<llvm::ConstantArray>(init)) {
            for (unsigned i = 0; i < arr->getNumOperands(); ++i)
                entries.push_back(arr->getOperand(i));
        }
        existing->eraseFromParent();
    }

    entries.push_back(llvm::ConstantExpr::getPointerCast(gv, ptrTy));

    auto* arrTy = llvm::ArrayType::get(ptrTy, entries.size());
    auto* arrInit = llvm::ConstantArray::get(arrTy, entries);

    auto* usedGV =
        new llvm::GlobalVariable(module, arrTy, false, llvm::GlobalValue::AppendingLinkage, arrInit, "llvm.used");
    usedGV->setSection("llvm.metadata");
}

} // anonymous namespace

std::vector<uint8_t> IREmbed::serializePipelineIR(const llvm::Module& module,
                                                  const SymbolTable& symbols,
                                                  const SymbolMapping& mapping) {
    // Collect all pipeline functions and their transitive callees
    std::set<llvm::Function*> funcsToClone;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline) continue;

        auto it = mapping.matched.find(logicBlock.qualifiedName);
        if (it == mapping.matched.end()) continue;

        collectCallees(it->second, funcsToClone);
    }

    if (funcsToClone.empty()) return {};

    // Clone the entire module, then remove functions we don't need
    llvm::ValueToValueMapTy vmap;
    auto clonedModule = llvm::CloneModule(module, vmap);

    // Build a hash set keyed by name so the prune walk is O(N) rather
    // than O(N*M). For a project with hundreds of pipeline callees and
    // thousands of module functions the linear scan was a real
    // scalability cliff (see issue
    // topo-llvm-ireembed-quadratic-name-match).
    llvm::StringSet<> keepNames;
    for (auto* orig : funcsToClone) keepNames.insert(orig->getName());

    // Remove functions not in our set
    std::vector<llvm::Function*> toRemove;
    for (auto& func : *clonedModule) {
        if (func.isDeclaration()) continue;
        if (!keepNames.contains(func.getName())) toRemove.push_back(&func);
    }
    for (auto* func : toRemove) {
        func->replaceAllUsesWith(llvm::UndefValue::get(func->getType()));
        func->eraseFromParent();
    }

    // Serialize to bitcode
    std::string buf;
    llvm::raw_string_ostream os(buf);
    llvm::WriteBitcodeToFile(*clonedModule, os);
    os.flush();

    return std::vector<uint8_t>(buf.begin(), buf.end());
}

std::vector<uint8_t> IREmbed::serializePreCodegenIR(const llvm::Module& module,
                                                    const SymbolTable& symbols,
                                                    const SymbolMapping& mapping) {
    // Collect the pipeline stub + pipeline_placeholder (with bodies).
    // Stage functions are kept as declarations only — their bodies would
    // pull in transitive callees (std library, RTTI, string constants)
    // that bloat the JIT module and create unresolvable symbol references.
    // At JIT time, stage functions are resolved from the host binary.
    std::set<llvm::Function*> funcsToKeepBody;
    std::set<llvm::Function*> funcsToKeepDecl;

    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline) continue;

        // Include the stub function itself and its callees
        // (the stub calls pipeline_placeholder which must be preserved)
        auto stubIt = mapping.matched.find(logicBlock.qualifiedName);
        if (stubIt != mapping.matched.end() && stubIt->second) collectCallees(stubIt->second, funcsToKeepBody);

        // Stage functions: declaration only (no transitive callees)
        for (const auto& calledFunc : logicBlock.calledFunctions) {
            auto it = mapping.matched.find(calledFunc);
            if (it != mapping.matched.end() && it->second) funcsToKeepDecl.insert(it->second);
        }
    }

    if (funcsToKeepBody.empty() && funcsToKeepDecl.empty()) return {};

    // Clone the entire module, then prune
    llvm::ValueToValueMapTy vmap;
    auto clonedModule = llvm::CloneModule(module, vmap);

    // Hash the keep sets by name so the prune walk is O(N) rather than
    // O(N*M) — same fix shape as serializePipelineIR above.
    llvm::StringSet<> keepBodyNames;
    for (auto* orig : funcsToKeepBody) keepBodyNames.insert(orig->getName());

    llvm::StringSet<> keepDeclNames;
    for (auto* orig : funcsToKeepDecl) keepDeclNames.insert(orig->getName());

    // First pass: strip bodies from declaration-only functions,
    // and collect functions to remove entirely.
    std::vector<llvm::Function*> toRemove;
    for (auto& func : *clonedModule) {
        if (func.isDeclaration()) continue;

        auto name = func.getName();
        if (keepBodyNames.contains(name)) continue;

        if (keepDeclNames.contains(name)) {
            func.deleteBody();
            func.setLinkage(llvm::GlobalValue::ExternalLinkage);
            continue;
        }

        toRemove.push_back(&func);
    }
    for (auto* func : toRemove) {
        func->replaceAllUsesWith(llvm::UndefValue::get(func->getType()));
        func->eraseFromParent();
    }

    // Remove llvm.used / llvm.compiler.used (these keep globals alive
    // that we want to clean up — RTTI, vtables, etc.)
    if (auto* used = clonedModule->getNamedGlobal("llvm.used")) used->eraseFromParent();
    if (auto* compUsed = clonedModule->getNamedGlobal("llvm.compiler.used")) compUsed->eraseFromParent();

    // Remove global_ctors/dtors (from the original module, not needed)
    if (auto* ctors = clonedModule->getNamedGlobal("llvm.global_ctors")) ctors->eraseFromParent();
    if (auto* dtors = clonedModule->getNamedGlobal("llvm.global_dtors")) dtors->eraseFromParent();

    // Remove aliases
    std::vector<llvm::GlobalAlias*> aliasesToRemove;
    for (auto& alias : clonedModule->aliases())
        aliasesToRemove.push_back(&alias);
    for (auto* alias : aliasesToRemove) {
        alias->replaceAllUsesWith(llvm::UndefValue::get(alias->getType()));
        alias->eraseFromParent();
    }

    // Aggressively remove RTTI/EH global variables. On MSVC targets,
    // clang emits RTTI descriptors (??_R*), catch type info (_CT??_R*),
    // and references to ??_7type_info@@6B@ (the type_info vtable).
    // These form circular reference chains so use_empty() never returns
    // true. Break cycles explicitly before the iterative cleanup.
    {
        std::vector<llvm::GlobalVariable*> rttiGVs;
        for (auto& gv : clonedModule->globals()) {
            auto name = gv.getName();
            if (name.starts_with("??_R") || name.starts_with("_CT??_R") || name.starts_with("??_7type_info"))
                rttiGVs.push_back(&gv);
        }
        for (auto* gv : rttiGVs) {
            gv->replaceAllUsesWith(llvm::UndefValue::get(gv->getType()));
            gv->eraseFromParent();
        }
    }

    // Iteratively remove unused global variables (string constants, etc.)
    // Multiple rounds needed because removing one GV may free uses of another.
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<llvm::GlobalVariable*> gvsToRemove;
        for (auto& gv : clonedModule->globals()) {
            if (gv.use_empty() && !gv.getName().starts_with("llvm.")) gvsToRemove.push_back(&gv);
        }
        for (auto* gv : gvsToRemove) {
            gv->eraseFromParent();
            changed = true;
        }
    }

    // Serialize to bitcode
    std::string buf;
    llvm::raw_string_ostream os(buf);
    llvm::WriteBitcodeToFile(*clonedModule, os);
    os.flush();

    return std::vector<uint8_t>(buf.begin(), buf.end());
}

std::string IREmbed::serializeMetadata(const SymbolTable& symbols,
                                       const ParallelConfig& parallelCfg,
                                       const AdaptiveConfig* adaptiveCfg) {
    nlohmann::json root;

    // Serialize pipeline DAG structures
    nlohmann::json pipelines = nlohmann::json::array();
    for (const auto& [name, logicBlock] : symbols.logicBlocks()) {
        if (!logicBlock.isPipeline || !logicBlock.pipelineAnalysis) continue;

        nlohmann::json pipeline;
        pipeline["name"] = logicBlock.qualifiedName;
        pipeline["simpleName"] = logicBlock.simpleName;

        // Edges
        nlohmann::json edges = nlohmann::json::array();
        for (const auto& edge : logicBlock.edges) {
            edges.push_back({{"source", edge.source}, {"target", edge.target}});
        }
        pipeline["edges"] = edges;

        // Stages
        const auto& analysis = *logicBlock.pipelineAnalysis;
        nlohmann::json stages;
        for (const auto& [node, stage] : analysis.stages)
            stages[node] = stage;
        pipeline["stages"] = stages;

        // Source and terminal nodes
        pipeline["sourceNodes"] = analysis.sourceNodes;
        pipeline["terminalNode"] = analysis.terminalNode;
        pipeline["terminalType"] = analysis.terminalType;

        // Demand info
        nlohmann::json demand;
        for (const auto& [node, fields] : analysis.demand) {
            nlohmann::json fieldArray = nlohmann::json::array();
            for (const auto& f : fields)
                fieldArray.push_back(f);
            demand[node] = fieldArray;
        }
        pipeline["demand"] = demand;

        // calledFunctions: needed by JIT to rebuild SymbolTable
        nlohmann::json calledFuncs = nlohmann::json::array();
        for (const auto& cf : logicBlock.calledFunctions)
            calledFuncs.push_back(cf);
        pipeline["calledFunctions"] = calledFuncs;

        // Data-aware optimization hints per node
        nlohmann::json nodeHints;
        for (const auto& cf : logicBlock.calledFunctions) {
            auto* fnSym = symbols.findFunction(cf);
            if (!fnSym) continue;
            nlohmann::json hint;
            if (fnSym->cardinality) {
                hint["cardinality_min"] = fnSym->cardinality->min;
                hint["cardinality_max"] = fnSym->cardinality->max;
            }
            if (fnSym->accessPattern != AccessPattern::None) {
                const char* patternName = "none";
                switch (fnSym->accessPattern) {
                case AccessPattern::Streaming: patternName = "streaming"; break;
                case AccessPattern::Random: patternName = "random"; break;
                case AccessPattern::Tiled: patternName = "tiled"; break;
                case AccessPattern::GatherScatter: patternName = "gather_scatter"; break;
                default: break;
                }
                hint["access_pattern"] = patternName;
                if (fnSym->accessPattern == AccessPattern::Tiled && fnSym->tiledSize > 0)
                    hint["tile_size"] = fnSym->tiledSize;
            }
            if (!hint.empty()) nodeHints[cf] = hint;
        }
        if (!nodeHints.empty()) pipeline["hints"] = nodeHints;

        pipelines.push_back(pipeline);
    }
    root["pipelines"] = pipelines;

    // Serialize function ownership metadata (for JIT re-specialization)
    nlohmann::json functions = nlohmann::json::object();
    for (const auto& [name, fn] : symbols.functions()) {
        bool hasOwnership = false;
        for (const auto& param : fn.params) {
            if (param.type.ownership != OwnershipKind::None) {
                hasOwnership = true;
                break;
            }
        }
        if (!hasOwnership) continue;

        nlohmann::json params = nlohmann::json::array();
        for (const auto& param : fn.params) {
            nlohmann::json p;
            p["name"] = param.name;
            if (param.type.ownership != OwnershipKind::None) {
                p["ownership"] = ownershipKindName(param.type.ownership);
            }
            params.push_back(p);
        }
        functions[fn.qualifiedName] = {{"params", params}};
    }
    if (!functions.empty()) {
        root["functions"] = functions;
    }

    // Serialize parallel config
    nlohmann::json parCfg;
    parCfg["enabled"] = parallelCfg.isEnabled();
    parCfg["instrument"] = parallelCfg.instrument;
    parCfg["exclude"] = parallelCfg.exclude;
    root["parallel"] = parCfg;

    // Serialize adaptive config (if enabled)
    if (adaptiveCfg) {
        nlohmann::json adpCfg;
        adpCfg["enabled"] = adaptiveCfg->isEnabled();
        root["adaptive"] = adpCfg;
    }

    // Serialize function binding targets
    nlohmann::json bindings = nlohmann::json::object();
    for (const auto& [name, fn] : symbols.functions()) {
        if (fn.bindingTarget) bindings[fn.qualifiedName] = *fn.bindingTarget;
    }
    if (!bindings.empty()) {
        root["bindings"] = bindings;
    }

    // Serialize type adapters (adapt for abstract type names)
    nlohmann::json typeAdapters = nlohmann::json::array();
    for (const auto& adp : symbols.adapts()) {
        // Check if this adapt targets an abstract type name
        auto kind = TypeRegistry::classifyAbstractName(adp.constraintName);
        if (!kind) continue;

        nlohmann::json adapter;
        adapter["abstractType"] = adp.constraintName;
        adapter["targetType"] = adp.targetType.toString();
        for (const auto& m : adp.mappings) {
            adapter[m.memberName] = m.targetName;
        }
        typeAdapters.push_back(adapter);
    }
    if (!typeAdapters.empty()) {
        root["typeAdapters"] = typeAdapters;
    }

    return root.dump(2);
}

void IREmbed::embed(llvm::Module& module, const std::vector<uint8_t>& bitcode, const std::string& metadata) {
    auto& ctx = module.getContext();

    // Embed bitcode as .topo_ir section.
    //
    // Linkage choice: ExternalLinkage (not PrivateLinkage).
    // PrivateLinkage would rename the symbol to an assembler-local name
    // (e.g., `l_topo_embedded_ir` on Mach-O, `.Ltopo_embedded_ir` on ELF)
    // that is dropped from the final binary's symbol table. The JIT engine
    // locates the payload by section name, not symbol, so either linkage
    // works at runtime — but tests and tooling (llvm-nm, Equivalence tests)
    // rely on the externally visible symbol to prove IREmbed fired.
    // The `llvm.used` entry keeps the global alive through optimization.
    if (!bitcode.empty()) {
        auto* dataType = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), bitcode.size());
        auto* data = llvm::ConstantDataArray::get(ctx, bitcode);

        auto* gv = new llvm::GlobalVariable(
            module, dataType, true, llvm::GlobalValue::ExternalLinkage, data, "topo_embedded_ir");
        gv->setSection(getSectionName(".topo_ir", module));
        gv->setAlignment(llvm::Align(1));
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
        gv->setVisibility(llvm::GlobalValue::HiddenVisibility);

        addToUsed(module, gv);
    }

    // Embed metadata as .tp_meta section
    // Note: PE section names are limited to 8 characters. ".topo_meta" (10 chars)
    // gets truncated by the linker, so we use ".tp_meta" (8 chars) instead.
    // Linkage: ExternalLinkage + Hidden visibility — same rationale as above.
    if (!metadata.empty()) {
        auto* strConst = llvm::ConstantDataArray::getString(ctx, metadata, true);

        auto* gv = new llvm::GlobalVariable(
            module, strConst->getType(), true, llvm::GlobalValue::ExternalLinkage, strConst, "topo_embedded_meta");
        gv->setSection(getSectionName(".tp_meta", module));
        gv->setAlignment(llvm::Align(1));
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
        gv->setVisibility(llvm::GlobalValue::HiddenVisibility);

        addToUsed(module, gv);
    }
}

} // namespace topo
