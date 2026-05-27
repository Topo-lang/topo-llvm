#include "topo/Backend/PassReportsSidecar.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace topo::backend {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

json headerToJson(const PassReportHeader& h) {
    json j = json::object();
    j["pass"] = h.passName;
    j["category"] = h.category;
    j["fired"] = h.fired;
    j["fired_count"] = h.firedCount;
    j["decision"] = h.decision;
    j["reason"] = h.reason;
    j["elapsed_ns"] = h.elapsedNs;
    return j;
}

// DataLayoutPass — full detail.
json dataLayoutToJson(const DataLayoutReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& c : r.candidates) {
        json o = json::object();
        o["pipeline"] = c.pipelineName;
        o["wrapper_ir_type"] = c.wrapperIRType;
        o["element_ir_type"] = c.elementIRType;
        o["array_size"] = c.arraySize;
        o["baseline_ns"] = c.baselineNs;
        o["variant_ns"] = c.variantNs;
        o["speedup"] = c.speedup;
        o["winner"] = c.winner;
        o["applied"] = c.applied;
        arr.push_back(std::move(o));
    }
    j["candidates"] = std::move(arr);
    return j;
}

json indirectionToJson(const IndirectionReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json s = json::object();
    s["unique_ptr_promoted"] = r.uniquePtrPromoted;
    s["shared_ptr_optimized"] = r.sharedPtrOptimized;
    s["shared_ptr_dereferenced"] = r.sharedPtrDereferenced;
    s["refcount_eliminated"] = r.refcountEliminated;
    s["vector_lowered"] = r.vectorLowered;
    s["pointer_attrs_added"] = r.pointerAttrsAdded;
    s["calls_devirtualized"] = r.callsDevirtualized;
    s["vtable_constants_annotated"] = r.vtableConstantsAnnotated;
    j["stats"] = std::move(s);
    return j;
}

json topoParallelToJson(const TopoParallelReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& c : r.candidates) {
        json o = json::object();
        o["pipeline"] = c.pipelineName;
        o["baseline_ns"] = c.baselineNs;
        o["variant_ns"] = c.variantNs;
        o["speedup"] = c.speedup;
        o["winner"] = c.winner;
        o["applied"] = c.applied;
        arr.push_back(std::move(o));
    }
    j["candidates"] = std::move(arr);
    return j;
}

json lifetimeArenaToJson(const LifetimeArenaReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& c : r.candidates) {
        json o = json::object();
        o["owner"] = c.ownerName;
        o["baseline_ns"] = c.baselineNs;
        o["variant_ns"] = c.variantNs;
        o["speedup"] = c.speedup;
        o["winner"] = c.winner;
        o["applied"] = c.applied;
        arr.push_back(std::move(o));
    }
    j["candidates"] = std::move(arr);
    return j;
}

json returnSpecializationToJson(const ReturnSpecializationReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& e : r.entries) {
        json o = json::object();
        o["host_function"] = e.hostFunction;
        o["eliminated_field_indices"] = e.eliminatedFieldIndices;
        o["kept_field_indices"] = e.keptFieldIndices;
        arr.push_back(std::move(o));
    }
    j["entries"] = std::move(arr);
    return j;
}

json topoInlineToJson(const TopoInlineReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& e : r.entries) {
        json o = json::object();
        o["callee"] = e.callee;
        o["reason"] = e.reason;
        arr.push_back(std::move(o));
    }
    j["entries"] = std::move(arr);
    return j;
}

json topoFlattenToJson(const TopoFlattenReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    j["demoted_functions"] = r.demotedFunctions;
    return j;
}

json adaptiveDispatchToJson(const AdaptiveDispatchReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& e : r.entries) {
        json o = json::object();
        o["stage"] = e.stageName;
        o["default_variant"] = e.defaultVariant;
        arr.push_back(std::move(o));
    }
    j["entries"] = std::move(arr);
    return j;
}

