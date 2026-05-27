#ifndef TOPO_IR_PASSPIPELINE_H
#define TOPO_IR_PASSPIPELINE_H

#include "topo/Basic/BuildTypes.h"
#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Transforms/ContainmentInterceptionPass.h"
#include "topo/Transforms/DataLayoutPass.h"
#include "topo/Transforms/IndirectionPass.h"
#include "topo/Transforms/ObservabilityPass.h"
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Transforms/PrefetchPass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace topo {

struct InlineConfig;

/// Aggregate of every per-pass config pointer + build-wide settings
/// consumed by `PassPipeline::run`. Introduced to retire the
/// 19-parameter signature; adding a new pass now adds one field here
/// instead of churning the parameter list at every call site.
///
/// All pointers are non-owning views. `nullptr` means "this pass's
/// config is absent" — passes that gate on `isEnabled()` treat absence
/// as off. `runtimeCosts` is a reception slot for future auto-mode
/// pass decisions; currently every caller passes `nullptr` and the
/// pipeline does not consume it.
struct PassPipelineConfig {
    const std::vector<VisibilityEntry>* entries = nullptr;
    const SymbolMapping* mapping = nullptr;
    const SymbolTable* symbols = nullptr;
    BuildMode mode = BuildMode::Dev;

    const ParallelConfig* parallelCfg = nullptr;
    const std::unordered_map<std::string, uint64_t>* runtimeCosts = nullptr;
    const AdaptiveConfig* adaptiveCfg = nullptr;
    const DataLayoutConfig* dataLayoutCfg = nullptr;
    const IndirectionConfig* indirectionCfg = nullptr;
    bool indirectionExplicit = true;
    const ObservabilityConfig* observabilityCfg = nullptr;
    const LifetimeConfig* lifetimeCfg = nullptr;
    const LoopParallelConfig* loopParallelCfg = nullptr;
    const PrefetchConfig* prefetchCfg = nullptr;
    const InlineConfig* inlineCfg = nullptr;
    const PipelineConfig* pipelineCfg = nullptr;
    const ContainmentConfig* containmentCfg = nullptr;

    /// Optional out-parameter: per-Pass analysis reports. When
    /// non-null, each judging Pass fills its slot with header
    /// (decision/reason/elapsed) + detail fields. The backend tool
    /// serialises this to `<output>.topo-passes/` via
    /// PassReportsSidecar.
    backend::PassReports* reports = nullptr;
};

class PassPipeline {
public:
    // Legacy: Run standard LLVM optimization pipeline only.
    static bool run(llvm::Module& module, OptLevel level);

    /// Full pipeline: custom Topo passes + standard LLVM optimization.
    /// The struct-overload form replaces the historical 19-parameter
    /// signature; new passes add a field to `PassPipelineConfig`
    /// rather than churning every call site. Existing callers may
    /// migrate by initializing the struct in place.
    static bool run(llvm::Module& module,
                    OptLevel level,
                    const PassPipelineConfig& cfg);

    /// Compatibility shim — forwards to the struct-form overload.
    /// Kept so out-of-tree callers (downstream backends, experimental
    /// tools) continue to compile. New code should use the struct
    /// form directly.
    [[deprecated("use PassPipeline::run(module, level, PassPipelineConfig{...}); "
                 "the 19-param form will be removed once all in-tree callers migrate")]]
    static bool run(llvm::Module& module,
                    OptLevel level,
                    const std::vector<VisibilityEntry>* entries,
                    const SymbolMapping* mapping,
                    const SymbolTable* symbols,
                    BuildMode mode = BuildMode::Dev,
                    const ParallelConfig* parallelCfg = nullptr,
                    const std::unordered_map<std::string, uint64_t>* runtimeCosts = nullptr,
                    const AdaptiveConfig* adaptiveCfg = nullptr,
                    const DataLayoutConfig* dataLayoutCfg = nullptr,
                    const IndirectionConfig* indirectionCfg = nullptr,
                    bool indirectionExplicit = true,
                    const ObservabilityConfig* observabilityCfg = nullptr,
                    const LifetimeConfig* lifetimeCfg = nullptr,
                    const LoopParallelConfig* loopParallelCfg = nullptr,
                    const PrefetchConfig* prefetchCfg = nullptr,
                    const InlineConfig* inlineCfg = nullptr,
                    const PipelineConfig* pipelineCfg = nullptr,
                    const ContainmentConfig* containmentCfg = nullptr,
                    backend::PassReports* reports = nullptr);
};

} // namespace topo

#endif // TOPO_IR_PASSPIPELINE_H
