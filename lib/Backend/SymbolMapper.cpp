#include "topo/Backend/SymbolMapper.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <unordered_set>

namespace topo {

SymbolMapper::SymbolMapper(llvm::LLVMContext& ctx) : ctx_(ctx) {}

std::unique_ptr<llvm::Module> SymbolMapper::loadModule(const std::string& path) {
    llvm::SMDiagnostic err;
    auto module = llvm::parseIRFile(path, err, ctx_);
    if (!module) {
        err.print("topo-builder", llvm::errs());
    }
    return module;
}

std::string SymbolMapper::demangleToQualified(const std::string& mangledName) {
    // llvm::demangle handles Itanium (_Z...), MSVC (?...), and Rust v0 (_R...)
    std::string demangled = llvm::demangle(mangledName);
    if (demangled == mangledName) {
        // Demangling failed — name was not mangled or uses unknown scheme
        return "";
    }

    // Strip parameter list: find first '('
    auto parenPos = demangled.find('(');
    if (parenPos != std::string::npos) {
        demangled = demangled.substr(0, parenPos);
    }

    // Trim trailing whitespace
    while (!demangled.empty() && demangled.back() == ' ')
        demangled.pop_back();

    // Rust v0 demangled format: "crate::module::function"
    // Already uses :: separators, so we just need to strip the crate hash
    // if present (e.g. "crate::module::function::h1234abcd")
    if (mangledName.size() >= 2 && mangledName[0] == '_' && mangledName[1] == 'R') {
        // Rust v0 mangling detected — strip trailing hash segment
        // Rust demangles to e.g. "mycrate::mymod::myfunc"
        // The hash suffix (::h...) may or may not be present in demangled form
        // depending on the LLVM version. Check for it.
        auto hashPos = demangled.rfind("::h");
        if (hashPos != std::string::npos) {
            // Verify it looks like a hex hash (all remaining chars are hex)
            std::string suffix = demangled.substr(hashPos + 3);
            bool isHash = !suffix.empty();
            for (char c : suffix) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    isHash = false;
                    break;
                }
            }
            if (isHash) {
                demangled = demangled.substr(0, hashPos);
            }
        }
        return demangled;
    }

    // MSVC mangled names: use microsoftDemangle with flags to strip
    // return type, calling convention, access specifiers, and member type.
    // Output format: "container::Shape::area(void) const"
    // We then strip the parameter list and trailing qualifiers.
    if (!mangledName.empty() && mangledName[0] == '?') {
        int status = 0;
        char* msResult = llvm::microsoftDemangle(
            mangledName,
            nullptr,
            &status,
            static_cast<llvm::MSDemangleFlags>(llvm::MSDF_NoReturnType | llvm::MSDF_NoCallingConvention |
                                               llvm::MSDF_NoAccessSpecifier | llvm::MSDF_NoMemberType));
        if (msResult && status == llvm::demangle_success) {
            std::string result(msResult);
            std::free(msResult);
            // Strip parameter list
            auto paren = result.find('(');
            if (paren != std::string::npos) result = result.substr(0, paren);
            // Strip trailing " const"
            if (result.size() > 6 && result.substr(result.size() - 6) == " const")
                result = result.substr(0, result.size() - 6);
            // Trim trailing whitespace
            while (!result.empty() && result.back() == ' ')
                result.pop_back();
            return result;
        }
        if (msResult) std::free(msResult);
        // Fallback: no calling convention found — try last space heuristic
    }

    // Itanium format is just:
    //   "engine::core::math::detail::clamp"
    // But Itanium with templates may contain spaces inside angle brackets:
    //   "container::Map<int, double>::insert"
    // Only apply the space-stripping when the part before the last
    // space looks like a return type (no '::' or '<').
    auto lastSpace = demangled.rfind(' ');
    if (lastSpace != std::string::npos) {
        std::string beforeSpace = demangled.substr(0, lastSpace);
        if (beforeSpace.find("::") == std::string::npos && beforeSpace.find('<') == std::string::npos) {
            std::string candidate = demangled.substr(lastSpace + 1);
            if (!candidate.empty() && (std::isalpha(candidate[0]) || candidate[0] == '_')) {
                demangled = candidate;
            }
        }
    }

    return demangled;
}

bool SymbolMapper::shouldSkip(const llvm::Function& func) {
    llvm::StringRef name = func.getName();

    // Skip LLVM intrinsics
    if (name.starts_with("llvm.")) return true;

    // Skip C++ runtime / ABI symbols (Itanium)
    if (name.starts_with("__cxa_") || name.starts_with("__gxx_") || name.starts_with("_Unwind_")) return true;

    // Skip MSVC runtime / CRT helper functions
    if (name.starts_with("__local_stdio") || name.starts_with("_vfprintf") || name.starts_with("__acrt_") ||
        name.starts_with("__std_"))
        return true;

    // Skip Rust standard library symbols (core, std, alloc crates only).
    // Avoid filtering by generic v0 prefix (_RNvNtCs) — that matches ALL
    // user crate functions, since Cs is the crate hash marker in Rust v0.
    if (name.starts_with("_RN4core") || name.starts_with("_RN3std") || name.starts_with("_RN5alloc") ||
        name.starts_with("_RNvNtN4core") || name.starts_with("_RNvNtN3std") || name.starts_with("_RNvNtN5alloc"))
        return true;
    if (name == "rust_begin_unwind" || name == "rust_panic" || name == "__rust_alloc" || name == "__rust_dealloc" ||
        name == "__rust_realloc" || name == "__rust_alloc_zeroed")
        return true;

    // Skip C library functions (printf, etc.)
    if (name == "printf" || name == "_printf" || name == "puts") return true;

    // Skip main
    if (name == "main" || name == "_main" || name == "mainCRTStartup") return true;

    // Skip declarations (no body)
    if (func.isDeclaration()) return true;

    return false;
}

