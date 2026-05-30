// topo-build-llvm-rust — LLVM backend for Rust projects
//
// Reads a JSON request file (argv[1]) from topo-build, executes Steps 3-7:
//   3. Compile Rust sources to LLVM bitcode
//   4. Load and link IR modules
//   5. Map symbols + verify Topo/IR consistency
//   6. Apply visibility + optimization passes + embed + obfuscate
//   7. Link optimized IR -> final binary via RustDriver

#include "RustDriver.h"

#include "topo/Basic/Diagnostic.h"
#include "topo/Build/AutoLink.h"
#include "topo/Backend/LLVMTransformBackend.h"
#include "topo/Backend/PassReportsSidecar.h"
#include "topo/Build/BackendProtocol.h"
#include "topo/Platform/Platform.h"
#include "topo/Platform/ToolResolution.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

// ============================================================
// backendExtras per-value validators (LLVM Rust backend). Same
// rationale as topo-build-llvm-cpp — see that file for context.
// ============================================================

static bool expectStringIfPresent(const nlohmann::json& extras, const char* key) {
    if (!extras.contains(key)) return true;
    const auto& v = extras.at(key);
    if (!v.is_string()) {
        std::cerr << "error: backendExtras." << key
                  << ": expected string, got " << v.type_name() << "\n";
        return false;
    }
    return true;
}

