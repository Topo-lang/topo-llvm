#include "topo/Transforms/DataLayoutPass.h"
#include "../../lib/Backend/LayoutCostModel.h"
#include "../../lib/Backend/LayoutBenchmark.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <gtest/gtest.h>

using namespace topo;

namespace {

class LayoutCostModelTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

/// Build a minimal module with two functions (AoS and SoA variants)
/// that have different instruction profiles.
/// The AoS variant accesses a struct array with interleaved fields.
/// The SoA variant accesses separate flat arrays (column-wise).
struct TestCostModelPipeline {
    std::unique_ptr<llvm::Module> module;
    llvm::Function* aosFn = nullptr;
    llvm::Function* soaFn = nullptr;

    static constexpr uint64_t N = 128;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("test_costmodel", ctx);
        module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));

        auto* floatTy = llvm::Type::getFloatTy(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // Struct type: { float, float, float, i32 }
        auto* particleSty = llvm::StructType::create(ctx, {floatTy, floatTy, floatTy, i32Ty}, "struct.Particle");
        auto* arrTy = llvm::ArrayType::get(particleSty, N);

        // AoS function: accesses fields through struct array GEPs
        auto* funcTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        aosFn = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline_aos", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", aosFn);
            llvm::IRBuilder<> b(bb);
            auto* arg = aosFn->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);
            // Simulate accessing 3 float fields across struct elements
            for (unsigned elem = 0; elem < 4; ++elem) {
                for (unsigned f = 0; f < 3; ++f) {
                    auto* gep = b.CreateGEP(
                        arrTy, arg, {zero, llvm::ConstantInt::get(i32Ty, elem), llvm::ConstantInt::get(i32Ty, f)});
                    auto* val = b.CreateLoad(floatTy, gep);
                    auto* added = b.CreateFAdd(val, llvm::ConstantFP::get(floatTy, 1.0));
                    b.CreateStore(added, gep);
                }
            }
            b.CreateRetVoid();
        }

        // SoA function: accesses through separate column arrays
        // Typically fewer instructions per access (no struct offset calc)
        auto* columnArrTy = llvm::ArrayType::get(floatTy, N);
        soaFn = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "pipeline_soa", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", soaFn);
            llvm::IRBuilder<> b(bb);
            auto* arg = soaFn->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);
            // Access a single flat column array -- simpler GEP pattern
            for (unsigned elem = 0; elem < 4; ++elem) {
                auto* gep = b.CreateGEP(columnArrTy, arg, {zero, llvm::ConstantInt::get(i32Ty, elem)});
                auto* val = b.CreateLoad(floatTy, gep);
                auto* added = b.CreateFAdd(val, llvm::ConstantFP::get(floatTy, 1.0));
                b.CreateStore(added, gep);
            }
            b.CreateRetVoid();
        }

        EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
    }

    /// Build with an explicit target triple (for cross-compilation tests).
    void buildWithTriple(llvm::LLVMContext& ctx, llvm::StringRef triple) {
        build(ctx);
        module->setTargetTriple(llvm::Triple(triple));
    }
};

// ============================================================================
// Basic estimate tests (preserved from original)
// ============================================================================

TEST_F(LayoutCostModelTest, EstimateReturnsResult) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    auto result = LayoutCostModel::estimate(*tp.module, variant);

    ASSERT_TRUE(result.has_value()) << "LayoutCostModel::estimate() should return a result";

    EXPECT_GT(result->aosMedianNs, 0.0) << "AoS cost should be positive";
    EXPECT_GT(result->soaMedianNs, 0.0) << "SoA cost should be positive";
    EXPECT_GT(result->speedup, 0.0) << "Speedup ratio should be positive";
    EXPECT_TRUE(result->winner == LayoutBenchmarkResult::AoS || result->winner == LayoutBenchmarkResult::SoA);
}

TEST_F(LayoutCostModelTest, StreamingBiasesTowardSoA) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    // Without streaming hint
    auto resultNone = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::None);
    // With streaming hint (should bias SoA cost lower -> higher speedup)
    auto resultStreaming = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::Streaming);

    ASSERT_TRUE(resultNone.has_value());
    ASSERT_TRUE(resultStreaming.has_value());

    // Streaming should make SoA more favorable (higher speedup ratio)
    EXPECT_GE(resultStreaming->speedup, resultNone->speedup) << "Streaming access should make SoA more favorable";
}

