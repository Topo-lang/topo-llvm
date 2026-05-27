// topo-jit-engine: LLVM-heavy JIT specialization engine.
// Built as a shared library, dynamically loaded by topo-jit-api at runtime.

#define TOPO_JIT_ENGINE_BUILDING
#include "topo/rt/jit_engine_rt.h"

#include "topo/Sema/SymbolTable.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Transforms/TopoParallelPass.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

// Platform-specific section reading
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <fstream>
#endif

namespace {

// ============================================================
// Section reading utilities (same as original topo_jit.cpp)
// ============================================================

struct SectionData {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

#ifdef _WIN32
SectionData readSection(const char* sectionName) {
    HMODULE hModule = GetModuleHandle(nullptr);
    if (!hModule) return {};

    std::string cofName = std::string(sectionName) + "$";

    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(hModule);
    PIMAGE_NT_HEADERS ntHeaders =
        reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t*>(hModule) + dosHeader->e_lfanew);
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(ntHeaders);

    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i) {
        std::string name(reinterpret_cast<char*>(section[i].Name), 8);
        auto nullPos = name.find('\0');
        if (nullPos != std::string::npos) name.erase(nullPos);

        if (name == sectionName || name == cofName) {
            auto* base = reinterpret_cast<uint8_t*>(hModule) + section[i].VirtualAddress;
            return {base, section[i].Misc.VirtualSize};
        }
    }
    return {};
}
#elif defined(__APPLE__)
SectionData readSection(const char* sectionName) {
    std::string macSectName;
    if (sectionName[0] == '.') {
        macSectName = "__" + std::string(sectionName + 1);
    } else {
        macSectName = "__" + std::string(sectionName);
    }

    // Iterate loaded images to find section data — works from shared libraries
    // (unlike _mh_execute_header which is only available in the main executable)
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header_64* header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(i));
        if (!header) continue;

        unsigned long size = 0;
        const uint8_t* data = getsectiondata(header, "__DATA", macSectName.c_str(), &size);
        if (data && size > 0) return {data, static_cast<size_t>(size)};
    }
    return {};
}
#else
SectionData readSection(const char* sectionName) {
    uintptr_t baseAddr = 0;
    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* data) -> int {
            auto* base = static_cast<uintptr_t*>(data);
            if (info->dlpi_name[0] == '\0') {
                *base = info->dlpi_addr;
                return 1;
            }
            return 0;
        },
        &baseAddr);

    std::ifstream elf("/proc/self/exe", std::ios::binary);
    if (!elf.is_open()) return {};

    Elf64_Ehdr ehdr;
    elf.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if (!elf.good()) return {};

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 || ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3)
        return {};

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    elf.seekg(static_cast<std::streamoff>(ehdr.e_shoff));
    elf.read(reinterpret_cast<char*>(shdrs.data()), static_cast<std::streamsize>(ehdr.e_shnum * sizeof(Elf64_Shdr)));
    if (!elf.good()) return {};

    if (ehdr.e_shstrndx >= ehdr.e_shnum) return {};
    auto& shstrtab = shdrs[ehdr.e_shstrndx];
    std::vector<char> strtab(shstrtab.sh_size);
    elf.seekg(static_cast<std::streamoff>(shstrtab.sh_offset));
    elf.read(strtab.data(), static_cast<std::streamsize>(shstrtab.sh_size));
    if (!elf.good()) return {};

    for (const auto& shdr : shdrs) {
        if (shdr.sh_name >= strtab.size()) continue;
        const char* name = strtab.data() + shdr.sh_name;
        if (std::strcmp(name, sectionName) == 0 && shdr.sh_size > 0) {
            auto* data = reinterpret_cast<const uint8_t*>(baseAddr + shdr.sh_addr);
            return {data, static_cast<size_t>(shdr.sh_size)};
        }
    }

    return {};
}
#endif

// ============================================================
// Metadata parsing
// ============================================================

struct PipelineMeta {
    std::string name;
    nlohmann::json stages;
    nlohmann::json edges;
    nlohmann::json demand;
    std::vector<std::string> sourceNodes;
    std::string terminalNode;
    std::string terminalType;
    std::vector<std::string> calledFunctions;
};

