#include "topo/Transforms/TopoParallelPass.h"
#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Backend/IREmbed.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class AdaptiveParallelTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

struct AdaptiveTestPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("adaptive_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* funcTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        // Create three functions: heavy, light, pipeline
        auto* heavyFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "heavy", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", heavyFunc);
            llvm::IRBuilder<> b(bb);
            llvm::Value* v = heavyFunc->getArg(0);
            for (int i = 0; i < 30; ++i)
                v = b.CreateMul(v, llvm::ConstantInt::get(i32Ty, 2));
            b.CreateRet(v);
        }

        auto* lightFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "light", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", lightFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(lightFunc->getArg(0));
        }

        auto* pipelineFunc = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* r1 = b.CreateCall(heavyFunc, {pipelineFunc->getArg(0)});
            auto* r2 = b.CreateCall(lightFunc, {pipelineFunc->getArg(0)});
            b.CreateRet(b.CreateAdd(r1, r2));
        }

        // Symbol table
        FunctionSymbol hs;
        hs.qualifiedName = "ns::heavy";
        hs.simpleName = "heavy";
        hs.visibility = Visibility::Protected;
        symbols.addFunction(hs);

        FunctionSymbol ls;
        ls.qualifiedName = "ns::light";
        ls.simpleName = "light";
        ls.visibility = Visibility::Protected;
        symbols.addFunction(ls);

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::heavy", "ns::light"};
        lb.edges = {{"input", "heavy", "", false, {}}, {"input", "light", "", false, {}}};

        PipelineAnalysis analysis;
        analysis.stages = {{"heavy", 1}, {"light", 1}};
        analysis.sourceNodes = {"input"};
        analysis.terminalNode = "output";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::heavy"] = heavyFunc;
        mapping.matched["ns::light"] = lightFunc;
    }
};

TEST_F(AdaptiveParallelTest, ParallelizesAllNonExcludedCandidates) {
    // The pass now always parallelizes qualifying stages (no cost heuristics).
    // Benefit decisions are made by VariantBenchmark in PassPipeline.
    llvm::LLVMContext ctx;
    AdaptiveTestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;

    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Both nodes in same stage, non-excluded → parallelized
    EXPECT_EQ(result, 1);
}

TEST_F(AdaptiveParallelTest, ExcludeDropsToOneCandidateStillParallelizes) {
    // The grain-size threshold was removed; with one remaining
    // candidate after exclude, the Pass still parallelizes (no value
    // judgment about whether 1 task is "worth" spawning).
    llvm::LLVMContext ctx;
    AdaptiveTestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Force;
    config.exclude = {"light"};

    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    EXPECT_EQ(result, 1);
}

TEST_F(AdaptiveParallelTest, AutoModeAlsoParallelizes) {
    // Auto mode in the pass itself behaves identically to force.
    // The decision to call the pass at all is made by PassPipeline.
    llvm::LLVMContext ctx;
    AdaptiveTestPipeline tp;
    tp.build(ctx);

    ParallelConfig config;
    config.mode = topo::FeatureMode::Auto;

    int result = TopoParallelPass::run(*tp.module, tp.symbols, tp.mapping, config);

    EXPECT_EQ(result, 1);
}

// ============================================================
// Constraint flow: verify that pruned edges in metadata
// cause PipelineCodeGenPass to exclude the pruned stage.
// This simulates the data path from adaptive monitor → JIT engine:
//   buildConstraintsFromCosts() → Context.prune_edge("*", stage)
//   → engine deserializes → applyConstraints() removes edges
//   → BFS prunes unreachable nodes → PipelineCodeGenPass omits them
// ============================================================

