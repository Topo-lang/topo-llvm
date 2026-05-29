#include "topo/Backend/LLVMTransformBackend.h"
#include "topo/Backend/IREmbed.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/PostTransformVerifier.h"
#include "topo/Backend/Verifier.h"
#include "topo/Backend/VisibilityApplier.h"
#include "topo/Transforms/PassFiredMarker.h"
#include "topo/Transforms/SymbolObfuscator.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace topo::backend {

LLVMTransformBackend::LLVMTransformBackend() = default;
LLVMTransformBackend::~LLVMTransformBackend() = default;

bool LLVMTransformBackend::loadAndLink(const std::vector<std::string>& irFiles) {
    if (irFiles.empty()) {
        std::cerr << "error: no IR files to link\n";
        return false;
    }

    llvm::SMDiagnostic smd;
    module_ = llvm::parseIRFile(irFiles[0], smd, ctx_);
    if (!module_) {
        std::cerr << "error: failed to load IR '" << irFiles[0] << "': ";
        smd.print("topo-build", llvm::errs());
        return false;
    }

    for (size_t i = 1; i < irFiles.size(); ++i) {
        auto srcModule = llvm::parseIRFile(irFiles[i], smd, ctx_);
        if (!srcModule) {
            std::cerr << "error: failed to load IR '" << irFiles[i] << "': ";
            smd.print("topo-build", llvm::errs());
            return false;
        }
        if (llvm::Linker::linkModules(*module_, std::move(srcModule))) {
            std::cerr << "error: failed to link '" << irFiles[i] << "'\n";
            return false;
        }
    }

    return true;
}

MappingResult LLVMTransformBackend::mapSymbols(const std::vector<VisibilityEntry>& visEntries) {
    SymbolMapper mapper(ctx_);
    mapping_ = mapper.mapSymbols(*module_, visEntries);

    MappingResult result;
    for (const auto& [name, func] : mapping_.matched) {
        result.matched.push_back({name, func->getName().str()});
    }
    result.unmatchedTopo = mapping_.unmatchedTopo;
    result.unmatchedIR = mapping_.unmatchedIR;
    return result;
}

VerifyResult LLVMTransformBackend::verify(DiagnosticEngine& diag,
                                          const SymbolTable& symbols,
                                          const std::vector<VisibilityEntry>& visEntries) {
    Verifier verifier(diag, symbols);
    return verifier.verify(*module_, mapping_, visEntries);
}

