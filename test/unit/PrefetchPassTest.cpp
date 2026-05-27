#include <gtest/gtest.h>

#include "topo/Transforms/PrefetchPass.h"
#include "topo/Backend/PassReports.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

using namespace topo;

class PrefetchPassTest : public ::testing::Test {
protected:
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;

    void SetUp() override { module = std::make_unique<llvm::Module>("test", ctx); }

    /// Create a function with a simple loop containing a load:
    ///   for (i = 0; i < N; ++i) { sum += arr[i]; }
    llvm::Function* createFuncWithLoop(const std::string& name) {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* funcTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, i32Ty}, false);
        auto* func = llvm::Function::Create(funcTy, llvm::Function::ExternalLinkage, name, module.get());

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        auto* header = llvm::BasicBlock::Create(ctx, "loop.header", func);
        auto* body = llvm::BasicBlock::Create(ctx, "loop.body", func);
        auto* exit = llvm::BasicBlock::Create(ctx, "loop.exit", func);

        llvm::IRBuilder<> builder(entry);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* phi = builder.CreatePHI(i32Ty, 2, "i");
        phi->addIncoming(llvm::ConstantInt::get(i32Ty, 0), entry);
        auto* cmp = builder.CreateICmpSLT(phi, func->getArg(1), "cmp");
        builder.CreateCondBr(cmp, body, exit);

        builder.SetInsertPoint(body);
        auto* gep = builder.CreateGEP(i32Ty, func->getArg(0), {phi}, "ptr");
        auto* load = builder.CreateLoad(i32Ty, gep, "val");
        // Use the load to prevent it from being trivially dead
        auto* inc = builder.CreateAdd(phi, llvm::ConstantInt::get(i32Ty, 1), "inc");
        phi->addIncoming(inc, body);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
        builder.CreateRetVoid();

        return func;
    }

    /// Register a function with the given access pattern in both the
    /// SymbolTable and SymbolMapping, wired through a logic block.
    void addFunctionWithPattern(const std::string& ns, const std::string& funcName, AccessPattern pattern) {
        std::string qualified = ns + "::" + funcName;
        auto* func = createFuncWithLoop(funcName);
        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = funcName;
        sym.visibility = Visibility::Public;
        sym.accessPattern = pattern;
        symbols.addFunction(sym);

        LogicBlockEntry entry;
        entry.qualifiedName = ns + "::run";
        entry.simpleName = "run";
        entry.calledFunctions.push_back(funcName);
        entry.stages.push_back(1);
        symbols.addLogicBlock(entry);
    }

    /// Count the number of llvm.prefetch calls in the module.
    int countPrefetchCalls() const {
        int count = 0;
        for (const auto& F : *module) {
            for (const auto& BB : F) {
                for (const auto& I : BB) {
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        if (auto* callee = call->getCalledFunction()) {
                            if (callee->getIntrinsicID() == llvm::Intrinsic::prefetch) {
                                ++count;
                            }
                        }
                    }
                }
            }
        }
        return count;
    }
};

TEST_F(PrefetchPassTest, StreamingLoopInsertsPrefetch) {
    addFunctionWithPattern("data", "process", AccessPattern::Streaming);

    PrefetchConfig config;
    config.mode = FeatureMode::Force;

    int result = PrefetchPass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
    EXPECT_GT(countPrefetchCalls(), 0);
}

TEST_F(PrefetchPassTest, RandomPatternStillPrefetches) {
    // The AccessPattern::Random skip was removed — Topo
    // passes do not gate on the declared access pattern (the declaration
    // is structural information, not a "is this worth prefetching"
    // judgment). LLVM/HW determine the actual benefit; if Random patterns
    // genuinely defeat prefetch, the COVERED non-regression contract
    // surfaces it via benchmark data rather than via Pass-internal skip.
    addFunctionWithPattern("data", "scatter", AccessPattern::Random);

    PrefetchConfig config;
    config.mode = FeatureMode::Force;

    int result = PrefetchPass::run(*module, symbols, mapping, config);
    EXPECT_GT(result, 0);
    EXPECT_GT(countPrefetchCalls(), 0);
}

TEST_F(PrefetchPassTest, OffModeNoOp) {
    addFunctionWithPattern("data", "process", AccessPattern::Streaming);

    PrefetchConfig config;
    config.mode = FeatureMode::Off;

    int result = PrefetchPass::run(*module, symbols, mapping, config);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(countPrefetchCalls(), 0);
}

