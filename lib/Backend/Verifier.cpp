#include "topo/Backend/Verifier.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/TypeRegistry.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <unordered_map>
#include <unordered_set>

namespace topo {

Verifier::Verifier(DiagnosticEngine& diag, const SymbolTable& symbols) : diag_(diag), symbols_(symbols) {}

VerifyResult Verifier::verify(const llvm::Module& module, const SymbolMapping& mapping) {
    VerifyResult result;

    checkPublicSymbols(mapping, result);
    checkLogicBlockConsistency(module, mapping, result);
    checkSignatures(mapping, result);
    checkClassMembers(mapping, result);
    checkStageOrdering(module, mapping, result);
    checkPipelineEdges(module, mapping, result);
    checkTemplateInstantiations(module, mapping, result);
    checkConstraintSatisfaction(module, mapping, result);
    checkStageParallelSafety(module, mapping, result);

    return result;
}

VerifyResult Verifier::verify(const llvm::Module& module,
                              const SymbolMapping& mapping,
                              const std::vector<VisibilityEntry>& entries) {
    VerifyResult result;

    checkPublicSymbols(mapping, result);
    checkLogicBlockConsistency(module, mapping, result);
    checkSignatures(mapping, result);
    checkConstConsistency(mapping, entries, result);
    checkClassMembers(mapping, result);
    checkStageOrdering(module, mapping, result);
    checkPipelineEdges(module, mapping, result);
    checkTemplateInstantiations(module, mapping, result);
    checkConstraintSatisfaction(module, mapping, result);
    checkStageParallelSafety(module, mapping, result);

    return result;
}

void Verifier::checkPublicSymbols(const SymbolMapping& mapping, VerifyResult& result) {
    // Build a set of all names that were part of the visibility collection.
    // Functions in symbols_.functions() but not in this set were never
    // collected (e.g., inside comptime blocks) and can't be verified.
    std::unordered_set<std::string> collectedNames;
    for (const auto& [name, _] : mapping.matched) {
        collectedNames.insert(name);
    }
    for (const auto& name : mapping.unmatchedTopo) {
        collectedNames.insert(name);
    }

    for (const auto& [name, sym] : symbols_.functions()) {
        if (sym.visibility != Visibility::Public) continue;

        // Skip template function declarations — uninstantiated templates
        // won't appear in IR. Template instantiations are verified
        // separately in checkTemplateInstantiations().
        if (!sym.templateParams.empty()) continue;

        // Skip functions that belong to a template class — their IR
        // symbols only exist for instantiated specializations.
        bool inTemplateClass = false;
        for (const auto& [clsName, cls] : symbols_.classSymbols()) {
            if (!cls.templateParams.empty()) {
                std::string prefix = clsName + "::";
                if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                    inTemplateClass = true;
                    break;
                }
            }
        }
        if (inTemplateClass) continue;

        // Skip functions whose name contains '<' — these are template
        // specialization members (e.g., TypeTraits<Int>::is_integral).
        // Their names use Topo aliases that differ from IR demangled names.
        // Template instantiations are verified in checkTemplateInstantiations().
        if (name.find('<') != std::string::npos) continue;

        // Skip functions not in the visibility collection (e.g., inside
        // comptime blocks that the VisibilityCollector doesn't traverse).
        if (collectedNames.find(name) == collectedNames.end()) continue;

        if (mapping.matched.find(name) == mapping.matched.end()) {
            diag_.error(sym.location, "public function '" + name + "' declared in .topo but missing in LLVM IR");
            ++result.publicMissing;
        }
    }
}

void Verifier::checkLogicBlockConsistency(const llvm::Module& /*module*/,
                                          const SymbolMapping& mapping,
                                          VerifyResult& result) {
    // Build a reverse map: LLVM function name → Topo qualified name
    std::unordered_map<std::string, std::string> llvmToTopo;
    for (const auto& [topoName, func] : mapping.matched) {
        llvmToTopo[func->getName().str()] = topoName;
    }

    for (const auto& [blockName, block] : symbols_.logicBlocks()) {
        // Pipeline fn blocks have stub bodies that PipelineCodeGenPass
        // replaces after verification — skip call-consistency checks.
        if (block.isPipeline) continue;

        // Find the LLVM function for this logic block
        auto it = mapping.matched.find(blockName);
        if (it == mapping.matched.end()) continue;

        const llvm::Function* llvmFunc = it->second;

        // Collect Topo-known functions called in the LLVM IR
        std::unordered_set<std::string> irCalledTopo;
        for (const auto& bb : *llvmFunc) {
            for (const auto& inst : bb) {
                const llvm::Function* callee = nullptr;
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    callee = call->getCalledFunction();
                } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    callee = invoke->getCalledFunction();
                }
                if (!callee) continue;

                // Check if this callee is a known Topo symbol
                auto nameIt = llvmToTopo.find(callee->getName().str());
                if (nameIt != llvmToTopo.end()) {
                    irCalledTopo.insert(nameIt->second);
                }
            }
        }

        // Build set of declared callees from the fn block
        // Resolve simple names to qualified names within the same namespace
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }

        std::unordered_set<std::string> declaredCallees;
        for (const auto& callee : block.calledFunctions) {
            // Pipeline blocks store already-qualified names; regular blocks
            // store simple names that need the namespace prefix.
            std::string qualified = block.isPipeline ? callee : nsPrefix + callee;
            declaredCallees.insert(qualified);
        }

        // Compare: declared in fn block but not called in IR → error
        for (const auto& declared : declaredCallees) {
            if (irCalledTopo.find(declared) == irCalledTopo.end()) {
                diag_.error(
                    block.location,
                    "fn block '" + blockName + "' declares operation '" + declared + "' but IR does not call it");
                ++result.blockMismatches;
            }
        }

        // Compare: called in IR but not declared in fn block → error
        for (const auto& called : irCalledTopo) {
            if (declaredCallees.find(called) == declaredCallees.end()) {
                diag_.error(
                    block.location,
                    "fn block '" + blockName + "': IR calls '" + called + "' but it is not declared in the fn block");
                ++result.blockMismatches;
            }
        }
    }
}