TEST_F(LayoutCostModelTest, RandomBiasesTowardAoS) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    // Without random hint
    auto resultNone = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::None);
    // With random hint (should bias SoA cost higher -> lower speedup)
    auto resultRandom = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::Random);

    ASSERT_TRUE(resultNone.has_value());
    ASSERT_TRUE(resultRandom.has_value());

    // Random should make SoA less favorable (lower speedup ratio)
    EXPECT_LE(resultRandom->speedup, resultNone->speedup) << "Random access should make SoA less favorable";
}

TEST_F(LayoutCostModelTest, GatherScatterBiasesTowardAoS) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    auto resultNone = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::None);
    auto resultGS = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::GatherScatter);

    ASSERT_TRUE(resultNone.has_value());
    ASSERT_TRUE(resultGS.has_value());

    // GatherScatter should penalize SoA most heavily
    EXPECT_LT(resultGS->speedup, resultNone->speedup) << "GatherScatter should strongly penalize SoA";
}

TEST_F(LayoutCostModelTest, SmallCardinalityPenalizesSoA) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    auto resultNone = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::None);

    CardinalityHint smallHint{10, 100}; // < 1k elements
    auto resultSmall = LayoutCostModel::estimate(*tp.module, variant, smallHint, AccessPattern::None);

    ASSERT_TRUE(resultNone.has_value());
    ASSERT_TRUE(resultSmall.has_value());

    // Small cardinality should penalize SoA (lower speedup)
    EXPECT_LE(resultSmall->speedup, resultNone->speedup) << "Small cardinality should penalize SoA";
}

TEST_F(LayoutCostModelTest, LargeCardinalityFavorsSoA) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    auto resultNone = LayoutCostModel::estimate(*tp.module, variant, std::nullopt, AccessPattern::None);

    CardinalityHint largeHint{50000, 200000}; // > 10k elements
    auto resultLarge = LayoutCostModel::estimate(*tp.module, variant, largeHint, AccessPattern::None);

    ASSERT_TRUE(resultNone.has_value());
    ASSERT_TRUE(resultLarge.has_value());

    // Large cardinality should favor SoA (higher speedup)
    EXPECT_GE(resultLarge->speedup, resultNone->speedup) << "Large cardinality should favor SoA";
}

TEST_F(LayoutCostModelTest, NullFunctionsReturnNullopt) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_null", ctx);
    module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));

    LayoutVariantPair variant;
    variant.aosFn = nullptr;
    variant.soaFn = nullptr;
    variant.pipelineName = "test::null";

    auto result = LayoutCostModel::estimate(*module, variant);
    EXPECT_FALSE(result.has_value()) << "Null functions should return nullopt";
}

// ============================================================================
// TTI cost breakdown tests
// ============================================================================

TEST_F(LayoutCostModelTest, BreakdownPopulatesCosts) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    LayoutCostBreakdown breakdown;
    auto result = LayoutCostModel::estimateWithBreakdown(*tp.module, variant, breakdown);

    ASSERT_TRUE(result.has_value());

    // AoS variant has loads and stores -> memory cost > 0
    EXPECT_GT(breakdown.aosMemoryCost, 0u) << "AoS should have nonzero memory cost";
    // AoS variant has fadd -> arithmetic cost > 0
    EXPECT_GT(breakdown.aosArithmeticCost, 0u) << "AoS should have nonzero arithmetic cost";

    // SoA variant also has loads/stores and fadds
    EXPECT_GT(breakdown.soaMemoryCost, 0u) << "SoA should have nonzero memory cost";
    EXPECT_GT(breakdown.soaArithmeticCost, 0u) << "SoA should have nonzero arithmetic cost";

    // Total cost should match sum of components
    EXPECT_EQ(breakdown.aosTotalCost(), breakdown.aosMemoryCost + breakdown.aosArithmeticCost + breakdown.aosOtherCost);
    EXPECT_EQ(breakdown.soaTotalCost(), breakdown.soaMemoryCost + breakdown.soaArithmeticCost + breakdown.soaOtherCost);
}

TEST_F(LayoutCostModelTest, BreakdownCacheUtilization) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    LayoutCostBreakdown breakdown;
    LayoutCostModel::estimateWithBreakdown(*tp.module, variant, breakdown);

    // Cache utilization should be in valid range [0, 1]
    EXPECT_GE(breakdown.aosCacheUtilization, 0.0);
    EXPECT_LE(breakdown.aosCacheUtilization, 1.0);
    EXPECT_GE(breakdown.soaCacheUtilization, 0.0);
    EXPECT_LE(breakdown.soaCacheUtilization, 1.0);

    // SoA should have higher cache utilization than AoS for column access
    EXPECT_GE(breakdown.soaCacheUtilization, breakdown.aosCacheUtilization)
        << "SoA should have >= cache utilization than AoS for column access";
}

