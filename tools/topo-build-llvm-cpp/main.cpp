// topo-build-llvm-cpp — LLVM backend for C++ projects
//
// Reads a JSON request file (argv[1]) from topo-build, executes Steps 3-7:
//   3. Compile C++ sources to LLVM IR
//   4. Load and link IR modules
//   5. Map symbols + verify Topo/IR consistency
//   6. Apply visibility + optimization passes + embed + obfuscate
//   7. Link optimized IR -> final binary via CppDriver

#include "CppDriver.h"
#include "NinjaGen.h"

#include "topo/Basic/Diagnostic.h"
#include "topo/Build/AutoLink.h"
#include "topo/Backend/PassReportsSidecar.h"
#include "topo/Build/BackendProtocol.h"
#include "topo/Build/IncrementalCache.h"
#include "topo/Backend/LLVMTransformBackend.h"
#include "topo/Platform/ToolResolution.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

// ============================================================
// backendExtras per-value validators (LLVM C++ backend).
//
// Mirrors the JVM backend pattern in topo-jvm/tools/topo-build-jvm-java/
// main.cpp. Diagnostic shape:
//   `error: backendExtras.<key>: <reason>`
// Runs after deserializeBackendRequest accepted the payload and before
// any compile step is reached, so a malformed value never propagates
// into a clang/linker diagnostic.
// ============================================================

/// Expect `backendExtras[key]` to be a JSON string (when present). Returns
/// false on type mismatch with a key-pointing diagnostic. Absent key is OK.
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

    // Per-value validation of backendExtras inputs. Centralised unknown-key
    // rejection (deserializeBackendRequest) only runs for JVM today; for
    // LLVM-cpp the schema is silent-tolerant on unknown keys but every
    // *known* key still has a fixed JSON type. Reject bad values here so
    // a typo like `"standard": 17` produces a key-pointing error rather
    // than a downstream `clang: argument has invalid type` diagnostic.
    if (!expectStringIfPresent(req.backendExtras, "hostCompilerPath")) return 1;
    if (!expectStringIfPresent(req.backendExtras, "standard")) return 1;

    // ================================================================
    // Step 3: Compile C++ sources to LLVM IR
    // ================================================================
    std::string hostCompiler = req.backendExtras.value("hostCompilerPath", std::string());
    std::string standard = req.backendExtras.value("standard", std::string("c++17"));

    topo::build::BuildConfig compileCfg;
    compileCfg.language = topo::HostLanguage::Cpp;
    compileCfg.sources = req.sources;
    compileCfg.includeDirs = req.includeDirs;
    compileCfg.hostCompilerPath = hostCompiler.empty() ? topo::platform::resolveLLVMTool("clang++") : hostCompiler;
    compileCfg.standard = standard;
    compileCfg.outputType = req.config.outputType;
    compileCfg.embedIR = req.config.embedIR;
    compileCfg.adaptiveCfg = req.config.adaptiveCfg;
    compileCfg.verbose = req.verbose;

    fs::path tempDir(req.tempDir);
    std::vector<std::string> irFiles;

    bool useIncremental = !req.noIncremental;
    fs::path projectDir = fs::current_path();
    topo::build::IncrementalCache cache(projectDir);

    if (useIncremental && topo::build::NinjaGen::isAvailable()) {
        auto result = topo::build::compileCppIncremental(compileCfg, cache.cacheDir());
        if (result.exitCode != 0) return 1;
        irFiles = std::move(result.outputFiles);
    } else {
        auto result = topo::build::compileCpp(compileCfg, useIncremental ? cache.irDir() : tempDir);
        if (result.exitCode != 0) return 1;
        irFiles = std::move(result.outputFiles);
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

    // Post-transform verification: check that transforms preserved .topo invariants
    if (!req.config.noVerify) {
        bool obfuscationEnabled = (backendResult.obfuscation.renamedCount > 0);
        auto ptResult = backend->postTransformVerify(req.visibilityEntries, obfuscationEnabled);

        if (!ptResult.passed()) {
            std::cerr << "Post-transform verification failed:\n";
            for (const auto& d : ptResult.diagnostics) {
                if (d.severity == topo::check::Severity::Error) {
                    std::cerr << "  error: " << d.message << "\n";
                }
            }
            if (!req.config.warnOnly) return 1;
            std::cerr << "      (continuing due to --warn-only)\n";
        }

        // Print warnings regardless of pass/fail
        for (const auto& d : ptResult.diagnostics) {
            if (d.severity == topo::check::Severity::Warning) {
                std::cerr << "  warning: " << d.message << "\n";
            }
        }

        if (req.verbose) {
            std::cerr << "      post-transform: " << ptResult.visibilityVerified
                      << " visibility checks passed";
            if (obfuscationEnabled) {
                std::cerr << ", " << ptResult.obfuscationVerified
                          << " obfuscation checks passed";
            }
            std::cerr << "\n";
        }
    }

    // Write optimized IR
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
    // Step 7: Link optimized IR -> final output (C++ Driver)
    // ================================================================

    // Reconstruct the BuildConfig fields needed for linking
    topo::build::BuildConfig linkCfg;
    linkCfg.language = topo::HostLanguage::Cpp;
    linkCfg.outputPath = req.outputPath;
    linkCfg.outputType = req.config.outputType;
    linkCfg.optLevel = req.config.optLevel;
    linkCfg.buildMode = req.config.buildMode;
    linkCfg.hostCompilerPath = hostCompiler.empty() ? topo::platform::resolveLLVMTool("clang++") : hostCompiler;
    linkCfg.standard = standard;
    linkCfg.linkLibs = req.linkLibs;
    linkCfg.linkDirs = req.linkDirs;
    linkCfg.verbose = req.verbose;

    // Ensure runtime auto-link (covers direct backend invocation without topo-build)
    topo::build::injectAutoLinkLibs(req.config, linkCfg.linkLibs, linkCfg.linkDirs);

    auto linkResult = topo::build::linkCpp(linkCfg, optIRPath, tempDir, backendResult.jitExports);

    if (linkResult.exitCode != 0) {
        std::cerr << "error: linking failed (exit " << linkResult.exitCode << ")\n";
        return 1;
    }

    // ================================================================
    // Step 8: Per-Pass report sidecar
    // ================================================================
    //
    // Write `<output>.topo-passes/<PassName>.json` so LSP, `topo debug`,
    // and other consumers can read each judging Pass's decisions as static
    // build artefacts (pass reports are owned by the backend). The
    // sidecar is non-load-bearing — failure logs but doesn't abort the
    // build. Reports owned by the LLVM backend; static_cast is safe because
    // this tool only ever uses createLLVMBackend() (LLVM disables RTTI so
    // dynamic_cast is unavailable).
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
