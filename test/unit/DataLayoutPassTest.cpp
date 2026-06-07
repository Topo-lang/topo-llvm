#include "topo/Transforms/DataLayoutPass.h"
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

class DataLayoutPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Helper: build a test module with:
//   %struct.Particle = type { float, float, float, i32 }  (x, y, z, id)
//   %"struct.<wrapperName>" = type { [128 x %struct.Particle] }
//   nodeA(ptr) reads fields {0,1,2} (x,y,z)
//   nodeB(ptr) reads fields {0,3} (x,id)
//   pipeline(ptr) -> i32 calls nodeA then nodeB
//
// The wrapper struct name is parameterised so tests can exercise every
// fixed-size array shape DataLayoutPass accepts:
//   * "struct.topo::array"         (legacy / explicit topo::)
//   * "struct.std::array"          (libstdc++ — topo::array aliases to it)
//   * "struct.std::__1::array"     (libc++ inline namespace)
struct TestDataLayoutPipeline {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* nodeAFunc = nullptr;
    llvm::Function* nodeBFunc = nullptr;
    llvm::StructType* particleSty = nullptr;
    llvm::StructType* wrapperSty = nullptr;

    static constexpr uint64_t N = 128;

    void build(llvm::LLVMContext& ctx, const std::string& wrapperName = "struct.topo::array") {
        module = std::make_unique<llvm::Module>("test_datalayout", ctx);
        auto* floatTy = llvm::Type::getFloatTy(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);

        // Create struct types
        particleSty = llvm::StructType::create(ctx, {floatTy, floatTy, floatTy, i32Ty}, "struct.Particle");

        auto* arrTy = llvm::ArrayType::get(particleSty, N);
        wrapperSty = llvm::StructType::create(ctx, {arrTy}, wrapperName);

        auto* ptrTy = llvm::PointerType::get(ctx, 0);

        // nodeA: reads fields {0,1,2} — x, y, z
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* nodeFuncTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        nodeAFunc = llvm::Function::Create(nodeFuncTy, llvm::GlobalValue::ExternalLinkage, "nodeA", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeAFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = nodeAFunc->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);

            // Access field 0 (x): GEP wrapper, 0, 0, 0, 0
            auto* gep0 = b.CreateGEP(wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), zero}, "x");
            b.CreateLoad(floatTy, gep0);

            // Access field 1 (y): GEP wrapper, 0, 0, 0, 1
            auto* gep1 = b.CreateGEP(
                wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, 1)}, "y");
            b.CreateLoad(floatTy, gep1);

            // Access field 2 (z): GEP wrapper, 0, 0, 0, 2
            auto* gep2 = b.CreateGEP(
                wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, 2)}, "z");
            b.CreateLoad(floatTy, gep2);

            b.CreateRetVoid();
        }

        // nodeB: reads fields {0,3} — x, id
        nodeBFunc = llvm::Function::Create(nodeFuncTy, llvm::GlobalValue::ExternalLinkage, "nodeB", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeBFunc);
            llvm::IRBuilder<> b(bb);
            auto* arg = nodeBFunc->getArg(0);
            auto* zero = llvm::ConstantInt::get(i32Ty, 0);

            // Access field 0 (x)
            auto* gep0 = b.CreateGEP(wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), zero}, "x");
            b.CreateLoad(floatTy, gep0);

            // Access field 3 (id)
            auto* gep3 = b.CreateGEP(wrapperSty,
                                     arg,
                                     {zero, zero, llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, 3)},
                                     "id");
            b.CreateLoad(i32Ty, gep3);

            b.CreateRetVoid();
        }

        // Pipeline function: calls nodeA then nodeB
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

        // Set up SymbolTable
        // ClassSymbol for Particle
        ClassSymbol particleClass;
        particleClass.qualifiedName = "physics::Particle";
        particleClass.simpleName = "Particle";
        particleClass.visibility = Visibility::Public;
        particleClass.memberVars = {
            {"x", {}, false, {}}, {"y", {}, false, {}}, {"z", {}, false, {}}, {"id", {}, false, {}}};
        symbols.addClassSymbol(particleClass);

        // Function symbols
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

        // Logic block (pipeline)
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

        // Symbol mapping
        mapping.matched["physics::simulate"] = pipelineFunc;
        mapping.matched["physics::apply_forces"] = nodeAFunc;
        mapping.matched["physics::compute_id"] = nodeBFunc;
    }
};

TEST_F(DataLayoutPassTest, DisabledConfigNoChanges) {
    llvm::LLVMContext ctx;
    TestDataLayoutPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Off;

    int result = DataLayoutPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 0);
}

// Removed: SmallArraySkipped. The `minArraySize` size-based skip was
// deleted from the Pass — Topo passes do not gate
// on workload-side cost heuristics. The Pass now transforms every
// qualifying topo::array<T,N>; LLVM cost model handles downstream
// cleanup of unprofitable rewrites.