TEST_F(PrefetchPassTest, StageBoundaryPrefetch) {
    // Build a pipeline with two stages: stage1 (Streaming) -> stage2 (Streaming).
    // After PipelineCodeGenPass would generate sequential calls, PrefetchPass
    // should insert prefetch intrinsics for stage2's pointer args between the calls.

    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    // Create stage1 function: ptr -> void (processes input array)
    auto* stage1Ty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, i32Ty}, false);
    auto* stage1Func = llvm::Function::Create(stage1Ty, llvm::Function::ExternalLinkage, "stage1", module.get());
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stage1Func);
        llvm::IRBuilder<> b(bb);
        b.CreateRetVoid();
    }

    // Create stage2 function: ptr -> void (processes output array)
    auto* stage2Ty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, i32Ty}, false);
    auto* stage2Func = llvm::Function::Create(stage2Ty, llvm::Function::ExternalLinkage, "stage2", module.get());
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stage2Func);
        llvm::IRBuilder<> b(bb);
        b.CreateRetVoid();
    }

    // Create the pipeline function that calls stage1 then stage2
    auto* pipelineTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, ptrTy, i32Ty}, false);
    auto* pipelineFunc =
        llvm::Function::Create(pipelineTy, llvm::Function::ExternalLinkage, "pipeline_run", module.get());
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
        llvm::IRBuilder<> b(bb);
        // Call stage1(arg0, arg2)
        b.CreateCall(stage1Func, {pipelineFunc->getArg(0), pipelineFunc->getArg(2)});
        // Call stage2(arg1, arg2)
        b.CreateCall(stage2Func, {pipelineFunc->getArg(1), pipelineFunc->getArg(2)});
        b.CreateRetVoid();
    }

    // Register functions in SymbolTable
    FunctionSymbol sym1;
    sym1.qualifiedName = "pipe::stage1";
    sym1.simpleName = "stage1";
    sym1.visibility = Visibility::Public;
    sym1.accessPattern = AccessPattern::Streaming;
    symbols.addFunction(sym1);

    FunctionSymbol sym2;
    sym2.qualifiedName = "pipe::stage2";
    sym2.simpleName = "stage2";
    sym2.visibility = Visibility::Public;
    sym2.accessPattern = AccessPattern::Streaming;
    symbols.addFunction(sym2);

    // Register the pipeline logic block with pipeline analysis
    LogicBlockEntry block;
    block.qualifiedName = "pipe::pipeline_run";
    block.simpleName = "pipeline_run";
    block.isPipeline = true;
    block.calledFunctions = {"stage1", "stage2"};
    block.stages = {1, 2};
    block.edges.push_back({"stage1", "stage2", "", false, {}});

    PipelineAnalysis pa;
    pa.stages = {{"stage1", 1}, {"stage2", 2}};
    pa.sourceNodes = {"stage1"};
    pa.terminalNode = "stage2";
    block.pipelineAnalysis = pa;

    symbols.addLogicBlock(block);

    // Register in mapping
    mapping.matched["pipe::stage1"] = stage1Func;
    mapping.matched["pipe::stage2"] = stage2Func;
    mapping.matched["pipe::pipeline_run"] = pipelineFunc;

    PrefetchConfig config;
    config.mode = FeatureMode::Force;

    int result = PrefetchPass::run(*module, symbols, mapping, config);
    // Should have inserted at least one prefetch for stage2's pointer arg
    EXPECT_GT(result, 0);
    EXPECT_GT(countPrefetchCalls(), 0);

    // Verify the prefetch is placed before the stage2 call
    // by checking the pipeline function's instruction sequence
    bool foundPrefetchBeforeStage2 = false;
    bool seenStage1Call = false;
    for (auto& BB : *pipelineFunc) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto* callee = call->getCalledFunction();
                if (!callee) continue;
                if (callee == stage1Func) {
                    seenStage1Call = true;
                } else if (callee->getIntrinsicID() == llvm::Intrinsic::prefetch) {
                    if (seenStage1Call) {
                        foundPrefetchBeforeStage2 = true;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(foundPrefetchBeforeStage2) << "Prefetch should appear after stage1 call, before stage2 call";
}

TEST_F(PrefetchPassTest, AutoModeStreamingOnly) {
    // First function: Streaming - should get prefetch
    addFunctionWithPattern("data", "stream_fn", AccessPattern::Streaming);

    // Second function: None - should be skipped in auto mode
    {
        std::string qualified = "data2::plain_fn";
        auto* func = createFuncWithLoop("plain_fn");
        mapping.matched[qualified] = func;

        FunctionSymbol sym;
        sym.qualifiedName = qualified;
        sym.simpleName = "plain_fn";
        sym.visibility = Visibility::Public;
        sym.accessPattern = AccessPattern::None;
        symbols.addFunction(sym);

        LogicBlockEntry entry;
        entry.qualifiedName = "data2::run";
        entry.simpleName = "run";
        entry.calledFunctions.push_back("plain_fn");
        entry.stages.push_back(1);
        symbols.addLogicBlock(entry);
    }

    PrefetchConfig config;
    config.mode = FeatureMode::Auto;

    int result = PrefetchPass::run(*module, symbols, mapping, config);
    // Only the streaming function should have prefetch inserted
    EXPECT_EQ(result, 1);
    EXPECT_EQ(countPrefetchCalls(), 1);
}

TEST_F(PrefetchPassTest, ReportEntryPopulated) {
    addFunctionWithPattern("data", "process", AccessPattern::Streaming);

    PrefetchConfig config;
    config.mode = FeatureMode::Force;

    backend::PrefetchReport report;
    int result = PrefetchPass::run(*module, symbols, mapping, config, &report);
    EXPECT_GT(result, 0);
    ASSERT_FALSE(report.entries.empty());
    bool found = false;
    for (const auto& e : report.entries) {
        if (e.hostFunction == "data::process") {
            EXPECT_GT(e.insertedHints, 0);
            EXPECT_EQ(e.distance, config.distance);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}