TEST_F(LayoutCostModelTest, BreakdownVectorWidth) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    LayoutCostBreakdown breakdown;
    LayoutCostModel::estimateWithBreakdown(*tp.module, variant, breakdown);

    // Vector width should be at least 1 (scalar) on any target
    EXPECT_GE(breakdown.targetVectorWidth, 1u) << "Vector width should be at least 1";
}

TEST_F(LayoutCostModelTest, BreakdownNativeNotCrossCompilation) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    LayoutCostBreakdown breakdown;
    LayoutCostModel::estimateWithBreakdown(*tp.module, variant, breakdown);

    // Module uses host triple, so this should not be cross-compilation
    EXPECT_FALSE(breakdown.isCrossCompilation) << "Host triple should not be detected as cross-compilation";
}

// ============================================================================
// Cross-compilation detection
// ============================================================================

TEST_F(LayoutCostModelTest, CrossCompilationDetected) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_cross", ctx);

    // Set a triple that differs from the host
    auto hostTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    if (hostTriple.getArch() == llvm::Triple::aarch64)
        module->setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
    else
        module->setTargetTriple(llvm::Triple("aarch64-unknown-linux-gnu"));

    EXPECT_TRUE(LayoutCostModel::isCrossCompilation(*module))
        << "Different arch should be detected as cross-compilation";
}

TEST_F(LayoutCostModelTest, NativeNotCrossCompilation) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_native", ctx);
    module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));

    EXPECT_FALSE(LayoutCostModel::isCrossCompilation(*module)) << "Host triple should not be cross-compilation";
}

TEST_F(LayoutCostModelTest, UnknownTripleNotCrossCompilation) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_unknown", ctx);
    // Empty/unknown triple: assume native

    EXPECT_FALSE(LayoutCostModel::isCrossCompilation(*module))
        << "Unknown triple should default to native (not cross-compilation)";
}

// ============================================================================
// Optimization remark formatting
// ============================================================================

TEST_F(LayoutCostModelTest, FormatRemarkContainsCosts) {
    LayoutCostBreakdown breakdown;
    breakdown.aosMemoryCost = 100;
    breakdown.aosArithmeticCost = 50;
    breakdown.aosOtherCost = 10;
    breakdown.soaMemoryCost = 60;
    breakdown.soaArithmeticCost = 50;
    breakdown.soaOtherCost = 10;
    breakdown.aosCacheUtilization = 0.25;
    breakdown.soaCacheUtilization = 0.90;
    breakdown.targetVectorWidth = 8;
    breakdown.isCrossCompilation = true;

    auto remark = breakdown.formatRemark();

    // Should contain key information
    EXPECT_NE(remark.find("cross-compilation"), std::string::npos) << "Remark should mention cross-compilation";
    EXPECT_NE(remark.find("memory=100"), std::string::npos) << "Remark should include AoS memory cost";
    EXPECT_NE(remark.find("memory=60"), std::string::npos) << "Remark should include SoA memory cost";
    EXPECT_NE(remark.find("arith=50"), std::string::npos) << "Remark should include arithmetic cost";
    EXPECT_NE(remark.find("vector_width=8"), std::string::npos) << "Remark should include vector width";
    EXPECT_NE(remark.find("cache_util=25%"), std::string::npos) << "Remark should include AoS cache utilization";
    EXPECT_NE(remark.find("cache_util=90%"), std::string::npos) << "Remark should include SoA cache utilization";
}

TEST_F(LayoutCostModelTest, FormatRemarkNativeOmitsCrossCompilation) {
    LayoutCostBreakdown breakdown;
    breakdown.isCrossCompilation = false;

    auto remark = breakdown.formatRemark();

    EXPECT_EQ(remark.find("cross-compilation"), std::string::npos)
        << "Native remark should not mention cross-compilation";
}

// ============================================================================
// Fallback chain: LayoutBenchmark integration
// ============================================================================

TEST_F(LayoutCostModelTest, FallbackChainReturnsResultForNative) {
    llvm::LLVMContext ctx;
    TestCostModelPipeline tp;
    tp.build(ctx);

    LayoutVariantPair variant;
    variant.aosFn = tp.aosFn;
    variant.soaFn = tp.soaFn;
    variant.pipelineName = "test::pipeline";

    // LayoutBenchmark::run should always return a result (never nullopt)
    // due to the three-level fallback chain.
    auto result = LayoutBenchmark::run(*tp.module, variant, 1, 1);

    ASSERT_TRUE(result.has_value()) << "LayoutBenchmark::run should always return a result via fallback chain";
}

} // namespace
