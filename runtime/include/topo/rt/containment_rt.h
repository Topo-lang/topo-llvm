#ifndef TOPO_RT_CONTAINMENT_RT_H
#define TOPO_RT_CONTAINMENT_RT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version — bump when any signature in this header changes (add/remove
/// parameter, change parameter or return type, change calling convention).
/// Adding a NEW entry point without touching existing ones is backward
/// compatible and does NOT require a bump.
///
/// Consumers obtain the runtime's actual version via topo_containment_version()
/// at process start and compare against TOPO_CONTAINMENT_ABI_VERSION; mismatch
/// is a hard error. See topo-llvm/runtime/ABI-COMPAT.md for the compatibility
/// matrix and bump history.
///
/// v1: initial pinned baseline (violation report, get_violation_count, dump,
///     reset).
#define TOPO_CONTAINMENT_ABI_VERSION 1

/// Report the runtime's compiled-in TOPO_CONTAINMENT_ABI_VERSION. Consumers
/// (typically a one-time ctor injected by ContainmentInterceptionPass) call
/// this and abort on mismatch with their own TOPO_CONTAINMENT_ABI_VERSION.
uint32_t topo_containment_version(void);

/// Record a containment violation (called by instrumented code).
/// @param caller  Name of the non-external function making the call
/// @param callee  Name of the external API being called
void __topo_containment_violation(const char* caller, const char* callee);

/// Get the number of recorded violations.
int __topo_containment_get_violation_count(void);

/// Dump all violations to stderr as JSON. Registered via atexit on first violation.
void __topo_containment_dump(void);

/// Reset violation storage (for testing).
void __topo_containment_reset(void);

#ifdef __cplusplus
}
#endif

#endif // TOPO_RT_CONTAINMENT_RT_H
