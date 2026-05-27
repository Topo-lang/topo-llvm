// @category: ENHANCE
#include "topo/Transforms/ContainmentInterceptionPass.h"

#include "topo/Backend/SymbolMapper.h"
#include "topo/Check/CapabilityCatalog.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Transforms/RuntimeAbiCheck.h"
#include "topo/Transforms/RuntimeAbiVersions.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace topo {

namespace {

/// Get or declare an external C function in the module.
llvm::FunctionCallee getOrDeclareFunc(llvm::Module& module, const std::string& name, llvm::FunctionType* ty) {
    if (auto* existing = module.getFunction(name)) return existing;
    return module.getOrInsertFunction(name, ty);
}

/// Create a global constant string, returning a pointer-typed constant.
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

/// Normalize an IR function name for catalog/SymbolTable matching.
/// Strips the leading underscore added by MachO targets for C symbols,
/// then attempts C++ demangling (Itanium or MSVC).
std::string normalizeIRName(const llvm::Function& F) {
    std::string raw = F.getName().str();

    // MachO targets prepend '_' to C symbol names (e.g. _fopen).
    // Strip it when the module targets MachO so the name matches the catalog.
    const llvm::Module* M = F.getParent();
    if (M) {
        auto triple = M->getTargetTriple().str();
        bool isMachO = triple.find("apple") != std::string::npos ||
                       triple.find("darwin") != std::string::npos ||
                       triple.find("macho") != std::string::npos;
        if (isMachO && !raw.empty() && raw[0] == '_') {
            raw = raw.substr(1);
        }
    }

    // Attempt C++ demangling (handles both Itanium and MSVC schemes).
    // llvm::demangle returns the original string unchanged if it is not mangled.
    std::string demangled = llvm::demangle(raw);
    if (demangled != raw) {
        // Strip trailing parameter list: "ns::func(int, int)" -> "ns::func"
        auto paren = demangled.find('(');
        if (paren != std::string::npos) {
            demangled = demangled.substr(0, paren);
        }
        return demangled;
    }

    return raw;
}

} // anonymous namespace

int ContainmentInterceptionPass::run(llvm::Module& module,
                                     const SymbolTable& symbols,
                                     const SymbolMapping& mapping,
                                     backend::ContainmentInterceptionReport* report) {
    auto& ctx = module.getContext();
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Build set of IR functions that correspond to external-declared symbols.
    // These functions are *allowed* to call external APIs — skip instrumentation.
    std::unordered_set<llvm::Function*> externalFuncs;
    std::unordered_map<llvm::Function*, std::string> funcQualName;
    for (const auto& [name, fn] : symbols.functions()) {
        if (!fn.isExternal) continue;
        auto it = mapping.matched.find(fn.qualifiedName);
        if (it != mapping.matched.end() && it->second) externalFuncs.insert(it->second);
    }
    for (const auto& [qname, func] : mapping.matched) {
        if (func) funcQualName[func] = qname;
    }

    // Build set of external function simple/qualified names for callee-side
    // exemption: if a callee is itself declared external in .topo, the call is
    // legitimate.  These names come from the SymbolTable (already demangled),
    // and we compare them against normalizeIRName output.
    std::unordered_set<std::string> declaredExternalNames;
    for (const auto& [name, fn] : symbols.functions()) {
        if (fn.isExternal) {
            declaredExternalNames.insert(fn.simpleName);
            declaredExternalNames.insert(fn.qualifiedName);
        }
    }

    // Lazy declaration of runtime violation function
    llvm::FunctionCallee violationCallee;
    bool runtimeDeclared = false;

    auto ensureRuntimeDeclared = [&]() {
        if (runtimeDeclared) return;
        auto* funcTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
        violationCallee = getOrDeclareFunc(module, "__topo_containment_violation", funcTy);
        runtimeDeclared = true;
    };

    int instrumented = 0;

    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        // Skip functions marked external — they are allowed to call external APIs
        if (externalFuncs.count(&func)) continue;

        // Resolve a display name for this function (used in violation reports)
        std::string funcName = func.getName().str();

        // Scan all call/invoke instructions
        for (auto& bb : func) {
            for (auto it = bb.begin(); it != bb.end(); ++it) {
                llvm::CallBase* call = nullptr;
                if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&*it))
                    call = ci;
                else if (auto* ii = llvm::dyn_cast<llvm::InvokeInst>(&*it))
                    call = ii;
                if (!call) continue;

                auto* callee = call->getCalledFunction();
                if (!callee) continue;

                // Normalize the callee name: strip macOS _ prefix, demangle C++.
                std::string calleeName = normalizeIRName(*callee);

                // Check if the callee is an external API (file, network, process, etc.)
                auto cap = check::classifyApiCall(calleeName);
                if (!cap) continue;

                // If the callee is a declared external function, calling it is allowed.
                // Comparison uses the normalized (demangled) name, which matches the
                // SymbolTable's qualified/simple names.
                if (declaredExternalNames.count(calleeName)) continue;

                // This is a violation: a non-external function calls an external API.
                // Insert __topo_containment_violation(caller, callee) before the call.
                ensureRuntimeDeclared();
                llvm::IRBuilder<> builder(call);

                auto* callerStr = getOrCreateGlobalString(module, funcName, ".str.topo_cont_caller.");
                auto* calleeStr = getOrCreateGlobalString(module, calleeName, ".str.topo_cont_callee.");

                builder.CreateCall(violationCallee, {callerStr, calleeStr});
                ++instrumented;

                if (report) {
                    backend::ContainmentInterceptionEntry entry;
                    auto qit = funcQualName.find(&func);
                    entry.callerFunction = (qit != funcQualName.end()) ? qit->second : funcName;
                    entry.interceptedCallee = calleeName;
                    report->entries.push_back(std::move(entry));
                }
            }
        }
    }

    if (instrumented > 0) {
        // At least one containment violation site was instrumented —
        // the emitted IR now calls __topo_containment_violation against
        // libtopo-containment. Wire the one-time ABI-version check
        // matching the pattern in topo-llvm/runtime/ABI-COMPAT.md.
        injectAbiCheckCtor(module, "containment", "topo_containment_version", abi::kContainmentVersion);
    }

    return instrumented;
}

} // namespace topo
