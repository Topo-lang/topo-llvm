#ifndef TOPO_BACKEND_IRTRANSFORMBACKEND_H
#define TOPO_BACKEND_IRTRANSFORMBACKEND_H

#include "topo/Backend/BackendTypes.h"
#include "topo/Check/PostTransformVerifier.h"
#include "topo/Basic/BuildTypes.h"
#include "topo/Basic/Diagnostic.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Sema/VisibilityCollector.h"

#include <memory>
#include <string>
#include <vector>

namespace topo::backend {

/// Abstract interface for the IR transformation backend.
///
/// Encapsulates Steps 4-6 of the topo-build pipeline:
///   - Load and link IR modules
///   - Map Topo symbols to IR symbols
///   - Verify Topo/IR consistency
///   - Apply visibility, run optimization passes, embed IR, obfuscate
///   - Write optimized IR to disk
class IRTransformBackend {
public:
    virtual ~IRTransformBackend() = default;

    /// Step 4: Load IR files and link into a single module.
    /// Returns true on success.
    virtual bool loadAndLink(const std::vector<std::string>& irFiles) = 0;

    /// Step 5a: Map Topo symbols to IR symbols.
    virtual MappingResult mapSymbols(const std::vector<VisibilityEntry>& visEntries) = 0;

    /// Step 5b: Verify Topo/IR consistency.
    virtual VerifyResult verify(DiagnosticEngine& diag,
                                const SymbolTable& symbols,
                                const std::vector<VisibilityEntry>& visEntries) = 0;

    /// Step 6: Full optimization pipeline (visibility + passes + embed + obfuscate).
    virtual BackendResult optimize(const BackendConfig& config,
                                   const SymbolTable& symbols,
                                   const std::vector<VisibilityEntry>& visEntries) = 0;

    /// Write optimized IR to the given directory, return the file path.
    /// Returns empty string on failure.
    virtual std::string writeIR(const std::string& outputDir) = 0;

    /// Copy IR to dump path (--dump-ir). Returns true on success.
    virtual bool dumpIR(const std::string& dumpPath) = 0;

    /// Step 6b: Post-transform verification -- checks that IR transformations
    /// preserved .topo invariants (visibility linkage, obfuscation, symbol existence).
    /// Default implementation returns a passing result (no-op for backends that
    /// do not support post-transform verification yet).
    virtual check::PostTransformResult postTransformVerify(
        const std::vector<VisibilityEntry>& visEntries,
        bool obfuscationEnabled) {
        return check::PostTransformResult{};
    }
};

/// Create the LLVM-based implementation of IRTransformBackend.
std::unique_ptr<IRTransformBackend> createLLVMBackend();

} // namespace topo::backend

#endif // TOPO_BACKEND_IRTRANSFORMBACKEND_H
