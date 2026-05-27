#ifndef TOPO_TRANSFORMS_RUNTIMEABIVERSIONS_H
#define TOPO_TRANSFORMS_RUNTIMEABIVERSIONS_H

#include <cstdint>

namespace topo {

/// Pinned, pass-compile-time copies of the `TOPO_<lib>_ABI_VERSION`
/// macros declared in `topo-llvm/runtime/include/topo/rt/<lib>_rt.h`.
///
/// Why a duplicate header instead of pulling the runtime headers in:
/// the LLVM Pass library (`TopoTransforms`) is intentionally not
/// linked or include-path-coupled to `topo-llvm/runtime/` (the runtime
/// libs are the *callees* of generated IR — not a build dependency of
/// the codegen). Mirroring the macros into a dedicated, pass-side
/// header keeps that boundary while still letting passes inject a
/// concrete expected-version literal into the ABI-check ctor they
/// emit (see `RuntimeAbiCheck.h`).
///
/// Bump rule: when the runtime header bumps its
/// `TOPO_<NAME>_ABI_VERSION` (per `topo-llvm/runtime/ABI-COMPAT.md`),
/// bump the matching constant here in the same commit. The
/// `CABIContractTest.<Lib>VersionMacroMatchesSymbol` cases plus the
/// process-level test infrastructure are the safety net if the two
/// drift.
namespace abi {

inline constexpr std::uint32_t kParallelVersion = 1;
inline constexpr std::uint32_t kArenaVersion = 1;
inline constexpr std::uint32_t kAdaptiveVersion = 1;
inline constexpr std::uint32_t kObserveVersion = 1;
inline constexpr std::uint32_t kContainmentVersion = 1;

} // namespace abi

} // namespace topo

#endif // TOPO_TRANSFORMS_RUNTIMEABIVERSIONS_H
