#include "topo/Backend/SymbolMapper.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>

#include <vector>

using namespace topo;

namespace {

// Helper: create a module with named void() functions
llvm::Function* addVoidFunc(llvm::Module& mod, const std::string& name) {
    auto& ctx = mod.getContext();
    auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, mod);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();
    return func;
}

VisibilityEntry entry(const std::string& name) {
    VisibilityEntry e;
    e.qualifiedName = name;
    e.visibility = Visibility::Public;
    return e;
}

// Real rustc v0 symbols for the rust_impl_methods e2e fixture
// (struct Cart + impl Cart { new, total, add } in crate
// "rust_impl_methods", compiled with -Csymbol-mangling-version=v0).
// They demangle to
// "<rust_impl_methods::Cart>::new" / "<rust_impl_methods::Cart>::total" —
// the angle-bracket impl wrapper that used to make every
// "ns::Type::method" member declaration fail IR verification.
constexpr const char* kCartNew = "_RNvMCs1wa4CNumcoK_17rust_impl_methodsNtB2_4Cart3new";
constexpr const char* kCartTotal = "_RNvMCs1wa4CNumcoK_17rust_impl_methodsNtB2_4Cart5total";
constexpr const char* kCartAdd = "_RNvMCs1wa4CNumcoK_17rust_impl_methodsNtB2_4Cart3add";

TEST(SymbolMapperRustTest, ImplMethodsMatchNsTypeMethodDecls) {
    llvm::LLVMContext ctx;
    llvm::Module mod("rust_impl_methods", ctx);
    auto* newFn = addVoidFunc(mod, kCartNew);
    auto* totalFn = addVoidFunc(mod, kCartTotal);
    auto* addFn = addVoidFunc(mod, kCartAdd);

    SymbolMapper mapper(ctx);
    std::vector<VisibilityEntry> entries = {
        entry("rust_impl_methods::Cart::new"),
        entry("rust_impl_methods::Cart::total"),
        entry("rust_impl_methods::Cart::add"),
        entry("Cart::new"),      // bare Type::method candidate form
        entry("app::Cart::new"), // wrong crate — must stay unmatched
    };
    auto mapping = mapper.mapSymbols(mod, entries);

    ASSERT_TRUE(mapping.matched.count("rust_impl_methods::Cart::new"))
        << "crate::Type::method key must resolve to the <crate::Type>::new impl";
    EXPECT_EQ(mapping.matched.at("rust_impl_methods::Cart::new"), newFn);
    ASSERT_TRUE(mapping.matched.count("rust_impl_methods::Cart::total"));
    EXPECT_EQ(mapping.matched.at("rust_impl_methods::Cart::total"), totalFn);
    ASSERT_TRUE(mapping.matched.count("rust_impl_methods::Cart::add"));
    EXPECT_EQ(mapping.matched.at("rust_impl_methods::Cart::add"), addFn);

    // The bare Type::method candidate resolves to the same function.
    ASSERT_TRUE(mapping.matched.count("Cart::new"));
    EXPECT_EQ(mapping.matched.at("Cart::new"), newFn);

    // The wrong-crate spelling stays unmatched.
    EXPECT_FALSE(mapping.matched.count("app::Cart::new"));
    EXPECT_EQ(mapping.unmatchedTopo.size(), 1u);
    EXPECT_EQ(mapping.unmatchedTopo.front(), "app::Cart::new");

    // All impl methods were consumed — nothing is reported as unmatched IR.
    EXPECT_TRUE(mapping.unmatchedIR.empty());
}

TEST(SymbolMapperRustTest, TraitImplMethodMatchesSelfTypeKey) {
    // Real rustc v0 trait-impl symbol: `impl Price for Cart { fn price }`
    // demangles to "<cart::Cart as cart::Price>::price". The declared key
    // uses the SELF type ("cart::Cart::price"), never the trait path.
    const char* kTraitPrice = "_RNvXCs1MbpQotcnBc_4cartNtB2_4CartNtB2_5Price5price";
    llvm::LLVMContext ctx;
    llvm::Module mod("trait_impl", ctx);
    auto* priceFn = addVoidFunc(mod, kTraitPrice);

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(mod, {entry("cart::Cart::price"), entry("cart::Price::price")});

    ASSERT_TRUE(mapping.matched.count("cart::Cart::price"));
    EXPECT_EQ(mapping.matched.at("cart::Cart::price"), priceFn);
    // The trait-path spelling is not generated — the method belongs to the
    // self type, so a "Price::price" declaration must not silently bind.
    EXPECT_FALSE(mapping.matched.count("cart::Price::price"));
}

TEST(SymbolMapperRustTest, PlainItaniumMatchingUnchanged) {
    // Non-rust regression guard: the candidate-key derivation only fires on
    // "<...>" demangles, so C++ Itanium matching is unchanged.
    llvm::LLVMContext ctx;
    llvm::Module mod("cpp", ctx);
    auto* fooFn = addVoidFunc(mod, "_ZN2ns3fooEv");

    SymbolMapper mapper(ctx);
    auto mapping = mapper.mapSymbols(mod, {entry("ns::foo")});

    ASSERT_TRUE(mapping.matched.count("ns::foo"));
    EXPECT_EQ(mapping.matched.at("ns::foo"), fooFn);
    EXPECT_TRUE(mapping.unmatchedIR.empty());
}

} // namespace
