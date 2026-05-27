#ifndef TOPO_BACKEND_LLVMTRANSFORMBACKEND_H
#define TOPO_BACKEND_LLVMTRANSFORMBACKEND_H

#include "topo/Backend/IRTransformBackend.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>

namespace topo::backend {

/// LLVM-based implementation of IRTransformBackend.
class LLVMTransformBackend : public IRTransformBackend {
public:
    LLVMTransformBackend();
    ~LLVMTransformBackend() override;

    bool loadAndLink(const std::vector<std::string>& irFiles) override;
    MappingResult mapSymbols(const std::vector<VisibilityEntry>& visEntries) override;
    VerifyResult verify(DiagnosticEngine& diag,
                        const SymbolTable& symbols,
                        const std::vector<VisibilityEntry>& visEntries) override;
    BackendResult optimize(const BackendConfig& config,
                           const SymbolTable& symbols,
                           const std::vector<VisibilityEntry>& visEntries) override;
    std::string writeIR(const std::string& outputDir) override;
    bool dumpIR(const std::string& dumpPath) override;
    check::PostTransformResult postTransformVerify(
        const std::vector<VisibilityEntry>& visEntries,
        bool obfuscationEnabled) override;

    /// Per-Pass analytical reports populated during optimize(). Backend tools
    /// (topo-build-llvm-cpp / topo-build-llvm-rust) downcast to call this and
    /// hand the aggregate to writePassReportsSidecar() — keeping the report
    /// shape out of the core IRTransformBackend interface (principle 19).
    const PassReports& passReports() const { return passReports_; }

private:
    llvm::LLVMContext ctx_;
    std::unique_ptr<llvm::Module> module_;
    SymbolMapping mapping_;
    std::string lastWrittenIRPath_;
    PassReports passReports_;
};

} // namespace topo::backend

#endif // TOPO_BACKEND_LLVMTRANSFORMBACKEND_H
