#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
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

// --- Regression: arg-store use-before-def when a stage's 2nd call consumes
// a value defined *between* the 1st and 2nd calls ----------------------------
//
// The spawn scaffold (including every call's argument-struct stores) is
// emitted at the position of the first stage call. If the second stage call
// consumes a value computed after the first call, that value does not dominate
// the spawn point — storing it there would be a use-before-def (invalid IR).
// The pass must DECLINE to parallelize such a stage rather than emit broken IR.
struct MidComputePipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* enhanceFunc = nullptr;
    llvm::Function* detectFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("test_midcompute", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        enhanceFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "enhance", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", enhanceFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(enhanceFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
        }

        detectFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "detect", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", detectFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(detectFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 2)));
        }

        // pipeline(i32): r1 = enhance(arg);  mid = r1 * 7;  r2 = detect(mid)
        // detect's operand `mid` is computed AFTER the first call (enhance), so
        // it does not dominate the spawn anchor at the first call.
        pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = pipelineFunc->getArg(0);
            auto* r1 = b.CreateCall(enhanceFunc, {arg});
            auto* mid = b.CreateMul(r1, llvm::ConstantInt::get(i32Ty, 7)); // defined between the two calls
            auto* r2 = b.CreateCall(detectFunc, {mid});
            b.CreateRet(b.CreateAdd(r1, r2));
        }

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

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::enhance"] = enhanceFunc;
        mapping.matched["ns::detect"] = detectFunc;
    }
};

TEST_F(TopoParallelPassTest, MidStageOperandNotDominatingSpawnBailsOut) {
    llvm::LLVMContext ctx;
    MidComputePipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;

    // The second call's operand is defined after the first call, so the stage
    // cannot be parallelized safely → the pass declines (returns 0).
    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 0);

    // Both original calls must remain intact and the module must verify clean —
    // no use-before-def from a spawn-anchored arg store.
    EXPECT_FALSE(llvm::verifyModule(*tp.module, &llvm::errs()))
        << "TopoParallelPass produced invalid IR for a mid-stage-defined operand";

    int callCount = 0;
    for (auto& BB : *tp.pipelineFunc)
        for (auto& I : BB)
            if (llvm::isa<llvm::CallInst>(&I)) ++callCount;
    EXPECT_EQ(callCount, 2) << "declined stage must leave the original calls untouched";
}

// --- Regression: a variadic callee in a parallelizable stage must not crash.
// call->arg_size() (actual args) can exceed callee->arg_size() (declared
// params) for a variadic callee, and callee->getArg(ai) asserts on the
// varargs-tail index. The pass must exclude variadic callees as candidates.
struct VarargPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* varFunc = nullptr;
    llvm::Function* detectFunc = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("test_vararg", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);

        // enhance(i32, ...) -> i32 : variadic, 1 declared param.
        auto* varTy = llvm::FunctionType::get(i32Ty, {i32Ty}, /*isVarArg=*/true);
        varFunc = llvm::Function::Create(varTy, llvm::GlobalValue::ExternalLinkage, "enhance", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", varFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(varFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
        }

        // detect(i32) -> i32 : ordinary fixed-arity sibling in the same stage.
        auto* fixedTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
        detectFunc = llvm::Function::Create(fixedTy, llvm::GlobalValue::ExternalLinkage, "detect", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", detectFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(detectFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 2)));
        }

        pipelineFunc = llvm::Function::Create(fixedTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = pipelineFunc->getArg(0);
            // Call enhance with MORE actuals than declared params (variadic tail).
            auto* r1 = b.CreateCall(varFunc, {arg, llvm::ConstantInt::get(i32Ty, 99)});
            auto* r2 = b.CreateCall(detectFunc, {arg});
            b.CreateRet(b.CreateAdd(r1, r2));
        }

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

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::enhance"] = varFunc;
        mapping.matched["ns::detect"] = detectFunc;
    }
};

TEST_F(TopoParallelPassTest, VariadicCalleeExcludedNoCrash) {
    llvm::LLVMContext ctx;
    VarargPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;

    // Must not crash on the variadic callee. The variadic `enhance` is excluded
    // as a candidate; only the fixed-arity `detect` remains, which the pass
    // parallelizes (one stage transformed).
    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 1);

    // Resulting module must be well-formed.
    EXPECT_FALSE(llvm::verifyModule(*tp.module, &llvm::errs()))
        << "TopoParallelPass produced invalid IR while excluding a variadic callee";

    // The variadic call to `enhance` must remain serial (not spawned), and no
    // wrapper should have been created for it.
    EXPECT_EQ(tp.module->getFunction("enhance.topo_parallel_wrap"), nullptr);
    EXPECT_NE(tp.module->getFunction("detect.topo_parallel_wrap"), nullptr);
}

} // namespace
