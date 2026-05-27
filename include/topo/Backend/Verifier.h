#ifndef TOPO_IR_VERIFIER_H
#define TOPO_IR_VERIFIER_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Basic/Diagnostic.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>

namespace topo {

class Verifier {
public:
    Verifier(DiagnosticEngine& diag, const SymbolTable& symbols);

    // Verify that the LLVM module is consistent with Topo declarations.
    VerifyResult verify(const llvm::Module& module, const SymbolMapping& mapping);

    // Extended verify with const info from VisibilityCollector.
    VerifyResult verify(const llvm::Module& module,
                        const SymbolMapping& mapping,
                        const std::vector<VisibilityEntry>& entries);

private:
    // Check 1: All public symbols must exist in the mapping
    void checkPublicSymbols(const SymbolMapping& mapping, VerifyResult& result);

    // Check 2: fn block operations vs IR call instructions
    void checkLogicBlockConsistency(const llvm::Module& module, const SymbolMapping& mapping, VerifyResult& result);

    // Check 3: Function signature verification (Issue 005)
    void checkSignatures(const SymbolMapping& mapping, VerifyResult& result);

    // Check 4: const attribute consistency (Issue 005)
    void checkConstConsistency(const SymbolMapping& mapping,
                               const std::vector<VisibilityEntry>& entries,
                               VerifyResult& result);

    // Check 5: class member functions exist in IR (Issue 005 advanced)
    void checkClassMembers(const SymbolMapping& mapping, VerifyResult& result);

    // Check 6: stage execution ordering (Issue 005)
    void checkStageOrdering(const llvm::Module& module, const SymbolMapping& mapping, VerifyResult& result);

    // Check 7: pipeline edge / terminal node verification (Issue 005)
    void checkPipelineEdges(const llvm::Module& module, const SymbolMapping& mapping, VerifyResult& result);

    // Check 8: template instantiation existence in IR
    void checkTemplateInstantiations(const llvm::Module& module, const SymbolMapping& mapping, VerifyResult& result);

    // Check 9: constraint satisfaction verification
    void checkConstraintSatisfaction(const llvm::Module& module, const SymbolMapping& mapping, VerifyResult& result);

    // Check 10: same-stage parallel safety (data independence)
    void checkStageParallelSafety(const llvm::Module& module, const SymbolMapping& mapping, VerifyResult& result);

    // Map a Topo type name to the expected LLVM type string for diagnostics.
    static std::string topoTypeToLLVMDesc(const TypeNode& type);

    // Check if an LLVM type matches a Topo TypeNode.
    bool typesMatch(llvm::Type* llvmType, const TypeNode& topoType);

    // Resolve a Topo type name to C++ demangled form (e.g. Int -> int)
    std::string resolveTopoTypeName(const TypeNode& type) const;

    // Recursively collect all constraint members (including inherited)
    std::vector<ConstraintMember> collectConstraintMembers(const std::string& constraintName) const;

    // Helper: find Topo-mapped calls in an IR function
    struct IRCallInfo {
        llvm::CallBase* call;
        std::string topoName;
        unsigned orderInBB;
    };
    static std::vector<IRCallInfo> findTopoCallsInFunction(
        const llvm::Function* func, const std::unordered_map<std::string, std::string>& llvmToTopo);

    DiagnosticEngine& diag_;
    const SymbolTable& symbols_;
};

} // namespace topo

#endif // TOPO_IR_VERIFIER_H