// --- Issue 005: Signature verification ---

std::string Verifier::topoTypeToLLVMDesc(const TypeNode& type) {
    if (type.nameParts.empty()) return "void";

    // Unwrap lifetime<scope, T> to inner type T
    if (type.nameParts.size() == 1 && type.nameParts[0] == "lifetime" && !type.templateArgs.empty()) {
        return topoTypeToLLVMDesc(type.templateArgs.back());
    }

    const std::string& name = type.nameParts.back();

    // Basic type mapping for diagnostics (C++, Topo, and Rust)
    if (name == "void") return "void";
    if (name == "bool") return "i1";
    if (name == "char" || name == "int8" || name == "i8" || name == "u8") return "i8";
    if (name == "short" || name == "int16" || name == "i16" || name == "u16") return "i16";
    if (name == "int" || name == "int32" || name == "i32" || name == "u32") return "i32";
    if (name == "long" || name == "int64" || name == "i64" || name == "u64") return "i64";
    if (name == "float" || name == "f32") return "float";
    if (name == "double" || name == "f64") return "double";

    // Abstract type names
    if (name == "integer" || name == "unsigned") return "i32";
    if (name == "floating") return "double";
    if (name == "boolean") return "i1";
    if (name == "text") return "ptr";

    // For ref/ptr types, LLVM uses opaque pointers (ptr)
    if (type.modifier == TypeNode::Ref || type.modifier == TypeNode::Ptr) {
        return "ptr";
    }

    // Custom types — return as-is (will be compared structurally)
    return type.toString();
}

