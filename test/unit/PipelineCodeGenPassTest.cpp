#include "topo/Transforms/PipelineCodeGenPass.h"
#include "topo/Backend/SymbolMapper.h"
#include "topo/Sema/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>

#include <gtest/gtest.h>

#include <string>

using namespace topo;

namespace {

class PipelineCodeGenPassTest : public ::testing::Test {
protected:
    void SetUp() override { llvm::InitializeNativeTarget(); }
};

// Itanium-mangled name for `topo::detail::pipeline_placeholder()`.
// The pass demangles callee names and looks for this substring to detect stubs.
static constexpr const char* kPipelinePlaceholderMangled = "_ZN4topo6detail20pipeline_placeholderEv";

static llvm::Function* createPlaceholderDecl(llvm::Module& m, llvm::LLVMContext& ctx) {
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* fty = llvm::FunctionType::get(voidTy, false);
    auto* f = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, kPipelinePlaceholderMangled, m);
    return f;
}

static unsigned countCallsTo(llvm::Function& in, llvm::Function* callee) {
    unsigned n = 0;
    for (auto& bb : in)
        for (auto& inst : bb)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                if (call->getCalledFunction() == callee) ++n;
    return n;
}

static unsigned countAllCalls(llvm::Function& in) {
    unsigned n = 0;
    for (auto& bb : in)
        for (auto& inst : bb)
            if (llvm::isa<llvm::CallInst>(&inst)) ++n;
    return n;
}

// Build a linear pipeline: stageA (source) -> stageB (terminal).
// stageA takes an i32, returns i32; stageB takes an i32, returns i32.
struct LinearPipelineFixture {
    std::unique_ptr<llvm::Module> module;
    SymbolTable symbols;
    SymbolMapping mapping;
    llvm::Function* pipelineFunc = nullptr;
    llvm::Function* stageAFunc = nullptr;
    llvm::Function* stageBFunc = nullptr;
    llvm::Function* placeholderDecl = nullptr;

    void build(llvm::LLVMContext& ctx) {
        module = std::make_unique<llvm::Module>("linear_pipeline", ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

        stageAFunc = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "stageA", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageAFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateAdd(stageAFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
        }

        stageBFunc = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "stageB", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageBFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(b.CreateMul(stageBFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 2)));
        }

        placeholderDecl = createPlaceholderDecl(*module, ctx);

        pipelineFunc = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "pipeline", *module);
        {
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
            llvm::IRBuilder<> b(bb);
            b.CreateCall(placeholderDecl, {});
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        }

        LogicBlockEntry lb;
        lb.qualifiedName = "ns::pipeline";
        lb.simpleName = "pipeline";
        lb.isPipeline = true;
        lb.calledFunctions = {"ns::stageA", "ns::stageB"};
        lb.edges = {{"input", "stageA"}, {"stageA", "stageB"}};

        PipelineAnalysis an;
        an.stages = {{"stageA", 1}, {"stageB", 2}};
        an.sourceNodes = {"stageA"};
        an.terminalNode = "stageB";
        an.terminalType = "int";
        lb.pipelineAnalysis = an;

        symbols.addLogicBlock(lb);

        mapping.matched["ns::pipeline"] = pipelineFunc;
        mapping.matched["ns::stageA"] = stageAFunc;
        mapping.matched["ns::stageB"] = stageBFunc;
    }
};

TEST_F(PipelineCodeGenPassTest, NoPipelineStubsNoOp) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("no_stub", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

    auto* plain = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "plain", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", plain);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateAdd(plain->getArg(0), llvm::ConstantInt::get(i32Ty, 42)));
    }

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::plain";
    lb.simpleName = "plain";
    lb.isPipeline = true;
    PipelineAnalysis an;
    an.stages = {{"stageA", 1}};
    an.sourceNodes = {"stageA"};
    an.terminalNode = "stageA";
    lb.pipelineAnalysis = an;
    lb.calledFunctions = {};
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["ns::plain"] = plain;

    unsigned instsBefore = 0;
    for (auto& bb : *plain) instsBefore += bb.size();

    int result = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_EQ(result, 0);

    unsigned instsAfter = 0;
    for (auto& bb : *plain) instsAfter += bb.size();
    EXPECT_EQ(instsAfter, instsBefore);
}

