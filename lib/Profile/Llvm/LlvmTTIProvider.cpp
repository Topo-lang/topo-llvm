// LlvmTTIProvider — LLVM-bound static TTI estimation for `topo-prof`.
//
// This is a verbatim relocation of the legacy
// computeFunctionTTICost() + buildTTIMap() from
// topo-llvm/tools/topo-prof/main.cpp. The only change is structural — the
// hard-coded std::cerr was replaced by the caller-supplied `err` stream
// (the shim passes std::cerr, so the warning text on stderr is unchanged),
// and toml++ here only checks Topo.toml existence/parse exactly as before.

#include "Profile/Llvm/LlvmTTIProvider.h"

#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <cctype>
#include <filesystem>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace topo {
namespace profile {
namespace llvm_backend {

// TTI cost computation (shared with TopoParallelPass)
static uint64_t computeFunctionTTICost(llvm::Function& func, llvm::TargetTransformInfo& TTI) {
    uint64_t cost = 0;
    for (auto& BB : func) {
        for (auto& I : BB) {
            auto ic = TTI.getInstructionCost(&I, llvm::TargetTransformInfo::TCK_RecipThroughput);
            if (ic.isValid())
                cost += static_cast<uint64_t>(ic.getValue());
            else
                cost += 1;
        }
    }
    return cost;
}

std::map<std::string, uint64_t> LlvmTTIProvider::buildTTIMap(const std::string& projectDir,
                                                             std::ostream& err) {
    std::map<std::string, uint64_t> ttiMap;

    fs::path tomlPath = fs::path(projectDir) / "Topo.toml";
    if (!fs::exists(tomlPath)) {
        err << "warning: Topo.toml not found in " << projectDir << ", skipping TTI estimates\n";
        return ttiMap;
    }

    auto result = toml::parse_file(tomlPath.string());
    if (!result) {
        err << "warning: failed to parse Topo.toml: " << result.error() << "\n";
        return ttiMap;
    }

    // Determine build directory and find IR files
    fs::path baseDir = tomlPath.parent_path();
    fs::path buildDir = baseDir / "build";

    // Collect .ll files from build directory
    std::vector<fs::path> irFiles;
    if (fs::exists(buildDir)) {
        for (auto& entry : fs::directory_iterator(buildDir)) {
            if (entry.path().extension() == ".ll") {
                irFiles.push_back(entry.path());
            }
        }
    }

    if (irFiles.empty()) {
        err << "warning: no .ll files found in " << buildDir.string() << ", skipping TTI estimates\n";
        return ttiMap;
    }

    // Initialize LLVM target for TTI
    llvm::InitializeNativeTarget();

    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    std::string error;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        err << "warning: cannot get target for TTI: " << error << "\n";
        return ttiMap;
    }

    std::unique_ptr<llvm::TargetMachine> TM(
        target->createTargetMachine(triple, "generic", "", llvm::TargetOptions(), std::nullopt));

    // Load each IR file and compute TTI costs
    for (const auto& irPath : irFiles) {
        llvm::LLVMContext ctx;
        llvm::SMDiagnostic smErr;
        auto mod = llvm::parseIRFile(irPath.string(), smErr, ctx);
        if (!mod) continue;

        for (auto& func : *mod) {
            if (func.isDeclaration()) continue;

            auto TTI = TM->getTargetTransformInfo(func);
            uint64_t cost = computeFunctionTTICost(func, TTI);

            // Demangle the function name to get qualified name
            std::string demangled = llvm::demangle(func.getName().str());
            if (demangled == func.getName().str()) {
                // Demangling failed, use raw name
                demangled = func.getName().str();
            }
            // Strip parameter list
            auto parenPos = demangled.find('(');
            if (parenPos != std::string::npos) {
                demangled = demangled.substr(0, parenPos);
            }
            // Strip return type / calling convention prefix
            auto lastSpace = demangled.rfind(' ');
            if (lastSpace != std::string::npos) {
                std::string candidate = demangled.substr(lastSpace + 1);
                if (!candidate.empty() &&
                    (std::isalpha(static_cast<unsigned char>(candidate[0])) || candidate[0] == '_')) {
                    demangled = candidate;
                }
            }

            if (!demangled.empty()) {
                ttiMap[demangled] = cost;
            }
        }
    }

    return ttiMap;
}

} // namespace llvm_backend
} // namespace profile
} // namespace topo
