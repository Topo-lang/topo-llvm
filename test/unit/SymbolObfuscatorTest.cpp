#include "topo/Transforms/SymbolObfuscator.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Basic/BuildTypes.h"
#include "topo/Sema/VisibilityCollector.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class SymbolObfuscatorTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

static VisibilityEntry makeEntry(const std::string& name, Visibility vis) {
    VisibilityEntry e;
    e.qualifiedName = name;
    e.visibility = vis;
    return e;
}

// A function with a body (declarations are skipped by the obfuscator).
static llvm::Function* createDefinedFunc(llvm::Module& m, const std::string& name) {
    auto& ctx = m.getContext();
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {}, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::InternalLinkage, name, m);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
    llvm::IRBuilder<> b(bb);
    b.CreateRet(llvm::ConstantInt::get(i32Ty, 7));
    return f;
}

// A private internal symbol is renamed to an obfuscated hash; a public symbol
// keeps its original name. A no-op pass leaves both names unchanged and the
// private-rename assertion fails.
TEST_F(SymbolObfuscatorTest, PrivateRenamedPublicLeftIntact) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("obf_mix", ctx);
    auto* priv = createDefinedFunc(*module, "secret_helper");
    auto* pub = createDefinedFunc(*module, "public_api");

    SymbolMapping mapping;
    mapping.matched["ns::secret_helper"] = priv;
    mapping.matched["ns::public_api"] = pub;
    std::vector<VisibilityEntry> entries = {
        makeEntry("ns::secret_helper", Visibility::Private),
        makeEntry("ns::public_api", Visibility::Public),
    };

    ObfuscationResult result =
        SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Normal);

    EXPECT_EQ(result.renamedCount, 1) << "only the private symbol should be renamed";
    EXPECT_NE(priv->getName(), "secret_helper") << "private symbol must be renamed";
    EXPECT_TRUE(priv->getName().starts_with("_Zt"))
        << "Normal-mode obfuscated name uses the _Zt prefix, got " << priv->getName().str();
    EXPECT_EQ(pub->getName(), "public_api") << "public symbol must be left intact";
}

// Normal mode is deterministic: the same qualified name hashes to the same
// obfuscated name across two independent modules.
TEST_F(SymbolObfuscatorTest, NormalModeIsDeterministic) {
    auto obfuscateOne = [](llvm::LLVMContext& ctx) -> std::string {
        auto module = std::make_unique<llvm::Module>("obf_det", ctx);
        auto* f = createDefinedFunc(*module, "helper");
        SymbolMapping mapping;
        mapping.matched["ns::helper"] = f;
        std::vector<VisibilityEntry> entries = {makeEntry("ns::helper", Visibility::Private)};
        SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Normal);
        return f->getName().str();
    };

    llvm::LLVMContext ctxA, ctxB;
    EXPECT_EQ(obfuscateOne(ctxA), obfuscateOne(ctxB))
        << "Normal mode must hash a name deterministically";
}

// Salted mode produces a different name for the same symbol under a different
// salt — proving the salt actually feeds the hash.
TEST_F(SymbolObfuscatorTest, SaltedModeVariesWithSalt) {
    auto obfuscateWithSalt = [](llvm::LLVMContext& ctx, const std::string& salt) -> std::string {
        auto module = std::make_unique<llvm::Module>("obf_salt", ctx);
        auto* f = createDefinedFunc(*module, "helper");
        SymbolMapping mapping;
        mapping.matched["ns::helper"] = f;
        std::vector<VisibilityEntry> entries = {makeEntry("ns::helper", Visibility::Private)};
        SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Salted, salt);
        return f->getName().str();
    };

    llvm::LLVMContext ctxA, ctxB;
    EXPECT_NE(obfuscateWithSalt(ctxA, "build-001"), obfuscateWithSalt(ctxB, "build-002"))
        << "different salts must yield different obfuscated names";
}

// Protected symbols are renamed and recorded in the protectedMapping so the
// linker / consumers can rebind to the obfuscated name.
TEST_F(SymbolObfuscatorTest, ProtectedRecordedInMapping) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("obf_prot", ctx);
    auto* prot = createDefinedFunc(*module, "prot_helper");

    SymbolMapping mapping;
    mapping.matched["ns::prot_helper"] = prot;
    std::vector<VisibilityEntry> entries = {makeEntry("ns::prot_helper", Visibility::Protected)};

    ObfuscationResult result =
        SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Normal);
    EXPECT_EQ(result.renamedCount, 1);
    ASSERT_EQ(result.protectedMapping.count("ns::prot_helper"), 1u);
    EXPECT_EQ(result.protectedMapping.at("ns::prot_helper"), prot->getName().str());
    EXPECT_NE(prot->getName(), "prot_helper");
}

// Cross-backend parity: assert SymbolObfuscator::computeHash uses
// SipHash-2-4-128 (the same primitive the JVM ObfuscationPass now
// uses). The expected hex below is also pinned by the JVM-side
// SipHash reference vector test in
// topo-jvm/transform/src/test/java/dev/topo/transform/pass/SipHashTest.java
// (key = 0x00..0x0f, message = 0x00..0x0e); both must agree byte-for-byte.
// If either side drifts, scripts/audit/cross-backend-parity.py
// reports the row.
TEST_F(SymbolObfuscatorTest, SipHashCrossBackendVector) {
    // Key = 16 bytes 0x00..0x0f, message = 15 bytes 0x00..0x0e.
    // Constructed as strings so computeHash's salt/name interface is used
    // verbatim — salt is truncated/zero-padded to 16 bytes, name is the
    // message.
    std::string salt;
    for (int i = 0; i < 16; ++i) salt.push_back(static_cast<char>(i));
    std::string name;
    for (int i = 0; i < 15; ++i) name.push_back(static_cast<char>(i));

    std::string hex = SymbolObfuscator::computeHash(name, salt);

    // Expected output: SipHash-2-4-128 reference algorithm, verified
    // against an independent Python reference implementation. The JVM
    // SipHashTest pins the same expected bytes.
    EXPECT_EQ(hex, "5493e99933b0a8117e08ec0f97cfc3d9")
        << "LLVM SipHash-2-4-128 output drifted; cross-backend parity "
           "with JVM ObfuscationPass is broken — see "
           "scripts/audit/cross-backend-parity.toml row "
           "[obfuscation.hash_algorithm].";
}

// A declaration (no body) must not be renamed — renaming it would create an
// unresolvable linker symbol.
TEST_F(SymbolObfuscatorTest, DeclarationNotRenamed) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("obf_decl", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {}, false);
    auto* decl = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "extern_decl", *module);

    SymbolMapping mapping;
    mapping.matched["ns::extern_decl"] = decl;
    std::vector<VisibilityEntry> entries = {makeEntry("ns::extern_decl", Visibility::Private)};

    ObfuscationResult result =
        SymbolObfuscator::obfuscate(*module, entries, mapping, ObfuscationMode::Normal);
    EXPECT_EQ(result.renamedCount, 0) << "a declaration must not be renamed";
    EXPECT_EQ(decl->getName(), "extern_decl");
}

} // namespace
