#include "topo/Transforms/AdaptiveDispatchPass.h"
#include "topo/Backend/PassPipeline.h"
#include "topo/Backend/PassReports.h"
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

class AdaptiveJITTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Build a pipeline with two stage functions for full pass testing
struct FullPipelineFixture {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    std::vector<VisibilityEntry> visEntries;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("adaptive_jit_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        // enhance(i32) -> i32
        auto* enhanceFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "enhance", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", enhanceFunc);
            llvm::IRBuilder<> b(bb);
            auto* v = b.CreateAdd(enhanceFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 10));
            b.CreateRet(v);
        }

        // detect(i32) -> i32
        auto* detectFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "detect", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", detectFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(detectFunc->getArg(0));
        }

        // pipeline(i32) -> i32
        auto* pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* r1 = b.CreateCall(enhanceFunc, {pipelineFunc->getArg(0)});
            auto* r2 = b.CreateCall(detectFunc, {pipelineFunc->getArg(0)});
            b.CreateRet(b.CreateAdd(r1, r2));
        }

        // Symbol table
        FunctionSymbol es;
        es.qualifiedName = "ns::enhance";
        es.simpleName = "enhance";
        es.visibility = Visibility::Protected;
        symbols.addFunction(es);

        FunctionSymbol ds;
        ds.qualifiedName = "ns::detect";
        ds.simpleName = "detect";
        ds.visibility = Visibility::Protected;
        symbols.addFunction(ds);

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::enhance", "ns::detect"};
        lb.edges = {{"input", "enhance"}, {"input", "detect"}};

        PipelineAnalysis analysis;
        analysis.stages = {{"enhance", 1}, {"detect", 1}};
        analysis.sourceNodes = {"input"};
        analysis.terminalNode = "compose";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::enhance"] = enhanceFunc;
        mapping.matched["ns::detect"] = detectFunc;

        // Visibility entries
        visEntries.push_back({"ns::pipeline", Visibility::Public});
        visEntries.push_back({"ns::enhance", Visibility::Protected});
        visEntries.push_back({"ns::detect", Visibility::Protected});
    }
};

TEST_F(AdaptiveJITTest, PassPipelineWithAdaptive) {
    llvm::LLVMContext ctx;
    FullPipelineFixture fp;
    fp.build(ctx);

    // Run AdaptiveDispatchPass directly
    AdaptiveConfig adaptiveCfg;
    adaptiveCfg.mode = topo::FeatureMode::Auto;

    int result = AdaptiveDispatchPass::run(*fp.module, fp.symbols, fp.mapping, adaptiveCfg);
    EXPECT_EQ(result, 1);

    // Verify IR structure
    auto* func = fp.module->getFunction("pipeline");
    ASSERT_NE(func, nullptr);

    // Should have entry, jit_path, aot_entry blocks
    bool hasEntry = false, hasJitPath = false, hasAotEntry = false;
    for (auto& BB : *func) {
        if (BB.getName() == "entry") hasEntry = true;
        if (BB.getName() == "jit_path") hasJitPath = true;
        if (BB.getName() == "aot_entry") hasAotEntry = true;
    }
    EXPECT_TRUE(hasEntry);
    EXPECT_TRUE(hasJitPath);
    EXPECT_TRUE(hasAotEntry);

    // Verify topo_adaptive_register is declared
    auto* regFunc = fp.module->getFunction("topo_adaptive_register");
    EXPECT_NE(regFunc, nullptr);
}

TEST_F(AdaptiveJITTest, AdaptiveWithParallel) {
    llvm::LLVMContext ctx;
    FullPipelineFixture fp;
    fp.build(ctx);

    // Both parallel and adaptive enabled
    ParallelConfig parallelCfg;
    parallelCfg.mode = topo::FeatureMode::Auto;
    AdaptiveConfig adaptiveCfg;
    adaptiveCfg.mode = topo::FeatureMode::Auto;

    // Run parallel first (as in real pipeline order)
    TopoParallelPass::run(*fp.module, fp.symbols, fp.mapping, parallelCfg);

    // Then adaptive dispatch
    int result = AdaptiveDispatchPass::run(*fp.module, fp.symbols, fp.mapping, adaptiveCfg, &parallelCfg);

    // Should instrument the pipeline function
    EXPECT_EQ(result, 1);

    // Both cost_begin/end (from parallel) and adaptive dispatch should be present
    auto* func = fp.module->getFunction("pipeline");
    ASSERT_NE(func, nullptr);

    bool hasCostBegin = false;
    bool hasJitPath = false;
    for (auto& BB : *func) {
        if (BB.getName() == "jit_path") hasJitPath = true;
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = call->getCalledFunction();
                if (callee && callee->getName() == "topo_cost_begin") hasCostBegin = true;
            }
        }
    }
    EXPECT_TRUE(hasCostBegin);
    EXPECT_TRUE(hasJitPath);
}

TEST_F(AdaptiveJITTest, DisabledAdaptiveNoChanges) {
    llvm::LLVMContext ctx;
    FullPipelineFixture fp;
    fp.build(ctx);

    AdaptiveConfig adaptiveCfg;
    adaptiveCfg.mode = topo::FeatureMode::Off;

    int result = AdaptiveDispatchPass::run(*fp.module, fp.symbols, fp.mapping, adaptiveCfg);

    EXPECT_EQ(result, 0);

    // No jit_path or __jit_ptr should exist
    auto* func = fp.module->getFunction("pipeline");
    ASSERT_NE(func, nullptr);

    for (auto& BB : *func) {
        EXPECT_NE(BB.getName(), "jit_path");
    }
    EXPECT_EQ(fp.module->getGlobalVariable("pipeline.__jit_ptr"), nullptr);
}

TEST_F(AdaptiveJITTest, ReportRecordsInstrumentedPipelines) {
    llvm::LLVMContext ctx;
    FullPipelineFixture fp;
    fp.build(ctx);

    AdaptiveConfig adaptiveCfg;
    adaptiveCfg.mode = topo::FeatureMode::Auto;

    backend::AdaptiveDispatchReport report;
    int result = AdaptiveDispatchPass::run(*fp.module, fp.symbols, fp.mapping, adaptiveCfg,
                                           /*parallelCfg=*/nullptr,
                                           /*excludeFuncs=*/nullptr,
                                           &report);
    EXPECT_EQ(result, 1);
    ASSERT_EQ(report.entries.size(), 1u);
    EXPECT_EQ(report.entries[0].defaultVariant, "aot");
    EXPECT_FALSE(report.entries[0].stageName.empty());
}

} // namespace
