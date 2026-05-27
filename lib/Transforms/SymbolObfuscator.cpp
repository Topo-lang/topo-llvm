// @category: INSTRUMENT
#include "topo/Transforms/SymbolObfuscator.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SipHash.h>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace topo {

std::string SymbolObfuscator::computeHash(const std::string& qualifiedName, const std::string& salt) {
    // Derive 16-byte key from salt (truncate or zero-pad)
    uint8_t key[16] = {};
    size_t copyLen = std::min(salt.size(), size_t(16));
    std::memcpy(key, salt.data(), copyLen);

    // SipHash-2-4-128
    uint8_t out[16];
    llvm::getSipHash_2_4_128(
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(qualifiedName.data()), qualifiedName.size()),
        key,
        out);

    // Format as 32 hex chars
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(out[i]);
    return oss.str();
}

ObfuscationResult SymbolObfuscator::obfuscate(llvm::Module& module,
                                              const std::vector<VisibilityEntry>& entries,
                                              const SymbolMapping& mapping,
                                              ObfuscationMode mode,
                                              const std::string& salt) {
    ObfuscationResult result;

    // Build set of valid Function pointers currently in the module.
    // This avoids accessing dangling pointers from functions erased
    // by FlattenPass or standard LLVM optimization.
    std::unordered_set<llvm::Function*> validFuncs;
    for (auto& func : module) {
        validFuncs.insert(&func);
    }

    for (const auto& entry : entries) {
        if (entry.visibility == Visibility::Public) continue;

        auto it = mapping.matched.find(entry.qualifiedName);
        if (it == mapping.matched.end()) continue;

        llvm::Function* func = it->second;

        // Skip if function was erased during optimization
        if (validFuncs.find(func) == validFuncs.end()) continue;

        // Skip declarations (body removed by inlining/DCE) — renaming
        // a declaration would create an unresolvable linker symbol
        if (func->isDeclaration()) continue;

        // Salt is the key, qualifiedName is the message
        std::string effectiveSalt = (mode == ObfuscationMode::Salted) ? salt : std::string();
        std::string hashed = computeHash(entry.qualifiedName, effectiveSalt);

        // Derive prefix from salt: "_Z" + one char from 't' to 'z'
        std::string prefix = "_Zt"; // default for Normal mode
        if (mode == ObfuscationMode::Salted && !salt.empty()) {
            char suffix = 't' + (static_cast<unsigned char>(salt[0]) % 7); // 't'..'z'
            prefix = std::string("_Z") + suffix;
        }

        std::string obfuscatedName = prefix + hashed;
        func->setName(obfuscatedName);
        ++result.renamedCount;

        if (entry.visibility == Visibility::Protected) {
            result.protectedMapping[entry.qualifiedName] = obfuscatedName;
        }
    }

    return result;
}

} // namespace topo