TEST_F(DataLayoutPassTest, DetectsTopoArrayOfStruct) {
    llvm::LLVMContext ctx;
    auto* floatTy = llvm::Type::getFloatTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    auto* elemSty = llvm::StructType::create(ctx, {floatTy, i32Ty}, "struct.MyStruct");
    auto* arrTy = llvm::ArrayType::get(elemSty, 64);
    auto* wrapperSty = llvm::StructType::create(ctx, {arrTy}, "struct.topo::array");

    // Should detect as topo::array<MyStruct, 64>
    auto* wrapperRaw = static_cast<llvm::StructType*>(wrapperSty);
    // The detection is tested indirectly via the pass; verify the struct setup
    EXPECT_TRUE(wrapperRaw->hasName());
    EXPECT_TRUE(wrapperRaw->getName().starts_with("struct.topo::array"));
    EXPECT_EQ(wrapperRaw->getNumElements(), 1u);
    auto* inner = llvm::dyn_cast<llvm::ArrayType>(wrapperRaw->getElementType(0));
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->getNumElements(), 64u);
    EXPECT_EQ(inner->getElementType(), elemSty);
}

// Parameterised over the wrapper name. topo::array is a `using` alias for
// std::array, so any IR that the host toolchain emits will carry one of:
//   - struct.topo::array          (legacy / explicit topo::)
//   - struct.std::array           (libstdc++)
//   - struct.std::__1::array      (libc++ inline namespace)
// The Pass must detect all three.
class DataLayoutPassWrapperNameTest : public DataLayoutPassTest,
                                      public ::testing::WithParamInterface<std::string> {};

TEST_P(DataLayoutPassWrapperNameTest, TransformsForWrapperName) {
    llvm::LLVMContext ctx;
    TestDataLayoutPipeline tp;
    tp.build(ctx, GetParam());

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    int result = DataLayoutPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 1) << "wrapper name " << GetParam() << " was not recognised";
}

INSTANTIATE_TEST_SUITE_P(WrapperNameVariants, DataLayoutPassWrapperNameTest,
                         ::testing::Values(std::string("struct.topo::array"),
                                           std::string("struct.std::array"),
                                           std::string("struct.std::__1::array"),
                                           std::string("class.topo::array"),
                                           std::string("class.std::array"),
                                           std::string("class.std::__1::array")));

TEST_F(DataLayoutPassTest, FieldUsageAnalysis) {
    llvm::LLVMContext ctx;
    TestDataLayoutPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    // Before transform, verify the pipeline has direct calls
    unsigned callCount = 0;
    for (auto& BB : *tp.pipelineFunc)
        for (auto& I : BB)
            if (llvm::isa<llvm::CallInst>(&I)) ++callCount;
    EXPECT_EQ(callCount, 2u); // nodeA + nodeB

    int result = DataLayoutPass::run(*tp.module, tp.symbols, tp.mapping, config);
    EXPECT_EQ(result, 1); // One array transformed
}

TEST_F(DataLayoutPassTest, TransformInsertsSoAAllocas) {
    llvm::LLVMContext ctx;
    TestDataLayoutPipeline tp;
    tp.build(ctx);

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    DataLayoutPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Verify SoA column allocas were created (align 64)
    unsigned align64AllocaCount = 0;
    for (auto& BB : *tp.pipelineFunc) {
        for (auto& I : BB) {
            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                if (alloca->getAlign() == llvm::Align(64)) {
                    // Verify it's an array type column
                    if (llvm::isa<llvm::ArrayType>(alloca->getAllocatedType())) ++align64AllocaCount;
                }
            }
        }
    }

    // Should have columns for fields {0,1,2,3} (union of nodeA's {0,1,2} and nodeB's {0,3})
    EXPECT_EQ(align64AllocaCount, 4u);
}

TEST_F(DataLayoutPassTest, TransformPreservesSignature) {
    llvm::LLVMContext ctx;
    TestDataLayoutPipeline tp;
    tp.build(ctx);

    auto* origType = tp.pipelineFunc->getFunctionType();
    unsigned origArgCount = tp.pipelineFunc->arg_size();

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    DataLayoutPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // Function signature should be unchanged
    EXPECT_EQ(tp.pipelineFunc->getFunctionType(), origType);
    EXPECT_EQ(tp.pipelineFunc->arg_size(), origArgCount);
}

TEST_F(DataLayoutPassTest, NonPipelineFunctionUnchanged) {
    llvm::LLVMContext ctx;
    TestDataLayoutPipeline tp;
    tp.build(ctx);

    // Count instructions in nodeA before
    unsigned nodeAInstsBefore = 0;
    for (auto& BB : *tp.nodeAFunc)
        for (auto& I : BB)
            ++nodeAInstsBefore;

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    DataLayoutPass::run(*tp.module, tp.symbols, tp.mapping, config);

    // nodeA (non-pipeline) should be unchanged
    unsigned nodeAInstsAfter = 0;
    for (auto& BB : *tp.nodeAFunc)
        for (auto& I : BB)
            ++nodeAInstsAfter;

    EXPECT_EQ(nodeAInstsBefore, nodeAInstsAfter);
}

