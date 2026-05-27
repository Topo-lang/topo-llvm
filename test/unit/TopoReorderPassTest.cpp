#include "topo/Transforms/TopoReorderPass.h"
#include "topo/Analysis/StageAnalysis.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class TopoReorderPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// A leaf function returning void.
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

// Read the i32 value of a call's `topo.stage` metadata. Returns -1 if absent.
static int stageMetadataOf(llvm::CallBase* call) {
    auto* md = call->getMetadata("topo.stage");
    if (!md || md->getNumOperands() == 0) return -1;
    auto* cam = llvm::dyn_cast<llvm::ConstantAsMetadata>(md->getOperand(0));
    if (!cam) return -1;
    auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cam->getValue());
    return ci ? static_cast<int>(ci->getSExtValue()) : -1;
}

// Build a logic-block function `run` that calls `step_a` (stage 0) and
// `step_b` (stage 1). Returns the two call instructions in call order.
struct ReorderFixture {
    std::unique_ptr<llvm::Module> module;
    llvm::Function* run = nullptr;
    llvm::CallInst* callA = nullptr;
    llvm::CallInst* callB = nullptr;
    SymbolTable symbols;
    SymbolMapping mapping;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("reorder_demo", ctx);
        auto* stepA = createLeaf(*module, "step_a");
        auto* stepB = createLeaf(*module, "step_b");

        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* runTy = llvm::FunctionType::get(voidTy, {}, false);
        run = llvm::Function::Create(runTy, llvm::GlobalValue::ExternalLinkage, "run", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", run);
            llvm::IRBuilder<> b(bb);
            callA = b.CreateCall(stepA, {});
            callB = b.CreateCall(stepB, {});
            b.CreateRetVoid();
        }

        mapping.matched["ns::run"] = run;
        mapping.matched["ns::step_a"] = stepA;
        mapping.matched["ns::step_b"] = stepB;

        // Logic block: `run` orders step_a at stage 0, step_b at stage 1.
        LogicBlockEntry lb;
        lb.qualifiedName = "ns::run";
        lb.simpleName = "run";
        lb.calledFunctions = {"step_a", "step_b"};
        lb.stages = {0, 1};
        symbols.addLogicBlock(lb);
    }
};

// Each staged call gets a `topo.stage` metadata node carrying its stage
// number. A no-op pass attaches nothing → stageMetadataOf returns -1.
TEST_F(TopoReorderPassTest, StagedCallsAnnotatedWithStageMetadata) {
    llvm::LLVMContext ctx;
    ReorderFixture fx;
    fx.build(ctx);

    int annotated = TopoReorderPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(annotated, 2) << "both staged calls should be annotated";
    EXPECT_EQ(stageMetadataOf(fx.callA), 0) << "step_a call must carry stage 0";
    EXPECT_EQ(stageMetadataOf(fx.callB), 1) << "step_b call must carry stage 1";
}

// A function with no logic block gets no annotations.
TEST_F(TopoReorderPassTest, NonLogicBlockFunctionNotAnnotated) {
    llvm::LLVMContext ctx;
    ReorderFixture fx;
    fx.build(ctx);

    // Drop the logic block — `run` is now an ordinary function.
    SymbolTable emptySymbols;
    int annotated = TopoReorderPass::run(*fx.module, emptySymbols, fx.mapping);
    EXPECT_EQ(annotated, 0);
    EXPECT_EQ(stageMetadataOf(fx.callA), -1);
    EXPECT_EQ(stageMetadataOf(fx.callB), -1);
}

// The pre-computed StageAnalysisResult overload produces the same annotations.
TEST_F(TopoReorderPassTest, PrecomputedStageAnalysisOverload) {
    llvm::LLVMContext ctx;
    ReorderFixture fx;
    fx.build(ctx);

    auto stageAnalysis = analysis::analyzeStages(fx.symbols);
    int annotated = TopoReorderPass::run(*fx.module, stageAnalysis, fx.mapping);
    EXPECT_EQ(annotated, 2);
    EXPECT_EQ(stageMetadataOf(fx.callA), 0);
    EXPECT_EQ(stageMetadataOf(fx.callB), 1);
}

} // namespace