bool Verifier::typesMatch(llvm::Type* llvmType, const TypeNode& topoType) {
    // void
    if (topoType.nameParts.empty() || (topoType.nameParts.size() == 1 && topoType.nameParts[0] == "void")) {
        return llvmType->isVoidTy();
    }

    // Unwrap lifetime<scope, T> to inner type T
    if (topoType.nameParts.size() == 1 && topoType.nameParts[0] == "lifetime" && !topoType.templateArgs.empty()) {
        return typesMatch(llvmType, topoType.templateArgs.back());
    }

    // ref/ptr → LLVM pointer type
    if (topoType.modifier == TypeNode::Ref || topoType.modifier == TypeNode::Ptr) {
        return llvmType->isPointerTy();
    }

    const std::string& name = topoType.nameParts.back();

    // Resolve type aliases: if name is a known alias, recurse with the target.
    // Guard against cycles (e.g. "using int = std::cpp17::int" where target's
    // last name part equals the alias name).
    const auto* alias = symbols_.findTypeAlias(name);
    if (alias && alias->targetType.nameParts.back() != name) {
        return typesMatch(llvmType, alias->targetType);
    }

    // Abstract type names: accept any matching LLVM type for the logical kind
    auto abstractKind = TypeRegistry::classifyAbstractName(name);
    if (abstractKind) {
        switch (*abstractKind) {
        case LogicalTypeKind::Integer: return llvmType->isIntegerTy();
        case LogicalTypeKind::UnsignedInteger: return llvmType->isIntegerTy();
        case LogicalTypeKind::Floating: return llvmType->isFloatingPointTy();
        case LogicalTypeKind::Boolean: return llvmType->isIntegerTy(1);
        case LogicalTypeKind::Text: return llvmType->isPointerTy() || llvmType->isStructTy();
        default: return true; // Opaque/Void/Sequence/Mapping — accept
        }
    }

    // Integer types (C++, Topo capitalized, and Rust)
    if (name == "bool" || name == "Bool") return llvmType->isIntegerTy(1);
    if (name == "char" || name == "Char" || name == "int8" || name == "Int8" || name == "i8" || name == "u8")
        return llvmType->isIntegerTy(8);
    if (name == "short" || name == "int16" || name == "Int16" || name == "i16" || name == "u16")
        return llvmType->isIntegerTy(16);
    if (name == "int" || name == "Int" || name == "int32" || name == "Int32" || name == "i32" || name == "u32")
        return llvmType->isIntegerTy(32);
    if (name == "long" || name == "Int64" || name == "int64" || name == "i64" || name == "u64")
        return llvmType->isIntegerTy(64);
    if (name == "isize" || name == "usize") return llvmType->isIntegerTy(64) || llvmType->isIntegerTy(32);

    // Floating-point types (C++, Topo capitalized, and Rust)
    if (name == "float" || name == "Float" || name == "f32") return llvmType->isFloatTy();
    if (name == "double" || name == "Double" || name == "f64") return llvmType->isDoubleTy();

    // Custom/struct types: accept conservatively.
    // LLVM struct names are implementation-dependent (class., struct., etc.)
    // and opaque pointers hide type info, so strict matching is unreliable.
    if (llvmType->isStructTy() || llvmType->isPointerTy()) {
        return true;
    }

    // Platform ABI may decompose small structs into scalars or arrays
    // (e.g., ARM64 returns {i32, i32} as i64, or passes it as [2 x i32]).
    // If the declared type is a known class, accept any non-void LLVM type.
    {
        std::string qualName;
        for (const auto& part : topoType.nameParts) {
            if (!qualName.empty()) qualName += "::";
            qualName += part;
        }
        // Direct lookup by qualified name
        if (symbols_.findClassSymbol(qualName) != nullptr) {
            return true;
        }
        // Suffix match: type may be referenced without namespace prefix
        // (e.g., "Vec2" within namespace "math" stored as "math::Vec2")
        std::string suffix = "::" + name;
        for (const auto& [cls, _] : symbols_.classSymbols()) {
            if (cls == name ||
                (cls.size() > suffix.size() && cls.compare(cls.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                return true;
            }
        }
    }

    return false;
}

void Verifier::checkSignatures(const SymbolMapping& mapping, VerifyResult& result) {
    for (const auto& [name, sym] : symbols_.functions()) {
        auto it = mapping.matched.find(name);
        if (it == mapping.matched.end()) continue;

        const llvm::Function* func = it->second;

        // --- Parameter count check ---
        // C++ member functions have an implicit 'this' pointer as first arg.
        // Detect member functions: name contains "::" with class prefix in
        // the symbol table's class symbols.
        size_t expectedArgs = sym.params.size();
        size_t actualArgs = func->arg_size();

        // Heuristic: if actual args = expected + 1 and first arg is pointer,
        // likely a member function with implicit 'this'.
        bool hasSretParam = false;
        bool hasThisParam = false;
        size_t skipCount = 0;

        // Check for sret (struct return) parameter — LLVM may add a hidden
        // pointer parameter for functions returning structs by value.
        if (actualArgs > 0 && func->hasParamAttribute(0, llvm::Attribute::StructRet)) {
            hasSretParam = true;
            ++skipCount;
        }

        // Check for 'this' pointer in member functions
        unsigned thisIdx = hasSretParam ? 1 : 0;
        if (actualArgs > thisIdx) {
            // If this is a known class member function, account for 'this'
            std::string nsPrefix;
            auto lastSep = name.rfind("::");
            if (lastSep != std::string::npos) {
                nsPrefix = name.substr(0, lastSep);
                if (symbols_.findClassSymbol(nsPrefix) != nullptr) {
                    hasThisParam = true;
                    ++skipCount;
                }
            }
        }

        size_t adjustedActual = (actualArgs >= skipCount) ? actualArgs - skipCount : actualArgs;

        if (adjustedActual != expectedArgs) {
            diag_.error(sym.location,
                        "function '" + name + "' declared with " + std::to_string(expectedArgs) +
                            " parameter(s) but IR has " + std::to_string(adjustedActual) +
                            " (excluding implicit this/sret)");
            ++result.signatureMismatches;
            continue; // Skip per-param type checks if count mismatches
        }

        // --- Return type check ---
        llvm::Type* irRetType = func->getReturnType();
        if (!sym.isMultiReturn) {
            // If sret is used, IR returns void but the actual return is via
            // the sret pointer parameter — accept the match.
            bool sretVoidOk = hasSretParam && irRetType->isVoidTy();
            if (!sretVoidOk && !typesMatch(irRetType, sym.returnType)) {
                diag_.error(sym.location,
                            "function '" + name + "' declared with return type '" + sym.returnType.toString() +
                                "' (expected " + topoTypeToLLVMDesc(sym.returnType) +
                                ") but IR returns a different type");
                ++result.signatureMismatches;
            }
        } else {
            // Multi-return: IR should return something (struct, array, or scalar)
            // or use sret. ARM64 ABI may pack small multi-return structs into
            // a single scalar (e.g. i64) or array ([2 x i64]).
            if (!hasSretParam && irRetType->isVoidTy()) {
                diag_.error(sym.location,
                            "function '" + name +
                                "' declared with multiple return values but IR "
                                "returns void without sret");
                ++result.signatureMismatches;
            }
        }

        // --- Per-parameter type check ---
        size_t argOffset = skipCount;
        for (size_t i = 0; i < sym.params.size() && (i + argOffset) < actualArgs; ++i) {
            llvm::Type* argType = func->getArg(static_cast<unsigned>(i + argOffset))->getType();
            const TypeNode& topoParamType = sym.params[i].type;
            if (!typesMatch(argType, topoParamType)) {
                diag_.warning(sym.location,
                              "function '" + name + "' parameter " + std::to_string(i) + " ('" + sym.params[i].name +
                                  "'): declared as '" + topoParamType.toString() + "' but IR has a different type");
                ++result.signatureMismatches;
            }
        }
    }
}

// --- Issue 005: const attribute consistency ---

void Verifier::checkConstConsistency(const SymbolMapping& mapping,
                                     const std::vector<VisibilityEntry>& entries,
                                     VerifyResult& result) {
    // Build a lookup from qualified name to VisibilityEntry
    std::unordered_map<std::string, const VisibilityEntry*> entryMap;
    for (const auto& entry : entries) {
        entryMap[entry.qualifiedName] = &entry;
    }

    for (const auto& [name, sym] : symbols_.functions()) {
        auto it = mapping.matched.find(name);
        if (it == mapping.matched.end()) continue;

        const llvm::Function* func = it->second;
        auto entryIt = entryMap.find(name);
        if (entryIt == entryMap.end()) continue;

        const VisibilityEntry& entry = *entryIt->second;

        // Check: Topo says const function, IR should have memory(read)
        if (sym.isConst && !func->onlyReadsMemory()) {
            diag_.warning(sym.location,
                          "function '" + name +
                              "' declared as const but IR does not have "
                              "memory(read) attribute — const contract not verifiable at -O0");
            ++result.constMismatches;
        }

        // Check: const ref/ptr parameters should have readonly on IR arg
        for (size_t i = 0; i < entry.paramConsts.size(); ++i) {
            const auto& pc = entry.paramConsts[i];
            if (!pc.isConst || (pc.modifier != TypeNode::Ref && pc.modifier != TypeNode::Ptr)) {
                continue;
            }

            // Find the corresponding IR argument (account for implicit this/sret)
            // This is a best-effort check — skip if arg index is out of range
            if (i >= func->arg_size()) continue;

            // Simple offset: just check the arg at position i
            // (The full offset calculation with this/sret is complex;
            //  this check catches the common case)
            const llvm::Argument* arg = func->getArg(static_cast<unsigned>(i));
            if (!arg->hasAttribute(llvm::Attribute::ReadOnly)) {
                diag_.warning(sym.location,
                              "function '" + name + "' parameter " + std::to_string(i) +
                                  " declared as const ref/ptr but IR argument "
                                  "lacks readonly attribute — const contract not verifiable at -O0");
                ++result.constMismatches;
            }
        }
    }
}

// --- Issue 005: Class member function verification ---

void Verifier::checkClassMembers(const SymbolMapping& mapping, VerifyResult& result) {
    for (const auto& [className, cls] : symbols_.classSymbols()) {
        // Skip template class — members only exist when instantiated.
        // Template instantiations are verified in checkTemplateInstantiations().
        if (!cls.templateParams.empty()) continue;

        // Skip template specialization classes (names containing '<').
        // Their member names use Topo aliases that may differ from IR.
        if (className.find('<') != std::string::npos) continue;

        // Check member functions
        for (const auto& memberName : cls.memberFunctions) {
            if (mapping.matched.find(memberName) == mapping.matched.end()) {
                // Determine severity based on visibility
                const auto* funcSym = symbols_.findFunction(memberName);

                // Skip template member functions within concrete classes
                if (funcSym && !funcSym->templateParams.empty()) continue;

                if (funcSym && funcSym->visibility == Visibility::Public) {
                    diag_.error(
                        cls.location,
                        "class '" + className + "' public member function '" + memberName + "' missing in LLVM IR");
                    ++result.classMemberMissing;
                } else if (funcSym) {
                    diag_.warning(cls.location,
                                  "class '" + className + "' member function '" + memberName + "' missing in LLVM IR");
                }
            }
        }

        // Check constructors — at least one variant should exist
        if (!cls.constructors.empty()) {
            bool anyCtorFound = false;
            for (const auto& ctorName : cls.constructors) {
                // Constructors in IR demangle as ClassName::ClassName
                // The SymbolMapper may match them directly by qualified name
                if (mapping.matched.find(ctorName) != mapping.matched.end()) {
                    anyCtorFound = true;
                    break;
                }
            }
            if (!anyCtorFound) {
                diag_.warning(cls.location, "class '" + className + "' declares constructor(s) but none found in IR");
            }
        }

        // Check destructor
        if (!cls.destructor.empty()) {
            if (mapping.matched.find(cls.destructor) == mapping.matched.end()) {
                diag_.warning(cls.location,
                              "class '" + className + "' declares destructor '" + cls.destructor +
                                  "' but it was not found in IR");
            }
        }
    }
}

// --- Issue 005: Stage execution ordering ---

void Verifier::checkStageOrdering(const llvm::Module& /*module*/, const SymbolMapping& mapping, VerifyResult& result) {
    // Build reverse map: LLVM function name → Topo qualified name
    std::unordered_map<std::string, std::string> llvmToTopo;
    for (const auto& [topoName, func] : mapping.matched) {
        llvmToTopo[func->getName().str()] = topoName;
    }

    for (const auto& [blockName, block] : symbols_.logicBlocks()) {
        // Only check blocks that have stage information
        if (block.stages.empty()) continue;

        // Build a map: callee simple name → stage
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }

        std::unordered_map<std::string, int> calleeStage;
        for (size_t i = 0; i < block.calledFunctions.size() && i < block.stages.size(); ++i) {
            // Pipeline blocks store qualified names; regular blocks store simple names
            std::string qualified = block.isPipeline ? block.calledFunctions[i] : nsPrefix + block.calledFunctions[i];
            calleeStage[qualified] = block.stages[i];
        }

        // Find the IR function for this logic block
        auto it = mapping.matched.find(blockName);
        if (it == mapping.matched.end()) continue;

        const llvm::Function* llvmFunc = it->second;

        // Traverse IR instructions in order, collect stage sequence
        int maxStageSeen = -1;
        for (const auto& bb : *llvmFunc) {
            for (const auto& inst : bb) {
                const llvm::Function* callee = nullptr;
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    callee = call->getCalledFunction();
                } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    callee = invoke->getCalledFunction();
                }
                if (!callee) continue;

                auto nameIt = llvmToTopo.find(callee->getName().str());
                if (nameIt == llvmToTopo.end()) continue;

                auto stageIt = calleeStage.find(nameIt->second);
                if (stageIt == calleeStage.end()) continue;

                int currentStage = stageIt->second;
                if (currentStage < maxStageSeen) {
                    diag_.error(block.location,
                                "fn block '" + blockName + "': IR calls '" + nameIt->second + "' (stage " +
                                    std::to_string(currentStage) + ") after a stage " + std::to_string(maxStageSeen) +
                                    " operation — violates stage ordering");
                    ++result.stageOrderViolations;
                }
                if (currentStage > maxStageSeen) {
                    maxStageSeen = currentStage;
                }
            }
        }
    }
}