bool parseMetadata(const std::string& json, std::vector<PipelineMeta>& pipelines, nlohmann::json& parallelCfg) {
    auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded()) return false;

    if (doc.contains("parallel")) parallelCfg = doc["parallel"];

    if (doc.contains("pipelines")) {
        for (const auto& p : doc["pipelines"]) {
            PipelineMeta meta;
            meta.name = p.value("name", "");
            meta.stages = p.value("stages", nlohmann::json::object());
            meta.edges = p.value("edges", nlohmann::json::array());
            meta.demand = p.value("demand", nlohmann::json::object());
            meta.terminalNode = p.value("terminalNode", "");
            meta.terminalType = p.value("terminalType", "");
            if (p.contains("sourceNodes")) {
                for (const auto& s : p["sourceNodes"])
                    meta.sourceNodes.push_back(s.get<std::string>());
            }
            if (p.contains("calledFunctions")) {
                for (const auto& cf : p["calledFunctions"])
                    meta.calledFunctions.push_back(cf.get<std::string>());
            }
            pipelines.push_back(std::move(meta));
        }
    }
    return true;
}

// ============================================================
// Context deserialization from JSON
// ============================================================

struct ContextData {
    struct PrunedEdge {
        std::string source, target;
    };
    struct NarrowedReturn {
        std::string func;
        std::vector<std::string> fields;
    };
    std::vector<PrunedEdge> prunedEdges;
    std::vector<NarrowedReturn> narrowedReturns;
};

ContextData deserializeContext(const char* json, size_t len) {
    ContextData ctx;
    auto doc = nlohmann::json::parse(std::string(json, len), nullptr, false);
    if (doc.is_discarded()) return ctx;

    if (doc.contains("prunedEdges")) {
        for (const auto& e : doc["prunedEdges"]) {
            ctx.prunedEdges.push_back({e.value("source", ""), e.value("target", "")});
        }
    }
    if (doc.contains("narrowedReturns")) {
        for (const auto& nr : doc["narrowedReturns"]) {
            ContextData::NarrowedReturn entry;
            entry.func = nr.value("func", "");
            if (nr.contains("fields")) {
                for (const auto& f : nr["fields"])
                    entry.fields.push_back(f.get<std::string>());
            }
            ctx.narrowedReturns.push_back(std::move(entry));
        }
    }
    return ctx;
}

std::unordered_map<std::string, uint64_t> deserializeCosts(const char* json, size_t len) {
    std::unordered_map<std::string, uint64_t> costs;
    auto doc = nlohmann::json::parse(std::string(json, len), nullptr, false);
    if (doc.is_discarded()) return costs;
    for (auto& [key, val] : doc.items()) {
        if (val.is_number_unsigned()) costs[key] = val.get<uint64_t>();
    }
    return costs;
}

// ============================================================
// Apply context constraints to metadata
// ============================================================