/// Resolve the clang++ that links this build, preferring the LLVM bundled
/// with THIS backend tool. The IR is produced by the libLLVM linked into this
/// tool, so the reader clang must share its major version; topo-core's generic
/// resolver falls back to a bare `"clang++"` on PATH (= the system clang, older
/// on a stock Linux runner) when topo-core was built zero-LLVM. This executable
/// bakes its own bundled `TOPO_LLVM_BINDIR`, so prefer that. See
/// topo-build-llvm-cpp/main.cpp for the full rationale.
static std::string resolveBundledClangxx() {
#ifdef TOPO_LLVM_BINDIR
    if (std::string_view(TOPO_LLVM_BINDIR).size() > 0) {
        fs::path bundled = fs::path(TOPO_LLVM_BINDIR) / "clang++";
        if constexpr (topo::platform::IsWindows) {
            if (!fs::exists(bundled) && bundled.extension().empty()) {
                bundled = fs::path(TOPO_LLVM_BINDIR) /
                          ("clang++" + std::string(topo::platform::ExeSuffix));
            }
        }
        if (fs::exists(bundled)) {
            return bundled.string();
        }
    }
#endif
    return topo::platform::resolveLLVMTool("clang++");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <request.json>\n"
                  << "  Backend tool invoked by topo-build. Not intended for direct use.\n";
        return 1;
    }

    // Read the JSON request file
    std::string requestPath = argv[1];
    std::ifstream ifs(requestPath);
    if (!ifs) {
        std::cerr << "error: cannot open request file '" << requestPath << "'\n";
        return 1;
    }

    std::ostringstream buf;
    buf << ifs.rdbuf();
    std::string jsonStr = buf.str();

    topo::build::BackendRequest req;
    if (!topo::build::deserializeBackendRequest(jsonStr, req)) {
        std::cerr << "error: failed to parse backend request JSON\n";
        return 1;
    }

    // Per-value validation of backendExtras inputs (see topo-build-llvm-cpp
    // for the rationale). Catches typos before cargo/rustc is reached.
    if (!expectStringIfPresent(req.backendExtras, "hostCompilerPath")) return 1;
    if (!expectStringIfPresent(req.backendExtras, "standard")) return 1;
    if (!expectStringIfPresent(req.backendExtras, "cargoPath")) return 1;

    // ================================================================
    // Step 3: Compile Rust sources to LLVM bitcode
    // ================================================================
    std::string hostCompiler = req.backendExtras.value("hostCompilerPath", std::string());
    std::string standard = req.backendExtras.value("standard", std::string("c++17"));
    std::string cargoPath = req.backendExtras.value("cargoPath", std::string("cargo"));

    topo::build::BuildConfig compileCfg;
    compileCfg.language = topo::HostLanguage::Rust;
    compileCfg.cargoPath = cargoPath;
    compileCfg.verbose = req.verbose;

    auto compileResult = topo::build::compileRust(compileCfg);
    if (compileResult.exitCode != 0) return 1;

    // compileRust returns [bcPath, rlibPath]
    std::string rlibPath;
    if (compileResult.outputFiles.size() >= 2) {
        rlibPath = compileResult.outputFiles[1];
    }

    // Only pass .bc files to the IR transform backend
    std::vector<std::string> irFiles;
    if (!compileResult.outputFiles.empty()) {
        irFiles.push_back(std::move(compileResult.outputFiles[0]));
    }

    // ================================================================
    // Steps 4-6: IR Transform Backend
    // ================================================================
    auto backend = topo::backend::createLLVMBackend();

    // Step 4: Link all IR modules
    std::cerr << "[4/7] Linking " << irFiles.size() << " IR module(s)...\n";

    if (!backend->loadAndLink(irFiles)) {
        return 1;
    }

    // Step 5: SymbolMapper + Verifier
    std::cerr << "[5/7] Mapping and verifying symbols...\n";

    auto mappingResult = backend->mapSymbols(req.visibilityEntries);

    if (req.config.dumpMap) {
        std::cerr << "=== Symbol Mapping ===\n";
        std::cerr << "  Matched (" << mappingResult.matched.size() << "):\n";
        for (const auto& [name, irName] : mappingResult.matched) {
            std::cerr << "    " << name << " -> " << irName << "\n";
        }
        if (!mappingResult.unmatchedTopo.empty()) {
            std::cerr << "  Unmatched Topo (" << mappingResult.unmatchedTopo.size() << "):\n";
            for (const auto& name : mappingResult.unmatchedTopo) {
                std::cerr << "    [warn] " << name << "\n";
            }
        }
        std::cerr << "\n";
    }

    topo::DiagnosticEngine diag;

    if (!req.config.noVerify) {
        auto vr = backend->verify(diag, req.symbolTable, req.visibilityEntries);

        if (!vr.passed()) {
            std::cerr << "Verification failed:\n";
            if (vr.publicMissing > 0) std::cerr << "  publicMissing: " << vr.publicMissing << "\n";
            if (vr.blockMismatches > 0) std::cerr << "  blockMismatches: " << vr.blockMismatches << "\n";
            if (vr.signatureMismatches > 0) std::cerr << "  signatureMismatches: " << vr.signatureMismatches << "\n";
            if (vr.constMismatches > 0) std::cerr << "  constMismatches: " << vr.constMismatches << "\n";
            if (vr.classMemberMissing > 0) std::cerr << "  classMemberMissing: " << vr.classMemberMissing << "\n";
            if (vr.stageOrderViolations > 0) std::cerr << "  stageOrderViolations: " << vr.stageOrderViolations << "\n";
            if (vr.pipelineEdgeMismatches > 0)
                std::cerr << "  pipelineEdgeMismatches: " << vr.pipelineEdgeMismatches << "\n";
            diag.print(std::cerr);
            if (!req.config.warnOnly) return 1;
            std::cerr << "      (continuing due to --warn-only)\n";
        }
    }

    std::cerr << "      " << mappingResult.matched.size() << " symbols matched\n";

    // Step 6: VisibilityApplier + PassPipeline
    std::cerr << "[6/7] Applying visibility + O" << static_cast<int>(req.config.optLevel) << " optimization"
              << (req.config.buildMode == topo::BuildMode::Aggressive ? " (aggressive/ThinLTO)" : "") << "...\n";

    auto backendResult = backend->optimize(req.config, req.symbolTable, req.visibilityEntries);

    std::cerr << "      visibility: " << backendResult.visibilityStats.publicCount << " public, "
              << backendResult.visibilityStats.protectedCount << " protected, "
              << backendResult.visibilityStats.privateCount << " private, "
              << backendResult.visibilityStats.internalCount << " internal\n";

    if (backendResult.embeddedIRBytes > 0) {
        std::cerr << "      embedded " << backendResult.embeddedIRBytes << " bytes IR + "
                  << backendResult.embeddedMetaBytes << " bytes metadata\n";
    }

    if (backendResult.obfuscation.renamedCount > 0) {
        std::cerr << "      obfuscated " << backendResult.obfuscation.renamedCount << " symbol(s)\n";
    }

    // Write optimized IR
    fs::path tempDir(req.tempDir);
    auto optIRPath = backend->writeIR(tempDir.string());
    if (optIRPath.empty()) {
        return 1;
    }

    if (req.config.dumpIR) {
        std::string dumpPath = req.config.outputPath + ".ll";
        if (backend->dumpIR(dumpPath)) {
            std::cerr << "      IR dumped to " << dumpPath << "\n";
        }
    }

    // ================================================================
    // Step 7: Link optimized IR -> final output (Rust Driver)
    // ================================================================

    // Reconstruct the BuildConfig fields needed for linking
    topo::build::BuildConfig linkCfg;
    linkCfg.language = topo::HostLanguage::Rust;
    linkCfg.outputPath = req.outputPath;
    linkCfg.outputType = req.config.outputType;
    linkCfg.optLevel = req.config.optLevel;
    linkCfg.buildMode = req.config.buildMode;
    linkCfg.hostCompilerPath = hostCompiler.empty() ? resolveBundledClangxx() : hostCompiler;
    linkCfg.standard = standard;
    linkCfg.linkLibs = req.linkLibs;
    linkCfg.linkDirs = req.linkDirs;
    linkCfg.verbose = req.verbose;

    // Ensure runtime auto-link (covers direct backend invocation without topo-build)
    topo::build::injectAutoLinkLibs(req.config, linkCfg.linkLibs, linkCfg.linkDirs);

    auto linkResult = topo::build::linkRust(linkCfg, optIRPath, tempDir, rlibPath);

    if (linkResult.exitCode != 0) {
        std::cerr << "error: linking failed (exit " << linkResult.exitCode << ")\n";
        return 1;
    }

    // ================================================================
    // Step 8: Per-Pass report sidecar (mirrors topo-build-llvm-cpp)
    // ================================================================
    {
        auto* llvmBackend =
            static_cast<topo::backend::LLVMTransformBackend*>(backend.get());
        if (!topo::backend::writePassReportsSidecar(llvmBackend->passReports(),
                                                    req.outputPath)) {
            std::cerr << "warning: failed to write some Pass report sidecars "
                         "(build continues)\n";
        }
    }

    return 0;
}
