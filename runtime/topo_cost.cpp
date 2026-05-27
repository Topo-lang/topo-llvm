// Pass-emitted callbacks: topo_cost_begin / topo_cost_end
//
// AdaptiveDispatchPass wraps each pipeline's AOT path with these at entry
// and every return. Isolating them in a thin forwarding TU lets the
// linker dead-strip this object when the pass was off — user code that
// touches topo::parallel alone will not drag the cost symbols into the
// final binary, which would otherwise leak via scheduler TU colocation.
//
// The actual implementation (TLS state + sample recording) stays in
// topo_parallel.cpp so we avoid extern-linkage thread_local ABI issues
// across TUs. This file just forwards to internal `_impl` entry points.

#include "topo/rt/parallel_rt.h"

extern "C" void topo_parallel_cost_begin_impl(const char* func_name);
extern "C" void topo_parallel_cost_end_impl(const char* func_name);

extern "C" void topo_cost_begin(const char* func_name) {
    topo_parallel_cost_begin_impl(func_name);
}

extern "C" void topo_cost_end(const char* func_name) {
    topo_parallel_cost_end_impl(func_name);
}
