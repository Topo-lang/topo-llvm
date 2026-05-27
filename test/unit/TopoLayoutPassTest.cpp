#include "topo/Transforms/TopoLayoutPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Triple.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class TopoLayoutPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

static llvm::Function* createLeaf(llvm::Module& m, const std::string& name) {
    auto& ctx = m.getContext();
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* fty = llvm::FunctionType::get(voidTy, {}, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::InternalLinkage, name, m);
    auto* bb = llvm::BasicBlock::Create(ctx, "entry", f);
    llvm::IRBuilder<> b(bb);
    b.CreateRetVoid();
    return f;
}

// Build a module whose logic block `run` calls `step_a` at stage 0 and
// `step_b` at stage 2. `targetTriple` controls the section-naming scheme.
struct LayoutFixture {
    std::unique_ptr<llvm::Module> module;
    llvm::Function* stepA = nullptr;
    llvm::Function* stepB = nullptr;
    SymbolTable symbols;
    SymbolMapping mapping;

    void build(llvm::LLVMContext& ctx, const std::string& targetTriple) {
        module = std::make_unique<llvm::Module>("layout_demo", ctx);
        module->setTargetTriple(llvm::Triple(targetTriple));
        stepA = createLeaf(*module, "step_a");
        stepB = createLeaf(*module, "step_b");

        mapping.matched["ns::step_a"] = stepA;
        mapping.matched["ns::step_b"] = stepB;

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::run";
        lb.simpleName = "run";
        lb.calledFunctions = {"step_a", "step_b"};
        lb.stages = {0, 2};
        symbols.addLogicBlock(lb);
    }
};

// On ELF, stage-grouped functions get `.text.topo.stage<N>` sections.
// A no-op pass leaves the section empty → assertions fail.
TEST_F(TopoLayoutPassTest, ElfSectionAssignedByStage) {
    llvm::LLVMContext ctx;
    LayoutFixture fx;
    fx.build(ctx, "x86_64-unknown-linux-gnu");

    int assigned = TopoLayoutPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(assigned, 2) << "both staged functions should get a section";
    EXPECT_EQ(fx.stepA->getSection(), ".text.topo.stage0");
    EXPECT_EQ(fx.stepB->getSection(), ".text.topo.stage2");
}

// On Mach-O, the section name uses the `__TEXT,__topo_stg<N>` form.
TEST_F(TopoLayoutPassTest, MachOSectionAssignedByStage) {
    llvm::LLVMContext ctx;
    LayoutFixture fx;
    fx.build(ctx, "arm64-apple-darwin");

    int assigned = TopoLayoutPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(assigned, 2);
    EXPECT_EQ(fx.stepA->getSection(), "__TEXT,__topo_stg0,regular,pure_instructions");
    EXPECT_EQ(fx.stepB->getSection(), "__TEXT,__topo_stg2,regular,pure_instructions");
}

// On PE/COFF, the section name uses the `.text.topo.stage<N>$` form.
TEST_F(TopoLayoutPassTest, PESectionAssignedByStage) {
    llvm::LLVMContext ctx;
    LayoutFixture fx;
    fx.build(ctx, "x86_64-pc-windows-msvc");

    int assigned = TopoLayoutPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(assigned, 2);
    EXPECT_EQ(fx.stepA->getSection(), ".text.topo.stage0$");
    EXPECT_EQ(fx.stepB->getSection(), ".text.topo.stage2$");
}

// A function not referenced by any logic block gets no section.
TEST_F(TopoLayoutPassTest, UnstagedFunctionGetsNoSection) {
    llvm::LLVMContext ctx;
    LayoutFixture fx;
    fx.build(ctx, "x86_64-unknown-linux-gnu");

    auto* loose = createLeaf(*fx.module, "loose_fn");
    fx.mapping.matched["ns::loose_fn"] = loose;

    int assigned = TopoLayoutPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(assigned, 2) << "only the two staged functions get a section";
    EXPECT_TRUE(loose->getSection().empty()) << "unstaged function must keep no section";
}

} // namespace
