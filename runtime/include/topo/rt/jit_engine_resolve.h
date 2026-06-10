#pragma once

#include <string>
#include <vector>

// Resolution of the JIT engine shared library path.
//
// SECURITY INVARIANT: this never yields a bare leaf name (e.g.
// "libtopo-jit-engine.so"). A bare name passed to dlopen/LoadLibrary lets the
// OS loader search loader-controlled locations — DYLD_LIBRARY_PATH /
// DYLD_FALLBACK_LIBRARY_PATH (whose default includes the user-writable
// /usr/local/lib on Intel Homebrew), LD_LIBRARY_PATH, a relative/$ORIGIN
// RUNPATH, and empty LD_LIBRARY_PATH entries (= CWD). An attacker who plants a
// libtopo-jit-engine.* in such a location would have its constructors and
// exports executed inside any process that links topo-jit-api (user binaries
// built with the adaptive/JIT feature, topo-prof). Every candidate produced
// here is therefore an absolute or explicitly-anchored path; the caller loads
// only these and never falls back to a bare name.

namespace topo::jit::detail {

// Candidate engine paths in priority order, all anchored (never a bare name):
//   1. $TOPO_JIT_ENGINE         — explicit operator override (trusted: env sits
//                                 above CWD in the trust hierarchy)
//   2. <exeDir>/<libName>       — dev/build tree: engine sits next to the tool
//   3. <exeDir>/../lib/<libName> — install layout: tools in bin/, engine in lib/
//
// Returns an empty vector when exeDir is empty and no override is set — the
// caller then reports the engine as unavailable rather than probing a bare name.
inline std::vector<std::string> engineSearchCandidates(const std::string& exeDir,
                                                       const std::string& libName,
                                                       const char* envOverride) {
    std::vector<std::string> candidates;
    if (envOverride != nullptr && envOverride[0] != '\0') {
        candidates.emplace_back(envOverride);
    }
    if (!exeDir.empty()) {
        candidates.emplace_back(exeDir + "/" + libName);
        candidates.emplace_back(exeDir + "/../lib/" + libName);
    }
    return candidates;
}

} // namespace topo::jit::detail