/// Debug logging (enabled via TOPO_JIT_VERBOSE=1)
static bool jitVerbose() {
    static const bool enabled = [] {
        const char* env = std::getenv("TOPO_JIT_VERBOSE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void applyConstraints(std::vector<PipelineMeta>& pipelines, const ContextData& ctx) {
    if (jitVerbose()) {
        llvm::errs() << "[topo-jit] applyConstraints: "
                     << ctx.prunedEdges.size() << " pruned edges, "
                     << ctx.narrowedReturns.size() << " narrowed returns\n";
    }

    for (auto& pipeline : pipelines) {
        for (const auto& pruned : ctx.prunedEdges) {
            auto& edgesArray = pipeline.edges;
            for (auto it = edgesArray.begin(); it != edgesArray.end();) {
                bool sourceMatch = (pruned.source == "*" || (*it)["source"] == pruned.source);
                bool targetMatch = (pruned.target == "*" || (*it)["target"] == pruned.target);
                if (sourceMatch && targetMatch) {
                    if (jitVerbose()) {
                        llvm::errs() << "[topo-jit] pruned edge: "
                                     << (*it)["source"].get<std::string>() << " -> "
                                     << (*it)["target"].get<std::string>()
                                     << " (pipeline '" << pipeline.name << "')\n";
                    }
                    it = edgesArray.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& narrow : ctx.narrowedReturns) {
            if (pipeline.demand.contains(narrow.func)) {
                nlohmann::json newFields = nlohmann::json::array();
                for (const auto& f : narrow.fields)
                    newFields.push_back(f);
                pipeline.demand[narrow.func] = newFields;

                if (jitVerbose()) {
                    llvm::errs() << "[topo-jit] narrowed returns for '"
                                 << narrow.func << "' to " << narrow.fields.size()
                                 << " fields\n";
                }
            }
        }
    }
}

// ============================================================
// Demangling helper
// ============================================================

std::string demangleQualified(const std::string& mangledName) {
    std::string demangled = llvm::demangle(mangledName);
    if (demangled == mangledName) return ""; // demangling failed
    auto parenPos = demangled.find('(');
    if (parenPos != std::string::npos) demangled = demangled.substr(0, parenPos);
    while (!demangled.empty() && demangled.back() == ' ')
        demangled.pop_back();
    auto lastSpace = demangled.rfind(' ');
    if (lastSpace != std::string::npos) {
        std::string candidate = demangled.substr(lastSpace + 1);
        if (!candidate.empty() && (std::isalpha(static_cast<unsigned char>(candidate[0])) || candidate[0] == '_'))
            demangled = candidate;
    }
    return demangled;
}

// ============================================================
// Global JIT state
// ============================================================

static std::mutex g_jit_mutex;
static std::unique_ptr<llvm::orc::LLJIT> g_jit;
static bool g_initialized = false;

bool ensureJIT() {
    std::lock_guard<std::mutex> lock(g_jit_mutex);
    if (g_initialized) return g_jit != nullptr;

    g_initialized = true;

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jitBuilder = llvm::orc::LLJITBuilder();
    auto jitExpected = jitBuilder.create();
    if (!jitExpected) {
        llvm::errs() << "JIT creation failed: " << llvm::toString(jitExpected.takeError()) << "\n";
        return false;
    }
    g_jit = std::move(*jitExpected);

    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

#ifdef _WIN32
    llvm::sys::DynamicLibrary::LoadLibraryPermanently("vcruntime140.dll");
    llvm::sys::DynamicLibrary::LoadLibraryPermanently("ucrtbase.dll");
#endif

    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(g_jit->getDataLayout().getGlobalPrefix());
    if (gen) g_jit->getMainJITDylib().addGenerator(std::move(*gen));

    return true;
}

// ============================================================
// Core specialize implementation
// ============================================================

void* doSpecializeImpl(const std::string& pipelineName,
                       const ContextData& ctx,
                       [[maybe_unused]] const std::unordered_map<std::string, uint64_t>& runtimeCosts,
                       const uint8_t* irData, size_t irSize,
                       const uint8_t* metaData, size_t metaSize) {
    if (jitVerbose()) {
        llvm::errs() << "[topo-jit] doSpecialize('" << pipelineName << "'): "
                     << ctx.prunedEdges.size() << " pruned edges, "
                     << ctx.narrowedReturns.size() << " narrowed returns, "
                     << runtimeCosts.size() << " cost entries\n";
        for (const auto& [name, ns] : runtimeCosts)
            llvm::errs() << "[topo-jit]   cost: " << name << " = " << ns << " ns\n";
    }
    if (!ensureJIT()) return nullptr;

    if (!irData || irSize == 0) return nullptr;

    // Step 2: Parse metadata (optional — callers that inject raw bitcode
    // directly may skip this when no pipeline edge/stage data applies).
    std::vector<PipelineMeta> pipelines;
    nlohmann::json parallelCfgJson;
    if (metaData && metaSize > 0) {
        std::string metaStr(reinterpret_cast<const char*>(metaData), metaSize);
        parseMetadata(metaStr, pipelines, parallelCfgJson);
    }

    // Step 3: Apply context constraints
    applyConstraints(pipelines, ctx);

    // Step 4: Load bitcode
    auto bufOrErr = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(irData), irSize), "", false);
    // MemoryBuffer::getMemBuffer returns std::unique_ptr<MemoryBuffer> which
    // is documented as nullable (allocation failure on memory pressure or
    // hardened-target size-mismatch rejection). The previous code went
    // straight to bufOrErr->getMemBufferRef(), null-dereferencing under
    // those conditions. Guard before deref so callers see a clean nullptr
    // return instead of a SIGSEGV.
    if (!bufOrErr) {
        llvm::errs() << "Failed to allocate MemoryBuffer for embedded bitcode (size=" << irSize << ")\n";
        return nullptr;
    }

    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto moduleOrErr = llvm::parseBitcodeFile(bufOrErr->getMemBufferRef(), *llvmCtx);
    if (!moduleOrErr) {
        llvm::errs() << "Failed to parse embedded bitcode: " << llvm::toString(moduleOrErr.takeError()) << "\n";
        return nullptr;
    }
    auto parsedModule = std::move(moduleOrErr.get());

    // Step 5: Find mangled name
    std::string mangledName;
    for (const auto& func : *parsedModule) {
        if (func.isDeclaration()) continue;
        auto demangled = demangleQualified(func.getName().str());
        if (demangled == pipelineName) {
            mangledName = func.getName().str();
            break;
        }
    }
    if (mangledName.empty()) {
        llvm::errs() << "JIT: no function matching '" << pipelineName << "' found in embedded IR\n";
        return nullptr;
    }

    // Step 6-7: Build mapping, run passes
    topo::SymbolMapping jitMapping;
    {
        // 7a: Build SymbolMapping by demangling
        for (auto& func : *parsedModule) {
            auto demangled = demangleQualified(func.getName().str());
            if (!demangled.empty()) jitMapping.matched[demangled] = &func;
        }

        // 7b: Build synthetic SymbolTable from metadata
        topo::SymbolTable syntheticSymbols;
        for (const auto& meta : pipelines) {
            topo::LogicBlockEntry lb;
            lb.qualifiedName = meta.name;
            auto colonPos = meta.name.rfind("::");
            lb.simpleName = (colonPos != std::string::npos) ? meta.name.substr(colonPos + 2) : meta.name;
            lb.isPipeline = true;
            lb.calledFunctions = meta.calledFunctions;

            for (const auto& e : meta.edges) {
                topo::PipelineEdge edge;
                edge.source = e.value("source", "");
                edge.target = e.value("target", "");
                lb.edges.push_back(edge);
            }

            topo::PipelineAnalysis analysis;
            for (auto& [node, stage] : meta.stages.items())
                analysis.stages[node] = stage.get<int>();
            analysis.sourceNodes = meta.sourceNodes;
            analysis.terminalNode = meta.terminalNode;
            analysis.terminalType = meta.terminalType;
            for (auto& [node, fields] : meta.demand.items()) {
                std::set<std::string> fieldSet;
                for (const auto& f : fields)
                    fieldSet.insert(f.get<std::string>());
                analysis.demand[node] = fieldSet;
            }

            // Prune unreachable nodes
            std::set<std::string> reachable(analysis.sourceNodes.begin(), analysis.sourceNodes.end());
            std::vector<std::string> worklist(analysis.sourceNodes.begin(), analysis.sourceNodes.end());
            while (!worklist.empty()) {
                std::string cur = worklist.back();
                worklist.pop_back();
                for (const auto& edge : lb.edges) {
                    if (edge.source == cur && reachable.find(edge.target) == reachable.end()) {
                        reachable.insert(edge.target);
                        worklist.push_back(edge.target);
                    }
                }
            }
            for (auto it = analysis.stages.begin(); it != analysis.stages.end();) {
                if (reachable.find(it->first) == reachable.end())
                    it = analysis.stages.erase(it);
                else
                    ++it;
            }

            lb.pipelineAnalysis = analysis;
            syntheticSymbols.addLogicBlock(lb);
        }

        // 7c: Rebuild ParallelConfig
        topo::ParallelConfig parallelConfig;
        if (!parallelCfgJson.empty()) {
            parallelConfig.mode =
                parallelCfgJson.value("enabled", true) ? topo::FeatureMode::Auto : topo::FeatureMode::Off;
            parallelConfig.instrument = parallelCfgJson.value("instrument", true);
            if (parallelCfgJson.contains("exclude")) {
                for (const auto& ex : parallelCfgJson["exclude"])
                    parallelConfig.exclude.push_back(ex.get<std::string>());
            }
        }

        // 7d: Run PipelineCodeGenPass
        topo::PipelineCodeGenPass::run(*parsedModule, syntheticSymbols, jitMapping);

        // 7e: Run TopoParallelPass
        if (parallelConfig.isEnabled()) {
            topo::TopoParallelPass::run(*parsedModule, syntheticSymbols, jitMapping, parallelConfig);
        }
    }

    // Step 8: Prepare module for JIT compilation
    {
        // jitCounter is read+incremented OUTSIDE g_jit_mutex (the mutex below
        // covers only the LLJIT dylib/lookup interactions; bitcode parsing,
        // metadata processing, and pass execution run concurrently per the
        // comment at line 583-587). Two concurrent doSpecializeImpl calls on
        // the same mangled function — exactly the adaptive-monitor / multi-
        // pipeline shared-stage path — would otherwise read the same counter
        // value, build the same jitName, and the second createJITDylib would
        // fail with duplicate-key.
        //
        // Use atomic fetch_add (relaxed: we only need the per-call value to
        // be unique within process lifetime; cross-thread visibility is not
        // required because the value is consumed immediately within the same
        // thread and the resulting jitName is local to this stack frame).
        static std::atomic<int> jitCounter{0};
        int jitId = jitCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        std::string jitName = mangledName + ".jit." + std::to_string(jitId);

        for (auto& func : *parsedModule) {
            if (func.getName() == mangledName) {
                func.setName(jitName);
                break;
            }
        }

        for (auto& func : *parsedModule) {
            if (func.isDeclaration()) continue;
            if (func.getName() == jitName) continue;
            func.deleteBody();
        }

        for (auto& func : *parsedModule) {
            if (func.hasPersonalityFn()) func.setPersonalityFn(nullptr);
            func.removeFnAttr(llvm::Attribute::UWTable);
        }

        for (auto& func : *parsedModule)
            if (func.hasComdat()) func.setComdat(nullptr);
        for (auto& gv : parsedModule->globals())
            if (gv.hasComdat()) gv.setComdat(nullptr);
        parsedModule->getComdatSymbolTable().clear();

        if (auto* used = parsedModule->getNamedGlobal("llvm.used")) used->eraseFromParent();
        if (auto* compUsed = parsedModule->getNamedGlobal("llvm.compiler.used")) compUsed->eraseFromParent();

        if (auto* ctors = parsedModule->getNamedGlobal("llvm.global_ctors")) ctors->eraseFromParent();
        if (auto* dtors = parsedModule->getNamedGlobal("llvm.global_dtors")) dtors->eraseFromParent();

        {
            std::vector<llvm::GlobalAlias*> aliases;
            for (auto& alias : parsedModule->aliases())
                aliases.push_back(&alias);
            for (auto* alias : aliases) {
                alias->replaceAllUsesWith(llvm::UndefValue::get(alias->getType()));
                alias->eraseFromParent();
            }
        }

        {
            std::vector<llvm::GlobalVariable*> rttiGVs;
            for (auto& gv : parsedModule->globals()) {
                auto name = gv.getName();
                if (name.starts_with("??_R") || name.starts_with("_CT??_R") || name.starts_with("??_7type_info"))
                    rttiGVs.push_back(&gv);
            }
            for (auto* gv : rttiGVs) {
                gv->replaceAllUsesWith(llvm::UndefValue::get(gv->getType()));
                gv->eraseFromParent();
            }
        }

        bool gvChanged = true;
        while (gvChanged) {
            gvChanged = false;
            std::vector<llvm::GlobalVariable*> gvsToRemove;
            for (auto& gv : parsedModule->globals()) {
                if (gv.use_empty() && !gv.getName().starts_with("llvm.")) gvsToRemove.push_back(&gv);
            }
            for (auto* gv : gvsToRemove) {
                gv->eraseFromParent();
                gvChanged = true;
            }
        }

        // Lock LLJIT interactions — multiple doSpecialize() calls may run
        // concurrently via std::async.  Keep the lock scope tight: only
        // createJITDylib → addIRModule → lookup need serialization.
        // Bitcode parsing, metadata processing, and pass execution above
        // remain concurrent.
        {
            std::lock_guard<std::mutex> lock(g_jit_mutex);

            auto dylibOrErr = g_jit->createJITDylib(jitName);
            if (!dylibOrErr) {
                llvm::errs() << "JIT createJITDylib failed: " << llvm::toString(dylibOrErr.takeError()) << "\n";
                return nullptr;
            }
            auto& dylib = *dylibOrErr;

            auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                g_jit->getDataLayout().getGlobalPrefix());
            if (gen) dylib.addGenerator(std::move(*gen));

#ifdef _WIN32
            for (const char* dllName : {"vcruntime140.dll", "ucrtbase.dll"}) {
                auto dllGen =
                    llvm::orc::DynamicLibrarySearchGenerator::Load(dllName, g_jit->getDataLayout().getGlobalPrefix());
                if (dllGen)
                    dylib.addGenerator(std::move(*dllGen));
                else
                    llvm::consumeError(dllGen.takeError());
            }
#endif

            auto tsm = llvm::orc::ThreadSafeModule(std::move(parsedModule), std::move(llvmCtx));

            if (auto err = g_jit->addIRModule(dylib, std::move(tsm))) {
                llvm::errs() << "JIT addIRModule failed: " << llvm::toString(std::move(err)) << "\n";
                return nullptr;
            }

            auto symOrErr = g_jit->lookup(dylib, jitName);
            if (!symOrErr) {
                llvm::errs() << "JIT lookup failed for '" << jitName << "': " << llvm::toString(symOrErr.takeError())
                             << "\n";
                return nullptr;
            }

            return reinterpret_cast<void*>(symOrErr->getValue());
        }
    }
}

char* dumpIRBytes(const uint8_t* irData, size_t irSize) {
    if (!irData || irSize == 0) return nullptr;

    auto bufOrErr = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(irData), irSize), "", false);
    // Same nullable-unique_ptr contract as in doSpecializeImpl above; without
    // this guard, an allocation failure here turns a "dump IR for debugging"
    // call into a segfault.
    if (!bufOrErr) return nullptr;

    llvm::LLVMContext llvmCtx;
    auto moduleOrErr = llvm::parseBitcodeFile(bufOrErr->getMemBufferRef(), llvmCtx);
    if (!moduleOrErr) return nullptr;

    std::string result;
    llvm::raw_string_ostream os(result);
    moduleOrErr.get()->print(os, nullptr);

    char* out = new char[result.size() + 1];
    std::memcpy(out, result.c_str(), result.size() + 1);
    return out;
}

} // anonymous namespace

