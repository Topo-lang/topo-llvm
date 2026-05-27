#ifndef TOPO_IR_SYMBOLOBFUSCATOR_H
#define TOPO_IR_SYMBOLOBFUSCATOR_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Module.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace topo {

class SymbolObfuscator {
public:
    // Obfuscate private/protected symbol names in the module.
    // Public symbols are left unchanged.
    //
    // Normal mode: hash(qualifiedName) -- deterministic
    // Salted mode: hash(salt + qualifiedName) -- per-build unique
    static ObfuscationResult obfuscate(llvm::Module& module,
                                       const std::vector<VisibilityEntry>& entries,
                                       const SymbolMapping& mapping,
                                       ObfuscationMode mode,
                                       const std::string& salt = "");

    // SipHash-2-4-128 over qualifiedName keyed by salt, formatted as
    // 32 lowercase hex chars. Exposed (not private) so the
    // cross-backend parity unit test in SymbolObfuscatorTest can pin
    // the exact algorithm/output against the JVM-side SipHash impl.
    static std::string computeHash(const std::string& qualifiedName, const std::string& salt);
};

} // namespace topo

#endif // TOPO_IR_SYMBOLOBFUSCATOR_H