SymbolMapping SymbolMapping::rebind(llvm::Module& target) const {
    SymbolMapping result;
    for (const auto& [qualifiedName, func] : matched) {
        if (!func) continue;
        auto* newFunc = target.getFunction(func->getName());
        if (newFunc) result.matched[qualifiedName] = newFunc;
    }
    result.unmatchedTopo = unmatchedTopo;
    result.unmatchedIR = unmatchedIR;
    return result;
}

SymbolMapping SymbolMapper::mapSymbols(llvm::Module& module, const std::vector<VisibilityEntry>& entries) {
    SymbolMapping result;

    // Build a set of Topo qualified names for fast lookup
    std::unordered_set<std::string> topoNames;
    for (const auto& entry : entries) {
        topoNames.insert(entry.qualifiedName);
    }

    // Build demangled name → Function map
    std::unordered_map<std::string, llvm::Function*> demangledMap;
    for (auto& func : module) {
        if (shouldSkip(func)) continue;

        std::string qualified = demangleToQualified(func.getName().str());
        if (qualified.empty()) {
            // Could not demangle — possibly a C function or unmangled name.
            // Try raw name as-is.
            qualified = func.getName().str();
        }
        demangledMap[qualified] = &func;
    }

    // Match Topo entries to LLVM functions
    std::unordered_set<std::string> matchedIR;

    // Priority pass: match entries that have explicit bindingTarget
    for (const auto& entry : entries) {
        if (!entry.bindingTarget) continue;
        auto it = demangledMap.find(*entry.bindingTarget);
        if (it != demangledMap.end()) {
            result.matched[entry.qualifiedName] = it->second;
            matchedIR.insert(*entry.bindingTarget);
        }
        // If not found, target may be in an external library — resolved at link time
    }

    for (const auto& entry : entries) {
        // Skip already matched (via bindingTarget)
        if (result.matched.count(entry.qualifiedName)) continue;

        auto it = demangledMap.find(entry.qualifiedName);
        if (it != demangledMap.end()) {
            result.matched[entry.qualifiedName] = it->second;
            matchedIR.insert(entry.qualifiedName);
            continue;
        }

        // Fallback 1: Suffix matching — IR name may have a crate/module
        // prefix not present in Topo declarations (e.g., Rust crate name).
        // Check if any IR name ends with "::qualifiedName".
        // When multiple IR names match, prefer the shortest (most precise).
        bool found = false;
        std::string suffix = "::" + entry.qualifiedName;
        llvm::Function* bestFunc = nullptr;
        std::string bestIRName;
        for (const auto& [irName, func] : demangledMap) {
            if (irName.size() > suffix.size() &&
                irName.compare(irName.size() - suffix.size(), suffix.size(), suffix) == 0) {
                if (!bestFunc || irName.size() < bestIRName.size()) {
                    bestFunc = func;
                    bestIRName = irName;
                }
            }
        }
        if (bestFunc) {
            result.matched[entry.qualifiedName] = bestFunc;
            matchedIR.insert(bestIRName);
            found = true;
        }
        if (found) continue;

        // Fallback 2: Simple name matching — for #[no_mangle] or C-linkage
        // functions where IR has bare name but Topo uses qualified name.
        auto lastSep = entry.qualifiedName.rfind("::");
        if (lastSep != std::string::npos) {
            std::string simpleName = entry.qualifiedName.substr(lastSep + 2);
            auto it2 = demangledMap.find(simpleName);
            if (it2 != demangledMap.end()) {
                result.matched[entry.qualifiedName] = it2->second;
                matchedIR.insert(simpleName);
                continue;
            }
        }

        result.unmatchedTopo.push_back(entry.qualifiedName);
    }

    // Find IR functions not matched by any Topo entry
    for (const auto& [name, func] : demangledMap) {
        if (matchedIR.find(name) == matchedIR.end()) {
            result.unmatchedIR.push_back(name);
        }
    }

    return result;
}

std::unordered_map<std::string, llvm::Function*> SymbolMapper::buildDemangledMap(llvm::Module& module) {
    std::unordered_map<std::string, llvm::Function*> result;
    for (auto& func : module) {
        if (shouldSkip(func)) continue;

        std::string qualified = demangleToQualified(func.getName().str());
        if (qualified.empty()) {
            qualified = func.getName().str();
        }
        result[qualified] = &func;
    }
    return result;
}

} // namespace topo
