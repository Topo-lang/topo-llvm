#ifndef TOPO_TRANSFORMS_RUNTIMEABICHECK_H
#define TOPO_TRANSFORMS_RUNTIMEABICHECK_H

#include <cstdint>
#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace topo {

/// Inject a one-time global ctor into `module` that calls the runtime
/// library's `<versionSymbol>()` introspection function at process start,
/// compares its return value against the compile-time `expectedVersion`
/// (the caller's pinned `TOPO_<lib>_ABI_VERSION` macro), and aborts with a
/// diagnostic on stderr when the two disagree.
///
/// Pattern source: `topo_jit_api.cpp:209-215` (the long-standing
/// `libtopo-jit-engine` check). Versioning policy: see
/// `topo-llvm/runtime/ABI-COMPAT.md`.
///
/// `libName` is the lowercase short library name used in the stderr
/// message and in the internal-symbol name (e.g. `"parallel"`,
/// `"arena"`); `versionSymbol` is the runtime entry point
/// (e.g. `"topo_parallel_version"`); `expectedVersion` is the caller's
/// compile-time macro value (e.g. `TOPO_PARALLEL_ABI_VERSION`).
///
/// Idempotent: a second call for the same `libName` on the same module
/// is a no-op. This lets two passes that both inject calls into the
/// same runtime library (e.g. `TopoParallelPass` and
/// `LoopParallelizePass` both touching `libtopo-parallel`) cooperate
/// without registering two ctors.
void injectAbiCheckCtor(llvm::Module& module,
                        const std::string& libName,
                        const std::string& versionSymbol,
                        std::uint32_t expectedVersion);

/// Stable name of the ctor function the helper synthesizes for `libName`.
/// Exposed for tests that want to look the ctor up by name without
/// hardcoding the mangling.
std::string runtimeAbiCheckCtorName(const std::string& libName);

} // namespace topo

#endif // TOPO_TRANSFORMS_RUNTIMEABICHECK_H