TEST_F(DataLayoutPassTest, ScalarArraySkipped) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_scalar", ctx);
    auto* floatTy = llvm::Type::getFloatTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);

    // topo::array<float, 128> — scalar element, should be skipped
    auto* arrTy = llvm::ArrayType::get(floatTy, 128);
    auto* wrapperSty = llvm::StructType::create(ctx, {arrTy}, "struct.topo::array.scalar");

    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* funcTy = llvm::FunctionType::get(i32Ty, {ptrTy}, false);

    auto* func = llvm::Function::Create(funcTy, llvm::GlobalValue::ExternalLinkage, "scalar_pipeline", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "test::scalar_pipeline";
    lb.simpleName = "scalar_pipeline";
    lb.isPipeline = true;
    PipelineAnalysis analysis;
    analysis.stages = {};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["test::scalar_pipeline"] = func;

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    int result = DataLayoutPass::run(*module, symbols, mapping, config);
    EXPECT_EQ(result, 0); // Scalar array → no transform
}

// Regression: a pipeline with TWO qualifying topo::array parameters must not
// double-terminate the entry block. The legacy transform's per-parameter loop
// used to re-fetch the (already-branched) entry block and emit a second
// unconditional branch into a new scatter loop, producing two terminators —
// malformed IR that trips verifyModule. The fix transforms only the first
// qualifying array parameter and declines the rest, so the result must verify.
TEST_F(DataLayoutPassTest, TwoArrayParamsProduceValidIR) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("test_two_arrays", ctx);
    auto* floatTy = llvm::Type::getFloatTy(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    constexpr uint64_t N = 128;
    auto* particleSty = llvm::StructType::create(ctx, {floatTy, floatTy, floatTy, i32Ty}, "struct.Particle");
    auto* arrTy = llvm::ArrayType::get(particleSty, N);
    auto* wrapperSty = llvm::StructType::create(ctx, {arrTy}, "struct.topo::array");

    auto* zero = llvm::ConstantInt::get(i32Ty, 0);

    // node(ptr): reads fields {0,1} from its array argument.
    auto* nodeFuncTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    auto* nodeFunc = llvm::Function::Create(nodeFuncTy, llvm::GlobalValue::ExternalLinkage, "node", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeFunc);
        llvm::IRBuilder<> b(bb);
        auto* arg = nodeFunc->getArg(0);
        auto* gep0 = b.CreateGEP(wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), zero}, "x");
        b.CreateLoad(floatTy, gep0);
        auto* gep1 = b.CreateGEP(
            wrapperSty, arg, {zero, zero, llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, 1)}, "y");
        b.CreateLoad(floatTy, gep1);
        b.CreateRetVoid();
    }

    // pipeline(ptr src, ptr dst): two topo::array<Particle,N> parameters.
    auto* pipelineFuncTy = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy}, false);
    auto* pipelineFunc =
        llvm::Function::Create(pipelineFuncTy, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(nodeFunc, {pipelineFunc->getArg(0)});
        b.CreateCall(nodeFunc, {pipelineFunc->getArg(1)});
        b.CreateRet(zero);
    }

    SymbolTable symbols;
    ClassSymbol particleClass;
    particleClass.qualifiedName = "physics::Particle";
    particleClass.simpleName = "Particle";
    particleClass.visibility = Visibility::Public;
    particleClass.memberVars = {{"x", {}, false, {}}, {"y", {}, false, {}}, {"z", {}, false, {}}, {"id", {}, false, {}}};
    symbols.addClassSymbol(particleClass);

    FunctionSymbol nodeSym;
    nodeSym.qualifiedName = "physics::node";
    nodeSym.simpleName = "node";
    nodeSym.visibility = Visibility::Protected;
    symbols.addFunction(nodeSym);

    LogicBlockEntry lb;
    lb.qualifiedName = "physics::simulate";
    lb.simpleName = "simulate";
    lb.isPipeline = true;
    lb.calledFunctions = {"physics::node"};
    PipelineAnalysis analysis;
    analysis.stages = {{"node", 1}};
    lb.pipelineAnalysis = analysis;
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["physics::simulate"] = pipelineFunc;
    mapping.matched["physics::node"] = nodeFunc;

    DataLayoutConfig config;
    config.mode = FeatureMode::Force;

    DataLayoutPass::run(*module, symbols, mapping, config);

    // The result must be valid IR — no double-terminated entry block.
    EXPECT_FALSE(llvm::verifyModule(*module, nullptr))
        << "two-array-param pipeline produced malformed IR";
}

} // namespace