json prefetchToJson(const PrefetchReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& e : r.entries) {
        json o = json::object();
        o["host_function"] = e.hostFunction;
        o["inserted_hints"] = e.insertedHints;
        o["distance"] = e.distance;
        arr.push_back(std::move(o));
    }
    j["entries"] = std::move(arr);
    return j;
}

json containmentInterceptionToJson(const ContainmentInterceptionReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& e : r.entries) {
        json o = json::object();
        o["caller"] = e.callerFunction;
        o["intercepted_callee"] = e.interceptedCallee;
        arr.push_back(std::move(o));
    }
    j["entries"] = std::move(arr);
    return j;
}

json loopParallelizeToJson(const LoopParallelizeReport& r) {
    json j = json::object();
    j["header"] = headerToJson(r.header);
    json arr = json::array();
    for (const auto& e : r.entries) {
        json o = json::object();
        o["host_function"] = e.hostFunction;
        o["annotated_loops"] = e.annotatedLoops;
        arr.push_back(std::move(o));
    }
    j["entries"] = std::move(arr);
    return j;
}

// Recursively sort object keys so generated files are byte-stable.
void sortKeys(nlohmann::ordered_json& j) {
    if (j.is_object()) {
        std::vector<std::pair<std::string, nlohmann::ordered_json>> pairs;
        pairs.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            pairs.emplace_back(it.key(), std::move(it.value()));
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        j = nlohmann::ordered_json::object();
        for (auto& [k, v] : pairs) {
            sortKeys(v);
            j[k] = std::move(v);
        }
    } else if (j.is_array()) {
        for (auto& item : j) sortKeys(item);
    }
}

// Write a single JSON file atomically. Returns true on success.
bool atomicWriteJson(const fs::path& dest, const json& body) {
    nlohmann::ordered_json ordered = nlohmann::ordered_json::parse(body.dump());
    sortKeys(ordered);

    fs::path tmp = dest;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "topo-passes: cannot open '" << tmp.string() << "' for write\n";
            return false;
        }
        out << ordered.dump(2) << "\n";
        out.flush();
        if (!out) {
            std::cerr << "topo-passes: write failure on '" << tmp.string() << "'\n";
            return false;
        }
    }

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    {
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
#endif

    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::cerr << "topo-passes: rename '" << tmp.string() << "' -> '"
                  << dest.string() << "' failed: " << ec.message() << "\n";
        return false;
    }
    return true;
}

} // namespace

bool writePassReportsSidecar(const PassReports& reports,
                             const std::string& outputPath) {
    fs::path dir = fs::path(outputPath + ".topo-passes");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "topo-passes: cannot create '" << dir.string()
                  << "': " << ec.message() << "\n";
        return false;
    }

    bool allOk = true;
    auto write = [&](const std::string& name, const json& body) {
        if (!atomicWriteJson(dir / (name + ".json"), body)) allOk = false;
    };

    write("DataLayoutPass",              dataLayoutToJson(reports.dataLayout));
    write("IndirectionPass",             indirectionToJson(reports.indirection));
    write("TopoParallelPass",            topoParallelToJson(reports.topoParallel));
    write("LifetimeArenaPass",           lifetimeArenaToJson(reports.lifetimeArena));
    write("ReturnSpecializationPass",    returnSpecializationToJson(reports.returnSpecialization));
    write("TopoInlinePass",              topoInlineToJson(reports.topoInline));
    write("TopoFlattenPass",             topoFlattenToJson(reports.topoFlatten));
    write("AdaptiveDispatchPass",        adaptiveDispatchToJson(reports.adaptiveDispatch));
    write("PrefetchPass",                prefetchToJson(reports.prefetch));
    write("ContainmentInterceptionPass", containmentInterceptionToJson(reports.containmentInterception));
    write("LoopParallelizePass",         loopParallelizeToJson(reports.loopParallelize));

    return allOk;
}

} // namespace topo::backend
