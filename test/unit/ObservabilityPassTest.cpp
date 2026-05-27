#include "topo/Transforms/ObservabilityPass.h"
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

class ObservabilityPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Helper: build a minimal module with a pipeline function that has stages
struct ObserveTestPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("observe_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        // Stage 0 function
        auto* stage0Func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "stage0", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stage0Func);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(stage0Func->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
        }

        // Stage 1 function
        auto* stage1Func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "stage1", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stage1Func);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateMul(stage1Func->getArg(0), llvm::ConstantInt::get(i32Ty, 2)));
        }

        // Pipeline function calling stage0 then stage1
        pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* r0 = b.CreateCall(stage0Func, {pipelineFunc->getArg(0)});
            auto* r1 = b.CreateCall(stage1Func, {r0});
            b.CreateRet(r1);
        }

        // Symbol table with two stages
        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stage0", "ns::stage1"};
        lb.stages = {0, 1};

        PipelineAnalysis analysis;
        analysis.stages = {{"stage0", 0}, {"stage1", 1}};
        analysis.sourceNodes = {"input"};
        analysis.terminalNode = "stage1";
        analysis.terminalType = "int";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::stage0"] = stage0Func;
        mapping.matched["ns::stage1"] = stage1Func;
    }
};

// Helper: build a pipeline with internal linkage
struct ObserveTestInternalPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("observe_internal_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        auto* stageFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "internal_stage", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(stageFunc->getArg(0));
        }

        // Pipeline function with internal linkage
        pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::InternalLinkage, "internal_pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* r = b.CreateCall(stageFunc, {pipelineFunc->getArg(0)});
            b.CreateRet(r);
        }

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::internal_pipeline";
        lb.simpleName = "internal_pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::internal_stage"};
        lb.stages = {0};

        PipelineAnalysis analysis;
        analysis.stages = {{"internal_stage", 0}};
        analysis.sourceNodes = {"input"};
        analysis.terminalNode = "internal_stage";
        analysis.terminalType = "int";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::internal_pipeline"] = pipelineFunc;
        mapping.matched["ns::internal_stage"] = stageFunc;
    }
};

TEST_F(ObservabilityPassTest, DisabledConfigNoChanges) {
    llvm::LLVMContext ctx;
    ObserveTestPipeline tp;
    tp.build(ctx);

    ObservabilityConfig config;
    config.mode = topo::FeatureMode::Off;

    int result = ObservabilityPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 0);

    // Verify no trace functions were declared
    EXPECT_EQ(tp.module->getFunction("topo_trace_span_begin"), nullptr);
    EXPECT_EQ(tp.module->getFunction("topo_trace_span_end"), nullptr);
}

TEST_F(ObservabilityPassTest, EnabledInsertsSpans) {
    llvm::LLVMContext ctx;
    ObserveTestPipeline tp;
    tp.build(ctx);

    ObservabilityConfig config;
    config.mode = topo::FeatureMode::Force;

    int result = ObservabilityPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_GT(result, 0);

    // Verify trace functions were declared
    auto* spanBegin = tp.module->getFunction("topo_trace_span_begin");
    auto* spanEnd = tp.module->getFunction("topo_trace_span_end");
    EXPECT_NE(spanBegin, nullptr);
    EXPECT_NE(spanEnd, nullptr);

    // Verify calls exist in the pipeline function
    bool foundSpanBegin = false;
    bool foundSpanEnd = false;
    for (auto& BB : *tp.pipelineFunc) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = call->getCalledFunction();
                if (callee && callee->getName() == "topo_trace_span_begin") foundSpanBegin = true;
                if (callee && callee->getName() == "topo_trace_span_end") foundSpanEnd = true;
            }
        }
    }
    EXPECT_TRUE(foundSpanBegin);
    EXPECT_TRUE(foundSpanEnd);
}

TEST_F(ObservabilityPassTest, InternalStagesSkipped) {
    llvm::LLVMContext ctx;
    ObserveTestInternalPipeline tp;
    tp.build(ctx);

    ObservabilityConfig config;
    config.mode = topo::FeatureMode::Force;
    config.internalStages = false; // skip internal linkage functions

    int result = ObservabilityPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 0);

    // No trace functions should be declared for internal pipeline
    EXPECT_EQ(tp.module->getFunction("topo_trace_span_begin"), nullptr);
}

TEST_F(ObservabilityPassTest, InternalStagesInstrumented) {
    llvm::LLVMContext ctx;
    ObserveTestInternalPipeline tp;
    tp.build(ctx);

    ObservabilityConfig config;
    config.mode = topo::FeatureMode::Force;
    config.internalStages = true; // instrument internal stages too

    int result = ObservabilityPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_GT(result, 0);

    // Trace functions should be declared
    EXPECT_NE(tp.module->getFunction("topo_trace_span_begin"), nullptr);
}

TEST_F(ObservabilityPassTest, SpanNameContainsStageNumber) {
    llvm::LLVMContext ctx;
    ObserveTestPipeline tp;
    tp.build(ctx);

    ObservabilityConfig config;
    config.mode = topo::FeatureMode::Force;

    ObservabilityPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Check that global strings contain stage numbers
    bool foundStage0 = false;
    bool foundStage1 = false;
    for (auto& gv : tp.module->globals()) {
        if (gv.hasInitializer()) {
            if (auto* arr = llvm::dyn_cast<llvm::ConstantDataArray>(gv.getInitializer())) {
                if (arr->isString()) {
                    std::string str = arr->getAsString().str();
                    if (str.find("::stage0") != std::string::npos) foundStage0 = true;
                    if (str.find("::stage1") != std::string::npos) foundStage1 = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundStage0);
    EXPECT_TRUE(foundStage1);
}

} // namespace
