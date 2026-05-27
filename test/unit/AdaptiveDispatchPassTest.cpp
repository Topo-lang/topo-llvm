#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class AdaptiveDispatchPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Helper: build a minimal pipeline module
struct DispatchTestPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("dispatch_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        // stage function
        auto* stageFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "stage", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(stageFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
        }

        // pipeline function calling stage
        pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* result = b.CreateCall(stageFunc, {pipelineFunc->getArg(0)});
            b.CreateRet(result);
        }

        // Symbol table
        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stage"};

        PipelineAnalysis analysis;
        analysis.stages = {{"stage", 0}};
        analysis.sourceNodes = {"input"};
        analysis.terminalNode = "stage";
        analysis.terminalType = "int";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::stage"] = stageFunc;
    }
};

TEST_F(AdaptiveDispatchPassTest, DisabledConfigNoChanges) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Off;

    int result = AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 0);

    // Verify entry block is unchanged (still named "entry")
    EXPECT_EQ(tp.pipelineFunc->getEntryBlock().getName(), "entry");
}

TEST_F(AdaptiveDispatchPassTest, EnabledInsertsDispatch) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Auto;

    int result = AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 1);

    // Verify the function has the new structure:
    // entry -> jit_path | aot_entry
    auto* entryBB = &tp.pipelineFunc->getEntryBlock();
    EXPECT_EQ(entryBB->getName(), "entry");

    // Entry block should end with a conditional branch
    auto* term = entryBB->getTerminator();
    ASSERT_NE(term, nullptr);
    auto* br = llvm::dyn_cast<llvm::BranchInst>(term);
    ASSERT_NE(br, nullptr);
    EXPECT_TRUE(br->isConditional());

    // Check for jit_path and aot_entry blocks
    bool hasJitPath = false;
    bool hasAotEntry = false;
    for (auto& BB : *tp.pipelineFunc) {
        if (BB.getName() == "jit_path") hasJitPath = true;
        if (BB.getName() == "aot_entry") hasAotEntry = true;
    }
    EXPECT_TRUE(hasJitPath);
    EXPECT_TRUE(hasAotEntry);
}

TEST_F(AdaptiveDispatchPassTest, JitPtrGlobalCreated) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Auto;

    AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Check that __jit_ptr global exists (internal linkage → need AllowInternal)
    auto* jitPtrGV = tp.module->getGlobalVariable("pipeline.__jit_ptr", true);
    ASSERT_NE(jitPtrGV, nullptr);
    EXPECT_TRUE(jitPtrGV->getValueType()->isPointerTy());
}

TEST_F(AdaptiveDispatchPassTest, AtomicLoadInEntry) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Auto;

    AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Verify the entry block contains an atomic load
    auto* entryBB = &tp.pipelineFunc->getEntryBlock();
    bool foundAtomicLoad = false;
    for (auto& I : *entryBB) {
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            if (load->isAtomic()) {
                foundAtomicLoad = true;
                EXPECT_EQ(load->getOrdering(), llvm::AtomicOrdering::Acquire);
            }
        }
    }
    EXPECT_TRUE(foundAtomicLoad);
}

TEST_F(AdaptiveDispatchPassTest, GlobalCtorRegistered) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Auto;

    AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Check that llvm.global_ctors was created
    auto* ctors = tp.module->getNamedGlobal("llvm.global_ctors");
    ASSERT_NE(ctors, nullptr);

    // Check the constructor function exists
    auto* ctorFunc = tp.module->getFunction("pipeline.__adaptive_ctor");
    EXPECT_NE(ctorFunc, nullptr);
}

TEST_F(AdaptiveDispatchPassTest, CostInstrumentationInserted) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Auto;

    AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Check for topo_cost_begin/end declarations
    auto* costBegin = tp.module->getFunction("topo_cost_begin");
    auto* costEnd = tp.module->getFunction("topo_cost_end");
    EXPECT_NE(costBegin, nullptr);
    EXPECT_NE(costEnd, nullptr);

    // Find calls to cost_begin/end in the aot_entry block
    bool foundCostBegin = false;
    bool foundCostEnd = false;
    for (auto& BB : *tp.pipelineFunc) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = call->getCalledFunction();
                if (callee && callee->getName() == "topo_cost_begin") foundCostBegin = true;
                if (callee && callee->getName() == "topo_cost_end") foundCostEnd = true;
            }
        }
    }
    EXPECT_TRUE(foundCostBegin);
    EXPECT_TRUE(foundCostEnd);
}

// The dispatch point must inject a one-shot call to the
// reusable pass-event wire (topo_pass_event_emit), declared as a plain
// external (same contract as topo_adaptive_register; resolved by
// injectAutoLinkLibs whenever [adaptive] is enabled).
TEST_F(AdaptiveDispatchPassTest, PassEventEmitInjectedAtDispatch) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Auto;

    AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);

    auto* emit = tp.module->getFunction("topo_pass_event_emit");
    ASSERT_NE(emit, nullptr);
    // Declared, not defined; a plain external (not weak — Mach-O treats
    // an unresolved weak external as a hard link error anyway).
    EXPECT_TRUE(emit->isDeclaration());
    EXPECT_FALSE(emit->hasExternalWeakLinkage());

    // The call must be present somewhere in the instrumented pipeline.
    bool foundEmitCall = false;
    for (auto& BB : *tp.pipelineFunc) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = call->getCalledFunction();
                if (callee && callee->getName() == "topo_pass_event_emit") {
                    foundEmitCall = true;
                    // 4 args: pass, from, to, subject.
                    EXPECT_EQ(call->arg_size(), 4u);
                }
            }
        }
    }
    EXPECT_TRUE(foundEmitCall);

    // One-shot guard global per pipeline.
    bool foundFlag = false;
    for (auto& GV : tp.module->globals()) {
        if (GV.getName().contains(".__pe_emitted")) foundFlag = true;
    }
    EXPECT_TRUE(foundFlag);
}

// Disabled config must not introduce the pass-event symbol at all
// (preserves the "off ≡ no artifacts" contract for the new wire too).
TEST_F(AdaptiveDispatchPassTest, PassEventNotInjectedWhenDisabled) {
    llvm::LLVMContext ctx;
    DispatchTestPipeline tp;
    tp.build(ctx);

    AdaptiveConfig config;
    config.mode = topo::FeatureMode::Off;

    AdaptiveDispatchPass::run(*tp.module, tp.symbols, tp.mapping, config);

    EXPECT_EQ(tp.module->getFunction("topo_pass_event_emit"), nullptr);
}

} // namespace