TEST_F(PipelineCodeGenPassTest, LinearPipelineStubReplaced) {
    llvm::LLVMContext ctx;
    LinearPipelineFixture fx;
    fx.build(ctx);

    unsigned placeholderCallsBefore = countCallsTo(*fx.pipelineFunc, fx.placeholderDecl);
    EXPECT_EQ(placeholderCallsBefore, 1u);

    int result = PipelineCodeGenPass::run(*fx.module, fx.symbols, fx.mapping);
    EXPECT_EQ(result, 1);

    EXPECT_EQ(countCallsTo(*fx.pipelineFunc, fx.placeholderDecl), 0u);
    EXPECT_EQ(countCallsTo(*fx.pipelineFunc, fx.stageAFunc), 1u);
    EXPECT_EQ(countCallsTo(*fx.pipelineFunc, fx.stageBFunc), 1u);
    EXPECT_FALSE(llvm::verifyFunction(*fx.pipelineFunc, &llvm::errs()));
}

TEST_F(PipelineCodeGenPassTest, DiamondPipelineStubReplaced) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("diamond", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* unaryTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* binaryTy = llvm::FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);

    auto* nodeA = llvm::Function::Create(unaryTy, llvm::GlobalValue::ExternalLinkage, "nodeA", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeA);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateAdd(nodeA->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
    }

    auto* nodeB = llvm::Function::Create(unaryTy, llvm::GlobalValue::ExternalLinkage, "nodeB", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeB);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateMul(nodeB->getArg(0), llvm::ConstantInt::get(i32Ty, 2)));
    }

    auto* nodeC = llvm::Function::Create(unaryTy, llvm::GlobalValue::ExternalLinkage, "nodeC", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeC);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateMul(nodeC->getArg(0), llvm::ConstantInt::get(i32Ty, 3)));
    }

    auto* nodeD = llvm::Function::Create(binaryTy, llvm::GlobalValue::ExternalLinkage, "nodeD", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", nodeD);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateAdd(nodeD->getArg(0), nodeD->getArg(1)));
    }

    auto* placeholder = createPlaceholderDecl(*module, ctx);

    auto* pipelineFunc = llvm::Function::Create(unaryTy, llvm::GlobalValue::ExternalLinkage, "diamond_pipeline",
                                                *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(placeholder, {});
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::diamond_pipeline";
    lb.simpleName = "diamond_pipeline";
    lb.isPipeline = true;
    lb.calledFunctions = {"ns::nodeA", "ns::nodeB", "ns::nodeC", "ns::nodeD"};
    lb.edges = {{"input", "nodeA"}, {"nodeA", "nodeB"}, {"nodeA", "nodeC"}, {"nodeB", "nodeD"}, {"nodeC", "nodeD"}};

    PipelineAnalysis an;
    an.stages = {{"nodeA", 1}, {"nodeB", 2}, {"nodeC", 2}, {"nodeD", 3}};
    an.sourceNodes = {"nodeA"};
    an.terminalNode = "nodeD";
    an.terminalType = "int";
    lb.pipelineAnalysis = an;
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["ns::diamond_pipeline"] = pipelineFunc;
    mapping.matched["ns::nodeA"] = nodeA;
    mapping.matched["ns::nodeB"] = nodeB;
    mapping.matched["ns::nodeC"] = nodeC;
    mapping.matched["ns::nodeD"] = nodeD;

    int result = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_EQ(result, 1);

    EXPECT_EQ(countCallsTo(*pipelineFunc, nodeA), 1u);
    EXPECT_EQ(countCallsTo(*pipelineFunc, nodeB), 1u);
    EXPECT_EQ(countCallsTo(*pipelineFunc, nodeC), 1u);
    EXPECT_EQ(countCallsTo(*pipelineFunc, nodeD), 1u);

    // nodeD must appear after nodeB and nodeC in the rewritten entry block.
    auto& entry = pipelineFunc->getEntryBlock();
    int idxB = -1, idxC = -1, idxD = -1;
    int i = 0;
    for (auto& inst : entry) {
        auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call) {
            auto* callee = call->getCalledFunction();
            if (callee == nodeB) idxB = i;
            if (callee == nodeC) idxC = i;
            if (callee == nodeD) idxD = i;
        }
        ++i;
    }
    ASSERT_GE(idxB, 0);
    ASSERT_GE(idxC, 0);
    ASSERT_GE(idxD, 0);
    EXPECT_GT(idxD, idxB);
    EXPECT_GT(idxD, idxC);
}

