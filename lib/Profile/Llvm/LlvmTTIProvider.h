#ifndef TOPO_LLVM_PROFILE_LLVMTTIPROVIDER_H
#define TOPO_LLVM_PROFILE_LLVMTTIPROVIDER_H

// LlvmTTIProvider — the LLVM-bound implementation of the zero-LLVM
// topo::profile::TTIProvider seam declared in topo-core. The static TTI
// cost estimation + build/*.ll IR reading live here so topo-core stays
// free of any llvm/* include or link.

#include "topo/Profile/ProfileEngine.h"

#include <map>
#include <ostream>
#include <string>

namespace topo {
namespace profile {
namespace llvm_backend {

// Computes a qualified/demangled-name → TTI-cost map from
// `<projectDir>/build/*.ll`. Behaviour (warning text on stderr, empty map
// on failure) is byte-identical to the legacy inline buildTTIMap().
class LlvmTTIProvider : public TTIProvider {
public:
    std::map<std::string, uint64_t> buildTTIMap(const std::string& projectDir,
                                                std::ostream& err) override;
};

} // namespace llvm_backend
} // namespace profile
} // namespace topo

#endif // TOPO_LLVM_PROFILE_LLVMTTIPROVIDER_H