struct ConstraintFlowPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;

    // Pipeline: input → stage_a → merge
    //           input → stage_b → merge
    // stage_a and stage_b are both source nodes at stage 0.
    // merge is the terminal at stage 1.
    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("constraint_flow_test", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);

        // Pipeline function (placeholder stub)
        auto* pipelineTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        // Placeholder declaration
        auto* placeholderTy = llvm::FunctionType::get(i32Ty, {}, false);
        // Use Itanium mangling for the placeholder
        auto* placeholder = llvm::Function::Create(
            placeholderTy, llvm::GlobalValue::ExternalLinkage,
            "_ZN4topo6detail20pipeline_placeholderIiEEiv", *module);

        auto* pipelineFunc = llvm::Function::Create(
            pipelineTy, llvm::GlobalValue::ExternalLinkage,
            "_ZN2ns6renderEi", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateCall(placeholder, {}));
        }

        // stage_a: heavy computation
        auto* stageATy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
        auto* stageAFunc = llvm::Function::Create(
            stageATy, llvm::GlobalValue::ExternalLinkage,
            "_ZN2ns7stage_aEi", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageAFunc);
            llvm::IRBuilder<> b(bb);
            llvm::Value* v = stageAFunc->getArg(0);
            for (int i = 0; i < 20; ++i)
                v = b.CreateMul(v, b.getInt32(3));
            b.CreateRet(v);
        }

        // stage_b: trivial (cold stage — candidate for pruning)
        auto* stageBTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
        auto* stageBFunc = llvm::Function::Create(
            stageBTy, llvm::GlobalValue::ExternalLinkage,
            "_ZN2ns7stage_bEi", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageBFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(stageBFunc->getArg(0));
        }

        // merge(int, int)
        auto* mergeTy = llvm::FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);
        auto* mergeFunc = llvm::Function::Create(
            mergeTy, llvm::GlobalValue::ExternalLinkage,
            "_ZN2ns5mergeEii", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", mergeFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(mergeFunc->getArg(0), mergeFunc->getArg(1)));
        }

        // Symbol table
        LogicBlockEntry lb;
        lb.qualifiedName = "ns::render";
        lb.simpleName = "render";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stage_a", "ns::stage_b", "ns::merge"};
        lb.edges = {
            {"stage_a", "merge", "", false, {}},
            {"stage_b", "merge", "", false, {}},
        };

        PipelineAnalysis analysis;
        analysis.stages = {{"stage_a", 0}, {"stage_b", 0}, {"merge", 1}};
        analysis.sourceNodes = {"stage_a", "stage_b"};
        analysis.terminalNode = "merge";
        analysis.terminalType = "int";
        lb.pipelineAnalysis = analysis;
        symbols.addLogicBlock(lb);

        mapping.matched["ns::render"] = pipelineFunc;
        mapping.matched["ns::stage_a"] = stageAFunc;
        mapping.matched["ns::stage_b"] = stageBFunc;
        mapping.matched["ns::merge"] = mergeFunc;
    }
};