TEST_F(PipelineCodeGenPassTest, SretReturnConvention) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("sret", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);

    auto* resultSty = llvm::StructType::create(ctx, {i32Ty, i32Ty}, "struct.Result");

    // Two-stage pipeline. stageA is the source returning i32; stageS is a
    // non-source stage taking an i32 and returning via sret. The pass
    // generates an alloca for stageS's sret parameter in the rewritten body.
    auto* stageATy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* stageAFunc = llvm::Function::Create(stageATy, llvm::GlobalValue::ExternalLinkage, "stageA", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageAFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateAdd(stageAFunc->getArg(0), llvm::ConstantInt::get(i32Ty, 1)));
    }

    auto* stageSTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptrTy, i32Ty}, false);
    auto* stageSFunc = llvm::Function::Create(stageSTy, llvm::GlobalValue::ExternalLinkage, "stageS", *module);
    stageSFunc->addParamAttr(0, llvm::Attribute::getWithStructRetType(ctx, resultSty));
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", stageSFunc);
        llvm::IRBuilder<> b(bb);
        auto* retPtr = stageSFunc->getArg(0);
        auto* zero = llvm::ConstantInt::get(i32Ty, 0);
        auto* one = llvm::ConstantInt::get(i32Ty, 1);
        auto* gep0 = b.CreateGEP(resultSty, retPtr, {zero, zero});
        b.CreateStore(stageSFunc->getArg(1), gep0);
        auto* gep1 = b.CreateGEP(resultSty, retPtr, {zero, one});
        b.CreateStore(llvm::ConstantInt::get(i32Ty, 7), gep1);
        b.CreateRetVoid();
    }

    auto* placeholder = createPlaceholderDecl(*module, ctx);
    // pipeline: i32 sret_pipeline(i32 %x)
    auto* pipelineTy = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* pipelineFunc = llvm::Function::Create(pipelineTy, llvm::GlobalValue::ExternalLinkage, "sret_pipeline",
                                                *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(placeholder, {});
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::sret_pipeline";
    lb.simpleName = "sret_pipeline";
    lb.isPipeline = true;
    lb.calledFunctions = {"ns::stageA", "ns::stageS"};
    lb.edges = {{"input", "stageA"}, {"stageA", "stageS"}};
    PipelineAnalysis an;
    an.stages = {{"stageA", 1}, {"stageS", 2}};
    an.sourceNodes = {"stageA"};
    an.terminalNode = "stageS";
    an.terminalType = "Result";
    lb.pipelineAnalysis = an;
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["ns::sret_pipeline"] = pipelineFunc;
    mapping.matched["ns::stageA"] = stageAFunc;
    mapping.matched["ns::stageS"] = stageSFunc;

    int result = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_EQ(result, 1);

    // After pass: body should contain both stage calls and an alloca
    // for stageS's sret parameter (generated by the non-source sret branch).
    EXPECT_EQ(countCallsTo(*pipelineFunc, stageAFunc), 1u);
    EXPECT_EQ(countCallsTo(*pipelineFunc, stageSFunc), 1u);
    unsigned sretAllocaCount = 0;
    for (auto& bb : *pipelineFunc) {
        for (auto& inst : bb) {
            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
                if (alloca->getAllocatedType() == resultSty) ++sretAllocaCount;
            }
        }
    }
    EXPECT_GE(sretAllocaCount, 1u);

    // Verify the stageS call uses the alloca as its first (sret) argument.
    bool foundSretUse = false;
    for (auto& bb : *pipelineFunc) {
        for (auto& inst : bb) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
            if (!call || call->getCalledFunction() != stageSFunc) continue;
            if (call->arg_size() >= 1 && llvm::isa<llvm::AllocaInst>(call->getArgOperand(0))) {
                foundSretUse = true;
            }
        }
    }
    EXPECT_TRUE(foundSretUse);
}

