// Pass-emitted callback: topo_adaptive_register
//
// AdaptiveDispatchPass emits one call to this from a per-pipeline
// global_ctor (`<pipeline>.__adaptive_ctor`). Isolating it in its own
// translation unit lets the linker dead-strip this object file when the
// pass was off — so user code calling topo::adaptive::stats() alone will
// not drag the register symbol into the final binary.

#include "topo/rt/adaptive_rt.h"
#include "topo_adaptive_internal.h"

#include <cstdint>
#include <mutex>
#include <utility>

extern "C" void topo_adaptive_register(const char* mangled_name,
                                       const char* pipeline_name,
                                       void** jit_ptr,
                                       uint64_t aot_tti_cost) {
    using namespace topo::adaptive::internal;

    std::lock_guard<std::mutex> lock(g_mutex);

    PipelineEntry entry;
    entry.mangledName = mangled_name;
    entry.pipelineName = pipeline_name;
    entry.jitPtr = jit_ptr;
    entry.aotTTICost = aot_tti_cost;

    g_pipelines.push_back(std::move(entry));
}