// --- Issue 005: Pipeline edge / terminal verification ---

void Verifier::checkPipelineEdges(const llvm::Module& /*module*/, const SymbolMapping& mapping, VerifyResult& result) {
    // Build reverse map: LLVM function name → Topo qualified name
    std::unordered_map<std::string, std::string> llvmToTopo;
    for (const auto& [topoName, func] : mapping.matched) {
        llvmToTopo[func->getName().str()] = topoName;
    }

    for (const auto& [blockName, block] : symbols_.logicBlocks()) {
        if (!block.isPipeline) continue;
        if (block.edges.empty()) continue;

        // Find the terminal node from edges
        std::string terminalNode;
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }

        for (const auto& edge : block.edges) {
            if (edge.isTerminal) {
                terminalNode = nsPrefix + edge.source;
                break;
            }
        }

        if (terminalNode.empty()) continue;

        // Find the IR function for this pipeline block
        auto it = mapping.matched.find(blockName);
        if (it == mapping.matched.end()) continue;

        const llvm::Function* llvmFunc = it->second;

        // Find the last Topo-known pipeline call in IR
        std::string lastCalledPipelineNode;

        // Build set of pipeline nodes (qualified names)
        // Pipeline blocks already store qualified names in calledFunctions
        std::unordered_set<std::string> pipelineNodes;
        for (const auto& callee : block.calledFunctions) {
            pipelineNodes.insert(callee);
        }

        for (const auto& bb : *llvmFunc) {
            for (const auto& inst : bb) {
                const llvm::Function* callee = nullptr;
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    callee = call->getCalledFunction();
                } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    callee = invoke->getCalledFunction();
                }
                if (!callee) continue;

                auto nameIt = llvmToTopo.find(callee->getName().str());
                if (nameIt == llvmToTopo.end()) continue;

                if (pipelineNodes.count(nameIt->second)) {
                    lastCalledPipelineNode = nameIt->second;
                }
            }
        }

        // Verify terminal node is the last called pipeline node
        if (!lastCalledPipelineNode.empty() && lastCalledPipelineNode != terminalNode) {
            diag_.error(block.location,
                        "pipeline '" + blockName + "': terminal node '" + terminalNode +
                            "' should be the last called pipeline node, "
                            "but '" +
                            lastCalledPipelineNode + "' is called after it");
            ++result.pipelineEdgeMismatches;
        }
    }
}