TEST_F(PipelineCodeGenPassTest, EmptyPipelineHandled) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("empty", ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* fty = llvm::FunctionType::get(voidTy, false);

    auto* placeholder = createPlaceholderDecl(*module, ctx);
    auto* pipelineFunc = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "empty_pipeline", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", pipelineFunc);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(placeholder, {});
        b.CreateRetVoid();
    }

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::empty_pipeline";
    lb.simpleName = "empty_pipeline";
    lb.isPipeline = true;
    PipelineAnalysis an;
    an.stages = {};
    an.sourceNodes = {};
    an.terminalNode = "";
    lb.pipelineAnalysis = an;
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["ns::empty_pipeline"] = pipelineFunc;

    // With an empty stage set, generatePipelineBody still clears the body
    // and emits a void return, reporting it as generated.
    int result = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_EQ(result, 1);

    // Body should contain exactly a terminator (ret void).
    auto& entry = pipelineFunc->getEntryBlock();
    EXPECT_EQ(entry.size(), 1u);
    EXPECT_NE(entry.getTerminator(), nullptr);
}

TEST_F(PipelineCodeGenPassTest, NonStubFunctionUntouched) {
    llvm::LLVMContext ctx;
    LinearPipelineFixture fx;
    fx.build(ctx);

    // Add a regular function alongside the stub.
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);
    auto* plain = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "plain", *fx.module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", plain);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateAdd(plain->getArg(0), llvm::ConstantInt::get(i32Ty, 99)));
    }

    unsigned plainInstsBefore = 0;
    for (auto& bb : *plain) plainInstsBefore += bb.size();

    PipelineCodeGenPass::run(*fx.module, fx.symbols, fx.mapping);

    unsigned plainInstsAfter = 0;
    for (auto& bb : *plain) plainInstsAfter += bb.size();
    EXPECT_EQ(plainInstsAfter, plainInstsBefore);
    EXPECT_EQ(countAllCalls(*plain), 0u);
}

TEST_F(PipelineCodeGenPassTest, NonPipelineLogicBlockIgnored) {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("non_pipeline", ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* fty = llvm::FunctionType::get(i32Ty, {i32Ty}, false);

    auto* placeholder = createPlaceholderDecl(*module, ctx);
    auto* logicFunc = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "logic_block", *module);
    {
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", logicFunc);
        llvm::IRBuilder<> b(bb);
        // Stub-looking body but the logic block isn't flagged as a pipeline.
        b.CreateCall(placeholder, {});
        b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
    }

    SymbolTable symbols;
    LogicBlockEntry lb;
    lb.qualifiedName = "ns::logic_block";
    lb.simpleName = "logic_block";
    lb.isPipeline = false;
    symbols.addLogicBlock(lb);

    SymbolMapping mapping;
    mapping.matched["ns::logic_block"] = logicFunc;

    unsigned beforeCalls = countAllCalls(*logicFunc);
    int result = PipelineCodeGenPass::run(*module, symbols, mapping);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(countAllCalls(*logicFunc), beforeCalls);
}

} // namespace