BackendResult LLVMTransformBackend::optimize(const BackendConfig& config,
                                             const SymbolTable& symbols,
                                             const std::vector<VisibilityEntry>& visEntries) {
    BackendResult result;

    // Apply visibility
    VisibilityApplier applier;
    result.visibilityStats = applier.apply(*module_, mapping_, visEntries, config.outputType, config.debugInternal);

    // Snapshot pre-codegen IR for JIT
    std::vector<uint8_t> preCodegenBitcode;
    if (config.embedIR) {
        preCodegenBitcode = IREmbed::serializePreCodegenIR(*module_, symbols, mapping_);

        // Promote JIT-relevant functions to external linkage before passes
        for (const auto& [name, lb] : symbols.logicBlocks()) {
            if (!lb.isPipeline) continue;
            for (const auto& cf : lb.calledFunctions) {
                auto it = mapping_.matched.find(cf);
                if (it != mapping_.matched.end() && it->second) {
                    it->second->setLinkage(llvm::GlobalValue::ExternalLinkage);
                }
            }
        }
    }

    // Run optimization passes
    passReports_ = PassReports{}; // reset before this run
    if (config.optLevel != OptLevel::O0) {
        PassPipelineConfig pcfg;
        pcfg.entries              = &visEntries;
        pcfg.mapping              = &mapping_;
        pcfg.symbols              = &symbols;
        pcfg.mode                 = config.buildMode;
        pcfg.parallelCfg          = config.parallelCfg.isEnabled() ? &config.parallelCfg : nullptr;
        pcfg.adaptiveCfg          = config.adaptiveCfg.isEnabled() ? &config.adaptiveCfg : nullptr;
        pcfg.dataLayoutCfg        = config.dataLayoutCfg.isEnabled() ? &config.dataLayoutCfg : nullptr;
        pcfg.indirectionCfg       = config.indirectionCfg.isEnabled() ? &config.indirectionCfg : nullptr;
        pcfg.indirectionExplicit  = config.indirectionExplicit;
        pcfg.observabilityCfg     = config.observabilityCfg.isEnabled() ? &config.observabilityCfg : nullptr;
        pcfg.lifetimeCfg          = config.lifetimeCfg.isEnabled() ? &config.lifetimeCfg : nullptr;
        pcfg.loopParallelCfg      = config.loopParallelCfg.isEnabled() ? &config.loopParallelCfg : nullptr;
        pcfg.prefetchCfg          = config.prefetchCfg.isEnabled() ? &config.prefetchCfg : nullptr;
        pcfg.pipelineCfg          = &config.pipelineCfg;
        pcfg.containmentCfg       = config.containmentCfg.isEnabled() ? &config.containmentCfg : nullptr;
        pcfg.reports              = &passReports_;
        PassPipeline::run(*module_, config.optLevel, pcfg);
    }

    // Embed IR and metadata
    if (config.embedIR) {
        auto metadata = IREmbed::serializeMetadata(
            symbols, config.parallelCfg, config.adaptiveCfg.isEnabled() ? &config.adaptiveCfg : nullptr);
        IREmbed::embed(*module_, preCodegenBitcode, metadata);
        result.embeddedIRBytes = static_cast<int>(preCodegenBitcode.size());
        result.embeddedMetaBytes = static_cast<int>(metadata.size());
    }

    // Collect JIT export info BEFORE obfuscation
    std::set<std::string> jitQualifiedNames;
    if (config.embedIR) {
        for (const auto& [name, lb] : symbols.logicBlocks()) {
            if (!lb.isPipeline) continue;
            for (const auto& cf : lb.calledFunctions)
                jitQualifiedNames.insert(cf);
            jitQualifiedNames.insert(lb.qualifiedName);
        }
    }

    // Build JIT export entries by demangling IR function names
    std::vector<std::pair<std::string, llvm::Function*>> jitFuncPtrs;
    if (!jitQualifiedNames.empty()) {
        for (auto& func : *module_) {
            if (func.isDeclaration()) continue;
            auto demangled = llvm::demangle(func.getName().str());
            if (demangled == func.getName().str()) continue;
            auto parenPos = demangled.find('(');
            if (parenPos != std::string::npos) demangled = demangled.substr(0, parenPos);
            while (!demangled.empty() && demangled.back() == ' ')
                demangled.pop_back();
            auto lastSpace = demangled.rfind(' ');
            if (lastSpace != std::string::npos) {
                auto candidate = demangled.substr(lastSpace + 1);
                if (!candidate.empty() &&
                    (std::isalpha(static_cast<unsigned char>(candidate[0])) || candidate[0] == '_'))
                    demangled = candidate;
            }
            if (jitQualifiedNames.count(demangled)) {
                jitFuncPtrs.push_back({func.getName().str(), &func});
            }
        }
    }

    // Symbol obfuscation (after all passes)
    result.obfuscation = SymbolObfuscator::obfuscate(*module_, visEntries, mapping_, config.obfMode, config.obfSalt);
    markPassFired(*module_, "SymbolObfuscator", static_cast<unsigned>(result.obfuscation.renamedCount));

    // Promote JIT-exported functions to external linkage (after obfuscation)
    for (auto& [origName, func] : jitFuncPtrs) {
        if (!func->isDeclaration()) func->setLinkage(llvm::GlobalValue::ExternalLinkage);

        JitExportEntry entry;
        entry.originalMangledName = origName;
        entry.currentMangledName = func->getName().str();
        result.jitExports.push_back(std::move(entry));
    }

    return result;
}

check::PostTransformResult LLVMTransformBackend::postTransformVerify(
    const std::vector<VisibilityEntry>& visEntries,
    bool obfuscationEnabled) {
    LLVMPostTransformVerifier ptv;
    // Re-map symbols against the transformed module to capture any renames
    SymbolMapper mapper(ctx_);
    auto postMapping = mapper.mapSymbols(*module_, visEntries);
    return ptv.verifyModule(*module_, postMapping, visEntries, obfuscationEnabled);
}

std::string LLVMTransformBackend::writeIR(const std::string& outputDir) {
    std::string path = (fs::path(outputDir) / "optimized.ll").string();
    std::error_code ec;
    llvm::raw_fd_ostream irOut(path, ec);
    if (ec) {
        std::cerr << "error: cannot write IR: " << ec.message() << "\n";
        return "";
    }
    // Emit debug info in the stable intrinsic form (`call void
    // @llvm.dbg.value(...)`) rather than LLVM's newer debug-record syntax
    // (`#dbg_value(...)`). This textual IR is re-parsed by the bundled
    // clang at the link step (CppDriver::linkCpp); the LLVM 22 prebuilt's
    // IR reader rejects `#dbg_value` with "expected instruction opcode",
    // failing the link on a standalone build (the macOS meta build happened
    // not to round-trip through this path). Normalising to the intrinsic
    // form every LLVM version parses keeps the written .ll portable. The
    // module is not used after this point (linkCpp re-reads the file), and
    // the conversion leaves the `!topo.fired.<Pass>` named metadata — which
    // the e2e harness scans — untouched.
    module_->convertFromNewDbgValues();
    module_->print(irOut, nullptr);
    lastWrittenIRPath_ = path;
    return path;
}

bool LLVMTransformBackend::dumpIR(const std::string& dumpPath) {
    if (lastWrittenIRPath_.empty()) return false;
    std::error_code ec;
    fs::copy_file(lastWrittenIRPath_, dumpPath, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::unique_ptr<IRTransformBackend> createLLVMBackend() {
    return std::make_unique<LLVMTransformBackend>();
}

} // namespace topo::backend