// ============================================================
// C ABI exports
// ============================================================

extern "C" {

TOPO_JIT_ENGINE_API uint32_t topo_jit_engine_version(void) {
    return TOPO_JIT_ENGINE_ABI_VERSION;
}

TOPO_JIT_ENGINE_API int topo_jit_engine_available(void) {
    return ensureJIT() ? 1 : 0;
}

TOPO_JIT_ENGINE_API void* topo_jit_engine_specialize(
    const char* name, const char* ctx_json, size_t ctx_len, const char* costs_json, size_t costs_len) {
    auto ctx = deserializeContext(ctx_json, ctx_len);
    auto costs = deserializeCosts(costs_json, costs_len);
    auto irSection = readSection(".topo_ir");
    auto metaSection = readSection(".tp_meta");
    return doSpecializeImpl(name, ctx, costs, irSection.data, irSection.size, metaSection.data, metaSection.size);
}

TOPO_JIT_ENGINE_API void* topo_jit_engine_specialize_bytes(
    const char* name,
    const void* ir_data, size_t ir_size,
    const char* meta_json, size_t meta_len,
    const char* ctx_json, size_t ctx_len,
    const char* costs_json, size_t costs_len) {
    auto ctx = deserializeContext(ctx_json, ctx_len);
    auto costs = deserializeCosts(costs_json, costs_len);
    return doSpecializeImpl(
        name ? name : "", ctx, costs,
        reinterpret_cast<const uint8_t*>(ir_data), ir_size,
        reinterpret_cast<const uint8_t*>(meta_json), meta_len);
}

TOPO_JIT_ENGINE_API char* topo_jit_engine_dump_ir(const char* /*name*/, const char* /*ctx_json*/, size_t /*ctx_len*/) {
    auto irSection = readSection(".topo_ir");
    return dumpIRBytes(irSection.data, irSection.size);
}

TOPO_JIT_ENGINE_API char* topo_jit_engine_dump_ir_bytes(const void* ir_data, size_t ir_size) {
    return dumpIRBytes(reinterpret_cast<const uint8_t*>(ir_data), ir_size);
}

TOPO_JIT_ENGINE_API void topo_jit_engine_free_string(char* str) {
    delete[] str;
}

} // extern "C"
