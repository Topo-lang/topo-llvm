#ifndef TOPO_RUNTIME_TOPO_ADAPTIVE_INTERNAL_H
#define TOPO_RUNTIME_TOPO_ADAPTIVE_INTERNAL_H

// Private shared state between topo_adaptive.cpp (monitor loop +
// user-facing API) and topo_adaptive_register.cpp (pass-emitted ctor
// callback). Split into two TUs so the linker dead-strips
// topo_adaptive_register when no __adaptive_ctor references it —
// that is, when AdaptiveDispatchPass was off.

#include "topo/adaptive.h"
#include "topo/jit.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace topo::adaptive::internal {

enum class PipelineState {
    WARMUP,
    MONITORING,
    SPECIALIZING,
    VERIFYING,
    ACTIVE,
};

struct PipelineEntry {
    std::string mangledName;
    std::string pipelineName;
    void** jitPtr;
    uint64_t aotTTICost;

    PipelineState state = PipelineState::WARMUP;
    uint32_t callCount = 0;
    uint32_t jitVersions = 0;
    uint64_t aotBaselineNS = 0;
    uint64_t jitBaselineNS = 0;
    uint32_t verifyCount = 0;

    std::future<void*> jitFuture;
};

extern std::mutex g_mutex;
extern std::vector<PipelineEntry> g_pipelines;
extern bool g_initialized;
extern bool g_shutdown;
extern std::thread g_monitorThread;
extern std::condition_variable g_cv;
extern topo::adaptive::Config g_config;

extern std::atomic<uint32_t> g_specializations;
extern std::atomic<uint32_t> g_deoptimizations;
extern std::atomic<uint32_t> g_activeJit;

} // namespace topo::adaptive::internal

#endif
