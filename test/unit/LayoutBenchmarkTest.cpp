#include "topo/Transforms/DataLayoutPass.h"
#include "../../lib/Backend/LayoutBenchmark.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar/SROA.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class LayoutBenchmarkTest : public ::testing::Test {
protected:
    void SetUp() override {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    }
};

// Reuse the same test module structure as DataLayoutPassTest
struct TestAutoSelectPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* nodeAFunc = nullptr;
    llvm::Function* nodeBFunc = nullptr;

    static constexpr uint64_t N = 128;

    void build(llvm::LLVMContext& ctx, const std::string& wrapperName = "struct.topo::array") {
        module = std::make_unique<llvm::Module>("test_autoselect", ctx);
        module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));

        auto* floatTy = llvm::Type::getFloatTy(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);

        auto* particleSty = llvm::StructType::create(ctx, {floatTy, floatTy, floatTy, i32Ty}, "struct.Particle");
        auto* arrTy = llvm::ArrayType::get(particleSty, N);
        auto* wrapperSty = llvm::StructType::create(ctx, {arrTy}, wrapperName);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // nodeA: reads fields {0,1,2}
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* nodeFuncTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        nodeAFunc = llvm::Function::Create(nodeFuncTy, llvm::GlobalValue::ExternalLinkage, "nodeA", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeAFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = nodeAFunc->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);
            for (unsigned f = 0; f < 3; ++f) {
                auto* gep = b.CreateGEP(
                    wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, f)});
                b.CreateLoad(floatTy, gep);
            }
            b.CreateRetVoid();
        }

        // nodeB: reads fields {0,3}
        nodeBFunc = llvm::Function::Create(nodeFuncTy, llvm::GlobalValue::ExternalLinkage, "nodeB", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeBFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = nodeBFunc->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);
            auto* gep0 = b.CreateGEP(wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), zero});
            b.CreateLoad(floatTy, gep0);
            auto* gep3 = b.CreateGEP(
                wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, 3)});
            b.CreateLoad(i32Ty, gep3);
            b.CreateRetVoid();
        }

        // Pipeline: calls nodeA then nodeB
        auto* pipelineFuncTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
        pipelineFunc = llvm::Function::Create(pipelineFuncTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = pipelineFunc->getArg(0);
            b.CreateCall(nodeAFunc, {arg});
            b.CreateCall(nodeBFunc, {arg});
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        }

        // SymbolTable setup
        ClassSymbol particleClass;
        particleClass.qualifiedName = "physics::Particle";
        particleClass.simpleName = "Particle";
        particleClass.visibility = Visibility::Public;
        particleClass.memberVars = {
            {"x", {}, false, {}}, {"y", {}, false, {}}, {"z", {}, false, {}}, {"id", {}, false, {}}};
        symbols.addClassSymbol(particleClass);

        FunctionSymbol nodeASym;
        nodeASym.qualifiedName = "physics::apply_forces";
        nodeASym.simpleName = "apply_forces";
        nodeASym.visibility = Visibility::Protected;
        symbols.addFunction(nodeASym);

        FunctionSymbol nodeBSym;
        nodeBSym.qualifiedName = "physics::compute_id";
        nodeBSym.simpleName = "compute_id";
        nodeBSym.visibility = Visibility::Protected;
        symbols.addFunction(nodeBSym);

        LogicBlockEntry lb;
        lb.qualifiedName = "physics::simulate";
        lb.simpleName = "simulate";
        lb.isPipeline = true;
        lb.calledFunctions = {"physics::apply_forces", "physics::compute_id"};
        lb.edges = {{"input", "apply_forces"}, {"apply_forces", "compute_id"}};

        PipelineAnalysis analysis;
        analysis.stages = {{"apply_forces", 1}, {"compute_id", 2}};
        analysis.sourceNodes = {"input"};
        analysis.terminalNode = "compute_id";
        lb.pipelineAnalysis = analysis;

        symbols.addLogicBlock(lb);

        mapping.matched["physics::simulate"] = pipelineFunc;
        mapping.matched["physics::apply_forces"] = nodeAFunc;
        mapping.matched["physics::compute_id"] = nodeBFunc;

        // Pre-inline node functions (mirrors PassPipeline: TopoInline → AlwaysInliner → SROA)
        nodeAFunc->addFnAttr(llvm::Attribute::AlwaysInline);
        nodeBFunc->addFnAttr(llvm::Attribute::AlwaysInline);

        llvm::ModulePassManager mpm;
        mpm.addPass(llvm::AlwaysInlinerPass());
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));

        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        mpm.run(*module, mam);
    }
};

TEST_F(LayoutBenchmarkTest, GenerateVariantsProducesPair) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);

    ASSERT_EQ(variants.size(), 1u);
    EXPECT_EQ(variants[0].aosFn, tp.pipelineFunc);
    EXPECT_NE(variants[0].soaFn, nullptr);
    EXPECT_NE(variants[0].soaFn, tp.pipelineFunc);
    EXPECT_TRUE(variants[0].soaFn->getName().contains("__soa_variant"));
}

TEST_F(LayoutBenchmarkTest, SoAVariantUsesHeapAllocation) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);

    ASSERT_EQ(variants.size(), 1u);
    auto* soaFn = variants[0].soaFn;

    // SoA variant should contain calls to aligned_alloc (not just allocas)
    bool hasAlignedAlloc = false;
    bool hasFree = false;
    for (auto& BB : *soaFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getName() == "aligned_alloc") hasAlignedAlloc = true;
                    if (callee->getName() == "free") hasFree = true;
                }
            }
        }
    }

    EXPECT_TRUE(hasAlignedAlloc) << "SoA variant should use aligned_alloc";
    EXPECT_TRUE(hasFree) << "SoA variant should free heap columns";
}

