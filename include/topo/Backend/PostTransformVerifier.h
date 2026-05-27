#ifndef TOPO_BACKEND_POSTTRANSFORMVERIFIER_H
#define TOPO_BACKEND_POSTTRANSFORMVERIFIER_H

#include "topo/Check/PostTransformVerifier.h"
#include "topo/Backend/SymbolMapper.h"

#include <string>
#include <vector>

namespace llvm { class Module; }

namespace topo::backend {

/// LLVM-specific post-transform verification.
/// Checks that LLVM IR transformations preserved .topo invariants.
class LLVMPostTransformVerifier : public check::PostTransformVerifier {
public:
    /// Verify a transformed LLVM module.
    /// Checks:
    /// 1. Visibility: LLVM linkage matches .topo public/private/internal
    /// 2. Obfuscation: private/internal symbols were renamed (if obfuscation enabled)
    /// 3. Symbol existence: all declared functions still exist in IR
    check::PostTransformResult verifyModule(
        llvm::Module& module,
        const SymbolMapping& mapping,
        const std::vector<VisibilityEntry>& visEntries,
        bool obfuscationEnabled);

    /// PostTransformVerifier interface -- reads bitcode from artifactDir.
    check::PostTransformResult verify(
        const SymbolTable& symbols,
        const std::vector<VisibilityEntry>& visEntries,
        const std::string& artifactDir) override;
};

} // namespace topo::backend

#endif // TOPO_BACKEND_POSTTRANSFORMVERIFIER_H
