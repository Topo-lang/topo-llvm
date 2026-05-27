#include "topo/jit.h"
#include "topo/parallel.h"
#include "topo/rt/jit_engine_rt.h"
#include "topo/Platform/SharedLibrary.h"
#include "topo/Platform/Platform.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace topo::jit {

// ============================================================
// Context implementation (pure C++, no LLVM)
// ============================================================

void Context::narrow_returns(const std::string& func, const std::vector<std::string>& fields) {
    narrowedReturns_.push_back({func, fields});
}

void Context::prune_edge(const std::string& src, const std::string& dst) {
    prunedEdges_.push_back({src, dst});
}

void Context::set(const std::string& key, const std::string& value) {
    params_.emplace_back(key, value);
}

// ============================================================
// Minimal JSON serializer (avoids nlohmann_json dependency)
// ============================================================

namespace {

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    out += '"';
    return out;
}

std::string serializeContext(const Context& ctx) {
    std::ostringstream ss;
    ss << "{";

    // prunedEdges
    ss << "\"prunedEdges\":[";
    bool first = true;
    for (const auto& e : ctx.prunedEdges()) {
        if (!first) ss << ",";
        ss << "{\"source\":" << escapeJson(e.source) << ",\"target\":" << escapeJson(e.target) << "}";
        first = false;
    }
    ss << "],";

    // narrowedReturns
    ss << "\"narrowedReturns\":[";
    first = true;
    for (const auto& nr : ctx.narrowedReturns()) {
        if (!first) ss << ",";
        ss << "{\"func\":" << escapeJson(nr.func) << ",\"fields\":[";
        bool ffirst = true;
        for (const auto& f : nr.fields) {
            if (!ffirst) ss << ",";
            ss << escapeJson(f);
            ffirst = false;
        }
        ss << "]}";
        first = false;
    }
    ss << "],";

    // params
    ss << "\"params\":[";
    first = true;
    for (const auto& p : ctx.params()) {
        if (!first) ss << ",";
        ss << "[" << escapeJson(p.first) << "," << escapeJson(p.second) << "]";
        first = false;
    }
    ss << "]";

    ss << "}";
    return ss.str();
}

std::string serializeCostSamples(const std::unordered_map<std::string, uint64_t>& samples) {
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [name, cost] : samples) {
        if (!first) ss << ",";
        ss << escapeJson(name) << ":" << cost;
        first = false;
    }
    ss << "}";
    return ss.str();
}

// ============================================================
// Engine loader
// ============================================================

struct EngineInterface {
    using VersionFn = uint32_t (*)();
    using AvailableFn = int (*)();
    using SpecializeFn = void* (*)(const char*, const char*, size_t, const char*, size_t);
    using SpecializeBytesFn = void* (*)(
        const char*,
        const void*, size_t,
        const char*, size_t,
        const char*, size_t,
        const char*, size_t);
    using DumpIRFn = char* (*)(const char*, const char*, size_t);
    using DumpIRBytesFn = char* (*)(const void*, size_t);
    using FreeStringFn = void (*)(char*);

    VersionFn version = nullptr;
    AvailableFn available = nullptr;
    SpecializeFn specialize = nullptr;
    SpecializeBytesFn specializeBytes = nullptr;
    DumpIRFn dumpIR = nullptr;
    DumpIRBytesFn dumpIRBytes = nullptr;
    FreeStringFn freeString = nullptr;

    bool loaded = false;
    bool triedLoad = false;
};

static std::mutex g_engineMutex;
static EngineInterface g_engine;
// Heap-allocated, intentionally leaked. LLVM initializes non-reversible
// global state inside the engine DLL; calling FreeLibrary/dlclose during
// process teardown causes a segfault. The OS reclaims memory at exit.
static platform::SharedLibrary* g_engineLib = nullptr;

std::string engineLibName() {
    // Library name with platform suffix
    if constexpr (platform::IsWindows) {
        return "topo-jit-engine.dll";
    } else if constexpr (platform::IsMacOS) {
        return "libtopo-jit-engine.dylib";
    } else {
        return "libtopo-jit-engine.so";
    }
}

