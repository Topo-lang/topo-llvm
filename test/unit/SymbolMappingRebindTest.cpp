#include "topo/Backend/SymbolMapper.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

// Helper: create a simple module with named void() functions
llvm::Function* addVoidFunc(llvm::Module& mod, const std::string& name) {
    auto& ctx = mod.getContext();
    auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, name, mod);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);
    builder.CreateRetVoid();
    return func;
}

TEST(SymbolMappingRebindTest, RebindRemapsPointers) {
    llvm::LLVMContext ctx1;
    llvm::Module mod1("original", ctx1);
    auto* fooOrig = addVoidFunc(mod1, "_Z3foov");
    auto* barOrig = addVoidFunc(mod1, "_Z3barv");

    SymbolMapping mapping;
    mapping.matched["foo"] = fooOrig;
    mapping.matched["bar"] = barOrig;

    // Create a separate module with same function names (simulates clone)
    llvm::LLVMContext ctx2;
    llvm::Module mod2("clone", ctx2);
    addVoidFunc(mod2, "_Z3foov");
    addVoidFunc(mod2, "_Z3barv");

    auto result = mapping.rebind(mod2);

    EXPECT_EQ(result.matched.size(), 2u);
    EXPECT_NE(result.matched.at("foo"), fooOrig);
    EXPECT_EQ(result.matched.at("foo")->getName(), "_Z3foov");
    EXPECT_NE(result.matched.at("bar"), barOrig);
    EXPECT_EQ(result.matched.at("bar")->getName(), "_Z3barv");
    // Verify pointers are in the new module
    EXPECT_EQ(result.matched.at("foo")->getParent(), &mod2);
    EXPECT_EQ(result.matched.at("bar")->getParent(), &mod2);
}

TEST(SymbolMappingRebindTest, RebindDropsMissing) {
    llvm::LLVMContext ctx1;
    llvm::Module mod1("original", ctx1);
    auto* fooOrig = addVoidFunc(mod1, "_Z3foov");
    auto* barOrig = addVoidFunc(mod1, "_Z3barv");

    SymbolMapping mapping;
    mapping.matched["foo"] = fooOrig;
    mapping.matched["bar"] = barOrig;

    // Target module only has foo, not bar
    llvm::LLVMContext ctx2;
    llvm::Module mod2("clone", ctx2);
    addVoidFunc(mod2, "_Z3foov");

    auto result = mapping.rebind(mod2);

    EXPECT_EQ(result.matched.size(), 1u);
    EXPECT_TRUE(result.matched.count("foo"));
    EXPECT_FALSE(result.matched.count("bar"));
}

TEST(SymbolMappingRebindTest, RebindPreservesStrings) {
    llvm::LLVMContext ctx1;
    llvm::Module mod1("original", ctx1);

    SymbolMapping mapping;
    mapping.matched["foo"] = addVoidFunc(mod1, "_Z3foov");
    mapping.unmatchedTopo = {"missing1"};
    mapping.unmatchedIR = {"extra1"};

    llvm::LLVMContext ctx2;
    llvm::Module mod2("clone", ctx2);
    addVoidFunc(mod2, "_Z3foov");

    auto result = mapping.rebind(mod2);

    ASSERT_EQ(result.unmatchedTopo.size(), 1u);
    EXPECT_EQ(result.unmatchedTopo[0], "missing1");
    ASSERT_EQ(result.unmatchedIR.size(), 1u);
    EXPECT_EQ(result.unmatchedIR[0], "extra1");
}

TEST(SymbolMappingRebindTest, RebindSkipsNull) {
    llvm::LLVMContext ctx1;
    llvm::Module mod1("original", ctx1);

    SymbolMapping mapping;
    mapping.matched["foo"] = addVoidFunc(mod1, "_Z3foov");
    mapping.matched["null_entry"] = nullptr;

    llvm::LLVMContext ctx2;
    llvm::Module mod2("clone", ctx2);
    addVoidFunc(mod2, "_Z3foov");

    auto result = mapping.rebind(mod2);

    EXPECT_EQ(result.matched.size(), 1u);
    EXPECT_TRUE(result.matched.count("foo"));
    EXPECT_FALSE(result.matched.count("null_entry"));
}

} // namespace
