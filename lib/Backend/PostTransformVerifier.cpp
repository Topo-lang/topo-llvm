#include "topo/Backend/PostTransformVerifier.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Support/MemoryBuffer.h>

#include <filesystem>
#include <unordered_map>

namespace topo::backend {

check::PostTransformResult LLVMPostTransformVerifier::verifyModule(
    llvm::Module& module,
    const SymbolMapping& mapping,
    const std::vector<VisibilityEntry>& visEntries,
    bool obfuscationEnabled) {

    check::PostTransformResult result;

    // Build visibility map from entries
    std::unordered_map<std::string, Visibility> expectedVisibility;
    for (const auto& entry : visEntries) {
        expectedVisibility[entry.qualifiedName] = entry.visibility;
    }

    // Check each mapped function
    for (const auto& [topoName, llvmFunc] : mapping.matched) {
        if (!llvmFunc) continue;

        // 1. Verify visibility via LLVM linkage
        auto it = expectedVisibility.find(topoName);
        if (it != expectedVisibility.end()) {
            Visibility expected = it->second;
            llvm::GlobalValue::LinkageTypes linkage = llvmFunc->getLinkage();

            // Determine actual linkage category for diagnostic messages
            const char* actual;
            if (linkage == llvm::GlobalValue::ExternalLinkage) {
                actual = "public";
            } else if (linkage == llvm::GlobalValue::InternalLinkage ||
                       linkage == llvm::GlobalValue::PrivateLinkage) {
                actual = "private";
            } else {
                actual = "internal";
            }

            // Map Topo visibility to expected LLVM linkage:
            //   public     -> ExternalLinkage
            //   private/protected -> InternalLinkage or PrivateLinkage
            //   internal   -> anything except ExternalLinkage
            //   ignore     -> skip check
            bool match = false;
            switch (expected) {
            case Visibility::Public:
                match = (linkage == llvm::GlobalValue::ExternalLinkage);
                break;
            case Visibility::Private:
            case Visibility::Protected:
                match = (linkage == llvm::GlobalValue::InternalLinkage ||
                         linkage == llvm::GlobalValue::PrivateLinkage);
                break;
            case Visibility::Internal:
                match = (linkage != llvm::GlobalValue::ExternalLinkage);
                break;
            case Visibility::Ignore:
                match = true; // skip
                break;
            }

            if (match) {
                result.visibilityVerified++;
            } else {
                result.visibilityMismatch++;
                check::CheckDiagnostic diag;
                diag.severity = check::Severity::Error;
                diag.check = "post-transform-visibility";
                diag.message = "linkage mismatch for '" + topoName +
                               "': expected " + visibilityName(expected) +
                               " but got " + actual;
                result.diagnostics.push_back(std::move(diag));
            }
        }

        // 2. Verify obfuscation (if enabled)
        if (obfuscationEnabled) {
            auto visIt = expectedVisibility.find(topoName);
            if (visIt != expectedVisibility.end()) {
                bool shouldBeObfuscated =
                    (visIt->second == Visibility::Private ||
                     visIt->second == Visibility::Internal);
                bool nameIsObfuscated =
                    llvmFunc->getName().starts_with("_topo_") ||
                    llvmFunc->getName().size() > 20; // hashed names are long

                if (shouldBeObfuscated && !nameIsObfuscated) {
                    result.obfuscationMismatch++;
                    check::CheckDiagnostic diag;
                    diag.severity = check::Severity::Warning;
                    diag.check = "post-transform-obfuscation";
                    diag.message = "'" + topoName +
                                   "' should be obfuscated but retains original name: " +
                                   llvmFunc->getName().str();
                    result.diagnostics.push_back(std::move(diag));
                } else {
                    result.obfuscationVerified++;
                }
            }
        }
    }

    // 3. Check for missing symbols (declared in .topo but not in IR)
    for (const auto& unmapped : mapping.unmatchedTopo) {
        check::CheckDiagnostic diag;
        diag.severity = check::Severity::Warning;
        diag.check = "post-transform-existence";
        diag.message = "function '" + unmapped +
                       "' declared in .topo but not found in transformed IR "
                       "(may have been inlined or dead-code eliminated)";
        result.diagnostics.push_back(std::move(diag));
    }

    return result;
}

check::PostTransformResult LLVMPostTransformVerifier::verify(
    const SymbolTable& symbols,
    const std::vector<VisibilityEntry>& visEntries,
    const std::string& artifactDir) {

    check::PostTransformResult result;

    // Find the output bitcode/IR file in artifactDir
    std::string bcPath;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(artifactDir, ec)) {
        auto ext = entry.path().extension().string();
        if (ext == ".bc" || ext == ".ll") {
            bcPath = entry.path().string();
            break;
        }
    }

    if (bcPath.empty()) {
        check::CheckDiagnostic diag;
        diag.severity = check::Severity::Warning;
        diag.check = "post-transform";
        diag.message = "no bitcode file found in " + artifactDir +
                       " -- skipping post-transform verification";
        result.diagnostics.push_back(std::move(diag));
        return result;
    }

    // Load the bitcode module
    auto bufOrErr = llvm::MemoryBuffer::getFile(bcPath);
    if (!bufOrErr) {
        check::CheckDiagnostic diag;
        diag.severity = check::Severity::Error;
        diag.check = "post-transform";
        diag.message = "failed to read bitcode: " + bcPath;
        result.diagnostics.push_back(std::move(diag));
        return result;
    }

    llvm::LLVMContext ctx;
    auto modOrErr = llvm::parseBitcodeFile((*bufOrErr)->getMemBufferRef(), ctx);
    if (!modOrErr) {
        check::CheckDiagnostic diag;
        diag.severity = check::Severity::Error;
        diag.check = "post-transform";
        diag.message = "failed to parse bitcode: " + bcPath;
        result.diagnostics.push_back(std::move(diag));
        return result;
    }

    // Map symbols and verify
    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(**modOrErr, visEntries);
    bool obfuscation = false; // standalone verify cannot determine obfuscation config
    return verifyModule(**modOrErr, mapping, visEntries, obfuscation);
}

} // namespace topo::backend