bool loadEngine() {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_engine.triedLoad) return g_engine.loaded;

    g_engine.triedLoad = true;

    // Search order: alongside executable, then system PATH
    g_engineLib = new platform::SharedLibrary();
    std::string exeDir = platform::getExecutableDir();
    bool found = false;
    std::string libName = engineLibName();

    if (!exeDir.empty()) {
        std::string fullPath = exeDir + "/" + libName;
        found = g_engineLib->load(fullPath);
    }

    if (!found) {
        found = g_engineLib->load(libName);
    }

    if (!found) {
        delete g_engineLib;
        g_engineLib = nullptr;
        return false;
    }

    // Resolve symbols
    g_engine.version = reinterpret_cast<EngineInterface::VersionFn>(g_engineLib->getSymbol("topo_jit_engine_version"));
    g_engine.available =
        reinterpret_cast<EngineInterface::AvailableFn>(g_engineLib->getSymbol("topo_jit_engine_available"));
    g_engine.specialize =
        reinterpret_cast<EngineInterface::SpecializeFn>(g_engineLib->getSymbol("topo_jit_engine_specialize"));
    g_engine.specializeBytes = reinterpret_cast<EngineInterface::SpecializeBytesFn>(
        g_engineLib->getSymbol("topo_jit_engine_specialize_bytes"));
    g_engine.dumpIR = reinterpret_cast<EngineInterface::DumpIRFn>(g_engineLib->getSymbol("topo_jit_engine_dump_ir"));
    g_engine.dumpIRBytes = reinterpret_cast<EngineInterface::DumpIRBytesFn>(
        g_engineLib->getSymbol("topo_jit_engine_dump_ir_bytes"));
    g_engine.freeString =
        reinterpret_cast<EngineInterface::FreeStringFn>(g_engineLib->getSymbol("topo_jit_engine_free_string"));

    // Verify ABI version
    if (!g_engine.version || !g_engine.available || !g_engine.specialize) {
        g_engineLib->unload();
        delete g_engineLib;
        g_engineLib = nullptr;
        return false;
    }

    uint32_t engineVersion = g_engine.version();
    if (engineVersion != TOPO_JIT_ENGINE_ABI_VERSION) {
        g_engineLib->unload();
        delete g_engineLib;
        g_engineLib = nullptr;
        return false;
    }

    g_engine.loaded = true;
    return true;
}

} // anonymous namespace

// ============================================================
// Public API
// ============================================================

bool available() {
    if (!loadEngine()) return false;
    return g_engine.available && g_engine.available() != 0;
}

std::future<void*> specialize(const std::string& pipelineName, const Context& ctx) {
    return std::async(std::launch::async, [pipelineName, ctx]() -> void* {
        if (!loadEngine() || !g_engine.specialize) return nullptr;

        auto ctxJson = serializeContext(ctx);
        auto costs = topo::parallel::get_cost_samples();
        auto costsJson = serializeCostSamples(costs);

        return g_engine.specialize(
            pipelineName.c_str(), ctxJson.c_str(), ctxJson.size(), costsJson.c_str(), costsJson.size());
    });
}

std::string dump_ir(const std::string& pipelineName, const Context& ctx) {
    if (!loadEngine() || !g_engine.dumpIR) return "<JIT engine not available>";

    auto ctxJson = serializeContext(ctx);
    char* result = g_engine.dumpIR(pipelineName.c_str(), ctxJson.c_str(), ctxJson.size());

    if (!result) return "<dump_ir failed>";

    std::string ir(result);
    if (g_engine.freeString) g_engine.freeString(result);
    return ir;
}

std::future<void*> specialize_bytes(const std::string& pipelineName,
                                    const void* irBytes, std::size_t irSize,
                                    const std::string& metaJson,
                                    const Context& ctx) {
    // Audit finding topo-llvm-specialize-bytes-pointer-lifetime: the
    // original lambda captured `irBytes` by value as a raw `const void*`,
    // which silently transferred the buffer-lifetime burden onto the
    // caller — and the public header documented no such contract. Two
    // existing callers (force_specialize_bytes in topo_adaptive.cpp) only
    // happen to be safe because they `.get()` the future immediately, but
    // any future caller that stores the future to poll later would have
    // a latent use-after-free.
    //
    // Copy the bytes into a std::vector captured by value. The lambda now
    // owns the buffer for its entire lifetime, matching the spirit of
    // "async specialize" (caller passes input + walks away). The cost is
    // one allocation per specialize on the IR buffer — negligible next
    // to the LLVM bitcode parse / JIT compile that follows.
    std::vector<std::uint8_t> irCopy;
    if (irBytes && irSize > 0) {
        irCopy.assign(static_cast<const std::uint8_t*>(irBytes),
                      static_cast<const std::uint8_t*>(irBytes) + irSize);
    }

    return std::async(std::launch::async,
                      [pipelineName, irBuf = std::move(irCopy), metaJson, ctx]() -> void* {
        if (!loadEngine() || !g_engine.specializeBytes) return nullptr;

        auto ctxJson = serializeContext(ctx);
        auto costs = topo::parallel::get_cost_samples();
        auto costsJson = serializeCostSamples(costs);

        return g_engine.specializeBytes(
            pipelineName.c_str(),
            irBuf.data(), irBuf.size(),
            metaJson.c_str(), metaJson.size(),
            ctxJson.c_str(), ctxJson.size(),
            costsJson.c_str(), costsJson.size());
    });
}

std::string dump_ir_bytes(const void* irBytes, std::size_t irSize) {
    if (!loadEngine() || !g_engine.dumpIRBytes) return "<JIT engine not available>";

    char* result = g_engine.dumpIRBytes(irBytes, irSize);
    if (!result) return "<dump_ir_bytes failed>";

    std::string ir(result);
    if (g_engine.freeString) g_engine.freeString(result);
    return ir;
}

} // namespace topo::jit
