#ifndef TOPO_IR_SYMBOLMAPPER_H
#define TOPO_IR_SYMBOLMAPPER_H

#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace topo {

struct SymbolMapping {
    // Topo qualified name → LLVM Function
    std::unordered_map<std::string, llvm::Function*> matched;
    // Topo entries that had no matching IR function
    std::vector<std::string> unmatchedTopo;
    // IR functions that had no matching Topo entry (excluding intrinsics etc.)
    std::vector<std::string> unmatchedIR;

    // Create a new SymbolMapping with Function* pointers rebound to the
    // corresponding functions in `target` (matched by mangled LLVM name).
    // Entries whose function cannot be found in `target` are dropped.
    SymbolMapping rebind(llvm::Module& target) const;
};

class SymbolMapper {
public:
    explicit SymbolMapper(llvm::LLVMContext& ctx);

    // Load LLVM IR from a .ll file. Returns null on failure.
    std::unique_ptr<llvm::Module> loadModule(const std::string& path);

    // Map Topo visibility entries to LLVM functions in the module.
    SymbolMapping mapSymbols(llvm::Module& module, const std::vector<VisibilityEntry>& entries);

    // Build a map of demangled qualified names → Function* for all non-skipped
    // functions in the module. Rust v0 impl methods ("<crate::Type>::method")
    // additionally register angle-bracket-free candidate keys
    // ("crate::Type::method", "Type::method") so "ns::Type::method"
    // declarations resolve. Used by Verifier for template matching and the
    // decompile lifter for metadata name resolution.
    static std::unordered_map<std::string, llvm::Function*> buildDemangledMap(llvm::Module& module);

private:
    // Demangle a mangled name (Itanium, MSVC, or Rust v0) and return
    // the qualified name (without parameter list).
    // Returns empty string if demangling fails.
    static std::string demangleToQualified(const std::string& mangledName);

    // Check if a function should be skipped during mapping
    // (intrinsics, C/C++/Rust runtime symbols, main, declarations)
    static bool shouldSkip(const llvm::Function& func);

    llvm::LLVMContext& ctx_;
};

} // namespace topo

#endif // TOPO_IR_SYMBOLMAPPER_H