TEST_F(AdaptiveParallelTest, ConstraintFlowPrunesColdStage) {
    // Simulate the full constraint data flow:
    //   1. Adaptive monitor detects stage_b is cold
    //   2. Context.prune_edge("*", "stage_b") is set
    //   3. Engine's applyConstraints removes edges into stage_b
    //   4. BFS makes stage_b unreachable
    //   5. PipelineCodeGenPass generates code without stage_b

    llvm::LLVMContext ctx;
    ConstraintFlowPipeline tp;
    tp.build(ctx);

    // Serialize metadata (before codegen)
    ParallelConfig parCfg;
    parCfg.mode = topo::FeatureMode::Off;
    auto metaJson = IREmbed::serializeMetadata(tp.symbols, parCfg);
    ASSERT_FALSE(metaJson.empty());

    auto doc = nlohmann::json::parse(metaJson);
    ASSERT_TRUE(doc.contains("pipelines"));
    auto pipelineJson = doc["pipelines"][0];

    // --- Simulate applyConstraints with wildcard prune ---
    // The adaptive monitor would call ctx.prune_edge("*", "stage_b"),
    // which serializes to {"source":"*","target":"stage_b"}.
    // The engine's applyConstraints matches "*" as wildcard source.
    auto& edgesArray = pipelineJson["edges"];
    for (auto it = edgesArray.begin(); it != edgesArray.end();) {
        // Wildcard source match: prune all edges targeting "stage_b"
        if ((*it)["target"] == "stage_b") {
            it = edgesArray.erase(it);
        } else {
            ++it;
        }
    }

    // Also remove stage_b from sourceNodes (it's no longer part of the DAG)
    auto& srcNodes = pipelineJson["sourceNodes"];
    for (auto it = srcNodes.begin(); it != srcNodes.end();) {
        if (*it == "stage_b") {
            it = srcNodes.erase(it);
        } else {
            ++it;
        }
    }

    // --- Build synthetic SymbolTable from pruned metadata ---
    SymbolTable syntheticSymbols;
    {
        LogicBlockEntry lb;
        lb.qualifiedName = "ns::render";
        lb.simpleName = "render";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stage_a", "ns::stage_b", "ns::merge"};

        for (const auto& e : pipelineJson["edges"]) {
            PipelineEdge edge;
            edge.source = e.value("source", "");
            edge.target = e.value("target", "");
            lb.edges.push_back(edge);
        }

        PipelineAnalysis analysis;
        for (auto& [node, stage] : pipelineJson["stages"].items())
            analysis.stages[node] = stage.get<int>();
        for (const auto& s : pipelineJson["sourceNodes"])
            analysis.sourceNodes.push_back(s.get<std::string>());
        analysis.terminalNode = pipelineJson.value("terminalNode", "");
        analysis.terminalType = pipelineJson.value("terminalType", "");

        // BFS reachability (same as doSpecialize in engine)
        std::set<std::string> reachable(analysis.sourceNodes.begin(),
                                         analysis.sourceNodes.end());
        std::vector<std::string> worklist(analysis.sourceNodes.begin(),
                                           analysis.sourceNodes.end());
        while (!worklist.empty()) {
            auto cur = worklist.back();
            worklist.pop_back();
            for (const auto& edge : lb.edges) {
                if (edge.source == cur && reachable.find(edge.target) == reachable.end()) {
                    reachable.insert(edge.target);
                    worklist.push_back(edge.target);
                }
            }
        }
        for (auto it = analysis.stages.begin(); it != analysis.stages.end();) {
            if (reachable.find(it->first) == reachable.end())
                it = analysis.stages.erase(it);
            else
                ++it;
        }

        lb.pipelineAnalysis = analysis;
        syntheticSymbols.addLogicBlock(lb);
    }

    // Verify stage_b was pruned from the analysis
    auto& lbs = syntheticSymbols.logicBlocks();
    ASSERT_EQ(lbs.size(), 1u);
    auto& prunedEntry = lbs.at("ns::render");
    ASSERT_TRUE(prunedEntry.pipelineAnalysis.has_value());
    auto& prunedAnalysis = *prunedEntry.pipelineAnalysis;
    EXPECT_TRUE(prunedAnalysis.stages.count("stage_a"))
        << "stage_a should remain reachable";
    EXPECT_FALSE(prunedAnalysis.stages.count("stage_b"))
        << "stage_b should be pruned as unreachable";
    EXPECT_TRUE(prunedAnalysis.stages.count("merge"))
        << "merge (terminal) should remain reachable";

    // Run PipelineCodeGenPass with the pruned metadata
    int generated = PipelineCodeGenPass::run(*tp.module, syntheticSymbols, tp.mapping);
    EXPECT_EQ(generated, 1) << "Should generate 1 pipeline body";

    // Verify the generated code calls stage_a and merge, but NOT stage_b
    auto* renderFunc = tp.mapping.matched["ns::render"];
    ASSERT_NE(renderFunc, nullptr);
    EXPECT_FALSE(renderFunc->isDeclaration());

    bool callsStageA = false;
    bool callsStageB = false;
    bool callsMerge = false;
    for (auto& BB : *renderFunc) {
        for (auto& I : BB) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!call || !call->getCalledFunction()) continue;
            auto name = call->getCalledFunction()->getName().str();
            if (name.find("stage_a") != std::string::npos) callsStageA = true;
            if (name.find("stage_b") != std::string::npos) callsStageB = true;
            if (name.find("merge") != std::string::npos) callsMerge = true;
        }
    }

    EXPECT_TRUE(callsStageA) << "Reachable stage_a should be called";
    EXPECT_FALSE(callsStageB) << "Pruned stage_b should NOT be called";
    EXPECT_TRUE(callsMerge) << "Terminal merge should be called";
}

} // namespace
