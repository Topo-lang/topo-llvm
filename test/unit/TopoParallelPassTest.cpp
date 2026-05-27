#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

#include <string>

using namespace topo;

namespace {

class TopoParallelPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Helper: create a module with two functions that can be parallelized
struct TestPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* enhanceFunc = nullptr;
    llvm::Function* detectFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);

        // Create enhance(i32) -> i32
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
        enhanceFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "enhance", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", enhanceFunc);
            llvm::IRBuilder<> b(bb);
            auto* v = b.CreateAdd(enhanceFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1));
            for (int i = 0; i < 20; ++i)
                v = b.CreateMul(v, llvm::ConstantInt::get(i32Ty, 2));
            b.CreateRet(v);
        }

        // Create detect(i32) -> i32
        detectFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "detect", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", detectFunc);
            llvm::IRBuilder<> b(bb);
            auto* v = b.CreateAdd(detectFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 2));
            for (int i = 0; i < 20; ++i)
                v = b.CreateMul(v, llvm::ConstantInt::get(i32Ty, 3));
            b.CreateRet(v);
        }

        // Create pipeline(i32) -> i32 with sequential calls
        pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = pipelineFunc->getArg(0);
            auto* r1 = b.CreateCall(enhanceFunc, {arg});
            auto* r2 = b.CreateCall(detectFunc, {arg});
            auto* sum = b.CreateAdd(r1, r2);
            b.CreateRet(sum);
        }

        // Set up symbol table with pipeline analysis
        FunctionSymbol enhanceSym;
        enhanceSym.qualifiedName = "ns::enhance";
        enhanceSym.simpleName = "enhance";
        enhanceSym.visibility = Visibility::Protected;
        symbols.addFunction(enhanceSym);

        FunctionSymbol detectSym;
        detectSym.qualifiedName = "ns::detect";
        detectSym.simpleName = "detect";
        detectSym.visibility = Visibility::Protected;
        symbols.addFunction(detectSym);

        // Logic block with pipeline analysis
        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::enhance", "ns::detect"};
        lb.edges = {{"load", "enhance"}, {"load", "detect"}};

        PipelineAnalysis analysis;
        analysis.stages = {{"enhance", 1}, {"detect", 1}};
        analysis.sourceNodes = {"load"};
        analysis.terminalNode = "compose";
        lb.pipelineAnalysis = analysis;

        symbols.addLogicBlock(lb);

        // Symbol mapping
        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::enhance"] = enhanceFunc;
        mapping.matched["ns::detect"] = detectFunc;
    }
};

TEST_F(TopoParallelPassTest, DisabledConfigNoChanges) {
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Off;

    EXPECT_EQ(TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config), 0);
}

TEST_F(TopoParallelPassTest, ForceParallelizesAllCandidates) {
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.instrument = true;

    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Both nodes in same stage, non-excluded → parallelized
    EXPECT_EQ(result, 1);
}

// Removed: MinTasksTooHighSkips. The Pass no longer makes grain-based
// decisions — `minTasksToParallelize` was a value judgment violating the
// "Topo doesn't judge" principle.

TEST_F(TopoParallelPassTest, ExcludeListSkipsFunctions) {
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.exclude = {"enhance", "detect"};

    EXPECT_EQ(TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config), 0);
}

TEST_F(TopoParallelPassTest, ExcludeOneStillParallelizesRemaining) {
    // With the grain-size threshold removed, excluding
    // one of two stage nodes leaves 1 candidate — the Pass still parallelizes
    // it (correctness only: empty-check guard, no value judgment). If the
    // user prefers serial execution at that grain, set [parallel] mode = "off".
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.exclude = {"enhance"};

    EXPECT_EQ(TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config), 1);
}

TEST_F(TopoParallelPassTest, AutoModeParallelizesLikeForce) {
    // In auto mode, the pass itself always parallelizes qualifying stages.
    // The auto decision is made by VariantBenchmark in PassPipeline, not here.
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    EXPECT_EQ(result, 1);
}

TEST_F(TopoParallelPassTest, InstrumentationAddsRuntimeCalls) {
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.instrument = true;

    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    EXPECT_EQ(result, 1);

    // Verify instrumentation functions were declared
    EXPECT_NE(tp.module->getFunction("topo_cost_begin"), nullptr);
    EXPECT_NE(tp.module->getFunction("topo_cost_end"), nullptr);
}

TEST_F(TopoParallelPassTest, SpawnRuntimeDeclared) {
    llvm::LLVMContext ctx;
    TestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;

    TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Verify parallel runtime functions were declared
    EXPECT_NE(tp.module->getFunction("topo_parallel_ensure_init"), nullptr);
    EXPECT_NE(tp.module->getFunction("topo_task_spawn_ret"), nullptr);
    EXPECT_NE(tp.module->getFunction("topo_task_await_all"), nullptr);
}

} // namespace
