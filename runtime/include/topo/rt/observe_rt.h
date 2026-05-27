#ifndef TOPO_RT_OBSERVE_RT_H
#define TOPO_RT_OBSERVE_RT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version — bump when any signature in this header changes (add/remove
/// parameter, change parameter or return type, change calling convention).
/// Adding a NEW entry point without touching existing ones is backward
/// compatible and does NOT require a bump.
///
/// Consumers obtain the runtime's actual version via topo_trace_version()
/// at process start and compare against TOPO_OBSERVE_ABI_VERSION; mismatch
/// is a hard error. See topo-llvm/runtime/ABI-COMPAT.md for the compatibility
/// matrix and bump history.
///
/// v1: initial pinned baseline (span_begin, span_end, init, shutdown).
#define TOPO_OBSERVE_ABI_VERSION 1

/// Report the runtime's compiled-in TOPO_OBSERVE_ABI_VERSION. Consumers
/// (typically a one-time ctor injected by ObservabilityPass) call this
/// and abort on mismatch with their own TOPO_OBSERVE_ABI_VERSION.
uint32_t topo_trace_version(void);

/// Begin a named tracing span. Pushes onto thread-local span stack.
///
/// Lifecycle contract: if the runtime has not been initialized (or has
/// been shut down), this call is a silent no-op — nothing is pushed and
/// nothing is emitted. Call topo_trace_init() before use.
///
/// The `name` pointer must remain valid until the matching
/// topo_trace_span_end(); the runtime stores it by reference.
void topo_trace_span_begin(const char* name);

/// End the current tracing span. Pops from thread-local span stack
/// and emits duration to the configured exporter.
///
/// Lifecycle contract: if the runtime has not been initialized (or has
/// been shut down), this call is a silent no-op with one exception — if
/// the thread-local span stack has residual entries from an earlier
/// initialized window, one entry is popped (without emission) to keep
/// begin/end pairs balanced across init/shutdown cycles.
void topo_trace_span_end(void);

/// Initialize the tracing runtime.
/// @param exporter  Reserved exporter name; only "stdout" is implemented.
///                  The value is currently ignored at runtime; ConfigValidator
///                  rejects anything else at build configure time. Reserved
///                  for future routing without an ABI break.
/// @param sampling_rate  Probability [0.0, 1.0] of recording each span
///
/// Idempotent: a second init before shutdown is a silent no-op. After
/// initialization, topo_trace_span_begin/end are live; before (or after
/// a subsequent shutdown) they are silent no-ops.
void topo_trace_init(const char* exporter, double sampling_rate);

/// Shut down the tracing runtime and flush any buffered output.
///
/// Idempotent: shutdown without a prior init is a silent no-op. After
/// shutdown, topo_trace_span_begin/end become no-ops until the next
/// topo_trace_init() call.
void topo_trace_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // TOPO_RT_OBSERVE_RT_H