TEST_F(LayoutBenchmarkTest, SoAVariantVerifiesClean) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);

    ASSERT_EQ(variants.size(), 1u);

    // Both variants should pass LLVM verification
    EXPECT_FALSE(llvm::verifyFunction(*variants[0].aosFn, &llvm::errs()));
    EXPECT_FALSE(llvm::verifyFunction(*variants[0].soaFn, &llvm::errs()));
}

TEST_F(LayoutBenchmarkTest, DisabledConfigNoVariants) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Off;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_TRUE(variants.empty());
}

// Removed: SmallArrayNoVariants. The `minArraySize` size-based skip was
// deleted from variant generation — every
// qualifying topo::array<T,N> is now a candidate, and benchmark
// decides whether SoA is beneficial.

TEST_F(LayoutBenchmarkTest, RandomAccessStillGeneratesVariants) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    // Random access patterns no longer skip variant generation.
    // The benchmark decides whether SoA is beneficial.
    auto* fnSym = tp.symbols.findFunction("physics::apply_forces");
    if (fnSym) {
        const_cast<FunctionSymbol*>(fnSym)->accessPattern = AccessPattern::Random;
    }

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_FALSE(variants.empty());
}

TEST_F(LayoutBenchmarkTest, BenchmarkRunsAndReturnsResult) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);
    ASSERT_EQ(variants.size(), 1u);

    // Run the actual JIT benchmark
    auto result = LayoutBenchmark::run(*tp.module,
                                       variants[0],
                                       /*warmup=*/10,
                                       /*iterations=*/100);

    // Should succeed on native target
    ASSERT_TRUE(result.has_value()) << "LayoutBenchmark::run() should succeed on native target";

    // Both timings should be positive
    EXPECT_GT(result->aosMedianNs, 0.0) << "AoS median should be positive";
    EXPECT_GT(result->soaMedianNs, 0.0) << "SoA median should be positive";

    // Speedup should be a finite positive number
    EXPECT_GT(result->speedup, 0.0) << "Speedup ratio should be positive";

    // Winner should be one of the two values
    EXPECT_TRUE(result->winner == LayoutBenchmarkResult::AoS || result->winner == LayoutBenchmarkResult::SoA)
        << "Winner should be AoS or SoA";
}

// Parameterised wrapper-name coverage: the auto-mode variant generator must
// recognise the std::array shapes (libstdc++ + libc++) the topo::array alias
// can resolve to, alongside the legacy struct.topo::array form. Without this,
// auto mode silently produces zero variants on a real host build.
class LayoutBenchmarkWrapperNameTest : public LayoutBenchmarkTest,
                                       public ::testing::WithParamInterface<std::string> {};

TEST_P(LayoutBenchmarkWrapperNameTest, GeneratesVariantsForWrapperName) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx, GetParam());

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);
    ASSERT_EQ(variants.size(), 1u) << "wrapper name " << GetParam() << " was not recognised";
    EXPECT_EQ(variants[0].aosFn, tp.pipelineFunc);
    EXPECT_NE(variants[0].soaFn, nullptr);
}

INSTANTIATE_TEST_SUITE_P(WrapperNameVariants, LayoutBenchmarkWrapperNameTest,
                         ::testing::Values(std::string("struct.topo::array"),
                                           std::string("struct.std::array"),
                                           std::string("struct.std::__1::array")));

TEST_F(LayoutBenchmarkTest, BenchmarkSelectsWinnerInPassPipeline) {
    llvm::LLVMContext ctx;
    TestAutoSelectPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Auto;

    // Count functions before
    unsigned funcCountBefore = 0;
    for (auto& F : *tp.module)
        if (!F.isDeclaration()) ++funcCountBefore;

    auto variants = DataLayoutPass::generateVariants(*tp.module, tp.symbols, tp.mapping, config);
    ASSERT_EQ(variants.size(), 1u);

    // After variant generation: one extra function (the SoA clone)
    unsigned funcCountAfterGen = 0;
    for (auto& F : *tp.module)
        if (!F.isDeclaration()) ++funcCountAfterGen;
    EXPECT_EQ(funcCountAfterGen, funcCountBefore + 1);

    // Run benchmark and prune loser (simulating PassPipeline logic)
    auto result = LayoutBenchmark::run(*tp.module,
                                       variants[0],
                                       /*warmup=*/10,
                                       /*iterations=*/100);
    ASSERT_TRUE(result.has_value());

    auto& vp = variants[0];
    if (result->winner == LayoutBenchmarkResult::SoA) {
        vp.aosFn->replaceAllUsesWith(vp.soaFn);
        vp.soaFn->takeName(vp.aosFn);
        vp.aosFn->eraseFromParent();
    } else {
        vp.soaFn->eraseFromParent();
    }

    // After pruning: should be back to original function count
    unsigned funcCountAfterPrune = 0;
    for (auto& F : *tp.module)
        if (!F.isDeclaration()) ++funcCountAfterPrune;
    EXPECT_EQ(funcCountAfterPrune, funcCountBefore);

    // Module should still verify
    EXPECT_FALSE(llvm::verifyModule(*tp.module, &llvm::errs()));
}

} // namespace
