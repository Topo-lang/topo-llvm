#ifndef TOPO_BACKEND_PASSREPORTS_H
#define TOPO_BACKEND_PASSREPORTS_H

// Per-Pass analytical reports for the LLVM backend.
//
// Each judging Pass (one that makes a decision based on analysis or
// benchmarking) produces a report describing what it found, what it decided,
// and why. Mechanical Passes (SymbolObfuscator, PipelineCodeGenPass,
// ObservabilityPass, TopoReorderPass, TopoLayoutPass) are excluded — they
// apply a rule, they don't analyse.
//
// The backend tool serialises this aggregate to one JSON file per Pass under
// `<outputPath>.topo-passes/<PassName>.json` so LSP, `topo debug`, and any
// other consumer can read each Pass's decisions as static build artefacts.
//
// Pass reports are owned by the backend: the entire data shape lives in the
// LLVM backend; topo-core never sees it.

#include <cstdint>
#include <string>
#include <vector>

namespace topo::backend {

// Common envelope present on every report. Fields the same regardless of
// which Pass produced it; downstream tooling can filter / sort / group on
// these before touching Pass-specific details.
struct PassReportHeader {
    std::string passName;     // e.g. "DataLayoutPass"
    std::string category;     // from PassCategoryRegistry: "OPT" / "ENHANCE" / ...
    bool fired = false;       // true if the Pass did any real work
    int firedCount = 0;       // matches the markPassFired() count
    std::string decision;     // pass-specific verdict (see each Pass for vocab)
    std::string reason;       // human-readable explanation behind decision
    int64_t elapsedNs = 0;    // wall-clock time spent inside the Pass + benchmark
};

// ============================================================================
// DataLayoutPass — AoS vs SoA judgment per pipeline function.
// ============================================================================

// One row per topo::array<T, N> candidate considered by the auto-mode
// benchmark. Force / global modes populate this with the array that was
// transformed (winner = "soa", baselineNs / variantNs = 0).
struct DataLayoutCandidate {
    std::string pipelineName;     // logic block name that owns this array
    // IR mangled name of the fixed-size array wrapper. Accepted forms:
    //   "struct.topo::array<Particle, 128>"        (legacy / explicit topo::)
    //   "struct.std::array<Particle, 128>"         (libstdc++ — topo::array
    //                                               is an alias for std::array)
    //   "struct.std::__1::array<Particle, 128>"    (libc++ inline namespace)
    std::string wrapperIRType;
    std::string elementIRType;    // "struct.Particle"
    uint64_t arraySize = 0;       // N
    int64_t baselineNs = 0;       // auto-mode only: AoS variant runtime
    int64_t variantNs = 0;        // auto-mode only: SoA variant runtime
    double speedup = 1.0;         // baselineNs / variantNs (auto only)
    std::string winner;           // "soa" / "aos" / "benchmark_failed" / "forced_soa"
    bool applied = false;         // SoA replaced AoS in the module
};

struct DataLayoutReport {
    PassReportHeader header;      // decision ∈ {"auto_soa", "auto_aos_mixed", "forced_soa", "disabled", "no_candidates"}
    std::vector<DataLayoutCandidate> candidates;
};

// ============================================================================
// IndirectionPass — eight orthogonal indirection optimisations, each scored.
// ============================================================================
struct IndirectionReport {
    PassReportHeader header;
    int uniquePtrPromoted = 0;
    int sharedPtrOptimized = 0;
    int sharedPtrDereferenced = 0;
    int refcountEliminated = 0;
    int vectorLowered = 0;
    int pointerAttrsAdded = 0;
    int callsDevirtualized = 0;
    int vtableConstantsAnnotated = 0;
};

// ============================================================================
// TopoParallelPass — auto-mode benchmark per pipeline.
// ============================================================================
struct TopoParallelCandidate {
    std::string pipelineName;
    int64_t baselineNs = 0;
    int64_t variantNs = 0;
    double speedup = 1.0;
    std::string winner;        // "parallel" / "serial" / "benchmark_failed" / "forced"
    bool applied = false;
};
struct TopoParallelReport {
    PassReportHeader header;
    std::vector<TopoParallelCandidate> candidates;
};

// ============================================================================
// LifetimeArenaPass — auto-mode benchmark per owner function.
// ============================================================================
struct LifetimeArenaCandidate {
    std::string ownerName;
    int64_t baselineNs = 0;
    int64_t variantNs = 0;
    double speedup = 1.0;
    std::string winner;        // "arena" / "heap" / "benchmark_failed" / "forced"
    bool applied = false;
};
struct LifetimeArenaReport {
    PassReportHeader header;
    std::vector<LifetimeArenaCandidate> candidates;
};

// ============================================================================
// ReturnSpecializationPass — eliminated unused return-struct fields.
// ============================================================================
struct ReturnSpecializationEntry {
    std::string hostFunction;
    std::vector<int> eliminatedFieldIndices;
    std::vector<int> keptFieldIndices;
};
struct ReturnSpecializationReport {
    PassReportHeader header;
    std::vector<ReturnSpecializationEntry> entries;
};

// ============================================================================
// TopoInlinePass — callees forced inline (via alwaysinline attribute).
// ============================================================================
struct TopoInlineEntry {
    std::string callee;
    std::string reason;        // "private", "protected", "pipeline_functor", ...
};
struct TopoInlineReport {
    PassReportHeader header;
    std::vector<TopoInlineEntry> entries;
};

// ============================================================================
// TopoFlattenPass — functions demoted to internal linkage so GlobalDCE can
// strip them in the standard pipeline.
// ============================================================================
struct TopoFlattenReport {
    PassReportHeader header;
    std::vector<std::string> demotedFunctions;
};

// ============================================================================
// AdaptiveDispatchPass — stage dispatch instrumentation sites.
// ============================================================================
struct AdaptiveDispatchEntry {
    std::string stageName;
    std::string defaultVariant;
};
struct AdaptiveDispatchReport {
    PassReportHeader header;
    std::vector<AdaptiveDispatchEntry> entries;
};

// ============================================================================
// PrefetchPass — software prefetch insertions per streaming loop.
// ============================================================================
struct PrefetchEntry {
    std::string hostFunction;
    int insertedHints = 0;
    int distance = 0;          // Pass-provided lookahead distance
};
struct PrefetchReport {
    PassReportHeader header;
    std::vector<PrefetchEntry> entries;
};

// ============================================================================
// ContainmentInterceptionPass — call sites wrapped in __topo_containment_violation.
// ============================================================================
struct ContainmentInterceptionEntry {
    std::string callerFunction;
    std::string interceptedCallee;
};
struct ContainmentInterceptionReport {
    PassReportHeader header;
    std::vector<ContainmentInterceptionEntry> entries;
};

// ============================================================================
// LoopParallelizePass — loops annotated with vectorize / parallel metadata.
// ============================================================================
struct LoopParallelizeEntry {
    std::string hostFunction;
    int annotatedLoops = 0;
};
struct LoopParallelizeReport {
    PassReportHeader header;
    std::vector<LoopParallelizeEntry> entries;
};

// ============================================================================
// Aggregate — one slot per judging Pass. Filled by PassPipeline as each Pass
// runs; ferried back to the backend tool via LLVMTransformBackend::passReports()
// after optimize() returns.
// ============================================================================

struct PassReports {
    DataLayoutReport dataLayout;
    IndirectionReport indirection;
    TopoParallelReport topoParallel;
    LifetimeArenaReport lifetimeArena;
    ReturnSpecializationReport returnSpecialization;
    TopoInlineReport topoInline;
    TopoFlattenReport topoFlatten;
    AdaptiveDispatchReport adaptiveDispatch;
    PrefetchReport prefetch;
    ContainmentInterceptionReport containmentInterception;
    LoopParallelizeReport loopParallelize;
};

} // namespace topo::backend

#endif // TOPO_BACKEND_PASSREPORTS_H