// --- Helper: resolve Topo type name to C++ demangled form ---

// Canonicalize fixed-width typedef names to their underlying C++ types,
// matching what appears in demangled IR symbols.
static std::string canonicalizeTypeName(const std::string& name) {
    static const std::unordered_map<std::string, std::string> canon = {
        {"int32_t", "int"},
        {"uint32_t", "unsigned int"},
        {"int64_t", "long"},
        {"uint64_t", "unsigned long"},
        {"int16_t", "short"},
        {"uint16_t", "unsigned short"},
        {"int8_t", "signed char"},
        {"uint8_t", "unsigned char"},
        {"size_t", "unsigned long"},
        {"float32_t", "float"},
        {"float64_t", "double"},
    };
    auto it = canon.find(name);
    return (it != canon.end()) ? it->second : name;
}

std::string Verifier::resolveTopoTypeName(const TypeNode& type) const {
    if (type.nameParts.empty()) return "";

    std::string name = type.nameParts.back();

    // Check if it's a type alias.
    // Guard against cycles: if the alias target's last name part equals the
    // alias name itself (e.g., "using int = std::cpp17::int"), resolving
    // would recurse infinitely.  Skip the alias in that case and fall
    // through to the built-in type mapping below.
    const auto* alias = symbols_.findTypeAlias(name);
    if (alias && alias->targetType.nameParts.back() != name) {
        // Recursively resolve the alias target
        return resolveTopoTypeName(alias->targetType);
    }

    // Basic Topo → C++ type mapping
    if (name == "Int" || name == "int") return "int";
    if (name == "Bool" || name == "bool") return "bool";
    if (name == "Char" || name == "char") return "char";
    if (name == "Float" || name == "float") return "float";
    if (name == "Double" || name == "double") return "double";
    if (name == "String") return "std::string";
    if (name == "Int8" || name == "int8") return "int8_t";
    if (name == "Int16" || name == "int16") return "int16_t";
    if (name == "Int32" || name == "int32") return "int32_t";
    if (name == "Int64" || name == "int64") return "int64_t";

    // For multi-part names (e.g., "std::cpp17::int32_t"), try to
    // canonicalize the last part first — if it resolves to a basic C++
    // type, return that directly instead of the qualified name.
    if (type.nameParts.size() > 1) {
        std::string lastPart = type.nameParts.back();
        std::string canonical = canonicalizeTypeName(lastPart);
        if (canonical != lastPart) {
            return canonical;
        }
        std::string result;
        for (size_t i = 0; i < type.nameParts.size(); ++i) {
            if (i > 0) result += "::";
            result += type.nameParts[i];
        }
        return result;
    }

    return canonicalizeTypeName(name);
}

// --- Helper: normalize template arguments for fuzzy matching ---
// Replaces typedef names inside angle brackets with canonical C++ types
// and removes extra whitespace, so that "Vector<int32_t>" matches "Vector<int>".
static std::string normalizeTemplateArgs(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    size_t i = 0;
    while (i < name.size()) {
        if (name[i] == '<' || name[i] == ',') {
            out += name[i++];
            // Skip whitespace after '<' or ','
            while (i < name.size() && name[i] == ' ')
                ++i;
            // Collect the next token (type name) until ',', '>', or '<'
            size_t start = i;
            while (i < name.size() && name[i] != ',' && name[i] != '>' && name[i] != '<' && name[i] != ' ') {
                ++i;
            }
            if (i > start) {
                std::string token = name.substr(start, i - start);
                out += canonicalizeTypeName(token);
            }
            // Skip trailing whitespace before ',' or '>'
            while (i < name.size() && name[i] == ' ')
                ++i;
        } else {
            out += name[i++];
        }
    }
    return out;
}

// --- Check 8: Template instantiation existence in IR ---

void Verifier::checkTemplateInstantiations(const llvm::Module& module,
                                           const SymbolMapping& mapping,
                                           VerifyResult& result) {
    const auto& instantiates = symbols_.instantiates();
    if (instantiates.empty()) return;

    // Build demangled map for template matching
    auto demangledMap = SymbolMapper::buildDemangledMap(const_cast<llvm::Module&>(module));

    for (const auto& inst : instantiates) {
        if (inst.type.nameParts.empty()) continue;

        // Build the expected demangled name from the instantiate type
        std::string baseName = inst.type.nameParts.back();

        // Resolve template arguments to C++ types
        std::string templateArgStr;
        if (!inst.type.templateArgs.empty()) {
            templateArgStr = "<";
            for (size_t i = 0; i < inst.type.templateArgs.size(); ++i) {
                if (i > 0) templateArgStr += ", ";
                templateArgStr += resolveTopoTypeName(inst.type.templateArgs[i]);
            }
            templateArgStr += ">";
        }

        // Build namespace prefix from qualifiedName.
        // Strip template arguments first, then take everything before the
        // last "::" as the namespace prefix.  This correctly handles
        // multi-level namespaces like "a::b::Vector<Int>" → "a::b::".
        std::string nsPrefix;
        {
            auto angleBracket = inst.qualifiedName.find('<');
            std::string beforeArgs =
                (angleBracket != std::string::npos) ? inst.qualifiedName.substr(0, angleBracket) : inst.qualifiedName;
            auto lastSep = beforeArgs.rfind("::");
            if (lastSep != std::string::npos) {
                nsPrefix = beforeArgs.substr(0, lastSep + 2);
            }
        }

        std::string expectedFull = nsPrefix + baseName + templateArgStr;

        // Strategy 1: Exact match for function templates
        bool found = false;
        for (const auto& [dname, func] : demangledMap) {
            if (dname == expectedFull) {
                found = true;
                break;
            }
        }

        // Strategy 2: Class template prefix match — look for any member
        // function with the class<args>:: prefix
        if (!found) {
            std::string classPrefix = expectedFull + "::";
            for (const auto& [dname, func] : demangledMap) {
                if (dname.size() > classPrefix.size() && dname.substr(0, classPrefix.size()) == classPrefix) {
                    found = true;
                    break;
                }
            }
        }

        // Strategy 3: Try without namespace prefix (the IR may use a
        // different namespace path)
        if (!found && !nsPrefix.empty()) {
            std::string shortName = baseName + templateArgStr;
            for (const auto& [dname, func] : demangledMap) {
                // Check for suffix match: "::baseName<args>" or exact
                if (dname == shortName) {
                    found = true;
                    break;
                }
                std::string suffix = "::" + shortName;
                if (dname.size() > suffix.size() && dname.substr(dname.size() - suffix.size()) == suffix) {
                    found = true;
                    break;
                }
                // Also check class template prefix without namespace
                std::string shortClassPrefix = shortName + "::";
                if (dname.find(shortClassPrefix) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }

        // Strategy 4: Normalize template args (typedef → canonical) and retry
        if (!found) {
            std::string normalized = normalizeTemplateArgs(expectedFull);
            for (const auto& [dname, func] : demangledMap) {
                std::string normDname = normalizeTemplateArgs(dname);
                if (normDname == normalized) {
                    found = true;
                    break;
                }
                // Also check class prefix match
                std::string normalizedPrefix = normalized + "::";
                if (normDname.size() > normalizedPrefix.size() &&
                    normDname.substr(0, normalizedPrefix.size()) == normalizedPrefix) {
                    found = true;
                    break;
                }
            }
        }

        // Strategy 5: Normalized match without namespace prefix
        if (!found && !nsPrefix.empty()) {
            std::string shortName = baseName + templateArgStr;
            std::string normShort = normalizeTemplateArgs(shortName);
            for (const auto& [dname, func] : demangledMap) {
                std::string normDname = normalizeTemplateArgs(dname);
                if (normDname == normShort) {
                    found = true;
                    break;
                }
                std::string normSuffix = "::" + normShort;
                if (normDname.size() > normSuffix.size() &&
                    normDname.compare(normDname.size() - normSuffix.size(), normSuffix.size(), normSuffix) == 0) {
                    found = true;
                    break;
                }
                std::string normClassPrefix = normShort + "::";
                if (normDname.find(normClassPrefix) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            diag_.error(inst.location,
                        "instantiate '" + inst.qualifiedName +
                            "' — no matching instantiation found in IR "
                            "(expected '" +
                            expectedFull + "' or member prefix)");
            ++result.templateInstantiationMissing;
        }
    }
}

// --- Helper: collect all constraint members including inherited ---

std::vector<ConstraintMember> Verifier::collectConstraintMembers(const std::string& constraintName) const {
    std::vector<ConstraintMember> members;
    std::unordered_set<std::string> visited;

    std::string current = constraintName;
    while (!current.empty() && visited.insert(current).second) {
        const auto* cs = symbols_.findConstraintSymbol(current);
        if (!cs) break;

        for (const auto& m : cs->members) {
            members.push_back(m);
        }

        current = cs->parentConstraint.value_or("");
    }

    return members;
}

// --- Check 9: Constraint satisfaction verification ---

void Verifier::checkConstraintSatisfaction(const llvm::Module& module,
                                           const SymbolMapping& mapping,
                                           VerifyResult& result) {
    const auto& instantiates = symbols_.instantiates();
    if (instantiates.empty()) return;

    // Build demangled map for function lookup
    auto demangledMap = SymbolMapper::buildDemangledMap(const_cast<llvm::Module&>(module));

    for (const auto& inst : instantiates) {
        if (inst.type.nameParts.empty() || inst.type.templateArgs.empty()) continue;

        // Find the template declaration (class or function)
        const std::string& typeName = inst.type.nameParts.back();

        // Look up class template by simple name
        const ClassSymbol* cls = nullptr;
        for (const auto& [qname, cs] : symbols_.classSymbols()) {
            if (cs.simpleName == typeName && !cs.templateParams.empty()) {
                cls = &cs;
                break;
            }
        }

        // Also check function templates
        const FunctionSymbol* funcTmpl = nullptr;
        if (!cls) {
            for (const auto& [qname, fs] : symbols_.functions()) {
                if (fs.simpleName == typeName && !fs.templateParams.empty()) {
                    funcTmpl = &fs;
                    break;
                }
            }
        }

        const std::vector<TemplateParamDecl>* templateParams = nullptr;
        if (cls)
            templateParams = &cls->templateParams;
        else if (funcTmpl)
            templateParams = &funcTmpl->templateParams;
        else
            continue;

        // Check each template parameter's constraint against the actual argument
        for (size_t i = 0; i < templateParams->size() && i < inst.type.templateArgs.size(); ++i) {
            const auto& param = (*templateParams)[i];
            if (param.kind != TemplateParamDecl::TypeParam) continue;
            if (param.constraintType.nameParts.empty()) continue;

            const std::string& constraintName = param.constraintType.nameParts[0];
            const auto& actualArg = inst.type.templateArgs[i];
            std::string actualTypeName = resolveTopoTypeName(actualArg);

            // Collect all members this constraint requires (including inherited)
            auto requiredMembers = collectConstraintMembers(constraintName);
            if (requiredMembers.empty()) continue;

            // Check if there's an adapt entry for this type → constraint
            auto adapts = symbols_.findAdaptsForConstraint(constraintName);
            const AdaptEntry* matchingAdapt = nullptr;
            for (const auto* a : adapts) {
                std::string adaptTarget = resolveTopoTypeName(a->targetType);
                if (adaptTarget == actualTypeName) {
                    matchingAdapt = a;
                    break;
                }
            }

            if (!matchingAdapt) {
                // No adapt found — issue warning (type may satisfy structurally)
                diag_.warning(inst.location,
                              "instantiate '" + inst.qualifiedName + "': type argument '" + actualArg.toString() +
                                  "' has no adapt for constraint '" + constraintName + "'");
                continue;
            }

            // Verify: each mapping target function must exist in IR
            for (const auto& m : matchingAdapt->mappings) {
                bool targetFound = false;
                for (const auto& [dname, func] : demangledMap) {
                    // Check if the target name appears in the demangled map
                    if (dname == m.targetName || dname.find("::" + m.targetName) != std::string::npos) {
                        targetFound = true;
                        break;
                    }
                }
                // Also check in the Topo matched symbols
                if (!targetFound) {
                    for (const auto& [topoName, func] : mapping.matched) {
                        if (topoName == m.targetName || topoName.find("::" + m.targetName) != std::string::npos) {
                            targetFound = true;
                            break;
                        }
                    }
                }

                if (!targetFound) {
                    diag_.error(matchingAdapt->location,
                                "adapt '" + matchingAdapt->constraintName + "' for '" + actualTypeName +
                                    "': mapping target '" + m.targetName + "' not found in IR");
                    ++result.constraintViolations;
                }
            }
        }
    }
}

// --- Helper: find Topo-mapped calls in an IR function ---

std::vector<Verifier::IRCallInfo> Verifier::findTopoCallsInFunction(
    const llvm::Function* func, const std::unordered_map<std::string, std::string>& llvmToTopo) {
    std::vector<IRCallInfo> calls;
    unsigned order = 0;

    for (const auto& bb : *func) {
        for (const auto& inst : bb) {
            llvm::CallBase* callBase = nullptr;
            if (auto* call = const_cast<llvm::CallInst*>(llvm::dyn_cast<llvm::CallInst>(&inst))) {
                callBase = call;
            } else if (auto* invoke = const_cast<llvm::InvokeInst*>(llvm::dyn_cast<llvm::InvokeInst>(&inst))) {
                callBase = invoke;
            }
            if (!callBase) continue;

            const llvm::Function* callee = callBase->getCalledFunction();
            if (!callee) continue;

            auto nameIt = llvmToTopo.find(callee->getName().str());
            if (nameIt != llvmToTopo.end()) {
                IRCallInfo info;
                info.call = callBase;
                info.topoName = nameIt->second;
                info.orderInBB = order;
                calls.push_back(std::move(info));
            }
            ++order;
        }
    }

    return calls;
}

// --- Check 10: Stage parallel safety ---

void Verifier::checkStageParallelSafety(const llvm::Module& module,
                                        const SymbolMapping& mapping,
                                        VerifyResult& result) {
    // Build reverse map: LLVM function name → Topo qualified name
    std::unordered_map<std::string, std::string> llvmToTopo;
    for (const auto& [topoName, func] : mapping.matched) {
        llvmToTopo[func->getName().str()] = topoName;
    }

    for (const auto& [blockName, block] : symbols_.logicBlocks()) {
        if (block.stages.empty()) continue;
        if (block.isPipeline) continue; // Pipeline blocks have DAG-based ordering

        // Group operations by stage
        std::string nsPrefix;
        auto lastSep = blockName.rfind("::");
        if (lastSep != std::string::npos) {
            nsPrefix = blockName.substr(0, lastSep + 2);
        }

        std::unordered_map<int, std::vector<std::string>> stageOps;
        for (size_t i = 0; i < block.calledFunctions.size() && i < block.stages.size(); ++i) {
            const auto& callee = block.calledFunctions[i];
            if (callee.size() > 8 && callee.substr(0, 8) == "<assign:") continue;
            std::string qualified = nsPrefix + callee;
            stageOps[block.stages[i]].push_back(qualified);
        }

        // Skip stages with only 1 operation
        auto it = mapping.matched.find(blockName);
        if (it == mapping.matched.end()) continue;
        const llvm::Function* llvmFunc = it->second;

        // Find all Topo calls in this function
        auto irCalls = findTopoCallsInFunction(llvmFunc, llvmToTopo);

        for (const auto& [stage, ops] : stageOps) {
            if (ops.size() < 2) continue;

            // Find the IR calls for this stage's operations
            std::vector<const IRCallInfo*> stageCalls;
            for (const auto& call : irCalls) {
                for (const auto& op : ops) {
                    if (call.topoName == op) {
                        stageCalls.push_back(&call);
                        break;
                    }
                }
            }

            // Check direct data dependency: call A's return used as call B's operand
            for (size_t i = 0; i < stageCalls.size(); ++i) {
                for (size_t j = i + 1; j < stageCalls.size(); ++j) {
                    const auto* callA = stageCalls[i];
                    const auto* callB = stageCalls[j];

                    // Check if callA's result is used by callB
                    for (auto& use : callA->call->uses()) {
                        if (use.getUser() == callB->call) {
                            diag_.error(block.location,
                                        "fn block '" + blockName + "': stage " + std::to_string(stage) +
                                            " operations '" + callA->topoName + "' and '" + callB->topoName +
                                            "' have direct data dependency — "
                                            "return value of the former feeds the latter");
                            ++result.stageParallelViolations;
                        }
                    }

                    // Check the reverse direction too
                    for (auto& use : callB->call->uses()) {
                        if (use.getUser() == callA->call) {
                            diag_.error(block.location,
                                        "fn block '" + blockName + "': stage " + std::to_string(stage) +
                                            " operations '" + callB->topoName + "' and '" + callA->topoName +
                                            "' have direct data dependency — "
                                            "return value of the former feeds the latter");
                            ++result.stageParallelViolations;
                        }
                    }
                }
            }

            // Check indirect dependency: same global variable accessed
            for (size_t i = 0; i < stageCalls.size(); ++i) {
                for (size_t j = i + 1; j < stageCalls.size(); ++j) {
                    const auto* callA = stageCalls[i];
                    const auto* callB = stageCalls[j];

                    // Collect globals accessed by each call's callee
                    const llvm::Function* funcA = callA->call->getCalledFunction();
                    const llvm::Function* funcB = callB->call->getCalledFunction();
                    if (!funcA || !funcB) continue;

                    // Only check if both functions have bodies
                    if (funcA->isDeclaration() || funcB->isDeclaration()) continue;

                    // Collect globals used by funcA
                    std::unordered_set<const llvm::GlobalVariable*> globalsA;
                    for (const auto& bb : *funcA) {
                        for (const auto& inst : bb) {
                            for (unsigned op = 0; op < inst.getNumOperands(); ++op) {
                                if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(inst.getOperand(op))) {
                                    globalsA.insert(gv);
                                }
                            }
                        }
                    }

                    // Check if funcB uses any of the same globals
                    for (const auto& bb : *funcB) {
                        for (const auto& inst : bb) {
                            for (unsigned op = 0; op < inst.getNumOperands(); ++op) {
                                if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(inst.getOperand(op))) {
                                    if (globalsA.count(gv)) {
                                        // Check if either function only reads
                                        bool aReadOnly = funcA->onlyReadsMemory();
                                        bool bReadOnly = funcB->onlyReadsMemory();
                                        if (!aReadOnly || !bReadOnly) {
                                            diag_.warning(block.location,
                                                          "fn block '" + blockName + "': stage " +
                                                              std::to_string(stage) + " operations '" +
                                                              callA->topoName + "' and '" + callB->topoName +
                                                              "' both access global '" + gv->getName().str() +
                                                              "' — potential data race");
                                        }
                                        // Break out of inner loops for this pair
                                        goto next_pair;
                                    }
                                }
                            }
                        }
                    }
                next_pair:;
                }
            }
        }
    }
}

} // namespace topo
